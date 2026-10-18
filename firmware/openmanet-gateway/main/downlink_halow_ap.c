#include <string.h>

#include "downlink_halow_ap.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"

#include "mmhalow.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "radio_control.h"

static const char *TAG = "downlink_halow_ap";

static gw_halow_ap_config_t s_cfg;
static esp_netif_t *s_netif = NULL;
static bool s_ready = false;
static bool s_started = false;

/* --- radio_control glue (review finding F06, design/PROJECT_REVIEW_2026-09-10.md) ---
 * See main/uplink_halow.c's own "radio_control glue" comment for the full
 * reasoning (static ctx only, config copied by value, per-wrapper mutex
 * only where a result struct is actually shared across tasks). This file's
 * init/set_config/register_vif_state_cb/wifi_start calls all happen once,
 * from the boot task, before anything else exists to race them - no lock
 * needed for those, same as uplink_halow.c's connect/disconnect/init/
 * set_config. list_channels is reachable from both the console and the web
 * UI and gets its own lock. */

typedef struct {
    esp_err_t result;
} radio_err_ctx_t;

static radio_err_ctx_t s_init_ctx;
static void do_mmhalow_init(void *arg)
{
    ((radio_err_ctx_t *)arg)->result = mmhalow_init(NULL);
}

static void do_mmhalow_print_version_info(void *arg)
{
    (void)arg;
    mmhalow_print_version_info();
}

typedef struct {
    wifi_interface_t iface;
    mmhalow_wifi_config_t conf;
    esp_err_t result;
} radio_set_config_ctx_t;

static radio_set_config_ctx_t s_set_config_ctx;
static void do_mmhalow_set_config(void *arg)
{
    radio_set_config_ctx_t *c = (radio_set_config_ctx_t *)arg;
    c->result = mmhalow_set_config(c->iface, &c->conf);
}

typedef struct {
    enum mmwlan_vif vif;
    mmwlan_vif_state_cb_t cb;
    void *cb_arg;
    enum mmwlan_status result;
} radio_vif_cb_ctx_t;

static radio_vif_cb_ctx_t s_vif_cb_ctx;
static void do_mmwlan_register_vif_state_cb(void *arg)
{
    radio_vif_cb_ctx_t *c = (radio_vif_cb_ctx_t *)arg;
    c->result = mmwlan_register_vif_state_cb(c->vif, c->cb, c->cb_arg);
}

static void do_mmhalow_wifi_start(void *arg)
{
    (void)arg;
    mmhalow_wifi_start(); /* void return - see downlink_halow_ap_is_started()'s doc comment */
}

static SemaphoreHandle_t s_channels_lock = NULL;
typedef struct {
    const struct mmwlan_s1g_channel_list *channel_list;
} radio_channels_ctx_t;
static radio_channels_ctx_t s_channels_ctx;
static void do_mmwlan_lookup_regulatory_domain(void *arg)
{
    radio_channels_ctx_t *c = (radio_channels_ctx_t *)arg;
    const struct mmwlan_regulatory_db *db = get_regulatory_db();
    c->channel_list = mmwlan_lookup_regulatory_domain(db, CONFIG_HALOW_COUNTRY_CODE);
}

/* Indexed by AID (mmwlan_ap_sta_status.aid), not a running counter - see
 * downlink_halow_ap_get_sta_count()'s header comment for why. +1 because
 * AIDs are 1-indexed (0 is reserved in 802.11). */
static bool s_sta_authorized[MMWLAN_AP_MAX_STAS_LIMIT + 1];

/* mmwlan_ap_args.sta_status_cb - fires on every state change for a STA
 * associated to this AP. MMWLAN_AP_STA_AUTHORIZED is "fully connected and
 * authorized for data transmission" (mmwlan.h); anything else (including a
 * STA that's merely authenticated/associated but not yet authorized, or one
 * that's dropped back to UNKNOWN) doesn't count as a connected leaf. */
static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *sta_status, void *arg)
{
    (void)arg;
    if (sta_status == NULL || sta_status->aid >= sizeof(s_sta_authorized)) {
        return;
    }
    bool was_authorized = s_sta_authorized[sta_status->aid];
    bool now_authorized = (sta_status->state == MMWLAN_AP_STA_AUTHORIZED);
    s_sta_authorized[sta_status->aid] = now_authorized;
    if (now_authorized != was_authorized) {
        ESP_LOGI(TAG, "leaf XIAO aid %u %s", sta_status->aid, now_authorized ? "associated" : "disassociated");
    }
}

uint8_t downlink_halow_ap_get_sta_count(void)
{
    uint8_t count = 0;
    for (size_t i = 0; i < sizeof(s_sta_authorized); i++) {
        if (s_sta_authorized[i]) {
            count++;
        }
    }
    return count;
}

