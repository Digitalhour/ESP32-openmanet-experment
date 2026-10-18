#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cot_relay.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heap_guard.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "task_stats.h"

static const char *TAG = "cot_relay";

static esp_netif_t *s_netif_a = NULL;
static esp_netif_t *s_netif_b = NULL;

/* lwIP interface indices for the two netifs above, resolved once at start.
 * These are what IP_PKTINFO actually reports per datagram (see relay_task);
 * they're stable for the lifetime of a netif, so there's no reason to look
 * them up per packet. */
static int s_ifindex_a = -1;
static int s_ifindex_b = -1;

static int s_sock = -1;
static struct sockaddr_in s_group_dest;

/* send_via() does setsockopt(IP_MULTICAST_IF) + sendto() as a pair on one
 * shared socket; without this lock a concurrent caller (relay_task vs. a
 * future cot_relay_inject() self-beacon) could retarget the egress
 * interface between another sender's two calls. Also serializes the tx
 * counters below (incremented inside send_via() itself) against
 * cot_relay_get_counters()'s reads (httpd task). The rx counters are
 * deliberately *not* under this lock - see relay_task()'s own comment on
 * why an occasional torn read there is an acceptable tradeoff. */
static SemaphoreHandle_t s_send_lock = NULL;

static cot_relay_counters_t s_counters_a; /* rx = arrived on a, tx = forwarded onto a */
static cot_relay_counters_t s_counters_b;

/* Datagrams dropped for arriving on the relay's port but not actually
 * addressed to the configured CoT group - see relay_task()'s own comment
 * (review finding F09). Not per-interface: which side it came in on doesn't
 * change why it was rejected. Unlocked like the rx counters above - only
 * relay_task() writes it, and a torn read on a stats display is harmless. */
static uint32_t s_wrong_dest_drops = 0;

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

static esp_err_t join_group(int sock, const struct in_addr *iface_addr, const struct in_addr *group_addr)
{
    struct ip_mreq mreq = { 0 };
    mreq.imr_interface = *iface_addr;
    mreq.imr_multiaddr = *group_addr;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGE(TAG, "IP_ADD_MEMBERSHIP failed: errno %d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void send_via(esp_netif_t *netif, cot_relay_counters_t *counters, const void *data, size_t len)
{
    struct in_addr iface_addr;
    if (get_netif_addr(netif, &iface_addr) != ESP_OK || iface_addr.s_addr == 0) {
        return; /* interface has no address yet - nothing to source from */
    }

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr)) < 0) {
        ESP_LOGW(TAG, "IP_MULTICAST_IF failed: errno %d", errno);
    } else if (sendto(s_sock, data, len, 0, (struct sockaddr *)&s_group_dest, sizeof(s_group_dest)) < 0) {
        ESP_LOGW(TAG, "sendto failed: errno %d", errno);
    } else if (counters != NULL) {
        counters->tx_packets++;
        counters->tx_bytes += len;
    }
    xSemaphoreGive(s_send_lock);
}

/* Second line of defence against a forwarding loop, behind IP_MULTICAST_LOOP
 * being disabled in cot_relay_start(): if a datagram we transmitted ever
 * comes back to us (loopback default changing, an upstream device reflecting
 * the group, or our own inject() echoing), its source address is one of our
 * own interface addresses and we must not forward it again. */
static bool is_own_address(const struct in_addr *addr)
{
    struct in_addr self;

    if (get_netif_addr(s_netif_a, &self) == ESP_OK && self.s_addr != 0 && self.s_addr == addr->s_addr) {
        return true;
    }
    if (get_netif_addr(s_netif_b, &self) == ESP_OK && self.s_addr != 0 && self.s_addr == addr->s_addr) {
        return true;
    }
    return false;
}

