/* SPDX-License-Identifier: 0BSD */
/*
 * HaLow mesh layer for the XIAO ESP32S3 + Wio-WM6180 (Morse Micro MM6108). See mesh.h.
 *
 * How it works, in one paragraph. Morselib (Morse Micro's microcontroller SDK) has no mesh
 * mode, but the chip does: the Linux driver runs 802.11s on the same firmware. So this file
 * reuses morselib's access-point code and changes three things on the way to the chip: the
 * interface type becomes MESH instead of AP (a wrap on the interface-create call), one extra
 * chip command switches mesh beaconing on (MESH_CONFIG, the command the Linux driver sends),
 * and the SSID element in every beacon is rewritten into a Mesh ID element. Data travels in
 * 4-address mesh data frames built here and handed to morselib's management transmit helper,
 * with a wrap on the driver's transmit entry marking them as data. Every frame the chip gives
 * the host passes through a wrap on the receive entry, which takes the mesh frames and leaves
 * the rest to morselib. The esp-halow build renames morselib's internals with an mmint_
 * prefix, which is why the wrapped names look the way they do.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "mmhalow.h"
#include "mmpkt.h"
#include "mmrc.h"

#include "mesh.h"

static const char *TAG = "mesh";

/* Chip command wire format: morselib's morse_commands.h (command set 57). MESH_CONFIG and its
 * values come from the Linux driver's copy of the same header (morse_driver, set 57.13). */
#define CMD_ADD_INTERFACE 0x0004
#define CMD_MESH_CONFIG 0x0039
#define VIF_ID_INVALID 0xffff
#define IF_TYPE_AP 2
#define IF_TYPE_MESH 5
#define MESH_CONFIG_OPCODE_START 0
#define MESH_MBCA_CONFIG 1          /* TBTT selection enabled, the Linux default */
#define MESH_MIN_BEACON_GAP_MS 25
#define MESH_TBTT_ADJ_MS 60000
#define MESH_SCAN_MS 2048

#define IE_SSID 0
#define IE_MESH_ID 114

#define TTL_DEFAULT 8
#define MAX_NEIGHBOURS 16
#define MAX_ROUTES 32
#define DEDUPE_RING 128
#define RX_QUEUE_LEN 24
#define NEIGHBOUR_TIMEOUT_US (60 * 1000000LL)
#define ROUTE_TIMEOUT_US (180 * 1000000LL)
#define ROUTE_STALE_US (30 * 1000000LL)
#define MAX_FRAME 1600
#define PKT_CLASS_DATA_TID0 0
#define RATE_ADAPT 1        /* 0: every frame at MCS0 like morselib's management frames */

/* 802.11 frame layout used here: QoS data, To DS + From DS, four addresses, Mesh Control. */
#define HDR_LEN 32          /* fc 2, duration 2, a1 6, a2 6, a3 6, seq 2, a4 6, qos 2 */
#define MESH_CTRL_LEN 6     /* flags 1, ttl 1, sequence 4 */
#define LLC_LEN 8           /* aa aa 03 00 00 00 + ethertype */
#define FRAME_OVERHEAD (HDR_LEN + MESH_CTRL_LEN + LLC_LEN)

struct __attribute__((packed)) cmd_hdr
{
    uint16_t flags, message_id, len, host_id, vif_id, pad;
};

struct __attribute__((packed)) add_if_req
{
    struct cmd_hdr hdr;
    uint8_t addr[6];
    uint32_t interface_type;
};

struct __attribute__((packed)) cmd_resp
{
    struct cmd_hdr hdr;
    uint32_t status;
};

struct __attribute__((packed)) mesh_config_req
{
    struct cmd_hdr hdr;
    uint8_t mesh_cfg_opcode;
    uint8_t enable_beaconing;
    uint8_t mbca_config;
    uint8_t min_beacon_gap_ms;
    uint16_t mbss_start_scan_duration_ms;
    uint16_t tbtt_adj_timer_interval_ms;
};

/* Layout of morselib's receive metadata (mmdrv.h), for the RSSI. */
struct rx_meta
{
    int16_t rssi;
    uint16_t freq_100khz;
    uint8_t bw_mhz;
    uint8_t flags;
    int8_t noise_dbm;
    uint8_t vif_id;
    uint32_t read_timestamp_ms;
};

/* Morselib internals this file borrows. All are plain functions in morselib's source; the
 * esp-halow build renames them with an mmint_ prefix. */
struct driver_data;
struct morse_cmd_req;
struct morse_cmd_resp;
struct umac_data;
struct mmrc_rate;
enum mmwlan_status __real_mmint_morse_cmd_tx(struct driver_data *driverd, struct morse_cmd_resp *resp,
                                             struct morse_cmd_req *cmd, uint32_t resp_maxlen,
                                             uint32_t timeout_ms, bool skip_errlog);
