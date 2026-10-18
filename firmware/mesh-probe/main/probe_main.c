/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Mesh probes for the Seeed Wio-WM6180 (Morse Micro MM6108) on a XIAO ESP32S3.
 *
 * Experiment 1 (interfaces): ask the chip firmware for MON/ADHOC/MESH interfaces and report
 * what it says. Result 2026-10-18: all accepted (see README).
 *
 * Experiment 2 (mesh beacon, two boards):
 *   BEACONER (board F034): start the normal access-point code, but hand the chip a MESH
 *   interface instead of an AP one. Morselib then configures the BSS, the chip schedules
 *   beacons on that interface and asks the host for each one (the host builds every beacon).
 *   Phase A beacons as-is (SSID "meshprobe"). Phase B rewrites the SSID element into a
 *   Mesh ID element, which is what an 802.11s mesh beacon carries.
 *   LISTENER (the other board): scans continuously and logs every frame the chip hands up,
 *   raw, before morselib's parser (which drops anything without an SSID element).
 *
 * Experiment 3 (mesh node, both boards the same): every board brings up a mesh interface with
 *   Mesh ID beacons as in experiment 2, learns its neighbours from their beacons, and sends each
 *   one a Mesh Peering Open action frame (category 15, self-protected) every few seconds, built on
 *   the host and sent through morselib's management transmit path. Received action frames are
 *   logged raw. This is the frame exchange that 802.11s peering is built on.
 *
 * Experiment 4 (added to the mesh node): every 2 s each board also sends each neighbour a
 *   4-address QoS data frame with a Mesh Control field, the frame format 802.11s mesh data
 *   uses. The frame is built on the host; morselib's management transmit helper fills in the
 *   chip metadata and a wrap on the driver's transmit entry marks it as data, not management.
 *   Received 4-address data frames are logged raw with their payload.
 */

#define PROBE_EXPERIMENT 3

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "mmhalow.h"
#include "mmpkt.h"

static const char *TAG = "probe";

/* Bench values that keep this hardware booting reliably (same as HalowChat). */
#define HALOW_SPI_MAX_HZ 40000000
#define HALOW_RESET_HOLD_MS 8000
#define HALOW_RESET_SETTLE_MS 250
#define S1G_CHANNEL 26
#define S1G_OPCLASS 69
#define TX_CAP_DBM 8

#define BEACONER_NODE 0xF034   /* XIAO efuse MAC last two bytes of the board that beacons */
#define MESH_SSID "meshprobe"
#define CONTROL_S 10
#define PHASE_A_S 20
#define PHASE_C_S 30
#define PHASE_D_S 30
#define PHASE_B_S 30

/* From the Linux driver's command set (57.13.0; this SDK's is 57.0.0, same major). The
 * microcontroller SDK never sends these. */
#define CMD_MESH_CONFIG 0x0039
#define CMD_BSS_BEACON_CONFIG 0x003D
#define MESH_CONFIG_OPCODE_START 0
#define MESH_CONFIG_OPCODE_STOP 1

/* MESH_CONFIG crashed the chip firmware (FW assert) with all-zero timing fields on 2026-10-18.
 * Off until it can be retried with the Linux driver's defaults; guarded so a crash can't loop. */
#define TRY_MESH_CONFIG 1
#define MESH_MBCA_CONFIG 1          /* TBTT selection enable, the Linux default */
#define MESH_MIN_BEACON_GAP_MS 25
#define MESH_TBTT_ADJ_MS 60000
#define MESH_SCAN_MS 2048

/* Wire format of morselib's chip commands (morse_commands.h, command set 57.0.0). */
#define CMD_ADD_INTERFACE 0x0004
#define VIF_ID_INVALID 0xffff

#define IF_TYPE_STA 1
#define IF_TYPE_AP 2
#define IF_TYPE_MON 3
#define IF_TYPE_ADHOC 4
#define IF_TYPE_MESH 5

#define IE_SSID 0
#define IE_MESH_CONFIG 113
#define IE_MESH_ID 114

