/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later
 *
 * The mesh password.
 *
 * Two peers that share a password authenticate with SAE (802.11-2020 12.4) over
 * Authentication frames, each side sending a Commit and a Confirm. That leaves both with the
 * same PMK. The peering Open and Confirm that follow are the Authenticated Mesh Peering
 * Exchange (14.5): they carry each side's nonce and group key inside an AMPE element that is
 * encrypted and authenticated with AES-SIV under the AEK derived from the PMK, with a MIC
 * element in front of it. Once the peering is established the pairwise key (MTK) comes out
 * of the PMK, the two nonces and the two link ids, and the chip gets three keys: the MTK, the
 * peer's group key for frames it sends to everyone, and our own group key.
 *
 * SAE itself, the EC maths and the SHA-256 KDF are hostap code that morselib already carries
 * (the esp-halow build prefixes it with mmint_). AES-SIV is written here on mbedtls, in the
 * form hostap uses: S2V-CMAC over the three associated-data vectors and the plaintext, then
 * AES-CTR. One detail differs between hostap versions: upstream binds the whole frame body up
 * to the MIC element into the MIC, the Morse SDK copy binds only its first six bytes
 * (MESH_RSN_FRAME_MIC_OFFSET). A HaLowLink 1 on Morse 2.7.6 does it the upstream way. Both are
 * accepted on receive, and whichever a peer uses is used back to it.
 *
 * Mode: hunting-and-pecking SAE (status 0), group 19, no PMF. That is what the Morse OpenWrt
 * mesh (HaLowLink, OpenMANET) runs; a hash-to-element Commit from a peer is logged and
 * ignored.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/aes.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include "mbedtls/ccm.h"
#include "mmwlan.h"

#include "mesh_sec.h"

#define TAG "mesh_sec"

/* ---------- hostap, as built into morselib ---------- */

struct crypto_bignum;
struct sae_temporary_data;
struct wpabuf;

enum sae_state
{
    SAE_NOTHING, SAE_COMMITTED, SAE_CONFIRMED, SAE_ACCEPTED
};

/* src/common/sae.h in the SDK's hostap tree. Allocated here, filled in there. */
struct sae_data
{
    enum sae_state state;
    uint16_t send_confirm;
    uint8_t pmk[64];
    size_t pmk_len;
    int akmp;
    uint32_t own_akm_suite_selector;
    uint32_t peer_akm_suite_selector;
    uint8_t pmkid[MESH_SEC_PMKID_LEN];
    struct crypto_bignum *peer_commit_scalar;
    struct crypto_bignum *peer_commit_scalar_accepted;
    int group;
    unsigned int sync;
    uint16_t rc;
    unsigned int h2e : 1;
    unsigned int pk : 1;
    unsigned int no_pw_id : 1;
    struct sae_temporary_data *tmp;
};

struct wpabuf
{
    size_t size;
    size_t used;
    uint8_t *buf;
    unsigned int flags;
};

int mmint_sae_set_group(struct sae_data *sae, int group);
void mmint_sae_clear_temp_data(struct sae_data *sae);
void mmint_sae_clear_data(struct sae_data *sae);
int mmint_sae_prepare_commit(const uint8_t *addr1, const uint8_t *addr2, const uint8_t *password,
                             size_t password_len, struct sae_data *sae);
int mmint_sae_process_commit(struct sae_data *sae);
int mmint_sae_write_commit(struct sae_data *sae, struct wpabuf *buf, const struct wpabuf *token,
                           const uint8_t *identifier, size_t identifier_len);
uint16_t mmint_sae_parse_commit(struct sae_data *sae, const uint8_t *data, size_t len, const uint8_t **token,
                                size_t *token_len, int *allowed_groups, int h2e, int *ie_offset);
int mmint_sae_write_confirm(struct sae_data *sae, struct wpabuf *buf);
int mmint_sae_check_confirm(struct sae_data *sae, const uint8_t *data, size_t len, int *ie_offset);
int mmint_sha256_prf(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t data_len,
                     uint8_t *buf, size_t buf_len);
struct wpabuf *mmint_wpabuf_alloc(size_t len);
void mmint_wpabuf_free(struct wpabuf *buf);

#define SAE_GROUP 19
#define SAE_SILENTLY_DISCARD 65535
#define SAE_COMMIT_BUF 1800
#define SAE_CONFIRM_BUF 1600
#define SAE_RETRANSMIT_US (2000 * 1000LL)
#define SAE_MAX_RETRIES 4
#define SAE_MAX_SYNC 5
#define SAE_BACKOFF_US (10 * 1000 * 1000LL)

#define STATUS_SAE_HASH_TO_ELEMENT 126

/* ---------- morselib driver: chip keys and station records ---------- */

struct mmdrv_key_conf
{
    bool is_pairwise;
    uint64_t tx_pn;
    uint16_t length;
    uint8_t key[32];
    uint8_t key_idx;
};

enum
{
    STA_NOTEXIST = 0, STA_NONE = 1, STA_AUTHENTICATED = 2, STA_ASSOCIATED = 3, STA_AUTHORIZED = 4
};

