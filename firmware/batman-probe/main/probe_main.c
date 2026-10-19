/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * batman-probe: the XIAO ESP32S3 as a batman-adv (BATMAN_V) node over its 2.4 GHz Wi-Fi.
 *
 * Joins a Wi-Fi network, takes raw Ethernet frames off the station interface (ethertype
 * 0x4305 goes to batadv.c, everything else to lwIP as usual), and offers a "bat0" network
 * interface on top of batadv.c. Meant to be tested against a Linux box running batman-adv on
 * the same LAN: `batctl n`, `batctl o`, `batctl tg` there should show this board, and bat0
 * to bat0 pings should work.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_private/wifi.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/inet.h"

#include "batadv.h"

static const char *TAG = "probe";

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_bat_netif;
static volatile bool s_wifi_up;
static volatile bool s_bat_started;

/* ---------- bat0: an Ethernet-style interface whose link is batadv.c ---------- */

static esp_err_t bat_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    return batadv_tx((const uint8_t *)buffer, len);
}

static esp_err_t bat_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    (void)netstack_buf;
    return bat_transmit(h, buffer, len);
}

static void bat_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

static esp_err_t bat_post_attach(esp_netif_t *netif, void *h)
{
    esp_netif_driver_ifconfig_t cfg = {
        .handle = h,
        .transmit = bat_transmit,
        .transmit_wrap = bat_transmit_wrap,
        .driver_free_rx_buffer = bat_free_rx,
    };
    return esp_netif_set_driver_config(netif, &cfg);
}

static void bat_deliver(const uint8_t *frame, size_t len)
{
    if (s_bat_netif == NULL)
    {
        return;
    }
    uint8_t *copy = malloc(len);
    if (copy == NULL)
    {
        return;
    }
    memcpy(copy, frame, len);
    if (esp_netif_receive(s_bat_netif, copy, len, copy) != ESP_OK)
    {
        free(copy);
    }
}

/* ---------- Wi-Fi as the hard interface ---------- */

static esp_err_t wifi_link_send(const uint8_t *frame, size_t len)
{
    if (!s_wifi_up)
    {
        return ESP_ERR_INVALID_STATE;
    }
    int r = esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, (uint16_t)len);
    return r == 0 ? ESP_OK : ESP_FAIL;
}

