/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "uplink_mesh.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "sdkconfig.h"

#include "mesh.h"

static const char *TAG = "uplink_mesh";

/* The mesh subnet. Every board takes 10.42.0.<last byte of its mesh address>; that is also
 * what the HalowChat mesh firmware uses, so the two can share a mesh. */
#define MESH_SUBNET 0x0a2a0000U

static uplink_halow_state_cb_t s_cb;
static void *s_cb_ctx;
static esp_netif_t *s_netif;
static volatile bool s_connected;
static volatile bool s_failed;

static void mesh_bringup_task(void *arg)
{
    (void)arg;
    struct mesh_config cfg = { 0 };
    strlcpy(cfg.mesh_id, CONFIG_GW_MESH_ID, sizeof(cfg.mesh_id));
    cfg.s1g_chan = CONFIG_GW_MESH_S1G_CHANNEL;
    cfg.opclass = CONFIG_GW_MESH_S1G_OPCLASS;
    cfg.tx_dbm = CONFIG_GW_MESH_TX_DBM;
    cfg.ttl = CONFIG_GW_MESH_TTL;
    cfg.spi_max_hz = CONFIG_GW_MESH_SPI_MAX_HZ;
    cfg.reset_hold_ms = CONFIG_GW_MESH_RESET_HOLD_MS;
    cfg.reset_settle_ms = CONFIG_GW_MESH_RESET_SETTLE_MS;
    if (mesh_start(&cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "mesh failed to start; the node keeps its SoftAP and config UI");
        s_failed = true;
        vTaskDelete(NULL);
        return;
    }
    s_netif = mesh_netif();

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
    esp_netif_ip_info_t info = { 0 };
    info.ip.addr = htonl(MESH_SUBNET | host);
    info.netmask.addr = htonl(0xffffff00U);
    info.gw.addr = 0;
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &info));
    esp_netif_action_connected(s_netif, NULL, 0, NULL);
    s_connected = true;
    ESP_LOGI(TAG, "mesh uplink up: " IPSTR " on mesh \"%s\"", IP2STR(&info.ip), CONFIG_GW_MESH_ID);
    if (s_cb != NULL)
    {
        s_cb(true, s_cb_ctx);
    }
    vTaskDelete(NULL);
}

esp_err_t uplink_mesh_start(void)
{
    if (xTaskCreate(mesh_bringup_task, "mesh_up", 8192, NULL, 5, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_netif_t *uplink_mesh_get_netif(void)
{
    return s_netif;
}

bool uplink_mesh_is_connected(void)
{
    return s_connected;
}

const char *uplink_mesh_state_name(void)
{
    if (s_failed)
    {
        return "radio failed";
    }
    if (!mesh_is_up())
    {
        return "starting";
    }
    return s_connected ? "up" : "down";
}

void uplink_mesh_set_state_callback(uplink_halow_state_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}
