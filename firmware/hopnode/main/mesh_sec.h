/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later
 *
 * The mesh password: SAE between peers, the authenticated peering exchange, and the keys
 * the chip needs. Used by mesh.c; nothing else should need this header.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MESH_SEC_MAX_PEERS 16
#define MESH_SEC_PMKID_LEN 16

/* mesh_sec_rx_auth() results. */
enum mesh_sec_event
{
    MESH_SEC_NONE = 0,       /* nothing for the caller to do */
    MESH_SEC_ACCEPTED,       /* SAE finished: the peer may be opened */
    MESH_SEC_RESTART,        /* the peer started SAE over: drop any peering with it */
};

/* Sends an Authentication frame body (algorithm, transaction, status, fields) to a peer. */
typedef void (*mesh_sec_send_fn)(const uint8_t *peer, const uint8_t *body, size_t len);

bool mesh_sec_enabled(void);
esp_err_t mesh_sec_init(const uint8_t *own_addr, const char *mesh_id, const char *password, mesh_sec_send_fn send);
void mesh_sec_set_vif(uint16_t vif_id);
esp_err_t mesh_sec_install_own_mgtk(void);

/* RSN element (ID, length, 20 bytes): CCMP, SAE. Returns the bytes written. */
size_t mesh_sec_rsn_ie(uint8_t *out, size_t cap);

/* SAE, one instance per neighbour slot. */
void mesh_sec_peer_reset(int ni);
void mesh_sec_peer_start(int ni, const uint8_t *addr);
enum mesh_sec_event mesh_sec_rx_auth(int ni, const uint8_t *addr, const uint8_t *frame, size_t len);
bool mesh_sec_peer_accepted(int ni);
bool mesh_sec_peer_busy(int ni);                 /* SAE running or in its retry backoff */
void mesh_sec_housekeeping(int64_t now_us);

/* Authenticated peering (AMPE). */
void mesh_sec_ampe_init(int ni);
void mesh_sec_pmkid(int ni, uint8_t out[MESH_SEC_PMKID_LEN]);
/* Appends the MIC element and the encrypted AMPE element to a peering frame body. body is the
 * frame body so far (category, action, everything up to where the MIC element goes), all of
 * which may be bound into the MIC. Returns bytes written or -1. */
int mesh_sec_protect(int ni, const uint8_t *body, size_t body_len, bool is_open, uint8_t *out, size_t cap);
/* Checks the MIC and decrypts the AMPE element of a received peering frame. body is the frame
 * body up to the MIC element (body_len bytes); mic points at the 16-byte MIC value; rest_len
 * covers it and everything after. 0 ok, -2 bad MIC, -1 other. */
int mesh_sec_process_ampe(int ni, const uint8_t *body, size_t body_len, bool is_open, const uint8_t *chosen_pmk,
                          const uint8_t *mic, size_t rest_len);
/* Derives the pairwise key and loads the peer's keys into the chip. */
esp_err_t mesh_sec_establish(int ni, uint16_t aid, uint16_t llid, uint16_t plid);
void mesh_sec_peer_lost(int ni, uint16_t aid);
bool mesh_sec_peer_keyed(int ni);

struct mesh_sec_stats
{
    uint32_t sae_started, sae_done, sae_failed, ampe_rejected, keys_installed;
};
void mesh_sec_get_stats(struct mesh_sec_stats *out);

