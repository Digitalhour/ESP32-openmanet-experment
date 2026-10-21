/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * hopnode: a cheap, disposable HaLow relay for a batman-adv mesh.
 *
 * Nothing but the two layers that matter: the 802.11s mesh link (mesh.c: beacons, peering,
 * 4-address frames) and batman-adv on top of it (batadv.c: BATMAN_V, the routing OpenMANET
 * runs). No hotspot, no page, no chat. It forwards for everyone, announces itself, and logs
 * what it sees. bat0 gets an address so the node can be pinged and ping its neighbours.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"

#include "driver/gpio.h"

#include "batadv.h"
#include "mesh.h"

static const char *TAG = "hop";

static esp_netif_t *s_bat_netif;
static uint8_t s_bat_mac[6];

/* ---------- bat0 ---------- */

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
        .handle = h, .transmit = bat_transmit, .transmit_wrap = bat_transmit_wrap, .driver_free_rx_buffer = bat_free_rx,
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

static uint32_t bat_ip_for(const uint8_t *bat_mac)
{
    uint32_t subnet = ntohl(esp_ip4addr_aton(CONFIG_HOP_BAT_SUBNET)) & 0xffffff00U;
    uint32_t host = bat_mac[5];
    if (host < 2)
    {
        host += 2;
    }
    if (host == 255)
    {
        host = 254;
    }
    return htonl(subnet | host);
}

/* ---------- Mesh link to batman ---------- */

static void mesh_l2_rx(const uint8_t *frame, size_t len)
{
    if (len >= 14 && frame[12] == 0x43 && frame[13] == 0x05)
    {
        batadv_rx(frame, len);
    }
}

static esp_err_t mesh_link_send(const uint8_t *frame, size_t len)
{
    return mesh_send_ethernet(frame, len);
}

/* ---------- Pinging the neighbours over bat0 ---------- */

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t tx = 0, rx = 0, ms = 0;
    ip_addr_t target;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &tx, sizeof(tx));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &rx, sizeof(rx));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &ms, sizeof(ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    ESP_LOGI(TAG, "ping %s over bat0: %lu of %lu answered, %lu ms total", ipaddr_ntoa(&target), (unsigned long)rx,
             (unsigned long)tx, (unsigned long)ms);
    esp_ping_delete_session(hdl);
}

static void ping_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(15000));
        struct batadv_tt_entry tg[16];
        int n = batadv_get_tt_global(tg, 16);
        for (int i = 0; i < n; i++)
        {
            if (tg[i].addr[0] & 1)
            {
                continue;   /* group addresses announced by Linux nodes */
            }
            esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
            cfg.target_addr.type = IPADDR_TYPE_V4;
            cfg.target_addr.u_addr.ip4.addr = bat_ip_for(tg[i].addr);
            cfg.count = 4;
            cfg.interval_ms = 500;
            cfg.timeout_ms = 2000;
            cfg.data_size = 32;
            esp_ping_callbacks_t cbs = { .on_ping_end = on_ping_end };
            esp_ping_handle_t h;
            if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK)
            {
                esp_ping_start(h);
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
    }
}

/* ---------- Status ---------- */

static void status_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        struct mesh_neighbour nb[8];
        int nn = mesh_get_neighbours(nb, 8);
        struct batadv_neighbour bn[8];
        struct batadv_originator og[16];
        struct batadv_tt_entry tg[16];
        struct batadv_stats st;
        int bnn = batadv_get_neighbours(bn, 8);
        int no = batadv_get_originators(og, 16);
        int ng = batadv_get_tt_global(tg, 16);
        batadv_get_stats(&st);
        struct mesh_stats ms;
        mesh_get_stats(&ms);
        if (mesh_is_secure())
        {
            ESP_LOGI(TAG, "---- secure: data frames seen %lu, encrypted %lu, chip decrypted %lu, plaintext refused %lu, unicast without key %lu",
                     (unsigned long)ms.rx_hook_data, (unsigned long)ms.rx_hook_prot, (unsigned long)ms.rx_hook_decrypted,
                     (unsigned long)ms.rx_unprotected, (unsigned long)ms.tx_unkeyed);
        }
        ESP_LOGI(TAG, "---- mesh: %d neighbours (tx %lu rx %lu beacons heard %lu) | batman: %d neighbours, %d originators, %d clients | elp %lu/%lu ogm %lu/%lu/%lu ucast %lu/%lu/%lu bcast %lu/%lu/%lu",
                 nn, (unsigned long)ms.tx_frames, (unsigned long)ms.rx_frames, (unsigned long)ms.beacons_heard, bnn, no, ng,
                 (unsigned long)st.elp_tx, (unsigned long)st.elp_rx, (unsigned long)st.ogm_tx, (unsigned long)st.ogm_rx, (unsigned long)st.ogm_fwd,
                 (unsigned long)st.ucast_tx, (unsigned long)st.ucast_rx, (unsigned long)st.ucast_fwd, (unsigned long)st.bcast_tx,
                 (unsigned long)st.bcast_rx, (unsigned long)st.bcast_fwd);
        for (int i = 0; i < nn; i++)
        {
            ESP_LOGI(TAG, "  mesh neighbour %02x:%02x:%02x:%02x:%02x:%02x rssi %d, %s", nb[i].addr[0], nb[i].addr[1], nb[i].addr[2],
                     nb[i].addr[3], nb[i].addr[4], nb[i].addr[5], nb[i].rssi, mesh_plink_state_name(nb[i].plink));
        }
        for (int i = 0; i < no; i++)
        {
            ESP_LOGI(TAG, "  originator %02x:%02x:%02x:%02x:%02x:%02x via %02x:%02x:%02x:%02x:%02x:%02x throughput %lu ttvn %u%s",
                     og[i].addr[0], og[i].addr[1], og[i].addr[2], og[i].addr[3], og[i].addr[4], og[i].addr[5], og[i].router[0], og[i].router[1],
                     og[i].router[2], og[i].router[3], og[i].router[4], og[i].router[5], (unsigned long)og[i].throughput, og[i].last_ttvn,
                     og[i].tt_known ? " (table known)" : "");
        }
        for (int i = 0; i < ng; i++)
        {
            ESP_LOGI(TAG, "  client %02x:%02x:%02x:%02x:%02x:%02x behind %02x:%02x:%02x:%02x:%02x:%02x", tg[i].addr[0], tg[i].addr[1], tg[i].addr[2],
                     tg[i].addr[3], tg[i].addr[4], tg[i].addr[5], tg[i].orig[0], tg[i].orig[1], tg[i].orig[2], tg[i].orig[3], tg[i].orig[4], tg[i].orig[5]);
        }
    }
}