enum mmwlan_status mmint_mmdrv_install_key(uint16_t vif_id, uint16_t aid, struct mmdrv_key_conf *key_conf);
enum mmwlan_status mmint_mmdrv_disable_key(uint16_t vif_id, uint16_t aid, uint8_t hw_key_idx, bool is_pairwise);
enum mmwlan_status mmint_mmdrv_update_sta_state(uint16_t vif_id, uint16_t aid, const uint8_t *addr, int state);

#define KEY_IDX_PAIRWISE 0
#define KEY_IDX_MGTK 1
#ifndef SEC_NO_PEER_MGTK
#define SEC_NO_PEER_MGTK 0   /* bench: 1 = do not load the peer's group key, to test the pairwise path alone */
#endif


/* ---------- state ---------- */

#define NONCE_LEN 32
#define AEK_LEN 32
#define MTK_LEN 16
#define MGTK_LEN 16
#define KEY_RSC_LEN 8

static const uint8_t AKM_SAE[4] = { 0x00, 0x0f, 0xac, 0x08 };
static const uint8_t CIPHER_CCMP[4] = { 0x00, 0x0f, 0xac, 0x04 };

struct sec_peer
{
    uint8_t addr[6];
    struct sae_data *sae;
    int64_t last_tx_us;
    int64_t retry_after_us;      /* after a failed run, do not start again before this */
    uint8_t retries;
    bool ampe_ready;
    uint8_t my_nonce[NONCE_LEN], peer_nonce[NONCE_LEN];
    uint8_t aek[AEK_LEN];        /* AEK from the 32-byte PMK (upstream hostap) */
    uint8_t aek64[AEK_LEN];      /* AEK from the PMK buffer zero-padded to 64 bytes (the SDK's hostap) */
    uint8_t aek_mode;            /* 0 unknown, 1 aek, 2 aek64 */
    uint8_t mtk[MTK_LEN];
    uint8_t peer_mgtk[MGTK_LEN];
    uint64_t peer_mgtk_pn;
    bool have_peer_mgtk;
    bool keys_installed;
    uint8_t aad_mode;            /* what the peer binds into the MIC: 0 unknown, 1 whole body, 2 six bytes */
};

static bool s_enabled;
static uint8_t s_own[6];
static char s_password[64];
static uint16_t s_vif = 0xffff;
static uint8_t s_mgtk[MGTK_LEN];
static bool s_mgtk_installed;
static mesh_sec_send_fn s_send;
static struct sec_peer s_peers[MESH_SEC_MAX_PEERS];
static struct mesh_sec_stats s_stats;

static void mac_str(const uint8_t *m, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ---------- AES-SIV (RFC 5297), hostap's construction on mbedtls ---------- */

static int cmac_aes128(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t out[16])
{
    const mbedtls_cipher_info_t *info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (info == NULL)
    {
        return -1;
    }
    return mbedtls_cipher_cmac(info, key, 128, data, len, out) == 0 ? 0 : -1;
}

static void siv_dbl(uint8_t *pad)
{
    int carry = pad[0] & 0x80;
    for (int i = 0; i < 15; i++)
    {
        pad[i] = (uint8_t)((pad[i] << 1) | (pad[i + 1] >> 7));
    }
    pad[15] <<= 1;
    if (carry)
    {
        pad[15] ^= 0x87;
    }
}

static void siv_xor(uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++)
    {
        a[i] ^= b[i];
    }
}

static int siv_s2v(const uint8_t k1[16], size_t n, const uint8_t *addr[], const size_t *len, uint8_t mac[16])
{
    uint8_t zero[16] = { 0 }, tmp[16], tmp2[16];
    if (n == 0)
    {
        tmp[15] = 1;
        memset(tmp, 0, 15);
        return cmac_aes128(k1, tmp, 16, mac);
    }
    if (cmac_aes128(k1, zero, 16, tmp))
    {
        return -1;
    }
    size_t i;
    for (i = 0; i < n - 1; i++)
    {
        if (cmac_aes128(k1, addr[i], len[i], tmp2))
        {
            return -1;
        }
        siv_dbl(tmp);
        siv_xor(tmp, tmp2);
    }
    if (len[i] >= 16)
    {
        uint8_t *buf = malloc(len[i]);
        if (buf == NULL)
        {
            return -1;
        }
        memcpy(buf, addr[i], len[i]);
        for (int j = 0; j < 16; j++)
        {
            buf[len[i] - 16 + j] ^= tmp[j];
        }
        int r = cmac_aes128(k1, buf, len[i], mac);
        memset(buf, 0, len[i]);
        free(buf);
        return r;
    }
    siv_dbl(tmp);
    memset(tmp2, 0, 16);
    memcpy(tmp2, addr[i], len[i]);
    tmp2[len[i]] = 0x80;
    siv_xor(tmp, tmp2);
    return cmac_aes128(k1, tmp, 16, mac);
}

