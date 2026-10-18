/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HalowChat: a group chat over Wi-Fi HaLow.
 *
 * Every board runs this same firmware. At boot it scans for the HaLow network; if it finds
 * one it joins as a station, otherwise it starts the network as the access point. Each board
 * also runs a 2.4 GHz hotspot with a chat page for a phone. Messages are flooded over the
 * HaLow network as UDP broadcasts with a packet ID, and every board rebroadcasts each new
 * packet once, so a message reaches everyone whether or not the access point forwards
 * broadcasts between stations. Receivers broadcast an ack, which the page shows as "seen by".
 *
 * Built from the Ottercam bridge and base firmware (same hardware, same esp-halow workarounds).
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "hal/brownout_hal.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "lwip/sockets.h"

#include "mmhalow.h"

static const char *TAG = "halowchat";

#define HALOW_NETIF_KEY "WIFI_STA_DEF" /* esp-halow creates its netif as a Wi-Fi station */
#define NVS_NAMESPACE   "halowchat"
#define HALOW_SUBNET    0x0a2a0000U    /* 10.42.0.0/24, host byte order */
#define HOTSPOT_IP      "10.42.100.1"
#define HOTSPOT_MASK    "255.255.255.0"

#define MAX_SCAN_RESULTS 16
#define SCAN_TIMEOUT_MS  15000
#define SCAN_ATTEMPTS    3

#define MAX_MESSAGES   300
#define SAVED_MESSAGES 50
#define RETRY_MAX      3
#define MAX_TEXT       200
#define MAX_NAME       24
#define MAX_PEERS      16
#define MAX_SEEN_IDS   256
#define FLOOD_MAX_HOPS 4

#define RESET_HISTORY_LEN 10


#define CONNECTED_BIT BIT0

typedef enum
{
    ROLE_UNDECIDED,
    ROLE_STATION,
    ROLE_ACCESS_POINT,
} role_t;

typedef struct
{
    uint32_t serial;         /* local order, increases forever */
    char id[16];             /* "<node>-<seq>" */
    char node[5];            /* sender's node id */
    char from[MAX_NAME + 1]; /* sender's display name */
    char text[MAX_TEXT + 1];
    int64_t ts_ms;           /* sender's phone time (ms since 1970), 0 if unknown */
    uint8_t hops;
    uint8_t seen_count;      /* boards that received it */
    char seen[MAX_PEERS][5];
    uint8_t read_count;      /* people who had it shown on a page, by board */
    char read_by[MAX_PEERS][MAX_NAME + 1];
    char read_node[MAX_PEERS][5];
    bool read_ack_sent;      /* this board's page has shown it and said so */
    uint8_t retries;         /* our own messages: resends so far */
    int64_t sent_us;         /* our own messages: when last sent */
} message_t;

typedef struct
{
    bool used;
    char node[5];
    char name[MAX_NAME + 1];
    char ip[16];
    bool is_hub;
    int64_t last_seen_us;
} peer_t;

static char s_node[5];        /* this board's node id, four hex digits from its MAC */
static char s_name[MAX_NAME + 1];
static uint8_t s_tx_dbm = CONFIG_CHAT_HALOW_MAX_TX_DBM;
static uint8_t s_role_setting = CONFIG_CHAT_ROLE; /* 0 auto, 1 always hub, 2 always member */
static uint32_t s_boot_count;
static uint8_t s_reset_history[RESET_HISTORY_LEN];
static uint8_t s_reset_count;

static volatile role_t s_role = ROLE_UNDECIDED;
static EventGroupHandle_t s_events;
static esp_netif_t *s_halow_netif;
static esp_netif_t *s_ap_netif;
static uint32_t s_halow_ip; /* network byte order */
static volatile bool s_hotspot_paused;
static int64_t s_role_decided_us;

static message_t *s_messages; /* in PSRAM */
static size_t s_message_count;
static uint32_t s_next_serial = 1;
static uint32_t s_next_seq = 1;
static peer_t s_peers[MAX_PEERS];
static char s_seen_ids[MAX_SEEN_IDS][24];
static size_t s_seen_next;
static SemaphoreHandle_t s_lock;
static int s_sock = -1;

static const struct mmwlan_s1g_channel *s_channel;

/* Scan state (station role decision) */
static char s_scan_found_ssid[MMWLAN_SSID_MAXLEN + 1];
static bool s_scan_found;
static SemaphoreHandle_t s_scan_done;
static volatile enum mmwlan_scan_state s_scan_state;

extern const char chat_ui_html_start[] asm("_binary_chat_ui_html_start");
extern const char chat_ui_html_end[] asm("_binary_chat_ui_html_end");

/* ---------- esp-halow workarounds (see the Ottercam READMEs) ---------- */

esp_err_t __real_spi_bus_add_device(spi_host_device_t host,
                                    const spi_device_interface_config_t *dev_config,
                                    spi_device_handle_t *handle);