static void relay_task(void *arg)
{
    (void)arg;
    static uint8_t rx_buffer[1500];

    for (;;) {
        struct sockaddr_in source_addr;
        u8_t cmsg_buf[CMSG_SPACE(sizeof(struct in_pktinfo))];
        struct iovec iov = {
            .iov_base = rx_buffer,
            .iov_len = sizeof(rx_buffer),
        };
        struct msghdr msg = {
            .msg_name = &source_addr,
            .msg_namelen = sizeof(source_addr),
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = cmsg_buf,
            .msg_controllen = sizeof(cmsg_buf),
        };

        int len = recvmsg(s_sock, &msg, 0);
        if (len < 0) {
            ESP_LOGW(TAG, "recvmsg failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (len == 0) {
            continue;
        }

        /* recvmsg() on a UDP socket returns the length of the *datagram*, not
         * the number of bytes copied into the iovec. lwip_recvmsg()'s UDP/RAW
         * path ends with:
         *
         *     if (datagram_len > buflen) { message->msg_flags |= MSG_TRUNC; }
         *     ...
         *     return (int)datagram_len;
         *
         * (esp-lwip 2.2.0-esp, src/api/sockets.c L1411-1417 - the branch
         * ESP-IDF v5.5.x ships), while lwip_recvfrom_udp_raw() copies only up
         * to the iovec's capacity and sets *datagram_len from netbuf_len(buf).
         *
         * So for a datagram larger than rx_buffer, `len` is the on-wire size
         * and every byte past sizeof(rx_buffer) was never written. Passing
         * `len` to send_via() would read off the end of rx_buffer into
         * whatever the linker placed after it in .bss and transmit that onto
         * the mesh. lwIP reassembles IP fragments before delivery, so an
         * oversized CoT event really does arrive here as one long datagram
         * rather than as MTU-sized pieces.
         *
         * msg_flags is safe to read: lwip_recvfrom_udp_raw() zeroes it
         * ("Initialize flag output", L1200) on every successful receive.
         *
         * Dropped rather than clamped to sizeof(rx_buffer): a truncated CoT
         * event is malformed XML that the far end can't parse anyway, so
         * forwarding the first 1500 bytes buys nothing and hides the cause.
         * This log line is what turns "ATAK intermittently misses events" into
         * a one-line diagnosis. */
        if (msg.msg_flags & MSG_TRUNC) {
            ESP_LOGW(TAG, "dropped a %d-byte datagram - larger than the %u-byte relay buffer",
                     len, (unsigned)sizeof(rx_buffer));
            continue;
        }

        /* Which interface did this arrive on? Use ipi_ifindex, NOT ipi_addr,
         * for *that* question: lwIP fills ipi_addr from the packet's
         * *destination* address (sockets.c: inet_addr_from_ip4addr(
         * &pkti->ipi_addr, ip_2_ip4(netbuf_destaddr(buf)))), which for
         * legitimate CoT traffic is always the multicast group and therefore
         * never matches an interface's own unicast address - useless for
         * telling interfaces apart. ipi_ifindex is the real arrival-interface
         * signal (pkti->ipi_ifindex = buf->p->if_idx).
         *
         * ipi_addr is still read here, for a different question: review
         * finding F09 (design/PROJECT_REVIEW_2026-09-10.md). The socket below
         * binds INADDR_ANY:port, so it receives *any* UDP datagram addressed
         * to this port on either interface - not just ones actually sent to
         * the configured multicast group. Without checking ipi_addr, a plain
         * unicast datagram sent straight at this node's own IP:port would be
         * picked up here and faithfully re-transmitted as multicast to the
         * whole other side - an open relay, not just a CoT forwarder. Because
         * ipi_addr reflects the packet's *real* destination as parsed from
         * its IP header, not a fixed value, it correctly reads back as
         * whatever a stray unicast/broadcast/wrong-group packet was actually
         * addressed to, which is exactly what makes it useful for validating
         * against s_group_dest below. */
        int arrival_ifindex = -1;
        struct in_addr dest_addr = { 0 };
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo *pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
                arrival_ifindex = pktinfo->ipi_ifindex;
                dest_addr = pktinfo->ipi_addr;
                break;
            }
        }

        if (arrival_ifindex < 0) {
            ESP_LOGW(TAG, "datagram had no IP_PKTINFO, can't tell arrival interface - dropping");
            continue;
        }

        if (dest_addr.s_addr != s_group_dest.sin_addr.s_addr) {
            /* Not addressed to the configured CoT group - a stray unicast,
             * broadcast, or different-multicast-group datagram that happens
             * to share this port. Counted, not logged per-packet: a flood of
             * these (deliberate or not) shouldn't itself become a logging
             * flood - see cot_relay_get_wrong_dest_drops(). */
            s_wrong_dest_drops++;
            continue;
        }

        if (is_own_address(&source_addr.sin_addr)) {
            continue; /* our own transmission came back - never re-forward */
        }

        esp_netif_t *forward_to = NULL;
        cot_relay_counters_t *rx_counters = NULL;
        cot_relay_counters_t *tx_counters = NULL;
        if (arrival_ifindex == s_ifindex_a) {
            forward_to = s_netif_b;
            rx_counters = &s_counters_a;
            tx_counters = &s_counters_b;
        } else if (arrival_ifindex == s_ifindex_b) {
            forward_to = s_netif_a;
            rx_counters = &s_counters_b;
            tx_counters = &s_counters_a;
        }

        if (forward_to == NULL) {
            continue; /* arrived via neither known interface - drop rather than guess */
        }

        /* Heap under pressure: shed CoT (UDP/best-effort, and the highest-
         * volume consumer of lwIP's shared pbuf pool) before the low-volume,
         * human-facing web UI/auth TCP traffic starves instead - see
         * heap_guard.h. Checked here rather than before parsing IP_PKTINFO so
         * the counters below still mean exactly what cot_relay.h documents:
         * "received on this netif and forwarded out the other one". */
        if (heap_guard_should_shed_cot()) {
            continue;
        }

        /* Unlocked: relay_task is the only writer of rx_packets/rx_bytes, and
         * a torn read by cot_relay_get_counters() on a stats display is
         * harmless (self-corrects next read) - not worth serializing the hot
         * receive path over. tx counters below go through send_via(), which
         * already holds s_send_lock for the socket call they have to be
         * consistent with anyway. */
        rx_counters->rx_packets++;
        rx_counters->rx_bytes += (size_t)len;

        send_via(forward_to, tx_counters, rx_buffer, len);
    }
}