struct __attribute__((packed)) cmd_hdr
{
    uint16_t flags;
    uint16_t message_id;
    uint16_t len;
    uint16_t host_id;
    uint16_t vif_id;
    uint16_t pad;
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

struct __attribute__((packed)) bss_beacon_config_req
{
    struct cmd_hdr hdr;
    uint8_t enable;
};

/* morselib internals we borrow (non-static in morselib; the ESP build renames internals with an mmint_ prefix). */
struct driver_data;
struct morse_cmd_req;
struct morse_cmd_resp;
struct umac_data;
enum mmwlan_status __real_mmint_morse_cmd_tx(struct driver_data *driverd, struct morse_cmd_resp *resp,
                                             struct morse_cmd_req *cmd, uint32_t resp_maxlen,
                                             uint32_t timeout_ms, bool skip_errlog);
enum mmwlan_status mmint_mmdrv_rm_if(uint16_t vif_id);
enum mmwlan_status __real_mmint_mmdrv_add_if(uint16_t *vif_id, const uint8_t *addr, int type);
struct mmpkt *__real_mmint_umac_ap_get_beacon(struct umac_data *umacd);
void __real_mmint_umac_scan_process_probe_resp(struct umac_data *umacd, struct mmpktview *rxbufview);
struct consbuf;
struct mmrc_rate;
typedef void (*mgmt_frame_builder_t)(struct umac_data *umacd, struct consbuf *buf, void *params);
struct umac_data *mmint_umac_data_get_umacd(void);
struct mmpkt *mmint_build_mgmt_frame(struct umac_data *umacd, mgmt_frame_builder_t builder, void *params);
void mmint_consbuf_append(struct consbuf *buf, const uint8_t *data, uint32_t len);
enum mmwlan_status mmint_umac_datapath_tx_mgmt_frame_ap(struct umac_data *umacd, struct mmpkt *txbuf,
                                                        struct mmrc_rate *rate_override);

enum mmwlan_status __real_mmint_mmdrv_tx_frame(struct mmpkt *mmpkt, bool is_mgmt);
static struct mmpkt *s_data_pkt_in_flight;

enum mmwlan_status __wrap_mmint_mmdrv_tx_frame(struct mmpkt *mmpkt, bool is_mgmt)
{
    if (mmpkt != NULL && mmpkt == s_data_pkt_in_flight)
    {
        is_mgmt = false;
    }
    return __real_mmint_mmdrv_tx_frame(mmpkt, is_mgmt);
}

/* Layout of morselib's receive metadata (mmdrv.h), for the RSSI. */
struct probe_rx_meta
{
    int16_t rssi;
    uint16_t freq_100khz;
    uint8_t bw_mhz;
    uint8_t flags;
    int8_t noise_dbm;
    uint8_t vif_id;
    uint32_t read_timestamp_ms;
};

static struct driver_data *s_driverd;
static uint8_t s_my_mac[6];          /* HaLow module MAC */
static bool s_beaconer;

/* ---------- esp-halow workarounds (see the Ottercam READMEs) ---------- */

esp_err_t __real_spi_bus_add_device(spi_host_device_t host,
                                    const spi_device_interface_config_t *dev_config,
                                    spi_device_handle_t *handle);

esp_err_t __wrap_spi_bus_add_device(spi_host_device_t host,
                                    const spi_device_interface_config_t *dev_config,
                                    spi_device_handle_t *handle)
{
    if (host == SPI2_HOST && dev_config->spics_io_num < 0 && dev_config->clock_speed_hz > HALOW_SPI_MAX_HZ)
    {
        spi_device_interface_config_t capped = *dev_config;
        capped.clock_speed_hz = HALOW_SPI_MAX_HZ;
        return __real_spi_bus_add_device(host, &capped, handle);
    }
    return __real_spi_bus_add_device(host, dev_config, handle);
}

void __wrap_mmhal_wlan_hard_reset(void)
{
    gpio_set_level(CONFIG_MM_RESET_N, 0);
    vTaskDelay(pdMS_TO_TICKS(HALOW_RESET_HOLD_MS));
    gpio_set_level(CONFIG_MM_RESET_N, 1);
    vTaskDelay(pdMS_TO_TICKS(HALOW_RESET_SETTLE_MS));
}

static struct mmwlan_s1g_channel s_chan_set[16];
static struct mmwlan_s1g_channel_list s_chan_list;

enum mmwlan_status __real_mmwlan_set_channel_list(const struct mmwlan_s1g_channel_list *list);

enum mmwlan_status __wrap_mmwlan_set_channel_list(const struct mmwlan_s1g_channel_list *list)
{
    const struct mmwlan_s1g_channel *anchor = NULL;
    for (unsigned i = 0; list != NULL && i < list->num_channels; i++)
    {
        const struct mmwlan_s1g_channel *ch = &list->channels[i];
        if (ch->s1g_chan_num == S1G_CHANNEL &&
            (ch->global_operating_class == S1G_OPCLASS || ch->s1g_operating_class == S1G_OPCLASS))
        {
            anchor = ch;
            break;
        }
    }
    if (anchor == NULL)
    {
        return __real_mmwlan_set_channel_list(list);
    }
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

/* ---------- Shared helpers ---------- */

static const char *type_name(uint32_t type)
{
    switch (type)
    {
        case IF_TYPE_STA: return "STA";
        case IF_TYPE_AP: return "AP";
        case IF_TYPE_MON: return "MON";
        case IF_TYPE_ADHOC: return "ADHOC";
        case IF_TYPE_MESH: return "MESH";
        default: return "?";
    }
}

static const char *status_name(enum mmwlan_status st)
{
    switch (st)
    {
        case MMWLAN_SUCCESS: return "SUCCESS";
        case MMWLAN_COMMAND_ERROR: return "COMMAND_ERROR (chip answered with an error)";
        case MMWLAN_TIMED_OUT: return "TIMED_OUT (chip never answered)";
        case MMWLAN_NOT_RUNNING: return "NOT_RUNNING";
        case MMWLAN_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        default: return "other";
    }
}

/* Every chip command morselib sends passes through here. Remember the driver handle so the probe
 * can send its own, show interface creation, and show any command the chip rejects. */
enum mmwlan_status __wrap_mmint_morse_cmd_tx(struct driver_data *driverd, struct morse_cmd_resp *resp,
                                             struct morse_cmd_req *cmd, uint32_t resp_maxlen,
                                             uint32_t timeout_ms, bool skip_errlog)
{
    s_driverd = driverd;
    const struct cmd_hdr *hdr = (const struct cmd_hdr *)cmd;
    uint16_t id = hdr->message_id;
    uint16_t vif = hdr->vif_id;
    uint32_t type = id == CMD_ADD_INTERFACE ? ((const struct add_if_req *)cmd)->interface_type : 0;

    enum mmwlan_status st = __real_mmint_morse_cmd_tx(driverd, resp, cmd, resp_maxlen, timeout_ms, skip_errlog);

    const struct cmd_resp *r = (const struct cmd_resp *)resp;
    if (id == CMD_ADD_INTERFACE)
    {
        ESP_LOGI(TAG, "chip command ADD_INTERFACE type %s: %s, chip status %ld, vif %u",
                 type_name(type), status_name(st),
                 r != NULL ? (long)(int32_t)r->status : 0L, r != NULL ? r->hdr.vif_id : 0);
    }
    else if (st != MMWLAN_SUCCESS)
    {
        ESP_LOGW(TAG, "chip command 0x%04x on vif %u: %s, chip status %ld", id, vif, status_name(st),
                 r != NULL ? (long)(int32_t)r->status : 0L);
    }
    return st;
}

/* Send our own ADD_INTERFACE with any type the chip knows. */
static enum mmwlan_status add_interface_raw(uint32_t type, const uint8_t *addr, uint16_t *vif_out, int32_t *chip_status)
{
    struct add_if_req req = { 0 };
    req.hdr.message_id = CMD_ADD_INTERFACE;
    req.hdr.len = sizeof(req) - sizeof(req.hdr);
    req.hdr.vif_id = VIF_ID_INVALID;
    memcpy(req.addr, addr, sizeof(req.addr));
    req.interface_type = type;

