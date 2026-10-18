#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GW_ROLE_RELAY only. A leaf associating to this node's HaLow AP has a
 * statically-addressed uplink (see gw_uplink_config_t.static_dns's own
 * comment for why) with no DHCP lease to learn a DNS server from - this
 * gives it one to point at: a small forwarder bound to this node's own
 * downlink (HaLow AP) address, port 53, which relays queries out through
 * this node's own uplink using whatever real DNS server *that* hop has
 * (read fresh via esp_netif_get_dns_info() on every query, not cached - a
 * GW_ROLE_RELAY node's own uplink is a normal DHCP client, so this is the
 * same real server ip_forward_nat.c's propagate_dns() already trusts for a
 * GW_ROLE_CLIENT node's SoftAP). Deliberately not a hardcoded public
 * resolver, so it keeps working if the upstream network's DNS server ever
 * changes, with no reprovisioning of every leaf.
 *
 * Call once bring_up_datapath() confirms NAT is up - same precondition
 * (both netifs exist, uplink has a usable IP) NAT itself needs, since this
 * forwarder's own outbound queries ride the same NAPT'd path. */
esp_err_t dns_forward_start(esp_netif_t *downlink_netif, esp_netif_t *uplink_netif);

/* True once dns_forward_start() has succeeded and the forwarder is live. */
bool dns_forward_is_running(void);

#ifdef __cplusplus
}
#endif
