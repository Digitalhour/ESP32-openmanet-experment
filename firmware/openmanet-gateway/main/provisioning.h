#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the NVS flash partition and the live-config mutex. Call once at
 * boot, before load/save and before any task that touches the config runs. */
esp_err_t provisioning_init(void);

/* Serializes access to the live gw_config_t shared by app_main, the console
 * REPL, and the httpd task. Hold across any read-modify-write of it; the
 * lock is recursive-free, so don't nest. Safe to call before
 * provisioning_init() (no-ops until the mutex exists). */
void provisioning_config_lock(void);
void provisioning_config_unlock(void);

/* Current live-config revision. Caller must already hold
 * provisioning_config_lock() - this is meant to be read in the same lock
 * scope as a snapshot of the config itself, so the two are consistent with
 * each other. Review finding F11 (design/PROJECT_REVIEW_2026-09-10.md): see
 * provisioning_config_commit()'s own comment for what this is for. Never
 * persisted and resets to 0 on every boot - it only means anything relative
 * to another read earlier in the same boot. */
uint32_t provisioning_config_revision(void);

/* Adopts new_cfg as the live config and bumps the revision counter above.
 * Caller must already hold provisioning_config_lock(). This is the only
 * function that should ever write through the pointer console commands got
 * from provisioning_register_console_commands() or web_ui.c holds via
 * app_main's s_cfg - going through it rather than a raw struct copy is what
 * lets provisioning_config_revision() actually mean something. */
void provisioning_config_commit(const gw_config_t *new_cfg);

/* Rejects a config that esp_wifi/lwIP would refuse at bring-up, or that
 * would take down the SoftAP the device is managed over (e.g. a WPA2
 * passphrase outside 8-63 chars, an out-of-range channel, a non-multicast
 * CoT group). Returns ESP_OK if usable, otherwise ESP_ERR_INVALID_ARG with a
 * human-readable explanation written to errbuf. Called automatically by
 * provisioning_save() and provisioning_load(); call it directly when you
 * want the reason string to show the user. */
esp_err_t provisioning_validate(const gw_config_t *cfg, char *errbuf, size_t errbuf_len);

/* Fills cfg with the built-in defaults: a working SoftAP and CoT relay, and
 * deliberately *no* uplink at all (empty SSID - see gw_uplink_is_configured()).
 * A factory-fresh node is therefore explicitly "not configured" rather than
 * quietly chasing a placeholder AP; the operator names the Pi's HaLow AP via
 * the web UI or `gwcfg-set-uplink`. Reopens onboarding, same as a genuinely
 * new device - only call this for a device that either never had a stored
 * config, or whose owner is deliberately, physically re-claiming it (see
 * factory_reset.c). For provisioning_load()'s own "a config was stored but
 * this firmware can't use it" fallback, use
 * provisioning_get_recovery_defaults() below instead. */
void provisioning_get_defaults(gw_config_t *cfg);

/* Same as provisioning_get_defaults() above, except onboarding is forced
 * permanently closed rather than left open - see this function's own
 * definition in provisioning.c for the full reasoning (review finding F14's
 * migration portion, design/PROJECT_REVIEW_2026-09-10.md). Use this wherever
 * a *stored* config existed but couldn't be used, so a routine
 * GW_CONFIG_VERSION bump or NVS corruption can't silently make an owned
 * device claimable by anyone with SoftAP access. */
void provisioning_get_recovery_defaults(gw_config_t *cfg);

/* True for the rest of this boot once provisioning_get_recovery_defaults()
 * ran. One caller: tls_identity.c's boot-time (non-operator-triggered)
 * identity generation, which must not auto-persist during a recovery boot -
 * see this function's own definition in provisioning.c for why (review
 * finding F14's migration portion). Not a general "is it safe to save"
 * check - console commands and web UI writes have their own, different
 * reasons they're already safe here; this exists for exactly one call site. */
bool provisioning_in_recovery_mode(void);

/* Loads config from NVS into cfg, falling back to provisioning_get_defaults()
 * if nothing has been saved yet or the stored blob is invalid. Always
 * returns ESP_OK; failures degrade to defaults rather than propagate, since
 * "no config yet" is a normal first-boot state. */
esp_err_t provisioning_load(gw_config_t *cfg);

/* Persists cfg to NVS, stamping it with the current magic/version. Validates
 * first and returns ESP_ERR_INVALID_ARG without writing if the config is
 * unusable, so a bad value can't be made permanent. */
esp_err_t provisioning_save(const gw_config_t *cfg);

/* Shared uplink-security (de)serialization, used by both the console
 * (gwcfg-set-uplink) and the web UI so the two never drift apart. */
/* Parses "open"/"owe"/"sae" into *out and returns true. An unrecognized
 * string returns false and leaves *out untouched - callers must reject the
 * request rather than fall back to a default, so a typo like "saee" can't
 * silently configure an open radio (review finding F10,
 * design/PROJECT_REVIEW_2026-09-10.md). */
bool provisioning_parse_security(const char *s, gw_security_mode_t *out);
const char *provisioning_security_name(gw_security_mode_t sec);

/* Same pattern, for gw_node_role_t - shared by the console (gwcfg-set-role)
 * and the web UI. */
gw_node_role_t provisioning_parse_role(const char *s);
const char *provisioning_role_name(gw_node_role_t role);

/* Registers `gwcfg-*` esp_console commands that read/mutate *cfg in place
 * and can persist it via provisioning_save(). cfg must outlive the console. */
esp_err_t provisioning_register_console_commands(gw_config_t *cfg);

/* Brings up a UART REPL console (stdin/stdout) running the registered
 * commands. Blocks nothing - starts a background task. */
esp_err_t provisioning_start_console(void);

#ifdef __cplusplus
}
#endif
