/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * batman-adv (B.A.T.M.A.N. V) for a microcontroller: a reimplementation of the Linux kernel's
 * net/batman-adv wire protocol, enough for a node to be seen, routed and translated by Linux
 * batman-adv nodes (compat version 15, routing algorithm BATMAN_V). See
 * docs/batman-adv-on-esp32.md for what is and is not implemented.
 *
 * Link-agnostic: the caller supplies one "hard interface" (its MAC, a send function for raw
 * Ethernet frames) and feeds received 0x4305 frames in. Data for the node's own stack comes
 * out through a deliver callback as Ethernet frames addressed to the node's bat MAC.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BATADV_ETHERTYPE 0x4305
#define BATADV_MAX_NEIGHBOURS 16
#define BATADV_MAX_ORIGINATORS 64
#define BATADV_MAX_TT_LOCAL 16
#define BATADV_MAX_TT_GLOBAL 128

struct batadv_config
{
    uint8_t hardif_mac[6];       /* the originator address: the link interface's MAC */
    uint8_t bat_mac[6];          /* the node's own client address on bat0 */
    uint16_t link_mtu;           /* bytes the link carries per frame, Ethernet header included (1514) */
    uint32_t link_throughput;    /* assumed throughput to every neighbour, 100 kbit/s units */
    uint32_t orig_interval_ms;
    uint32_t elp_interval_ms;
    uint8_t hop_penalty;
    /* Send one Ethernet frame (destination, source, 0x4305, payload) on the link. */
    esp_err_t (*link_send)(const uint8_t *frame, size_t len);
    /* An Ethernet frame for the node's own stack (destination is bat_mac, broadcast or a group). */
    void (*deliver)(const uint8_t *frame, size_t len);
};

struct batadv_neighbour
{
    uint8_t addr[6];
    int64_t last_seen_us;
    uint32_t elp_interval_ms;
    uint32_t throughput;
};

struct batadv_originator
{
    uint8_t addr[6];
    uint8_t router[6];           /* next hop neighbour, zeros if none */
    uint32_t throughput;         /* via the router, 100 kbit/s units */
    uint32_t last_seqno;
    uint8_t last_ttvn;
    bool tt_known;
    int64_t last_seen_us;
};

struct batadv_tt_entry
{
    uint8_t addr[6];
    uint8_t orig[6];             /* originator behind which the client sits (ours for local) */
    uint16_t flags;
};

struct batadv_stats
{
    uint32_t elp_tx, elp_rx, ogm_tx, ogm_rx, ogm_fwd, ogm_dropped;
    uint32_t tt_req_tx, tt_req_rx, tt_resp_tx, tt_resp_rx;
    uint32_t ucast_tx, ucast_rx, ucast_fwd, ucast_flooded;
    uint32_t bcast_tx, bcast_rx, bcast_fwd, bcast_dup;
    uint32_t frag_tx, frag_rx, frag_merged, frag_dropped;
    uint32_t delivered, link_tx_failed, unknown_type;
};

esp_err_t batadv_start(const struct batadv_config *cfg);
/* A 0x4305 Ethernet frame from the link, header included. Safe from any task. */
void batadv_rx(const uint8_t *frame, size_t len);
/* An Ethernet frame from the node's own stack (bat0 output). */
esp_err_t batadv_tx(const uint8_t *frame, size_t len);
/* Add a client address behind this node (a hotspot device). */
void batadv_tt_local_add(const uint8_t *addr, bool wifi);
void batadv_tt_local_del(const uint8_t *addr);

int batadv_get_neighbours(struct batadv_neighbour *out, int max);
int batadv_get_originators(struct batadv_originator *out, int max);
int batadv_get_tt_global(struct batadv_tt_entry *out, int max);
int batadv_get_tt_local(struct batadv_tt_entry *out, int max);
void batadv_get_stats(struct batadv_stats *out);
uint8_t batadv_ttvn(void);
uint32_t batadv_local_crc(void);
