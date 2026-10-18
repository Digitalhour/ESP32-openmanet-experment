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

#include "mesh.h"

static const char *TAG = "halowchat";

#define NVS_NAMESPACE   "halowchat"
#define HALOW_SUBNET    0x0a2a0000U    /* 10.42.0.0/24, host byte order */
#define HOTSPOT_IP      "10.42.100.1"
#define HOTSPOT_MASK    "255.255.255.0"

#define MAX_MESSAGES   300
#define SAVED_MESSAGES 50
#define RETRY_MAX      3
#define MAX_TEXT       200
#define MAX_NAME       24
#define MAX_PEERS      16
#define MAX_SEEN_IDS   256
#define FLOOD_MAX_HOPS 0 /* the mesh layer relays; the chat layer does not */

#define RESET_HISTORY_LEN 10


#define CONNECTED_BIT BIT0

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
    int32_t rtt_ms;     /* last unicast ping over the mesh, -1 if none yet */
    uint32_t pings, pongs;
} peer_t;

static char s_node[5];        /* this board's node id, four hex digits from its MAC */
static char s_name[MAX_NAME + 1];
static uint8_t s_tx_dbm = CONFIG_CHAT_HALOW_MAX_TX_DBM;
static uint32_t s_boot_count;
static uint8_t s_reset_history[RESET_HISTORY_LEN];
static uint8_t s_reset_count;

static EventGroupHandle_t s_events;
static esp_netif_t *s_halow_netif;
static esp_netif_t *s_ap_netif;
static uint32_t s_halow_ip; /* network byte order */
static volatile bool s_hotspot_paused;
static int64_t s_mesh_up_us;

static message_t *s_messages; /* in PSRAM */
static size_t s_message_count;
static uint32_t s_next_serial = 1;
static uint32_t s_next_seq = 1;
static peer_t s_peers[MAX_PEERS];
static char s_seen_ids[MAX_SEEN_IDS][24];
static size_t s_seen_next;
static SemaphoreHandle_t s_lock;
static int s_sock = -1;

extern const char chat_ui_html_start[] asm("_binary_chat_ui_html_start");
extern const char chat_ui_html_end[] asm("_binary_chat_ui_html_end");

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
    if (!slot->used)
    {
        memset(slot, 0, sizeof(*slot));
        slot->rtt_ms = -1;
        ESP_LOGI(TAG, "Board %s is on the mesh", node);
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

/* Unicast to one board, for pings and replies. */
static void udp_send_to(const char *ip, const char *payload, size_t len)
{
    if (s_sock < 0 || ip == NULL || ip[0] == '\0')
    {
        return;
    }
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_CHAT_UDP_PORT),
    };
    if (inet_aton(ip, &to.sin_addr) == 0)
    {
        return;
    }
    sendto(s_sock, payload, len, 0, (struct sockaddr *)&to, sizeof(to));
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
    else if (strcmp(type->valuestring, "ping") == 0)
    {
        const cJSON *ts = cJSON_GetObjectItem(p, "ts");
        cJSON *pong = cJSON_CreateObject();
        cJSON_AddStringToObject(pong, "t", "pong");
        cJSON_AddStringToObject(pong, "node", s_node);
        cJSON_AddNumberToObject(pong, "ts", cJSON_IsNumber(ts) ? ts->valuedouble : 0);
        char *body = cJSON_PrintUnformatted(pong);
        if (body != NULL)
        {
            udp_send_to(from_ip, body, strlen(body));
            free(body);
        }
        cJSON_Delete(pong);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        note_peer(node->valuestring, NULL);
        note_peer_ip(node->valuestring, from_ip, NULL);
        xSemaphoreGive(s_lock);
    }
    else if (strcmp(type->valuestring, "pong") == 0)
    {
        const cJSON *ts = cJSON_GetObjectItem(p, "ts");
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < MAX_PEERS; i++)
        {
            if (s_peers[i].used && strcmp(s_peers[i].node, node->valuestring) == 0 && cJSON_IsNumber(ts))
            {
                s_peers[i].rtt_ms = (int32_t)((now - (int64_t)ts->valuedouble) / 1000);
                s_peers[i].pongs++;
                s_peers[i].last_seen_us = now;
                if (s_peers[i].pongs <= 3 || s_peers[i].pongs % 6 == 0)
                {
                    ESP_LOGI(TAG, "Ping to board %s over the mesh: %ld ms (%lu of %lu answered)", s_peers[i].node,
                             (long)s_peers[i].rtt_ms, (unsigned long)s_peers[i].pongs, (unsigned long)s_peers[i].pings);
                }
            }
        }
        xSemaphoreGive(s_lock);
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

        /* Unicast ping to every board we know, every 10 s: exercises routes, not just floods. */
        static int64_t last_ping = 0;
        if (s_halow_ip != 0 && now - last_ping > 10 * 1000000LL)
        {
            last_ping = now;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (size_t i = 0; i < MAX_PEERS; i++)
            {
                if (!s_peers[i].used || s_peers[i].ip[0] == '\0')
                {
                    continue;
                }
                s_peers[i].pings++;
                char body[96];
                snprintf(body, sizeof(body), "{\"t\":\"ping\",\"node\":\"%s\",\"ts\":%lld}", s_node, (long long)now);
                udp_send_to(s_peers[i].ip, body, strlen(body));
            }
            xSemaphoreGive(s_lock);
        }

        if (s_halow_ip != 0 && now - last_hello > 20 * 1000000LL)
        {
            last_hello = now;
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "t", "hello");
            cJSON_AddStringToObject(p, "node", s_node);
            cJSON_AddStringToObject(p, "name", s_name);
            cJSON_AddStringToObject(p, "role", "mesh");
            send_packet(p);
            cJSON_Delete(p);
        }
    }
}

