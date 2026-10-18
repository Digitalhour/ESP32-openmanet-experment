#ifndef ESP_PLATFORM
/* inet_aton() is a BSD extension, not POSIX - glibc's <arpa/inet.h> hides its
 * declaration unless a permissive-enough feature-test macro is already
 * defined, which a strict `-std=c11` (as opposed to `-std=gnu11`) build does
 * not do by default. Defined unconditionally before any system header so it
 * applies regardless of which -std= flag whatever compiles this host build
 * happens to use - confirmed necessary, not precautionary: the very first
 * host compile of this file failed with exactly this without it. */
#define _DEFAULT_SOURCE
#endif

#include "net_validate.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "lwip/inet.h" /* inet_aton()/ntohl() - ESP-IDF's lwIP port */
#else
#include <arpa/inet.h> /* host build - the same two functions, glibc's own */
#endif

bool provisioning_parse_security(const char *s, gw_security_mode_t *out)
{
    if (strcmp(s, "open") == 0) {
        *out = GW_SECURITY_OPEN;
        return true;
    }
    if (strcmp(s, "owe") == 0) {
        *out = GW_SECURITY_OWE;
        return true;
    }
    if (strcmp(s, "sae") == 0) {
        *out = GW_SECURITY_SAE;
        return true;
    }
    return false;
}

esp_err_t validate_host_subnet(const char *what, const char *ip_str, const char *netmask_str,
                                const char *gateway_str, uint32_t *ip_h_out, uint32_t *mask_h_out,
                                char *errbuf, size_t errbuf_len)
{
#define GW_SUBNET_REJECT(...)                                \
    do {                                                     \
        if (errbuf != NULL && errbuf_len > 0) {               \
            snprintf(errbuf, errbuf_len, __VA_ARGS__);        \
        }                                                     \
        return ESP_ERR_INVALID_ARG;                           \
    } while (0)

    struct in_addr ip, mask;
    if (inet_aton(ip_str, &ip) == 0) {
        GW_SUBNET_REJECT("%s IP '%s' is not a valid address", what, ip_str);
    }
    if (inet_aton(netmask_str, &mask) == 0) {
        GW_SUBNET_REJECT("%s netmask '%s' is not a valid address", what, netmask_str);
    }

    uint32_t mask_h = ntohl(mask.s_addr);
    if (mask_h == 0) {
        GW_SUBNET_REJECT("%s netmask '%s' must not be all-zero", what, netmask_str);
    }
    /* A valid netmask is some number of leading 1 bits followed by trailing 0
     * bits. Inverted, that's trailing 1 bits with nothing above them - which
     * is exactly the values of the form 2^n - 1 (including 0, /32's case).
     * `inv & (inv + 1)` is zero only for such values: incrementing a run of
     * trailing 1s carries all the way through it, so the AND has nothing left
     * in common; any 1 bit sitting above a 0 (the non-contiguous case) survives
     * the AND untouched. */
    uint32_t inverted_mask = ~mask_h;
    if ((inverted_mask & (inverted_mask + 1)) != 0) {
        GW_SUBNET_REJECT("%s netmask '%s' is not contiguous", what, netmask_str);
    }

    uint32_t ip_h = ntohl(ip.s_addr);
    uint32_t host_mask = ~mask_h;
    uint32_t host_bits = ip_h & host_mask;
    if (host_bits == 0) {
        GW_SUBNET_REJECT("%s IP '%s' is the network address of its own subnet, not a usable host",
                          what, ip_str);
    }
    if (host_bits == host_mask) {
        GW_SUBNET_REJECT("%s IP '%s' is the broadcast address of its own subnet, not a usable host",
                          what, ip_str);
    }

    if (gateway_str != NULL) {
        struct in_addr gw;
        if (inet_aton(gateway_str, &gw) == 0 || gw.s_addr == 0) {
            GW_SUBNET_REJECT("%s gateway '%s' must be a valid, non-zero address", what, gateway_str);
        }
        uint32_t gw_h = ntohl(gw.s_addr);
        if ((gw_h & mask_h) != (ip_h & mask_h)) {
            GW_SUBNET_REJECT("%s gateway '%s' is not in the same subnet as %s", what, gateway_str, ip_str);
        }
    }

    if (ip_h_out != NULL) {
        *ip_h_out = ip_h;
    }
    if (mask_h_out != NULL) {
        *mask_h_out = mask_h;
    }
    return ESP_OK;

#undef GW_SUBNET_REJECT
}

bool subnets_overlap(uint32_t ip_a_h, uint32_t mask_a_h, uint32_t ip_b_h, uint32_t mask_b_h)
{
    uint32_t net_a = ip_a_h & mask_a_h;
    uint32_t net_b = ip_b_h & mask_b_h;
    return ((net_a & mask_b_h) == net_b) || ((net_b & mask_a_h) == net_a);
}
