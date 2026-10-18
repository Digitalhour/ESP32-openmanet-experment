#include <inttypes.h>
#include <string.h>

#include "uplink_halow.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "mmhalow.h"

#include "radio_control.h"
#include "task_stats.h"

static const char *TAG = "uplink_halow";

#define RECONNECT_BACKOFF_MIN_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 60000
#define HALOW_CONNECT_TIMEOUT_MS 15000
#define LINK_POLL_INTERVAL_MS    2000

/* How long to wait for a DHCP lease after 802.11 association before
 * concluding the lease isn't coming. Association succeeding while DHCP never
 * completes is a realistic failure (no/misconfigured DHCP server on the Pi
 * side), and without a bound here the task would sit in its "waiting for
 * lease" poll loop forever, never retrying and never reporting anything. */
#define DHCP_LEASE_TIMEOUT_MS 30000

/* On the first timeout the DHCP client is restarted in place, which fixes
 * the transient cases cheaply. If a lease still doesn't arrive, fall all the
 * way back to re-associating. */
#define DHCP_RESTART_ATTEMPTS 2

static gw_uplink_config_t s_cfg;
static esp_netif_t *s_netif = NULL;
static bool s_ready = false;               /* uplink_halow_init() succeeded */
static bool s_configured = false;          /* an uplink SSID was actually provisioned */
static volatile bool s_associated = false; /* 802.11 association only */
static volatile bool s_associating = false;
static volatile bool s_has_ip = false;     /* the signal callers actually want */
static uplink_halow_state_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static TaskHandle_t s_reconnect_task_handle = NULL;
static SemaphoreHandle_t s_connect_sem = NULL;

/* Serializes uplink_halow_scan(): the driver takes one scan request at a time,
 * and scan_rx_cb() below writes into a single shared context.
 *
 * That context and its completion semaphore are deliberately static rather
 * than stack-allocated in uplink_halow_scan(). A scan that times out leaves
 * the driver still holding the `cb_arg` pointer we handed it, and a late
 * callback writing through a pointer to a returned stack frame - or to a
 * semaphore we deleted - is a use-after-free. Keeping the storage alive for
 * the life of the process makes a late callback harmless, and the generation
 * counter makes it a no-op rather than a result misattributed to whatever scan
 * is running by then. */
static SemaphoreHandle_t s_scan_lock = NULL;
static SemaphoreHandle_t s_scan_done = NULL;

/* Caps how many results one scan keeps. Review finding F07
 * (design/PROJECT_REVIEW_2026-09-10.md) - see this file's own explanation
 * below of why results are buffered here rather than delivered live. 32 is
 * generous for this radio's own regulatory channel set in a bench/field
 * deployment (a handful of HaLow APs, not a dense 2.4GHz venue); anything
 * beyond it is counted, not silently dropped - see s_scan.overflow. */
#define UPLINK_HALOW_SCAN_MAX_RESULTS 32

/* result/count/generation are written only by scan_rx_cb() (the driver's own
 * scan/event task) and read only by uplink_halow_scan() (whichever task
 * called it) after that task's own wait on s_scan_done has already returned -
 * so despite living on two different tasks, there is no point where both
 * sides touch this struct at once: uplink_halow_scan() bumps `generation`
 * (retiring the one scan_rx_cb() was just using) before it reads anything
 * back out of `result`, and a scan_rx_cb() that already passed the generation
 * check before that bump lands is, at worst, writing into a slot this task is
 * about to stop reading past - never a slot it has already delivered.
 *
 * `cb`/`ctx` deliberately do NOT live here (they used to - see git history).
 * Review finding F07 flagged exactly that: scan_rx_cb() could pass its
 * generation check, then be preempted while uplink_halow_scan() timed out and
 * returned to a caller that went on to free/reuse `ctx` (web_ui.c's
 * scan_post_handler(), for one, deletes the cJSON tree `ctx` points into
 * right after this function returns) - the callback would then resume and
 * call cb(ctx) through memory that no longer belongs to it. Checking
 * generation once doesn't close that window; nothing about it stops a
 * request that already passed the check from running long after its owner
 * gave up. Fixed by never letting scan_rx_cb() touch the caller's cb/ctx at
 * all: it only ever writes into this module's own static `result` array, and
 * only uplink_halow_scan() itself - on the caller's own task, synchronously,
 * after the wait below has already returned - walks that array and invokes
 * cb(ctx). cb/ctx are plain local variables in that function from here on,
 * never shared across tasks, so there is nothing left for a late callback to
 * race against. */
static struct scan_ctx {
    uplink_scan_result_t result[UPLINK_HALOW_SCAN_MAX_RESULTS];
    uint32_t count;    /* total APs seen this scan, including any beyond capacity */
    uint32_t generation;
} s_scan;

