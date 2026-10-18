#include <string.h>

#include "dns_forward.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "task_stats.h"

static const char *TAG = "dns_forward";

#define DNS_PORT 53

/* DNS messages over UDP are capped at 512 bytes without EDNS0 (RFC 1035
 * §4.2.1). A larger reply from the real resolver is not turned into a valid
 * truncated (TC-bit) response by capping the read here - lwIP's recvfrom()
 * silently hands back only the first DNS_MSG_MAX bytes of a bigger datagram
 * with no signal that bytes were dropped (min(buflen, datagram_len), same
 * semantics documented for cot_relay.c's own recvmsg() truncation check -
 * esp-lwip 2.2.0-esp src/api/sockets.c). Forwarding that partial buffer
 * would hand the leaf a corrupted, not-actually-truncated DNS message. This
 * forwarder does not implement TCP, so an oversize reply is read via
 * recvmsg() (which does report the real datagram length even when it
 * exceeds the buffer) and dropped rather than forwarded - see
 * dns_forward_task(). Review finding F05, design/PROJECT_REVIEW_2026-09-10.md. */
#define DNS_MSG_MAX 512

/* Fixed 12-byte DNS header (RFC 1035 §4.1.1) plus one question: up to a
 * 255-byte QNAME (63-byte labels, root-terminated) plus 4 bytes of
 * QTYPE/QCLASS. Used only to size the per-slot question-copy below, not as
 * a parse limit on the whole message. */
#define DNS_QUESTION_MAX 259

/* How many queries can be in flight at once, awaiting a reply from the
 * upstream resolver, before the oldest is evicted to make room. A handful
 * of mesh clients doing ordinary lookups will not have this many concurrent
 * outstanding queries in practice - generous headroom, not a tight budget. */
#define DNS_PENDING_MAX 8

/* Drop a pending query if nothing answered it within this long, so a lost
 * reply (or a client that gave up and re-queried with a fresh txn ID)
 * doesn't hold a table slot forever. */
#define DNS_PENDING_TTL_US (5LL * 1000000LL)

typedef struct {
    bool in_use;
    uint16_t client_txn_id;   /* the ID the leaf's own query used - restored
                                * when the matched reply is relayed back. */
    uint16_t upstream_txn_id; /* an ID this forwarder chose and rewrote the
                                * query to use upstream instead of the
                                * client's own - see pending_alloc(). Matching
                                * replies against this instead of the raw
                                * client ID is what makes two simultaneous
                                * leaf queries that happen to pick the same ID
                                * unambiguous (review finding F04). */
    struct sockaddr_in leaf_addr;
    struct sockaddr_in resolver_addr; /* exact resolver this query was sent
                                        * to - a reply must come from exactly
                                        * this address:port, not merely "the
                                        * upstream socket got something". */
    uint8_t question[DNS_QUESTION_MAX];
    size_t question_len;
    int64_t sent_at_us;
} pending_query_t;

static esp_netif_t *s_downlink_netif = NULL;
static esp_netif_t *s_uplink_netif = NULL;
static int s_listen_sock = -1;   /* bound to the downlink's own address, port 53 - leaves query this */
static int s_upstream_sock = -1; /* unbound - used to talk to whatever the uplink's real DNS server is */
static pending_query_t s_pending[DNS_PENDING_MAX];
static bool s_running = false;

static esp_err_t get_netif_addr(esp_netif_t *netif, struct in_addr *out)
{
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    out->s_addr = ip_info.ip.addr;
    return ESP_OK;
}

/* Re-read on every query rather than cached once at startup - a GW_ROLE_RELAY
 * node's own uplink is a normal DHCP client (see dns_forward.h's own
 * comment), so this tracks a lease renewal handing out a different DNS
 * server without needing a restart. Cheap: esp_netif_get_dns_info() reads
 * already-stored state, no network round trip. */
static bool get_upstream_dns_addr(struct sockaddr_in *out)
{
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_uplink_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
        dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(DNS_PORT);
    out->sin_addr.s_addr = dns.ip.u_addr.ip4.addr;
    return true;
}

/* Matches on the ID this forwarder chose for the upstream leg
 * (upstream_txn_id), never the client's own ID - see pending_query_t's own
 * comment for why. */