enum mmwlan_status __real_mmint_mmdrv_add_if(uint16_t *vif_id, const uint8_t *addr, int type);
struct mmpkt *__real_mmint_umac_ap_get_beacon(struct umac_data *umacd);
void __real_mmint_mmdrv_host_process_rx_frame(struct mmpkt *rxbuf, uint16_t channel);
enum mmwlan_status __real_mmint_mmdrv_tx_frame(struct mmpkt *mmpkt, bool is_mgmt);
void __real_mmint_umac_rc_init_rate_table_mgmt(struct umac_data *umacd, struct mmrc_rate_table *table, bool rts_required);
struct umac_data *mmint_umac_data_get_umacd(void);
struct mmpkt *mmint_umac_datapath_alloc_raw_tx_mmpkt(uint8_t pkt_class, uint32_t space_at_start, uint32_t space_at_end);
enum mmwlan_status mmint_umac_datapath_tx_mgmt_frame_ap(struct umac_data *umacd, struct mmpkt *txbuf,
                                                        struct mmrc_rate *rate_override);

/* ---------- State ---------- */

static struct mesh_config s_cfg;
static uint8_t s_addr[6];
static volatile bool s_up;
static esp_netif_t *s_netif;
static struct driver_data *s_driverd;
static struct umac_data *s_umacd;
static uint16_t s_mesh_vif = VIF_ID_INVALID;
static volatile bool s_force_mesh_vif;
static volatile bool s_patch_mesh_id;
static struct mmpkt *s_data_pkt_in_flight;
static int16_t s_tx_rssi_hint;   /* RSSI of the next hop for the frame in flight, INT16_MIN for broadcast */
static char s_fw_version[32];
static const struct mmwlan_s1g_channel *s_channel;

static struct mesh_neighbour s_neighbours[MAX_NEIGHBOURS];
static struct mesh_route s_routes[MAX_ROUTES];
static struct
{
    uint8_t src[6];
    uint32_t seq;
} s_dedupe[DEDUPE_RING];
static size_t s_dedupe_next;
static uint32_t s_seq;
static SemaphoreHandle_t s_lock;    /* tables and stats */
static SemaphoreHandle_t s_tx_lock; /* one frame in the transmit helper at a time */
static QueueHandle_t s_rx_queue;
static struct mesh_stats s_stats;

static const uint8_t BROADCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

struct rx_item
{
    uint16_t len;
    int16_t rssi;
    uint8_t data[];
};

static inline bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static void mac_str(const uint8_t *m, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ---------- esp-halow workarounds (see the Ottercam READMEs) ---------- */

esp_err_t __real_spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *dev_config,
                                    spi_device_handle_t *handle);

esp_err_t __wrap_spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *dev_config,
                                    spi_device_handle_t *handle)
{
    if (host == SPI2_HOST && dev_config->spics_io_num < 0 && dev_config->clock_speed_hz > CONFIG_CHAT_HALOW_SPI_MAX_HZ)
    {
        spi_device_interface_config_t capped = *dev_config;
        capped.clock_speed_hz = CONFIG_CHAT_HALOW_SPI_MAX_HZ;
        return __real_spi_bus_add_device(host, &capped, handle);
    }
    return __real_spi_bus_add_device(host, dev_config, handle);
}

void __wrap_mmhal_wlan_hard_reset(void)
{
    gpio_set_level(CONFIG_MM_RESET_N, 0);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CHAT_HALOW_RESET_HOLD_MS));
    gpio_set_level(CONFIG_MM_RESET_N, 1);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CHAT_HALOW_RESET_SETTLE_MS));
}

/* The whole mesh lives on one channel; give morselib only the channels inside it (2 MHz and
 * up). Nothing scans in this firmware, but the window keeps any probe short, which weak USB
 * ports need, and it records the channel for the status page. */
static struct mmwlan_s1g_channel s_chan_set[16];
static struct mmwlan_s1g_channel_list s_chan_list;

enum mmwlan_status __real_mmwlan_set_channel_list(const struct mmwlan_s1g_channel_list *list);