esp_err_t __wrap_spi_bus_add_device(spi_host_device_t host,
                                    const spi_device_interface_config_t *dev_config,
                                    spi_device_handle_t *handle)
{
    if (host == SPI2_HOST && dev_config->spics_io_num < 0 &&
        dev_config->clock_speed_hz > CONFIG_CHAT_HALOW_SPI_MAX_HZ)
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

/* esp-halow leaves the transmit interface unspecified; with both a station and an access
 * point interface present morselib drops the packet. Pick the one for our role. */
enum mmwlan_status __real_mmwlan_tx_pkt(struct mmpkt *pkt, const struct mmwlan_tx_metadata *metadata);

enum mmwlan_status __wrap_mmwlan_tx_pkt(struct mmpkt *pkt, const struct mmwlan_tx_metadata *metadata)
{
    struct mmwlan_tx_metadata md = MMWLAN_TX_METADATA_INIT;
    if (metadata != NULL)
    {
        md = *metadata;
    }
    if (md.vif == MMWLAN_VIF_UNSPECIFIED && s_role != ROLE_UNDECIDED)
    {
        md.vif = s_role == ROLE_ACCESS_POINT ? MMWLAN_VIF_AP : MMWLAN_VIF_STA;
    }
    return __real_mmwlan_tx_pkt(pkt, &md);
}

/* The whole network lives on one channel, so give morselib only the channels inside it
 * (2 MHz and up). Scans are then fast, and the first transmissions stay short, which weak
 * USB ports need. */
static struct mmwlan_s1g_channel s_chan_set[16];
static struct mmwlan_s1g_channel_list s_chan_list;

enum mmwlan_status __real_mmwlan_set_channel_list(const struct mmwlan_s1g_channel_list *list);

enum mmwlan_status __wrap_mmwlan_set_channel_list(const struct mmwlan_s1g_channel_list *list)
{
    const struct mmwlan_s1g_channel *anchor = NULL;
    for (unsigned i = 0; list != NULL && i < list->num_channels; i++)
    {
        const struct mmwlan_s1g_channel *ch = &list->channels[i];
        if (ch->s1g_chan_num == CONFIG_CHAT_S1G_CHANNEL &&
            (ch->global_operating_class == CONFIG_CHAT_S1G_OPCLASS ||
             ch->s1g_operating_class == CONFIG_CHAT_S1G_OPCLASS))
        {
            anchor = ch;
            break;
        }
    }
    if (anchor == NULL)
    {
        ESP_LOGE(TAG, "Channel %d/%d isn't in the %s list; using the full list",
                 CONFIG_CHAT_S1G_CHANNEL, CONFIG_CHAT_S1G_OPCLASS, CONFIG_HALOW_COUNTRY_CODE);
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

/* ---------- Settings and reset history ---------- */

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason)
    {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "crash";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB: return "USB";
        default: return "unknown";
    }
}

static void load_settings(void)
{
    snprintf(s_name, sizeof(s_name), "board %s", s_node);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
    {
        return;
    }
    size_t len = sizeof(s_name);
    nvs_get_str(nvs, "name", s_name, &len);
    uint8_t tx;
    if (nvs_get_u8(nvs, "tx", &tx) == ESP_OK && tx <= 30)
    {
        s_tx_dbm = tx;
    }
    uint8_t role;
    if (nvs_get_u8(nvs, "role", &role) == ESP_OK && role <= 2)
    {
        s_role_setting = role;
    }
    nvs_close(nvs);
}

static esp_err_t save_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_str(nvs, "name", s_name);
    if (err == ESP_OK)
    {
        err = nvs_set_u8(nvs, "tx", s_tx_dbm);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_u8(nvs, "role", s_role_setting);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void record_reset(esp_reset_reason_t reason)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        return;
    }
    size_t len = sizeof(s_reset_history);
    if (nvs_get_blob(nvs, "resets", s_reset_history, &len) != ESP_OK)
    {
        len = 0;
    }
    s_reset_count = len < RESET_HISTORY_LEN ? (uint8_t)len + 1 : RESET_HISTORY_LEN;
    memmove(&s_reset_history[1], &s_reset_history[0], (s_reset_count - 1) * sizeof(uint8_t));
    s_reset_history[0] = (uint8_t)reason;
    nvs_set_blob(nvs, "resets", s_reset_history, s_reset_count);
    if (nvs_get_u32(nvs, "boots", &s_boot_count) != ESP_OK)
    {
        s_boot_count = 0;
    }
    s_boot_count++;
    nvs_set_u32(nvs, "boots", s_boot_count);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ---------- Message store ---------- */

static bool seen_before(const char *pid)
{
    for (size_t i = 0; i < MAX_SEEN_IDS; i++)
    {
        if (s_seen_ids[i][0] != '\0' && strcmp(s_seen_ids[i], pid) == 0)
        {
            return true;
        }
    }
    strlcpy(s_seen_ids[s_seen_next], pid, sizeof(s_seen_ids[0]));
    s_seen_next = (s_seen_next + 1) % MAX_SEEN_IDS;
    return false;
}

static void note_peer_ip(const char *node, const char *ip, const char *role)
{
    for (size_t i = 0; i < MAX_PEERS; i++)
    {
        if (s_peers[i].used && strcmp(s_peers[i].node, node) == 0)
        {
            if (ip != NULL)
            {
                strlcpy(s_peers[i].ip, ip, sizeof(s_peers[i].ip));
            }
            if (role != NULL)
            {
                s_peers[i].is_hub = strcmp(role, "ap") == 0;
            }
            return;
        }
    }
}

static void note_peer(const char *node, const char *name)
{
    if (strcmp(node, s_node) == 0)
    {
        return;
    }
    peer_t *slot = NULL;
    for (size_t i = 0; i < MAX_PEERS; i++)
    {
        if (s_peers[i].used && strcmp(s_peers[i].node, node) == 0)
        {
            slot = &s_peers[i];
            break;
        }
        if (!s_peers[i].used && slot == NULL)
        {
            slot = &s_peers[i];
        }
    }
    if (slot == NULL)
    {
        return;
    }
    slot->used = true;
    strlcpy(slot->node, node, sizeof(slot->node));
    if (name != NULL && name[0] != '\0')
    {
        strlcpy(slot->name, name, sizeof(slot->name));
    }
    slot->last_seen_us = esp_timer_get_time();
}

static message_t *store_message(const char *id, const char *node, const char *from, const char *text,
                                int64_t ts_ms, uint8_t hops)
{
    message_t *m;
    if (s_message_count < MAX_MESSAGES)
    {
        m = &s_messages[s_message_count++];
    }
    else
    {
        memmove(&s_messages[0], &s_messages[1], (MAX_MESSAGES - 1) * sizeof(message_t));
        m = &s_messages[MAX_MESSAGES - 1];
    }
    memset(m, 0, sizeof(*m));
    m->serial = s_next_serial++;
    strlcpy(m->id, id, sizeof(m->id));
    strlcpy(m->node, node, sizeof(m->node));
    strlcpy(m->from, from, sizeof(m->from));
    strlcpy(m->text, text, sizeof(m->text));
    m->ts_ms = ts_ms;
    m->hops = hops;
    return m;
}

static void mark_seen(const char *id, const char *node)
{
    for (size_t i = 0; i < s_message_count; i++)
    {
        message_t *m = &s_messages[i];
        if (strcmp(m->id, id) != 0)
        {
            continue;
        }
        for (uint8_t j = 0; j < m->seen_count; j++)
        {
            if (strcmp(m->seen[j], node) == 0)
            {
                return;
            }
        }
        if (m->seen_count < MAX_PEERS)
        {
            strlcpy(m->seen[m->seen_count++], node, sizeof(m->seen[0]));
        }
        return;
    }
}

static void mark_read(const char *id, const char *node, const char *name)
{
    for (size_t i = 0; i < s_message_count; i++)
    {
        message_t *m = &s_messages[i];
        if (strcmp(m->id, id) != 0)
        {
            continue;
        }
        for (uint8_t j = 0; j < m->read_count; j++)
        {
            if (strcmp(m->read_node[j], node) == 0)
            {
                strlcpy(m->read_by[j], name, sizeof(m->read_by[j]));
                return;
            }
        }
        if (m->read_count < MAX_PEERS)
        {
            strlcpy(m->read_node[m->read_count], node, sizeof(m->read_node[0]));
            strlcpy(m->read_by[m->read_count], name, sizeof(m->read_by[0]));
            m->read_count++;
        }
        return;
    }
}

/* The last SAVED_MESSAGES messages are kept in NVS so a reboot doesn't wipe the chat. Call
 * with s_lock held. */
static void save_history(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        return;
    }
    size_t n = s_message_count < SAVED_MESSAGES ? s_message_count : SAVED_MESSAGES;
    const message_t *first = &s_messages[s_message_count - n];
    nvs_set_blob(nvs, "history", first, n * sizeof(message_t));
    nvs_set_u32(nvs, "seq", s_next_seq);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_history(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
    {
        return;
    }
    size_t len = SAVED_MESSAGES * sizeof(message_t);
    if (nvs_get_blob(nvs, "history", s_messages, &len) == ESP_OK && len % sizeof(message_t) == 0)
    {
        s_message_count = len / sizeof(message_t);
        for (size_t i = 0; i < s_message_count; i++)
        {
            s_messages[i].serial = s_next_serial++;
            s_messages[i].retries = RETRY_MAX; /* never resend old messages */
            seen_before(s_messages[i].id);
        }
        ESP_LOGI(TAG, "Restored %u message(s)", (unsigned)s_message_count);
    }
    uint32_t seq;
    if (nvs_get_u32(nvs, "seq", &seq) == ESP_OK)
    {
        s_next_seq = seq + 1;
    }
    nvs_close(nvs);
}

static void my_ip_string(char *out, size_t size);

/* ---------- HaLow transport: UDP broadcast flooding ---------- */

static void udp_broadcast(const char *payload, size_t len)
{
    if (s_sock < 0 || s_halow_ip == 0)
    {
        return;
    }
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_CHAT_UDP_PORT),
        .sin_addr.s_addr = htonl(HALOW_SUBNET | 0xff),
    };
    if (sendto(s_sock, payload, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
    {
        ESP_LOGW(TAG, "HaLow send failed: errno %d", errno);
    }
}

static void send_packet(cJSON *packet)
{
    char *body = cJSON_PrintUnformatted(packet);
    if (body != NULL)
    {
        udp_broadcast(body, strlen(body));
        free(body);
    }
}

/* Sends a chat message from this board's phone. */
static void my_ip_string(char *out, size_t size)
{
    esp_ip4_addr_t a = { .addr = s_halow_ip };
    snprintf(out, size, IPSTR, IP2STR(&a));
}

/* Sends a chat message from this board's phone. */
static void send_message(const char *name, const char *text, int64_t ts_ms)
{
    char id[16];
    snprintf(id, sizeof(id), "%s-%" PRIu32, s_node, s_next_seq++);
    char ip[16];
    my_ip_string(ip, sizeof(ip));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    seen_before(id); /* remember our own packet id so a rebroadcast of it is ignored */
    message_t *m = store_message(id, s_node, name, text, ts_ms, 0);
    m->sent_us = esp_timer_get_time();
    save_history();
    xSemaphoreGive(s_lock);

    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "t", "msg");
    cJSON_AddStringToObject(p, "id", id);
    cJSON_AddStringToObject(p, "node", s_node);
    cJSON_AddStringToObject(p, "from", name);
    cJSON_AddStringToObject(p, "text", text);
    cJSON_AddNumberToObject(p, "ts", (double)ts_ms);
    cJSON_AddStringToObject(p, "ip", ip);
    cJSON_AddNumberToObject(p, "hops", 0);
    send_packet(p);
    cJSON_Delete(p);
}

