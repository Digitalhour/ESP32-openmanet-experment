/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * batman-adv (B.A.T.M.A.N. V) for a microcontroller. See batadv.h and
 * docs/batman-adv-on-esp32.md. Written against the Linux kernel's net/batman-adv as the
 * reference; function names below mirror the kernel's where the behaviour is the same.
 */

#include "batadv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "batadv";

/* ---------- Wire format (include/uapi/linux/batadv_packet.h, compat 15) ---------- */

#define COMPAT_VERSION 15

#define PT_BCAST 0x01
#define PT_ELP 0x03
#define PT_OGM2 0x04
#define PT_UNICAST 0x40
#define PT_UNICAST_FRAG 0x41
#define PT_UNICAST_4ADDR 0x42
#define PT_ICMP 0x43
#define PT_UNICAST_TVLV 0x44

#define TVLV_TT 0x04
#define TVLV_ROAM 0x05

#define TT_OGM_DIFF 0x01
#define TT_REQUEST 0x02
#define TT_RESPONSE 0x04
#define TT_FULL_TABLE 0x10

#define TT_CLIENT_DEL 0x0001
#define TT_CLIENT_ROAM 0x0002
#define TT_CLIENT_WIFI 0x0010
#define TT_CLIENT_ISOLA 0x0020
#define TT_CLIENT_NOPURGE 0x0100
#define TT_CLIENT_NEW 0x0200
#define TT_CLIENT_TEMP 0x0800
#define TT_SYNC_MASK 0x00F0

#define TTL_DEFAULT 50
#define JITTER_MS 20
#define TQ_MAX_VALUE 255
#define WINDOW_SIZE 64
#define OGM_MAX_ORIGDIFF 5
#define EXPECTED_SEQNO_RANGE 65536
#define RESET_PROTECTION_US (30 * 1000000LL)
#define PURGE_TIMEOUT_US (200 * 1000000LL)
#define TT_REQUEST_TIMEOUT_US (3 * 1000000LL)
#define FRAG_MAX_FRAGMENTS 16
#define FRAG_MAX_FRAG_SIZE 1280
#define FRAG_TIMEOUT_US (10 * 1000000LL)
#define FRAG_CHAINS 8
#define THROUGHPUT_MAX 0xFFFFFFFFu

struct __attribute__((packed)) eth_hdr
{
    uint8_t dst[6], src[6];
    uint16_t type;
};

struct __attribute__((packed)) elp_packet
{
    uint8_t packet_type, version;
    uint8_t orig[6];
    uint32_t seqno;
    uint32_t elp_interval;
};

struct __attribute__((packed)) ogm2_packet
{
    uint8_t packet_type, version, ttl, flags;
    uint32_t seqno;
    uint8_t orig[6];
    uint16_t tvlv_len;
    uint32_t throughput;
};

struct __attribute__((packed)) tvlv_hdr
{
    uint8_t type, version;
    uint16_t len;
};

struct __attribute__((packed)) tvlv_tt_data
{
    uint8_t flags, ttvn;
    uint16_t num_vlan;
};

struct __attribute__((packed)) tvlv_tt_vlan
{
    uint32_t crc;
    uint16_t vid, reserved;
};

struct __attribute__((packed)) tvlv_tt_change
{
    uint8_t flags, reserved[3];
    uint8_t addr[6];
    uint16_t vid;
};

struct __attribute__((packed)) unicast_packet
{
    uint8_t packet_type, version, ttl, ttvn;
    uint8_t dest[6];
};

struct __attribute__((packed)) unicast_4addr_packet
{
    struct unicast_packet u;
    uint8_t src[6];
    uint8_t subtype, reserved;
};

struct __attribute__((packed)) bcast_packet
{
    uint8_t packet_type, version, ttl, reserved;
    uint32_t seqno;
    uint8_t orig[6];
};

struct __attribute__((packed)) frag_packet
{
    uint8_t packet_type, version, ttl;
    uint8_t no_prio;             /* no:4 (high bits), priority:3, reserved:1 */
    uint8_t dest[6];
    uint8_t orig[6];
    uint16_t seqno;
    uint16_t total_size;
};

struct __attribute__((packed)) unicast_tvlv_packet
{
    uint8_t packet_type, version, ttl, reserved;
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t tvlv_len;
    uint16_t align;
};

static inline uint16_t be16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

/* ---------- State ---------- */

struct neigh
{
    bool used;
    uint8_t addr[6];
    int64_t last_seen;
    bool seq_valid;
    uint32_t elp_seqno;
    uint32_t elp_interval;
};

struct orig
{
    bool used;
    uint8_t addr[6];
    int64_t last_seen;
    bool seqno_valid;
    uint32_t last_real_seqno;
    int64_t ogm_reset_us;
    struct
    {
        bool valid;
        uint32_t throughput;
        uint32_t seqno;
    } via[BATADV_MAX_NEIGHBOURS];
    int router;
    bool fwd_valid;
    uint32_t last_fwd_seqno;
    uint8_t last_ttvn;
    bool tt_init;
    int64_t tt_req_us;
    bool bcast_valid;
    uint32_t last_bcast_seqno;
    uint64_t bcast_bits;
    int64_t bcast_reset_us;
};

struct tt_local
{
    bool used;
    uint8_t addr[6];
    uint16_t flags;
    bool pending_del;
};

struct tt_global
{
    bool used;
    uint8_t addr[6];
    int orig;
    uint8_t flags;
};

struct frag_chain
{
    bool used;
    uint8_t orig[6];
    uint16_t seqno;
    uint16_t total;
    uint16_t have;
    int64_t t;
    struct
    {
        bool used;
        uint8_t no;
        uint16_t len;
        uint8_t *data;
    } part[FRAG_MAX_FRAGMENTS];
};

static struct batadv_config s_cfg;
static SemaphoreHandle_t s_lock;
static struct neigh s_neigh[BATADV_MAX_NEIGHBOURS];
static struct orig s_orig[BATADV_MAX_ORIGINATORS];
static struct tt_local s_tt_local[BATADV_MAX_TT_LOCAL];
static struct tt_global s_tt_global[BATADV_MAX_TT_GLOBAL];
static bool s_tt_dirty;
static uint8_t s_ttvn;
static uint32_t s_local_crc;
static uint32_t s_elp_seqno, s_ogm_seqno, s_bcast_seqno;
static uint16_t s_frag_seqno;
static struct frag_chain s_frag[FRAG_CHAINS];
static struct batadv_stats s_stats;
static uint32_t s_crc_table[256];
static const uint8_t BROADCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static inline bool mac_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }
static inline bool mac_is_group(const uint8_t *a) { return (a[0] & 1) != 0; }
static inline int64_t now_us(void) { return esp_timer_get_time(); }

