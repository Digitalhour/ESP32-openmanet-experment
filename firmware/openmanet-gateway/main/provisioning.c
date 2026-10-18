#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provisioning.h"

#include "auth.h"
#include "cot_relay.h"
#include "downlink_halow_ap.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link_history.h"
#include "lwip/inet.h"
#include "net_validate.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "task_stats.h"
#include "tls_identity.h"
#include "uplink_halow.h"
#include "uplink_wifi.h"

static const char *TAG = "provisioning";

#define GWCFG_NVS_NAMESPACE "gwcfg"
#define GWCFG_NVS_KEY       "config"

/* Generous compared to the web UI's budget: nothing is waiting on a socket
 * here, and a console scan is a deliberate, attended action. */
#define GWCFG_SCAN_TIMEOUT_MS 12000

/* Points at the app's live in-RAM config so console commands can edit it
 * directly; set by provisioning_register_console_commands(). */
static gw_config_t *s_cfg = NULL;

/* Set by provisioning_get_recovery_defaults() below, never cleared - a fresh
 * boot starts this false again regardless, and nothing needs to turn it back
 * off mid-boot. See provisioning_in_recovery_mode()'s own comment for what
 * this guards (review finding F14's migration portion). */
static bool s_recovery_mode = false;

/* The live config is touched by three tasks - the console REPL, the httpd
 * task, and app_main at boot - so mutation is serialized. Created in
 * provisioning_init(), i.e. before any of those exist. */
static SemaphoreHandle_t s_cfg_lock = NULL;

/* Bumped by every provisioning_config_commit() below - never persisted,
 * never compared across a reboot, purely an in-RAM "has anyone written since
 * I last looked" signal for the one caller that needs it. Review finding F11
 * (design/PROJECT_REVIEW_2026-09-10.md): every console setter here already
 * does its own read-validate-write as one unbroken provisioning_config_lock()
 * hold, so they can't race each other or lose an update. web_ui.c's
 * config_post_handler is the one exception - it deliberately unlocks between
 * reading the snapshot it edits and writing the result back, so as not to
 * hold this lock across a slow JSON parse - and used to write that whole
 * snapshot back unconditionally, silently discarding anything a console
 * command (or another request) changed in between. This counter is what lets
 * that one caller detect the gap and refuse instead of clobbering - see
 * config_post_handler's own use of provisioning_config_revision(). */
static uint32_t s_cfg_revision = 0;

void provisioning_config_lock(void)
{
    if (s_cfg_lock != NULL) {
        xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    }
}

void provisioning_config_unlock(void)
{
    if (s_cfg_lock != NULL) {
        xSemaphoreGive(s_cfg_lock);
    }
}

uint32_t provisioning_config_revision(void)
{
    return s_cfg_revision;
}

void provisioning_config_commit(const gw_config_t *new_cfg)
{
    *s_cfg = *new_cfg;
    s_cfg_revision++;
}

void provisioning_get_defaults(gw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->magic = GW_CONFIG_MAGIC;
    cfg->version = GW_CONFIG_VERSION;

    /* Last two bytes of the factory-burned base MAC, so two units on a bench
     * don't both show up as "xiao-gateway" - esp_efuse_mac_get_default() reads
     * BLK1 directly (esp-idf v5.5.1 components/esp_hw_support/mac_addr.c),
     * so it works here before esp_wifi/esp_netif exist, and it's already
     * unique per chip, unlike a randomly generated value nothing would need
     * to persist. Falls back to "0000" if the read ever fails - not expected
     * on real hardware, every chip ships with this fused. */
    char mac_suffix[5] = "0000";
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(mac_suffix, sizeof(mac_suffix), "%02x%02x", mac[4], mac[5]);
    }

    snprintf(cfg->node_id, sizeof(cfg->node_id), "xiao-gw-%s", mac_suffix);

    /* GW_ROLE_CLIENT: today's original design, and still what a factory-fresh
     * node ships as - a relay is a deliberate per-node choice
     * (gwcfg-set-role), not a default anyone should get by accident. */
    cfg->role = GW_ROLE_CLIENT;

    /* Off: see gw_config.h's own comment. An operator opts a specific node
     * into this, it never happens by accident. */
    cfg->allow_uplink_management = false;

    /* No uplink by default, and deliberately no placeholder SSID.
     *
     * This used to ship "openmanet-halow", which made a factory-fresh node
     * indistinguishable from a configured one whose AP is switched off: both
     * reported "searching" and blinked identically, while the node burned
     * 15-second association attempts against a name nobody had chosen. An
     * empty SSID is the "not configured yet" state (gw_uplink_is_configured())
     * and the firmware reports and acts on it - see uplink_halow_start(). */
    cfg->uplink.ssid[0] = '\0';
    cfg->uplink.psk[0] = '\0';
    cfg->uplink.security = GW_SECURITY_OPEN;
    cfg->uplink.use_static_ip = false;
    cfg->uplink.static_ip[0] = '\0';
    cfg->uplink.static_gateway[0] = '\0';
    cfg->uplink.static_netmask[0] = '\0';
    cfg->uplink.static_dns[0] = '\0';

    /* GW_ROLE_RELAY fields. Also unconfigured by default, same reasoning as
     * the HaLow uplink above - and harmless to leave populated with their
     * defaults on a GW_ROLE_CLIENT node, since bring_up_client_role() never
     * reads them. */
    cfg->wifi_uplink.ssid[0] = '\0';
    cfg->wifi_uplink.psk[0] = '\0';

    cfg->halow_ap.ssid[0] = '\0';
    cfg->halow_ap.psk[0] = '\0';
    cfg->halow_ap.security = GW_SECURITY_SAE;
    cfg->halow_ap.op_class = 0;
    cfg->halow_ap.s1g_chan_num = 0;
    cfg->halow_ap.max_stas = 0; /* => MMWLAN_DEFAULT_AP_MAX_STAS (4) */
    /* A different /24 than the client role's SoftAP subnet (172.16.50.0/24)
     * purely so the two are never confusable in logs/ARP tables if someone's
     * looking at both roles side by side - see gw_config.h. */
    strlcpy(cfg->halow_ap.ip, "172.16.60.1", sizeof(cfg->halow_ap.ip));
    strlcpy(cfg->halow_ap.netmask, "255.255.255.0", sizeof(cfg->halow_ap.netmask));

    /* Local client-facing SoftAP.
     *
     * 172.16.50.0/24, not 192.168.x: this subnet has to coexist with whatever
     * the phone or tablet was last attached to, and with the mesh subnet on
     * the far side of the NAT. 192.168.0/1/4/50.x are heavily used by home
     * routers, phone hotspots and other ESP32 SoftAPs (esp_netif's own default
     * is 192.168.4.1), and an overlap between a client's remembered network
     * and this one produces routing behaviour that is very hard to diagnose in
     * the field. 172.16.0.0/12 is the least-trafficked of the three RFC1918
     * blocks. Every XIAO node can safely use the same subnet - each one NATs
     * behind its own uplink address, so they never see each other's. */
    snprintf(cfg->softap.ssid, sizeof(cfg->softap.ssid), "xiao-gateway-%s", mac_suffix);
    strlcpy(cfg->softap.psk, "openmanet", sizeof(cfg->softap.psk));
    cfg->softap.channel = 6;
    cfg->softap.max_connections = 8;
    cfg->softap.use_custom_subnet = true;
    strlcpy(cfg->softap.ip, "172.16.50.1", sizeof(cfg->softap.ip));
    strlcpy(cfg->softap.gateway, "172.16.50.1", sizeof(cfg->softap.gateway));
    strlcpy(cfg->softap.netmask, "255.255.255.0", sizeof(cfg->softap.netmask));

    /* ATAK CoT multicast group. */
    strlcpy(cfg->cot.group, "239.2.3.1", sizeof(cfg->cot.group));
    cfg->cot.port = 6969;

    /* auth: left all-zero by the memset above. password_set == false is the
     * correct default - see gw_auth_config_t's own comment - and forces the
     * first-use password-set flow in web_ui.html rather than shipping a
     * fixed default credential (a compliance requirement, not a preference -
     * CA SB-327, UK PSTI - see CLAUDE.md). */
}