static int siv_ctr(const uint8_t k2[16], const uint8_t v[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t ctr[16], stream[16];
    size_t off = 0;
    memcpy(ctr, v, 16);
    ctr[8] &= 0x7f;
    ctr[12] &= 0x7f;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int r = mbedtls_aes_setkey_enc(&aes, k2, 128);
    if (r == 0)
    {
        r = mbedtls_aes_crypt_ctr(&aes, len, &off, ctr, stream, in, out);
    }
    mbedtls_aes_free(&aes);
    return r == 0 ? 0 : -1;
}

/* out = V (16 bytes) || CTR(plaintext). */
static int siv_encrypt(const uint8_t key[32], const uint8_t *pt, size_t pt_len, size_t n, const uint8_t *aad[],
                       const size_t *aad_len, uint8_t *out)
{
    const uint8_t *addr[4];
    size_t len[4];
    if (n > 3)
    {
        return -1;
    }
    for (size_t i = 0; i < n; i++)
    {
        addr[i] = aad[i];
        len[i] = aad_len[i];
    }
    addr[n] = pt;
    len[n] = pt_len;
    uint8_t v[16];
    if (siv_s2v(key, n + 1, addr, len, v))
    {
        return -1;
    }
    memcpy(out, v, 16);
    return siv_ctr(key + 16, v, pt, out + 16, pt_len);
}

/* in = V || ciphertext; out gets the plaintext (in_len - 16 bytes). */
static int siv_decrypt(const uint8_t key[32], const uint8_t *in, size_t in_len, size_t n, const uint8_t *aad[],
                       const size_t *aad_len, uint8_t *out)
{
    if (in_len < 16 || n > 3)
    {
        return -1;
    }
    size_t pt_len = in_len - 16;
    if (siv_ctr(key + 16, in, in + 16, out, pt_len))
    {
        return -1;
    }
    const uint8_t *addr[4];
    size_t len[4];
    for (size_t i = 0; i < n; i++)
    {
        addr[i] = aad[i];
        len[i] = aad_len[i];
    }
    addr[n] = out;
    len[n] = pt_len;
    uint8_t check[16];
    if (siv_s2v(key, n + 1, addr, len, check))
    {
        return -1;
    }
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
    {
        diff |= check[i] ^ in[i];
    }
    return diff == 0 ? 0 : -2;
}

/* ---------- setup ---------- */

bool mesh_sec_enabled(void)
{
    return s_enabled;
}

esp_err_t mesh_sec_init(const uint8_t *own_addr, const char *mesh_id, const char *password, mesh_sec_send_fn send)
{
    (void)mesh_id;
    if (password == NULL || password[0] == '\0' || strlen(password) >= sizeof(s_password) || send == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_own, own_addr, 6);
    strcpy(s_password, password);
    s_send = send;
    esp_fill_random(s_mgtk, sizeof(s_mgtk));
    memset(s_peers, 0, sizeof(s_peers));
    s_enabled = true;
    {
        /* RFC 5297 A.1: deterministic authenticated encryption test vector. */
        static const uint8_t key[32] = { 0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8, 0xf7, 0xf6, 0xf5, 0xf4, 0xf3, 0xf2, 0xf1, 0xf0,
                                         0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff };
        static const uint8_t ad[24] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
                                        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27 };
        static const uint8_t pt[14] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
        static const uint8_t want[30] = { 0x85, 0x63, 0x2d, 0x07, 0xc6, 0xe8, 0xf3, 0x7f, 0x95, 0x0a, 0xcd, 0x32, 0x0a, 0x2e, 0xcc, 0x93,
                                          0x40, 0xc0, 0x2b, 0x96, 0x90, 0xc4, 0xdc, 0x04, 0xda, 0xef, 0x7f, 0x6a, 0xfe, 0x5c };
        uint8_t out[30], back[14];
        const uint8_t *aad[1] = { ad };
        const size_t aad_len[1] = { sizeof(ad) };
        int e = siv_encrypt(key, pt, sizeof(pt), 1, aad, aad_len, out);
        int d = siv_decrypt(key, want, sizeof(want), 1, aad, aad_len, back);
        ESP_LOGI(TAG, "AES-SIV self test: encrypt %s, decrypt %s", (e == 0 && memcmp(out, want, 30) == 0) ? "OK" : "FAIL",
                 (d == 0 && memcmp(back, pt, 14) == 0) ? "OK" : "FAIL");
    }
    ESP_LOGI(TAG, "Mesh password set: SAE group %d, CCMP", SAE_GROUP);
    return ESP_OK;
}

void mesh_sec_set_vif(uint16_t vif_id)
{
    s_vif = vif_id;
}

esp_err_t mesh_sec_install_own_mgtk(void)
{
    if (!s_enabled || s_vif == 0xffff)
    {
        return ESP_ERR_INVALID_STATE;
    }
    struct mmdrv_key_conf k = { .is_pairwise = false, .tx_pn = 0, .length = MGTK_LEN, .key_idx = KEY_IDX_MGTK };
    memcpy(k.key, s_mgtk, MGTK_LEN);
    enum mmwlan_status st = mmint_mmdrv_install_key(s_vif, 0, &k);
    s_mgtk_installed = st == MMWLAN_SUCCESS;
    ESP_LOGI(TAG, "Own group key: %s (chip status %d)", s_mgtk_installed ? "installed" : "REFUSED", (int)st);
    return s_mgtk_installed ? ESP_OK : ESP_FAIL;
}