static void send_ack(const char *id)
{
    char pid[24];
    snprintf(pid, sizeof(pid), "ack-%s-%s", id, s_node);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    seen_before(pid);
    xSemaphoreGive(s_lock);

    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "t", "ack");
    cJSON_AddStringToObject(p, "pid", pid);
    cJSON_AddStringToObject(p, "id", id);
    cJSON_AddStringToObject(p, "node", s_node);
    cJSON_AddStringToObject(p, "name", s_name);
    cJSON_AddNumberToObject(p, "hops", 0);
    send_packet(p);
    cJSON_Delete(p);
}

/* Sent once a person's page on this board has shown the message. */
static void send_read_ack(const char *id, const char *viewer)
{
    char pid[24];
    snprintf(pid, sizeof(pid), "read-%s-%s", id, s_node);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    seen_before(pid);
    xSemaphoreGive(s_lock);

    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "t", "read");
    cJSON_AddStringToObject(p, "pid", pid);
    cJSON_AddStringToObject(p, "id", id);
    cJSON_AddStringToObject(p, "node", s_node);
    cJSON_AddStringToObject(p, "name", viewer);
    cJSON_AddNumberToObject(p, "hops", 0);
    send_packet(p);
    cJSON_Delete(p);
}

/* Rebroadcast a packet we've just seen for the first time, so it reaches boards that
 * couldn't hear the sender. A short random delay keeps boards from all shouting at once. */
