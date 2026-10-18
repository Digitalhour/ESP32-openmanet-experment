#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 30s x 240 samples = 2 hours of retention. A live-dashboard/field-test
 * window, not an overnight-unattended one - the log ring (log_buffer.h) and
 * chip_temp.h already cover "did it survive the night", this is for "how did
 * the link look during the walk test I was just running". */
#define LINK_HISTORY_SAMPLES    240
#define LINK_HISTORY_INTERVAL_S 30

/* Starts an esp_timer that samples the active uplink's RSSI (whichever of
 * uplink_halow_get_rssi()/uplink_wifi_get_rssi() isn't reporting its own
 * "not ready" sentinel - see link_history.c) into a ring buffer every
 * LINK_HISTORY_INTERVAL_S seconds. Safe to call once at boot regardless of
 * role. Takes one sample immediately so a page load right after boot isn't
 * empty. */
esp_err_t link_history_init(void);

/* Copies up to out_capacity samples into out, oldest first, and returns how
 * many were written. Each value is a signed RSSI in dBm, or INT16_MIN for
 * "no reading at that sample time" (radio not up yet, or link_history_init()
 * was never called/failed). */
size_t link_history_get_rssi(int16_t *out, size_t out_capacity);

/* Status-cache follow-up (Stage F, design/ROADMAP.md): the raw HaLow/native
 * Wi-Fi RSSI readings from the *same* periodic sample this file already
 * takes for the history ring above - cached here separately because the
 * ring only keeps one collapsed "whichever uplink is live" value per sample
 * (sample_uplink_rssi()'s own logic), but callers that need to know both
 * values independently (gwcfg-status/api/status, which report per-role or
 * always-both-shapes respectively) need the raw pair, not the collapsed one.
 *
 * Before these existed, gwcfg-status and /api/status each called
 * uplink_halow_get_rssi()/uplink_wifi_get_rssi() live on every request -
 * competing with this file's own timer, and with each other, for the same
 * single-owner radio_control task (main/radio_control.h) on every status
 * check. These getters read back whatever the periodic timer already
 * collected instead - no radio_control_run() call, no contention - at the
 * cost of the reading being up to LINK_HISTORY_INTERVAL_S stale. That
 * tradeoff is the same one link_history_get_rssi() above has always made for
 * the history graph; RSSI on a stationary gateway doesn't move fast enough
 * for a human checking a status page to notice a 30s-old value, and this is
 * explicitly a status *display*, not a control-loop input - nothing decides
 * to reconnect or hand off based on this. Same INT32_MIN/INT8_MIN sentinels
 * as the live calls they replace, for "not associated"/no sample yet - see
 * uplink_halow_get_rssi()/uplink_wifi_get_rssi()'s own comments. */
int32_t link_history_get_latest_halow_rssi(void);
int8_t link_history_get_latest_wifi_rssi(void);

#ifdef __cplusplus
}
#endif