    struct cmd_resp resp = { 0 };
    enum mmwlan_status st = __real_mmint_morse_cmd_tx(s_driverd, (struct morse_cmd_resp *)&resp,
                                                      (struct morse_cmd_req *)&req, sizeof(resp), 3000, false);
    if (chip_status != NULL)
    {
        *chip_status = (int32_t)resp.status;
    }
    if (vif_out != NULL)
    {
        *vif_out = resp.hdr.vif_id;
    }
    return st;
}

/* Send any command to the chip: a header plus a body, on a given interface. */
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

static void mac_str(const uint8_t *m, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ---------- Experiment 1: which interface types does the chip accept? ---------- */

struct trial
{
    uint32_t type;
    enum mmwlan_status add;
    int32_t chip_status;
    uint16_t vif_id;
    enum mmwlan_status remove;
};

static void run_trial(struct trial *t, const uint8_t *base_mac, uint8_t salt)
{
    uint8_t addr[6];
    memcpy(addr, base_mac, sizeof(addr));
    addr[0] |= 0x02;   /* locally administered, so it never collides with the chip's own */
    addr[5] += salt;

    ESP_LOGI(TAG, "---- asking the chip for a %s interface (type %lu) ----", type_name(t->type), (unsigned long)t->type);
    t->add = add_interface_raw(t->type, addr, &t->vif_id, &t->chip_status);
    ESP_LOGI(TAG, "%s: %s, chip status %ld, vif %u", type_name(t->type), status_name(t->add),
             (long)t->chip_status, t->vif_id);

    t->remove = MMWLAN_SUCCESS;
    if (t->add == MMWLAN_SUCCESS)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        t->remove = mmint_mmdrv_rm_if(t->vif_id);
        ESP_LOGI(TAG, "%s: removed again: %s", type_name(t->type), status_name(t->remove));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}

static void experiment_interfaces(const struct mmwlan_version *v)
{
    struct trial trials[] = {
        { .type = IF_TYPE_AP },
        { .type = IF_TYPE_MESH },
        { .type = IF_TYPE_ADHOC },
        { .type = IF_TYPE_MON },
        { .type = IF_TYPE_AP },
    };
    const size_t n = sizeof(trials) / sizeof(trials[0]);
    for (size_t i = 0; i < n; i++)
    {
        run_trial(&trials[i], s_my_mac, (uint8_t)(i + 1));
    }

    printf("\n==== mesh-interface probe result ====\n");
    printf("chip firmware %s, morselib %s\n", v->morse_fw_version, v->morselib_version);
    for (size_t i = 0; i < n; i++)
    {
        printf("  %-6s add: %-45s chip status %ld, vif %u%s\n", type_name(trials[i].type),
               status_name(trials[i].add), (long)trials[i].chip_status, trials[i].vif_id,
               trials[i].add == MMWLAN_SUCCESS
                   ? (trials[i].remove == MMWLAN_SUCCESS ? ", removed OK" : ", REMOVE FAILED")
                   : "");
    }
    printf("=====================================\n\n");
}

/* ---------- Experiment 2: beacon on a mesh interface, listen on the other board ---------- */

static volatile bool s_force_mesh_vif;    /* beaconer: turn morselib's AP interface into a MESH one */
static volatile bool s_patch_mesh_id;     /* beaconer: rewrite the SSID element into a Mesh ID element */
static volatile uint32_t s_beacons_built;
static volatile uint32_t s_beacons_patched;
static volatile bool s_dump_next_beacon;
static uint16_t s_mesh_vif = VIF_ID_INVALID;

/* Morselib asks the chip for its AP interface through here. */
enum mmwlan_status __wrap_mmint_mmdrv_add_if(uint16_t *vif_id, const uint8_t *addr, int type)
{
    if (s_force_mesh_vif && type == IF_TYPE_AP)
    {
        int32_t chip_status = 0;
        enum mmwlan_status st = add_interface_raw(IF_TYPE_MESH, addr, vif_id, &chip_status);
        ESP_LOGI(TAG, "morselib wanted an AP interface; gave it a MESH one instead: %s, vif %u",
                 status_name(st), *vif_id);
        s_mesh_vif = *vif_id;
        return st;
    }
    return __real_mmint_mmdrv_add_if(vif_id, addr, type);
}

/* The chip raises a beacon request every beacon interval; morselib builds the beacon here. */
struct mmpkt *__wrap_mmint_umac_ap_get_beacon(struct umac_data *umacd)
{
    struct mmpkt *beacon = __real_mmint_umac_ap_get_beacon(umacd);
    if (beacon == NULL)
    {
        return NULL;
    }
    s_beacons_built++;

    struct mmpktview *view = mmpkt_open(beacon);
    uint8_t *data = mmpkt_get_data_start(view);
    uint32_t len = mmpkt_get_data_length(view);

    if (s_patch_mesh_id)
    {
        /* The SSID element: id 0, length, text. Same length as a Mesh ID element, so only
         * the id byte changes and the frame keeps its size. */
        const size_t ssid_len = strlen(MESH_SSID);
        for (uint32_t i = 0; i + 2 + ssid_len <= len; i++)
        {
            if (data[i] == IE_SSID && data[i + 1] == ssid_len && memcmp(&data[i + 2], MESH_SSID, ssid_len) == 0)
            {
                data[i] = IE_MESH_ID;
                s_beacons_patched++;
                break;
            }
        }
    }

    if (s_dump_next_beacon)
    {
        s_dump_next_beacon = false;
        ESP_LOGI(TAG, "beacon #%lu as handed to the chip (%lu bytes):", (unsigned long)s_beacons_built, (unsigned long)len);
        ESP_LOG_BUFFER_HEX(TAG, data, len > 128 ? 128 : len);
    }
    mmpkt_close(&view);
    return beacon;
}

/* Walk information elements, describing the ones that matter. */
static void describe_ies(const uint8_t *ies, uint32_t len, char *out, size_t out_len)
{
    size_t used = 0;
    uint32_t i = 0;
    while (i + 2 <= len && used < out_len - 1)
    {
        uint8_t id = ies[i], l = ies[i + 1];
        if (i + 2 + l > len)
        {
            break;
        }
        int n;
        if ((id == IE_SSID || id == IE_MESH_ID) && l > 0 && l <= 32)
        {
            n = snprintf(out + used, out_len - used, "%s%u(%u)'%.*s'", used ? " " : "", id, l, l, (const char *)&ies[i + 2]);
        }
        else
        {
            n = snprintf(out + used, out_len - used, "%s%u(%u)", used ? " " : "", id, l);
        }
        if (n < 0)
        {
            break;
        }
        used += (size_t)n;
        i += 2 + l;
    }
}

static volatile uint32_t s_raw_frames, s_raw_ours, s_raw_mesh_id;

/* Same module vendor prefix as this board, not this board itself; ignore the local-admin bit. */
static bool is_other_board(const uint8_t *addr)
{
    return (addr[0] & ~0x02) == (s_my_mac[0] & ~0x02) && memcmp(&addr[1], &s_my_mac[1], 3) == 0 &&
           memcmp(&addr[1], &s_my_mac[1], 5) != 0;
}

#define MAX_PEERS 4
struct peer
{
    uint8_t addr[6];
    uint32_t beacons;
    int16_t rssi;
    int64_t last_seen_us;
    uint32_t opens_sent, opens_ok, opens_rx;
    uint32_t data_sent, data_ok, data_rx;
};
static volatile uint32_t s_rx_data_4addr;
static struct peer s_peers[MAX_PEERS];
static int s_num_peers;
static volatile uint32_t s_rx_action, s_rx_peering_open;

static struct peer *peer_by_addr(const uint8_t *addr, bool create)
{
    for (int i = 0; i < s_num_peers; i++)
    {
        if (memcmp(s_peers[i].addr, addr, 6) == 0)
        {
            return &s_peers[i];
        }
    }
    if (!create || s_num_peers >= MAX_PEERS)
    {
        return NULL;
    }
    struct peer *p = &s_peers[s_num_peers++];
    memset(p, 0, sizeof(*p));
    memcpy(p->addr, addr, 6);
    char a[18];
    mac_str(addr, a);
    ESP_LOGI(TAG, "new mesh neighbour %s (heard its Mesh ID beacon)", a);
    return p;
}

/* Every frame the chip hands the host, scan or not, lands here first. Count them by kind and
 * log the S1G beacons from the other board, which morselib's scan code never reports. */
void __real_mmint_mmdrv_host_process_rx_frame(struct mmpkt *rxbuf, uint16_t channel);
static volatile uint32_t s_rx_total, s_rx_s1g_beacons, s_rx_s1g_beacons_ours, s_rx_mesh_id_beacons, s_rx_data, s_rx_mgmt;

void __wrap_mmint_mmdrv_host_process_rx_frame(struct mmpkt *rxbuf, uint16_t channel)
{
    struct mmpktview *view = mmpkt_open(rxbuf);
    const uint8_t *data = mmpkt_get_data_start(view);
    uint32_t len = mmpkt_get_data_length(view);
    s_rx_total++;
    if (len >= 16)
    {
        uint16_t fc = data[0] | (data[1] << 8);
        unsigned ftype = (fc >> 2) & 3, subtype = (fc >> 4) & 0xF;
        if (ftype == 3 && subtype == 1)
        {
            s_rx_s1g_beacons++;
            const uint8_t *src = &data[4];
            bool ours = is_other_board(src);
            uint32_t ies_at = 15 + ((fc & 0x0100) ? 3 : 0) + ((fc & 0x0200) ? 4 : 0) + ((fc & 0x0400) ? 1 : 0);
            char ies[200] = "";
            if (ies_at < len)
            {
                describe_ies(&data[ies_at], len - ies_at, ies, sizeof(ies));
            }
            bool mesh_id = strstr(ies, " 114(") != NULL || strncmp(ies, "114(", 4) == 0;
            if (ours)
            {
                s_rx_s1g_beacons_ours++;
                if (mesh_id)
                {
                    s_rx_mesh_id_beacons++;
                }
            }
            if (mesh_id && strstr(ies, "'" MESH_SSID "'") != NULL)
            {
                struct peer *p = peer_by_addr(src, true);
                if (p != NULL)
                {
                    p->beacons++;
                    p->rssi = ((struct probe_rx_meta *)mmpkt_get_metadata(rxbuf).opaque)->rssi;
                    p->last_seen_us = esp_timer_get_time();
                }
            }
            uint32_t n = ours ? s_rx_s1g_beacons_ours : s_rx_s1g_beacons;
            if (n <= 5 || n % 50 == 0)
            {
                char from[18];
                mac_str(src, from);
                ESP_LOGI(TAG, "RX S1G BEACON #%lu from %s%s, %lu bytes, fc 0x%04x: %s", (unsigned long)n, from,
                         ours ? " (OUR OTHER BOARD)" : "", (unsigned long)len, fc, ies);
            }
        }
        else if (ftype == 2)
        {
            s_rx_data++;
            bool to_ds = data[1] & 0x01, from_ds = data[1] & 0x02;
            if (to_ds && from_ds && len >= 32)
            {
                s_rx_data_4addr++;
                bool qos = subtype & 0x08;
                uint32_t body_at = 30 + (qos ? 2 : 0);
                bool mesh_ctrl = qos && (data[31] & 0x01);
                uint32_t payload_at = body_at + (mesh_ctrl ? 6 : 0) + 8;   /* mesh control, LLC/SNAP */
                char a1[18], a2[18], a3[18], a4[18];
                mac_str(&data[4], a1); mac_str(&data[10], a2); mac_str(&data[16], a3); mac_str(&data[24], a4);
                struct peer *p = peer_by_addr(&data[10], false);
                uint32_t n = s_rx_data_4addr;
                if (p != NULL)
                {
                    p->data_rx++;
                    n = p->data_rx;
                }
                if (n <= 5 || n % 20 == 0)
                {
                    char text[48] = "";
                    if (payload_at < len)
                    {
                        uint32_t t = len - payload_at;
                        if (t > sizeof(text) - 1) t = sizeof(text) - 1;
                        memcpy(text, &data[payload_at], t);
                        text[t] = '\0';
                        for (uint32_t i = 0; i < t; i++) if (text[i] < 32 || text[i] > 126) text[i] = '.';
                    }
                    ESP_LOGI(TAG, "RX 4-ADDRESS DATA #%lu: RA %s TA %s DA %s SA %s, %lu bytes, %s%s, ttl %u, rssi %d: \"%s\"",
                             (unsigned long)n, a1, a2, a3, a4, (unsigned long)len, qos ? "QoS" : "non-QoS",
                             mesh_ctrl ? " + Mesh Control" : "", mesh_ctrl ? data[body_at + 1] : 0,
                             ((struct probe_rx_meta *)mmpkt_get_metadata(rxbuf).opaque)->rssi, text);
                }
            }
        }
        else if (ftype == 0)
        {
            s_rx_mgmt++;
            if (subtype == 13 && len >= 26)
            {
                s_rx_action++;
                uint8_t category = data[24], action = data[25];
                char from[18], to[18];
                mac_str(&data[10], from);
                mac_str(&data[4], to);
                char ies[200] = "";
                if (category == 15 && len > 28)
                {
                    describe_ies(&data[28], len - 28, ies, sizeof(ies));
                }
                bool open = category == 15 && action == 1;
                if (open)
                {
                    s_rx_peering_open++;
                    struct peer *p = peer_by_addr(&data[10], false);
                    if (p != NULL)
                    {
                        p->opens_rx++;
                    }
                }
                ESP_LOGI(TAG, "RX ACTION frame from %s to %s: category %u action %u%s, %lu bytes, rssi %d%s%s", from, to,
                         category, action, open ? " = MESH PEERING OPEN" : "", (unsigned long)len,
                         ((struct probe_rx_meta *)mmpkt_get_metadata(rxbuf).opaque)->rssi, ies[0] ? ": " : "", ies);
            }
        }
    }
    mmpkt_close(&view);
    __real_mmint_mmdrv_host_process_rx_frame(rxbuf, channel);
}

/* Listener: every frame the chip hands up during a scan lands here, before morselib's parser. */
void __wrap_mmint_umac_scan_process_probe_resp(struct umac_data *umacd, struct mmpktview *rxbufview)
{
    const uint8_t *data = mmpkt_get_data_start(rxbufview);
    uint32_t len = mmpkt_get_data_length(rxbufview);
    s_raw_frames++;

    if (len >= 16)
    {
        uint16_t fc = data[0] | (data[1] << 8);
        unsigned ftype = (fc >> 2) & 3, subtype = (fc >> 4) & 0xF;
        const uint8_t *src;
        uint32_t ies_at;
        const char *kind;
        if (ftype == 3 && subtype == 1)
        {
            /* S1G beacon: fc, duration, SA, timestamp(4), change seq(1), then optional
             * next TBTT(3), compressed SSID(4), ANO(1) flagged in the frame control. */
            kind = "S1G beacon";
            src = &data[4];
            ies_at = 15 + ((fc & 0x0100) ? 3 : 0) + ((fc & 0x0200) ? 4 : 0) + ((fc & 0x0400) ? 1 : 0);
        }
        else
        {
            /* Legacy management frame: 24-byte header, timestamp(8), interval(2), capability(2). */
            kind = ftype == 0 && subtype == 5 ? "probe response" : ftype == 0 && subtype == 8 ? "beacon" : "mgmt";
            src = &data[10];
            ies_at = 36;
        }
        char from[18];
        mac_str(src, from);
        bool ours = is_other_board(src);
        if (ours)
        {
            s_raw_ours++;
        }
        char ies[200] = "";
        if (ies_at < len)
        {
            describe_ies(&data[ies_at], len - ies_at, ies, sizeof(ies));
        }
        if (strstr(ies, " 114(") != NULL || strncmp(ies, "114(", 4) == 0)
        {
            s_raw_mesh_id++;
        }
        ESP_LOGI(TAG, "rx %s from %s%s, %lu bytes, fc 0x%04x: %s", kind, from, ours ? " (OUR OTHER BOARD)" : "",
                 (unsigned long)len, fc, ies);
    }
    __real_mmint_umac_scan_process_probe_resp(umacd, rxbufview);
}

/* A flag that survives a chip-crash reboot: set before a risky command, cleared after. */
static void guard_set(bool armed)
{
    nvs_handle_t nvs;
    if (nvs_open("probe", NVS_READWRITE, &nvs) == ESP_OK)
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

static bool guard_was_armed(void)
{
    nvs_handle_t nvs;
    uint8_t v = 0;
    if (nvs_open("probe", NVS_READONLY, &nvs) == ESP_OK)
    {
        nvs_get_u8(nvs, "armed", &v);
        nvs_close(nvs);
    }
    return v != 0;
}

static void beaconer_run(void)
{
    mmwlan_override_max_tx_power(TX_CAP_DBM);
    bool crashed_last_time = guard_was_armed();
    (void)crashed_last_time;
    guard_set(false);

    struct mmwlan_ap_args args = MMWLAN_AP_ARGS_INIT;
    args.ssid_len = strlen(MESH_SSID);
    memcpy(args.ssid, MESH_SSID, args.ssid_len);
    args.security_type = MMWLAN_OPEN;
    args.pmf_mode = MMWLAN_PMF_DISABLED;
    args.op_class = S1G_OPCLASS;
    args.s1g_chan_num = S1G_CHANNEL;
    args.max_stas = 4;

    /* Control: the same AP on a real AP interface, to see how often the chip asks for beacons. */
    s_force_mesh_vif = false;
    ESP_LOGI(TAG, "==== CONTROL: the AP code on a real AP interface for %d s ====", CONTROL_S);
    enum mmwlan_status st = mmwlan_ap_enable(&args);
    ESP_LOGI(TAG, "mmwlan_ap_enable (AP interface): %s", status_name(st));
    for (int s = 0; s < CONTROL_S; s += 10)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "control: %lu beacons built so far", (unsigned long)s_beacons_built);
    }
    uint32_t control_beacons = s_beacons_built;
    st = mmwlan_ap_disable();
    ESP_LOGI(TAG, "mmwlan_ap_disable: %s", status_name(st));
    vTaskDelay(pdMS_TO_TICKS(3000));
    s_beacons_built = 0;

    s_force_mesh_vif = true;
    s_dump_next_beacon = true;
    ESP_LOGI(TAG, "==== PHASE A: starting the AP code on a MESH interface, SSID %s, channel %d ====", MESH_SSID, S1G_CHANNEL);
    st = mmwlan_ap_enable(&args);
    ESP_LOGI(TAG, "mmwlan_ap_enable: %s", status_name(st));
    if (st != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "the chip would not beacon on a mesh interface; see the command log above");
        return;
    }