/* ---------- HaLow mesh ---------- */

static void halow_task(void *arg)
{
    (void)arg;
    struct mesh_config cfg = { 0 };
    strlcpy(cfg.mesh_id, CONFIG_CHAT_MESH_ID, sizeof(cfg.mesh_id));
    cfg.s1g_chan = CONFIG_CHAT_S1G_CHANNEL;
    cfg.opclass = CONFIG_CHAT_S1G_OPCLASS;
    cfg.tx_dbm = s_tx_dbm;
    cfg.ttl = CONFIG_CHAT_MESH_TTL;
    if (mesh_start(&cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "Mesh failed to start; rebooting in 10 s");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
    s_halow_netif = mesh_netif();

    /* Static address from the mesh address: 10.42.0.<last byte>. */
    uint8_t addr[6];
    mesh_get_addr(addr);
    uint32_t host = addr[5];
    if (host < 2)
    {
        host += 2;
    }
    if (host == 255)
    {
        host = 254;
    }
    s_halow_ip = htonl(HALOW_SUBNET | host);
    esp_netif_ip_info_t info = { 0 };
    info.ip.addr = s_halow_ip;
    info.netmask.addr = htonl(0xffffff00U);
    info.gw.addr = 0;
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_halow_netif, &info));
    esp_netif_action_connected(s_halow_netif, NULL, 0, NULL);
    s_mesh_up_us = esp_timer_get_time();
    xEventGroupSetBits(s_events, CONNECTED_BIT);
    esp_ip4_addr_t ip = { .addr = s_halow_ip };
    ESP_LOGI(TAG, "Mesh address " IPSTR, IP2STR(&ip));
    vTaskDelete(NULL);
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
        bool halow_settled = s_mesh_up_us != 0 && esp_timer_get_time() - s_mesh_up_us > 3 * 1000000LL;
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
    cJSON_AddStringToObject(root, "role", "mesh");
    bool up = xEventGroupGetBits(s_events) & CONNECTED_BIT;
    cJSON_AddStringToObject(root, "halow", up ? "up" : "down");
    if (s_halow_ip != 0)
    {
        char ip[16];
        esp_ip4_addr_t a = { .addr = s_halow_ip };
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
        cJSON_AddStringToObject(root, "halow_ip", ip);
    }
    cJSON_AddNumberToObject(root, "channel", CONFIG_CHAT_S1G_CHANNEL);
    uint32_t freq_hz;
    uint8_t bw_mhz;
    if (mesh_channel_info(&freq_hz, &bw_mhz))
    {
        cJSON_AddNumberToObject(root, "freq_mhz", freq_hz / 1e6);
        cJSON_AddNumberToObject(root, "bw_mhz", bw_mhz);
    }
    cJSON_AddStringToObject(root, "mesh_id", CONFIG_CHAT_MESH_ID);
    cJSON_AddStringToObject(root, "chip_firmware", mesh_chip_firmware());
    {
        uint8_t addr[6];
        mesh_get_addr(addr);
        char a[18];
        snprintf(a, sizeof(a), "%02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        cJSON_AddStringToObject(root, "mesh_addr", a);
    }
    cJSON *neighbours = cJSON_AddArrayToObject(root, "neighbours");
    struct mesh_neighbour nb[16];
    int nn = mesh_get_neighbours(nb, 16);
    int64_t t_now = esp_timer_get_time();
    for (int i = 0; i < nn; i++)
    {
        cJSON *n = cJSON_CreateObject();
        char a[18];
        snprintf(a, sizeof(a), "%02x:%02x:%02x:%02x:%02x:%02x", nb[i].addr[0], nb[i].addr[1], nb[i].addr[2], nb[i].addr[3], nb[i].addr[4], nb[i].addr[5]);
        cJSON_AddStringToObject(n, "addr", a);
        cJSON_AddNumberToObject(n, "rssi", nb[i].rssi);
        cJSON_AddNumberToObject(n, "beacons", nb[i].beacons);
        cJSON_AddNumberToObject(n, "last_seen_s", (double)((t_now - nb[i].last_seen_us) / 1000000));
        cJSON_AddItemToArray(neighbours, n);
    }
    cJSON *routes = cJSON_AddArrayToObject(root, "routes");
    struct mesh_route rt[32];
    int nr = mesh_get_routes(rt, 32);
    for (int i = 0; i < nr; i++)
    {
        cJSON *r = cJSON_CreateObject();
        char a[18], h[18];
        snprintf(a, sizeof(a), "%02x:%02x:%02x:%02x:%02x:%02x", rt[i].dst[0], rt[i].dst[1], rt[i].dst[2], rt[i].dst[3], rt[i].dst[4], rt[i].dst[5]);
        snprintf(h, sizeof(h), "%02x:%02x:%02x:%02x:%02x:%02x", rt[i].next_hop[0], rt[i].next_hop[1], rt[i].next_hop[2], rt[i].next_hop[3], rt[i].next_hop[4], rt[i].next_hop[5]);
        cJSON_AddStringToObject(r, "dst", a);
        cJSON_AddStringToObject(r, "via", h);
        cJSON_AddNumberToObject(r, "hops", rt[i].hops);
        cJSON_AddItemToArray(routes, r);
    }
    struct mesh_stats ms;
    mesh_get_stats(&ms);
    cJSON *stats = cJSON_AddObjectToObject(root, "mesh");
    cJSON_AddNumberToObject(stats, "tx", ms.tx_frames);
    cJSON_AddNumberToObject(stats, "tx_failed", ms.tx_failed);
    cJSON_AddNumberToObject(stats, "tx_flooded", ms.tx_flooded);
    cJSON_AddNumberToObject(stats, "rx", ms.rx_frames);
    cJSON_AddNumberToObject(stats, "delivered", ms.rx_delivered);
    cJSON_AddNumberToObject(stats, "forwarded", ms.rx_forwarded);
    cJSON_AddNumberToObject(stats, "duplicates", ms.rx_duplicates);
    cJSON_AddNumberToObject(stats, "queue_full", ms.rx_queue_full);
    cJSON_AddNumberToObject(stats, "beacons_built", ms.beacons_built);
    cJSON_AddNumberToObject(stats, "beacons_heard", ms.beacons_heard);
    cJSON_AddNumberToObject(root, "tx_dbm", s_tx_dbm);
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
        cJSON_AddStringToObject(p, "ip", s_peers[i].ip);
        cJSON_AddNumberToObject(p, "rtt_ms", s_peers[i].rtt_ms);
        cJSON_AddNumberToObject(p, "pings", s_peers[i].pings);
        cJSON_AddNumberToObject(p, "pongs", s_peers[i].pongs);
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
    ESP_LOGI(TAG, "HalowChat mesh node %s (%s), reset reason: %s", s_node, s_name, reset_reason_name(reason));

    s_messages = heap_caps_calloc(MAX_MESSAGES, sizeof(message_t), MALLOC_CAP_SPIRAM);
    if (s_messages == NULL)
    {
        s_messages = calloc(MAX_MESSAGES, sizeof(message_t));
    }
    assert(s_messages);
    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    load_history();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    hotspot_start();
    web_server_start();
    xTaskCreate(halow_task, "halow", 8192, NULL, 5, NULL);
    xTaskCreate(hotspot_resume_task, "hotspot_resume", 3072, NULL, 4, NULL);
    xTaskCreate(udp_task, "chat_udp", 6144, NULL, 6, NULL);
}