enum mmwlan_status __wrap_mmwlan_set_channel_list(const struct mmwlan_s1g_channel_list *list)
{
    const struct mmwlan_s1g_channel *anchor = NULL;
    for (unsigned i = 0; list != NULL && i < list->num_channels; i++)
    {
        const struct mmwlan_s1g_channel *ch = &list->channels[i];
        if (ch->s1g_chan_num == s_cfg.s1g_chan &&
            (ch->global_operating_class == s_cfg.opclass || ch->s1g_operating_class == s_cfg.opclass))
        {
            anchor = ch;
            break;
        }
    }
    if (anchor == NULL)
    {
        ESP_LOGE(TAG, "Channel %u/%u isn't in the %s list; using the full list", s_cfg.s1g_chan, s_cfg.opclass,
                 CONFIG_HALOW_COUNTRY_CODE);
        return __real_mmwlan_set_channel_list(list);
    }
    s_channel = anchor;
    uint32_t low = anchor->centre_freq_hz - anchor->bw_mhz * 500000U;
    uint32_t high = anchor->centre_freq_hz + anchor->bw_mhz * 500000U;
    unsigned n = 0;
    for (unsigned i = 0; i < list->num_channels && n < 16; i++)
    {
        const struct mmwlan_s1g_channel *ch = &list->channels[i];
        uint32_t ch_low = ch->centre_freq_hz - ch->bw_mhz * 500000U;
        uint32_t ch_high = ch->centre_freq_hz + ch->bw_mhz * 500000U;
        if (ch_low >= low && ch_high <= high && ch->bw_mhz >= 2)
        {
            s_chan_set[n++] = *ch;
        }
    }
    s_chan_list = *list;
    s_chan_list.num_channels = n;
    s_chan_list.channels = s_chan_set;
    return __real_mmwlan_set_channel_list(&s_chan_list);
}

/* ---------- Chip commands ---------- */

/* Every command morselib sends to the chip passes through here. Remember the driver handle so
 * this file can send its own, and log anything the chip rejects. */
enum mmwlan_status __wrap_mmint_morse_cmd_tx(struct driver_data *driverd, struct morse_cmd_resp *resp,
                                             struct morse_cmd_req *cmd, uint32_t resp_maxlen,
                                             uint32_t timeout_ms, bool skip_errlog)
{
    s_driverd = driverd;
    const struct cmd_hdr *hdr = (const struct cmd_hdr *)cmd;
    uint16_t id = hdr->message_id;
    enum mmwlan_status st = __real_mmint_morse_cmd_tx(driverd, resp, cmd, resp_maxlen, timeout_ms, skip_errlog);
    if (st != MMWLAN_SUCCESS)
    {
        const struct cmd_resp *r = (const struct cmd_resp *)resp;
        ESP_LOGW(TAG, "Chip rejected command 0x%04x (status %d, chip status %ld)", id, (int)st,
                 r != NULL ? (long)(int32_t)r->status : 0L);
    }
    return st;
}

static enum mmwlan_status send_raw(uint16_t id, uint16_t vif, void *req, size_t req_len, int32_t *chip_status)
{
    struct cmd_hdr *hdr = (struct cmd_hdr *)req;
    memset(hdr, 0, sizeof(*hdr));
    hdr->message_id = id;
    hdr->len = req_len - sizeof(*hdr);
    hdr->vif_id = vif;
    struct cmd_resp resp = { 0 };
    enum mmwlan_status st = __real_mmint_morse_cmd_tx(s_driverd, (struct morse_cmd_resp *)&resp,
                                                      (struct morse_cmd_req *)req, sizeof(resp), 3000, false);
    if (chip_status != NULL)
    {
        *chip_status = (int32_t)resp.status;
    }
    return st;
}

/* Morselib asks the chip for its access-point interface through here; hand it a mesh one. */
enum mmwlan_status __wrap_mmint_mmdrv_add_if(uint16_t *vif_id, const uint8_t *addr, int type)
{
    if (s_force_mesh_vif && type == IF_TYPE_AP)
    {
        struct add_if_req req = { 0 };
        memcpy(req.addr, addr, sizeof(req.addr));
        req.interface_type = IF_TYPE_MESH;
        int32_t chip_status = 0;
        struct cmd_resp resp = { 0 };
        req.hdr.message_id = CMD_ADD_INTERFACE;
        req.hdr.len = sizeof(req) - sizeof(req.hdr);
        req.hdr.vif_id = VIF_ID_INVALID;
        enum mmwlan_status st = __real_mmint_morse_cmd_tx(s_driverd, (struct morse_cmd_resp *)&resp,
                                                          (struct morse_cmd_req *)&req, sizeof(resp), 3000, false);
        chip_status = (int32_t)resp.status;
        *vif_id = resp.hdr.vif_id;
        s_mesh_vif = *vif_id;
        ESP_LOGI(TAG, "Mesh interface: %s (chip status %ld, vif %u)", st == MMWLAN_SUCCESS ? "created" : "REFUSED",
                 (long)chip_status, *vif_id);
        return st;
    }
    return __real_mmint_mmdrv_add_if(vif_id, addr, type);
}

/* The chip asks the host for a beacon before every beacon time; morselib builds it here with
 * an SSID element. A mesh beacon carries a Mesh ID element instead: same length, one byte. */