static pending_query_t *pending_find(uint16_t upstream_txn_id)
{
    for (int i = 0; i < DNS_PENDING_MAX; i++) {
        if (s_pending[i].in_use && s_pending[i].upstream_txn_id == upstream_txn_id) {
            return &s_pending[i];
        }
    }
    return NULL;
}

/* esp_random() draw, retried against every other slot currently in flight
 * until it's unique - DNS_PENDING_MAX is small (8), so this converges
 * immediately in practice. A fresh ID per query, chosen by us rather than
 * carried over from whatever the client sent, both removes the multi-client
 * collision this table used to be vulnerable to and adds real entropy an
 * off-path attacker has to guess, rather than reusing whatever (possibly
 * predictable) ID the client's own resolver library picked. RFC 5452 §9.2. */
static uint16_t alloc_upstream_txn_id(void)
{
    uint16_t id;
    do {
        id = (uint16_t)esp_random();
    } while (pending_find(id) != NULL);
    return id;
}

/* Copies the single question (QNAME+QTYPE+QCLASS) out of a DNS message into
 * out, for later exact comparison between a query and its reply - RFC 5452
 * §9.2's third matching field alongside transaction ID and source
 * address:port. Deliberately narrow: rejects (returns false on) compression
 * pointers in the question section, which a well-formed query never uses
 * (there is nothing earlier in the message to point at) and a well-formed
 * reply's echoed question normally doesn't either - treating either as
 * malformed here is a bounded simplification for a forwarder, not a general
 * DNS parser, and fails closed (no match) rather than mis-parsing. */
static bool extract_question(const uint8_t *msg, size_t msg_len, uint8_t *out, size_t out_cap,
                              size_t *out_len)
{
    if (msg_len < 12) {
        return false;
    }
    uint16_t qdcount = ((uint16_t)msg[4] << 8) | msg[5];
    if (qdcount < 1) {
        return false;
    }

    size_t start = 12;
    size_t pos = start;
    for (;;) {
        if (pos >= msg_len) {
            return false; /* ran off the end mid-label */
        }
        uint8_t label_len = msg[pos];
        if (label_len == 0) {
            pos++;
            break; /* root label */
        }
        if ((label_len & 0xC0) != 0 || label_len > 63) {
            return false; /* compression pointer, or a length RFC 1035 forbids */
        }
        pos += 1 + (size_t)label_len;
        if (pos > msg_len) {
            return false;
        }
    }
    if (pos + 4 > msg_len) {
        return false; /* no room for QTYPE+QCLASS */
    }
    pos += 4;

    size_t qlen = pos - start;
    if (qlen > out_cap) {
        return false;
    }
    memcpy(out, msg + start, qlen);
    *out_len = qlen;
    return true;
}

static bool sockaddr_in_equal(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* Rejects anything shorter than a DNS header (RFC 1035 §4.1.1 - the F04
 * finding's "messages as short as two bytes are accepted" gap) and enforces
 * the QR bit matches which direction this message arrived from: a leaf's
 * query must have QR=0, an upstream reply must have QR=1. Guards against a
 * reflected/injected "reply" that is actually a query shape, or vice versa. */
static bool dns_header_valid(size_t len, const uint8_t *buf, bool expect_response)
{
    if (len < 12) {
        return false;
    }
    bool qr = (buf[2] & 0x80) != 0;
    return qr == expect_response;
}

/* Evicts the oldest slot if the table is full - matches this project's
 * existing LRU-on-full pattern (main/auth.c's session table) rather than
 * refusing new queries outright while a stuck one lingers. */
static pending_query_t *pending_alloc(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < DNS_PENDING_MAX; i++) {
        if (!s_pending[i].in_use) {
            return &s_pending[i];
        }
    }
    int oldest_idx = 0;
    int64_t oldest_time = now;
    for (int i = 0; i < DNS_PENDING_MAX; i++) {
        if (s_pending[i].sent_at_us < oldest_time) {
            oldest_time = s_pending[i].sent_at_us;
            oldest_idx = i;
        }
    }
    return &s_pending[oldest_idx];
}

static void pending_expire_stale(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < DNS_PENDING_MAX; i++) {
        if (s_pending[i].in_use && (now - s_pending[i].sent_at_us) > DNS_PENDING_TTL_US) {
            s_pending[i].in_use = false;
        }
    }
}