/* Async scan job state - see uplink_halow_scan_start()/_poll()'s own comments
 * in uplink_halow.h. s_scan_state and s_scan_deadline_us are written only by
 * uplink_halow_scan_start() (RUNNING + a fresh deadline) and
 * uplink_halow_scan_poll() (back to IDLE, once finalized) - both always run
 * on whichever task called them, serialized against each other the same way
 * uplink_halow_scan() itself always was: s_scan_lock is held for the whole
 * RUNNING window, so a second _start() while one is already in flight fails
 * with ESP_ERR_INVALID_STATE exactly like a concurrent uplink_halow_scan()
 * call always has.
 *
 * s_scan_cache is separate from s_scan.result above on purpose: s_scan.result
 * is scratch space scan_rx_cb() is still writing into for as long as
 * s_scan_state is RUNNING, but uplink_halow_scan_get_cached_results() must
 * stay safely readable by an HTTP task at any time, including while a new
 * scan is mid-flight - see that function's own "last finalized scan" comment
 * in the header. Finalizing (in uplink_halow_scan_poll()) copies out of
 * s_scan.result into here before releasing s_scan_lock, at the same point
 * the old blocking uplink_halow_scan() used to deliver results via cb() -
 * after that copy, s_scan.result is free for the next scan to overwrite
 * without disturbing what a poller reads back out of the cache. */
static volatile uplink_halow_scan_state_t s_scan_state = UPLINK_HALOW_SCAN_IDLE;
static int64_t s_scan_deadline_us;
static struct {
    bool has_result; /* false until the first scan ever finalizes */
    bool complete;   /* true = finished before its deadline, false = timed out */
    uplink_scan_result_t result[UPLINK_HALOW_SCAN_MAX_RESULTS];
    uint32_t count;
} s_scan_cache;

/* --- radio_control glue (review finding F06, design/PROJECT_REVIEW_2026-09-10.md) ---
 *
 * Every non-TX mmwlan_ and mmhalow_ call in this file now goes through
 * radio_control_run() instead of calling the driver directly - see
 * main/radio_control.h for why. Every ctx below is static, never a stack
 * local, same reasoning s_scan above already documents; where a call needs
 * input (mmhalow_set_config()'s config struct), that input is copied into
 * the static ctx by value before submitting, not passed as a pointer to
 * the caller's own stack - a pointer into a caller's frame is exactly what
 * a timed-out request must never let the owner task dereference later.
 *
 * get_rssi is reachable from more than one task (the reconnect task, the
 * console, and the web UI) and shares one static result struct across all
 * of them, so it gets its own mutex (s_rssi_lock) around that struct, on
 * top of radio_control's own single-owner guarantee for the driver call
 * itself - the same division of responsibility s_scan_lock above already
 * has: radio_control serializes the *call*, this file's own lock serializes
 * *callers reusing the same result storage*. print_version_info is also
 * multi-caller but has no result to share (it only logs), so it needs no
 * such lock. connect/disconnect/init/set_config have exactly one caller
 * each (the boot sequence or the reconnect task) and don't need one either. */

static void mm_sta_state_cb(enum mmwlan_sta_state sta_state); /* defined below */

typedef struct {
    esp_err_t result;
} radio_err_ctx_t;

static radio_err_ctx_t s_init_ctx;
static void do_mmhalow_init(void *arg)
{
    ((radio_err_ctx_t *)arg)->result = mmhalow_init(NULL);
}

static radio_err_ctx_t s_connect_ctx;
static void do_mmhalow_connect(void *arg)
{
    ((radio_err_ctx_t *)arg)->result = mmhalow_connect(mm_sta_state_cb);
}

static radio_err_ctx_t s_disconnect_ctx;
static void do_mmhalow_disconnect(void *arg)
{
    ((radio_err_ctx_t *)arg)->result = mmhalow_disconnect();
}

typedef struct {
    wifi_interface_t iface;
    mmhalow_wifi_config_t conf; /* by value - see this block's own top comment */
    esp_err_t result;
} radio_set_config_ctx_t;

static radio_set_config_ctx_t s_set_config_ctx;
static void do_mmhalow_set_config(void *arg)
{
    radio_set_config_ctx_t *c = (radio_set_config_ctx_t *)arg;
    c->result = mmhalow_set_config(c->iface, &c->conf);
}

static SemaphoreHandle_t s_rssi_lock = NULL;
typedef struct {
    int32_t rssi;
} radio_rssi_ctx_t;
static radio_rssi_ctx_t s_rssi_ctx;
static void do_mmwlan_get_rssi(void *arg)
{
    ((radio_rssi_ctx_t *)arg)->rssi = mmwlan_get_rssi();
}

static void do_mmhalow_print_version_info(void *arg)
{
    (void)arg;
    mmhalow_print_version_info();
}

/* s_scan's own generation/cb/ctx fields above are already single-writer,
 * guarded by s_scan_lock (see that struct's comment) - this is just the
 * args struct mmhalow_scan() itself needs, moved off uplink_halow_scan()'s
 * stack for the same reason as everything else in this block. */
static struct mmhalow_scan_args s_scan_args;
static esp_err_t s_scan_start_result;
static void do_mmhalow_scan(void *arg)
{
    (void)arg;
    s_scan_start_result = mmhalow_scan(&s_scan_args);
}

/* HaLow (802.11ah) has no WPA2-PSK - confirmed against mmwlan.h's
 * enum mmwlan_security_type (MMWLAN_OPEN / MMWLAN_OWE / MMWLAN_SAE only). */