static void rebroadcast(cJSON *packet, int hops)
{
    if (hops >= FLOOD_MAX_HOPS)
    {
        return;
    }
    cJSON_DeleteItemFromObject(packet, "hops");
    cJSON_AddNumberToObject(packet, "hops", hops + 1);
    vTaskDelay(pdMS_TO_TICKS(50 + esp_random() % 250));
    send_packet(packet);
}

static void handle_packet(const char *data, size_t len, const char *from_ip)
{
    cJSON *p = cJSON_ParseWithLength(data, len);
    if (p == NULL)
    {
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(p, "t");
    const cJSON *node = cJSON_GetObjectItem(p, "node");
    const cJSON *hops = cJSON_GetObjectItem(p, "hops");
    if (!cJSON_IsString(type) || !cJSON_IsString(node) || strlen(node->valuestring) != 4)
    {
        cJSON_Delete(p);
        return;
    }
    int hop_count = cJSON_IsNumber(hops) ? (int)hops->valuedouble : 0;

    if (strcmp(type->valuestring, "msg") == 0)
    {
        const cJSON *id = cJSON_GetObjectItem(p, "id");
        const cJSON *from = cJSON_GetObjectItem(p, "from");
        const cJSON *text = cJSON_GetObjectItem(p, "text");
        const cJSON *ts = cJSON_GetObjectItem(p, "ts");
        const cJSON *ip = cJSON_GetObjectItem(p, "ip");
        if (cJSON_IsString(id) && cJSON_IsString(from) && cJSON_IsString(text) &&
            strlen(id->valuestring) < 16)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool dup = seen_before(id->valuestring);
            if (!dup)
            {
                store_message(id->valuestring, node->valuestring, from->valuestring, text->valuestring,
                              cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0, (uint8_t)hop_count);
                note_peer(node->valuestring, from->valuestring);
                if (cJSON_IsString(ip))
                {
                    note_peer_ip(node->valuestring, ip->valuestring, NULL);
                }
                save_history();
            }
            xSemaphoreGive(s_lock);
            if (!dup)
            {
                ESP_LOGI(TAG, "Message from %s (%s) via %s, %d hop(s): %s",
                         from->valuestring, node->valuestring, from_ip, hop_count, text->valuestring);
                send_ack(id->valuestring);
                rebroadcast(p, hop_count);
            }
        }
    }
    else if (strcmp(type->valuestring, "ack") == 0)
    {
        const cJSON *pid = cJSON_GetObjectItem(p, "pid");
        const cJSON *id = cJSON_GetObjectItem(p, "id");
        const cJSON *name = cJSON_GetObjectItem(p, "name");
        if (cJSON_IsString(pid) && cJSON_IsString(id) && strlen(pid->valuestring) < 24)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool dup = seen_before(pid->valuestring);
            if (!dup)
            {
                mark_seen(id->valuestring, node->valuestring);
                note_peer(node->valuestring, cJSON_IsString(name) ? name->valuestring : NULL);
                save_history();
            }
            xSemaphoreGive(s_lock);
            if (!dup)
            {
                rebroadcast(p, hop_count);
            }
        }
    }
    else if (strcmp(type->valuestring, "read") == 0)
    {
        const cJSON *pid = cJSON_GetObjectItem(p, "pid");
        const cJSON *id = cJSON_GetObjectItem(p, "id");
        const cJSON *name = cJSON_GetObjectItem(p, "name");
        if (cJSON_IsString(pid) && cJSON_IsString(id) && strlen(pid->valuestring) < 24)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool dup = seen_before(pid->valuestring);
            if (!dup)
            {
                mark_read(id->valuestring, node->valuestring, cJSON_IsString(name) ? name->valuestring : "someone");
                save_history();
            }
            xSemaphoreGive(s_lock);
            if (!dup)
            {
                rebroadcast(p, hop_count);
            }
        }
    }
    else if (strcmp(type->valuestring, "hello") == 0)
    {
        const cJSON *name = cJSON_GetObjectItem(p, "name");
        const cJSON *role = cJSON_GetObjectItem(p, "role");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        note_peer(node->valuestring, cJSON_IsString(name) ? name->valuestring : NULL);
        note_peer_ip(node->valuestring, from_ip, cJSON_IsString(role) ? role->valuestring : NULL);
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(p);
}