struct mmpkt *__wrap_mmint_umac_ap_get_beacon(struct umac_data *umacd)
{
    struct mmpkt *beacon = __real_mmint_umac_ap_get_beacon(umacd);
    if (beacon == NULL)
    {
        return NULL;
    }
    s_stats.beacons_built++;
    if (s_patch_mesh_id)
    {
        struct mmpktview *view = mmpkt_open(beacon);
        uint8_t *data = mmpkt_get_data_start(view);
        uint32_t len = mmpkt_get_data_length(view);
        size_t id_len = strlen(s_cfg.mesh_id);
        for (uint32_t i = 15; i + 2 + id_len <= len; i++)
        {
            if (data[i] == IE_SSID && data[i + 1] == id_len && memcmp(&data[i + 2], s_cfg.mesh_id, id_len) == 0)
            {
                data[i] = IE_MESH_ID;
                break;
            }
        }
        mmpkt_close(&view);
    }
    return beacon;
}

/* Frames this file builds go to the chip as data, not management. */
enum mmwlan_status __wrap_mmint_mmdrv_tx_frame(struct mmpkt *mmpkt, bool is_mgmt)
{
    if (mmpkt != NULL && mmpkt == s_data_pkt_in_flight)
    {
        is_mgmt = false;
    }
    return __real_mmint_mmdrv_tx_frame(mmpkt, is_mgmt);
}

/* Morselib's management transmit helper picks MCS0 for everything. For our unicast data frames,
 * start higher when the next hop's beacons are strong, and fall back down the chain. */
static enum mmrc_bw channel_bw(void)
{
    uint8_t bw = s_channel != NULL ? s_channel->bw_mhz : 2;
    return bw == 1 ? MMRC_BW_1MHZ : bw == 4 ? MMRC_BW_4MHZ : bw == 8 ? MMRC_BW_8MHZ : MMRC_BW_2MHZ;
}

void __wrap_mmint_umac_rc_init_rate_table_mgmt(struct umac_data *umacd, struct mmrc_rate_table *table, bool rts_required)
{
    if (!RATE_ADAPT || s_data_pkt_in_flight == NULL || s_tx_rssi_hint == INT16_MIN)
    {
        __real_mmint_umac_rc_init_rate_table_mgmt(umacd, table, rts_required);
        return;
    }
    int16_t rssi = s_tx_rssi_hint;
    uint8_t top = rssi >= -55 ? MMRC_MCS5 : rssi >= -62 ? MMRC_MCS4 : rssi >= -70 ? MMRC_MCS3 : rssi >= -78 ? MMRC_MCS2 : rssi >= -86 ? MMRC_MCS1 : MMRC_MCS0;
    uint8_t mid = top >= 2 ? top - 2 : MMRC_MCS0;
    const uint8_t chain[3] = { top, mid, MMRC_MCS0 };
    const uint8_t attempts[3] = { 2, 2, 3 };
    memset(table, 0, sizeof(*table));
    for (int i = 0; i < 3; i++)
    {
        table->rates[i].rate = chain[i];
        table->rates[i].attempts = attempts[i];
        table->rates[i].bw = channel_bw();
        table->rates[i].guard = MMRC_GUARD_LONG;
        table->rates[i].ss = MMRC_SPATIAL_STREAM_1;
        table->rates[i].flags = 0;
    }
    table->rates[3].rate = MMRC_MCS_UNUSED;
    if (top < 8)
    {
        s_stats.tx_by_mcs[top]++;
    }
}

/* ---------- Tables ---------- */

static void neighbour_heard(const uint8_t *addr, int16_t rssi)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    struct mesh_neighbour *slot = NULL, *oldest = &s_neighbours[0];
    for (int i = 0; i < MAX_NEIGHBOURS; i++)
    {
        struct mesh_neighbour *n = &s_neighbours[i];
        if (n->beacons != 0 && mac_eq(n->addr, addr))
        {
            slot = n;
            break;
        }
        if (n->beacons == 0 || n->last_seen_us < oldest->last_seen_us)
        {
            oldest = n;
        }
    }
    if (slot == NULL)
    {
        slot = oldest;
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->addr, addr, 6);
        char a[18];
        mac_str(addr, a);
        ESP_LOGI(TAG, "Neighbour %s heard (rssi %d)", a, rssi);
    }
    slot->rssi = rssi;
    slot->beacons++;
    slot->last_seen_us = now;
    s_stats.beacons_heard++;
    xSemaphoreGive(s_lock);
}

