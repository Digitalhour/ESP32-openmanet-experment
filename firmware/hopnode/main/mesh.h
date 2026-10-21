/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * HaLow mesh layer for the XIAO ESP32S3 + Wio-WM6180 (Morse Micro MM6108).
 *
 * Brings up an 802.11s-style mesh interface on the chip, beacons with a Mesh ID, learns
 * neighbours from their beacons, and carries Ethernet frames between boards in 4-address
 * mesh data frames. Broadcasts are flooded with a hop limit and duplicate suppression;
 * unicast frames follow routes learned from traffic, or are flooded when no route is known.
 * Presents itself to the rest of the firmware as an esp_netif, so IP just works on top.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#define MESH_PASSWORD_MAX 63
#define MESH_ID_MAX 32

struct mesh_config
{
    char mesh_id[MESH_ID_MAX + 1];
    char password[MESH_PASSWORD_MAX + 1];   /* mesh password (SAE); empty for an open mesh */
    uint8_t s1g_chan;   /* S1G channel number, e.g. 26 */
    uint8_t opclass;    /* global operating class, e.g. 69 (2 MHz) */
    uint8_t tx_dbm;     /* cap, 0 = regulatory maximum */
    uint8_t ttl;        /* hop limit for frames this board sends */
    bool require_peering; /* only exchange data with neighbours the 802.11s peering handshake accepted */
    /* Layer 2 mode for a routing protocol above (batman-adv): no network interface, no
     * relaying, no flooding. Every received frame goes to l2_rx as an Ethernet frame, and
     * mesh_send_ethernet() sends one hop to the neighbour named in its destination. */
    bool l2_mode;
    void (*l2_rx)(const uint8_t *frame, size_t len);
    /* Board bring-up knobs, 0 = the values proven on the XIAO + Wio-WM6180 bench. */
    uint32_t spi_max_hz;      /* cap on the SPI clock to the chip (40 MHz) */
    uint16_t reset_hold_ms;   /* how long the chip is held in reset (8000) */
    uint16_t reset_settle_ms; /* chip boot time after reset (250) */
};

/* 802.11s peer link states (802.11-2020 14.3.3). Data flows only with ESTAB peers. */
enum mesh_plink_state
{
    MESH_PLINK_IDLE = 0,
    MESH_PLINK_OPN_SNT,
    MESH_PLINK_CNF_RCVD,
    MESH_PLINK_OPN_RCVD,
    MESH_PLINK_ESTAB,
    MESH_PLINK_HOLDING,
};

struct mesh_neighbour
{
    uint8_t addr[6];
    int16_t rssi;
    uint32_t beacons;
    int64_t last_seen_us;
    enum mesh_plink_state plink;
    uint16_t llid, plid;         /* our and their link ids */
};

const char *mesh_plink_state_name(enum mesh_plink_state s);

struct mesh_route
{
    uint8_t dst[6];
    uint8_t next_hop[6];
    uint8_t hops;
    int64_t last_us;
};

struct mesh_stats
{
    uint32_t tx_frames, tx_failed, tx_flooded;
    uint32_t rx_frames, rx_delivered, rx_forwarded, rx_duplicates, rx_queue_full;
    uint32_t beacons_built, beacons_heard;
    uint32_t tx_by_mcs[8];      /* unicast frames sent at each starting MCS */
    uint32_t plink_open_tx, plink_confirm_tx, plink_close_tx, plink_rx, plink_established, plink_lost, rx_unpeered;
    uint32_t rx_unprotected, tx_unkeyed;   /* secure mesh: plaintext data refused; unicast with no key yet */
    uint32_t rx_hook_data, rx_hook_prot, rx_hook_decrypted;   /* data frames the chip handed us; encrypted; decrypted by it */
    uint32_t t_tx_avg_us, t_tx_max_us, t_rxq_avg_us, t_rxq_max_us;   /* time in the chip transmit call; receive queue wait */
};

/* Boots the HaLow chip, starts the mesh interface and creates the network interface (not yet
 * up: set its IP address, then esp_netif_action_connected()). Call once. */
esp_err_t mesh_start(const struct mesh_config *cfg);
bool mesh_is_up(void);
bool mesh_is_secure(void);
/* l2_mode: send an Ethernet frame to the neighbour in its destination (or to all, if group). */
esp_err_t mesh_send_ethernet(const uint8_t *frame, size_t len);
esp_netif_t *mesh_netif(void);
void mesh_get_addr(uint8_t out[6]);
const char *mesh_chip_firmware(void);
bool mesh_channel_info(uint32_t *centre_freq_hz, uint8_t *bw_mhz);
int mesh_get_neighbours(struct mesh_neighbour *out, int max);
int mesh_get_routes(struct mesh_route *out, int max);
void mesh_get_stats(struct mesh_stats *out);