    for (int s = 0; s < PHASE_A_S; s += 10)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "phase A: %lu beacons built so far", (unsigned long)s_beacons_built);
    }

    uint32_t phase_a_beacons = s_beacons_built;

    /* Phase D: the Linux driver's beacon timer enable. */
    struct bss_beacon_config_req bc = { 0 };
    bc.enable = 1;
    int32_t chip_status_d = 0;
    ESP_LOGI(TAG, "==== PHASE D: BSS_BEACON_CONFIG enable, vif %u ====", s_mesh_vif);
    enum mmwlan_status st_d = send_raw(CMD_BSS_BEACON_CONFIG, s_mesh_vif, &bc, sizeof(bc), &chip_status_d);
    ESP_LOGI(TAG, "BSS_BEACON_CONFIG: %s, chip status %ld", status_name(st_d), (long)chip_status_d);
    s_dump_next_beacon = true;
    for (int s = 0; s < PHASE_D_S; s += 10)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "phase D: %lu beacons built so far", (unsigned long)s_beacons_built);
    }
    uint32_t phase_d_beacons = s_beacons_built;

#if TRY_MESH_CONFIG
    /* Phase C: what the Linux driver sends for a mesh interface: MESH_CONFIG start with beaconing. */
    struct mesh_config_req mc = { 0 };
    mc.mesh_cfg_opcode = MESH_CONFIG_OPCODE_START;
    mc.enable_beaconing = 1;
    mc.mbca_config = MESH_MBCA_CONFIG;
    mc.min_beacon_gap_ms = MESH_MIN_BEACON_GAP_MS;
    mc.tbtt_adj_timer_interval_ms = MESH_TBTT_ADJ_MS;
    mc.mbss_start_scan_duration_ms = MESH_SCAN_MS;
    int32_t chip_status = 0;
    enum mmwlan_status st_c = MMWLAN_UNAVAILABLE;
    if (crashed_last_time)
    {
        ESP_LOGW(TAG, "==== PHASE C skipped: MESH_CONFIG crashed the chip on the previous boot ====");
    }
    else
    {
        ESP_LOGI(TAG, "==== PHASE C: MESH_CONFIG start, beaconing on, vif %u ====", s_mesh_vif);
        guard_set(true);
        st_c = send_raw(CMD_MESH_CONFIG, s_mesh_vif, &mc, sizeof(mc), &chip_status);
        vTaskDelay(pdMS_TO_TICKS(2000));
        guard_set(false);
        ESP_LOGI(TAG, "MESH_CONFIG: %s, chip status %ld", status_name(st_c), (long)chip_status);
    }
    s_dump_next_beacon = true;
    for (int s = 0; s < PHASE_C_S; s += 10)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "phase C: %lu beacons built so far", (unsigned long)s_beacons_built);
    }
    uint32_t phase_c_beacons = s_beacons_built;

