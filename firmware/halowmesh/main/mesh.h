/* SPDX-License-Identifier: 0BSD */
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

#define MESH_ID_MAX 32

struct mesh_config
{
    char mesh_id[MESH_ID_MAX + 1];
    uint8_t s1g_chan;   /* S1G channel number, e.g. 26 */
    uint8_t opclass;    /* global operating class, e.g. 69 (2 MHz) */
    uint8_t tx_dbm;     /* cap, 0 = regulatory maximum */
    uint8_t ttl;        /* hop limit for frames this board sends */
};

struct mesh_neighbour
{
    uint8_t addr[6];
    int16_t rssi;
    uint32_t beacons;
    int64_t last_seen_us;
};

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
};

/* Boots the HaLow chip, starts the mesh interface and creates the network interface (not yet
 * up: set its IP address, then esp_netif_action_connected()). Call once. */
esp_err_t mesh_start(const struct mesh_config *cfg);
bool mesh_is_up(void);
esp_netif_t *mesh_netif(void);
void mesh_get_addr(uint8_t out[6]);
const char *mesh_chip_firmware(void);
bool mesh_channel_info(uint32_t *centre_freq_hz, uint8_t *bw_mhz);
int mesh_get_neighbours(struct mesh_neighbour *out, int max);
int mesh_get_routes(struct mesh_route *out, int max);
void mesh_get_stats(struct mesh_stats *out);
