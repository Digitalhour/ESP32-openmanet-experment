#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the ATAK CoT multicast relay: joins the
 * configured group/port on both netif_a and netif_b via a single socket,
 * and bidirectionally forwards datagrams between them. Uses IP_PKTINFO to
 * determine which interface a datagram actually arrived on rather than
 * running two independently-bound sockets, which would risk both sockets
 * seeing the same inbound packet and forwarding it back onto its own
 * origin interface. Both netifs must already have a valid IP (call this
 * once the HaLow uplink is connected, not at cold boot). */
esp_err_t cot_relay_start(esp_netif_t *netif_a, esp_netif_t *netif_b, const gw_cot_config_t *cfg);

/* Sends data to the CoT group on both interfaces as if it originated
 * locally - the generic send primitive the self-beacon will need, so a
 * future self-beacon (GPS/battery/status) can reuse this path instead of
 * needing separate code. Not yet called anywhere; no self-beacon exists
 * yet. */
esp_err_t cot_relay_inject(const void *data, size_t len);

/* True once cot_relay_start() has succeeded and the relay socket is live.
 * Surfaced in the web UI's /api/status because "did the relay actually come
 * up?" is otherwise invisible without a serial console - it only starts
 * after the uplink gets a DHCP lease, so a false here is a normal state
 * early in boot, not necessarily a fault. */
bool cot_relay_is_running(void);

/* Packets/bytes relayed *through* one side - i.e. received on the other
 * netif and forwarded out this one, or vice versa. Not a generic per-netif
 * traffic counter: lwIP's own SNMP MIB2 counters (netif->mib2_counters,
 * lwip/snmp.h) would give that, but MIB2_STATS defaults to 0 and ESP-IDF's
 * lwipopts.h (components/lwip/port/include/lwipopts.h) exposes no Kconfig
 * knob to turn it on, and esp_netif.h has no accessor for the underlying
 * `struct netif *` to flip it from application code either - only
 * esp_netif_get_netif_impl_index()/_name(). Counting CoT traffic here
 * instead measures exactly the payload this project exists to move, using
 * only code this project owns. */
typedef struct {
    uint32_t rx_packets; /* received on this netif and forwarded out the other */
    uint64_t rx_bytes;
    uint32_t tx_packets; /* received on the other netif and forwarded out this one */
    uint64_t tx_bytes;
} cot_relay_counters_t;

/* Fills *out_uplink and *out_downlink with the counters for the netif_a
 * ("uplink") and netif_b ("downlink") arguments cot_relay_start() was
 * called with. All-zero before cot_relay_start() succeeds. Either pointer
 * may be NULL to skip it. */
void cot_relay_get_counters(cot_relay_counters_t *out_uplink, cot_relay_counters_t *out_downlink);

/* Count of datagrams dropped for arriving on the relay's port but not
 * actually addressed to the configured CoT multicast group - review finding
 * F09 (design/PROJECT_REVIEW_2026-09-10.md): the relay socket binds
 * INADDR_ANY:port, so without this check a stray unicast/broadcast/
 * wrong-group datagram sent straight at this node would be picked up and
 * faithfully re-transmitted as multicast to the whole other side. A nonzero,
 * growing value here means something is sending unexpected traffic at this
 * port, worth investigating - zero is the expected steady state. */
uint32_t cot_relay_get_wrong_dest_drops(void);

#ifdef __cplusplus
}
#endif