static void mac_str(const uint8_t *m, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* crc32c, Castagnoli, as the kernel's crc32c(seed, data, len): no inversion before or after. */
static void crc32c_init(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
        {
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        }
        s_crc_table[i] = c;
    }
}

static uint32_t crc32c(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
    {
        crc = s_crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }
    return crc;
}

/* One entry's contribution to a translation table CRC (batadv_tt_local_crc / global_crc). */
static uint32_t tt_entry_crc(const uint8_t *addr, uint16_t vid, uint16_t flags)
{
    uint16_t vid_be = be16(vid);
    uint8_t f = (uint8_t)(flags & TT_SYNC_MASK);
    uint32_t c = crc32c(0, &vid_be, 2);
    c = crc32c(c, &f, 1);
    return crc32c(c, addr, 6);
}

/* ---------- Tables ---------- */

static int neigh_find(const uint8_t *addr)
{
    for (int i = 0; i < BATADV_MAX_NEIGHBOURS; i++)
    {
        if (s_neigh[i].used && mac_eq(s_neigh[i].addr, addr))
        {
            return i;
        }
    }
    return -1;
}

static int neigh_get_or_create(const uint8_t *addr)
{
    int i = neigh_find(addr);
    if (i >= 0)
    {
        return i;
    }
    int oldest = -1;
    for (int k = 0; k < BATADV_MAX_NEIGHBOURS; k++)
    {
        if (!s_neigh[k].used)
        {
            oldest = k;
            break;
        }
        if (oldest < 0 || s_neigh[k].last_seen < s_neigh[oldest].last_seen)
        {
            oldest = k;
        }
    }
    memset(&s_neigh[oldest], 0, sizeof(s_neigh[oldest]));
    s_neigh[oldest].used = true;
    memcpy(s_neigh[oldest].addr, addr, 6);
    char a[18];
    mac_str(addr, a);
    ESP_LOGI(TAG, "neighbour %s appeared", a);
    return oldest;
}

static int orig_find(const uint8_t *addr)
{
    for (int i = 0; i < BATADV_MAX_ORIGINATORS; i++)
    {
        if (s_orig[i].used && mac_eq(s_orig[i].addr, addr))
        {
            return i;
        }
    }
    return -1;
}

static int orig_get_or_create(const uint8_t *addr)
{
    int i = orig_find(addr);
    if (i >= 0)
    {
        return i;
    }
    int oldest = -1;
    for (int k = 0; k < BATADV_MAX_ORIGINATORS; k++)
    {
        if (!s_orig[k].used)
        {
            oldest = k;
            break;
        }
        if (oldest < 0 || s_orig[k].last_seen < s_orig[oldest].last_seen)
        {
            oldest = k;
        }
    }
    memset(&s_orig[oldest], 0, sizeof(s_orig[oldest]));
    s_orig[oldest].used = true;
    s_orig[oldest].router = -1;
    memcpy(s_orig[oldest].addr, addr, 6);
    char a[18];
    mac_str(addr, a);
    ESP_LOGI(TAG, "originator %s appeared", a);
    return oldest;
}

static void tt_global_del_orig(int oi)
{
    for (int i = 0; i < BATADV_MAX_TT_GLOBAL; i++)
    {
        if (s_tt_global[i].used && s_tt_global[i].orig == oi)
        {
            s_tt_global[i].used = false;
        }
    }
}

static int tt_global_find(const uint8_t *addr)
{
    for (int i = 0; i < BATADV_MAX_TT_GLOBAL; i++)
    {
        if (s_tt_global[i].used && mac_eq(s_tt_global[i].addr, addr))
        {
            return i;
        }
    }
    return -1;
}

static void tt_global_add(const uint8_t *addr, int oi, uint8_t flags)
{
    int i = tt_global_find(addr);
    if (i < 0)
    {
        for (int k = 0; k < BATADV_MAX_TT_GLOBAL; k++)
        {
            if (!s_tt_global[k].used)
            {
                i = k;
                break;
            }
        }
        if (i < 0)
        {
            return;
        }
        s_tt_global[i].used = true;
        memcpy(s_tt_global[i].addr, addr, 6);
    }
    s_tt_global[i].orig = oi;
    s_tt_global[i].flags = flags;
}

static void tt_global_del(const uint8_t *addr, int oi)
{
    int i = tt_global_find(addr);
    if (i >= 0 && s_tt_global[i].orig == oi)
    {
        s_tt_global[i].used = false;
    }
}

static uint32_t tt_global_crc(int oi)
{
    uint32_t crc = 0;
    for (int i = 0; i < BATADV_MAX_TT_GLOBAL; i++)
    {
        const struct tt_global *g = &s_tt_global[i];
        if (g->used && g->orig == oi && !(g->flags & (TT_CLIENT_ROAM | TT_CLIENT_TEMP)))
        {
            crc ^= tt_entry_crc(g->addr, 0, g->flags);
        }
    }
    return crc;
}

static uint32_t tt_local_crc(void)
{
    uint32_t crc = 0;
    for (int i = 0; i < BATADV_MAX_TT_LOCAL; i++)
    {
        const struct tt_local *l = &s_tt_local[i];
        if (l->used && !(l->flags & TT_CLIENT_NEW) && !l->pending_del)
        {
            crc ^= tt_entry_crc(l->addr, 0, l->flags);
        }
    }
    return crc;
}

/* Sequence window protection (batadv_window_protected). Returns true when the packet must be
 * dropped; sets *reset when the window was restarted (a rebooted node). */
static bool window_protected(int32_t seq_diff, int64_t *last_reset, bool *reset)
{
    *reset = false;
    if (seq_diff < -WINDOW_SIZE || seq_diff >= EXPECTED_SEQNO_RANGE)
    {
        if (now_us() - *last_reset < RESET_PROTECTION_US)
        {
            return true;
        }
        *last_reset = now_us();
        *reset = true;
    }
    return false;
}

/* ---------- Sending ---------- */

static esp_err_t send_frame(const uint8_t *dst, const void *payload, size_t len)
{
    uint8_t *f = malloc(14 + len);
    if (f == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    struct eth_hdr *e = (struct eth_hdr *)f;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, s_cfg.hardif_mac, 6);
    e->type = be16(BATADV_ETHERTYPE);
    memcpy(f + 14, payload, len);
    esp_err_t err = s_cfg.link_send(f, 14 + len);
    free(f);
    if (err != ESP_OK)
    {
        s_stats.link_tx_failed++;
    }
    return err;
}