static void udp_task(void *arg)
{
    (void)arg;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0)
    {
        ESP_LOGE(TAG, "UDP socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_CHAT_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        ESP_LOGE(TAG, "UDP bind failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    char *buf = malloc(1024);
    int64_t last_hello = 0;
    for (;;)
    {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, 1023, 0, (struct sockaddr *)&from, &from_len);
        if (n > 0 && from.sin_addr.s_addr != s_halow_ip)
        {
            char ip[16];
            inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
            handle_packet(buf, (size_t)n, ip);
        }

        /* Broadcasts can be lost: resend our own messages that nobody has acked yet. */
        int64_t now = esp_timer_get_time();
        cJSON *resend = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < s_message_count && resend == NULL; i++)
        {
            message_t *m = &s_messages[i];
            if (strcmp(m->node, s_node) == 0 && m->seen_count == 0 && m->retries < RETRY_MAX &&
                now - m->sent_us > 3 * 1000000LL)
            {
                m->retries++;
                m->sent_us = now;
                resend = cJSON_CreateObject();
                cJSON_AddStringToObject(resend, "t", "msg");
                cJSON_AddStringToObject(resend, "id", m->id);
                cJSON_AddStringToObject(resend, "node", s_node);
                cJSON_AddStringToObject(resend, "from", m->from);
                cJSON_AddStringToObject(resend, "text", m->text);
                cJSON_AddNumberToObject(resend, "ts", (double)m->ts_ms);
                cJSON_AddNumberToObject(resend, "hops", 0);
            }
        }
        xSemaphoreGive(s_lock);
        if (resend != NULL)
        {
            char ip[16];
            my_ip_string(ip, sizeof(ip));
            cJSON_AddStringToObject(resend, "ip", ip);
            send_packet(resend);
            cJSON_Delete(resend);
        }

        if (s_halow_ip != 0 && now - last_hello > 20 * 1000000LL)
        {
            last_hello = now;
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "t", "hello");
            cJSON_AddStringToObject(p, "node", s_node);
            cJSON_AddStringToObject(p, "name", s_name);
            cJSON_AddStringToObject(p, "role", s_role == ROLE_ACCESS_POINT ? "ap" : "sta");
            send_packet(p);
            cJSON_Delete(p);
        }
    }
}

/* ---------- HaLow: scan, then station or access point ---------- */

static void scan_rx_cb(const struct mmwlan_scan_result *result, void *arg)
{
    (void)arg;
    if (result->ssid_len == strlen(CONFIG_CHAT_HALOW_SSID) &&
        memcmp(result->ssid, CONFIG_CHAT_HALOW_SSID, result->ssid_len) == 0)
    {
        s_scan_found = true;
        memcpy(s_scan_found_ssid, result->ssid, result->ssid_len);
        s_scan_found_ssid[result->ssid_len] = '\0';
    }
}

static void scan_done_cb(enum mmwlan_scan_state state, void *arg)
{
    (void)arg;
    s_scan_state = state;
    xSemaphoreGive(s_scan_done);
}

static bool network_exists(void)
{
    for (int attempt = 1; attempt <= SCAN_ATTEMPTS; attempt++)
    {
        s_scan_found = false;
        xSemaphoreTake(s_scan_done, 0);
        struct mmhalow_scan_args args = {
            .rx_cb = scan_rx_cb,
            .complete_cb = scan_done_cb,
            .cb_arg = NULL,
        };
        if (mmhalow_scan(&args) != ESP_OK)
        {
            ESP_LOGW(TAG, "Scan request failed (attempt %d)", attempt);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(SCAN_TIMEOUT_MS));
        if (s_scan_found)
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

static void set_static_halow_address(void)
{
    uint8_t mac[6];
    mmwlan_get_mac_addr(mac);
    uint32_t host = mac[5];
    if (host < 2)
    {
        host += 2;
    }
    if (host == 255)
    {
        host = 254;
    }
    s_halow_ip = htonl(HALOW_SUBNET | host);

    esp_netif_dhcpc_stop(s_halow_netif);
    esp_netif_ip_info_t info = { 0 };
    info.ip.addr = s_halow_ip;
    info.netmask.addr = htonl(0xffffff00U);
    info.gw.addr = 0;
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_halow_netif, &info));
    esp_ip4_addr_t ip = { .addr = s_halow_ip };
    ESP_LOGI(TAG, "HaLow address " IPSTR, IP2STR(&ip));
}

