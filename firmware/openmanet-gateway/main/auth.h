#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Web UI authentication (design/ROADMAP.md item 1): PBKDF2-HMAC-SHA256
 * credential verification, RAM-only sessions, and login lockout/backoff.
 *
 * Deliberately owns zero dependency on esp_http_server.h - main/web_ui.c
 * does all cookie/header/JSON plumbing and calls into the pure functions
 * below, the same split chip_temp.c/link_history.c already use to stay
 * independent of the HTTP layer.
 *
 * The PBKDF2 derivation itself never runs on this device. The client always
 * has the plaintext password (a human just typed it) and is the only party
 * that ever needs to *derive* a key from it; this module only ever *stores*
 * a already-derived stored_key (via auth_commit_password()) and *verifies*
 * a login attempt against it with one HMAC-SHA256 call. This keeps a
 * 100,000-round KDF loop off the already-tight GW_STACK_WEB_UI budget - see
 * main/CMakeLists.txt's mbedtls comment. */

/* Hex-encoded lengths (2 chars per byte) of the values this API exchanges. */
#define AUTH_SALT_HEX_LEN       32 /* 16 bytes */
#define AUTH_NONCE_HEX_LEN      32 /* 16 bytes */
#define AUTH_STORED_KEY_HEX_LEN 64 /* 32 bytes */
#define AUTH_RESPONSE_HEX_LEN   64 /* 32 bytes, same size as stored_key - both are HMAC-SHA256 output */
#define AUTH_TOKEN_HEX_LEN      32 /* 16 bytes */
#define AUTH_SETUP_SECRET_HEX_LEN 32 /* 16 bytes - review finding F03, see gw_auth_config_t's comment */

/* Call once at boot (app_main.c, alongside provisioning_init()) - not from
 * inside web_ui_start(), because provisioning.c's gwcfg-reset-auth console
 * command (a sibling module) needs auth_drop_all_sessions() too. Stashes cfg
 * (already loaded by provisioning_load()) and creates the session/lockout
 * lock. */
esp_err_t auth_init(gw_config_t *cfg);

/* False only if auth_init() was never called or failed (e.g. mutex
 * allocation) - every other function below takes s_auth_lock unconditionally
 * and would assert on FreeRTOS's non-NULL-handle check if called first.
 * web_ui_start() checks this and refuses to start rather than serve a
 * management interface no auth call can safely gate - review finding "safe
 * auth-init failure", design/PROJECT_REVIEW_2026-09-10.md. */
bool auth_is_ready(void);

bool auth_password_is_set(void);

/* Fills nonce_hex_out (AUTH_NONCE_HEX_LEN+1 bytes), salt_hex_out
 * (AUTH_SALT_HEX_LEN+1 bytes) and *iterations_out from the currently stored
 * credential, and remembers the nonce as the single pending login challenge
 * (TTL 30s, single-use). Returns false without touching any output if no
 * password is set yet. */
bool auth_begin_login_challenge(char *nonce_hex_out, char *salt_hex_out, uint32_t *iterations_out);

typedef enum {
    AUTH_LOGIN_OK,
    AUTH_LOGIN_BAD_CREDENTIALS,
    AUTH_LOGIN_LOCKED_OUT,
    AUTH_LOGIN_NO_CHALLENGE,
} auth_login_result_t;

/* Verifies response_hex against the pending challenge issued by
 * auth_begin_login_challenge() for the given nonce_hex - the challenge is
 * consumed (cleared) by this call regardless of outcome, so a captured
 * response can never be replayed. On AUTH_LOGIN_OK, fills token_hex_out
 * (AUTH_TOKEN_HEX_LEN+1 bytes) with a freshly minted session token. On
 * AUTH_LOGIN_LOCKED_OUT, fills *retry_after_s_out. */
auth_login_result_t auth_verify_login(const char *nonce_hex, const char *response_hex,
                                       char *token_hex_out, uint32_t *retry_after_s_out);