static void send_elp(void)
{
    struct elp_packet p = { 0 };
    p.packet_type = PT_ELP;
    p.version = COMPAT_VERSION;
    memcpy(p.orig, s_cfg.hardif_mac, 6);
    p.seqno = be32(s_elp_seqno++);
    p.elp_interval = be32(s_cfg.elp_interval_ms);
    /* Linux pads the broadcast ELP with four zero bytes. */
    uint8_t buf[sizeof(p) + 4] = { 0 };
    memcpy(buf, &p, sizeof(p));
    if (send_frame(BROADCAST, buf, sizeof(buf)) == ESP_OK)
    {
        s_stats.elp_tx++;
    }
}

/* Commits pending local table changes (batadv_tt_local_commit_changes) and writes the TT TVLV
 * for our own OGM into buf. Returns the TVLV's total length including its header. */
static size_t build_tt_tvlv(uint8_t *buf, size_t cap)
{
    struct tvlv_tt_change changes[BATADV_MAX_TT_LOCAL];
    int n = 0;
    if (s_tt_dirty)
    {
        s_ttvn++;
        for (int i = 0; i < BATADV_MAX_TT_LOCAL; i++)
        {
            struct tt_local *l = &s_tt_local[i];
            if (!l->used)
            {
                continue;
            }
            if (l->pending_del)
            {
                memset(&changes[n], 0, sizeof(changes[n]));
                changes[n].flags = TT_CLIENT_DEL;
                memcpy(changes[n].addr, l->addr, 6);
                n++;
                l->used = false;
            }
            else if (l->flags & TT_CLIENT_NEW)
            {
                l->flags &= ~TT_CLIENT_NEW;
                memset(&changes[n], 0, sizeof(changes[n]));
                changes[n].flags = (uint8_t)l->flags;
                memcpy(changes[n].addr, l->addr, 6);
                n++;
            }
        }
        s_tt_dirty = false;
        s_local_crc = tt_local_crc();
    }
    size_t need = sizeof(struct tvlv_hdr) + sizeof(struct tvlv_tt_data) + sizeof(struct tvlv_tt_vlan) + n * sizeof(struct tvlv_tt_change);
    if (need > cap)
    {
        n = 0;
        need = sizeof(struct tvlv_hdr) + sizeof(struct tvlv_tt_data) + sizeof(struct tvlv_tt_vlan);
    }
    struct tvlv_hdr *h = (struct tvlv_hdr *)buf;
    h->type = TVLV_TT;
    h->version = 1;
    h->len = be16((uint16_t)(need - sizeof(*h)));
    struct tvlv_tt_data *d = (struct tvlv_tt_data *)(buf + sizeof(*h));
    d->flags = TT_OGM_DIFF;
    d->ttvn = s_ttvn;
    d->num_vlan = be16(1);
    struct tvlv_tt_vlan *v = (struct tvlv_tt_vlan *)(d + 1);
    v->crc = be32(s_local_crc);
    v->vid = 0;
    v->reserved = 0;
    memcpy(v + 1, changes, n * sizeof(changes[0]));
    return need;
}

static void send_ogm(void)
{
    uint8_t buf[sizeof(struct ogm2_packet) + 4 + 4 + 8 + BATADV_MAX_TT_LOCAL * 12];
    struct ogm2_packet *o = (struct ogm2_packet *)buf;
    size_t tvlv_len = build_tt_tvlv(buf + sizeof(*o), sizeof(buf) - sizeof(*o));
    o->packet_type = PT_OGM2;
    o->version = COMPAT_VERSION;
    o->ttl = TTL_DEFAULT;
    o->flags = 0;
    o->seqno = be32(s_ogm_seqno++);
    memcpy(o->orig, s_cfg.hardif_mac, 6);
    o->tvlv_len = be16((uint16_t)tvlv_len);
    o->throughput = be32(THROUGHPUT_MAX);
    if (send_frame(BROADCAST, buf, sizeof(*o) + tvlv_len) == ESP_OK)
    {
        s_stats.ogm_tx++;
    }
}

/* Next hop for an originator, or -1. */
static int router_for(int oi)
{
    if (oi < 0 || s_orig[oi].router < 0 || !s_neigh[s_orig[oi].router].used)
    {
        return -1;
    }
    return s_orig[oi].router;
}

/* Sends a complete batman packet (header plus payload) towards an originator, fragmenting if
 * the link cannot carry it (batadv_send_skb_to_orig / batadv_frag_send_packet). */