#else
    enum mmwlan_status st_c = MMWLAN_UNAVAILABLE;
    int32_t chip_status = 0;
    uint32_t phase_c_beacons = s_beacons_built;
#endif

    s_patch_mesh_id = true;
    s_dump_next_beacon = true;
    ESP_LOGI(TAG, "==== PHASE B: rewriting the SSID element into a Mesh ID element (114) ====");
    for (int s = 0; s < PHASE_B_S; s += 10)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "phase B: %lu beacons built, %lu patched", (unsigned long)s_beacons_built, (unsigned long)s_beacons_patched);
    }

    printf("\n==== mesh-beacon probe (beaconer) ====\n");
    printf("control, real AP interface: %lu beacons asked for in %d s\n", (unsigned long)control_beacons, CONTROL_S);
    printf("AP code on a MESH interface: %s\n", status_name(st));
    printf("A  mesh interface, AP commands only:        %lu beacons in %d s\n", (unsigned long)phase_a_beacons, PHASE_A_S);
    printf("D  + BSS_BEACON_CONFIG enable (%s, chip %ld): %lu beacons in %d s\n", status_name(st_d), (long)chip_status_d,
           (unsigned long)(phase_d_beacons - phase_a_beacons), PHASE_D_S);
    printf("C  + MESH_CONFIG start/beaconing (%s, chip %ld): %lu beacons in %d s%s\n", status_name(st_c), (long)chip_status,
           (unsigned long)(phase_c_beacons - phase_d_beacons), PHASE_C_S, TRY_MESH_CONFIG ? "" : " (not tried)");
    printf("B  SSID element rewritten to Mesh ID:        %lu beacons in %d s, %lu patched\n",
           (unsigned long)(s_beacons_built - phase_c_beacons), PHASE_B_S, (unsigned long)s_beacons_patched);
    printf("======================================\n\n");
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "still beaconing with Mesh ID: %lu beacons built", (unsigned long)s_beacons_built);
    }
}