static void relay_cleanup(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    if (s_send_lock != NULL) {
        vSemaphoreDelete(s_send_lock);
        s_send_lock = NULL;
    }
}

esp_err_t cot_relay_start(esp_netif_t *netif_a, esp_netif_t *netif_b, const gw_cot_config_t *cfg)
{
    if (netif_a == NULL || netif_b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sock >= 0) {
        return ESP_ERR_INVALID_STATE; /* already running */
    }

    s_netif_a = netif_a;
    s_netif_b = netif_b;

    struct in_addr group_addr;
    if (inet_aton(cfg->group, &group_addr) == 0) {
        ESP_LOGE(TAG, "invalid multicast group '%s'", cfg->group);
        return ESP_ERR_INVALID_ARG;
    }

    /* Resolved once - this is what IP_PKTINFO reports per datagram. */
    s_ifindex_a = esp_netif_get_netif_impl_index(netif_a);
    s_ifindex_b = esp_netif_get_netif_impl_index(netif_b);
    if (s_ifindex_a < 0 || s_ifindex_b < 0 || s_ifindex_a == s_ifindex_b) {
        ESP_LOGE(TAG, "bad netif indices (a=%d b=%d) - can't distinguish arrival interface",
                 s_ifindex_a, s_ifindex_b);
        return ESP_FAIL;
    }

    s_send_lock = xSemaphoreCreateMutex();
    if (s_send_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        relay_cleanup();
        return ESP_FAIL;
    }

    int reuse = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = { 0 };
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(cfg->port);
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        relay_cleanup();
        return ESP_FAIL;
    }

    struct in_addr addr_a, addr_b;
    esp_err_t err = ESP_OK;
    if ((err = get_netif_addr(netif_a, &addr_a)) != ESP_OK) {
        ESP_LOGE(TAG, "netif_a has no IP yet: %s", esp_err_to_name(err));
        goto fail;
    }
    if ((err = get_netif_addr(netif_b, &addr_b)) != ESP_OK) {
        ESP_LOGE(TAG, "netif_b has no IP yet: %s", esp_err_to_name(err));
        goto fail;
    }
    if ((err = join_group(s_sock, &addr_a, &group_addr)) != ESP_OK) {
        goto fail;
    }
    if ((err = join_group(s_sock, &addr_b, &group_addr)) != ESP_OK) {
        goto fail;
    }

    /* We are joined to this group on both interfaces we also transmit on, so
     * loopback of our own datagrams would be re-received and forwarded back
     * out the other interface - a self-sustaining loop. Turn it off
     * explicitly rather than relying on the stack's default. */
    u8_t loop = 0;
    if (setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        ESP_LOGE(TAG, "IP_MULTICAST_LOOP off failed: errno %d", errno);
        err = ESP_FAIL;
        goto fail;
    }

    int pktinfo_enable = 1;
    if (setsockopt(s_sock, IPPROTO_IP, IP_PKTINFO, &pktinfo_enable, sizeof(pktinfo_enable)) < 0) {
        ESP_LOGE(TAG, "IP_PKTINFO failed: errno %d (is CONFIG_LWIP_NETBUF_RECVINFO enabled?)", errno);
        err = ESP_FAIL;
        goto fail;
    }

    memset(&s_group_dest, 0, sizeof(s_group_dest));
    s_group_dest.sin_family = AF_INET;
    s_group_dest.sin_addr = group_addr;
    s_group_dest.sin_port = htons(cfg->port);

    if (xTaskCreate(relay_task, "cot_relay", GW_STACK_COT_RELAY, NULL, 5, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "CoT relay joined %s:%u on both interfaces (ifindex %d and %d)",
             cfg->group, cfg->port, s_ifindex_a, s_ifindex_b);
    return ESP_OK;

fail:
    relay_cleanup();
    return err;
}