/* Marks the downlink netif up/down at the esp_netif level for AP mode -
 * mmhalow_init() only wires mmwlan_register_link_state_cb() (mmhalow.c
 * L215), and that callback's own doc comment says outright: "This link
 * status callback will not be invoked in AP mode." So for a relay, nothing
 * was ever calling esp_netif_action_connected() on this netif - confirmed
 * on real hardware (2026-08-30): wait_for_downlink_up() (main/app_main.c)
 * timed out every time regardless of how long it waited, because the netif
 * was never going to report up at all, not because bring-up was merely
 * slow. mmwlan_register_vif_state_cb() is the non-deprecated replacement
 * and (unlike the STA-only one) carries no such AP-mode exclusion in its
 * doc comment - registered per-VIF, MMWLAN_VIF_AP included.
 *
 * Mirrors mmhalow_link_state()'s own call shape (mmhalow.c L16-29) exactly:
 * esp_netif_action_connected()/_disconnected() with no event data, since
 * nothing downstream of esp_netif reads it for this transition either. */
static void ap_vif_state_cb(const struct mmwlan_vif_state *state, void *arg)
{
    (void)arg;
    if (state == NULL || s_netif == NULL) {
        return;
    }
    if (state->link_state == MMWLAN_LINK_UP) {
        esp_netif_action_connected(s_netif, NULL, 0, NULL);
    } else {
        esp_netif_action_disconnected(s_netif, NULL, 0, NULL);
    }
}

/* AP mode doesn't support the full STA security set - mmwlan.h says so
 * explicitly (mmwlan_ap_enable() docs, v2.11.2-esp32-2): "OWE security is
 * not currently supported for AP mode." provisioning_validate() is meant to
 * catch GW_SECURITY_OWE before it ever reaches here; this fallback exists so
 * a stored config from a build that validated differently doesn't silently
 * hand the driver an unsupported value. */
static enum mmwlan_security_type ap_security_from_gw(gw_security_mode_t sec)
{
    switch (sec) {
    case GW_SECURITY_SAE:
        return MMWLAN_SAE;
    case GW_SECURITY_OWE:
        ESP_LOGW(TAG, "OWE is not supported for HaLow AP mode - falling back to open");
        return MMWLAN_OPEN;
    case GW_SECURITY_OPEN:
    default:
        return MMWLAN_OPEN;
    }
}

