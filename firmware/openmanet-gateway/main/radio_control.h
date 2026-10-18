#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Owns every non-TX morselib/mmwlan/mmhalow API call behind one dedicated
 * task and a bounded request slot (review finding F06,
 * design/PROJECT_REVIEW_2026-09-10.md).
 *
 * The vendored SDK's own contract is a blanket rule with no per-function
 * carve-outs beyond the three it names: mmwlan.h's top-of-file @warning
 * reads "the functions in this API must not be called concurrently (e.g.,
 * from different thread contexts). The exception to this is the TX API
 * (mmwlan_tx(), mmwlan_tx_tid(), and mmwlan_tx_pkt())." Nothing else is
 * exempt - not a getter, not a regulatory-table lookup, nothing - so this
 * module doesn't try to classify calls as "probably safe"; every non-TX
 * call in this project's own code routes through here instead. TX itself
 * (packet transmission) keeps using its existing, unrelated network path -
 * this module has nothing to do with it.
 *
 * Before this, five independent, unsynchronized execution contexts called
 * into the driver: the boot task (init/config), the halow_reconnect task
 * (connect/disconnect), the esp_timer service task via link_history.c's
 * periodic sampler (RSSI), the console_repl task (RSSI/scan/version/channel
 * list), and the httpd task (the same set, from the web UI). Only
 * scan-vs-scan was ever serialized (uplink_halow.c's own s_scan_lock).
 *
 * Deliberately NOT a generic job queue with caller-owned, possibly-
 * stack-allocated context: uplink_halow.c's own s_scan_ctx already
 * documents why that's dangerous - a request whose caller times out and
 * returns can leave the owner task still holding a pointer into memory that
 * caller no longer owns. So callers of radio_control_run() below must pass
 * a ctx that outlives the call - a static/module-level struct, exactly like
 * s_scan_ctx, never a stack local. When more than one task can call the
 * same wrapper function (e.g. uplink_halow_get_rssi(), reachable from the
 * reconnect task, the console and the web UI), that wrapper is responsible
 * for its own additional mutex around its own static ctx - this module only
 * guarantees the *driver call itself* is single-owner; two callers racing
 * to reuse the same static result struct is a separate problem each
 * wrapper function owns solving, the same way s_scan_lock already does for
 * uplink_halow_scan(). */

typedef void (*radio_control_fn_t)(void *ctx);

/* Call once at boot, before any uplink/downlink bring-up - both
 * uplink_halow_init() and downlink_halow_ap_init() route every non-TX call
 * through radio_control_run() from that point on, so the owner task must
 * already exist. */
esp_err_t radio_control_init(void);

/* Every real driver call this project makes through here (init/set_config/
 * connect/disconnect/scan-start/get_rssi/print_version_info/
 * lookup_regulatory_domain) is documented or observed to be a fast "kick it
 * off or read current state" call, not one that blocks until a connection
 * or scan actually completes - callers that need to wait for that *result*
 * do so themselves afterward, via their own semaphore or callback, outside
 * this call entirely (see e.g. uplink_halow.c's halow_sta_connect(), which
 * waits on s_connect_sem after the radio_control_run() that issues
 * mmhalow_connect() has already returned). A few seconds is generous margin
 * for something that should take milliseconds, while still bounded. */
#define RADIO_CONTROL_DEFAULT_TIMEOUT_MS 3000

/* Runs fn(ctx) on the radio_control task and blocks the calling task until
 * it completes or timeout_ms elapses. fn reports its own result through
 * ctx - this function's return value only describes whether fn got to run:
 *
 *   ESP_OK                 - fn ran to completion; its result is in ctx.
 *   ESP_ERR_TIMEOUT         - fn had not completed within timeout_ms. It may
 *                             still be queued or actually executing on the
 *                             owner task, and may write into ctx *after*
 *                             this call returns - which is exactly why ctx
 *                             must be static/persistent (see this file's
 *                             top comment), never a stack local a timed-out
 *                             caller might otherwise go on to invalidate.
 *   ESP_ERR_NO_MEM          - the single request slot was already occupied
 *                             and didn't free up within timeout_ms - the
 *                             "queue full" case.
 *   ESP_ERR_INVALID_STATE   - radio_control_init() hasn't run yet. */
esp_err_t radio_control_run(radio_control_fn_t fn, void *ctx, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