static enum mmwlan_security_type halow_security_from_gw(gw_security_mode_t sec)
{
    switch (sec) {
    case GW_SECURITY_OWE:
        return MMWLAN_OWE;
    case GW_SECURITY_SAE:
        return MMWLAN_SAE;
    case GW_SECURITY_OPEN:
    default:
        return MMWLAN_OPEN;
    }
}

/* Registered once with mmhalow_connect() and called on every STA state
 * transition thereafter (association only - not IP). Drives the
 * connect-with-timeout wrapper below via s_connect_sem. */
static void mm_sta_state_cb(enum mmwlan_sta_state sta_state)
{
    switch (sta_state) {
    case MMWLAN_STA_CONNECTED:
        s_associated = true;
        s_associating = false;
        if (s_connect_sem != NULL) {
            xSemaphoreGive(s_connect_sem);
        }
        break;
    case MMWLAN_STA_CONNECTING:
        s_associating = true;
        break;
    case MMWLAN_STA_DISABLED:
    default:
        s_associated = false;
        s_associating = false;
        /* Signal the waiter here too. A connect attempt that fails fast
         * should retry after the backoff, not sit out the full
         * HALOW_CONNECT_TIMEOUT_MS first; halow_sta_connect() distinguishes
         * the two by re-checking s_associated after the take succeeds. */
        if (s_connect_sem != NULL) {
            xSemaphoreGive(s_connect_sem);
        }
        break;
    }
}

static void set_has_ip(bool has_ip)
{
    if (s_has_ip == has_ip) {
        return;
    }
    s_has_ip = has_ip;
    ESP_LOGI(TAG, "HaLow uplink %s", has_ip ? "up (has IP)" : "down");
    if (s_cb) {
        s_cb(has_ip, s_cb_ctx);
    }
}

/* mmhalow_init() creates its netif via ESP_NETIF_DEFAULT_WIFI_STA(), which
 * sets .get_ip_event = IP_EVENT_STA_GOT_IP and DHCP-client flags just like
 * a normal esp_wifi STA netif (confirmed by reading mmhalow.c directly) -
 * so the standard IP_EVENT_STA_GOT_IP/LOST_IP pair is the real "ready"
 * signal, filtered to our netif since other netifs (the SoftAP) exist too. */
static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event->esp_netif == s_netif) {
            set_has_ip(true);
        }
    } else if (id == IP_EVENT_STA_LOST_IP) {
        /* This project has exactly one DHCP-client netif (the HaLow STA) -
         * the SoftAP runs a DHCP *server*, not a client - so no netif
         * filter is needed here. */
        set_has_ip(false);
    }
}

/* Applies a fixed address to the uplink netif instead of running DHCP.
 *
 * This exists for exactly one topology: a leaf associating to a GW_ROLE_RELAY
 * node's HaLow AP. That AP runs no DHCP server and structurally cannot -
 * mmhalow_init() builds its netif from ESP_NETIF_DEFAULT_WIFI_STA(), a DHCP
 * *client* shape, so esp_netif never allocates it a dhcps handle (see the long
 * comment on gw_uplink_config_t.use_static_ip). Against a real Pi this stays
 * off and the DHCP client runs as it always has.
 *
 * Called once at bring-up, before association, and that ordering is what makes
 * the rest of the firmware need no changes at all - esp_netif already
 * implements this path end to end:
 *
 *  - esp_netif_dhcpc_stop() from the initial ESP_NETIF_DHCP_INIT state takes
 *    neither the STARTED nor the STOPPED branch, falls through to
 *    `dhcpc_status = ESP_NETIF_DHCP_STOPPED` and returns ESP_OK
 *    (esp_netif_lwip.c L1547-1582 at v5.5.1). It has to run first:
 *    esp_netif_set_ip_info() rejects a netif whose DHCP client isn't stopped,
 *    with ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED (L1982-1985).
 *  - esp_netif_set_ip_info() then stores the address in esp_netif->ip_info.
 *    It does NOT raise an event here, because that block is guarded by
 *    netif_is_up() (L1997) and this netif is not up yet:
 *    ESP_NETIF_INHERENT_DEFAULT_WIFI_STA() does not carry
 *    ESP_NETIF_FLAG_AUTOUP, which is the only flag that would have brought it
 *    up back in esp_netif_start_api() (L1187). That matters - an
 *    IP_EVENT_STA_GOT_IP raised before association would start NAT and the CoT
 *    relay against a link that doesn't exist.
 *  - On association mmhalow raises its link-state callback (mmhalow.c L16-29),
 *    which calls esp_netif_action_connected(). That runs esp_netif_up() -
 *    applying esp_netif->ip_info to the lwIP netif ("use last obtained ip, or
 *    static ip") - and then, seeing ESP_NETIF_DHCP_CLIENT with status STOPPED
 *    and a valid static address, posts IP_EVENT_STA_GOT_IP itself
 *    (esp_netif_handlers.c, esp_netif_action_connected). ip_event_handler()
 *    below already consumes that, so set_has_ip(true) and the datapath
 *    bring-up happen through the identical path DHCP uses.
 *
 * Because that lives in action_connected, it re-runs on every reconnect - the
 * address is reapplied each time the link comes back, with no work here.
 *
 * One real consequence, worked around as of 2026-08-30 (previously
 * documented here as a deliberate trade - it wasn't, it was a gap that
 * simply hadn't been exercised by real cross-mesh traffic yet): a static
 * uplink learns no DNS server on its own, and esp_netif_set_ip_info()
 * additionally calls dns_clear_servers(true) on a DHCP-client netif
 * (L1987), so this file must set one explicitly, and only *after*
 * set_ip_info() or that same call would immediately clear it again.
 * cfg->static_dns (empty by default - see its own comment in gw_config.h)
 * is that explicit value; for a leaf on a GW_ROLE_RELAY node's HaLow AP it
 * should be the relay's own downlink address, where main/dns_forward.c runs
 * a forwarder using the relay's *own* real upstream DNS server. Left empty,
 * behavior is unchanged from before: ip_forward_nat.c's propagate_dns()
 * warns and offers clients no DNS option rather than 0.0.0.0. */