/* True if the challenge slot is currently locked out (used so
 * auth_begin_login_challenge()'s caller can skip issuing a challenge - and
 * the client an expensive PBKDF2 derivation it can't use yet - while locked).
 * Fills *retry_after_s_out when returning true. */
bool auth_is_locked_out(uint32_t *retry_after_s_out);

/* Touches the session's last-seen time on a hit (sliding idle timeout). */
bool auth_check_session(const char *token_hex);

void auth_end_session(const char *token_hex);

/* Generates a fresh server-side salt for a password set/change, remembers it
 * as the single pending "salt offer" (TTL 120s), and fills salt_hex_out
 * (AUTH_SALT_HEX_LEN+1 bytes) and *iterations_out. */
void auth_begin_password_salt(char *salt_hex_out, uint32_t *iterations_out);

typedef enum {
    AUTH_SET_OK,
    AUTH_SET_NO_PENDING_SALT,
    AUTH_SET_BAD_KEY,
    AUTH_SET_BAD_SETUP_SECRET, /* first-use claim only - see setup_secret_hex below */
    AUTH_SET_SAVE_FAILED, /* provisioning_save() failed - live config is untouched, matching
                           * config_post_handler's own "don't write back unless persisted"
                           * discipline (main/web_ui.c) - the caller should surface a 500 */
} auth_set_password_result_t;

/* Commits stored_key_hex (must be exactly AUTH_STORED_KEY_HEX_LEN hex chars)
 * against the pending salt offer from auth_begin_password_salt() - the
 * salt/iterations actually written come from that offer, never from the
 * caller, so a client can't smuggle in a weaker pair.
 *
 * setup_secret_hex gates *first* ownership (review finding F03,
 * design/PROJECT_REVIEW_2026-09-10.md; see gw_auth_config_t's own comment,
 * main/gw_config.h): when no password is set yet, it must be exactly
 * AUTH_SETUP_SECRET_HEX_LEN hex chars and match the device's current setup
 * secret, or this returns AUTH_SET_BAD_SETUP_SECRET without touching
 * anything. Ignored (may be NULL) once a password already exists - that
 * path is a change-password request, already gated by an authenticated
 * session at the call site (main/web_ui.c's auth_require_session()), not by
 * this.
 *
 * Persists immediately via provisioning_save(); the live config is only
 * updated if that succeeds. On AUTH_SET_OK, drops every existing session
 * (including the caller's) and fills token_hex_out with a fresh one, so a
 * password change never logs the caller out; also closes the onboarding
 * window for good (real ownership is established now, not just claimable). */
auth_set_password_result_t auth_commit_password(const char *stored_key_hex, const char *setup_secret_hex,
                                                  char *token_hex_out);

/* True if a physically-present operator can still claim this device -
 * password not yet set and the onboarding window (gw_auth_config_t's own
 * comment) still open. */
bool auth_onboarding_is_open(void);

/* Fills out (AUTH_SETUP_SECRET_HEX_LEN+1 bytes) with the current setup
 * secret's hex encoding. Returns false (leaving out untouched) if onboarding
 * is closed or a password is already set - gwcfg-show-setup-secret and the
 * one-time boot log line both go through this, so there is exactly one
 * place that decides whether the secret is currently showable. */
bool auth_get_setup_secret_hex(char *out, size_t out_size);

/* Console recovery (gwcfg-reopen-onboarding): regenerates the setup secret
 * and reopens the onboarding window immediately, without a reboot - for an
 * operator who let the boot budget run out before claiming the device.
 * Returns ESP_ERR_INVALID_STATE without changing anything if a password is
 * already set (the claim endpoints stop checking the setup secret entirely
 * once one exists, so reopening would be a no-op that could only confuse an
 * operator into thinking it did something). Persists via
 * provisioning_save(); the live config is only updated if that succeeds. */
esp_err_t auth_reopen_onboarding(void);

/* Drops every live session immediately, without a reboot. Used by
 * gwcfg-reset-auth and internally by auth_commit_password(). */
void auth_drop_all_sessions(void);

#ifdef __cplusplus
}
#endif