/* ---------- Main ---------- */

static void halow_quiesce(void)
{
    gpio_reset_pin(CONFIG_MM_SPI_IRQ);
    gpio_reset_pin(CONFIG_MM_BUSY);
    gpio_config_t reset_pin = { .pin_bit_mask = 1ULL << CONFIG_MM_RESET_N, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&reset_pin);
    gpio_set_level(CONFIG_MM_RESET_N, 0);
}

static void bringup_task(void *arg)
{
    (void)arg;
    struct mesh_config mc = { 0 };
    strlcpy(mc.mesh_id, CONFIG_HOP_MESH_ID, sizeof(mc.mesh_id));
    strlcpy(mc.password, CONFIG_HOP_MESH_PASSWORD, sizeof(mc.password));
    mc.s1g_chan = CONFIG_HOP_S1G_CHANNEL;
    mc.opclass = CONFIG_HOP_S1G_OPCLASS;
    mc.tx_dbm = CONFIG_HOP_TX_DBM;
    mc.ttl = 1;
    mc.require_peering = true;
    mc.l2_mode = true;
    mc.l2_rx = mesh_l2_rx;
    mc.spi_max_hz = CONFIG_HOP_HALOW_SPI_MAX_HZ;
    mc.reset_hold_ms = CONFIG_HOP_HALOW_RESET_HOLD_MS;
    mc.reset_settle_ms = CONFIG_HOP_HALOW_RESET_SETTLE_MS;
    if (mesh_start(&mc) != ESP_OK)
    {
        ESP_LOGE(TAG, "mesh failed to start; rebooting in 10 s");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    struct batadv_config bc = { 0 };
    mesh_get_addr(bc.hardif_mac);
    memcpy(s_bat_mac, bc.hardif_mac, 6);
    s_bat_mac[5] ^= 0x01;
    memcpy(bc.bat_mac, s_bat_mac, 6);
    bc.link_mtu = 1514;
    bc.link_throughput = 10;
    bc.orig_interval_ms = 1000;
    bc.elp_interval_ms = 500;
    bc.hop_penalty = 30;
    bc.link_send = mesh_link_send;
    bc.deliver = bat_deliver;

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base.if_key = "BAT0";
    base.if_desc = "bat0";
    base.route_prio = 40;
    base.flags = (esp_netif_flags_t)(ESP_NETIF_FLAG_GARP | ESP_NETIF_FLAG_EVENT_IP_MODIFIED);
    esp_netif_config_t ncfg = { .base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA };
    s_bat_netif = esp_netif_new(&ncfg);
    assert(s_bat_netif);
    static esp_netif_driver_base_t driver = { .post_attach = bat_post_attach };
    ESP_ERROR_CHECK(esp_netif_attach(s_bat_netif, &driver));
    esp_netif_set_mac(s_bat_netif, s_bat_mac);
    esp_netif_action_start(s_bat_netif, NULL, 0, NULL);
    esp_netif_ip_info_t info = { 0 };
    info.ip.addr = bat_ip_for(s_bat_mac);
    info.netmask.addr = esp_ip4addr_aton(CONFIG_HOP_BAT_MASK);
    esp_netif_dhcpc_stop(s_bat_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_bat_netif, &info));
    esp_netif_action_connected(s_bat_netif, NULL, 0, NULL);

    ESP_ERROR_CHECK(batadv_start(&bc));
    ESP_LOGI(TAG, "hop node up: bat0 " IPSTR, IP2STR(&info.ip));
    xTaskCreate(status_task, "status", 6144, NULL, 3, NULL);
#if CONFIG_HOP_PING_PEERS
    xTaskCreate(ping_task, "pinger", 4096, NULL, 3, NULL);
#endif
    vTaskDelete(NULL);
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
    ESP_LOGI(TAG, "hopnode starting (reset reason %d)", (int)esp_reset_reason());
    xTaskCreate(bringup_task, "bringup", 8192, NULL, 5, NULL);
}