static void halow_status_cb(enum mmwlan_sta_state state)
{
    static enum mmwlan_sta_state last = MMWLAN_STA_DISABLED;
    if (state != last)
    {
        ESP_EARLY_LOGI(TAG, "HaLow link: %s",
                       state == MMWLAN_STA_CONNECTED ? "connected" : state == MMWLAN_STA_CONNECTING ? "connecting" : "disconnected");
        last = state;
    }
    if (state == MMWLAN_STA_CONNECTED)
    {
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
    else
    {
        xEventGroupClearBits(s_events, CONNECTED_BIT);
    }
}

static void become_station(void)
{
    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };
    conf.sta.ssid_len = strlen(CONFIG_CHAT_HALOW_SSID);
    memcpy(conf.sta.ssid, CONFIG_CHAT_HALOW_SSID, conf.sta.ssid_len);
    conf.sta.security_type = MMWLAN_SAE;
    conf.sta.passphrase_len = strlen(CONFIG_CHAT_HALOW_PSK);
    memcpy(conf.sta.passphrase, CONFIG_CHAT_HALOW_PSK, conf.sta.passphrase_len);
    conf.sta.scan_interval_base_s = 5;
    conf.sta.scan_interval_limit_s = 30;
    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_STA, &conf));
    s_role = ROLE_STATION;
    s_role_decided_us = esp_timer_get_time();
    esp_err_t err = mmhalow_connect(halow_status_cb);
    ESP_LOGI(TAG, "Joining %s as a station (%s)", CONFIG_CHAT_HALOW_SSID, esp_err_to_name(err));
}

static void become_access_point(void)
{
    struct mmwlan_ap_args args = MMWLAN_AP_ARGS_INIT;
    args.ssid_len = strlen(CONFIG_CHAT_HALOW_SSID);
    memcpy(args.ssid, CONFIG_CHAT_HALOW_SSID, args.ssid_len);
    args.security_type = MMWLAN_SAE;
    args.passphrase_len = strlen(CONFIG_CHAT_HALOW_PSK);
    memcpy(args.passphrase, CONFIG_CHAT_HALOW_PSK, args.passphrase_len);
    args.op_class = CONFIG_CHAT_S1G_OPCLASS;
    args.s1g_chan_num = CONFIG_CHAT_S1G_CHANNEL;
    args.max_stas = 10;
    s_role = ROLE_ACCESS_POINT;
    s_role_decided_us = esp_timer_get_time();
    enum mmwlan_status status = mmwlan_ap_enable(&args);
    if (status == MMWLAN_SUCCESS)
    {
        /* The station-style netif only comes up on a link event, which an access point never gets. */
        esp_netif_action_connected(s_halow_netif, NULL, 0, NULL);
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        ESP_LOGI(TAG, "Started %s as the access point, channel %d", CONFIG_CHAT_HALOW_SSID, CONFIG_CHAT_S1G_CHANNEL);
    }
    else
    {
        ESP_LOGE(TAG, "Access point failed to start (status %d); rebooting in 10 s", (int)status);
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
}

static void halow_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(mmhalow_init(NULL));
    s_halow_netif = esp_netif_get_handle_from_ifkey(HALOW_NETIF_KEY);
    assert(s_halow_netif);
    set_static_halow_address();

    if (s_tx_dbm > 0)
    {
        mmwlan_override_max_tx_power(s_tx_dbm);
        ESP_LOGI(TAG, "HaLow TX power capped at %u dBm", s_tx_dbm);
    }

    /* Boards powered up together would all scan, find nothing, and all become access points.
     * A delay derived from the node id spreads them out; the lowest id wins. */
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    uint32_t stagger_ms = ((mac[0] + mac[1] + mac[2] + mac[3] + mac[4] + mac[5]) % 10) * 2000;
    vTaskDelay(pdMS_TO_TICKS(stagger_ms));

    /* A station that lost its access point reboots with this flag set to take over. */
    bool force_ap = false;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        uint8_t flag;
        if (nvs_get_u8(nvs, "force_ap", &flag) == ESP_OK && flag)
        {
            force_ap = true;
            nvs_erase_key(nvs, "force_ap");
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    /* Role: a board that can't afford the station's scan probes on its power supply can be
     * pinned as the hub; a board that must never be the hub can be pinned as a member. */
    if (s_role_setting == 1 || force_ap)
    {
        become_access_point();
    }
    else if (s_role_setting == 2 || network_exists())
    {
        become_station();
    }
    else
    {
        become_access_point();
    }

    /* Station: if the access point disappears for long enough, take over as the access point
     * by rebooting into it (a clean switch; the chip can't be re-initialised in place). */
    int64_t down_since = 0;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (s_role != ROLE_STATION)
        {
            continue;
        }
        bool up = xEventGroupGetBits(s_events) & CONNECTED_BIT;
        int64_t now = esp_timer_get_time();
        if (up)
        {
            down_since = 0;
        }
        else if (down_since == 0)
        {
            down_since = now;
        }
        else if (now - down_since > 120 * 1000000LL)
        {
            ESP_LOGW(TAG, "No access point for 2 minutes; rebooting to take over");
            nvs_handle_t nvs;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
            {
                nvs_set_u8(nvs, "force_ap", 1);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
            esp_restart();
        }
    }
}

/* ---------- Phone hotspot ---------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED)
    {
        ESP_LOGI(TAG, "Phone joined the hotspot");
    }
}

static void hotspot_start(void)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();
    assert(s_ap_netif);
    esp_netif_ip_info_t ip_info = { 0 };
    ip_info.ip.addr = esp_ip4addr_aton(HOTSPOT_IP);
    ip_info.gw.addr = ip_info.ip.addr;
    ip_info.netmask.addr = esp_ip4addr_aton(HOTSPOT_MASK);
    esp_netif_dhcps_stop(s_ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    esp_netif_dhcps_start(s_ap_netif);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), "HalowChat-%s", s_node);
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    strlcpy((char *)cfg.ap.password, CONFIG_CHAT_WIFI_PSK, sizeof(cfg.ap.password));
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.channel = CONFIG_CHAT_WIFI_CHANNEL;
    cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Hotspot %s on %s", cfg.ap.ssid, HOTSPOT_IP);

    /* Quiet start: the 2.4 GHz radio is calibrated now, but stays off while HaLow starts. */
    esp_wifi_stop();
    s_hotspot_paused = true;
}