/* Every frame the Wi-Fi station receives lands here first. */
static esp_err_t wifi_rx(void *buffer, uint16_t len, void *eb)
{
    const uint8_t *f = (const uint8_t *)buffer;
    if (len >= 14 && f[12] == 0x43 && f[13] == 0x05)
    {
        batadv_rx(f, len);
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    return esp_netif_receive(s_sta_netif, buffer, len, eb);
}

static void start_batadv(void)
{
    if (s_bat_started)
    {
        return;
    }
    s_bat_started = true;

    struct batadv_config cfg = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, cfg.hardif_mac);
    memcpy(cfg.bat_mac, cfg.hardif_mac, 6);
    cfg.bat_mac[0] |= 0x02;              /* locally administered twin of the station MAC */
    cfg.bat_mac[5] ^= 0x01;
    cfg.link_mtu = 1514;
    cfg.link_throughput = CONFIG_BATP_LINK_THROUGHPUT;
    cfg.orig_interval_ms = CONFIG_BATP_ORIG_INTERVAL_MS;
    cfg.elp_interval_ms = CONFIG_BATP_ELP_INTERVAL_MS;
    cfg.hop_penalty = CONFIG_BATP_HOP_PENALTY;
    cfg.link_send = wifi_link_send;
    cfg.deliver = bat_deliver;

    /* bat0 */
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base.if_key = "BAT0";
    base.if_desc = "bat0";
    base.route_prio = 40;
    bool dhcp = CONFIG_BATP_BAT_IP[0] == '\0';
    base.flags = (esp_netif_flags_t)((dhcp ? ESP_NETIF_DHCP_CLIENT : 0) | ESP_NETIF_FLAG_GARP | ESP_NETIF_FLAG_EVENT_IP_MODIFIED);
    esp_netif_config_t ncfg = { .base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA };
    s_bat_netif = esp_netif_new(&ncfg);
    assert(s_bat_netif);
    static esp_netif_driver_base_t driver = { .post_attach = bat_post_attach };
    ESP_ERROR_CHECK(esp_netif_attach(s_bat_netif, &driver));
    esp_netif_set_mac(s_bat_netif, cfg.bat_mac);
    esp_netif_action_start(s_bat_netif, NULL, 0, NULL);
    if (!dhcp)
    {
        esp_netif_ip_info_t info = { 0 };
        info.ip.addr = esp_ip4addr_aton(CONFIG_BATP_BAT_IP);
        info.netmask.addr = esp_ip4addr_aton(CONFIG_BATP_BAT_MASK);
        esp_netif_dhcpc_stop(s_bat_netif);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_bat_netif, &info));
    }
    esp_netif_action_connected(s_bat_netif, NULL, 0, NULL);

    ESP_ERROR_CHECK(batadv_start(&cfg));
    ESP_LOGI(TAG, "bat0 up: %s", dhcp ? "DHCP over the mesh" : CONFIG_BATP_BAT_IP);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "Wi-Fi joined %s", CONFIG_BATP_WIFI_SSID);
        /* Take the station's receive path over; non-batman frames still reach lwIP. */
        ESP_ERROR_CHECK(esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_rx));
        s_wifi_up = true;
        start_batadv();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_wifi_up = false;
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Wi-Fi not connected (reason %d, rssi %d); retrying", d != NULL ? d->reason : -1, d != NULL ? d->rssi : 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "LAN address " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void status_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!s_bat_started)
        {
            continue;
        }
        struct batadv_neighbour nb[8];
        struct batadv_originator og[16];
        struct batadv_tt_entry tg[32];
        struct batadv_stats st;
        int nn = batadv_get_neighbours(nb, 8);
        int no = batadv_get_originators(og, 16);
        int ng = batadv_get_tt_global(tg, 32);
        batadv_get_stats(&st);
        int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG, "---- batman: %d neighbours, %d originators, %d global clients | ttvn %u crc %08lx | elp tx/rx %lu/%lu ogm tx/rx/fwd %lu/%lu/%lu tt req/resp rx %lu/%lu ucast tx/rx %lu/%lu bcast tx/rx %lu/%lu frag rx/merged %lu/%lu",
                 nn, no, ng, batadv_ttvn(), (unsigned long)batadv_local_crc(), (unsigned long)st.elp_tx, (unsigned long)st.elp_rx,
                 (unsigned long)st.ogm_tx, (unsigned long)st.ogm_rx, (unsigned long)st.ogm_fwd, (unsigned long)st.tt_req_rx,
                 (unsigned long)st.tt_resp_rx, (unsigned long)st.ucast_tx, (unsigned long)st.ucast_rx, (unsigned long)st.bcast_tx,
                 (unsigned long)st.bcast_rx, (unsigned long)st.frag_rx, (unsigned long)st.frag_merged);
        for (int i = 0; i < nn; i++)
        {
            ESP_LOGI(TAG, "  neighbour %02x:%02x:%02x:%02x:%02x:%02x  last seen %lld s ago, its ELP interval %lu ms",
                     nb[i].addr[0], nb[i].addr[1], nb[i].addr[2], nb[i].addr[3], nb[i].addr[4], nb[i].addr[5],
                     (long long)((now - nb[i].last_seen_us) / 1000000), (unsigned long)nb[i].elp_interval_ms);
        }
        for (int i = 0; i < no; i++)
        {
            ESP_LOGI(TAG, "  originator %02x:%02x:%02x:%02x:%02x:%02x via %02x:%02x:%02x:%02x:%02x:%02x  throughput %lu  seqno %lu  ttvn %u%s",
                     og[i].addr[0], og[i].addr[1], og[i].addr[2], og[i].addr[3], og[i].addr[4], og[i].addr[5],
                     og[i].router[0], og[i].router[1], og[i].router[2], og[i].router[3], og[i].router[4], og[i].router[5],
                     (unsigned long)og[i].throughput, (unsigned long)og[i].last_seqno, og[i].last_ttvn, og[i].tt_known ? " (table known)" : "");
        }
        for (int i = 0; i < ng; i++)
        {
            ESP_LOGI(TAG, "  client %02x:%02x:%02x:%02x:%02x:%02x behind %02x:%02x:%02x:%02x:%02x:%02x flags %02x",
                     tg[i].addr[0], tg[i].addr[1], tg[i].addr[2], tg[i].addr[3], tg[i].addr[4], tg[i].addr[5],
                     tg[i].orig[0], tg[i].orig[1], tg[i].orig[2], tg[i].orig[3], tg[i].orig[4], tg[i].orig[5], tg[i].flags);
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, NULL));

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, CONFIG_BATP_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_BATP_WIFI_PSK, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = CONFIG_BATP_WIFI_PSK[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (CONFIG_BATP_WIFI_PSK[0] == '\0')
    {
        ESP_LOGW(TAG, "No Wi-Fi password set: run idf.py menuconfig, batman-probe, Wi-Fi password");
    }
    xTaskCreate(status_task, "status", 6144, NULL, 3, NULL);
}
