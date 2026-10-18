/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mesh role's uplink: the HaLow radio as an 802.11s-style mesh point (main/mesh.c) instead
 * of a station on somebody's access point. Same shape as uplink_halow.h so app_main.c's
 * datapath bring-up does not care which one it has. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "uplink_halow.h" /* uplink_halow_state_cb_t */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uplink_mesh_start(void);
esp_netif_t *uplink_mesh_get_netif(void);
bool uplink_mesh_is_connected(void);
const char *uplink_mesh_state_name(void);
void uplink_mesh_set_state_callback(uplink_halow_state_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