/* Same networking defaults as provisioning_get_defaults(), but with
 * onboarding forced permanently closed instead of the fresh-device default
 * of open. Use this - not provisioning_get_defaults() - wherever
 * provisioning_load() falls back to defaults because a *stored* config
 * existed but this firmware can't use it (wrong size, magic/version
 * mismatch, failed validation), as opposed to no config ever having been
 * written at all.
 *
 * Review finding F14's migration portion (design/PROJECT_REVIEW_2026-09-10.md):
 * before this, every one of those fallback paths called
 * provisioning_get_defaults() directly, which leaves password_set == false
 * *and* onboarding_open == true - indistinguishable from a genuinely fresh,
 * never-owned device. A routine GW_CONFIG_VERSION bump (this project has
 * done three - v7, v8, v9 - across the same session that found this) would
 * therefore silently un-own a real device on its next update: the operator's
 * password is gone, and anyone with SoftAP access can walk through the
 * first-use claim flow as if it were fresh out of the box. Not hypothetical -
 * every version bump already shipped this session would have done exactly
 * that to a device with a real stored credential.
 *
 * Forcing onboarding closed here means that path is no longer available by
 * accident: the device still boots, still serves SoftAP/console/management
 * (it is not bricked), but nobody can claim it through the web UI. The only
 * way back in is the existing physical factory-reset button
 * (factory_reset.c's do_factory_reset(), which calls
 * provisioning_get_defaults() - not this function - deliberately, on
 * physical possession) - exactly the "closed recovery state requiring local
 * owner action" the review's acceptance criteria ask for, reusing an
 * already-built, already-verified mechanism rather than a new one. */
void provisioning_get_recovery_defaults(gw_config_t *cfg)
{
    provisioning_get_defaults(cfg);
    cfg->auth.onboarding_ever_started = true;
    cfg->auth.onboarding_open = false;
    cfg->auth.onboarding_boots_remaining = 0;
    s_recovery_mode = true;
}

/* True for the rest of this boot once provisioning_get_recovery_defaults()
 * has run. Exists for exactly one caller: tls_identity.c's boot-time
 * identity generation, which - like this whole fix is about - would
 * otherwise auto-persist the very recovery defaults this function just
 * derived, with no operator involved at all. Found on real hardware, not
 * predicted: verifying this fix by simulating a version bump against a real
 * stored credential showed the *auth* side working correctly (onboarding
 * stayed closed), but the original config was still gone for good on the
 * very next boot - tls_identity_init()'s own "no identity yet, generate and
 * save one" logic ran during the same recovery boot and persisted the whole
 * live (recovery-defaulted) struct in the process, permanently overwriting
 * the real blob still sitting in NVS at that point.
 *
 * Deliberately not a general "block every write" gate: the console already
 * requires physical access, which is the same "local owner action" bar the
 * review asks a recovery state to enforce, so every console command
 * (gwcfg-save, gwcfg-reset, gwcfg-reset-auth, gwcfg-reset-tls-identity, ...)
 * stays free to persist normally, deliberately, when an operator is actually
 * sitting at it. web_ui.c's config writes are already blocked in recovery
 * mode for an unrelated reason - auth_require_session() has no session to
 * check against with no password ever set. Only an unattended, no-operator
 * background task auto-persisting on this device's own initiative needs to
 * check this. */
bool provisioning_in_recovery_mode(void)
{
    return s_recovery_mode;
}