static esp_err_t send_to_orig(int oi, const uint8_t *pkt, size_t len)
{
    int r = router_for(oi);
    if (r < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }
    const uint8_t *next = s_neigh[r].addr;
    size_t link_payload = s_cfg.link_mtu - 14;
    if (len <= link_payload)
    {
        return send_frame(next, pkt, len);
    }
    size_t max_piece = (link_payload < FRAG_MAX_FRAG_SIZE ? link_payload : FRAG_MAX_FRAG_SIZE) - sizeof(struct frag_packet);
    size_t n = (len + max_piece - 1) / max_piece;
    if (n > FRAG_MAX_FRAGMENTS)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t piece = (len + n - 1) / n;
    uint16_t seqno = s_frag_seqno++;
    uint8_t *buf = malloc(sizeof(struct frag_packet) + piece);
    if (buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    struct frag_packet *fp = (struct frag_packet *)buf;
    size_t remaining = len;
    uint8_t no = 0;
    esp_err_t err = ESP_OK;
    /* Tail pieces first (numbered 0, 1, ...), the head last with the highest number. */
    while (remaining > 0 && err == ESP_OK)
    {
        size_t this_len = remaining > piece ? piece : remaining;
        size_t off = remaining - this_len;
        memset(fp, 0, sizeof(*fp));
        fp->packet_type = PT_UNICAST_FRAG;
        fp->version = COMPAT_VERSION;
        fp->ttl = TTL_DEFAULT;
        fp->no_prio = (uint8_t)(no << 4);
        memcpy(fp->dest, s_orig[oi].addr, 6);
        memcpy(fp->orig, s_cfg.hardif_mac, 6);
        fp->seqno = be16(seqno);
        fp->total_size = be16((uint16_t)len);
        memcpy(buf + sizeof(*fp), pkt + off, this_len);
        err = send_frame(next, buf, sizeof(*fp) + this_len);
        s_stats.frag_tx++;
        remaining = off;
        no++;
    }
    free(buf);
    return err;
}

/* ---------- Translation table messages ---------- */

static void tt_send_request(int oi)
{
    struct orig *o = &s_orig[oi];
    if (now_us() - o->tt_req_us < TT_REQUEST_TIMEOUT_US)
    {
        return;
    }
    o->tt_req_us = now_us();
    uint8_t buf[sizeof(struct unicast_tvlv_packet) + sizeof(struct tvlv_hdr) + sizeof(struct tvlv_tt_data) + sizeof(struct tvlv_tt_vlan)];
    struct unicast_tvlv_packet *u = (struct unicast_tvlv_packet *)buf;
    memset(u, 0, sizeof(*u));
    u->packet_type = PT_UNICAST_TVLV;
    u->version = COMPAT_VERSION;
    u->ttl = TTL_DEFAULT;
    memcpy(u->dst, o->addr, 6);
    memcpy(u->src, s_cfg.hardif_mac, 6);
    u->tvlv_len = be16(sizeof(buf) - sizeof(*u));
    struct tvlv_hdr *h = (struct tvlv_hdr *)(u + 1);
    h->type = TVLV_TT;
    h->version = 1;
    h->len = be16(sizeof(struct tvlv_tt_data) + sizeof(struct tvlv_tt_vlan));
    struct tvlv_tt_data *d = (struct tvlv_tt_data *)(h + 1);
    d->flags = TT_REQUEST | TT_FULL_TABLE;
    d->ttvn = o->last_ttvn;
    d->num_vlan = be16(1);
    struct tvlv_tt_vlan *v = (struct tvlv_tt_vlan *)(d + 1);
    v->crc = be32(tt_global_crc(oi));
    v->vid = 0;
    v->reserved = 0;
    if (send_to_orig(oi, buf, sizeof(buf)) == ESP_OK)
    {
        s_stats.tt_req_tx++;
    }
}

/* Our full local table, unicast to whoever asked (batadv_send_my_tt_response). */
static void tt_send_response(int requester_oi)
{
    struct tvlv_tt_change changes[BATADV_MAX_TT_LOCAL];
    int n = 0;
    for (int i = 0; i < BATADV_MAX_TT_LOCAL; i++)
    {
        const struct tt_local *l = &s_tt_local[i];
        if (l->used && !(l->flags & TT_CLIENT_NEW) && !l->pending_del)
        {
            memset(&changes[n], 0, sizeof(changes[n]));
            changes[n].flags = (uint8_t)l->flags;
            memcpy(changes[n].addr, l->addr, 6);
            n++;
        }
    }
    size_t tvlv_len = sizeof(struct tvlv_hdr) + sizeof(struct tvlv_tt_data) + sizeof(struct tvlv_tt_vlan) + n * sizeof(changes[0]);
    size_t len = sizeof(struct unicast_tvlv_packet) + tvlv_len;
    uint8_t *buf = calloc(1, len);
    if (buf == NULL)
    {
        return;
    }
    struct unicast_tvlv_packet *u = (struct unicast_tvlv_packet *)buf;
    u->packet_type = PT_UNICAST_TVLV;
    u->version = COMPAT_VERSION;
    u->ttl = TTL_DEFAULT;
    memcpy(u->dst, s_orig[requester_oi].addr, 6);
    memcpy(u->src, s_cfg.hardif_mac, 6);
    u->tvlv_len = be16((uint16_t)tvlv_len);
    struct tvlv_hdr *h = (struct tvlv_hdr *)(u + 1);
    h->type = TVLV_TT;
    h->version = 1;
    h->len = be16((uint16_t)(tvlv_len - sizeof(*h)));
    struct tvlv_tt_data *d = (struct tvlv_tt_data *)(h + 1);
    d->flags = TT_RESPONSE | TT_FULL_TABLE;
    d->ttvn = s_ttvn;
    d->num_vlan = be16(1);
    struct tvlv_tt_vlan *v = (struct tvlv_tt_vlan *)(d + 1);
    v->crc = be32(s_local_crc);
    memcpy(v + 1, changes, n * sizeof(changes[0]));
    if (send_to_orig(requester_oi, buf, len) == ESP_OK)
    {
        s_stats.tt_resp_tx++;
    }
    free(buf);
}

static void tt_apply_changes(int oi, const struct tvlv_tt_change *c, int n, uint8_t ttvn, bool full)
{
    if (full)
    {
        tt_global_del_orig(oi);
    }
    for (int i = 0; i < n; i++)
    {
        if (c[i].flags & TT_CLIENT_DEL)
        {
            tt_global_del(c[i].addr, oi);
        }
        else
        {
            tt_global_add(c[i].addr, oi, c[i].flags);
        }
    }
    s_orig[oi].last_ttvn = ttvn;
    s_orig[oi].tt_init = true;
}

/* The TT TVLV riding in an originator's OGM (batadv_tt_update_orig). */
static void tt_handle_ogm(int oi, const struct tvlv_tt_data *d, size_t len)
{
    if (len < sizeof(*d))
    {
        return;
    }
    uint16_t num_vlan = be16(d->num_vlan);
    size_t vlan_bytes = (size_t)num_vlan * sizeof(struct tvlv_tt_vlan);
    if (len < sizeof(*d) + vlan_bytes)
    {
        return;
    }
    const struct tvlv_tt_vlan *vlan = (const struct tvlv_tt_vlan *)(d + 1);
    uint32_t wire_crc = 0;
    bool have_vid0 = false;
    for (uint16_t i = 0; i < num_vlan; i++)
    {
        if (be16(vlan[i].vid) == 0)
        {
            wire_crc = be32(vlan[i].crc);
            have_vid0 = true;
        }
    }
    const struct tvlv_tt_change *changes = (const struct tvlv_tt_change *)((const uint8_t *)vlan + vlan_bytes);
    int n = (int)((len - sizeof(*d) - vlan_bytes) / sizeof(struct tvlv_tt_change));
    struct orig *o = &s_orig[oi];
    uint8_t ttvn = d->ttvn;

    bool next = (!o->tt_init && ttvn == 1) || (o->tt_init && (uint8_t)(ttvn - o->last_ttvn) == 1);
    if (next)
    {
        if (n == 0)
        {
            tt_send_request(oi);
            return;
        }
        tt_apply_changes(oi, changes, n, ttvn, false);
        if (have_vid0 && tt_global_crc(oi) != wire_crc)
        {
            tt_send_request(oi);
        }
        return;
    }
    if (o->tt_init && ttvn == o->last_ttvn)
    {
        if (have_vid0 && tt_global_crc(oi) != wire_crc)
        {
            tt_send_request(oi);
        }
        return;
    }
    tt_send_request(oi);
}

/* ---------- Receive paths ---------- */

static void process_elp(const uint8_t *sender, const uint8_t *p, size_t len)
{
    if (len < sizeof(struct elp_packet))
    {
        return;
    }
    const struct elp_packet *e = (const struct elp_packet *)p;
    if (mac_eq(e->orig, s_cfg.hardif_mac) || mac_eq(sender, s_cfg.hardif_mac))
    {
        return;
    }
    s_stats.elp_rx++;
    int ni = neigh_get_or_create(sender);
    struct neigh *nb = &s_neigh[ni];
    uint32_t seqno = be32(e->seqno);
    if (nb->seq_valid)
    {
        int32_t diff = (int32_t)(seqno - nb->elp_seqno);
        if (diff < 1 && diff > -WINDOW_SIZE)
        {
            return;   /* stale or duplicate */
        }
    }
    nb->seq_valid = true;
    nb->elp_seqno = seqno;
    nb->elp_interval = be32(e->elp_interval);
    nb->last_seen = now_us();
    /* Also mark the originator the ELP came from as seen. */
    int oi = orig_find(e->orig);
    if (oi >= 0)
    {
        s_orig[oi].last_seen = now_us();
    }
}

static uint32_t forward_penalty(uint32_t tp)
{
    tp = (uint32_t)((uint64_t)tp * (TQ_MAX_VALUE - s_cfg.hop_penalty) / TQ_MAX_VALUE);
    /* One half-duplex radio: an OGM that goes back out the way it came costs half. */
    if (tp > 10)
    {
        tp /= 2;
    }
    return tp;
}

static void process_tvlvs_ogm(int oi, const uint8_t *tvlv, size_t len)
{
    while (len >= sizeof(struct tvlv_hdr))
    {
        const struct tvlv_hdr *h = (const struct tvlv_hdr *)tvlv;
        size_t l = be16(h->len);
        if (sizeof(*h) + l > len)
        {
            break;
        }
        if (h->type == TVLV_TT && h->version == 1)
        {
            tt_handle_ogm(oi, (const struct tvlv_tt_data *)(h + 1), l);
        }
        tvlv += sizeof(*h) + l;
        len -= sizeof(*h) + l;
    }
}

static void process_ogm2(const uint8_t *sender, const uint8_t *p, size_t len)
{
    if (len < sizeof(struct ogm2_packet))
    {
        return;
    }
    const struct ogm2_packet *o = (const struct ogm2_packet *)p;
    size_t tvlv_len = be16(o->tvlv_len);
    if (sizeof(*o) + tvlv_len > len)
    {
        return;
    }
    if (mac_eq(o->orig, s_cfg.hardif_mac) || mac_eq(sender, s_cfg.hardif_mac))
    {
        return;
    }
    uint32_t tp = be32(o->throughput);
    if (tp == 0)
    {
        s_stats.ogm_dropped++;
        return;
    }
    int ni = neigh_find(sender);
    if (ni < 0)
    {
        s_stats.ogm_dropped++;   /* no ELP from this sender yet */
        return;
    }
    s_stats.ogm_rx++;
    int oi = orig_get_or_create(o->orig);
    struct orig *orig = &s_orig[oi];
    orig->last_seen = now_us();
    s_neigh[ni].last_seen = now_us();

    /* Sequence window (batadv_v_ogm_metric_update). */
    uint32_t seqno = be32(o->seqno);
    bool is_new = false;
    if (!orig->seqno_valid)
    {
        orig->seqno_valid = true;
        orig->last_real_seqno = seqno;
        is_new = true;
    }
    else
    {
        int32_t diff = (int32_t)(seqno - orig->last_real_seqno);
        bool reset = false;
        if (window_protected(diff, &orig->ogm_reset_us, &reset))
        {
            s_stats.ogm_dropped++;
            return;
        }
        if (diff < 0 && !reset)
        {
            s_stats.ogm_dropped++;
            return;
        }
        is_new = diff > 0 || reset;
        if (is_new)
        {
            orig->last_real_seqno = seqno;
        }
    }

    /* Path throughput via this neighbour. */
    uint32_t link_tp = s_cfg.link_throughput;
    uint32_t path_tp = tp < link_tp ? tp : link_tp;
    orig->via[ni].valid = true;
    orig->via[ni].throughput = path_tp;
    orig->via[ni].seqno = seqno;

    /* Route update (batadv_v_ogm_route_update). */
    int r = orig->router;
    if (r < 0 || !s_neigh[r].used || !orig->via[r].valid)
    {
        orig->router = ni;
    }
    else if (r != ni)
    {
        int32_t seq_adv = (int32_t)(seqno - orig->via[r].seqno);
        if (seq_adv >= OGM_MAX_ORIGDIFF || path_tp > orig->via[r].throughput)
        {
            orig->router = ni;
        }
    }

    /* Forward (batadv_v_ogm_forward): only what came from our router, once per seqno. */
    if (o->ttl > 1 && orig->router == ni && (!orig->fwd_valid || orig->last_fwd_seqno != seqno))
    {
        orig->fwd_valid = true;
        orig->last_fwd_seqno = seqno;
        uint8_t *copy = malloc(sizeof(*o) + tvlv_len);
        if (copy != NULL)
        {
            memcpy(copy, p, sizeof(*o) + tvlv_len);
            struct ogm2_packet *c = (struct ogm2_packet *)copy;
            c->ttl--;
            c->throughput = be32(forward_penalty(path_tp));
            if (send_frame(BROADCAST, copy, sizeof(*o) + tvlv_len) == ESP_OK)
            {
                s_stats.ogm_fwd++;
            }
            free(copy);
        }
    }

    if (is_new)
    {
        process_tvlvs_ogm(oi, p + sizeof(*o), tvlv_len);
    }
}

static void deliver(const uint8_t *frame, size_t len)
{
    if (len >= 14 && s_cfg.deliver != NULL)
    {
        s_stats.delivered++;
        s_cfg.deliver(frame, len);
    }
}

static void process_unicast(const uint8_t *sender, const uint8_t *p, size_t len)
{
    (void)sender;
    size_t hlen = p[0] == PT_UNICAST_4ADDR ? sizeof(struct unicast_4addr_packet) : sizeof(struct unicast_packet);
    if (len < hlen + 14)
    {
        return;
    }
    const struct unicast_packet *u = (const struct unicast_packet *)p;
    s_stats.ucast_rx++;
    if (mac_eq(u->dest, s_cfg.hardif_mac))
    {
        deliver(p + hlen, len - hlen);
        return;
    }
    if (u->ttl < 2)
    {
        return;
    }
    int oi = orig_find(u->dest);
    if (oi < 0)
    {
        return;
    }
    uint8_t *copy = malloc(len);
    if (copy == NULL)
    {
        return;
    }
    memcpy(copy, p, len);
    copy[2] = u->ttl - 1;
    if (send_to_orig(oi, copy, len) == ESP_OK)
    {
        s_stats.ucast_fwd++;
    }
    free(copy);
}

static void process_bcast(const uint8_t *sender, const uint8_t *p, size_t len)
{
    if (len < sizeof(struct bcast_packet) + 14)
    {
        return;
    }
    const struct bcast_packet *b = (const struct bcast_packet *)p;
    if (mac_eq(b->orig, s_cfg.hardif_mac) || mac_eq(sender, s_cfg.hardif_mac))
    {
        return;
    }
    if (b->ttl < 2)
    {
        return;
    }
    int oi = orig_find(b->orig);
    if (oi < 0)
    {
        return;   /* unknown originator: Linux drops too */
    }
    struct orig *o = &s_orig[oi];
    uint32_t seqno = be32(b->seqno);
    if (o->bcast_valid)
    {
        int32_t diff = (int32_t)(seqno - o->last_bcast_seqno);
        bool reset = false;
        if (window_protected(diff, &o->bcast_reset_us, &reset))
        {
            s_stats.bcast_dup++;
            return;
        }
        if (reset)
        {
            o->bcast_bits = 0;
            o->last_bcast_seqno = seqno;
        }
        else if (diff > 0)
        {
            o->bcast_bits = diff >= 64 ? 0 : o->bcast_bits << diff;
            o->last_bcast_seqno = seqno;
        }
        else
        {
            uint64_t bit = 1ull << (-diff);
            if (o->bcast_bits & bit)
            {
                s_stats.bcast_dup++;
                return;
            }
            o->bcast_bits |= bit;
            goto deliver_only;   /* an older but unseen one: deliver, do not rebroadcast */
        }
    }
    else
    {
        o->bcast_valid = true;
        o->last_bcast_seqno = seqno;
        o->bcast_bits = 0;
    }
    o->bcast_bits |= 1;
    s_stats.bcast_rx++;
    {
        uint8_t *copy = malloc(len);
        if (copy != NULL)
        {
            memcpy(copy, p, len);
            copy[2] = b->ttl - 1;
            if (send_frame(BROADCAST, copy, len) == ESP_OK)
            {
                s_stats.bcast_fwd++;
            }
            free(copy);
        }
    }
deliver_only:
    deliver(p + sizeof(*b), len - sizeof(*b));
}

static void process_packet(const uint8_t *sender, const uint8_t *p, size_t len);

static void frag_chain_reset(struct frag_chain *c)
{
    for (int i = 0; i < FRAG_MAX_FRAGMENTS; i++)
    {
        free(c->part[i].data);
    }
    memset(c, 0, sizeof(*c));
}

static void process_frag(const uint8_t *sender, const uint8_t *p, size_t len)
{
    if (len < sizeof(struct frag_packet) + 1)
    {
        return;
    }
    const struct frag_packet *f = (const struct frag_packet *)p;
    s_stats.frag_rx++;
    if (!mac_eq(f->dest, s_cfg.hardif_mac))
    {
        /* Not ours: forward unmerged (batadv_frag_skb_fwd). */
        if (f->ttl < 2)
        {
            return;
        }
        int oi = orig_find(f->dest);
        if (oi < 0)
        {
            return;
        }
        uint8_t *copy = malloc(len);
        if (copy != NULL)
        {
            memcpy(copy, p, len);
            copy[2] = f->ttl - 1;
            int r = router_for(oi);
            if (r >= 0)
            {
                send_frame(s_neigh[r].addr, copy, len);
            }
            free(copy);
        }
        return;
    }
    uint16_t seqno = be16(f->seqno);
    uint16_t total = be16(f->total_size);
    uint8_t no = f->no_prio >> 4;
    size_t plen = len - sizeof(*f);
    struct frag_chain *c = &s_frag[seqno % FRAG_CHAINS];
    if (c->used && (c->seqno != seqno || !mac_eq(c->orig, f->orig) || now_us() - c->t > FRAG_TIMEOUT_US))
    {
        frag_chain_reset(c);
    }
    if (!c->used)
    {
        c->used = true;
        c->seqno = seqno;
        c->total = total;
        memcpy(c->orig, f->orig, 6);
        c->t = now_us();
    }
    if (total != c->total || no >= FRAG_MAX_FRAGMENTS || c->part[no].used || total > FRAG_MAX_FRAGMENTS * FRAG_MAX_FRAG_SIZE)
    {
        s_stats.frag_dropped++;
        return;
    }
    c->part[no].data = malloc(plen);
    if (c->part[no].data == NULL)
    {
        return;
    }
    memcpy(c->part[no].data, p + sizeof(*f), plen);
    c->part[no].len = (uint16_t)plen;
    c->part[no].no = no;
    c->part[no].used = true;
    c->have += (uint16_t)plen;
    if (c->have > total)
    {
        s_stats.frag_dropped++;
        frag_chain_reset(c);
        return;
    }
    if (c->have < total)
    {
        return;
    }
    /* Complete: the head has the highest number; pieces go in descending order. */
    uint8_t *merged = malloc(total);
    if (merged != NULL)
    {
        size_t off = 0;
        for (int i = FRAG_MAX_FRAGMENTS - 1; i >= 0; i--)
        {
            if (c->part[i].used)
            {
                memcpy(merged + off, c->part[i].data, c->part[i].len);
                off += c->part[i].len;
            }
        }
        s_stats.frag_merged++;
        if (merged[0] != PT_UNICAST_FRAG)
        {
            process_packet(sender, merged, total);
        }
        free(merged);
    }
    frag_chain_reset(c);
}

static void process_unicast_tvlv(const uint8_t *sender, const uint8_t *p, size_t len)
{
    (void)sender;
    if (len < sizeof(struct unicast_tvlv_packet))
    {
        return;
    }
    const struct unicast_tvlv_packet *u = (const struct unicast_tvlv_packet *)p;
    size_t tvlv_len = be16(u->tvlv_len);
    if (sizeof(*u) + tvlv_len > len)
    {
        return;
    }
    if (!mac_eq(u->dst, s_cfg.hardif_mac))
    {
        if (u->ttl < 2)
        {
            return;
        }
        int oi = orig_find(u->dst);
        if (oi < 0)
        {
            return;
        }
        uint8_t *copy = malloc(len);
        if (copy != NULL)
        {
            memcpy(copy, p, len);
            copy[2] = u->ttl - 1;
            send_to_orig(oi, copy, len);
            free(copy);
        }
        return;
    }
    int src_oi = orig_find(u->src);
    const uint8_t *t = p + sizeof(*u);
    size_t rem = tvlv_len;
    while (rem >= sizeof(struct tvlv_hdr))
    {
        const struct tvlv_hdr *h = (const struct tvlv_hdr *)t;
        size_t l = be16(h->len);
        if (sizeof(*h) + l > rem)
        {
            break;
        }
        if (h->type == TVLV_TT && h->version == 1 && l >= sizeof(struct tvlv_tt_data))
        {
            const struct tvlv_tt_data *d = (const struct tvlv_tt_data *)(h + 1);
            if ((d->flags & TT_REQUEST) && src_oi >= 0)
            {
                s_stats.tt_req_rx++;
                tt_send_response(src_oi);
            }
            else if ((d->flags & TT_RESPONSE) && src_oi >= 0)
            {
                s_stats.tt_resp_rx++;
                uint16_t num_vlan = be16(d->num_vlan);
                size_t vlan_bytes = (size_t)num_vlan * sizeof(struct tvlv_tt_vlan);
                if (l >= sizeof(*d) + vlan_bytes)
                {
                    const struct tvlv_tt_change *changes = (const struct tvlv_tt_change *)((const uint8_t *)(d + 1) + vlan_bytes);
                    int n = (int)((l - sizeof(*d) - vlan_bytes) / sizeof(*changes));
                    tt_apply_changes(src_oi, changes, n, d->ttvn, (d->flags & TT_FULL_TABLE) != 0);
                    char a[18];
                    mac_str(u->src, a);
                    ESP_LOGI(TAG, "TT %s table from %s: %d entries, ttvn %u", (d->flags & TT_FULL_TABLE) ? "full" : "diff", a, n, d->ttvn);
                }
            }
        }
        t += sizeof(*h) + l;
        rem -= sizeof(*h) + l;
    }
}

static void process_packet(const uint8_t *sender, const uint8_t *p, size_t len)
{
    if (len < 2 || p[1] != COMPAT_VERSION)
    {
        return;
    }
    switch (p[0])
    {
        case PT_ELP: process_elp(sender, p, len); break;
        case PT_OGM2: process_ogm2(sender, p, len); break;
        case PT_UNICAST:
        case PT_UNICAST_4ADDR: process_unicast(sender, p, len); break;
        case PT_BCAST: process_bcast(sender, p, len); break;
        case PT_UNICAST_FRAG: process_frag(sender, p, len); break;
        case PT_UNICAST_TVLV: process_unicast_tvlv(sender, p, len); break;
        case PT_ICMP: break;
        default: s_stats.unknown_type++; break;
    }
}

void batadv_rx(const uint8_t *frame, size_t len)
{
    if (len < 14 + 2)
    {
        return;
    }
    const struct eth_hdr *e = (const struct eth_hdr *)frame;
    if (be16(e->type) != BATADV_ETHERTYPE)
    {
        return;
    }
    LOCK();
    process_packet(e->src, frame + 14, len - 14);
    UNLOCK();
}

/* ---------- Transmit from our own stack ---------- */

esp_err_t batadv_tx(const uint8_t *frame, size_t len)
{
    if (len < 14)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const struct eth_hdr *e = (const struct eth_hdr *)frame;
    esp_err_t err;
    LOCK();
    int oi = -1;
    if (!mac_is_group(e->dst))
    {
        int g = tt_global_find(e->dst);
        if (g >= 0)
        {
            oi = s_tt_global[g].orig;
        }
        else
        {
            oi = orig_find(e->dst);   /* a node's originator address itself */
        }
    }
    if (oi >= 0 && router_for(oi) >= 0)
    {
        size_t plen = sizeof(struct unicast_packet) + len;
        uint8_t *buf = malloc(plen);
        if (buf == NULL)
        {
            UNLOCK();
            return ESP_ERR_NO_MEM;
        }
        struct unicast_packet *u = (struct unicast_packet *)buf;
        u->packet_type = PT_UNICAST;
        u->version = COMPAT_VERSION;
        u->ttl = TTL_DEFAULT;
        u->ttvn = s_orig[oi].last_ttvn;
        memcpy(u->dest, s_orig[oi].addr, 6);
        memcpy(buf + sizeof(*u), frame, len);
        err = send_to_orig(oi, buf, plen);
        free(buf);
        if (err == ESP_OK)
        {
            s_stats.ucast_tx++;
        }
    }
    else
    {
        if (!mac_is_group(e->dst))
        {
            s_stats.ucast_flooded++;
        }
        size_t plen = sizeof(struct bcast_packet) + len;
        uint8_t *buf = malloc(plen);
        if (buf == NULL)
        {
            UNLOCK();
            return ESP_ERR_NO_MEM;
        }
        struct bcast_packet *b = (struct bcast_packet *)buf;
        b->packet_type = PT_BCAST;
        b->version = COMPAT_VERSION;
        b->ttl = TTL_DEFAULT;
        b->reserved = 0;
        b->seqno = be32(s_bcast_seqno++);
        memcpy(b->orig, s_cfg.hardif_mac, 6);
        memcpy(buf + sizeof(*b), frame, len);
        err = send_frame(BROADCAST, buf, plen);
        free(buf);
        if (err == ESP_OK)
        {
            s_stats.bcast_tx++;
        }
    }
    UNLOCK();
    return err;
}

/* ---------- Local clients ---------- */

static void tt_local_add_locked(const uint8_t *addr, uint16_t flags)
{
    for (int i = 0; i < BATADV_MAX_TT_LOCAL; i++)
    {
        if (s_tt_local[i].used && mac_eq(s_tt_local[i].addr, addr))
        {
            s_tt_local[i].pending_del = false;
            return;
        }
    }
    for (int i = 0; i < BATADV_MAX_TT_LOCAL; i++)
    {
        if (!s_tt_local[i].used)
        {
            s_tt_local[i].used = true;
            memcpy(s_tt_local[i].addr, addr, 6);
            s_tt_local[i].flags = flags | TT_CLIENT_NEW;
            s_tt_local[i].pending_del = false;
            s_tt_dirty = true;
            return;
        }
    }
}

void batadv_tt_local_add(const uint8_t *addr, bool wifi)
{
    LOCK();
    tt_local_add_locked(addr, wifi ? TT_CLIENT_WIFI : 0);
    UNLOCK();
}

void batadv_tt_local_del(const uint8_t *addr)
{
    LOCK();
    for (int i = 0; i < BATADV_MAX_TT_LOCAL; i++)
    {
        if (s_tt_local[i].used && mac_eq(s_tt_local[i].addr, addr) && !(s_tt_local[i].flags & TT_CLIENT_NOPURGE))
        {
            s_tt_local[i].pending_del = true;
            s_tt_dirty = true;
        }
    }
    UNLOCK();
}

/* ---------- Housekeeping task ---------- */

static uint32_t jittered(uint32_t interval_ms)
{
    return interval_ms - JITTER_MS + (esp_random() % (2 * JITTER_MS));
}

static void purge(void)
{
    int64_t t = now_us();
    for (int i = 0; i < BATADV_MAX_NEIGHBOURS; i++)
    {
        if (s_neigh[i].used && t - s_neigh[i].last_seen > PURGE_TIMEOUT_US)
        {
            char a[18];
            mac_str(s_neigh[i].addr, a);
            ESP_LOGI(TAG, "neighbour %s gone", a);
            s_neigh[i].used = false;
        }
    }
    for (int i = 0; i < BATADV_MAX_ORIGINATORS; i++)
    {
        if (s_orig[i].used && t - s_orig[i].last_seen > PURGE_TIMEOUT_US)
        {
            char a[18];
            mac_str(s_orig[i].addr, a);
            ESP_LOGI(TAG, "originator %s gone", a);
            tt_global_del_orig(i);
            s_orig[i].used = false;
        }
    }
    for (int i = 0; i < FRAG_CHAINS; i++)
    {
        if (s_frag[i].used && t - s_frag[i].t > FRAG_TIMEOUT_US)
        {
            frag_chain_reset(&s_frag[i]);
        }
    }
}

static void batadv_task(void *arg)
{
    (void)arg;
    int64_t next_elp = now_us();
    int64_t next_ogm = now_us() + 200000;
    int64_t next_purge = now_us() + 1000000;
    for (;;)
    {
        int64_t t = now_us();
        LOCK();
        if (t >= next_elp)
        {
            send_elp();
            next_elp = t + (int64_t)jittered(s_cfg.elp_interval_ms) * 1000;
        }
        if (t >= next_ogm)
        {
            send_ogm();
            next_ogm = t + (int64_t)jittered(s_cfg.orig_interval_ms) * 1000;
        }
        if (t >= next_purge)
        {
            purge();
            next_purge = t + 1000000;
        }
        UNLOCK();
        int64_t next = next_elp < next_ogm ? next_elp : next_ogm;
        int64_t wait_ms = (next - now_us()) / 1000;
        vTaskDelay(pdMS_TO_TICKS(wait_ms < 5 ? 5 : wait_ms));
    }
}

esp_err_t batadv_start(const struct batadv_config *cfg)
{
    s_cfg = *cfg;
    if (s_cfg.link_mtu == 0)
    {
        s_cfg.link_mtu = 1514;
    }
    if (s_cfg.link_throughput == 0)
    {
        s_cfg.link_throughput = 10;
    }
    if (s_cfg.orig_interval_ms == 0)
    {
        s_cfg.orig_interval_ms = 1000;
    }
    if (s_cfg.elp_interval_ms == 0)
    {
        s_cfg.elp_interval_ms = 500;
    }
    crc32c_init();
    s_lock = xSemaphoreCreateMutex();
    s_elp_seqno = esp_random();
    s_ogm_seqno = esp_random();
    s_bcast_seqno = 1;
    s_frag_seqno = (uint16_t)esp_random();
    /* Our own client address is the first local entry, never purged, like bat0's MAC on Linux. */
    tt_local_add_locked(s_cfg.bat_mac, TT_CLIENT_NOPURGE);
    if (xTaskCreate(batadv_task, "batadv", 6144, NULL, 6, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    char a[18], b[18];
    mac_str(s_cfg.hardif_mac, a);
    mac_str(s_cfg.bat_mac, b);
    ESP_LOGI(TAG, "BATMAN_V started: originator %s, bat MAC %s, ELP %lu ms, OGM %lu ms", a, b,
             (unsigned long)s_cfg.elp_interval_ms, (unsigned long)s_cfg.orig_interval_ms);
    return ESP_OK;
}

/* ---------- Status ---------- */

int batadv_get_neighbours(struct batadv_neighbour *out, int max)
{
    int n = 0;
    LOCK();
    for (int i = 0; i < BATADV_MAX_NEIGHBOURS && n < max; i++)
    {
        if (s_neigh[i].used)
        {
            memcpy(out[n].addr, s_neigh[i].addr, 6);
            out[n].last_seen_us = s_neigh[i].last_seen;
            out[n].elp_interval_ms = s_neigh[i].elp_interval;
            out[n].throughput = s_cfg.link_throughput;
            n++;
        }
    }
    UNLOCK();
    return n;
}

int batadv_get_originators(struct batadv_originator *out, int max)
{
    int n = 0;
    LOCK();
    for (int i = 0; i < BATADV_MAX_ORIGINATORS && n < max; i++)
    {
        if (s_orig[i].used)
        {
            memcpy(out[n].addr, s_orig[i].addr, 6);
            int r = router_for(i);
            if (r >= 0)
            {
                memcpy(out[n].router, s_neigh[r].addr, 6);
                out[n].throughput = s_orig[i].via[r].throughput;
            }
            else
            {
                memset(out[n].router, 0, 6);
                out[n].throughput = 0;
            }
            out[n].last_seqno = s_orig[i].last_real_seqno;
            out[n].last_ttvn = s_orig[i].last_ttvn;
            out[n].tt_known = s_orig[i].tt_init;
            out[n].last_seen_us = s_orig[i].last_seen;
            n++;
        }
    }
    UNLOCK();
    return n;
}

int batadv_get_tt_global(struct batadv_tt_entry *out, int max)
{
    int n = 0;
    LOCK();
    for (int i = 0; i < BATADV_MAX_TT_GLOBAL && n < max; i++)
    {
        if (s_tt_global[i].used && s_orig[s_tt_global[i].orig].used)
        {
            memcpy(out[n].addr, s_tt_global[i].addr, 6);
            memcpy(out[n].orig, s_orig[s_tt_global[i].orig].addr, 6);
            out[n].flags = s_tt_global[i].flags;
            n++;
        }
    }
    UNLOCK();
    return n;
}

int batadv_get_tt_local(struct batadv_tt_entry *out, int max)
{
    int n = 0;
    LOCK();
    for (int i = 0; i < BATADV_MAX_TT_LOCAL && n < max; i++)
    {
        if (s_tt_local[i].used)
        {
            memcpy(out[n].addr, s_tt_local[i].addr, 6);
            memcpy(out[n].orig, s_cfg.hardif_mac, 6);
            out[n].flags = s_tt_local[i].flags;
            n++;
        }
    }
    UNLOCK();
    return n;
}

void batadv_get_stats(struct batadv_stats *out)
{
    LOCK();
    *out = s_stats;
    UNLOCK();
}

uint8_t batadv_ttvn(void)
{
    return s_ttvn;
}

uint32_t batadv_local_crc(void)
{
    return s_local_crc;
}