size_t mesh_sec_rsn_ie(uint8_t *out, size_t cap)
{
    /* RSN: version 1, group CCMP, one pairwise CCMP, one AKM SAE, capabilities 0. */
    static const uint8_t rsn[22] = {
        0x30, 20, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00,
        0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x08, 0x00, 0x00
    };
    if (cap < sizeof(rsn))
    {
        return 0;
    }
    memcpy(out, rsn, sizeof(rsn));
    return sizeof(rsn);
}

/* ---------- SAE ---------- */

static struct sec_peer *peer(int ni)
{
    return (ni >= 0 && ni < MESH_SEC_MAX_PEERS) ? &s_peers[ni] : NULL;
}

static void sae_free(struct sec_peer *p)
{
    if (p->sae != NULL)
    {
        mmint_sae_clear_data(p->sae);
        memset(p->sae, 0, sizeof(*p->sae));
        free(p->sae);
        p->sae = NULL;
    }
    p->retries = 0;
    p->ampe_ready = false;
}

static void sae_fail(struct sec_peer *p, const char *why)
{
    char a[18];
    mac_str(p->addr, a);
    ESP_LOGW(TAG, "SAE with %s failed: %s. Trying again in %d s", a, why, (int)(SAE_BACKOFF_US / 1000000));
    s_stats.sae_failed++;
    sae_free(p);
    p->retry_after_us = esp_timer_get_time() + SAE_BACKOFF_US;
}

static void send_auth(struct sec_peer *p, uint16_t transaction, uint16_t status, const struct wpabuf *b)
{
    size_t len = 6 + b->used;
    uint8_t *body = malloc(len);
    if (body == NULL)
    {
        return;
    }
    body[0] = 3;   /* SAE */
    body[1] = 0;
    body[2] = (uint8_t)transaction;
    body[3] = (uint8_t)(transaction >> 8);
    body[4] = (uint8_t)status;
    body[5] = (uint8_t)(status >> 8);
    memcpy(body + 6, b->buf, b->used);
    s_send(p->addr, body, len);
    free(body);
    p->last_tx_us = esp_timer_get_time();
}

static int send_commit(struct sec_peer *p, bool fresh)
{
    if (fresh && mmint_sae_prepare_commit(s_own, p->addr, (const uint8_t *)s_password, strlen(s_password), p->sae) < 0)
    {
        return -1;
    }
    struct wpabuf *b = mmint_wpabuf_alloc(SAE_COMMIT_BUF);
    if (b == NULL)
    {
        return -1;
    }
    int r = mmint_sae_write_commit(p->sae, b, NULL, NULL, 0);
    if (r == 0)
    {
        send_auth(p, 1, 0, b);
    }
    mmint_wpabuf_free(b);
    return r;
}

static int send_confirm(struct sec_peer *p)
{
    struct wpabuf *b = mmint_wpabuf_alloc(SAE_CONFIRM_BUF);
    if (b == NULL)
    {
        return -1;
    }
    int r = mmint_sae_write_confirm(p->sae, b);
    if (r == 0)
    {
        send_auth(p, 2, 0, b);
    }
    mmint_wpabuf_free(b);
    return r;
}

static bool sae_alloc(struct sec_peer *p)
{
    if (p->sae != NULL)
    {
        return true;
    }
    p->sae = calloc(1, sizeof(*p->sae));
    if (p->sae == NULL)
    {
        return false;
    }
    p->sae->state = SAE_NOTHING;
    p->sae->no_pw_id = 1;
    return true;
}

void mesh_sec_peer_reset(int ni)
{
    struct sec_peer *p = peer(ni);
    if (p == NULL)
    {
        return;
    }
    sae_free(p);
    memset(p, 0, sizeof(*p));
}

