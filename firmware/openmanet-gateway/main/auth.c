#include "auth.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"

#include "provisioning.h"

static const char *TAG = "auth";

#define AUTH_SESSION_MAX 4 /* a phone plus a laptop plus headroom, not an open-ended table */

/* Lockout schedule: locks at the 5th failure, doubling on each failure that
 * lands while *not* currently locked (i.e. across separate lockout windows -
 * a failure that arrives while already locked is rejected before it's ever
 * evaluated, so it can't itself grow the lockout), capped at 5 minutes. */
#define AUTH_LOCKOUT_THRESHOLD 5
#define AUTH_LOCKOUT_BASE_S    30
#define AUTH_LOCKOUT_MAX_S     300

#define AUTH_CHALLENGE_TTL_US  (30LL * 1000000LL)        /* one login round trip */
#define AUTH_SALT_OFFER_TTL_US (120LL * 1000000LL)       /* generous - a human is typing */
#define AUTH_SESSION_IDLE_US   (30LL * 60 * 1000000LL)   /* 30 minutes */

/* Absolute cap on a session's lifetime regardless of activity - review
 * finding F15 (design/PROJECT_REVIEW_2026-09-10.md): before this, a session
 * kept continuously active (a dashboard tab left open and auto-refreshing,
 * say) never actually expired, since AUTH_SESSION_IDLE_US only resets on
 * every touch and nothing ever compared against when the session began. Set
 * to match web_ui.c's own cookie Max-Age (43200s) rather than picking a new
 * number - that value already states how long this project considers a
 * session's whole lifetime reasonable; this just makes the server actually
 * enforce it instead of leaving it advisory (a client that simply keeps
 * resending the cookie past its stated Max-Age was previously not stopped by
 * anything server-side). */
#define AUTH_SESSION_ABSOLUTE_MAX_US (43200LL * 1000000LL) /* 12 hours */

/* How many login challenges can be pending at once - "a phone plus a laptop
 * plus headroom", same reasoning as AUTH_SESSION_MAX above. A challenge is
 * requested and then either completed or abandoned well before a session
 * would ordinarily need this many slots, so reusing the same figure is
 * generous, not tight. */
#define AUTH_CHALLENGE_MAX 4

/* Stored per-credential in gw_auth_config_t.iterations rather than assumed
 * fixed, so this can be tuned later without stranding already-provisioned
 * devices - see gw_config.h's own comment. This is only the value handed out
 * for a *new* password; an existing credential's own stored iterations
 * count is what login actually verifies against. */
#define AUTH_PBKDF2_ITERATIONS 100000

/* How many boots the first-ownership onboarding window survives before it
 * closes on its own (review finding F03, design/PROJECT_REVIEW_2026-09-10.md;
 * see gw_auth_config_t's own comment, main/gw_config.h, for why this is a
 * boot count rather than a wall-clock timeout). Generous enough to cover a
 * real unboxing-to-claimed session with a couple of reboots for
 * troubleshooting along the way, still clearly bounded rather than
 * open-ended. */
#define AUTH_ONBOARDING_BOOT_BUDGET 10

typedef struct {
    bool in_use;
    char token_hex[AUTH_TOKEN_HEX_LEN + 1];
    int64_t last_seen_us; /* sliding idle timeout - see AUTH_SESSION_IDLE_US */
    int64_t issued_us;    /* fixed at creation - see AUTH_SESSION_ABSOLUTE_MAX_US */
} auth_session_t;

typedef struct {
    bool active;
    char nonce_hex[AUTH_NONCE_HEX_LEN + 1];
    int64_t issued_us;
} auth_pending_challenge_t;

typedef struct {
    bool active;
    uint8_t salt[16];
    uint32_t iterations;
    int64_t issued_us;
} auth_pending_salt_t;

static gw_config_t *s_cfg = NULL;

/* Guards everything below - the session table, the challenge table, the
 * single-slot pending password-salt offer, and the lockout counters.
 * Deliberately separate from provisioning.c's s_cfg_lock, which is
 * config-(NVS blob)-only; this state is RAM-only and unrelated to
 * gw_config_t mutation. gw_config_t's own .auth fields are still read/written
 * under provisioning_config_lock() at the points that touch them, same as
 * every other field. */