static int16_t neighbour_rssi(const uint8_t *addr)
{
    int16_t rssi = INT16_MIN;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_NEIGHBOURS; i++)
    {
        if (s_neighbours[i].beacons != 0 && mac_eq(s_neighbours[i].addr, addr) &&
            now - s_neighbours[i].last_seen_us < NEIGHBOUR_TIMEOUT_US)
        {
            rssi = s_neighbours[i].rssi;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return rssi;
}

/* A frame from SA came in via TA: TA is a next hop towards SA, hops away. */
static void route_learn(const uint8_t *dst, const uint8_t *next_hop, uint8_t hops)
{
    if (mac_eq(dst, next_hop))
    {
        hops = 1;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    struct mesh_route *slot = NULL, *oldest = &s_routes[0];
    for (int i = 0; i < MAX_ROUTES; i++)
    {
        struct mesh_route *r = &s_routes[i];
        if (r->last_us != 0 && mac_eq(r->dst, dst))
        {
            slot = r;
            break;
        }
        if (r->last_us < oldest->last_us)
        {
            oldest = r;
        }
    }
    if (slot == NULL)
    {
        slot = oldest;
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->dst, dst, 6);
        slot->hops = 255;
    }
    bool better = hops <= slot->hops || now - slot->last_us > ROUTE_STALE_US;
    if (better)
    {
        memcpy(slot->next_hop, next_hop, 6);
        slot->hops = hops;
        slot->last_us = now;
    }
    else if (mac_eq(slot->next_hop, next_hop))
    {
        slot->last_us = now;
    }
    xSemaphoreGive(s_lock);
}

static bool route_lookup(const uint8_t *dst, uint8_t *next_hop)
{
    int64_t now = esp_timer_get_time();
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_ROUTES; i++)
    {
        struct mesh_route *r = &s_routes[i];
        if (r->last_us != 0 && mac_eq(r->dst, dst) && now - r->last_us < ROUTE_TIMEOUT_US)
        {
            memcpy(next_hop, r->next_hop, 6);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

/* Returns true if (src, seq) was seen recently; records it otherwise. */
static bool dedupe_seen(const uint8_t *src, uint32_t seq)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < DEDUPE_RING; i++)
    {
        if (s_dedupe[i].seq == seq && mac_eq(s_dedupe[i].src, src))
        {
            xSemaphoreGive(s_lock);
            return true;
        }
    }
    memcpy(s_dedupe[s_dedupe_next].src, src, 6);
    s_dedupe[s_dedupe_next].seq = seq;
    s_dedupe_next = (s_dedupe_next + 1) % DEDUPE_RING;
    xSemaphoreGive(s_lock);
    return false;
}

/* ---------- Transmit ---------- */

/* Sends one 4-address mesh data frame: receiver (next hop) ra, destination da, source sa. */
static esp_err_t send_frame(const uint8_t *ra, const uint8_t *da, const uint8_t *sa, uint8_t ttl, uint32_t seq,
                            uint16_t ethertype, const uint8_t *payload, size_t payload_len)
{
    size_t len = FRAME_OVERHEAD + payload_len;
    if (len > MAX_FRAME)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_up || s_umacd == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    struct mmpkt *pkt = mmint_umac_datapath_alloc_raw_tx_mmpkt(PKT_CLASS_DATA_TID0, 0, len);
    if (pkt == NULL)
    {
        s_stats.tx_failed++;
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_NO_MEM;
    }
    struct mmpktview *view = mmpkt_open(pkt);
    if (mmpkt_available_space_at_end(view) < len)
    {
        mmpkt_close(&view);
        mmpkt_release(pkt);
        s_stats.tx_failed++;
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *p = mmpkt_append(view, len);
    memset(p, 0, HDR_LEN);
    p[0] = 0x88;                 /* data, QoS data */
    p[1] = 0x03;                 /* To DS + From DS: four addresses */
    memcpy(&p[4], ra, 6);        /* address 1: receiver, the next hop */
    memcpy(&p[10], s_addr, 6);   /* address 2: transmitter, us */
    memcpy(&p[16], da, 6);       /* address 3: final destination */
    memcpy(&p[24], sa, 6);       /* address 4: original source */
    p[30] = 0x00;                /* QoS control: TID 0 */
    p[31] = 0x01;                /* Mesh Control present */
    p[32] = 0x00;                /* mesh flags */
    p[33] = ttl;
    p[34] = (uint8_t)seq;
    p[35] = (uint8_t)(seq >> 8);
    p[36] = (uint8_t)(seq >> 16);
    p[37] = (uint8_t)(seq >> 24);
    static const uint8_t llc[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
    memcpy(&p[38], llc, sizeof(llc));
    p[44] = (uint8_t)(ethertype >> 8);
    p[45] = (uint8_t)ethertype;
    memcpy(&p[46], payload, payload_len);
    mmpkt_close(&view);

    s_tx_rssi_hint = (ra[0] & 1) ? INT16_MIN : neighbour_rssi(ra);
    s_data_pkt_in_flight = pkt;
    enum mmwlan_status st = mmint_umac_datapath_tx_mgmt_frame_ap(s_umacd, pkt, NULL);
    s_data_pkt_in_flight = NULL;
    xSemaphoreGive(s_tx_lock);
    if (st != MMWLAN_SUCCESS)
    {
        s_stats.tx_failed++;
        return ESP_FAIL;
    }
    s_stats.tx_frames++;
    return ESP_OK;
}

/* lwIP hands us an Ethernet frame: destination, source, type, payload. */
static esp_err_t mesh_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (len < 14)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *eth = (const uint8_t *)buffer;
    const uint8_t *dst = eth;
    uint16_t ethertype = (eth[12] << 8) | eth[13];
    uint8_t next_hop[6];
    if ((dst[0] & 1) || !route_lookup(dst, next_hop))
    {
        memcpy(next_hop, BROADCAST, 6);
        if (!(dst[0] & 1))
        {
            s_stats.tx_flooded++;
        }
    }
    uint32_t seq = ++s_seq;
    return send_frame(next_hop, dst, s_addr, s_cfg.ttl, seq, ethertype, eth + 14, len - 14);
}

static esp_err_t mesh_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    (void)netstack_buf;
    return mesh_transmit(h, buffer, len);
}

static void mesh_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

static esp_err_t mesh_post_attach(esp_netif_t *netif, void *h)
{
    esp_netif_driver_ifconfig_t cfg = {
        .handle = h,
        .transmit = mesh_transmit,
        .transmit_wrap = mesh_transmit_wrap,
        .driver_free_rx_buffer = mesh_free_rx,
    };
    return esp_netif_set_driver_config(netif, &cfg);
}

/* ---------- Receive ---------- */

/* Every frame the chip hands the host lands here first, in morselib's driver task. Beacons
 * feed the neighbour table and go on to morselib; mesh data frames are queued for the mesh
 * task and taken away from morselib, which would only drop them. */
void __wrap_mmint_mmdrv_host_process_rx_frame(struct mmpkt *rxbuf, uint16_t channel)
{
    struct mmpktview *view = mmpkt_open(rxbuf);
    const uint8_t *d = mmpkt_get_data_start(view);
    uint32_t len = mmpkt_get_data_length(view);
    bool consumed = false;
    if (len >= 16)
    {
        uint16_t fc = d[0] | (d[1] << 8);
        unsigned ftype = (fc >> 2) & 3, subtype = (fc >> 4) & 0xF;
        int16_t rssi = ((struct rx_meta *)mmpkt_get_metadata(rxbuf).opaque)->rssi;
        if (ftype == 3 && subtype == 1)
        {
            /* S1G beacon: look for our Mesh ID among its elements. */
            uint32_t i = 15 + ((fc & 0x0100) ? 3 : 0) + ((fc & 0x0200) ? 4 : 0) + ((fc & 0x0400) ? 1 : 0);
            size_t id_len = strlen(s_cfg.mesh_id);
            while (i + 2 <= len)
            {
                uint8_t id = d[i], l = d[i + 1];
                if (i + 2 + l > len)
                {
                    break;
                }
                if (id == IE_MESH_ID && l == id_len && memcmp(&d[i + 2], s_cfg.mesh_id, id_len) == 0)
                {
                    neighbour_heard(&d[4], rssi);
                    break;
                }
                i += 2 + l;
            }
        }
        else if (ftype == 2 && (d[1] & 0x03) == 0x03 && len >= HDR_LEN)
        {
            consumed = true;
            if (!mac_eq(&d[24], s_addr))   /* our own frame relayed back to us */
            {
                struct rx_item *it = malloc(sizeof(*it) + len);
                if (it != NULL)
                {
                    it->len = (uint16_t)len;
                    it->rssi = rssi;
                    memcpy(it->data, d, len);
                    if (xQueueSend(s_rx_queue, &it, 0) != pdTRUE)
                    {
                        free(it);
                        s_stats.rx_queue_full++;
                    }
                }
            }
        }
        else if (ftype == 0 && subtype == 13 && len >= 26 && d[24] == 15)
        {
            consumed = true;   /* mesh peering frames: nothing to do with them yet */
        }
    }
    mmpkt_close(&view);
    if (consumed)
    {
        mmpkt_release(rxbuf);
        return;
    }
    __real_mmint_mmdrv_host_process_rx_frame(rxbuf, channel);
}

static void mesh_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        struct rx_item *it = NULL;
        if (xQueueReceive(s_rx_queue, &it, portMAX_DELAY) != pdTRUE || it == NULL)
        {
            continue;
        }
        const uint8_t *d = it->data;
        uint32_t len = it->len;
        const uint8_t *ra = &d[4], *ta = &d[10], *da = &d[16], *sa = &d[24];
        bool qos = ((d[0] >> 4) & 0x08) != 0;
        uint32_t body_at = qos ? 32 : 30;
        bool mesh_ctrl = qos && (d[31] & 0x01);
        uint8_t ttl = 1;
        uint32_t seq = 0;
        if (mesh_ctrl && len >= body_at + MESH_CTRL_LEN)
        {
            ttl = d[body_at + 1];
            seq = d[body_at + 2] | (d[body_at + 3] << 8) | ((uint32_t)d[body_at + 4] << 16) | ((uint32_t)d[body_at + 5] << 24);
        }
        uint32_t llc_at = body_at + (mesh_ctrl ? MESH_CTRL_LEN : 0);
        s_stats.rx_frames++;

        if (dedupe_seen(sa, seq))
        {
            s_stats.rx_duplicates++;
            free(it);
            continue;
        }
        uint8_t hops = ttl < s_cfg.ttl ? (uint8_t)(s_cfg.ttl - ttl + 1) : 1;
        route_learn(sa, ta, hops);

        uint16_t ethertype = 0;
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        if (len >= llc_at + LLC_LEN && d[llc_at] == 0xaa && d[llc_at + 1] == 0xaa)
        {
            ethertype = (d[llc_at + 6] << 8) | d[llc_at + 7];
            payload = &d[llc_at + LLC_LEN];
            payload_len = len - llc_at - LLC_LEN;
        }
        bool for_me = mac_eq(da, s_addr);
        bool group = (da[0] & 1) != 0;

        if ((for_me || group) && payload != NULL)
        {
            uint8_t *eth = malloc(14 + payload_len);
            if (eth != NULL)
            {
                memcpy(eth, da, 6);
                memcpy(eth + 6, sa, 6);
                eth[12] = (uint8_t)(ethertype >> 8);
                eth[13] = (uint8_t)ethertype;
                memcpy(eth + 14, payload, payload_len);
                if (esp_netif_receive(s_netif, eth, 14 + payload_len, eth) != ESP_OK)
                {
                    free(eth);
                }
                else
                {
                    s_stats.rx_delivered++;
                }
            }
        }

        /* Relay: group frames always, unicast only when we were chosen as the next hop. */
        if (payload != NULL && ttl > 1 && (group || (!for_me && mac_eq(ra, s_addr))))
        {
            uint8_t next_hop[6];
            if (group || !route_lookup(da, next_hop))
            {
                memcpy(next_hop, BROADCAST, 6);
            }
            if (send_frame(next_hop, da, sa, ttl - 1, seq, ethertype, payload, payload_len) == ESP_OK)
            {
                s_stats.rx_forwarded++;
            }
        }
        free(it);
    }
}