esp_err_t downlink_halow_ap_init(const gw_halow_ap_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));

    s_channels_lock = xSemaphoreCreateMutex();
    if (s_channels_lock == NULL) {
        ESP_LOGW(TAG, "couldn't create channel-list lock - gwcfg-list-halow-channels/GET /api/channels "
                      "will be unavailable");
    }

    esp_err_t err = radio_control_run(do_mmhalow_init, &s_init_ctx, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = s_init_ctx.result;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmhalow_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Same if_key uplink_halow.c relies on - mmhalow_init() always creates
     * its netif with if_key "WIFI_STA_DEF" regardless of which mode (STA or
     * AP) the caller ultimately runs (confirmed by reading mmhalow.c). */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "mmhalow_init() didn't register the expected WIFI_STA_DEF netif");
        return ESP_FAIL;
    }
    s_netif = netif;

    /* That netif is DHCP-client shaped (see the long comment in
     * downlink_halow_ap.h) - stop the client explicitly before assigning a
     * static address, so it can't contest the address we're about to set. */
    esp_err_t dhcpc_err = esp_netif_dhcpc_stop(netif);
    if (dhcpc_err != ESP_OK && dhcpc_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(dhcpc_err));
    }

    esp_netif_ip_info_t ip_info = { 0 };
    ip_info.ip.addr = ipaddr_addr(s_cfg.ip);
    ip_info.gw.addr = ipaddr_addr(s_cfg.ip); /* this radio's own address - no routing beyond it */
    ip_info.netmask.addr = ipaddr_addr(s_cfg.netmask);
    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to set static IP %s/%s: %s", s_cfg.ip, s_cfg.netmask, esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    /* Logged unconditionally, same reasoning as uplink_halow_init(): if this
     * prints, host<->MM6108 SPI works, which rules out wiring/pin/BCF issues
     * before an AP start attempt (which has its own, separate, alpha-API
     * failure modes) muddies the picture. */
    radio_control_run(do_mmhalow_print_version_info, NULL, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);

    if (!gw_halow_ap_is_configured(cfg)) {
        ESP_LOGW(TAG, "no HaLow AP SSID configured - radio is up but no AP will be started");
        return ESP_OK;
    }

    /* Built directly in the static radio_control ctx, same reasoning as
     * uplink_halow.c's halow_sta_bringup(). */
    s_set_config_ctx.iface = WIFI_IF_AP;
    mmhalow_wifi_config_t *conf = &s_set_config_ctx.conf;
    *conf = (mmhalow_wifi_config_t){
        .ap = MMWLAN_AP_ARGS_INIT,
    };

    size_t ssid_len = strlen(s_cfg.ssid);
    if (ssid_len > sizeof(conf->ap.ssid)) {
        ESP_LOGE(TAG, "HaLow AP SSID is %u bytes, max %u", (unsigned)ssid_len, (unsigned)sizeof(conf->ap.ssid));
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(conf->ap.ssid, s_cfg.ssid, ssid_len);
    conf->ap.ssid_len = ssid_len;

    conf->ap.security_type = ap_security_from_gw(s_cfg.security);
    if (conf->ap.security_type == MMWLAN_SAE) {
        size_t psk_len = strlen(s_cfg.psk);
        if (psk_len > sizeof(conf->ap.passphrase) - 1) {
            ESP_LOGE(TAG, "HaLow AP passphrase is %u bytes, max %u", (unsigned)psk_len,
                     (unsigned)sizeof(conf->ap.passphrase) - 1);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(conf->ap.passphrase, s_cfg.psk, psk_len);
        conf->ap.passphrase_len = psk_len;
    }

    /* pmf_mode already defaults to MMWLAN_PMF_REQUIRED via MMWLAN_AP_ARGS_INIT
     * - left as-is, matching Morse Micro's own "softap" reference example
     * (esp-halow v2.11.2-esp32-2, halow/examples/softap/main/app_main.c)
     * rather than second-guessing an alpha API's chosen default. */
    conf->ap.op_class = (uint16_t)s_cfg.op_class;
    conf->ap.s1g_chan_num = s_cfg.s1g_chan_num;
    conf->ap.max_stas = s_cfg.max_stas;
    conf->ap.sta_status_cb = ap_sta_status_cb;
    conf->ap.sta_status_cb_arg = NULL;
    memset(s_sta_authorized, 0, sizeof(s_sta_authorized));

    err = radio_control_run(do_mmhalow_set_config, &s_set_config_ctx, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = s_set_config_ctx.result;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmhalow_set_config(AP) failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Before starting, not after: mmwlan_ap_enable() (inside
     * mmhalow_wifi_start() below) is what actually brings the VIF up, so
     * registering any later risks missing that first transition. See
     * ap_vif_state_cb()'s own comment for why this is needed at all. */
    s_vif_cb_ctx = (radio_vif_cb_ctx_t){
        .vif = MMWLAN_VIF_AP,
        .cb = ap_vif_state_cb,
        .cb_arg = NULL,
    };
    esp_err_t vif_cb_err =
        radio_control_run(do_mmwlan_register_vif_state_cb, &s_vif_cb_ctx, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    if (vif_cb_err != ESP_OK || s_vif_cb_ctx.result != MMWLAN_SUCCESS) {
        ESP_LOGW(TAG, "mmwlan_register_vif_state_cb failed: %d - downlink netif may never be marked up",
                 s_vif_cb_ctx.result);
    }

    ESP_LOGI(TAG, "starting HaLow AP '%s' (op_class %d, chan %u)...", s_cfg.ssid, s_cfg.op_class,
             s_cfg.s1g_chan_num);
    radio_control_run(do_mmhalow_wifi_start, NULL, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    s_started = true;

    return ESP_OK;
}

bool downlink_halow_ap_is_ready(void)
{
    return s_ready;
}

esp_netif_t *downlink_halow_ap_get_netif(void)
{
    return s_netif;
}

bool downlink_halow_ap_is_started(void)
{
    return s_started;
}

size_t downlink_halow_ap_list_channels(halow_ap_channel_cb_t cb, void *ctx)
{
    /* Reachable from both the console and the web UI - s_channels_lock
     * serializes callers reusing s_channels_ctx, same division of labor as
     * uplink_halow.c's s_rssi_lock. Created once in downlink_halow_ap_init()
     * (see below), not lazily here: two first callers racing to create it
     * concurrently could each end up with their own mutex instance and
     * never actually be serialized against each other. */
    if (s_channels_lock == NULL || xSemaphoreTake(s_channels_lock, pdMS_TO_TICKS(RADIO_CONTROL_DEFAULT_TIMEOUT_MS)) != pdTRUE) {
        return 0;
    }

    esp_err_t err = radio_control_run(do_mmwlan_lookup_regulatory_domain, &s_channels_ctx,
                                       RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    const struct mmwlan_s1g_channel_list *channel_list = (err == ESP_OK) ? s_channels_ctx.channel_list : NULL;
    xSemaphoreGive(s_channels_lock);

    if (channel_list == NULL) {
        return 0;
    }

    for (unsigned i = 0; i < channel_list->num_channels; i++) {
        const struct mmwlan_s1g_channel *chan = &channel_list->channels[i];
        halow_ap_channel_t out = {
            .op_class = chan->s1g_operating_class,
            .s1g_chan_num = chan->s1g_chan_num,
            .freq_hz = chan->centre_freq_hz,
            .bw_mhz = chan->bw_mhz,
        };
        if (cb != NULL) {
            cb(&out, ctx);
        }
    }
    return channel_list->num_channels;
}