static SemaphoreHandle_t s_auth_lock = NULL;

static auth_session_t s_sessions[AUTH_SESSION_MAX];

/* A small table rather than one global slot - see AUTH_CHALLENGE_MAX's own
 * comment and challenge_issue_locked() below (review finding F15). Still
 * just one pending password-salt offer (s_pending_salt): that one is only
 * ever issued as part of the single, inherently-serial first-ownership
 * onboarding flow (review finding F03), which has no equivalent
 * multiple-concurrent-client scenario to protect against. */
static auth_pending_challenge_t s_pending_challenges[AUTH_CHALLENGE_MAX];
static auth_pending_salt_t s_pending_salt;

static uint8_t s_fail_count = 0;
static int64_t s_locked_until_us = 0;

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (hex == NULL || strlen(hex) != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* Full-length, no early exit - this is the one comparison in this file that
 * actually guards a secret (the login HMAC); session-token lookups below use
 * plain strcmp(), which is proportionate for a bearer token already behind
 * HttpOnly + the existing subnet check, but not for a password-derived
 * value an attacker could otherwise learn one byte at a time via timing. */
static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* Caller must hold s_auth_lock. Mints a fresh token into token_hex_out and
 * installs it, evicting the least-recently-seen slot if the table is full -
 * mirrors the lru_purge_enable idea main/web_ui.c already applies to httpd
 * sockets, applied here to sessions instead. */
static void session_create_locked(char *token_hex_out)
{
    uint8_t token_bytes[16];
    esp_fill_random(token_bytes, sizeof(token_bytes));
    bytes_to_hex(token_bytes, sizeof(token_bytes), token_hex_out);

    int slot = -1;
    int64_t oldest_seen = INT64_MAX;
    int oldest_idx = 0;
    for (int i = 0; i < AUTH_SESSION_MAX; i++) {
        if (!s_sessions[i].in_use) {
            slot = i;
            break;
        }
        if (s_sessions[i].last_seen_us < oldest_seen) {
            oldest_seen = s_sessions[i].last_seen_us;
            oldest_idx = i;
        }
    }
    if (slot < 0) {
        slot = oldest_idx;
    }

    int64_t now = esp_timer_get_time();
    s_sessions[slot].in_use = true;
    strlcpy(s_sessions[slot].token_hex, token_hex_out, sizeof(s_sessions[slot].token_hex));
    s_sessions[slot].last_seen_us = now;
    s_sessions[slot].issued_us = now;
}

/* Caller must hold s_auth_lock. Installs a fresh nonce into nonce_hex_out and
 * an available slot, evicting the oldest-issued slot if the table is full -
 * same eviction policy as session_create_locked() above, applied to pending
 * login challenges instead of sessions. Review finding F15
 * (design/PROJECT_REVIEW_2026-09-10.md): before this, one global pending
 * challenge meant a second client's GET .../challenge silently invalidated a
 * first client's in-flight one, and a wrong/stale nonce submitted by anyone
 * cleared whatever challenge happened to be pending - not necessarily the
 * sender's own. A small table, each slot independently matched by its own
 * nonce in auth_verify_login(), means one client's traffic - legitimate or
 * not - can no longer affect another's. An expired-but-not-yet-reclaimed slot
 * counts as free here, same as an unused one, so a burst of abandoned
 * challenges from one client doesn't evict a different client's still-valid
 * one ahead of its own TTL. */
static void challenge_issue_locked(char *nonce_hex_out)
{
    uint8_t nonce_bytes[16];
    esp_fill_random(nonce_bytes, sizeof(nonce_bytes));
    bytes_to_hex(nonce_bytes, sizeof(nonce_bytes), nonce_hex_out);

    int64_t now = esp_timer_get_time();
    int slot = -1;
    int64_t oldest_issued = INT64_MAX;
    int oldest_idx = 0;
    for (int i = 0; i < AUTH_CHALLENGE_MAX; i++) {
        if (!s_pending_challenges[i].active || (now - s_pending_challenges[i].issued_us) >= AUTH_CHALLENGE_TTL_US) {
            slot = i;
            break;
        }
        if (s_pending_challenges[i].issued_us < oldest_issued) {
            oldest_issued = s_pending_challenges[i].issued_us;
            oldest_idx = i;
        }
    }
    if (slot < 0) {
        slot = oldest_idx;
    }

    strlcpy(s_pending_challenges[slot].nonce_hex, nonce_hex_out, sizeof(s_pending_challenges[slot].nonce_hex));
    s_pending_challenges[slot].active = true;
    s_pending_challenges[slot].issued_us = now;
}

esp_err_t auth_init(gw_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;

    s_auth_lock = xSemaphoreCreateMutex();
    if (s_auth_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Onboarding window state machine (review finding F03,
     * design/PROJECT_REVIEW_2026-09-10.md; see gw_auth_config_t's own
     * comment, main/gw_config.h). Deliberately mutates *cfg in place - a
     * static global (app_main.c's s_cfg), not a stack copy - rather than
     * following auth_commit_password()'s scratch-copy-then-save discipline:
     * that pattern needs a full gw_config_t local (now well over a KB with
     * the tls/auth fields this project has added), and this function runs
     * on FreeRTOS's default "main" task at boot - the same
     * ~3584-byte CONFIG_ESP_MAIN_TASK_STACK_SIZE budget a gw_config_t-sized
     * local overflowed once already this session (see
     * GW_STACK_TLS_IDENTITY_GEN's own comment, task_stats.h). The state
     * here is boot-count bookkeeping, not a credential a failed write could
     * leave inconsistent in a way that matters - worst case on a failed
     * save is the next boot re-derives the same decision.
     *
     * Three states, distinguished exactly as gw_auth_config_t's own comment
     * describes:
     *   - never started (fresh/reset device): open a window.
     *   - started and still open: spend one boot of the budget, closing it
     *     if that was the last one.
     *   - started and already closed: do nothing - a plain reboot must not
     *     reopen it (review acceptance criterion), only
     *     auth_reopen_onboarding() (gwcfg-reopen-onboarding) may. */
    if (!cfg->auth.password_set) {
        provisioning_config_lock();
        bool changed = false;
        if (!cfg->auth.onboarding_ever_started) {
            esp_fill_random(cfg->auth.setup_secret, sizeof(cfg->auth.setup_secret));
            cfg->auth.onboarding_ever_started = true;
            cfg->auth.onboarding_open = true;
            cfg->auth.onboarding_boots_remaining = AUTH_ONBOARDING_BOOT_BUDGET;
            changed = true;
        } else if (cfg->auth.onboarding_open) {
            if (cfg->auth.onboarding_boots_remaining > 0) {
                cfg->auth.onboarding_boots_remaining--;
            }
            if (cfg->auth.onboarding_boots_remaining == 0) {
                cfg->auth.onboarding_open = false;
            }
            changed = true;
        }
        esp_err_t save_err = changed ? provisioning_save(cfg) : ESP_OK;
        bool open_now = cfg->auth.onboarding_open;
        provisioning_config_unlock();

        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to persist onboarding window state: %s", esp_err_to_name(save_err));
        }
        if (open_now) {
            char secret_hex[AUTH_SETUP_SECRET_HEX_LEN + 1];
            if (auth_get_setup_secret_hex(secret_hex, sizeof(secret_hex))) {
                ESP_LOGI(TAG, "onboarding open - device setup code: %s", secret_hex);
                ESP_LOGI(TAG, "present this to claim admin ownership - also available any time via "
                              "'gwcfg-show-setup-secret' while onboarding stays open");
            }
        }
    }

    return ESP_OK;
}

bool auth_is_ready(void)
{
    return s_auth_lock != NULL;
}

bool auth_password_is_set(void)
{
    provisioning_config_lock();
    bool set = s_cfg->auth.password_set;
    provisioning_config_unlock();
    return set;
}

bool auth_is_locked_out(uint32_t *retry_after_s_out)
{
    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    bool locked = s_locked_until_us > now;
    if (locked && retry_after_s_out != NULL) {
        *retry_after_s_out = (uint32_t)((s_locked_until_us - now + 999999) / 1000000);
    }
    xSemaphoreGive(s_auth_lock);
    return locked;
}

bool auth_begin_login_challenge(char *nonce_hex_out, char *salt_hex_out, uint32_t *iterations_out)
{
    if (!auth_password_is_set()) {
        return false;
    }

    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    challenge_issue_locked(nonce_hex_out);
    xSemaphoreGive(s_auth_lock);

    provisioning_config_lock();
    bytes_to_hex(s_cfg->auth.salt, sizeof(s_cfg->auth.salt), salt_hex_out);
    *iterations_out = s_cfg->auth.iterations;
    provisioning_config_unlock();

    return true;
}

auth_login_result_t auth_verify_login(const char *nonce_hex, const char *response_hex,
                                       char *token_hex_out, uint32_t *retry_after_s_out)
{
    xSemaphoreTake(s_auth_lock, portMAX_DELAY);

    int64_t now = esp_timer_get_time();
    if (s_locked_until_us > now) {
        if (retry_after_s_out != NULL) {
            *retry_after_s_out = (uint32_t)((s_locked_until_us - now + 999999) / 1000000);
        }
        xSemaphoreGive(s_auth_lock);
        return AUTH_LOGIN_LOCKED_OUT;
    }

    /* Scan for the one slot matching this nonce, not a single shared one -
     * see challenge_issue_locked()'s comment (review finding F15). Single-use
     * regardless of outcome, cleared as soon as it's matched, before the HMAC
     * below is even checked - a captured (nonce, response) pair can never be
     * replayed, and this clears only the caller's own slot, never a
     * different, still-pending challenge belonging to someone else. */
    bool challenge_ok = false;
    if (nonce_hex != NULL) {
        for (int i = 0; i < AUTH_CHALLENGE_MAX; i++) {
            if (s_pending_challenges[i].active &&
                (now - s_pending_challenges[i].issued_us) < AUTH_CHALLENGE_TTL_US &&
                strcmp(s_pending_challenges[i].nonce_hex, nonce_hex) == 0) {
                s_pending_challenges[i].active = false;
                challenge_ok = true;
                break;
            }
        }
    }

    if (!challenge_ok) {
        xSemaphoreGive(s_auth_lock);
        return AUTH_LOGIN_NO_CHALLENGE;
    }

    uint8_t nonce_bytes[16];
    uint8_t response_bytes[32];
    if (!hex_to_bytes(nonce_hex, nonce_bytes, sizeof(nonce_bytes)) ||
        !hex_to_bytes(response_hex, response_bytes, sizeof(response_bytes))) {
        xSemaphoreGive(s_auth_lock);
        return AUTH_LOGIN_NO_CHALLENGE; /* malformed request - same as no valid challenge */
    }

    provisioning_config_lock();
    uint8_t stored_key[32];
    memcpy(stored_key, s_cfg->auth.stored_key, sizeof(stored_key));
    provisioning_config_unlock();

    uint8_t expected[32];
    int rc = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                              stored_key, sizeof(stored_key),
                              nonce_bytes, sizeof(nonce_bytes),
                              expected);

    if (rc == 0 && constant_time_equal(expected, response_bytes, sizeof(expected))) {
        s_fail_count = 0;
        s_locked_until_us = 0;
        session_create_locked(token_hex_out);
        xSemaphoreGive(s_auth_lock);
        return AUTH_LOGIN_OK;
    }

    if (s_fail_count < UINT8_MAX) {
        s_fail_count++;
    }
    if (s_fail_count >= AUTH_LOCKOUT_THRESHOLD) {
        uint32_t duration_s = AUTH_LOCKOUT_BASE_S;
        uint32_t shift = s_fail_count - AUTH_LOCKOUT_THRESHOLD;
        for (uint32_t i = 0; i < shift && duration_s < AUTH_LOCKOUT_MAX_S; i++) {
            duration_s *= 2;
        }
        if (duration_s > AUTH_LOCKOUT_MAX_S) {
            duration_s = AUTH_LOCKOUT_MAX_S;
        }
        s_locked_until_us = now + (int64_t)duration_s * 1000000;
    }

    xSemaphoreGive(s_auth_lock);
    return AUTH_LOGIN_BAD_CREDENTIALS;
}

bool auth_check_session(const char *token_hex)
{
    if (token_hex == NULL) {
        return false;
    }

    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    bool ok = false;
    for (int i = 0; i < AUTH_SESSION_MAX; i++) {
        if (s_sessions[i].in_use && strcmp(s_sessions[i].token_hex, token_hex) == 0) {
            /* Two independent reasons a session can be too old to use -
             * review finding F15: idle timeout resets on every touch and
             * never expires a session kept continuously active on its own;
             * the absolute cap below does not reset, so even a session
             * touched every minute for hours is still cut off eventually. */
            if (now - s_sessions[i].last_seen_us > AUTH_SESSION_IDLE_US ||
                now - s_sessions[i].issued_us > AUTH_SESSION_ABSOLUTE_MAX_US) {
                s_sessions[i].in_use = false; /* timed out - drop it rather than extend it */
            } else {
                s_sessions[i].last_seen_us = now; /* sliding idle timeout only - issued_us never moves */
                ok = true;
            }
            break;
        }
    }
    xSemaphoreGive(s_auth_lock);
    return ok;
}

void auth_end_session(const char *token_hex)
{
    if (token_hex == NULL) {
        return;
    }
    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    for (int i = 0; i < AUTH_SESSION_MAX; i++) {
        if (s_sessions[i].in_use && strcmp(s_sessions[i].token_hex, token_hex) == 0) {
            s_sessions[i].in_use = false;
            break;
        }
    }
    xSemaphoreGive(s_auth_lock);
}

void auth_begin_password_salt(char *salt_hex_out, uint32_t *iterations_out)
{
    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    esp_fill_random(s_pending_salt.salt, sizeof(s_pending_salt.salt));
    s_pending_salt.iterations = AUTH_PBKDF2_ITERATIONS;
    s_pending_salt.active = true;
    s_pending_salt.issued_us = esp_timer_get_time();
    bytes_to_hex(s_pending_salt.salt, sizeof(s_pending_salt.salt), salt_hex_out);
    *iterations_out = s_pending_salt.iterations;
    xSemaphoreGive(s_auth_lock);
}

auth_set_password_result_t auth_commit_password(const char *stored_key_hex, const char *setup_secret_hex,
                                                  char *token_hex_out)
{
    /* First-ownership claim (review finding F03,
     * design/PROJECT_REVIEW_2026-09-10.md) checked *before* the pending
     * salt offer below is touched at all, deliberately - confirmed on real
     * hardware why order matters here: an earlier version checked this
     * after marking the salt consumed, so a single wrong-setup-secret
     * attempt burned the one-time salt offer and made the *next* attempt -
     * even with the correct secret - fail with AUTH_SET_NO_PENDING_SALT
     * instead. A locked-out operator with the right code in hand should
     * never see that. */
    if (!auth_password_is_set()) {
        provisioning_config_lock();
        uint8_t expected[sizeof(s_cfg->auth.setup_secret)];
        memcpy(expected, s_cfg->auth.setup_secret, sizeof(expected));
        bool open = s_cfg->auth.onboarding_open;
        provisioning_config_unlock();

        uint8_t given[sizeof(expected)];
        bool secret_ok = open && hex_to_bytes(setup_secret_hex, given, sizeof(given)) &&
                          constant_time_equal(expected, given, sizeof(expected));
        if (!secret_ok) {
            return AUTH_SET_BAD_SETUP_SECRET;
        }
    }

    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    bool salt_ok = s_pending_salt.active && (now - s_pending_salt.issued_us) < AUTH_SALT_OFFER_TTL_US;
    if (!salt_ok) {
        xSemaphoreGive(s_auth_lock);
        return AUTH_SET_NO_PENDING_SALT;
    }

    uint8_t stored_key[32];
    if (!hex_to_bytes(stored_key_hex, stored_key, sizeof(stored_key))) {
        xSemaphoreGive(s_auth_lock);
        return AUTH_SET_BAD_KEY;
    }

    /* Salt/iterations come from the offer this module generated, never from
     * the request - a client can't smuggle in a weaker pair. Single-use,
     * cleared here regardless of what happens next. */
    uint8_t salt_copy[16];
    memcpy(salt_copy, s_pending_salt.salt, sizeof(salt_copy));
    uint32_t iterations_copy = s_pending_salt.iterations;
    s_pending_salt.active = false;
    xSemaphoreGive(s_auth_lock);

    /* Scratch-copy-then-save-then-writeback, matching config_post_handler's
     * own discipline (main/web_ui.c): never mutate the live s_cfg unless
     * provisioning_save() actually persisted it, so RAM and NVS can't
     * silently diverge on a failed write. */
    gw_config_t work;
    provisioning_config_lock();
    memcpy(&work, s_cfg, sizeof(work));

    memcpy(work.auth.salt, salt_copy, sizeof(work.auth.salt));
    work.auth.iterations = iterations_copy;
    memcpy(work.auth.stored_key, stored_key, sizeof(work.auth.stored_key));
    work.auth.password_set = true;
    /* Real ownership is established now - close onboarding for good and
     * drop the secret rather than leave it sitting unused in NVS. A plain
     * reboot must not reopen it (same acceptance criterion auth_init()'s
     * own comment cites); only a factory reset (which wipes password_set
     * too, reopening a fresh window on next boot - see gw_config.h's v9
     * comment) or gwcfg-reopen-onboarding can, and the latter refuses once
     * password_set is true (auth_reopen_onboarding()'s own comment). */
    work.auth.onboarding_open = false;
    memset(work.auth.setup_secret, 0, sizeof(work.auth.setup_secret));

    esp_err_t err = provisioning_save(&work);
    if (err == ESP_OK) {
        provisioning_config_commit(&work);
    }
    provisioning_config_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist new credential: %s", esp_err_to_name(err));
        return AUTH_SET_SAVE_FAILED;
    }

    auth_drop_all_sessions();

    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    s_fail_count = 0;
    s_locked_until_us = 0;
    session_create_locked(token_hex_out);
    xSemaphoreGive(s_auth_lock);

    return AUTH_SET_OK;
}

void auth_drop_all_sessions(void)
{
    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    memset(s_sessions, 0, sizeof(s_sessions));
    xSemaphoreGive(s_auth_lock);
}

bool auth_onboarding_is_open(void)
{
    provisioning_config_lock();
    bool open = !s_cfg->auth.password_set && s_cfg->auth.onboarding_open;
    provisioning_config_unlock();
    return open;
}

bool auth_get_setup_secret_hex(char *out, size_t out_size)
{
    if (out == NULL || out_size < AUTH_SETUP_SECRET_HEX_LEN + 1) {
        return false;
    }
    provisioning_config_lock();
    bool open = !s_cfg->auth.password_set && s_cfg->auth.onboarding_open;
    uint8_t secret[sizeof(s_cfg->auth.setup_secret)];
    memcpy(secret, s_cfg->auth.setup_secret, sizeof(secret));
    provisioning_config_unlock();

    if (!open) {
        return false;
    }
    bytes_to_hex(secret, sizeof(secret), out);
    return true;
}

esp_err_t auth_reopen_onboarding(void)
{
    provisioning_config_lock();
    if (s_cfg->auth.password_set) {
        provisioning_config_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /* Same "mutate the global in place, no gw_config_t-sized stack local"
     * reasoning as auth_init() - this is reachable from the console task
     * (provisioning.c's REPL, 4096 bytes), which is more headroom than the
     * boot-time "main" task ever had, but there's no reason to reintroduce
     * the pattern that already caused one crash this session just because
     * this particular caller happens to have more room. */
    esp_fill_random(s_cfg->auth.setup_secret, sizeof(s_cfg->auth.setup_secret));
    s_cfg->auth.onboarding_ever_started = true;
    s_cfg->auth.onboarding_open = true;
    s_cfg->auth.onboarding_boots_remaining = AUTH_ONBOARDING_BOOT_BUDGET;

    esp_err_t err = provisioning_save(s_cfg);
    provisioning_config_unlock();
    return err;
}
