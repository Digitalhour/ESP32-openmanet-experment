#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called whenever the HaLow uplink transitions between having a DHCP-leased
 * IP and not (IP_EVENT_STA_GOT_IP/LOST_IP on this netif - deliberately not
 * just 802.11 association, since NAT/CoT relay need a real IP to be useful).
 *
 * Invoked from the default esp_event loop task, not an ISR - so the
 * implementation must be short and must not block. That task ("sys_evt") has
 * 2816 bytes of stack in this build and the same task delivers every other
 * event in the system. Doing real work here is what overflowed it and put the
 * node in a reboot loop; see the comment on datapath_task() in app_main.c,
 * which is how the current callback stays within budget. */
typedef void (*uplink_halow_state_cb_t)(bool connected, void *ctx);

/* Where the uplink actually is, as opposed to the binary "usable / not usable"
 * the state callback reports.
 *
 * The distinction matters during bring-up: association and the DHCP lease are
 * the two separate milestones of step 3 in design/HARDWARE.md, with completely
 * different failure causes - a stuck ASSOCIATING points at country code,
 * security mode, SSID or RF; a stuck ASSOCIATED points at DHCP on the Pi. A
 * single "connected" flag collapses those two into one indistinguishable
 * "it doesn't work".
 *
 * UNCONFIGURED is the same argument applied one step earlier. "Nobody has told
 * this node which AP to join" and "the AP we were told about isn't answering"
 * have nothing in common except that neither is associated, and only the
 * second one is a fault. Reporting both as DOWN sent an operator hunting a
 * radio problem that did not exist. */
typedef enum {
    UPLINK_LINK_RADIO_FAILED = 0, /* uplink_halow_init() failed - no radio at all */
    UPLINK_LINK_UNCONFIGURED,     /* radio up, but no uplink has ever been provisioned */
    UPLINK_LINK_DOWN,             /* radio up, configured, not associated */
    UPLINK_LINK_ASSOCIATING,      /* association in progress */
    UPLINK_LINK_ASSOCIATED,       /* 802.11 associated, still no DHCP lease */
    UPLINK_LINK_UP,               /* associated + leased: the datapath is usable */
} uplink_link_state_t;

/* One flattened scan hit. Deliberately not `struct mmwlan_scan_result`: that
 * one is full of pointers into the driver's receive buffer which are only
 * valid inside the callback, so it can't be stored or handed to the web UI. */
typedef struct {
    char ssid[GW_SSID_MAX_LEN + 1];
    uint8_t bssid[6];
    int16_t rssi;
    uint32_t freq_hz;
    uint8_t bw_mhz;
} uplink_scan_result_t;

/* Invoked once per discovered AP (up to uplink_halow_scan()'s own capacity -
 * see main/uplink_halow.c), synchronously from within uplink_halow_scan()
 * itself once the scan has finished or timed out - never from the driver's
 * own scan task. Review finding F07 (design/PROJECT_REVIEW_2026-09-10.md):
 * this used to be invoked live, from the driver's task, which raced a
 * timed-out caller freeing `ctx`. `result` is owned by the caller and only
 * valid for the duration of the call. */
typedef void (*uplink_scan_cb_t)(const uplink_scan_result_t *result, void *ctx);

/* One-time radio/netif bring-up. Safe to call even if the HaLow component
 * integration isn't wired up yet (the bring-up runbook expects
 * SoftAP/NAT/CoT relay to be exercisable before HaLow STA works) - on
 * failure this logs and returns an error, it does not abort. Check
 * uplink_halow_is_ready() before calling anything else here. */
esp_err_t uplink_halow_init(const gw_uplink_config_t *cfg);

/* True once uplink_halow_init() has completed successfully. Everything below
 * except uplink_halow_get_link_state() is meaningless when this is false. */
bool uplink_halow_is_ready(void);

/* Starts the association + reconnect/backoff task. Refuses with
 * ESP_ERR_INVALID_STATE if init failed - a reconnect loop against a radio that
 * never initialized just spins, and its error spam buries the one log line
 * that says what actually went wrong. */
esp_err_t uplink_halow_start(void);

/* NULL until uplink_halow_init() has successfully created the netif. */
esp_netif_t *uplink_halow_get_netif(void);

/* True only in UPLINK_LINK_UP - i.e. associated *and* holding a DHCP lease.
 * This is the signal NAT and the CoT relay gate on. */
bool uplink_halow_is_connected(void);

uplink_link_state_t uplink_halow_get_link_state(void);
const char *uplink_halow_link_state_name(uplink_link_state_t state);

/* Last known RSSI of the associated AP in dBm, or INT32_MIN when unknown
 * (not associated, or the radio hasn't heard from the AP yet). Thin wrapper
 * over mmwlan_get_rssi().
 *
 * The single most useful number during bring-up and antenna placement: it
 * separates "the link is configured wrong" from "the link is configured right
 * but too weak", which otherwise look identical. */
int32_t uplink_halow_get_rssi(void);

