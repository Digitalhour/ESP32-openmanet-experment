#pragma once

/* Pure network-config validation logic, deliberately free of any ESP-IDF
 * dependency beyond esp_err_t itself (shimmed below when building off-target)
 * - this is what makes it possible to compile and run this file's logic on
 * a development host, not just on the ESP32-S3, so it can have a real,
 * CI-runnable regression suite instead of "spot-checked once against a
 * standalone host program and never saved" (design/ROADMAP.md's own prior
 * note on this exact code, before this file existed). Review finding F16's
 * "no maintained automated firmware regression suite"
 * (design/PROJECT_REVIEW_2026-09-10.md) - this module and its host tests
 * (main/host_tests/) are the first piece of that.
 *
 * Extracted verbatim from provisioning.c, not rewritten - same logic, same
 * comments, same review-finding citations (F10) that justified it
 * originally. provisioning.c still owns provisioning_validate() (the
 * caller that ties these together with the rest of gw_config_t, and which
 * itself stays ESP-IDF-only - NVS, esp_console, etc. make the rest of that
 * file genuinely untestable off-target without much heavier stubbing than
 * this narrow extraction needed). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
/* Host build (main/host_tests/) - mirrors ESP-IDF's esp_err.h
 * (components/esp_common/include/esp_err.h at v5.5.1) for the two values
 * this module actually returns, so host test expectations translate
 * directly to the real on-target values rather than a parallel made-up
 * numbering. */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#endif

#include "gw_config.h" /* gw_security_mode_t - already has zero ESP-IDF
                           dependency of its own (checked, not assumed - see
                           that header's own #include list), so pulling it in
                           here doesn't reintroduce the thing this file exists
                           to avoid. */

/* HaLow (802.11ah) has no WPA2-PSK mode - only open/OWE/SAE, confirmed
 * against the real morsemicro/halow SDK's enum mmwlan_security_type.
 *
 * Parses "open"/"owe"/"sae" into *out and returns true. An unrecognized
 * string returns false and leaves *out untouched - callers must reject the
 * request rather than fall back to a default, so a typo like "saee" can't
 * silently configure an open radio (review finding F10,
 * design/PROJECT_REVIEW_2026-09-10.md). Also declared in provisioning.h,
 * identically - that's the public API surface the rest of the firmware
 * includes; this declaration exists so this header is self-contained for a
 * host build that never sees provisioning.h (which pulls in esp_err.h
 * unconditionally and would break off-target compilation). Two identical
 * extern declarations of the same function are valid C. */
bool provisioning_parse_security(const char *s, gw_security_mode_t *out);

/* Validates one IPv4 host+netmask(+gateway) triple as actually usable, not
 * merely parseable - inet_aton() succeeding says nothing about a
 * non-contiguous mask, an IP that's really the network/broadcast address of
 * its own subnet, or a gateway that isn't reachable on it. `what` names the
 * field group in rejection messages (e.g. "local Wi-Fi"). `gateway_str` may
 * be NULL for a subnet with no gateway concept (the HaLow AP's own downlink
 * address - leaves point directly at it). On success, *ip_h_out and
 * *mask_h_out (host byte order) let the caller check two subnets against
 * each other - see subnets_overlap() below. Review finding F10,
 * design/PROJECT_REVIEW_2026-09-10.md. */
esp_err_t validate_host_subnet(const char *what, const char *ip_str, const char *netmask_str,
                                const char *gateway_str, uint32_t *ip_h_out, uint32_t *mask_h_out,
                                char *errbuf, size_t errbuf_len);

/* True if the two (network, mask) pairs describe overlapping address ranges -
 * either network address falling inside the other's range, checked both ways
 * since neither mask is assumed to be the more specific one. Both inputs are
 * assumed already-validated (contiguous, non-zero) subnets. */
bool subnets_overlap(uint32_t ip_a_h, uint32_t mask_a_h, uint32_t ip_b_h, uint32_t mask_b_h);