bool cot_relay_is_running(void)
{
    return s_sock >= 0;
}

esp_err_t cot_relay_inject(const void *data, size_t len)
{
    if (s_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    send_via(s_netif_a, &s_counters_a, data, len);
    send_via(s_netif_b, &s_counters_b, data, len);
    return ESP_OK;
}

void cot_relay_get_counters(cot_relay_counters_t *out_uplink, cot_relay_counters_t *out_downlink)
{
    /* s_send_lock is NULL until cot_relay_start() succeeds - a fresh boot, an
     * unconfigured uplink, or a failed start all reach this function that way
     * (status_get_handler() calls it unconditionally). Taking a NULL
     * semaphore isn't a no-op: FreeRTOS-Kernel/queue.c's xQueueSemaphoreTake
     * asserts its queue handle is non-NULL, so this would panic rather than
     * just misbehave. Report zero counters instead - review finding F01,
     * design/PROJECT_REVIEW_2026-09-10.md. */
    if (s_send_lock == NULL) {
        if (out_uplink != NULL) {
            memset(out_uplink, 0, sizeof(*out_uplink));
        }
        if (out_downlink != NULL) {
            memset(out_downlink, 0, sizeof(*out_downlink));
        }
        return;
    }

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (out_uplink != NULL) {
        *out_uplink = s_counters_a;
    }
    if (out_downlink != NULL) {
        *out_downlink = s_counters_b;
    }
    xSemaphoreGive(s_send_lock);
}

uint32_t cot_relay_get_wrong_dest_drops(void)
{
    return s_wrong_dest_drops;
}