static esp_err_t apply_static_ip(esp_netif_t *netif, const gw_uplink_config_t *cfg)
{
    esp_netif_ip_info_t ip_info = { 0 };
    ip_info.ip.addr = ipaddr_addr(cfg->static_ip);
    ip_info.gw.addr = ipaddr_addr(cfg->static_gateway);
    ip_info.netmask.addr = ipaddr_addr(cfg->static_netmask);

    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "couldn't stop the uplink DHCP client: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't set the uplink static IP %s/%s: %s", cfg->static_ip,
                 cfg->static_netmask, esp_err_to_name(err));
        return err;
    }

    if (cfg->static_dns[0] != '\0') {
        /* Must come after set_ip_info() above - see this function's own
         * comment on dns_clear_servers(true). */
        esp_netif_dns_info_t dns_info = { 0 };
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        dns_info.ip.u_addr.ip4.addr = ipaddr_addr(cfg->static_dns);
        esp_err_t dns_err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
        if (dns_err != ESP_OK) {
            /* Non-fatal - IP connectivity still works, only name resolution
             * is affected, same severity ip_forward_nat.c's propagate_dns()
             * already treats a missing DNS server as. */
            ESP_LOGW(TAG, "couldn't set the uplink static DNS %s: %s", cfg->static_dns,
                     esp_err_to_name(dns_err));
        }
    }

    ESP_LOGI(TAG, "uplink is statically addressed %s/%s via %s%s%s - no DHCP client will run",
             cfg->static_ip, cfg->static_netmask, cfg->static_gateway,
             cfg->static_dns[0] != '\0' ? ", DNS " : "",
             cfg->static_dns[0] != '\0' ? cfg->static_dns : "");
    return ESP_OK;
}