static SemaphoreHandle_t s_scan_done;
static volatile uint32_t s_scan_results;

static void scan_rx_cb(const struct mmwlan_scan_result *result, void *arg)
{
    (void)arg;
    s_scan_results++;
    char from[18];
    mac_str(result->bssid, from);
    ESP_LOGI(TAG, "  parsed result: %s ssid '%.*s' rssi %d, %u MHz, cap 0x%04x", from, result->ssid_len,
             (const char *)result->ssid, result->rssi, result->bw_mhz, result->capability_info);
}

static void scan_done_cb(enum mmwlan_scan_state state, void *arg)
{
    (void)arg;
    (void)state;
    xSemaphoreGive(s_scan_done);
}

static void listener_run(void)
{
    mmwlan_override_max_tx_power(TX_CAP_DBM);
    s_scan_done = xSemaphoreCreateBinary();
    ESP_LOGI(TAG, "==== LISTENER: scanning channel %d over and over; raw frames are logged before the parser ====", S1G_CHANNEL);
    for (uint32_t n = 1;; n++)
    {
        uint32_t raw_before = s_raw_frames, ours_before = s_raw_ours, mesh_before = s_raw_mesh_id;
        s_scan_results = 0;
        struct mmhalow_scan_args args = { .rx_cb = scan_rx_cb, .complete_cb = scan_done_cb, .cb_arg = NULL };
        if (mmhalow_scan(&args) != ESP_OK)
        {
            ESP_LOGW(TAG, "scan %lu: request failed", (unsigned long)n);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(15000)) != pdTRUE)
        {
            ESP_LOGW(TAG, "scan %lu: no completion in 15 s", (unsigned long)n);
        }
        ESP_LOGI(TAG, "scan %lu done: %lu probe responses (%lu from our other board, %lu with Mesh ID) | all RX so far: %lu frames, "
                 "%lu S1G beacons, %lu from our other board, %lu of those with Mesh ID",
                 (unsigned long)n, (unsigned long)(s_raw_frames - raw_before), (unsigned long)(s_raw_ours - ours_before),
                 (unsigned long)(s_raw_mesh_id - mesh_before), (unsigned long)s_rx_total, (unsigned long)s_rx_s1g_beacons,
                 (unsigned long)s_rx_s1g_beacons_ours, (unsigned long)s_rx_mesh_id_beacons);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------- Experiment 3: mesh node with peering-open frames ---------- */

struct open_frame_params
{
    uint8_t to[6];
    uint8_t from[6];
    uint16_t link_id;
};

/* Builds a Mesh Peering Open action frame (802.11-2020 9.6.16.2) into morselib's buffer. */
static void build_peering_open(struct umac_data *umacd, struct consbuf *buf, void *params)
{
    (void)umacd;
    const struct open_frame_params *f = (const struct open_frame_params *)params;
    uint8_t hdr[24] = { 0 };
    hdr[0] = 0xd0;                                    /* management, action */
    memcpy(&hdr[4], f->to, 6);                        /* address 1: the neighbour */
    memcpy(&hdr[10], f->from, 6);                     /* address 2: us */
    memcpy(&hdr[16], f->from, 6);                     /* address 3: us (mesh peering frames) */
    mmint_consbuf_append(buf, hdr, sizeof(hdr));

    const uint8_t fixed[] = { 15, 1, 0x00, 0x00 };    /* self-protected, peering open, capability */
    mmint_consbuf_append(buf, fixed, sizeof(fixed));

    uint8_t mesh_id[2 + 32];
    mesh_id[0] = IE_MESH_ID;
    mesh_id[1] = strlen(MESH_SSID);
    memcpy(&mesh_id[2], MESH_SSID, mesh_id[1]);
    mmint_consbuf_append(buf, mesh_id, 2 + mesh_id[1]);

    /* Mesh Configuration: HWMP, airtime metric, no congestion control, neighbour offset sync,
     * no authentication, formation info 0, capability: accepting peerings. */
    const uint8_t mesh_conf[] = { IE_MESH_CONFIG, 7, 1, 1, 0, 1, 0, 0, 0x01 };
    mmint_consbuf_append(buf, mesh_conf, sizeof(mesh_conf));

    /* Mesh Peering Management: protocol 0 (no AMPE), our local link id. */
    const uint8_t mpm[] = { 117, 4, 0, 0, (uint8_t)(f->link_id & 0xff), (uint8_t)(f->link_id >> 8) };
    mmint_consbuf_append(buf, mpm, sizeof(mpm));
}

struct data_frame_params
{
    uint8_t to[6];
    uint8_t from[6];
    uint32_t mesh_seq;
    char text[48];
};

/* Builds an 802.11s mesh data frame: QoS data, To DS + From DS (four addresses), Mesh
 * Control in the body, LLC/SNAP, then a text payload. */
static void build_mesh_data(struct umac_data *umacd, struct consbuf *buf, void *params)
{
    (void)umacd;
    const struct data_frame_params *f = (const struct data_frame_params *)params;
    uint8_t hdr[32] = { 0 };
    hdr[0] = 0x88;                      /* data, QoS data */
    hdr[1] = 0x03;                      /* To DS, From DS: four addresses */
    memcpy(&hdr[4], f->to, 6);          /* address 1: receiver (next hop) */
    memcpy(&hdr[10], f->from, 6);       /* address 2: transmitter */
    memcpy(&hdr[16], f->to, 6);         /* address 3: destination */
    memcpy(&hdr[24], f->from, 6);       /* address 4: source */
    hdr[30] = 0x00;                     /* QoS control: TID 0 */
    hdr[31] = 0x01;                     /* bit 8: Mesh Control present */
    mmint_consbuf_append(buf, hdr, sizeof(hdr));

    uint8_t mesh_ctrl[6] = { 0x00, 31, (uint8_t)f->mesh_seq, (uint8_t)(f->mesh_seq >> 8),
                             (uint8_t)(f->mesh_seq >> 16), (uint8_t)(f->mesh_seq >> 24) };   /* flags, TTL, sequence */
    mmint_consbuf_append(buf, mesh_ctrl, sizeof(mesh_ctrl));

    const uint8_t llc[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0xb5 };   /* SNAP, experimental ethertype */
    mmint_consbuf_append(buf, llc, sizeof(llc));
    mmint_consbuf_append(buf, (const uint8_t *)f->text, strlen(f->text));
}

static void mesh_node_run(void)
{
    mmwlan_override_max_tx_power(TX_CAP_DBM);
    bool crashed_last_time = guard_was_armed();
    guard_set(false);

    struct mmwlan_ap_args args = MMWLAN_AP_ARGS_INIT;
    args.ssid_len = strlen(MESH_SSID);
    memcpy(args.ssid, MESH_SSID, args.ssid_len);
    args.security_type = MMWLAN_OPEN;
    args.pmf_mode = MMWLAN_PMF_DISABLED;
    args.op_class = S1G_OPCLASS;
    args.s1g_chan_num = S1G_CHANNEL;
    args.max_stas = 4;

    s_force_mesh_vif = true;
    ESP_LOGI(TAG, "==== MESH NODE: mesh interface, Mesh ID %s, channel %d ====", MESH_SSID, S1G_CHANNEL);
    enum mmwlan_status st = mmwlan_ap_enable(&args);
    ESP_LOGI(TAG, "mmwlan_ap_enable on the mesh interface: %s", status_name(st));
    if (st != MMWLAN_SUCCESS)
    {
        return;
    }

    struct mesh_config_req mc = { 0 };
    mc.mesh_cfg_opcode = MESH_CONFIG_OPCODE_START;
    mc.enable_beaconing = 1;
    mc.mbca_config = MESH_MBCA_CONFIG;
    mc.min_beacon_gap_ms = MESH_MIN_BEACON_GAP_MS;
    mc.tbtt_adj_timer_interval_ms = MESH_TBTT_ADJ_MS;
    mc.mbss_start_scan_duration_ms = MESH_SCAN_MS;
    int32_t chip_status = 0;
    if (crashed_last_time)
    {
        ESP_LOGW(TAG, "MESH_CONFIG skipped: it crashed the chip on the previous boot");
    }
    else
    {
        guard_set(true);
        st = send_raw(CMD_MESH_CONFIG, s_mesh_vif, &mc, sizeof(mc), &chip_status);
        vTaskDelay(pdMS_TO_TICKS(2000));
        guard_set(false);
        ESP_LOGI(TAG, "MESH_CONFIG start, beaconing on: %s, chip status %ld", status_name(st), (long)chip_status);
    }
    s_patch_mesh_id = true;

    uint8_t me[6] = { 0 };
    mmwlan_ap_get_bssid(me);
    char me_s[18];
    mac_str(me, me_s);
    ESP_LOGI(TAG, "our mesh address is %s; listening for neighbours' Mesh ID beacons", me_s);

    struct umac_data *umacd = mmint_umac_data_get_umacd();
    uint16_t link_id = (uint16_t)(esp_random() & 0xffff);
    uint32_t tx_tried = 0, tx_ok = 0;
    for (uint32_t tick = 1;; tick++)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        for (int i = 0; i < s_num_peers; i++)
        {
            struct peer *p = &s_peers[i];
            struct data_frame_params d = { .mesh_seq = tick };
            memcpy(d.to, p->addr, 6);
            memcpy(d.from, me, 6);
            snprintf(d.text, sizeof(d.text), "mesh data #%lu from %s", (unsigned long)(p->data_sent + 1), me_s);
            struct mmpkt *dp = mmint_build_mgmt_frame(umacd, build_mesh_data, &d);
            if (dp != NULL)
            {
                p->data_sent++;
                s_data_pkt_in_flight = dp;
                enum mmwlan_status ds = mmint_umac_datapath_tx_mgmt_frame_ap(umacd, dp, NULL);
                s_data_pkt_in_flight = NULL;
                if (ds == MMWLAN_SUCCESS)
                {
                    p->data_ok++;
                }
                if (p->data_sent <= 3 || p->data_sent % 15 == 0)
                {
                    char a[18];
                    mac_str(p->addr, a);
                    ESP_LOGI(TAG, "TX 4-address data #%lu to %s: %s", (unsigned long)p->data_sent, a, status_name(ds));
                }
            }
            if (tick % 5 != 0)
            {
                continue;
            }
            struct open_frame_params f = { .link_id = link_id };
            memcpy(f.to, p->addr, 6);
            memcpy(f.from, me, 6);
            struct mmpkt *pkt = mmint_build_mgmt_frame(umacd, build_peering_open, &f);
            if (pkt == NULL)
            {
                ESP_LOGW(TAG, "could not build the peering open frame");
                continue;
            }
            tx_tried++;
            p->opens_sent++;
            enum mmwlan_status ts = mmint_umac_datapath_tx_mgmt_frame_ap(umacd, pkt, NULL);
            if (ts == MMWLAN_SUCCESS)
            {
                tx_ok++;
                p->opens_ok++;
            }
            char a[18];
            mac_str(p->addr, a);
            if (p->opens_sent <= 3 || p->opens_sent % 15 == 0)
            {
                ESP_LOGI(TAG, "TX mesh peering open #%lu to %s: %s", (unsigned long)p->opens_sent, a, status_name(ts));
            }
        }
        if (tick % 5 == 0)
        {
            ESP_LOGI(TAG, "---- %lu s: %lu beacons built | rx: %lu S1G beacons, %lu action frames (%lu peering opens), %lu data frames (%lu four-address) | tx opens %lu, accepted %lu",
                     (unsigned long)(tick * 2), (unsigned long)s_beacons_built, (unsigned long)s_rx_s1g_beacons,
                     (unsigned long)s_rx_action, (unsigned long)s_rx_peering_open, (unsigned long)s_rx_data, (unsigned long)s_rx_data_4addr,
                     (unsigned long)tx_tried, (unsigned long)tx_ok);
            for (int i = 0; i < s_num_peers; i++)
            {
                struct peer *p = &s_peers[i];
                char a[18];
                mac_str(p->addr, a);
                ESP_LOGI(TAG, "     neighbour %s: %lu beacons, rssi %d, last %lld s ago | opens sent %lu ok %lu rx %lu | data sent %lu ok %lu rx %lu", a,
                         (unsigned long)p->beacons, p->rssi, (long long)((esp_timer_get_time() - p->last_seen_us) / 1000000),
                         (unsigned long)p->opens_sent, (unsigned long)p->opens_ok, (unsigned long)p->opens_rx,
                         (unsigned long)p->data_sent, (unsigned long)p->data_ok, (unsigned long)p->data_rx);
            }
        }
    }
}

