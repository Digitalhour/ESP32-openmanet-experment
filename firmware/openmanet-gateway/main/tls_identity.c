#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tls_identity.h"

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "provisioning.h"
#include "task_stats.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "tls_identity";

/* Points at the app's live in-RAM config, same convention as auth.c's
 * s_cfg - set by tls_identity_init(), read by every accessor below. */
static gw_config_t *s_cfg = NULL;

/* mbedtls_x509write_crt_der()/mbedtls_pk_write_key_der() both write from the
 * *end* of the buffer they're given (both functions' own doc comments,
 * esp-idf v5.5.1 mbedtls/include/mbedtls/x509_crt.h and pk.h) and return the
 * real length - the caller copies out exactly that many bytes from the tail.
 * These scratch buffers are sized well above gw_tls_identity_t's persisted
 * bounds (256/700 bytes - see that struct's own comment for why those hold)
 * so a slightly larger-than-typical ASN.1 encoding (e.g. a signature r/s
 * pair that both happen to need a leading zero byte) has room to write
 * before the bound check below ever has to reject it. */
#define GEN_KEY_BUF_LEN  512
#define GEN_CERT_BUF_LEN 1024

static esp_err_t generate_identity(const char *node_id, gw_tls_identity_t *out)
{
    esp_err_t result = ESP_FAIL;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    uint8_t *key_buf = NULL;
    uint8_t *cert_buf = NULL;
    int ret;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    /* mbedtls_entropy_func here draws on the ESP32's hardware RNG - ESP-IDF's
     * mbedtls port always defines MBEDTLS_ENTROPY_HARDWARE_ALT and errors out
     * at build time if it's ever unset (mbedtls/port/esp_hardware.c L15-17),
     * so this is the same true entropy source esp_random()/esp_fill_random()
     * use elsewhere in this project (auth.c), not a software PRNG. */
    static const char pers[] = "xiao-gw-tls-identity";
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers,
                                 sizeof(pers) - 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
        ESP_LOGE(TAG, "pk_setup failed: -0x%04x", -ret);
        goto cleanup;
    }
    /* P-256 (secp256r1): widely supported by browsers, and a fraction of the
     * key/signature size (and TLS handshake cost) an RSA key of comparable
     * strength would need - the smallest-footprint choice that every major
     * browser's default TLS config still accepts without extra flags. */
    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random,
                               &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "ecp_gen_key failed: -0x%04x", -ret);
        goto cleanup;
    }

    key_buf = malloc(GEN_KEY_BUF_LEN);
    cert_buf = malloc(GEN_CERT_BUF_LEN);
    if (key_buf == NULL || cert_buf == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ret = mbedtls_pk_write_key_der(&key, key_buf, GEN_KEY_BUF_LEN);
    if (ret < 0) {
        ESP_LOGE(TAG, "pk_write_key_der failed: -0x%04x", -ret);
        goto cleanup;
    }
    size_t key_len = (size_t)ret;
    if (key_len > sizeof(out->key_der)) {
        ESP_LOGE(TAG, "generated key (%u bytes) exceeds the %u-byte stored bound", (unsigned)key_len,
                 (unsigned)sizeof(out->key_der));
        goto cleanup;
    }
    memcpy(out->key_der, key_buf + GEN_KEY_BUF_LEN - key_len, key_len);
    out->key_der_len = (uint16_t)key_len;

    /* Self-signed: subject == issuer, signed with the same key it certifies -
     * there is no CA in this design (design/PROJECT_REVIEW_2026-09-10.md
     * finding F02: no CA can issue a certificate for a private IP). Trust
     * comes from the operator comparing tls_identity_get_fingerprint_hex()'s
     * output against what their browser shows, the same role an SSH host
     * key fingerprint plays - not from a certificate chain. */
    char dn[48];
    snprintf(dn, sizeof(dn), "CN=%s", (node_id != NULL && node_id[0] != '\0') ? node_id : "xiao-gateway");

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3); /* extensions require v3 */
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    ret = mbedtls_x509write_crt_set_subject_name(&crt, dn);
    if (ret != 0) {
        ESP_LOGE(TAG, "set_subject_name failed: -0x%04x", -ret);
        goto cleanup;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(&crt, dn);
    if (ret != 0) {
        ESP_LOGE(TAG, "set_issuer_name failed: -0x%04x", -ret);
        goto cleanup;
    }

    /* Fixed, wide validity window rather than derived from this board's own
     * clock: there is no RTC battery here, so at generation time (always
     * shortly after boot) the ESP32's clock may not have been set from NTP
     * yet, and a "not_before" timestamp of 1970 (or whatever the clock last
     * held) reads as suspicious to some TLS stacks. This range is checked
     * against the *verifier's* (the browser's) clock, not this device's, and
     * comfortably outlives any realistic deployment of this hardware. */
    ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20540101000000");
    if (ret != 0) {
        ESP_LOGE(TAG, "set_validity failed: -0x%04x", -ret);
        goto cleanup;
    }

    /* Serial number: real randomness, not a fixed "01" repeated on every
     * device this firmware ever provisions - purely cosmetic here (no CA
     * chain to disambiguate against), but a constant value across every
     * unit is the kind of thing that looks like a bug the first time two
     * devices' captures sit side by side. */
    uint8_t serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7Fu; /* DER INTEGER must be non-negative - clear the sign bit */
    ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    if (ret != 0) {
        ESP_LOGE(TAG, "set_serial_raw failed: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_x509write_crt_set_basic_constraints(&crt, /*is_ca=*/0, /*max_pathlen=*/-1);
    if (ret != 0) {
        ESP_LOGE(TAG, "set_basic_constraints failed: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_x509write_crt_der(&crt, cert_buf, GEN_CERT_BUF_LEN, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret < 0) {
        ESP_LOGE(TAG, "x509write_crt_der failed: -0x%04x", -ret);
        goto cleanup;
    }
    size_t cert_len = (size_t)ret;
    if (cert_len > sizeof(out->cert_der)) {
        ESP_LOGE(TAG, "generated cert (%u bytes) exceeds the %u-byte stored bound", (unsigned)cert_len,
                 (unsigned)sizeof(out->cert_der));
        goto cleanup;
    }
    memcpy(out->cert_der, cert_buf + GEN_CERT_BUF_LEN - cert_len, cert_len);
    out->cert_der_len = (uint16_t)cert_len;

    out->identity_set = true;
    result = ESP_OK;

cleanup:
    free(key_buf);
    free(cert_buf);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return result;
}

typedef struct {
    bool force; /* true: generate even if an identity already exists */
    esp_err_t result;
    SemaphoreHandle_t done;
} identity_worker_args_t;

/* Runs the *entire* scratch-copy/generate/save/writeback sequence, not just
 * generate_identity() - see GW_STACK_TLS_IDENTITY_GEN's own comment
 * (task_stats.h) for why that's load-bearing, not defensive. First cut of
 * this fix only moved generate_identity() itself to a dedicated task and
 * still crashed on real hardware: `gw_config_t work` is a several-KB local
 * (gw_tls_identity_t alone added ~960 bytes to that struct) that was still
 * being copied on whatever thin caller stack called this - for
 * tls_identity_init(), that's FreeRTOS's default "main" task
 * (CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584 in this project's sdkconfig), which
 * overflowed on that local alone, on top of provisioning_save()'s own frame,
 * independent of whether the crypto work itself was on a separate task.
 * Moving the whole body here removes the caller's stack as a variable
 * entirely, rather than trying to reason about how much of it each piece
 * needs. */
static void identity_worker_task(void *arg)
{
    identity_worker_args_t *args = (identity_worker_args_t *)arg;

    /* Scratch-copy-then-save-then-writeback, matching auth_commit_password()'s
     * own discipline (main/auth.c): never mutate the live s_cfg unless
     * provisioning_save() actually persisted it. */
    gw_config_t work;
    provisioning_config_lock();
    memcpy(&work, s_cfg, sizeof(work));
    provisioning_config_unlock();

    esp_err_t err;
    if (work.tls.identity_set && !args->force) {
        /* Nothing to do - identity_set flipped true between the caller's own
         * check and this task actually running (vanishingly unlikely, but
         * cheaper to handle than to assume away). */
        err = ESP_OK;
    } else {
        if (args->force) {
            work.tls.identity_set = false; /* generate_identity() overwrites the rest regardless */
        }
        err = generate_identity(work.node_id, &work.tls);
    }

    if (err == ESP_OK) {
        provisioning_config_lock();
        if (!args->force && provisioning_in_recovery_mode()) {
            /* Review finding F14's migration portion
             * (design/PROJECT_REVIEW_2026-09-10.md): this is the automatic,
             * no-operator-involved boot-time path (tls_identity_init(), not
             * the console's gwcfg-reset-tls-identity, which passes force=true
             * and is exempt - see provisioning_in_recovery_mode()'s own
             * comment). Found on real hardware: without this check, this
             * call would persist the whole live (recovery-defaulted) struct
             * the moment it ran, permanently overwriting a real config this
             * boot couldn't read but hadn't actually lost yet. Use the
             * freshly generated identity for this boot only - RAM, not NVS -
             * so the device still serves HTTPS, but the original NVS blob is
             * left alone for a future firmware (or the operator) to still
             * have a chance at. */
            memcpy(s_cfg, &work, sizeof(*s_cfg));
        } else {
            err = provisioning_save(&work);
            if (err == ESP_OK) {
                memcpy(s_cfg, &work, sizeof(*s_cfg));
            }
        }
        provisioning_config_unlock();
    }

    args->result = err;
    xSemaphoreGive(args->done);
    vTaskDelete(NULL);
}

static esp_err_t run_identity_worker(bool force)
{
    identity_worker_args_t args = {
        .force = force,
        .result = ESP_FAIL,
        .done = xSemaphoreCreateBinary(),
    };
    if (args.done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* "tls_id_gen", not "tls_identity_gen": FreeRTOS task names are bounded
     * by configMAX_TASK_NAME_LEN (16 in this project's sdkconfig -
     * CONFIG_FREERTOS_MAX_TASK_NAME_LEN), and xTaskGetHandle() - which
     * task_stats.c's gwcfg-tasks/GET /api/tasks uses to look this task up by
     * name - asserts strlen(name) < that limit (tasks.c's own
     * configASSERT). "tls_identity_gen" is exactly 16 characters and crashed
     * gwcfg-tasks on real hardware the first time it ran after this task
     * existed; confirmed by counting, not assumed, this time. */
    if (xTaskCreate(identity_worker_task, "tls_id_gen", GW_STACK_TLS_IDENTITY_GEN, &args, 5, NULL) !=
        pdPASS) {
        vSemaphoreDelete(args.done);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(args.done, portMAX_DELAY);
    vSemaphoreDelete(args.done);
    return args.result;
}

esp_err_t tls_identity_init(gw_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;

    if (cfg->tls.identity_set) {
        ESP_LOGI(TAG, "using existing HTTPS identity from NVS");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "no HTTPS identity in NVS - generating one (ECDSA P-256, self-signed)");
    esp_err_t err = run_identity_worker(/*force=*/false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "identity generation/persist failed: %s", esp_err_to_name(err));
        return err;
    }

    char fp[TLS_IDENTITY_FINGERPRINT_HEX_LEN + 1];
    tls_identity_get_fingerprint_hex(fp, sizeof(fp));
    ESP_LOGI(TAG, "generated HTTPS identity - certificate SHA-256 fingerprint: %s", fp);
    ESP_LOGI(TAG, "verify this fingerprint against what your browser shows before trusting it - "
                  "also available any time via 'gwcfg-show-cert'");
    return ESP_OK;
}

esp_err_t tls_identity_regenerate(gw_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;
    return run_identity_worker(/*force=*/true);
}

bool tls_identity_is_ready(void)
{
    return s_cfg != NULL && s_cfg->tls.identity_set;
}

const uint8_t *tls_identity_get_cert_der(size_t *len_out)
{
    if (!tls_identity_is_ready()) {
        if (len_out != NULL) {
            *len_out = 0;
        }
        return NULL;
    }
    if (len_out != NULL) {
        *len_out = s_cfg->tls.cert_der_len;
    }
    return s_cfg->tls.cert_der;
}

const uint8_t *tls_identity_get_key_der(size_t *len_out)
{
    if (!tls_identity_is_ready()) {
        if (len_out != NULL) {
            *len_out = 0;
        }
        return NULL;
    }
    if (len_out != NULL) {
        *len_out = s_cfg->tls.key_der_len;
    }
    return s_cfg->tls.key_der;
}

void tls_identity_get_fingerprint_hex(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!tls_identity_is_ready() || out_size < TLS_IDENTITY_FINGERPRINT_HEX_LEN + 1) {
        return;
    }

    uint8_t digest[32];
    mbedtls_sha256(s_cfg->tls.cert_der, s_cfg->tls.cert_der_len, digest, /*is224=*/0);

    char *p = out;
    for (size_t i = 0; i < sizeof(digest); i++) {
        p += snprintf(p, 4, "%02X%s", digest[i], (i + 1 < sizeof(digest)) ? ":" : "");
    }
}
