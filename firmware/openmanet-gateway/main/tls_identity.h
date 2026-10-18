#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-device HTTPS identity for the web UI (review finding F02,
 * design/PROJECT_REVIEW_2026-09-10.md). See gw_tls_identity_t's own comment
 * in gw_config.h for the storage format and why DER, not PEM.
 *
 * Deliberately owns zero dependency on esp_http_server.h/esp_https_server.h -
 * web_ui.c does all httpd_ssl_config_t plumbing and calls into the plain
 * accessors below, the same split auth.c/chip_temp.c already use to stay
 * independent of the HTTP layer. */

/* Call once at boot, after provisioning_load() so cfg->tls reflects the
 * just-loaded NVS state (or its all-zero/identity_set==false default) - same
 * ordering requirement auth_init() has. If cfg->tls.identity_set is false,
 * generates a fresh ECDSA P-256 keypair and self-signed certificate and
 * persists them via provisioning_save() before returning; otherwise just
 * points this module at the already-generated identity already in cfg. */
esp_err_t tls_identity_init(gw_config_t *cfg);

/* True once an identity exists (freshly generated this boot, or loaded from
 * NVS). web_ui_start() checks this the same way it checks auth_is_ready()
 * and refuses to start httpd_ssl rather than call it with no certificate. */
bool tls_identity_is_ready(void);

/* Raw DER buffers to hand straight to httpd_ssl_config_t's servercert/
 * prvtkey_pem fields - both accept DER despite the "_pem" naming, see
 * gw_tls_identity_t's own comment for why. Valid only once
 * tls_identity_is_ready() is true; return NULL otherwise. */
const uint8_t *tls_identity_get_cert_der(size_t *len_out);
const uint8_t *tls_identity_get_key_der(size_t *len_out);

/* Hex fingerprint (SHA-256 of the DER certificate), colon-separated
 * (e.g. "AB:CD:...:12"), for an operator to read off the serial console and
 * compare against what their browser shows before trusting the
 * certificate - the same role an SSH host key fingerprint plays. out must be
 * at least TLS_IDENTITY_FINGERPRINT_HEX_LEN+1 bytes. Fills out with an empty
 * string if no identity exists yet. */
#define TLS_IDENTITY_FINGERPRINT_HEX_LEN (32 * 3 - 1) /* 32 bytes -> "XX:" * 31 + "XX" */
void tls_identity_get_fingerprint_hex(char *out, size_t out_size);

/* Drops the stored identity and generates a fresh one immediately, without a
 * reboot - console recovery path (gwcfg-reset-tls-identity) for a suspected-
 * compromised key, same shape as auth_drop_all_sessions()/gwcfg-reset-auth.
 * Persists via provisioning_save(); the live config is only updated if that
 * succeeds. */
esp_err_t tls_identity_regenerate(gw_config_t *cfg);

#ifdef __cplusplus
}
#endif