static void hotspot_resume_task(void *arg)
{
    (void)arg;
    int64_t start = esp_timer_get_time();
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool halow_settled = s_role_decided_us != 0 && esp_timer_get_time() - s_role_decided_us > 3 * 1000000LL;
        if (halow_settled || esp_timer_get_time() - start > 45 * 1000000LL)
        {
            esp_wifi_start();
            s_hotspot_paused = false;
            ESP_LOGI(TAG, "Hotspot on");
            vTaskDelete(NULL);
            return;
        }
    }
}

/* ---------- Web page and API ---------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    httpd_resp_set_status(req, status);
    return send_json(req, root);
}

static esp_err_t read_body(httpd_req_t *req, char *body, size_t size)
{
    if (req->content_len == 0 || req->content_len >= size)
    {
        return ESP_FAIL;
    }
    int received = 0;
    while (received < (int)req->content_len)
    {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0)
        {
            return ESP_FAIL;
        }
        received += n;
    }
    body[received] = '\0';
    return ESP_OK;
}

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, chat_ui_html_start, chat_ui_html_end - chat_ui_html_start - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "node", s_node);
    cJSON_AddStringToObject(root, "name", s_name);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "role", s_role == ROLE_ACCESS_POINT ? "hub" : s_role == ROLE_STATION ? "member" : "deciding");
    bool up = xEventGroupGetBits(s_events) & CONNECTED_BIT;
    cJSON_AddStringToObject(root, "halow", up ? "up" : "down");
    if (s_halow_ip != 0)
    {
        char ip[16];
        esp_ip4_addr_t a = { .addr = s_halow_ip };
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
        cJSON_AddStringToObject(root, "halow_ip", ip);
    }
    int32_t rssi = mmwlan_get_rssi();
    if (s_role == ROLE_STATION && up && rssi != INT32_MIN)
    {
        cJSON_AddNumberToObject(root, "rssi", rssi);
    }
    cJSON_AddNumberToObject(root, "channel", CONFIG_CHAT_S1G_CHANNEL);
    if (s_channel != NULL)
    {
        cJSON_AddNumberToObject(root, "freq_mhz", s_channel->centre_freq_hz / 1e6);
        cJSON_AddNumberToObject(root, "bw_mhz", s_channel->bw_mhz);
    }
    cJSON_AddNumberToObject(root, "tx_dbm", s_tx_dbm);
    cJSON_AddStringToObject(root, "role_setting", s_role_setting == 1 ? "hub" : s_role_setting == 2 ? "member" : "auto");
    cJSON_AddNumberToObject(root, "boots", s_boot_count);
    cJSON *resets = cJSON_AddArrayToObject(root, "resets");
    for (size_t i = 0; i < s_reset_count; i++)
    {
        cJSON_AddItemToArray(resets, cJSON_CreateString(reset_reason_name((esp_reset_reason_t)s_reset_history[i])));
    }

    cJSON *peers = cJSON_AddArrayToObject(root, "peers");
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < MAX_PEERS; i++)
    {
        if (!s_peers[i].used)
        {
            continue;
        }
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "node", s_peers[i].node);
        cJSON_AddStringToObject(p, "name", s_peers[i].name);
        cJSON_AddNumberToObject(p, "last_seen_s", (double)((now - s_peers[i].last_seen_us) / 1000000));
        cJSON_AddItemToArray(peers, p);
    }
    xSemaphoreGive(s_lock);
    return send_json(req, root);
}

/* GET /api/messages?after=N&name=... returns messages with serial > N. A page asking for new
 * messages is a person reading them, so this is where read acks go out. */
static esp_err_t messages_handler(httpd_req_t *req)
{
    uint32_t after = 0;
    char query[96], value[16], viewer[MAX_NAME + 1] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "after", value, sizeof(value)) == ESP_OK)
        {
            after = (uint32_t)strtoul(value, NULL, 10);
        }
        httpd_query_key_value(query, "name", viewer, sizeof(viewer));
    }
    char to_ack[8][16];
    size_t n_ack = 0;
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "messages");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_message_count; i++)
    {
        message_t *m = &s_messages[i];
        if (m->serial <= after)
        {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "serial", m->serial);
        cJSON_AddStringToObject(j, "id", m->id);
        cJSON_AddStringToObject(j, "node", m->node);
        cJSON_AddStringToObject(j, "from", m->from);
        cJSON_AddStringToObject(j, "text", m->text);
        cJSON_AddNumberToObject(j, "ts", (double)m->ts_ms);
        cJSON_AddNumberToObject(j, "hops", m->hops);
        cJSON_AddBoolToObject(j, "mine", strcmp(m->node, s_node) == 0);
        cJSON *seen = cJSON_AddArrayToObject(j, "seen");
        for (uint8_t k = 0; k < m->seen_count; k++)
        {
            cJSON_AddItemToArray(seen, cJSON_CreateString(m->seen[k]));
        }
        cJSON *read = cJSON_AddArrayToObject(j, "read_by");
        for (uint8_t k = 0; k < m->read_count; k++)
        {
            cJSON_AddItemToArray(read, cJSON_CreateString(m->read_by[k]));
        }
        cJSON_AddItemToArray(list, j);
        if (strcmp(m->node, s_node) != 0 && !m->read_ack_sent && viewer[0] != '\0' && n_ack < 8)
        {
            m->read_ack_sent = true;
            strlcpy(to_ack[n_ack++], m->id, sizeof(to_ack[0]));
        }
    }
    cJSON_AddNumberToObject(root, "latest", s_next_serial - 1);
    xSemaphoreGive(s_lock);
    for (size_t i = 0; i < n_ack; i++)
    {
        send_read_ack(to_ack[i], viewer);
    }
    return send_json(req, root);
}