/* Runs one scan and invokes cb for each AP found (up to
 * UPLINK_HALOW_SCAN_MAX_RESULTS in main/uplink_halow.c - anything past that
 * is counted in the "scan complete"/"scan ended early" log line but not
 * delivered), then returns once the scan completes or timeout_ms elapses.
 * Blocks the calling task; cb is invoked from that same task, after the wait
 * is already over - see uplink_scan_cb_t's own comment. Built on top of
 * uplink_halow_scan_start()/_poll() below - see those for the non-blocking
 * version. Kept around because it's exactly what a console command wants:
 * gwcfg-scan (provisioning.c) blocking its own task for the scan's duration
 * is the right UX for a synchronous CLI, unlike an HTTP request handler.
 *
 * This answers the question nothing else in this firmware can: "is the Pi's
 * HaLow AP visible at all, on what channel, at what strength?" - which is
 * bring-up step 2 in design/HARDWARE.md, and previously required a
 * Pi-side capture to answer.
 *
 * Note it scans the channel list derived from CONFIG_HALOW_COUNTRY_CODE. An AP
 * on a channel outside this build's regulatory domain will not be found, so an
 * empty result is itself evidence about the country-code question rather than
 * proof the AP is absent. Only one scan runs at a time; concurrent callers get
 * ESP_ERR_INVALID_STATE. */
esp_err_t uplink_halow_scan(uplink_scan_cb_t cb, void *ctx, uint32_t timeout_ms);

/* Stage F review finding (design/PROJECT_REVIEW_2026-09-10.md): "make scans
 * asynchronous and bounded" - uplink_halow_scan() above blocks its caller for
 * the scan's full duration (WEB_UI_SCAN_TIMEOUT_MS = 8s in web_ui.c), which
 * for an HTTP request handler means blocking the single-threaded httpd task
 * for that long - no other request (not even /api/status) is served until it
 * returns. web_ui.c's scan endpoints use this job-style pair instead:
 * uplink_halow_scan_start() submits the scan and returns immediately
 * (bounded only by the same RADIO_CONTROL_DEFAULT_TIMEOUT_MS every other
 * radio_control_run() call in this file already accepts blocking on - see
 * main/radio_control.h), and uplink_halow_scan_poll() is a cheap,
 * non-blocking check callable from a GET handler on every page poll. There's
 * only ever one scan job globally (the radio only supports one at a time -
 * the same constraint uplink_halow_scan()'s own ESP_ERR_INVALID_STATE
 * already enforces), so there's no numeric job ID to track - "is a scan
 * running right now" is the whole of the job's state. */
typedef enum {
    UPLINK_HALOW_SCAN_IDLE = 0, /* no scan in progress right now */
    UPLINK_HALOW_SCAN_RUNNING,  /* a scan submitted by _start() hasn't finished or timed out yet */
} uplink_halow_scan_state_t;

/* Submits a scan and returns immediately - does not wait for it to complete.
 * Same channel-list/regulatory-domain and "only one at a time" notes as
 * uplink_halow_scan() above apply identically; ESP_ERR_INVALID_STATE means a
 * scan is already running. timeout_ms bounds how long uplink_halow_scan_poll()
 * below will keep reporting UPLINK_HALOW_SCAN_RUNNING before giving up and
 * finalizing with whatever was found so far, same as uplink_halow_scan()'s own
 * timeout_ms parameter. */
esp_err_t uplink_halow_scan_start(uint32_t timeout_ms);

/* Non-blocking. Checks whether the scan started by uplink_halow_scan_start()
 * has completed or hit its deadline and, if so, finalizes it (moves results
 * into the cache uplink_halow_scan_get_cached_results() reads from, releases
 * the scan slot for the next caller) before returning. Safe to call as often
 * as a page wants to poll - each call does at most a couple of non-blocking
 * semaphore checks and a timestamp comparison, never a radio_control_run(). */
uplink_halow_scan_state_t uplink_halow_scan_poll(void);

/* Copies the results of the most recently finalized scan into cb, same
 * delivery contract as uplink_scan_cb_t itself. Returns false if no scan has
 * ever finished (nothing written to *out_complete then - a scan finding zero
 * APs is a real result and must stay distinguishable from "never ran", so
 * this isn't inferred from a result count of 0). When it returns true,
 * *out_complete says whether that scan finished within its deadline (false =
 * the cached results are from a scan that timed out, same "partial success"
 * case uplink_halow_scan()'s own ESP_ERR_TIMEOUT return represents). The
 * cache holds the *last* finalized scan's results regardless of current
 * state, so it's safe to call this while UPLINK_HALOW_SCAN_RUNNING reports a
 * new scan already under way - callers get the previous scan's results until
 * the new one finalizes. */
bool uplink_halow_scan_get_cached_results(uplink_scan_cb_t cb, void *ctx, bool *out_complete);

/* Logs the radio's BCF, firmware and morselib versions via the component's own
 * mmhalow_print_version_info(). Getting output here proves host<->MM6108 SPI
 * communication works, which is the cheapest way to tell a wiring/BCF problem
 * apart from a regulatory or credentials problem - the two present identically
 * as "it never associates". */
void uplink_halow_log_radio_info(void);

void uplink_halow_set_state_callback(uplink_halow_state_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