/* ---------- Bring-up ---------- */

static bool guard_was_armed(void)
{
    nvs_handle_t nvs;
    uint8_t v = 0;
    if (nvs_open("mesh", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_get_u8(nvs, "armed", &v);
        nvs_erase_key(nvs, "armed");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return v != 0;
}

static void guard_set(bool armed)
{
    nvs_handle_t nvs;
    if (nvs_open("mesh", NVS_READWRITE, &nvs) == ESP_OK)
    {
        if (armed)
        {
            nvs_set_u8(nvs, "armed", 1);
        }
        else
        {
            nvs_erase_key(nvs, "armed");
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

esp_err_t mesh_start(const struct mesh_config *cfg)
{
    s_cfg = *cfg;
    if (s_cfg.ttl == 0)
    {
        s_cfg.ttl = TTL_DEFAULT;
    }
    s_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(struct rx_item *));

    /* Boots the chip and loads its firmware. esp-halow also creates a station interface,
     * which this firmware never uses. */
    ESP_ERROR_CHECK(mmhalow_init(NULL));
    struct mmwlan_version v = { 0 };
    mmwlan_get_version(&v);
    strlcpy(s_fw_version, v.morse_fw_version, sizeof(s_fw_version));
    if (s_driverd == NULL)
    {
        ESP_LOGE(TAG, "No chip command seen; the morselib hooks are not linked in");
        return ESP_FAIL;
    }
    s_umacd = mmint_umac_data_get_umacd();
    if (s_cfg.tx_dbm > 0)
    {
        mmwlan_override_max_tx_power(s_cfg.tx_dbm);
    }

    struct mmwlan_ap_args args = MMWLAN_AP_ARGS_INIT;
    args.ssid_len = strlen(s_cfg.mesh_id);
    memcpy(args.ssid, s_cfg.mesh_id, args.ssid_len);
    args.security_type = MMWLAN_OPEN;
    args.pmf_mode = MMWLAN_PMF_DISABLED;
    args.op_class = s_cfg.opclass;
    args.s1g_chan_num = s_cfg.s1g_chan;
    args.max_stas = 1;
    s_force_mesh_vif = true;
    enum mmwlan_status st = mmwlan_ap_enable(&args);
    s_force_mesh_vif = false;
    if (st != MMWLAN_SUCCESS || s_mesh_vif == VIF_ID_INVALID)
    {
        ESP_LOGE(TAG, "Mesh interface failed to start (status %d)", (int)st);
        return ESP_FAIL;
    }

    /* Switch the chip's mesh beaconing on. With zeroed timing fields this command crashes the
     * chip; a flag that survives the reboot makes sure a crash can't loop. */
    if (guard_was_armed())
    {
        ESP_LOGW(TAG, "MESH_CONFIG crashed the chip last boot; skipping it this boot (no beacons)");
    }
    else
    {
        struct mesh_config_req mc = { 0 };
        mc.mesh_cfg_opcode = MESH_CONFIG_OPCODE_START;
        mc.enable_beaconing = 1;
        mc.mbca_config = MESH_MBCA_CONFIG;
        mc.min_beacon_gap_ms = MESH_MIN_BEACON_GAP_MS;
        mc.tbtt_adj_timer_interval_ms = MESH_TBTT_ADJ_MS;
        mc.mbss_start_scan_duration_ms = MESH_SCAN_MS;
        int32_t chip_status = 0;
        guard_set(true);
        st = send_raw(CMD_MESH_CONFIG, s_mesh_vif, &mc, sizeof(mc), &chip_status);
        vTaskDelay(pdMS_TO_TICKS(1000));
        guard_set(false);
        if (st != MMWLAN_SUCCESS)
        {
            ESP_LOGE(TAG, "MESH_CONFIG failed (status %d, chip status %ld); no beacons", (int)st, (long)chip_status);
        }
    }
    s_patch_mesh_id = true;
    mmwlan_ap_get_bssid(s_addr);

    /* The network interface: an Ethernet-style lwIP interface whose link is this file. */
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base.if_key = "MESH";
    base.if_desc = "mesh";
    base.flags = (esp_netif_flags_t)(ESP_NETIF_FLAG_GARP | ESP_NETIF_FLAG_EVENT_IP_MODIFIED);
    base.route_prio = 50;
    esp_netif_config_t netif_cfg = { .base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA };
    s_netif = esp_netif_new(&netif_cfg);
    if (s_netif == NULL)
    {
        ESP_LOGE(TAG, "Could not create the mesh network interface");
        return ESP_FAIL;
    }
    static esp_netif_driver_base_t driver = { .post_attach = mesh_post_attach };
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, &driver));
    esp_netif_set_mac(s_netif, s_addr);
    esp_netif_action_start(s_netif, NULL, 0, NULL);

    xTaskCreate(mesh_task, "mesh", 6144, NULL, 6, NULL);
    s_up = true;
    char a[18];
    mac_str(s_addr, a);
    ESP_LOGI(TAG, "Mesh \"%s\" up on channel %u, address %s, chip firmware %s, hop limit %u", s_cfg.mesh_id,
             s_cfg.s1g_chan, a, s_fw_version, s_cfg.ttl);
    return ESP_OK;
}

/* ---------- Status ---------- */

bool mesh_is_up(void)
{
    return s_up;
}

esp_netif_t *mesh_netif(void)
{
    return s_netif;
}

void mesh_get_addr(uint8_t out[6])
{
    memcpy(out, s_addr, 6);
}

const char *mesh_chip_firmware(void)
{
    return s_fw_version;
}

bool mesh_channel_info(uint32_t *centre_freq_hz, uint8_t *bw_mhz)
{
    if (s_channel == NULL)
    {
        return false;
    }
    *centre_freq_hz = s_channel->centre_freq_hz;
    *bw_mhz = s_channel->bw_mhz;
    return true;
}

int mesh_get_neighbours(struct mesh_neighbour *out, int max)
{
    int64_t now = esp_timer_get_time();
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_NEIGHBOURS && n < max; i++)
    {
        if (s_neighbours[i].beacons != 0 && now - s_neighbours[i].last_seen_us < NEIGHBOUR_TIMEOUT_US)
        {
            out[n++] = s_neighbours[i];
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

int mesh_get_routes(struct mesh_route *out, int max)
{
    int64_t now = esp_timer_get_time();
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_ROUTES && n < max; i++)
    {
        if (s_routes[i].last_us != 0 && now - s_routes[i].last_us < ROUTE_TIMEOUT_US)
        {
            out[n++] = s_routes[i];
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

void mesh_get_stats(struct mesh_stats *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_lock);
}
