/* Host-side regression tests for main/net_validate.c - review finding F16
 * (design/PROJECT_REVIEW_2026-09-10.md), "no maintained automated firmware
 * regression suite was found." Compiles and runs on the development
 * machine/CI runner, not the target - no ESP-IDF toolchain, no hardware.
 *
 * Plain C, no external test framework: this project already leans toward
 * "don't add a dependency for something this small" (see CLAUDE.md's own
 * conventions), and the whole point of this file is to be trivially
 * buildable with nothing but a system compiler. RUN() below is the entire
 * "framework" - a name, a boolean, and a running pass/fail count.
 *
 * Build and run: see README.md in this directory, or
 * .github/workflows/build-firmware.yml's "host-tests" job.
 */
#include <stdio.h>

#include "net_validate.h"

static int g_failures = 0;
static int g_total = 0;

#define RUN(name, cond)                                                    \
    do {                                                                   \
        g_total++;                                                        \
        if (cond) {                                                       \
            printf("  ok   %s\n", name);                                  \
        } else {                                                          \
            printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__);      \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

static void test_parse_security(void)
{
    printf("provisioning_parse_security()\n");

    gw_security_mode_t mode;

    RUN("\"open\" -> GW_SECURITY_OPEN",
        provisioning_parse_security("open", &mode) && mode == GW_SECURITY_OPEN);
    RUN("\"owe\" -> GW_SECURITY_OWE",
        provisioning_parse_security("owe", &mode) && mode == GW_SECURITY_OWE);
    RUN("\"sae\" -> GW_SECURITY_SAE",
        provisioning_parse_security("sae", &mode) && mode == GW_SECURITY_SAE);

    /* The actual review finding F10 bug this function exists to prevent:
     * the original code returned GW_SECURITY_OPEN for *any* unrecognized
     * string, so a typo like "saee" silently configured an open radio
     * instead of being rejected. This is the one regression this suite
     * must never let back in. */
    RUN("\"saee\" (typo) -> rejected, not silently open",
        !provisioning_parse_security("saee", &mode));
    RUN("\"\" (empty) -> rejected",
        !provisioning_parse_security("", &mode));
    RUN("\"OPEN\" (wrong case) -> rejected, not case-insensitive",
        !provisioning_parse_security("OPEN", &mode));
    RUN("\"wpa2\" (not a HaLow mode) -> rejected",
        !provisioning_parse_security("wpa2", &mode));
}

/* Small helper so each case below reads as "what" + four strings instead of
 * repeating the out-param dance every time. */
static esp_err_t validate(const char *what, const char *ip, const char *mask, const char *gw)
{
    uint32_t ip_h, mask_h;
    char errbuf[128] = { 0 };
    esp_err_t err = validate_host_subnet(what, ip, mask, gw, &ip_h, &mask_h, errbuf, sizeof(errbuf));
    if (err != ESP_OK && errbuf[0] == '\0') {
        printf("       (rejected with no reason written to errbuf - a real bug if this fires)\n");
    }
    return err;
}

static void test_validate_host_subnet(void)
{
    printf("validate_host_subnet()\n");

    RUN("ordinary host+mask+gateway -> ESP_OK",
        validate("test", "172.16.50.5", "255.255.255.0", "172.16.50.1") == ESP_OK);
    RUN("no gateway concept (NULL) -> ESP_OK, gateway not checked",
        validate("test", "172.16.60.1", "255.255.255.0", NULL) == ESP_OK);

    RUN("all-zero netmask -> rejected",
        validate("test", "172.16.50.5", "0.0.0.0", "172.16.50.1") != ESP_OK);
    RUN("non-contiguous netmask -> rejected",
        validate("test", "172.16.50.5", "255.255.0.255", "172.16.50.1") != ESP_OK);

    RUN("IP is the network address of its own subnet -> rejected",
        validate("test", "172.16.50.0", "255.255.255.0", NULL) != ESP_OK);
    RUN("IP is the broadcast address of its own subnet -> rejected",
        validate("test", "172.16.50.255", "255.255.255.0", NULL) != ESP_OK);

    RUN("gateway outside the subnet -> rejected",
        validate("test", "172.16.50.5", "255.255.255.0", "172.16.60.1") != ESP_OK);
    RUN("all-zero gateway -> rejected",
        validate("test", "172.16.50.5", "255.255.255.0", "0.0.0.0") != ESP_OK);

    RUN("unparseable IP string -> rejected",
        validate("test", "not-an-ip", "255.255.255.0", NULL) != ESP_OK);
    RUN("unparseable netmask string -> rejected",
        validate("test", "172.16.50.5", "not-a-mask", NULL) != ESP_OK);

    /* /32: host_mask is 0, so host_bits is 0 for every address - every /32
     * is rejected as "the network address of its own subnet". Locking this
     * in as documented, observed behavior, not proposing to change it. */
    RUN("/32 mask -> rejected (documented edge case, not a bug to fix here)",
        validate("test", "172.16.50.5", "255.255.255.255", NULL) != ESP_OK);
}

static void test_subnets_overlap(void)
{
    printf("subnets_overlap()\n");

    uint32_t softap_ip, softap_mask, halow_ip, halow_mask;
    char errbuf[128];

    /* This project's own real, shipped defaults - design/ROADMAP.md's F10
     * entry records these as "correctly non-overlapping" on real hardware.
     * If this ever starts reporting true, every relay in the field just
     * became unconfigurable at boot (provisioning_validate() rejects
     * overlapping uplink/downlink subnets) - this is the one case in this
     * whole file where a false positive breaks something a real operator
     * would actually hit, not just a synthetic edge case. */
    if (validate_host_subnet("softap", "172.16.50.1", "255.255.255.0", NULL,
                              &softap_ip, &softap_mask, errbuf, sizeof(errbuf)) != ESP_OK) {
        printf("  FAIL setup: 172.16.50.1/24 itself failed to validate\n");
        g_failures++;
        g_total++;
    } else if (validate_host_subnet("halow_ap", "172.16.60.1", "255.255.255.0", NULL,
                                     &halow_ip, &halow_mask, errbuf, sizeof(errbuf)) != ESP_OK) {
        printf("  FAIL setup: 172.16.60.1/24 itself failed to validate\n");
        g_failures++;
        g_total++;
    } else {
        RUN("this project's real default SoftAP (172.16.50.0/24) vs HaLow AP "
            "(172.16.60.0/24) subnets -> do not overlap",
            !subnets_overlap(softap_ip, softap_mask, halow_ip, halow_mask));
    }

    RUN("identical subnets -> overlap",
        subnets_overlap(0xC0A80000, 0xFFFFFF00, 0xC0A80000, 0xFFFFFF00));
    RUN("a /16 containing a /24 -> overlap (checked from the wider side)",
        subnets_overlap(0xC0A80000, 0xFFFF0000, 0xC0A80500, 0xFFFFFF00));
    RUN("a /24 contained in a /16 -> overlap (checked from the narrower side)",
        subnets_overlap(0xC0A80500, 0xFFFFFF00, 0xC0A80000, 0xFFFF0000));
    RUN("adjacent, non-overlapping /24s -> no overlap",
        !subnets_overlap(0xC0A83200, 0xFFFFFF00, 0xC0A83300, 0xFFFFFF00));
}

int main(void)
{
    test_parse_security();
    test_validate_host_subnet();
    test_subnets_overlap();

    printf("\n%d/%d passed\n", g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