static esp_err_t halow_sta_bringup(const gw_uplink_config_t *cfg, esp_netif_t **out_netif)
{
    /* radio_control isn't strictly needed yet at this exact point - nothing
     * else touches the radio until uplink_halow_start() creates the
     * reconnect task, well after this returns - but every non-TX call in
     * this file goes through it uniformly (see this file's own "radio_control
     * glue" comment) rather than carving out an exemption for "the ones that
     * happen to run before anything else exists yet". */
    esp_err_t err = radio_control_run(do_mmhalow_init, &s_init_ctx, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = s_init_ctx.result;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmhalow_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* mmhalow.c has no public "get netif" accessor - its netif is a
     * module-static pointer. It's created with if_key "WIFI_STA_DEF" (the
     * same default esp_wifi STA would use), which is safe to look up here
     * since this project's own SoftAP uses "WIFI_AP_DEF" instead. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "mmhalow_init() didn't register the expected WIFI_STA_DEF netif");
        return ESP_FAIL;
    }
    *out_netif = netif;

    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* Before the "not configured" early return below: an operator can set a
     * static address and an SSID in either order, and applying it here keeps
     * this independent of which came first. Non-fatal - a leaf that can't take
     * its static address is still worth bringing up, since the console and web
     * UI are how the mistake gets corrected. */
    if (cfg->use_static_ip) {
        (void)apply_static_ip(netif, cfg);
    }

    /* Radio bring-up finishes here even with no uplink provisioned, and that's
     * deliberate: the scan API needs an initialized radio, and scanning is
     * exactly what an operator does *before* there's an SSID to configure.
     * Handing mmhalow_set_config() a zero-length SSID would be rejected later
     * by umac_connection_validate_sta_args() anyway, so skip it entirely and
     * leave the driver's config untouched until there's something real to
     * apply. */
    if (!gw_uplink_is_configured(cfg)) {
        ESP_LOGW(TAG, "no HaLow uplink configured - radio is up and scannable, "
                      "but no association will be attempted");
        return ESP_OK;
    }

    /* Built directly in the static radio_control ctx (see this file's
     * "radio_control glue" comment) rather than a local later copied in -
     * one less copy, and there's no reason for this particular struct to
     * ever exist as a stack local at all. */
    s_set_config_ctx.iface = WIFI_IF_STA;
    mmhalow_wifi_config_t *conf = &s_set_config_ctx.conf;
    *conf = (mmhalow_wifi_config_t){
        .sta = MMWLAN_STA_ARGS_INIT,
    };

    /* Bounded by the destination field, not just by our own buffer: these
     * are two independently-sized structs (ours from gw_config.h, theirs
     * from mmwlan.h) and a copy sized only by strlen() would overflow if
     * theirs is ever the smaller of the two. */
    size_t ssid_len = strlen(cfg->ssid);
    if (ssid_len > sizeof(conf->sta.ssid)) {
        ESP_LOGE(TAG, "uplink SSID is %u bytes, max %u", (unsigned)ssid_len,
                 (unsigned)sizeof(conf->sta.ssid));
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(conf->sta.ssid, cfg->ssid, ssid_len);
    conf->sta.ssid_len = ssid_len;

    conf->sta.security_type = halow_security_from_gw(cfg->security);
    if (conf->sta.security_type == MMWLAN_SAE) {
        size_t psk_len = strlen(cfg->psk);
        if (psk_len > sizeof(conf->sta.passphrase)) {
            ESP_LOGE(TAG, "uplink passphrase is %u bytes, max %u", (unsigned)psk_len,
                     (unsigned)sizeof(conf->sta.passphrase));
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(conf->sta.passphrase, cfg->psk, psk_len);
        conf->sta.passphrase_len = psk_len;
    }

    esp_err_t set_config_err =
        radio_control_run(do_mmhalow_set_config, &s_set_config_ctx, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    return (set_config_err == ESP_OK) ? s_set_config_ctx.result : set_config_err;
}

static esp_err_t halow_sta_connect(TickType_t timeout_ticks)
{
    if (s_connect_sem == NULL) {
        s_connect_sem = xSemaphoreCreateBinary();
        if (s_connect_sem == NULL) {
            ESP_LOGE(TAG, "couldn't create connect semaphore");
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_connect_sem, 0); /* drain any stale signal before retrying */

    /* Already associated: a previous attempt's association can complete during
     * the backoff sleep, after its timeout was declared. mmhalow_connect() ->
     * mmwlan_sta_enable() must not be re-issued in that state - mmwlan.h
     * (v2.11.2-esp32-2, above mmwlan_sta_enable): "If station mode is already
     * enabled when this function is invoked then it will disconnect from (if
     * already connected) and initiate connection" - i.e. it would tear down
     * the working link we just got. */
    if (s_associated) {
        return ESP_OK;
    }

    esp_err_t err = radio_control_run(do_mmhalow_connect, &s_connect_ctx, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = s_connect_ctx.result;
    }
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_connect_sem, timeout_ticks) != pdTRUE) {
        /* The wait timing out doesn't prove association failed - it can land
         * in the gap between the timeout expiring and this check. Declaring
         * failure here would loop back into mmhalow_connect() and tear the
         * fresh association down (see the mmwlan.h citation above), so trust
         * the state flag over the semaphore. If association reliably takes
         * longer than HALOW_CONNECT_TIMEOUT_MS the retry loop still converges:
         * each mmwlan_sta_enable() restarts the driver's internal scan cycle,
         * and whichever attempt completes during a backoff window is accepted
         * by the s_associated checks instead of being discarded. */
        return s_associated ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    /* The callback signals both outcomes, so a successful take doesn't by
     * itself mean association - it means the state settled. */
    return s_associated ? ESP_OK : ESP_FAIL;
}

static bool halow_sta_link_up(void)
{
    return s_associated;
}

/* Waits up to timeout_ms for the DHCP lease, giving up early if the link
 * drops underneath us. Returns true if the uplink has an IP. */
static bool wait_for_dhcp_lease(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (waited_ms < timeout_ms) {
        if (s_has_ip) {
            return true;
        }
        if (!halow_sta_link_up()) {
            return false; /* association lost - outer loop handles it */
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_POLL_INTERVAL_MS));
        waited_ms += LINK_POLL_INTERVAL_MS;
    }

    return s_has_ip;
}

static void reconnect_task(void *arg)
{
    (void)arg;
    uint32_t backoff_ms = RECONNECT_BACKOFF_MIN_MS;

    for (;;) {
        esp_err_t err = halow_sta_connect(pdMS_TO_TICKS(HALOW_CONNECT_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HaLow STA connect failed (%s), retrying in %u ms",
                     esp_err_to_name(err), (unsigned)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms * 2 > RECONNECT_BACKOFF_MAX_MS) ? RECONNECT_BACKOFF_MAX_MS : backoff_ms * 2;
            continue;
        }

        backoff_ms = RECONNECT_BACKOFF_MIN_MS;
        ESP_LOGI(TAG, "HaLow STA associated (RSSI %" PRId32 " dBm), waiting for %s...",
                 uplink_halow_get_rssi(), s_cfg.use_static_ip ? "the static address to apply"
                                                              : "DHCP lease");

        /* Association alone isn't usable - NAT and the CoT relay both need a
         * real address (see ip_event_handler's comment). Bound the wait so a
         * silent DHCP failure can't strand the task here indefinitely.
         *
         * A statically-addressed uplink gets exactly one attempt: its address
         * arrives via esp_netif_action_connected() posting IP_EVENT_STA_GOT_IP
         * on link-up (see apply_static_ip()), which either happens within a
         * poll interval or points at something a retry can't fix. Restarting
         * the DHCP client - what the extra attempts exist to do - would be
         * actively harmful there: esp_netif_dhcpc_start() puts the netif back
         * under DHCP and discards the static address this leaf depends on,
         * because its AP has no DHCP server to answer the request that
         * follows. */
        const int attempts = s_cfg.use_static_ip ? 1 : DHCP_RESTART_ATTEMPTS;
        bool leased = false;
        for (int attempt = 0; attempt < attempts; attempt++) {
            if (wait_for_dhcp_lease(DHCP_LEASE_TIMEOUT_MS)) {
                leased = true;
                break;
            }
            if (!halow_sta_link_up()) {
                break; /* dropped while waiting - re-associate below */
            }
            if (attempt + 1 < attempts) {
                ESP_LOGW(TAG, "associated but no DHCP lease after %u ms, restarting DHCP client",
                         (unsigned)DHCP_LEASE_TIMEOUT_MS);
                esp_netif_dhcpc_stop(s_netif);
                esp_err_t dhcp_err = esp_netif_dhcpc_start(s_netif);
                if (dhcp_err != ESP_OK) {
                    ESP_LOGW(TAG, "dhcpc restart failed: %s", esp_err_to_name(dhcp_err));
                }
            }
        }

        if (!leased && halow_sta_link_up()) {
            /* Still associated but unusable. Drop the association properly
             * before retrying: mmhalow_disconnect() (-> mmwlan_sta_disable())
             * is a real API in the component's public header, so the earlier
             * approach of re-calling mmhalow_connect() on top of a live
             * association is no longer necessary. */
            if (s_cfg.use_static_ip) {
                /* Not a DHCP problem, so don't report one. The likely causes
                 * are a relay AP that accepted the association but never
                 * brought its link up, or an address this build rejected -
                 * see apply_static_ip()'s log line earlier in the boot. */
                ESP_LOGW(TAG, "associated but the static address never took effect, "
                              "disconnecting and re-associating");
            } else {
                ESP_LOGW(TAG, "no DHCP lease on this association, disconnecting and re-associating");
            }
            set_has_ip(false);
            esp_err_t dis_err =
                radio_control_run(do_mmhalow_disconnect, &s_disconnect_ctx, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
            if (dis_err == ESP_OK) {
                dis_err = s_disconnect_ctx.result;
            }
            if (dis_err != ESP_OK) {
                ESP_LOGW(TAG, "mmhalow_disconnect failed: %s", esp_err_to_name(dis_err));
            }
            s_associated = false;
            continue;
        }

        /* Leased and healthy - hold here until the link actually drops. */
        while (halow_sta_link_up()) {
            vTaskDelay(pdMS_TO_TICKS(LINK_POLL_INTERVAL_MS));
        }

        ESP_LOGW(TAG, "HaLow uplink dropped, will reassociate");
        set_has_ip(false);
    }
}

esp_err_t uplink_halow_init(const gw_uplink_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    s_configured = gw_uplink_is_configured(&s_cfg);
    esp_err_t err = halow_sta_bringup(&s_cfg, &s_netif);
    s_ready = (err == ESP_OK);
    if (s_ready) {
        s_scan_lock = xSemaphoreCreateMutex();
        s_scan_done = xSemaphoreCreateBinary();
        if (s_scan_lock == NULL || s_scan_done == NULL) {
            ESP_LOGW(TAG, "couldn't create scan primitives - scanning will be unavailable");
        }
        s_rssi_lock = xSemaphoreCreateMutex();
        if (s_rssi_lock == NULL) {
            ESP_LOGW(TAG, "couldn't create RSSI lock - RSSI reads will be unavailable");
        }
        /* Logged unconditionally at bring-up: if this prints, host<->MM6108
         * SPI works, which rules out the single most likely first-flash
         * failure before any association attempt muddies the picture. */
        uplink_halow_log_radio_info();
    }
    return err;
}

bool uplink_halow_is_ready(void)
{
    return s_ready;
}

esp_err_t uplink_halow_start(void)
{
    if (!s_ready) {
        /* Without this the reconnect task would spin on mmhalow_connect()
         * against an uninitialized radio forever, and its once-per-second
         * error log would bury the single line from uplink_halow_init() that
         * says what actually went wrong. */
        ESP_LOGE(TAG, "not starting the reconnect task - the radio never initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_configured) {
        /* Same reasoning, one step earlier. With no SSID there is nothing to
         * associate with, so the loop would spend forever alternating between
         * 15-second connect attempts and backoff - producing log noise that
         * looks like a fault, and holding the radio busy exactly when the
         * operator is trying to scan with it. Provision an uplink and reboot;
         * the state is reported as "not configured" until then. */
        ESP_LOGW(TAG, "not starting the reconnect task - no uplink SSID is configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_reconnect_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreate(reconnect_task, "halow_reconnect", GW_STACK_HALOW_RECONNECT, NULL, 5, &s_reconnect_task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_netif_t *uplink_halow_get_netif(void)
{
    return s_netif;
}

bool uplink_halow_is_connected(void)
{
    return s_has_ip;
}

uplink_link_state_t uplink_halow_get_link_state(void)
{
    if (!s_ready) {
        return UPLINK_LINK_RADIO_FAILED;
    }
    if (!s_configured) {
        return UPLINK_LINK_UNCONFIGURED;
    }
    if (s_has_ip) {
        return UPLINK_LINK_UP;
    }
    if (s_associated) {
        return UPLINK_LINK_ASSOCIATED;
    }
    if (s_associating) {
        return UPLINK_LINK_ASSOCIATING;
    }
    return UPLINK_LINK_DOWN;
}

const char *uplink_halow_link_state_name(uplink_link_state_t state)
{
    switch (state) {
    case UPLINK_LINK_RADIO_FAILED:
        return "radio failed";
    case UPLINK_LINK_UNCONFIGURED:
        return "not configured";
    case UPLINK_LINK_DOWN:
        return "searching";
    case UPLINK_LINK_ASSOCIATING:
        return "associating";
    case UPLINK_LINK_ASSOCIATED:
        return "associated, no lease";
    case UPLINK_LINK_UP:
        return "up";
    default:
        return "unknown";
    }
}

int32_t uplink_halow_get_rssi(void)
{
    /* mmwlan_get_rssi() documents INT32_MIN as its error return, which is also
     * what we want to mean "not associated", so an unassociated radio and a
     * failed read are reported identically and callers only need one check. */
    if (!s_ready || !s_associated) {
        return INT32_MIN;
    }
    /* Reachable from the reconnect task, the console and the web UI -
     * s_rssi_lock serializes callers reusing the shared s_rssi_ctx; see this
     * file's "radio_control glue" comment for why that's this wrapper's own
     * job rather than radio_control's. Falls back to INT32_MIN (the same
     * "unknown" value as the checks above) if the lock was never created -
     * see uplink_halow_init(). */
    if (s_rssi_lock == NULL || xSemaphoreTake(s_rssi_lock, pdMS_TO_TICKS(RADIO_CONTROL_DEFAULT_TIMEOUT_MS)) != pdTRUE) {
        return INT32_MIN;
    }
    esp_err_t err = radio_control_run(do_mmwlan_get_rssi, &s_rssi_ctx, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    int32_t rssi = (err == ESP_OK) ? s_rssi_ctx.rssi : INT32_MIN;
    xSemaphoreGive(s_rssi_lock);
    return rssi;
}

void uplink_halow_log_radio_info(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "radio not initialized - no version info available");
        return;
    }
    /* The component's own printer: BCF API/build version, board description,
     * firmware and morselib versions, all via ESP_LOGI. No shared ctx to
     * protect (the printer takes no arguments and returns nothing), so
     * unlike get_rssi this needs no caller-side mutex on top of
     * radio_control's own serialization of the call itself. */
    radio_control_run(do_mmhalow_print_version_info, NULL, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
}

/* `arg` is the generation this callback was registered for, passed by value
 * through the driver's void* so a late callback from a timed-out scan can be
 * recognised and dropped instead of feeding results to the wrong caller. */
static void scan_rx_cb(const struct mmwlan_scan_result *result, void *arg)
{
    struct scan_ctx *sc = &s_scan;
    if (result == NULL || (uint32_t)(uintptr_t)arg != sc->generation) {
        return;
    }

    uplink_scan_result_t out = { 0 };

    /* result->ssid points into the driver's receive buffer and is not
     * NUL-terminated; copy it out bounded by both lengths. */
    size_t ssid_len = result->ssid_len;
    if (ssid_len > sizeof(out.ssid) - 1) {
        ssid_len = sizeof(out.ssid) - 1;
    }
    if (result->ssid != NULL && ssid_len > 0) {
        memcpy(out.ssid, result->ssid, ssid_len);
    }
    out.ssid[ssid_len] = '\0';

    if (result->bssid != NULL) {
        memcpy(out.bssid, result->bssid, sizeof(out.bssid));
    }
    out.rssi = result->rssi;
    out.freq_hz = result->channel_freq_hz;
    out.bw_mhz = result->bw_mhz;

    /* Store, don't deliver - see this file's struct scan_ctx comment for why
     * scan_rx_cb() never touches a caller's cb/ctx. Bounds-checked against
     * the cap so an AP-dense scan overwrites nothing; sc->count keeps
     * counting past it so uplink_halow_scan() can report how many were
     * dropped. */
    if (sc->count < UPLINK_HALOW_SCAN_MAX_RESULTS) {
        sc->result[sc->count] = out;
    }
    sc->count++;
}

static void scan_complete_cb(enum mmwlan_scan_state state, void *arg)
{
    (void)state;
    if ((uint32_t)(uintptr_t)arg != s_scan.generation) {
        return; /* completion for a scan that already timed out */
    }
    if (s_scan_done != NULL) {
        xSemaphoreGive(s_scan_done);
    }
}

/* Shared tail of uplink_halow_scan_start()'s submit-failure path and
 * uplink_halow_scan_poll()'s completion path - the same "close review
 * finding F07, then harvest whatever scan_rx_cb() wrote" logic the old
 * synchronous uplink_halow_scan() always ran at its own tail, retargeted to
 * land in s_scan_cache (read later, on whichever task happens to call
 * uplink_halow_scan_get_cached_results()) instead of a caller's cb() - see
 * this file's "Async scan job state" comment above s_scan_cache for why.
 * Always called with s_scan_lock held; releases it before returning. */
static void finalize_scan(esp_err_t err)
{
    /* Bump the generation before touching s_scan.result/count, not after:
     * this - not the check inside scan_rx_cb() - is what actually closes
     * review finding F07 (see struct scan_ctx's own comment above). Any
     * scan_rx_cb() invocation that hasn't already passed its generation
     * check by this exact point will bail instead of writing into a result
     * slot this function is about to read; nothing below this line can race
     * it any more. Safe to call even when the scan never started - s_scan.count
     * is still 0 from the reset in uplink_halow_scan_start(), so the copy
     * loop below is a no-op. */
    s_scan.generation++;

    uint32_t delivered = s_scan.count < UPLINK_HALOW_SCAN_MAX_RESULTS ? s_scan.count
                                                                       : UPLINK_HALOW_SCAN_MAX_RESULTS;
    for (uint32_t i = 0; i < delivered; i++) {
        s_scan_cache.result[i] = s_scan.result[i];
    }
    s_scan_cache.count = delivered;
    s_scan_cache.complete = (err == ESP_OK);
    s_scan_cache.has_result = true;

    if (delivered > 0) {
        ESP_LOGI(TAG, "scan %s: %" PRIu32 " AP(s) found%s", err == ESP_OK ? "complete" : "ended early",
                 s_scan.count, s_scan.count > UPLINK_HALOW_SCAN_MAX_RESULTS
                                    ? " (capacity reached, later results were dropped)"
                                    : "");
    }

    s_scan_state = UPLINK_HALOW_SCAN_IDLE;
    xSemaphoreGive(s_scan_lock);
}

esp_err_t uplink_halow_scan_start(uint32_t timeout_ms)
{
    if (!s_ready || s_scan_lock == NULL || s_scan_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_scan_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE; /* another scan is already running */
    }

    s_scan.generation++;
    s_scan.count = 0;
    uint32_t generation = s_scan.generation;

    /* Drain any completion left over from a previous scan that timed out, so
     * this one doesn't finalize instantly on a stale signal. */
    xSemaphoreTake(s_scan_done, 0);

    /* s_scan_args is static (see this file's "radio_control glue" comment) -
     * safe to build in place here rather than in a local, since s_scan_lock
     * (held for the whole RUNNING window, taken just above) already
     * guarantees only one caller builds and submits it at a time. */
    s_scan_args = (struct mmhalow_scan_args){
        .rx_cb = scan_rx_cb,
        .complete_cb = scan_complete_cb,
        .cb_arg = (void *)(uintptr_t)generation,
    };

    esp_err_t err = radio_control_run(do_mmhalow_scan, NULL, RADIO_CONTROL_DEFAULT_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = s_scan_start_result;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mmhalow_scan failed: %s", esp_err_to_name(err));
        finalize_scan(err); /* nothing was submitted - releases s_scan_lock, records an empty/failed result */
        return err;
    }

    s_scan_deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    s_scan_state = UPLINK_HALOW_SCAN_RUNNING;
    return ESP_OK;
}

uplink_halow_scan_state_t uplink_halow_scan_poll(void)
{
    if (s_scan_state != UPLINK_HALOW_SCAN_RUNNING) {
        return UPLINK_HALOW_SCAN_IDLE;
    }

    if (xSemaphoreTake(s_scan_done, 0) == pdTRUE) {
        finalize_scan(ESP_OK);
        return UPLINK_HALOW_SCAN_IDLE;
    }

    if (esp_timer_get_time() >= s_scan_deadline_us) {
        ESP_LOGW(TAG, "scan didn't complete within its deadline - delivering whatever was found "
                      "before it");
        finalize_scan(ESP_ERR_TIMEOUT);
        return UPLINK_HALOW_SCAN_IDLE;
    }

    return UPLINK_HALOW_SCAN_RUNNING;
}

bool uplink_halow_scan_get_cached_results(uplink_scan_cb_t cb, void *ctx, bool *out_complete)
{
    if (!s_scan_cache.has_result) {
        return false;
    }
    if (out_complete != NULL) {
        *out_complete = s_scan_cache.complete;
    }
    for (uint32_t i = 0; i < s_scan_cache.count; i++) {
        cb(&s_scan_cache.result[i], ctx);
    }
    return true;
}

esp_err_t uplink_halow_scan(uplink_scan_cb_t cb, void *ctx, uint32_t timeout_ms)
{
    esp_err_t err = uplink_halow_scan_start(timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    /* Poll rather than block directly on s_scan_done, same as any other
     * caller of the async pair above - see uplink_halow_scan_poll()'s own
     * comment. 20ms keeps this console/CLI wrapper responsive without
     * spinning; a scan takes at minimum hundreds of ms, so this adds
     * negligible latency next to the old direct semaphore wait it replaces. */
    while (uplink_halow_scan_poll() == UPLINK_HALOW_SCAN_RUNNING) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    bool complete = false;
    uplink_halow_scan_get_cached_results(cb, ctx, &complete);
    return complete ? ESP_OK : ESP_ERR_TIMEOUT;
}

void uplink_halow_set_state_callback(uplink_halow_state_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}