/* POST /api/send {"name": "...", "text": "...", "ts": 1700000000000} */
static esp_err_t send_handler(httpd_req_t *req)
{
    char body[512];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
    {
        return send_error(req, "400 Bad Request", "Bad request");
    }
    cJSON *json = cJSON_Parse(body);
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    const cJSON *text = cJSON_GetObjectItem(json, "text");
    const cJSON *ts = cJSON_GetObjectItem(json, "ts");
    if (!cJSON_IsString(text) || text->valuestring[0] == '\0' || strlen(text->valuestring) > MAX_TEXT ||
        (name != NULL && (!cJSON_IsString(name) || strlen(name->valuestring) > MAX_NAME)))
    {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "Message must be 1 to 200 characters");
    }
    if (cJSON_IsString(name) && name->valuestring[0] != '\0' && strcmp(name->valuestring, s_name) != 0)
    {
        strlcpy(s_name, name->valuestring, sizeof(s_name));
        save_settings();
    }
    if (s_halow_ip == 0 || !(xEventGroupGetBits(s_events) & CONNECTED_BIT))
    {
        cJSON_Delete(json);
        return send_error(req, "503 Service Unavailable", "HaLow isn't up yet");
    }
    send_message(s_name, text->valuestring, cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0);
    cJSON_Delete(json);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

/* POST /api/settings {"name": "...", "tx_dbm": 8}; TX power takes effect after a reboot. */
static esp_err_t settings_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
    {
        return send_error(req, "400 Bad Request", "Bad request");
    }
    cJSON *json = cJSON_Parse(body);
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    const cJSON *tx = cJSON_GetObjectItem(json, "tx_dbm");
    if (cJSON_IsString(name) && name->valuestring[0] != '\0' && strlen(name->valuestring) <= MAX_NAME)
    {
        strlcpy(s_name, name->valuestring, sizeof(s_name));
    }
    bool reboot = false;
    if (cJSON_IsNumber(tx) && tx->valuedouble >= 0 && tx->valuedouble <= 30 && (uint8_t)tx->valuedouble != s_tx_dbm)
    {
        s_tx_dbm = (uint8_t)tx->valuedouble;
        reboot = true;
    }
    const cJSON *role = cJSON_GetObjectItem(json, "role");
    if (cJSON_IsString(role))
    {
        uint8_t want = strcmp(role->valuestring, "hub") == 0 ? 1 : strcmp(role->valuestring, "member") == 0 ? 2 : 0;
        if (want != s_role_setting)
        {
            s_role_setting = want;
            reboot = true;
        }
    }
    cJSON_Delete(json);
    esp_err_t err = save_settings();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "reboot_needed", reboot);
    return send_json(req, root);
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    const esp_timer_create_args_t args = { .callback = reboot_cb, .name = "reboot" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK)
    {
        esp_timer_start_once(timer, 1000000);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_CHAT_WEB_PORT;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 16;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "Web page failed to start");
        return;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/messages", .method = HTTP_GET, .handler = messages_handler },
        { .uri = "/api/send", .method = HTTP_POST, .handler = send_handler },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_handler },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        httpd_register_uri_handler(server, &routes[i]);
    }
}

/* ---------- Main ---------- */

/* A warm restart leaves the HaLow chip and the GPIO interrupt settings as they were, which
 * can crash-loop the ESP32 before esp-halow installs its handler. Quiet them first. */
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
    esp_reset_reason_t reason = esp_reset_reason();
    const brownout_hal_config_t bod = {
        .threshold = CONFIG_ESP_BROWNOUT_DET_LVL,
        .enabled = true,
        .reset_enabled = true,
        .flash_power_down = true,
        .rf_power_down = true,
    };
    brownout_hal_config(&bod);
    halow_quiesce();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_node, sizeof(s_node), "%02X%02X", mac[4], mac[5]);
    record_reset(reason);
    load_settings();
    ESP_LOGI(TAG, "HalowChat node %s (%s), reset reason: %s", s_node, s_name, reset_reason_name(reason));

    s_messages = heap_caps_calloc(MAX_MESSAGES, sizeof(message_t), MALLOC_CAP_SPIRAM);
    if (s_messages == NULL)
    {
        s_messages = calloc(MAX_MESSAGES, sizeof(message_t));
    }
    assert(s_messages);
    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    load_history();
    s_scan_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    hotspot_start();
    web_server_start();
    xTaskCreate(halow_task, "halow", 8192, NULL, 5, NULL);
    xTaskCreate(hotspot_resume_task, "hotspot_resume", 3072, NULL, 4, NULL);
    xTaskCreate(udp_task, "chat_udp", 6144, NULL, 6, NULL);
}