void mesh_sec_peer_start(int ni, const uint8_t *addr)
{
    struct sec_peer *p = peer(ni);
    if (p == NULL || !s_enabled)
    {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (memcmp(p->addr, addr, 6) != 0)
    {
        mesh_sec_peer_reset(ni);
        memcpy(p->addr, addr, 6);
    }
    if (p->sae != NULL || now < p->retry_after_us)
    {
        return;
    }
    if (!sae_alloc(p) || mmint_sae_set_group(p->sae, SAE_GROUP) < 0)
    {
        sae_fail(p, "no memory");
        return;
    }
    char a[18];
    mac_str(addr, a);
    ESP_LOGI(TAG, "SAE with %s: sending Commit", a);
    s_stats.sae_started++;
    if (send_commit(p, true) < 0)
    {
        sae_fail(p, "could not build the Commit");
        return;
    }
    p->sae->state = SAE_COMMITTED;
    p->sae->sync = 0;
    p->retries = 0;
}

bool mesh_sec_peer_accepted(int ni)
{
    struct sec_peer *p = peer(ni);
    return p != NULL && p->sae != NULL && p->sae->state == SAE_ACCEPTED;
}

bool mesh_sec_peer_busy(int ni)
{
    struct sec_peer *p = peer(ni);
    if (p == NULL)
    {
        return false;
    }
    if (p->sae != NULL && p->sae->state != SAE_ACCEPTED)
    {
        return true;
    }
    return p->sae == NULL && esp_timer_get_time() < p->retry_after_us;
}

enum mesh_sec_event mesh_sec_rx_auth(int ni, const uint8_t *addr, const uint8_t *frame, size_t len)
{
    struct sec_peer *p = peer(ni);
    if (p == NULL || !s_enabled || len < 30)
    {
        return MESH_SEC_NONE;
    }
    const uint8_t *body = frame + 24;
    size_t body_len = len - 24;
    uint16_t alg = get_le16(body), transaction = get_le16(body + 2), status = get_le16(body + 4);
    if (alg != 3)
    {
        return MESH_SEC_NONE;
    }
    char a[18];
    mac_str(addr, a);
    if (memcmp(p->addr, addr, 6) != 0)
    {
        mesh_sec_peer_reset(ni);
        memcpy(p->addr, addr, 6);
    }
    const uint8_t *fields = body + 6;
    size_t fields_len = body_len - 6;

    if (transaction == 1)
    {
        if (status == STATUS_SAE_HASH_TO_ELEMENT)
        {
            static bool said;
            if (!said)
            {
                said = true;
                ESP_LOGW(TAG, "%s wants hash-to-element SAE, which this build does not do; ignoring", a);
            }
            return MESH_SEC_NONE;
        }
        if (status != 0)
        {
            ESP_LOGW(TAG, "SAE Commit from %s with status %u, ignored", a, status);
            return MESH_SEC_NONE;
        }
        if (p->sae != NULL && p->sae->state == SAE_ACCEPTED)
        {
            ESP_LOGI(TAG, "%s is starting SAE over; dropping the old session", a);
            sae_free(p);
            p->retry_after_us = 0;
            /* Fall through with a fresh instance: their Commit is answered below. */
            if (!sae_alloc(p))
            {
                return MESH_SEC_RESTART;
            }
            int groups[2] = { SAE_GROUP, 0 };
            const uint8_t *token = NULL;
            size_t token_len = 0;
            uint16_t r = mmint_sae_parse_commit(p->sae, fields, fields_len, &token, &token_len, groups, 0, NULL);
            if (r != 0)
            {
                sae_free(p);
                return MESH_SEC_RESTART;
            }
            s_stats.sae_started++;
            if (send_commit(p, true) < 0 || mmint_sae_process_commit(p->sae) < 0 || send_confirm(p) < 0)
            {
                sae_fail(p, "could not answer the Commit");
                return MESH_SEC_RESTART;
            }
            p->sae->state = SAE_CONFIRMED;
            p->sae->sync = 0;
            return MESH_SEC_RESTART;
        }
        if (p->sae == NULL && esp_timer_get_time() < p->retry_after_us)
        {
            return MESH_SEC_NONE;   /* backing off after a failure; let them retry later */
        }
        if (!sae_alloc(p))
        {
            return MESH_SEC_NONE;
        }
        int groups[2] = { SAE_GROUP, 0 };
        const uint8_t *token = NULL;
        size_t token_len = 0;
        uint16_t r = mmint_sae_parse_commit(p->sae, fields, fields_len, &token, &token_len, groups, 0, NULL);
        if (r == SAE_SILENTLY_DISCARD)
        {
            return MESH_SEC_NONE;
        }
        if (r != 0)
        {
            ESP_LOGW(TAG, "SAE Commit from %s rejected (status %u)", a, r);
            return MESH_SEC_NONE;
        }
        switch (p->sae->state)
        {
            case SAE_NOTHING:
                ESP_LOGI(TAG, "SAE with %s: Commit received, answering", a);
                s_stats.sae_started++;
                if (send_commit(p, true) < 0)
                {
                    sae_fail(p, "could not build the Commit");
                    return MESH_SEC_NONE;
                }
                p->sae->state = SAE_COMMITTED;
                if (mmint_sae_process_commit(p->sae) < 0 || send_confirm(p) < 0)
                {
                    sae_fail(p, "could not process the Commit");
                    return MESH_SEC_NONE;
                }
                p->sae->state = SAE_CONFIRMED;
                p->sae->sync = 0;
                break;
            case SAE_COMMITTED:
                ESP_LOGI(TAG, "SAE with %s: Commit received, sending Confirm", a);
                if (mmint_sae_process_commit(p->sae) < 0 || send_confirm(p) < 0)
                {
                    sae_fail(p, "could not process the Commit");
                    return MESH_SEC_NONE;
                }
                p->sae->state = SAE_CONFIRMED;
                p->sae->sync = 0;
                break;
            case SAE_CONFIRMED:
                if (++p->sae->sync > SAE_MAX_SYNC)
                {
                    sae_fail(p, "too many Commits");
                    return MESH_SEC_NONE;
                }
                if (send_commit(p, true) < 0 || mmint_sae_process_commit(p->sae) < 0 || send_confirm(p) < 0)
                {
                    sae_fail(p, "could not process the repeated Commit");
                    return MESH_SEC_NONE;
                }
                break;
            default:
                break;
        }
        return MESH_SEC_NONE;
    }

    if (transaction == 2)
    {
        if (p->sae == NULL)
        {
            return MESH_SEC_NONE;
        }
        switch (p->sae->state)
        {
            case SAE_COMMITTED:
                /* Their Confirm before their Commit reached us: send our Commit again. */
                if (++p->sae->sync > SAE_MAX_SYNC)
                {
                    sae_fail(p, "no Commit from the peer");
                    return MESH_SEC_NONE;
                }
                send_commit(p, false);
                break;
            case SAE_CONFIRMED:
                if (mmint_sae_check_confirm(p->sae, fields, fields_len, NULL) == 0)
                {
                    p->sae->state = SAE_ACCEPTED;
                    p->sae->send_confirm = 0xffff;
                    mmint_sae_clear_temp_data(p->sae);
                    p->retries = 0;
                    s_stats.sae_done++;
                    ESP_LOGI(TAG, "SAE with %s: done, PMK agreed (pmk_len %u, group %d)", a, (unsigned)p->sae->pmk_len, p->sae->group);
                    return MESH_SEC_ACCEPTED;
                }
                sae_fail(p, "Confirm did not verify (different password?)");
                break;
            default:
                break;
        }
    }
    return MESH_SEC_NONE;
}

void mesh_sec_housekeeping(int64_t now_us)
{
    if (!s_enabled)
    {
        return;
    }
    for (int ni = 0; ni < MESH_SEC_MAX_PEERS; ni++)
    {
        struct sec_peer *p = &s_peers[ni];
        if (p->sae == NULL || p->sae->state == SAE_ACCEPTED || p->sae->state == SAE_NOTHING)
        {
            continue;
        }
        if (now_us - p->last_tx_us < SAE_RETRANSMIT_US)
        {
            continue;
        }
        if (++p->retries > SAE_MAX_RETRIES)
        {
            sae_fail(p, "no answer");
            continue;
        }
        if (p->sae->state == SAE_COMMITTED)
        {
            send_commit(p, false);
        }
        else
        {
            send_confirm(p);
        }
    }
}

/* ---------- AMPE ---------- */

static void minmax6(const uint8_t *a, const uint8_t *b, const uint8_t **lo, const uint8_t **hi)
{
    if (memcmp(a, b, 6) < 0)
    {
        *lo = a;
        *hi = b;
    }
    else
    {
        *lo = b;
        *hi = a;
    }
}

void mesh_sec_ampe_init(int ni)
{
    struct sec_peer *p = peer(ni);
    if (p == NULL || p->sae == NULL)
    {
        return;
    }
    esp_fill_random(p->my_nonce, NONCE_LEN);
    memset(p->peer_nonce, 0, NONCE_LEN);
    p->have_peer_mgtk = false;
    /* AEK = KDF-256(PMK, "AEK Derivation", AKM || min(MAC) || max(MAC)) */
    uint8_t ctx[4 + 12];
    const uint8_t *lo, *hi;
    minmax6(s_own, p->addr, &lo, &hi);
    memcpy(ctx, AKM_SAE, 4);
    memcpy(ctx + 4, lo, 6);
    memcpy(ctx + 10, hi, 6);
    mmint_sha256_prf(p->sae->pmk, 32, "AEK Derivation", ctx, sizeof(ctx), p->aek, AEK_LEN);
    mmint_sha256_prf(p->sae->pmk, 64, "AEK Derivation", ctx, sizeof(ctx), p->aek64, AEK_LEN);
    p->ampe_ready = true;
}

void mesh_sec_pmkid(int ni, uint8_t out[MESH_SEC_PMKID_LEN])
{
    struct sec_peer *p = peer(ni);
    if (p != NULL && p->sae != NULL)
    {
        memcpy(out, p->sae->pmkid, MESH_SEC_PMKID_LEN);
    }
    else
    {
        memset(out, 0, MESH_SEC_PMKID_LEN);
    }
}

#define AMPE_FIXED (4 + NONCE_LEN + NONCE_LEN)
#define AMPE_GTKDATA (MGTK_LEN + KEY_RSC_LEN + 4)
#define AAD_WHOLE_BODY 1
#define AAD_SIX_BYTES 2
#define MIC_OFFSET 6   /* Morse SDK hostap: bytes of the body bound into the MIC */

static size_t aad_body_len(uint8_t mode, size_t body_len)
{
    if (mode == AAD_SIX_BYTES)
    {
        return body_len < MIC_OFFSET ? body_len : MIC_OFFSET;
    }
    return body_len;
}

int mesh_sec_protect(int ni, const uint8_t *body, size_t body_len, bool is_open, uint8_t *out, size_t cap)
{
    struct sec_peer *p = peer(ni);
    if (p == NULL || !p->ampe_ready)
    {
        return -1;
    }
    const uint8_t *cat = body;
    size_t ampe_len = AMPE_FIXED + (is_open ? AMPE_GTKDATA : 0);
    size_t total = 2 + 16 + 2 + ampe_len;
    if (cap < total)
    {
        return -1;
    }
    uint8_t plain[2 + AMPE_FIXED + AMPE_GTKDATA];
    uint8_t *q = plain;
    *q++ = 139;                       /* AMPE */
    *q++ = (uint8_t)ampe_len;
    memcpy(q, CIPHER_CCMP, 4);
    q += 4;
    memcpy(q, p->my_nonce, NONCE_LEN);
    q += NONCE_LEN;
    memcpy(q, p->peer_nonce, NONCE_LEN);
    q += NONCE_LEN;
    if (is_open)
    {
        memcpy(q, s_mgtk, MGTK_LEN);
        q += MGTK_LEN;
        memset(q, 0, KEY_RSC_LEN);    /* our group key PN: fresh key, starts at 0 */
        q += KEY_RSC_LEN;
        memset(q, 0xff, 4);           /* GTK expiry: never */
        q += 4;
    }
    const uint8_t *aad[3] = { s_own, p->addr, cat };
    const size_t aad_len[3] = { 6, 6, aad_body_len(p->aad_mode ? p->aad_mode : AAD_WHOLE_BODY, body_len) };
    const uint8_t *aek = p->aek_mode == 2 ? p->aek64 : p->aek;
    out[0] = 140;                     /* MIC */
    out[1] = 16;
    if (siv_encrypt(aek, plain, 2 + ampe_len, 3, aad, aad_len, out + 2))
    {
        return -1;
    }
    memset(plain, 0, sizeof(plain));
    return (int)total;
}

int mesh_sec_process_ampe(int ni, const uint8_t *body, size_t body_len, bool is_open, const uint8_t *chosen_pmk,
                          const uint8_t *mic, size_t rest_len)
{
    const uint8_t *cat = body;
    struct sec_peer *p = peer(ni);
    if (p == NULL || !p->ampe_ready || p->sae == NULL || p->sae->state != SAE_ACCEPTED)
    {
        return -1;
    }
    if (chosen_pmk != NULL && memcmp(chosen_pmk, p->sae->pmkid, MESH_SEC_PMKID_LEN) != 0)
    {
        return -1;
    }
    if (rest_len < 16 + 2 + AMPE_FIXED)
    {
        return -1;
    }
    size_t plain_len = rest_len - 16;
    uint8_t *plain = malloc(plain_len);
    if (plain == NULL)
    {
        return -1;
    }
    const uint8_t *aad[3] = { p->addr, s_own, cat };
    size_t aad_len[3] = { 6, 6, 0 };
    /* Try the peer's known binding and key first, else every combination, and remember what
     * worked: whole body or six bytes in the MIC, AEK from a 32-byte or a 64-byte PMK buffer. */
    uint8_t modes[2] = { p->aad_mode ? p->aad_mode : AAD_WHOLE_BODY, p->aad_mode ? 0 : AAD_SIX_BYTES };
    uint8_t keys[2] = { p->aek_mode ? p->aek_mode : 1, p->aek_mode ? 0 : 2 };
    int r = -2;
    for (int k = 0; k < 2 && r != 0; k++)
    {
        if (keys[k] == 0)
        {
            break;
        }
        for (int m = 0; m < 2 && r != 0; m++)
        {
            if (modes[m] == 0)
            {
                break;
            }
            aad_len[2] = aad_body_len(modes[m], body_len);
            r = siv_decrypt(keys[k] == 2 ? p->aek64 : p->aek, mic, rest_len, 3, aad, aad_len, plain);
            if (r == 0 && (p->aad_mode != modes[m] || p->aek_mode != keys[k]))
            {
                p->aad_mode = modes[m];
                p->aek_mode = keys[k];
                char a[18];
                mac_str(p->addr, a);
                ESP_LOGI(TAG, "%s: peering MIC binds %s, AEK from a %d-byte PMK buffer", a,
                         modes[m] == AAD_WHOLE_BODY ? "the whole body" : "six bytes", keys[k] == 2 ? 64 : 32);
            }
        }
    }
    if (r != 0)
    {
        free(plain);
        s_stats.ampe_rejected++;
        return -2;
    }
    r = -1;
    if (plain[0] == 139 && plain[1] >= AMPE_FIXED && 2 + plain[1] <= plain_len)
    {
        const uint8_t *e = plain + 2;
        const uint8_t *their_nonce = e + 4, *our_nonce_echo = e + 4 + NONCE_LEN;
        static const uint8_t zeros[NONCE_LEN] = { 0 };
        if (memcmp(our_nonce_echo, zeros, NONCE_LEN) == 0 || memcmp(our_nonce_echo, p->my_nonce, NONCE_LEN) == 0)
        {
            memcpy(p->peer_nonce, their_nonce, NONCE_LEN);
            if (is_open && plain[1] >= AMPE_FIXED + AMPE_GTKDATA)
            {
                const uint8_t *g = e + AMPE_FIXED;
                memcpy(p->peer_mgtk, g, MGTK_LEN);
                uint64_t pn = 0;
                for (int i = 5; i >= 0; i--)
                {
                    pn = (pn << 8) | g[MGTK_LEN + i];
                }
                p->peer_mgtk_pn = pn;
                p->have_peer_mgtk = true;
            }
            r = 0;
        }
    }
    memset(plain, 0, plain_len);
    free(plain);
    return r;
}

static enum mmwlan_status sta_state(const uint8_t *addr, uint16_t aid, int state)
{
    return mmint_mmdrv_update_sta_state(s_vif, aid, addr, state);
}

esp_err_t mesh_sec_establish(int ni, uint16_t aid, uint16_t llid, uint16_t plid)
{
    struct sec_peer *p = peer(ni);
    if (p == NULL || !p->ampe_ready || p->sae == NULL || s_vif == 0xffff)
    {
        return ESP_ERR_INVALID_STATE;
    }
    char a[18];
    mac_str(p->addr, a);

    /* MTK = KDF-128(PMK, "Temporal Key Derivation", min(nonces) || max(nonces) ||
     *       min(lids) || max(lids) || AKM || min(MAC) || max(MAC)) */
    uint8_t ctx[2 * NONCE_LEN + 4 + 4 + 12];
    uint8_t *q = ctx;
    if (memcmp(p->my_nonce, p->peer_nonce, NONCE_LEN) < 0)
    {
        memcpy(q, p->my_nonce, NONCE_LEN);
        memcpy(q + NONCE_LEN, p->peer_nonce, NONCE_LEN);
    }
    else
    {
        memcpy(q, p->peer_nonce, NONCE_LEN);
        memcpy(q + NONCE_LEN, p->my_nonce, NONCE_LEN);
    }
    q += 2 * NONCE_LEN;
    uint16_t lo = llid < plid ? llid : plid, hi = llid < plid ? plid : llid;
    q[0] = (uint8_t)lo;
    q[1] = (uint8_t)(lo >> 8);
    q[2] = (uint8_t)hi;
    q[3] = (uint8_t)(hi >> 8);
    q += 4;
    memcpy(q, AKM_SAE, 4);
    q += 4;
    const uint8_t *mlo, *mhi;
    minmax6(s_own, p->addr, &mlo, &mhi);
    memcpy(q, mlo, 6);
    memcpy(q + 6, mhi, 6);
    mmint_sha256_prf(p->sae->pmk, 32, "Temporal Key Derivation", ctx, sizeof(ctx), p->mtk, MTK_LEN);

    /* Same order as mac80211 drives this chip: the station becomes known and associated, the
     * keys go in, then it is authorized. */
    enum mmwlan_status st;
    enum mmwlan_status s1 = sta_state(p->addr, aid, STA_AUTHENTICATED);
    enum mmwlan_status s2 = sta_state(p->addr, aid, STA_ASSOCIATED);
    ESP_LOGI(TAG, "Station record for %s (aid %u): authenticated %d, associated %d", a, aid, (int)s1, (int)s2);
#ifndef SEC_NO_RX_KEYS
#define SEC_NO_RX_KEYS 0   /* bench: 1 = install no keys for peers, so their frames arrive raw for inspection */
#endif
    struct mmdrv_key_conf k = { .is_pairwise = true, .tx_pn = 0, .length = MTK_LEN, .key_idx = KEY_IDX_PAIRWISE };
    memcpy(k.key, p->mtk, MTK_LEN);
    st = SEC_NO_RX_KEYS ? MMWLAN_SUCCESS : mmint_mmdrv_install_key(s_vif, aid, &k);
    if (st != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "Chip refused the pairwise key for %s (aid %u, status %d)", a, aid, (int)st);
        return ESP_FAIL;
    }
    if (p->have_peer_mgtk && !SEC_NO_PEER_MGTK)
    {
        struct mmdrv_key_conf g = { .is_pairwise = false, .tx_pn = p->peer_mgtk_pn, .length = MGTK_LEN,
                                    .key_idx = KEY_IDX_MGTK };
        memcpy(g.key, p->peer_mgtk, MGTK_LEN);
        st = SEC_NO_RX_KEYS ? MMWLAN_SUCCESS : mmint_mmdrv_install_key(s_vif, aid, &g);
        if (st != MMWLAN_SUCCESS)
        {
            ESP_LOGW(TAG, "Chip refused the group key of %s (aid %u, status %d)", a, aid, (int)st);
        }
    }
    else
    {
        ESP_LOGW(TAG, "%s sent no group key; its broadcasts will not decrypt", a);
    }
    st = sta_state(p->addr, aid, STA_AUTHORIZED);
    if (st != MMWLAN_SUCCESS)
    {
        ESP_LOGW(TAG, "Chip refused to authorize %s (aid %u, status %d)", a, aid, (int)st);
    }
    p->keys_installed = true;
    s_stats.keys_installed++;
    ESP_LOGI(TAG, "Keys in place for %s (aid %u): pairwise%s", a, aid, p->have_peer_mgtk ? " and group" : "");
    return ESP_OK;
}

void mesh_sec_peer_lost(int ni, uint16_t aid)
{
    struct sec_peer *p = peer(ni);
    if (p == NULL || !s_enabled)
    {
        return;
    }
    if (p->keys_installed && s_vif != 0xffff)
    {
        mmint_mmdrv_disable_key(s_vif, aid, KEY_IDX_PAIRWISE, true);
        if (p->have_peer_mgtk)
        {
            mmint_mmdrv_disable_key(s_vif, aid, KEY_IDX_MGTK, false);
        }
        sta_state(p->addr, aid, STA_NONE);
        sta_state(p->addr, aid, STA_NOTEXIST);
    }
    p->keys_installed = false;
    p->have_peer_mgtk = false;
    p->ampe_ready = false;
    memset(p->mtk, 0, sizeof(p->mtk));
    /* A new peering means a new SAE run, as Linux does it without PMKSA caching. */
    sae_free(p);
}

bool mesh_sec_peer_keyed(int ni)
{
    struct sec_peer *p = peer(ni);
    return p != NULL && p->keys_installed;
}

void mesh_sec_get_stats(struct mesh_sec_stats *out)
{
    *out = s_stats;
}