/* ---------- Main ---------- */

static void probe_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(mmhalow_init(NULL));

    struct mmwlan_version v = { 0 };
    mmwlan_get_version(&v);
    ESP_LOGI(TAG, "morselib %s, chip firmware %s, chip %s", v.morselib_version, v.morse_fw_version, v.morse_chip_id_string);

    vTaskDelay(pdMS_TO_TICKS(1000));
    if (s_driverd == NULL)
    {
        ESP_LOGE(TAG, "never saw a chip command go by; the wrap isn't in the link");
        vTaskDelete(NULL);
        return;
    }

    mmwlan_get_mac_addr(s_my_mac);
    char mac[18];
    mac_str(s_my_mac, mac);
    ESP_LOGI(TAG, "module MAC %s", mac);

#if PROBE_EXPERIMENT == 1
    experiment_interfaces(&v);
#elif PROBE_EXPERIMENT == 3
    mesh_node_run();
#else
    if (s_beaconer)
    {
        beaconer_run();
    }
    else
    {
        listener_run();
    }
#endif
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "probe finished; idle");
    }
}

/* A warm restart leaves the HaLow chip and the GPIO interrupt settings as they were. Quiet them. */
static void halow_quiesce(void)
{
    gpio_reset_pin(CONFIG_MM_SPI_IRQ);
    gpio_reset_pin(CONFIG_MM_BUSY);
    gpio_config_t reset_pin = { .pin_bit_mask = 1ULL << CONFIG_MM_RESET_N, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&reset_pin);
    gpio_set_level(CONFIG_MM_RESET_N, 0);
}

void app_main(void)
{
    halow_quiesce();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    uint8_t efuse[6];
    esp_efuse_mac_get_default(efuse);
    uint16_t node = (efuse[4] << 8) | efuse[5];
    s_beaconer = node == BEACONER_NODE;
    ESP_LOGI(TAG, "mesh probe experiment %d starting on node %04X as %s (reset reason %d)", PROBE_EXPERIMENT, node,
             PROBE_EXPERIMENT == 1 ? "interface tester" : PROBE_EXPERIMENT == 3 ? "MESH NODE" : s_beaconer ? "BEACONER" : "LISTENER",
             (int)esp_reset_reason());
    xTaskCreate(probe_task, "probe", 8192, NULL, 5, NULL);
}