esp_err_t provisioning_init(void)
{
    if (s_cfg_lock == NULL) {
        s_cfg_lock = xSemaphoreCreateMutex();
        if (s_cfg_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t provisioning_load(gw_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GWCFG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no stored config (%s), using defaults", esp_err_to_name(err));
        provisioning_get_defaults(cfg);
        return ESP_OK;
    }

    size_t len = sizeof(*cfg);
    err = nvs_get_blob(handle, GWCFG_NVS_KEY, cfg, &len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* The namespace exists (nvs_open() above succeeded) but this key was
         * never written under it - genuinely nothing to preserve, same as
         * the nvs_open() failure case above. Open defaults are correct here. */
        ESP_LOGW(TAG, "no stored config (%s), using defaults", esp_err_to_name(err));
        provisioning_get_defaults(cfg);
        return ESP_OK;
    }
    if (err != ESP_OK || len != sizeof(*cfg)) {
        /* Unlike the case above, a value *was* found under this key - it's
         * just not usable (wrong size, or some other NVS-level error reading
         * it). Something was stored here once, possibly by a real owner, so
         * this does not get to look like a fresh device - see
         * provisioning_get_recovery_defaults()'s own comment (review finding
         * F14's migration portion). */
        ESP_LOGW(TAG, "stored config missing/invalid (%s), using recovery defaults", esp_err_to_name(err));
        provisioning_get_recovery_defaults(cfg);
        return ESP_OK;
    }

    /* Size alone doesn't prove compatibility - a field could change meaning
     * without changing the struct's size. Check the stamp explicitly. */
    if (cfg->magic != GW_CONFIG_MAGIC || cfg->version != GW_CONFIG_VERSION) {
        ESP_LOGW(TAG, "stored config is magic=0x%08" PRIx32 " v%" PRIu32 ", expected 0x%08" PRIx32
                      " v%" PRIu32 " - using recovery defaults",
                 cfg->magic, cfg->version, (uint32_t)GW_CONFIG_MAGIC, (uint32_t)GW_CONFIG_VERSION);
        provisioning_get_recovery_defaults(cfg);
        return ESP_OK;
    }

    /* A blob written by an older build (or hand-edited NVS) could still hold
     * values this build considers unusable; catching that here beats letting
     * esp_wifi fail at AP start and taking the management path down. */
    char reason[96];
    if (provisioning_validate(cfg, reason, sizeof(reason)) != ESP_OK) {
        ESP_LOGW(TAG, "stored config failed validation (%s), using recovery defaults", reason);
        provisioning_get_recovery_defaults(cfg);
    }

    return ESP_OK;
}

esp_err_t provisioning_save(const gw_config_t *cfg)
{
    char reason[96];
    esp_err_t err = provisioning_validate(cfg, reason, sizeof(reason));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "refusing to save invalid config: %s", reason);
        return err;
    }

    /* Written unconditionally so a blob saved by this build always carries
     * this build's stamp, even if the caller's struct came from elsewhere. */
    gw_config_t stamped;
    memcpy(&stamped, cfg, sizeof(stamped));
    stamped.magic = GW_CONFIG_MAGIC;
    stamped.version = GW_CONFIG_VERSION;

    nvs_handle_t handle;
    err = nvs_open(GWCFG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, GWCFG_NVS_KEY, &stamped, sizeof(stamped));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* validate_host_subnet()/subnets_overlap() moved to net_validate.c (review
 * finding F16, design/PROJECT_REVIEW_2026-09-10.md's regression-suite ask) -
 * same logic, unchanged, just relocated so it can be compiled and tested on
 * a host instead of only ever running for the first time on real hardware.
 * See net_validate.h for the full reasoning. */

/* Rejects configs that would brick the device's own management path or that
 * esp_wifi/lwIP would refuse at bring-up. Shared by the console, the web UI,
 * and the NVS load path so all three agree on what "valid" means. On failure
 * writes a human-readable reason into errbuf (shown verbatim in the web UI's
 * error banner and on the console). */
esp_err_t provisioning_validate(const gw_config_t *cfg, char *errbuf, size_t errbuf_len)
{
#define GW_REJECT(...)                                       \
    do {                                                     \
        if (errbuf != NULL && errbuf_len > 0) {              \
            snprintf(errbuf, errbuf_len, __VA_ARGS__);       \
        }                                                    \
        return ESP_ERR_INVALID_ARG;                          \
    } while (0)

    if (cfg->node_id[0] == '\0') {
        GW_REJECT("node_id must not be empty");
    }

    /* An empty uplink SSID is valid: it is how "not configured yet" is
     * represented (gw_uplink_is_configured()), and rejecting it here would
     * make the built-in defaults themselves fail validation on first boot.
     * The remaining uplink checks only apply once one has actually been set. */
    if (gw_uplink_is_configured(&cfg->uplink)) {
        /* HaLow SAE needs a passphrase; open/OWE must not carry one. (No length
         * floor asserted for SAE - unlike WPA2-PSK, SAE does not specify one.) */
        if (cfg->uplink.security == GW_SECURITY_SAE && cfg->uplink.psk[0] == '\0') {
            GW_REJECT("uplink security is SAE but no passphrase is set");
        }
    }

    /* Only meaningful against a GW_ROLE_RELAY's HaLow AP - see the long
     * comment on gw_uplink_config_t.use_static_ip for why a real Pi doesn't
     * need this. Validated whenever it's turned on, regardless of the
     * node's current role, same reasoning as the rest of this function:
     * one validator, shared by console/web UI/NVS load, that doesn't need
     * to know which fields the active role actually reads. */
    /* Tracked across the two subnet blocks below so the client role's static
     * uplink (when set) can be checked against its own SoftAP subnet for
     * overlap - two different physical netifs assigned the same address
     * range breaks NAT/routing between them in a way parsing alone can't
     * catch. Only meaningful once both are known valid. */
    uint32_t uplink_ip_h = 0, uplink_mask_h = 0;
    bool have_uplink_subnet = false;

    if (cfg->uplink.use_static_ip) {
        /* Parsing is necessary but not sufficient - a zero address/netmask
         * makes esp_netif_is_valid_static_ip() false (associates, then sits
         * at "no lease" forever with nothing naming the cause), a
         * non-contiguous mask or a network/broadcast-address IP misconfigures
         * the subnet in ways that fail just as silently, and a gateway
         * outside the resulting subnet means ip_forward_nat_init() makes the
         * uplink the default route to nowhere - see
         * validate_host_subnet()'s own comment. */
        esp_err_t sub_err = validate_host_subnet("uplink static", cfg->uplink.static_ip,
                                                   cfg->uplink.static_netmask, cfg->uplink.static_gateway,
                                                   &uplink_ip_h, &uplink_mask_h, errbuf, errbuf_len);
        if (sub_err != ESP_OK) {
            return sub_err;
        }
        have_uplink_subnet = true;

        /* Unlike the fields above, empty is valid here - it means "no DNS
         * configured", the same state this field has always defaulted to.
         * Only reject a value that was actually supplied but isn't usable. */
        struct in_addr addr;
        if (cfg->uplink.static_dns[0] != '\0' &&
            (inet_aton(cfg->uplink.static_dns, &addr) == 0 || addr.s_addr == 0)) {
            GW_REJECT("uplink static DNS '%s' must be empty or a valid, non-zero address",
                      cfg->uplink.static_dns);
        }
    }

    if (cfg->softap.ssid[0] == '\0') {
        GW_REJECT("local Wi-Fi SSID must not be empty");
    }

    /* Empty means an open AP, which is allowed; anything else must be a
     * legal WPA2 passphrase or esp_wifi refuses to start the AP - and the
     * AP is how this device is managed. */
    size_t ap_psk_len = strlen(cfg->softap.psk);
    if (ap_psk_len > 0 && (ap_psk_len < GW_WPA2_PSK_MIN_LEN || ap_psk_len > GW_WPA2_PSK_MAX_LEN)) {
        GW_REJECT("local Wi-Fi passphrase must be %d-%d characters (or empty for an open network)",
                  GW_WPA2_PSK_MIN_LEN, GW_WPA2_PSK_MAX_LEN);
    }

    if (cfg->softap.channel < GW_SOFTAP_CHANNEL_MIN || cfg->softap.channel > GW_SOFTAP_CHANNEL_MAX) {
        GW_REJECT("local Wi-Fi channel must be %d-%d", GW_SOFTAP_CHANNEL_MIN, GW_SOFTAP_CHANNEL_MAX);
    }

    if (cfg->softap.max_connections > 15) {
        GW_REJECT("max_connections must be 15 or fewer");
    }

    /* Effective SoftAP subnet either way - esp_netif's own compiled-in
     * default (192.168.4.1/255.255.255.0, _g_esp_netif_soft_ap_ip in
     * esp-idf v5.5.1's esp_netif_defaults.c) when no custom one is set, so
     * the overlap check below always has a real subnet to compare against,
     * not just the custom-subnet case. */
    uint32_t softap_ip_h, softap_mask_h;
    if (cfg->softap.use_custom_subnet) {
        esp_err_t sub_err = validate_host_subnet("local Wi-Fi", cfg->softap.ip, cfg->softap.netmask,
                                                   cfg->softap.gateway, &softap_ip_h, &softap_mask_h, errbuf,
                                                   errbuf_len);
        if (sub_err != ESP_OK) {
            return sub_err;
        }
    } else {
        softap_ip_h = ((uint32_t)192 << 24) | (168u << 16) | (4u << 8) | 1u;
        softap_mask_h = 0xFFFFFF00u;
    }

    /* Two different physical netifs (the SoftAP and a statically-addressed
     * uplink) assigned overlapping ranges silently breaks NAT/routing
     * between them - not a parse error either field would catch alone.
     * Only checkable for the client role's own uplink; wifi_uplink (relay
     * role) is DHCP-only and its subnet isn't known at validate time, and
     * halow_ap never coexists with a SoftAP on the same node (mutually
     * exclusive roles) - see gw_config.h's gw_node_role_t. */
    if (have_uplink_subnet && subnets_overlap(uplink_ip_h, uplink_mask_h, softap_ip_h, softap_mask_h)) {
        GW_REJECT("uplink static IP '%s' and the local Wi-Fi subnet overlap - NAT needs them distinct",
                  cfg->uplink.static_ip);
    }

    /* GW_ROLE_RELAY fields. Same "empty means not configured yet" pattern as
     * the HaLow uplink - validated only once an operator has actually set a
     * value, so the built-in defaults (both empty) pass validation too. */
    if (gw_wifi_uplink_is_configured(&cfg->wifi_uplink)) {
        size_t wifi_psk_len = strlen(cfg->wifi_uplink.psk);
        if (wifi_psk_len > 0 && (wifi_psk_len < GW_WPA2_PSK_MIN_LEN || wifi_psk_len > GW_WPA2_PSK_MAX_LEN)) {
            GW_REJECT("Wi-Fi uplink passphrase must be %d-%d characters (or empty for an open network)",
                      GW_WPA2_PSK_MIN_LEN, GW_WPA2_PSK_MAX_LEN);
        }
    }

    if (gw_halow_ap_is_configured(&cfg->halow_ap)) {
        /* "OWE security is not currently supported for AP mode" - mmwlan.h's
         * own mmwlan_ap_enable() docs (v2.11.2-esp32-2). Rejected here rather
         * than silently downgraded, so a bad choice is caught at the point
         * it's made instead of surfacing later as an AP that starts wrong. */
        if (cfg->halow_ap.security == GW_SECURITY_OWE) {
            GW_REJECT("HaLow AP security cannot be OWE (unsupported for AP mode) - use open or sae");
        }
        if (cfg->halow_ap.security == GW_SECURITY_SAE && cfg->halow_ap.psk[0] == '\0') {
            GW_REJECT("HaLow AP security is SAE but no passphrase is set");
        }
        /* Zero is MMWLAN_AP_ARGS_INIT's default, meaning "never actually
         * chosen" here - mmwlan_ap_enable() only auto-configures from 0/0
         * when a STA is concurrently active on the same radio, which a
         * relay's HaLow radio never is (AP mode only - see gw_config.h). An
         * operator must pick a real channel, e.g. via a channel list command
         * backed by downlink_halow_ap_list_channels(). */
        if (cfg->halow_ap.op_class == 0) {
            GW_REJECT("HaLow AP channel must be set explicitly (op_class 0 has no meaning without "
                      "a concurrently active STA)");
        }
        if (cfg->halow_ap.max_stas > 20) { /* MMWLAN_AP_MAX_STAS_LIMIT, mmwlan.h */
            GW_REJECT("HaLow AP max_stas must be 20 or fewer");
        }
        /* No gateway field - this radio's own address terminates the
         * subnet, leaves point directly at it (gw_config.h's own comment on
         * gw_halow_ap_config_t.ip). */
        esp_err_t sub_err = validate_host_subnet("HaLow AP", cfg->halow_ap.ip, cfg->halow_ap.netmask, NULL,
                                                   NULL, NULL, errbuf, errbuf_len);
        if (sub_err != ESP_OK) {
            return sub_err;
        }
    }

    struct in_addr group;
    if (inet_aton(cfg->cot.group, &group) == 0) {
        GW_REJECT("CoT group '%s' is not a valid address", cfg->cot.group);
    }
    /* Must be in 224.0.0.0/4 - a unicast address here would make the relay
     * join a group it can never receive on. */
    if ((ntohl(group.s_addr) & 0xF0000000u) != 0xE0000000u) {
        GW_REJECT("CoT group '%s' is not a multicast address (224.0.0.0/4)", cfg->cot.group);
    }

    if (cfg->cot.port == 0) {
        GW_REJECT("CoT port must be 1-65535");
    }

    /* salt/stored_key have no invalid byte patterns to reject - iterations
     * is the one field that can be silently wrong (e.g. a corrupted or
     * hand-edited NVS blob) in a way that would make every future login fail
     * with no clue why, so it's worth catching here rather than at auth.c's
     * first HMAC attempt. */
    if (cfg->auth.password_set && cfg->auth.iterations == 0) {
        GW_REJECT("auth iterations must be nonzero when a password is set");
    }

    return ESP_OK;

#undef GW_REJECT
}

const char *provisioning_security_name(gw_security_mode_t sec)
{
    static const char *names[] = { "open", "owe", "sae" };
    if ((size_t)sec >= sizeof(names) / sizeof(names[0])) {
        return "open";
    }
    return names[sec];
}

/* provisioning_parse_security() also moved to net_validate.c, same reasoning
 * as validate_host_subnet()/subnets_overlap() above - see net_validate.h. */

const char *provisioning_role_name(gw_node_role_t role)
{
    return role == GW_ROLE_RELAY ? "relay" : "client";
}

gw_node_role_t provisioning_parse_role(const char *s)
{
    return strcmp(s, "relay") == 0 ? GW_ROLE_RELAY : GW_ROLE_CLIENT;
}

static void print_config(const gw_config_t *cfg)
{
    printf("node_id       : %s\n", cfg->node_id);
    printf("role          : %s\n", provisioning_role_name(cfg->role));
    printf("uplink_mgmt   : %s\n", cfg->allow_uplink_management ? "on" : "off");
    if (cfg->role == GW_ROLE_RELAY) {
        printf("wifi_uplink.ssid: %s\n", cfg->wifi_uplink.ssid);
        printf("halow_ap.ssid : %s\n", cfg->halow_ap.ssid);
        printf("halow_ap.security: %s\n", provisioning_security_name(cfg->halow_ap.security));
        printf("halow_ap.chan : op_class %d, s1g_chan_num %u\n", cfg->halow_ap.op_class,
               cfg->halow_ap.s1g_chan_num);
        printf("halow_ap.ip   : %s/%s\n", cfg->halow_ap.ip, cfg->halow_ap.netmask);
    } else {
        printf("uplink.ssid   : %s\n", cfg->uplink.ssid);
        printf("uplink.security: %s\n", provisioning_security_name(cfg->uplink.security));
        if (cfg->uplink.use_static_ip) {
            printf("uplink.static_ip: %s/%s via %s\n", cfg->uplink.static_ip, cfg->uplink.static_netmask,
                   cfg->uplink.static_gateway);
            printf("uplink.static_dns: %s\n",
                   cfg->uplink.static_dns[0] != '\0' ? cfg->uplink.static_dns : "(none configured)");
        }
        printf("softap.ssid   : %s\n", cfg->softap.ssid);
        printf("softap.channel: %u\n", cfg->softap.channel);
        printf("softap.subnet : %s\n",
               cfg->softap.use_custom_subnet ? cfg->softap.ip : "192.168.4.1 (esp-netif default)");
    }
    printf("cot           : %s:%u\n", cfg->cot.group, cfg->cot.port);
}

static int cmd_gwcfg_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_cfg) {
        return 1;
    }
    provisioning_config_lock();
    print_config(s_cfg);
    provisioning_config_unlock();
    return 0;
}