/* One task, one select() loop over both sockets - this project's own
 * cot_relay.c uses a single shared socket instead because a multicast group
 * join naturally serves both directions on one fd; DNS forwarding is a real
 * two-socket relay (a listener for leaf queries, a separate unconnected
 * socket for talking to whatever the upstream resolver currently is), so
 * select() is what multiplexes them without a task per query or per leaf. */
static void dns_forward_task(void *arg)
{
    (void)arg;
    static uint8_t buf[DNS_MSG_MAX];

    for (;;) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(s_listen_sock, &read_fds);
        FD_SET(s_upstream_sock, &read_fds);
        int maxfd = (s_listen_sock > s_upstream_sock) ? s_listen_sock : s_upstream_sock;

        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
        int ready = select(maxfd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0) {
            ESP_LOGW(TAG, "select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        pending_expire_stale();

        if (ready == 0) {
            continue; /* timeout - just here to run pending_expire_stale() periodically */
        }

        if (FD_ISSET(s_listen_sock, &read_fds)) {
            struct sockaddr_in leaf_addr;
            struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
            struct msghdr msg = {
                .msg_name = &leaf_addr,
                .msg_namelen = sizeof(leaf_addr),
                .msg_iov = &iov,
                .msg_iovlen = 1,
            };
            int len = recvmsg(s_listen_sock, &msg, 0);
            /* len is the real datagram length even when it exceeds sizeof(buf)
             * - see DNS_MSG_MAX's comment - so a truncated read must be
             * rejected before anything below trusts buf[0..len). */
            if (len >= 0 && !(msg.msg_flags & MSG_TRUNC) && (size_t)len <= sizeof(buf)) {
                uint8_t question[DNS_QUESTION_MAX];
                size_t question_len;
                if (!dns_header_valid((size_t)len, buf, /*expect_response=*/false) ||
                    !extract_question(buf, (size_t)len, question, sizeof(question), &question_len)) {
                    ESP_LOGW(TAG, "dropped a malformed/non-query DNS message from a leaf");
                } else {
                    struct sockaddr_in upstream_addr;
                    if (get_upstream_dns_addr(&upstream_addr)) {
                        pending_query_t *slot = pending_alloc();
                        slot->in_use = true;
                        slot->client_txn_id = ((uint16_t)buf[0] << 8) | buf[1];
                        slot->upstream_txn_id = alloc_upstream_txn_id();
                        slot->leaf_addr = leaf_addr;
                        slot->resolver_addr = upstream_addr;
                        memcpy(slot->question, question, question_len);
                        slot->question_len = question_len;
                        slot->sent_at_us = esp_timer_get_time();

                        /* Rewrite only the transaction ID in place - everything
                         * else in the query is forwarded unmodified. */
                        buf[0] = (uint8_t)(slot->upstream_txn_id >> 8);
                        buf[1] = (uint8_t)(slot->upstream_txn_id & 0xFF);

                        if (sendto(s_upstream_sock, buf, (size_t)len, 0,
                                   (struct sockaddr *)&upstream_addr, sizeof(upstream_addr)) < 0) {
                            ESP_LOGW(TAG, "forwarding query upstream failed: errno %d", errno);
                            slot->in_use = false;
                        }
                    } else {
                        ESP_LOGW(TAG, "no upstream DNS server known yet (uplink not up?) - "
                                      "dropping a query rather than guessing one");
                    }
                }
            } else if (len >= 0) {
                ESP_LOGW(TAG, "dropped an oversize query from a leaf (%d bytes, limit %u) - "
                              "this forwarder does not support EDNS0/TCP",
                         len, (unsigned)sizeof(buf));
            }
        }

        if (FD_ISSET(s_upstream_sock, &read_fds)) {
            struct sockaddr_in upstream_addr;
            struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
            struct msghdr msg = {
                .msg_name = &upstream_addr,
                .msg_namelen = sizeof(upstream_addr),
                .msg_iov = &iov,
                .msg_iovlen = 1,
            };
            int len = recvmsg(s_upstream_sock, &msg, 0);
            if (len >= 0 && (msg.msg_flags & MSG_TRUNC)) {
                /* The real resolver's reply didn't fit DNS_MSG_MAX. Forwarding
                 * the first sizeof(buf) bytes would hand the leaf a corrupted
                 * message with no TC bit set (this forwarder never sets one -
                 * see DNS_MSG_MAX's comment), which is worse than no reply at
                 * all: a client would parse a malformed response instead of
                 * cleanly retrying. */
                ESP_LOGW(TAG, "dropped a %d-byte reply from the resolver - larger than the "
                              "%u-byte forwarder buffer",
                         len, (unsigned)sizeof(buf));
            } else if (len >= 0) {
                uint16_t upstream_txn_id = dns_header_valid((size_t)len, buf, /*expect_response=*/true)
                                                ? (((uint16_t)buf[0] << 8) | buf[1])
                                                : 0xFFFF; /* never a real allocated ID */
                pending_query_t *slot = (upstream_txn_id != 0xFFFF) ? pending_find(upstream_txn_id) : NULL;
                if (slot == NULL) {
                    /* Either malformed/not-a-response, or a reply to a query
                     * we've already expired/evicted - drop it either way. */
                } else if (!sockaddr_in_equal(&upstream_addr, &slot->resolver_addr)) {
                    /* Came from the wrong address:port for this transaction -
                     * do not consume the slot, so the real resolver's reply
                     * can still land later. RFC 5452 §9.2. */
                    ESP_LOGW(TAG, "dropped a DNS reply from an unexpected source for a pending query");
                } else {
                    uint8_t question[DNS_QUESTION_MAX];
                    size_t question_len;
                    if (!extract_question(buf, (size_t)len, question, sizeof(question), &question_len) ||
                        question_len != slot->question_len ||
                        memcmp(question, slot->question, question_len) != 0) {
                        ESP_LOGW(TAG, "dropped a DNS reply whose question didn't match the pending query");
                    } else {
                        /* Restore the client's own transaction ID before
                         * relaying - it never saw the rewritten upstream one. */
                        buf[0] = (uint8_t)(slot->client_txn_id >> 8);
                        buf[1] = (uint8_t)(slot->client_txn_id & 0xFF);
                        sendto(s_listen_sock, buf, (size_t)len, 0,
                               (struct sockaddr *)&slot->leaf_addr, sizeof(slot->leaf_addr));
                        slot->in_use = false;
                    }
                }
            }
        }
    }
}

esp_err_t dns_forward_start(esp_netif_t *downlink_netif, esp_netif_t *uplink_netif)
{
    if (downlink_netif == NULL || uplink_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE; /* already running */
    }

    s_downlink_netif = downlink_netif;
    s_uplink_netif = uplink_netif;
    memset(s_pending, 0, sizeof(s_pending));

    struct in_addr downlink_addr;
    esp_err_t err = get_netif_addr(downlink_netif, &downlink_addr);
    if (err != ESP_OK || downlink_addr.s_addr == 0) {
        ESP_LOGE(TAG, "downlink has no address yet: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    s_listen_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "listen socket() failed: errno %d", errno);
        return ESP_FAIL;
    }
    struct sockaddr_in bind_addr = { 0 };
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr = downlink_addr;
    bind_addr.sin_port = htons(DNS_PORT);
    if (bind(s_listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind() on downlink:%d failed: errno %d", DNS_PORT, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    s_upstream_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_upstream_sock < 0) {
        ESP_LOGE(TAG, "upstream socket() failed: errno %d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(dns_forward_task, "dns_forward", GW_STACK_DNS_FORWARD, NULL, 5, NULL) != pdPASS) {
        close(s_listen_sock);
        close(s_upstream_sock);
        s_listen_sock = -1;
        s_upstream_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    esp_ip4_addr_t downlink_ip4 = { .addr = downlink_addr.s_addr };
    ESP_LOGI(TAG, "DNS forwarder listening on " IPSTR ":%d", IP2STR(&downlink_ip4), DNS_PORT);
    return ESP_OK;
}

bool dns_forward_is_running(void)
{
    return s_running;
}