static int cmd_gwcfg_set_node(int argc, char **argv)
{
    if (!s_cfg || argc != 2) {
        printf("usage: gwcfg-set-node <node_id>\n");
        return 1;
    }

    /* Scratch-copy + validate, like every other setter: rejecting here, next
     * to the edit, beats a "not saved: ..." from gwcfg-save minutes later
     * with no hint about which command caused it. */
    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.node_id, argv[1], sizeof(work.node_id));

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    provisioning_config_commit(&work);
    provisioning_config_unlock();
    return 0;
}

static int cmd_gwcfg_set_uplink(int argc, char **argv)
{
    if (!s_cfg || argc < 4) {
        printf("usage: gwcfg-set-uplink <ssid> <psk|-> <open|owe|sae>\n");
        return 1;
    }

    /* Validated against a scratch copy so a rejected value never lands in
     * the live config - same discipline as gwcfg-set-softap and the web
     * UI's POST handler. Catches e.g. SAE with no passphrase immediately
     * instead of at gwcfg-save time. */
    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.uplink.ssid, argv[1], sizeof(work.uplink.ssid));
    strlcpy(work.uplink.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(work.uplink.psk));
    if (!provisioning_parse_security(argv[3], &work.uplink.security)) {
        provisioning_config_unlock();
        printf("rejected: security mode '%s' not recognized (use open, owe or sae)\n", argv[3]);
        return 1;
    }

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    provisioning_config_commit(&work);
    provisioning_config_unlock();
    printf("uplink config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

static int cmd_gwcfg_set_softap(int argc, char **argv)
{
    if (!s_cfg || argc < 3) {
        printf("usage: gwcfg-set-softap <ssid> <psk|-> [channel]\n");
        return 1;
    }

    /* Validated against a scratch copy so a rejected value never lands in
     * the live config - the same discipline the web UI's POST handler uses. */
    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.softap.ssid, argv[1], sizeof(work.softap.ssid));
    strlcpy(work.softap.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(work.softap.psk));
    if (argc >= 4) {
        work.softap.channel = (uint8_t)atoi(argv[3]);
    }

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    provisioning_config_commit(&work);
    provisioning_config_unlock();
    printf("softap config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

static int cmd_gwcfg_set_role(int argc, char **argv)
{
    if (!s_cfg || argc != 2) {
        printf("usage: gwcfg-set-role <client|relay>\n");
        return 1;
    }

    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    work.role = provisioning_parse_role(argv[1]);

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    provisioning_config_commit(&work);
    provisioning_config_unlock();
    printf("role set to '%s' in RAM; run 'gwcfg-save' then reboot to apply - it changes which "
           "radios/netifs come up entirely, not just a config value\n",
           provisioning_role_name(work.role));
    return 0;
}

static int cmd_gwcfg_set_uplink_mgmt(int argc, char **argv)
{
    if (!s_cfg || argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        printf("usage: gwcfg-set-uplink-mgmt <on|off>\n");
        return 1;
    }

    provisioning_config_lock();
    s_cfg->allow_uplink_management = (strcmp(argv[1], "on") == 0);
    bool now_on = s_cfg->allow_uplink_management;
    provisioning_config_unlock();

    printf("uplink management %s in RAM; run 'gwcfg-save' then reboot to apply.%s\n",
           now_on ? "enabled" : "disabled",
           now_on ? " The config UI becomes reachable from whatever network this node's "
                    "uplink joins - only do this for an uplink network you trust, "
                    "there is still no login on the config UI itself."
                  : "");
    return 0;
}

/* Separate from gwcfg-set-uplink deliberately: this only matters when the
 * uplink is a GW_ROLE_RELAY's HaLow AP rather than a real Pi (see the long
 * comment on gw_uplink_config_t.use_static_ip), so it's an edge case worth
 * keeping out of the common command's argument list. */
static int cmd_gwcfg_set_uplink_static_ip(int argc, char **argv)
{
    if (!s_cfg || (argc != 2 && argc != 4 && argc != 5)) {
        printf("usage: gwcfg-set-uplink-static-ip -              (use DHCP, the default)\n"
               "       gwcfg-set-uplink-static-ip <ip> <gateway> <netmask> [dns]\n"
               "  [dns] matters most when associating to a GW_ROLE_RELAY node's HaLow AP -\n"
               "  point it at that relay's own downlink address (e.g. 172.16.60.1); the\n"
               "  relay forwards queries from there using its own real upstream DNS server.\n");
        return 1;
    }

    provisioning_config_lock();
    gw_config_t work = *s_cfg;

    if (argc == 2 && strcmp(argv[1], "-") == 0) {
        work.uplink.use_static_ip = false;
        work.uplink.static_ip[0] = '\0';
        work.uplink.static_gateway[0] = '\0';
        work.uplink.static_netmask[0] = '\0';
        work.uplink.static_dns[0] = '\0';
    } else if (argc == 4 || argc == 5) {
        work.uplink.use_static_ip = true;
        strlcpy(work.uplink.static_ip, argv[1], sizeof(work.uplink.static_ip));
        strlcpy(work.uplink.static_gateway, argv[2], sizeof(work.uplink.static_gateway));
        strlcpy(work.uplink.static_netmask, argv[3], sizeof(work.uplink.static_netmask));
        strlcpy(work.uplink.static_dns, argc == 5 ? argv[4] : "", sizeof(work.uplink.static_dns));
    } else {
        provisioning_config_unlock();
        printf("usage: gwcfg-set-uplink-static-ip -              (use DHCP, the default)\n"
               "       gwcfg-set-uplink-static-ip <ip> <gateway> <netmask> [dns]\n");
        return 1;
    }

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    provisioning_config_commit(&work);
    provisioning_config_unlock();
    printf("uplink static-IP config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

static int cmd_gwcfg_set_wifi_uplink(int argc, char **argv)
{
    if (!s_cfg || argc != 3) {
        printf("usage: gwcfg-set-wifi-uplink <ssid> <psk|->\n"
               "  (GW_ROLE_RELAY only - the native 2.4GHz uplink to the Pi's local AP)\n");
        return 1;
    }

    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.wifi_uplink.ssid, argv[1], sizeof(work.wifi_uplink.ssid));
    strlcpy(work.wifi_uplink.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(work.wifi_uplink.psk));

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    provisioning_config_commit(&work);
    provisioning_config_unlock();
    printf("Wi-Fi uplink config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

static int cmd_gwcfg_set_halow_ap(int argc, char **argv)
{
    if (!s_cfg || argc != 6) {
        printf("usage: gwcfg-set-halow-ap <ssid> <psk|-> <open|sae> <op_class> <s1g_chan_num>\n"
               "  (GW_ROLE_RELAY only - the HaLow AP leaf XIAOs associate to)\n"
               "  run 'gwcfg-list-halow-channels' first to see legal (op_class, s1g_chan_num) pairs\n");
        return 1;
    }

    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.halow_ap.ssid, argv[1], sizeof(work.halow_ap.ssid));
    strlcpy(work.halow_ap.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(work.halow_ap.psk));
    /* AP mode doesn't support OWE (mmwlan.h) - reusing provisioning_parse_security()
     * here means "owe" is parseable but provisioning_validate() below rejects it,
     * same as any other bad value, rather than silently mapping it to open. */
    if (!provisioning_parse_security(argv[3], &work.halow_ap.security)) {
        provisioning_config_unlock();
        printf("rejected: security mode '%s' not recognized (use open or sae)\n", argv[3]);
        return 1;
    }
    work.halow_ap.op_class = (int16_t)atoi(argv[4]);
    work.halow_ap.s1g_chan_num = (uint8_t)atoi(argv[5]);

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    provisioning_config_commit(&work);
    provisioning_config_unlock();
    printf("HaLow AP config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

/* Bring-up commands. These don't touch config at all - they exist because the
 * serial console is the one interface guaranteed to work when the SoftAP
 * hasn't come up, and the questions they answer ("does the radio respond?",
 * "is the AP visible?", "how strong?") are the first three asked of a node
 * that isn't associating. */
static int cmd_gwcfg_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const esp_app_desc_t *desc = esp_app_get_description();
    printf("firmware ver  : %s\n", desc ? desc->version : "unknown");

    gw_node_role_t role = s_cfg ? s_cfg->role : GW_ROLE_CLIENT;
    printf("role          : %s\n", provisioning_role_name(role));

    esp_netif_t *uplink_netif;
    if (role == GW_ROLE_RELAY) {
        printf("wifi uplink   : %s\n", uplink_wifi_link_state_name(uplink_wifi_get_link_state()));
        int8_t rssi = link_history_get_latest_wifi_rssi();
        if (rssi == INT8_MIN) {
            printf("wifi RSSI     : (not associated)\n");
        } else {
            printf("wifi RSSI     : %" PRId8 " dBm\n", rssi);
        }
        printf("halow ap      : %s\n", downlink_halow_ap_is_started() ? "started (best-effort - "
                                                                          "mmhalow_wifi_start() has no "
                                                                          "return code)"
                                                                       : "not started");
        printf("halow ap stas : %u\n", downlink_halow_ap_get_sta_count());
        uplink_netif = uplink_wifi_get_netif();
    } else {
        uplink_link_state_t state = uplink_halow_get_link_state();
        printf("uplink state  : %s\n", uplink_halow_link_state_name(state));

        int32_t rssi = link_history_get_latest_halow_rssi();
        if (rssi == INT32_MIN) {
            printf("uplink RSSI   : (not associated)\n");
        } else {
            printf("uplink RSSI   : %" PRId32 " dBm\n", rssi);
        }
        uplink_netif = uplink_halow_get_netif();
    }

    esp_netif_ip_info_t ip_info;
    if (uplink_netif != NULL && esp_netif_get_ip_info(uplink_netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        printf("uplink IP     : " IPSTR "\n", IP2STR(&ip_info.ip));
        printf("uplink gw     : " IPSTR "\n", IP2STR(&ip_info.gw));
    } else {
        printf("uplink IP     : (no lease)\n");
    }

    printf("cot relay     : %s\n", cot_relay_is_running() ? "running" : "not started");
    if (cot_relay_is_running()) {
        cot_relay_counters_t uplink_side, downlink_side;
        cot_relay_get_counters(&uplink_side, &downlink_side);
        printf("cot uplink    : rx %" PRIu32 " pkt (%" PRIu64 " B)  tx %" PRIu32 " pkt (%" PRIu64
               " B)\n",
               uplink_side.rx_packets, uplink_side.rx_bytes, uplink_side.tx_packets,
               uplink_side.tx_bytes);
        printf("cot downlink  : rx %" PRIu32 " pkt (%" PRIu64 " B)  tx %" PRIu32 " pkt (%" PRIu64
               " B)\n",
               downlink_side.rx_packets, downlink_side.rx_bytes, downlink_side.tx_packets,
               downlink_side.tx_bytes);
        printf("cot wrong-dest drops: %" PRIu32 "\n", cot_relay_get_wrong_dest_drops());
    }
    printf("country code  : %s (build-time, not settable here)\n", CONFIG_HALOW_COUNTRY_CODE);
    printf("free heap     : %u bytes\n", (unsigned)esp_get_free_heap_size());
    /* Internal SRAM alone - see heap_guard.h for why this is worth printing
     * separately from the line above once CONFIG_SPIRAM_USE_MALLOC=y makes
     * that figure a combined internal+PSRAM number. */
    printf("free heap (internal): %u bytes\n", (unsigned)esp_get_free_internal_heap_size());
    return 0;
}

/* Bench-test helper: fires a burst of small datagrams into the CoT multicast
 * group via cot_relay_inject() - the same "generic send primitive the
 * self-beacon will need" cot_relay.h already documents as existing for
 * exactly this kind of caller, just not yet consumed by anything. Sends on
 * *this* node's own interfaces (both uplink and downlink), so run it on
 * whichever node is upstream of the link being tested and read the loss off
 * the *other* node's `cot uplink`/`cot downlink` rx counters above -
 * cot_relay_inject() itself has no delivery confirmation (it's UDP
 * multicast), so "sent" here only means the socket call succeeded, not that
 * it arrived. Blocks the console for roughly count*interval_ms - deliberate,
 * this is an attended bench command, not something a script should loop on a
 * timer. */
static int cmd_gwcfg_cot_test(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: gwcfg-cot-test <count> <interval_ms>\n"
               "  Injects <count> small test datagrams into the CoT multicast group,\n"
               "  <interval_ms> apart, via cot_relay_inject(). Read the loss off the\n"
               "  *other* node's 'cot uplink'/'cot downlink' rx counters (gwcfg-status).\n");
        return 1;
    }
    if (!cot_relay_is_running()) {
        printf("CoT relay not running on this node - nothing to inject through\n");
        return 1;
    }

    int count = atoi(argv[1]);
    int interval_ms = atoi(argv[2]);
    if (count <= 0 || count > 10000 || interval_ms < 0) {
        printf("count must be 1-10000, interval_ms must be >= 0\n");
        return 1;
    }

    static const char payload[] = "GWCFG-COT-TEST-PACKET";
    int sent = 0;
    for (int i = 0; i < count; i++) {
        if (cot_relay_inject(payload, sizeof(payload) - 1) == ESP_OK) {
            sent++;
        }
        if (interval_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
        }
    }
    printf("injected %d/%d test datagrams (socket-accepted, not delivery-confirmed)\n", sent, count);
    return 0;
}

static int cmd_gwcfg_radio(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!uplink_halow_is_ready()) {
        printf("radio not initialized - check CONFIG_MM_* pins/BCF (design/HARDWARE.md)\n");
        return 1;
    }
    uplink_halow_log_radio_info();
    return 0;
}

static void task_stack_print_cb(const task_stack_info_t *info, void *ctx)
{
    unsigned *rows = (unsigned *)ctx;
    (*rows)++;

    if (!info->present) {
        printf("%-17s %8s  %8s  %6s  %s\n", info->name, "-", "-", "-", "not running");
        return;
    }

    /* Integer percentage, and of *used* rather than free, so the number grows
     * as the situation worsens - the same reason the frequency column below
     * avoids %f: this table is read during bring-up, when a misread costs
     * hardware time. Rounded up, so a task that has touched any of its stack
     * at all never reports 0% used. */
    size_t used = info->stack_total - info->stack_free_min;
    unsigned pct = info->stack_total ? (unsigned)((used * 100 + info->stack_total - 1) / info->stack_total) : 0;

    /* Flagged, not just printed. A bare column of numbers requires the reader
     * to already know what "352 bytes free" means for a task they didn't
     * create; these thresholds say it outright. */
    const char *note = "";
    if (info->stack_free_min < 256) {
        note = "  <-- CRITICAL, will overflow";
    } else if (info->stack_free_min < 512) {
        note = "  <-- tight, raise it";
    }

    printf("%-17s %8u  %8u  %5u%%  %s\n", info->name, (unsigned)info->stack_total,
           (unsigned)info->stack_free_min, pct, note);
}

/* Reports how close each task has come to overflowing its stack.
 *
 * Exists because this firmware has already lost a node to a stack overflow
 * that nothing visible predicted (design/ROADMAP.md item 8): the failure is
 * silent right up to the panic, and the margin is not inferable from reading
 * the code. This is the instrument that makes it a number instead of a guess.
 *
 * Worth running after the node has been up a while and through a reconnect or
 * two, not just at boot - the mark is a high-water measurement, so it only
 * reflects paths that have actually executed. A task that has never yet taken
 * its deepest branch will flatter itself here. */
static int cmd_gwcfg_tasks(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    unsigned rows = 0;
    printf("%-17s %8s  %8s  %6s\n", "TASK", "STACK", "FREE", "USED");
    size_t found = task_stats_each_stack(task_stack_print_cb, &rows);
    printf("%u of %u watched tasks running. FREE is the least this task has "
           "ever had spare, in bytes.\n",
           (unsigned)found, rows);
    return 0;
}

static void scan_print_cb(const uplink_scan_result_t *result, void *ctx)
{
    unsigned *n = (unsigned *)ctx;
    (*n)++;
    /* Frequency is formatted with integer arithmetic rather than %f on a
     * double. CONFIG_LIBC_NEWLIB_NANO_FORMAT is off in this build so %f would
     * work today, but it is exactly the kind of knob someone reaches for to
     * shrink a binary later - and the failure mode is a scan table that prints
     * garbage frequencies during bring-up, which is when it's least welcome. */
    unsigned mhz = (unsigned)(result->freq_hz / 1000000u);
    unsigned khz = (unsigned)((result->freq_hz % 1000000u) / 1000u);
    printf("%-32s %02x:%02x:%02x:%02x:%02x:%02x  %4d dBm  %4u.%03u MHz  %2u MHz\n",
           result->ssid[0] ? result->ssid : "(hidden)", result->bssid[0], result->bssid[1],
           result->bssid[2], result->bssid[3], result->bssid[4], result->bssid[5], result->rssi,
           mhz, khz, result->bw_mhz);
}

static int cmd_gwcfg_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!uplink_halow_is_ready()) {
        printf("radio not initialized - nothing to scan with\n");
        return 1;
    }

    printf("scanning (regulatory domain %s)...\n", CONFIG_HALOW_COUNTRY_CODE);
    printf("%-32s %-17s  %8s  %11s  %s\n", "SSID", "BSSID", "RSSI", "FREQ", "BW");

    unsigned found = 0;
    esp_err_t err = uplink_halow_scan(scan_print_cb, &found, GWCFG_SCAN_TIMEOUT_MS);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        printf("scan failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("%u AP(s) found%s\n", found, err == ESP_ERR_TIMEOUT ? " (scan timed out, partial)" : "");
    if (found == 0) {
        /* Said explicitly because the obvious conclusion ("the AP is off") is
         * only one of two, and the other one is a build-time setting that
         * can't be fixed from this console - or from a different build, since
         * this hardware is 902-928 MHz only (design/HARDWARE.md "Regulatory
         * domain"). The fix is on the Pi, so point there. */
        printf("note: only channels legal in '%s' were scanned. An AP on a channel outside\n"
               "      this regulatory domain is invisible here - this board is 902-928 MHz\n"
               "      only, so the Pi's HaLow radio has to be on '%s' too.\n",
               CONFIG_HALOW_COUNTRY_CODE, CONFIG_HALOW_COUNTRY_CODE);
    }
    return 0;
}

static void channel_print_cb(const halow_ap_channel_t *chan, void *ctx)
{
    unsigned *n = (unsigned *)ctx;
    (*n)++;
    unsigned mhz = (unsigned)(chan->freq_hz / 1000000u);
    unsigned khz = (unsigned)((chan->freq_hz % 1000000u) / 1000u);
    printf("op_class %-4d  s1g_chan_num %-4u  %4u.%03u MHz  %2u MHz\n", chan->op_class,
           chan->s1g_chan_num, mhz, khz, chan->bw_mhz);
}

/* GW_ROLE_RELAY only: mmwlan_ap_args wants an (op_class, s1g_chan_num) pair,
 * not a frequency - this exists so gwcfg-set-halow-ap's arguments can be
 * chosen from a real table instead of guessed. Pure table walk against
 * CONFIG_HALOW_COUNTRY_CODE's already-loaded regulatory domain, no radio
 * activity - works even before downlink_halow_ap_init() has run. */
static int cmd_gwcfg_list_halow_channels(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("HaLow channels legal in regulatory domain '%s':\n", CONFIG_HALOW_COUNTRY_CODE);
    unsigned found = 0;
    downlink_halow_ap_list_channels(channel_print_cb, &found);
    printf("%u channel(s)\n", found);
    return 0;
}

static int cmd_gwcfg_save(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_cfg) {
        return 1;
    }

    provisioning_config_lock();
    char reason[96];
    esp_err_t err = provisioning_validate(s_cfg, reason, sizeof(reason));
    if (err == ESP_OK) {
        err = provisioning_save(s_cfg);
    }
    provisioning_config_unlock();

    if (err == ESP_ERR_INVALID_ARG) {
        printf("not saved: %s\n", reason);
    } else {
        printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

static int cmd_gwcfg_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_cfg) {
        return 1;
    }
    provisioning_config_lock();
    provisioning_get_defaults(s_cfg);
    provisioning_config_unlock();
    printf("reset to defaults in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

/* Deliberately diverges from cmd_gwcfg_reset()'s "edit in RAM, operator runs
 * gwcfg-save then reboots" convention: this is a physically-present recovery
 * action for someone locked out right now (design/ROADMAP.md item 1's
 * settled "Recovery" decision), not a routine config edit. Saving and
 * dropping sessions immediately means the lockout is actually gone the
 * moment this command returns, not after a save-then-reboot an operator
 * might forget mid-recovery. */
static int cmd_gwcfg_reset_auth(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_cfg) {
        return 1;
    }
    provisioning_config_lock();
    memset(&s_cfg->auth, 0, sizeof(s_cfg->auth)); /* password_set=false - forces first-use flow again */
    esp_err_t err = provisioning_save(s_cfg);
    provisioning_config_unlock();
    auth_drop_all_sessions(); /* immediate effect - no reboot required */
    if (err == ESP_OK) {
        printf("auth credential cleared and saved; sessions dropped - no reboot needed\n");
    } else {
        printf("failed to save: %s\n", esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

/* The one channel an operator has to verify the web UI's self-signed
 * certificate against before trusting a browser warning - same role an SSH
 * host key fingerprint plays, since no CA can issue one for a private IP
 * (review finding F02, design/PROJECT_REVIEW_2026-09-10.md). Also logged
 * once at boot by tls_identity_init() (main/app_main.c), so this command is
 * for re-reading it later, not the only place it's ever shown. */
static int cmd_gwcfg_show_cert(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!tls_identity_is_ready()) {
        printf("no HTTPS identity yet - this shouldn't happen after a successful boot\n");
        return 1;
    }
    char fp[TLS_IDENTITY_FINGERPRINT_HEX_LEN + 1];
    tls_identity_get_fingerprint_hex(fp, sizeof(fp));
    printf("certificate SHA-256 fingerprint: %s\n", fp);
    printf("compare this against what your browser shows before accepting its warning\n");
    return 0;
}

/* Recovery path for a suspected-compromised device key, without a full
 * factory reset (which would also drop the admin credential, radio
 * passphrases and every other setting). Same "physically-present recovery
 * action, immediate effect, no reboot" shape as gwcfg-reset-auth above -
 * tls_identity_regenerate() does its own locking and provisioning_save(). */
static int cmd_gwcfg_reset_tls_identity(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_cfg) {
        return 1;
    }
    esp_err_t err = tls_identity_regenerate(s_cfg);
    if (err != ESP_OK) {
        printf("failed to regenerate HTTPS identity: %s\n", esp_err_to_name(err));
        return 1;
    }
    char fp[TLS_IDENTITY_FINGERPRINT_HEX_LEN + 1];
    tls_identity_get_fingerprint_hex(fp, sizeof(fp));
    printf("new HTTPS identity generated and saved - new fingerprint: %s\n", fp);
    printf("every browser that already trusted the old certificate will show a fresh warning "
           "next visit - that is expected, verify the new fingerprint before accepting it\n");
    return 0;
}

/* The one channel a physically-present operator has to claim first
 * ownership of an unclaimed device (review finding F03,
 * design/PROJECT_REVIEW_2026-09-10.md) - the web UI's first-use form asks
 * for this alongside the new password, and it must match before
 * POST /api/auth/password will commit one. Also logged once at boot by
 * auth_init() (main/app_main.c) while onboarding is open, so this command
 * is for re-reading it later or after a reboot, not the only place it's
 * ever shown. */
static int cmd_gwcfg_show_setup_secret(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char secret[AUTH_SETUP_SECRET_HEX_LEN + 1];
    if (!auth_get_setup_secret_hex(secret, sizeof(secret))) {
        printf(auth_password_is_set() ? "already claimed - no setup code to show\n"
                                       : "onboarding is closed - use gwcfg-reopen-onboarding to claim this "
                                         "device\n");
        return 1;
    }
    printf("device setup code: %s\n", secret);
    printf("present this in the web UI's first-use form to claim admin ownership\n");
    return 0;
}

/* Recovery path for an operator who let the onboarding boot budget run out
 * (AUTH_ONBOARDING_BOOT_BUDGET, main/auth.c) before claiming the device -
 * without this, a closed window would need a full factory reset (which also
 * drops the admin credential slot, radio passphrases and every other
 * setting - nothing to drop here since none of that exists yet on an
 * unclaimed device, but it's still the wrong-sized tool for "I just need a
 * fresh setup code"). Same "physically-present recovery action, immediate
 * effect, no reboot" shape as gwcfg-reset-auth/gwcfg-reset-tls-identity
 * above - auth_reopen_onboarding() does its own locking and
 * provisioning_save(). */
static int cmd_gwcfg_reopen_onboarding(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = auth_reopen_onboarding();
    if (err == ESP_ERR_INVALID_STATE) {
        printf("already claimed - nothing to reopen\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("failed to reopen onboarding: %s\n", esp_err_to_name(err));
        return 1;
    }
    char secret[AUTH_SETUP_SECRET_HEX_LEN + 1];
    auth_get_setup_secret_hex(secret, sizeof(secret));
    printf("onboarding reopened - new setup code: %s\n", secret);
    return 0;
}

esp_err_t provisioning_register_console_commands(gw_config_t *cfg)
{
    s_cfg = cfg;

    const esp_console_cmd_t cmds[] = {
        { .command = "gwcfg-show", .help = "Show current gateway config", .hint = NULL, .func = &cmd_gwcfg_show },
        { .command = "gwcfg-set-node", .help = "Set node id: gwcfg-set-node <id>", .hint = NULL, .func = &cmd_gwcfg_set_node },
        { .command = "gwcfg-set-uplink", .help = "Set HaLow uplink STA config", .hint = NULL, .func = &cmd_gwcfg_set_uplink },
        { .command = "gwcfg-set-softap", .help = "Set local SoftAP config", .hint = NULL, .func = &cmd_gwcfg_set_softap },
        { .command = "gwcfg-set-role", .help = "Set node role: gwcfg-set-role <client|relay>", .hint = NULL, .func = &cmd_gwcfg_set_role },
        { .command = "gwcfg-set-uplink-mgmt", .help = "Allow/deny reaching the config UI from this node's own uplink: gwcfg-set-uplink-mgmt <on|off>", .hint = NULL, .func = &cmd_gwcfg_set_uplink_mgmt },
        { .command = "gwcfg-set-uplink-static-ip", .help = "Set/clear a static IP on the uplink (relay leaves only)", .hint = NULL, .func = &cmd_gwcfg_set_uplink_static_ip },
        { .command = "gwcfg-set-wifi-uplink", .help = "Set the relay role's native Wi-Fi uplink to the Pi", .hint = NULL, .func = &cmd_gwcfg_set_wifi_uplink },
        { .command = "gwcfg-set-halow-ap", .help = "Set the relay role's HaLow AP downlink", .hint = NULL, .func = &cmd_gwcfg_set_halow_ap },
        { .command = "gwcfg-save", .help = "Persist current config to NVS", .hint = NULL, .func = &cmd_gwcfg_save },
        { .command = "gwcfg-reset", .help = "Reset in-RAM config to built-in defaults", .hint = NULL, .func = &cmd_gwcfg_reset },
        { .command = "gwcfg-reset-auth", .help = "Clear the web UI admin credential and drop sessions immediately (no reboot)", .hint = NULL, .func = &cmd_gwcfg_reset_auth },
        { .command = "gwcfg-show-cert", .help = "Show the web UI's HTTPS certificate fingerprint - verify against your browser before trusting it", .hint = NULL, .func = &cmd_gwcfg_show_cert },
        { .command = "gwcfg-reset-tls-identity", .help = "Generate a fresh HTTPS certificate/key immediately (no reboot) - for a suspected-compromised key", .hint = NULL, .func = &cmd_gwcfg_reset_tls_identity },
        { .command = "gwcfg-show-setup-secret", .help = "Show the setup code needed to claim first admin ownership of this device", .hint = NULL, .func = &cmd_gwcfg_show_setup_secret },
        { .command = "gwcfg-reopen-onboarding", .help = "Reopen the first-ownership claim window with a fresh setup code (no reboot) - for a timed-out window", .hint = NULL, .func = &cmd_gwcfg_reopen_onboarding },
        { .command = "gwcfg-status", .help = "Show live uplink/relay state, RSSI and IPs", .hint = NULL, .func = &cmd_gwcfg_status },
        { .command = "gwcfg-cot-test", .help = "Bench test: inject <count> CoT datagrams <interval_ms> apart; read loss off the other node's counters", .hint = NULL, .func = &cmd_gwcfg_cot_test },
        { .command = "gwcfg-scan", .help = "Scan for HaLow APs on this build's channel list", .hint = NULL, .func = &cmd_gwcfg_scan },
        { .command = "gwcfg-list-halow-channels", .help = "List legal (op_class, s1g_chan_num) pairs for gwcfg-set-halow-ap", .hint = NULL, .func = &cmd_gwcfg_list_halow_channels },
        { .command = "gwcfg-radio", .help = "Print HaLow BCF/firmware versions (proves SPI works)", .hint = NULL, .func = &cmd_gwcfg_radio },
        { .command = "gwcfg-tasks", .help = "Show worst-case stack headroom per task", .hint = NULL, .func = &cmd_gwcfg_tasks },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t provisioning_start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "xiao-gw>";

    esp_err_t err;
    /* Which peripheral esp_console attaches to must match sdkconfig's
     * ESP_CONSOLE_* choice (sdkconfig.defaults sets USB_SERIAL_JTAG for the
     * XIAO S3's native USB port) - mirrors ESP-IDF's own console example. */
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl);
#else
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&hw_config, &repl_config, &repl);
#endif
    if (err != ESP_OK) {
        return err;
    }

    return esp_console_start_repl(repl);
}
