#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SSID_MAX_LEN     32
#define GW_PSK_MAX_LEN      64
#define GW_NODE_ID_MAX_LEN  32
#define GW_IP4_STR_MAX_LEN  16 /* "255.255.255.255" + NUL */

/* WPA2-PSK passphrase bounds, fixed by the 802.11i spec and enforced by
 * esp_wifi at AP start - a SoftAP configured outside this range fails to
 * come up, which on this device would take the primary management path
 * (the SoftAP itself) down with it. Validated before save, not at boot. */
#define GW_WPA2_PSK_MIN_LEN 8
#define GW_WPA2_PSK_MAX_LEN 63

/* 2.4GHz SoftAP channel range. 14 is excluded deliberately: it's Japan-only
 * and 802.11b-only, and esp_wifi rejects it under most country settings. */
#define GW_SOFTAP_CHANNEL_MIN 1
#define GW_SOFTAP_CHANNEL_MAX 13

/* Stamped into every persisted blob so a layout change is detected rather
 * than silently reinterpreted. Bump GW_CONFIG_VERSION whenever the shape or
 * meaning of anything below changes; a mismatch makes provisioning_load()
 * fall back to defaults and say so, instead of loading garbage into a
 * deployed unit. */
#define GW_CONFIG_MAGIC   0x4F4D4757u /* "OMGW" */
/* v2: default SoftAP subnet moved from 192.168.50.0/24 to 172.16.50.0/24.
 * The struct layout is unchanged, so the bump is not strictly required to read
 * a v1 blob - it is deliberate, so that any unit already provisioned on a
 * bench picks up the new default instead of silently staying on a subnet that
 * collides with the phone-hotspot and home-router space. A v1 blob is
 * discarded with a log line and the unit falls back to defaults; reprovision
 * it via the web UI or `gwcfg-*`. */
/* v3: the uplink SSID no longer ships with a placeholder value, and an empty
 * one now *means* something - see gw_uplink_is_configured() below. A v2 blob
 * carries "openmanet-halow" whether or not anyone chose it, so it would come
 * back as "configured" and resume chasing an AP that may never have existed.
 * Discarding it costs one reprovision and removes that ambiguity for good. */
/* v4: adds `role` (client/relay), an optional static-IP override on the
 * uplink (either kind), a native 2.4GHz Wi-Fi uplink config, and a HaLow
 * AP-mode config - see design/ROADMAP.md items 8 for why. Existing fields are
 * unchanged in layout and meaning, but the struct grew, so a v3 blob must be
 * discarded rather than reinterpreted with garbage tail bytes. */
/* v5: adds `allow_uplink_management`, off by default - see its own comment
 * below. Existing fields are unchanged; the struct grew, so a v4 blob must be
 * discarded rather than reinterpreted with a garbage trailing byte. */
/* v6: adds `auth` (gw_auth_config_t) for design/ROADMAP.md item 1's web UI
 * credential. A v5 blob is discarded (grown struct, not safely
 * reinterpretable) rather than migrated - it comes back with
 * auth.password_set == false, which is exactly the forced-first-use state
 * anyway, so no migration code is needed. */
/* v7: adds `gw_uplink_config_t.static_dns` - see its own comment. Existing
 * fields unchanged; a v6 blob is discarded rather than reinterpreted, which
 * comes back with static_dns empty - the same "no DNS configured" state a
 * static-IP uplink has always silently had, just now a real field instead
 * of an unreachable one. */
/* v8: adds `tls` (gw_tls_identity_t) - the web UI's per-device HTTPS
 * identity (review finding F02, design/PROJECT_REVIEW_2026-09-10.md). A v7
 * blob is discarded (grown struct) rather than migrated; it comes back with
 * `tls.identity_set == false`, which is exactly the "generate one at next
 * boot" state tls_identity_init() already treats as normal - no migration
 * code needed, same reasoning v6's auth field addition used. */
/* v9: adds `gw_auth_config_t.onboarding_ever_started`/`onboarding_open`/
 * `setup_secret`/`onboarding_boots_remaining` - the first-ownership claim
 * window (review finding F03, design/PROJECT_REVIEW_2026-09-10.md). A v8
 * blob is discarded rather than migrated; it comes back with
 * `onboarding_ever_started == false`, which correctly reopens a fresh
 * onboarding window on next boot for a device whose config was just reset
 * to defaults by the version mismatch itself - the review's own "an
 * invalid/migrated configuration does not silently reopen public onboarding"
 * concern is about a corrupt blob resuming with *no* gate, not about a
 * legitimately-reset device getting a fresh, gated window - same "no
 * migration code needed" reasoning v6 and v8 already used. */
#define GW_CONFIG_VERSION 9u

/* Which pair of radio roles this node runs. Selects the entire bring-up path
 * in app_main.c - the two are mutually exclusive because both would-be uses
 * of the HaLow radio (uplink_halow's STA mode, downlink_halow_ap's AP mode)
 * share the one physical MM6108 and its single mmhalow_init() call.
 *
 * GW_ROLE_CLIENT is today's original design and remains the default: local
 * 2.4GHz SoftAP for phones/ATAK devices, HaLow STA uplink to whatever AP is
 * configured (a Pi, or a GW_ROLE_RELAY node's HaLow AP - the uplink code
 * can't tell the difference and doesn't need to).
 *
 * GW_ROLE_RELAY is new: HaLow AP downlink (so client-role XIAOs have
 * something to associate to when the Pi itself can't run one - see
 * design/PI_SIDE.md item 0) plus a native 2.4GHz Wi-Fi STA uplink joining
 * the Pi's own local AP directly. A relay runs no local SoftAP of its own. */
typedef enum {
    GW_ROLE_CLIENT = 0,
    GW_ROLE_RELAY,
} gw_node_role_t;

/* Security modes the HaLow uplink STA can be configured for. Must match
 * whatever the associated Pi's HaLow AP is running (design/PI_SIDE.md) -
 * unknown at scaffold time, so this is provisioned, not hardcoded.
 *
 * NOTE: unlike legacy 2.4GHz Wi-Fi, HaLow (802.11ah) has no WPA2-PSK mode -
 * confirmed against the real morsemicro/halow SDK (enum mmwlan_security_type
 * in mmwlan.h only defines MMWLAN_OPEN / MMWLAN_OWE / MMWLAN_SAE). The
 * design doc's original "open / WPA2-PSK / WPA3-SAE" (§4.1) was wrong on
 * this point; corrected here and in design/PI_SIDE.md. */
typedef enum {
    GW_SECURITY_OPEN = 0,
    GW_SECURITY_OWE,
    GW_SECURITY_SAE,
} gw_security_mode_t;

typedef struct {
    /* Empty means "nobody has configured an uplink on this node yet", which is
     * a real state the firmware acts on - not merely a missing value. See
     * gw_uplink_is_configured(). */
    char ssid[GW_SSID_MAX_LEN + 1];
    char psk[GW_PSK_MAX_LEN + 1]; /* only used when security == GW_SECURITY_SAE */
    gw_security_mode_t security;
    /* No per-connection channel selection in the real API - the HaLow STA
     * scans within the regulatory channel list derived from
     * CONFIG_HALOW_COUNTRY_CODE, which is a *build-time* Kconfig value
     * (sdkconfig.defaults), not something this NVS-provisioned struct can
     * override at runtime. See design/PI_SIDE.md "Still to verify" item 3. */

    /* Off by default: the uplink runs a normal DHCP client, exactly as it
     * always has when joining a real Pi's HaLow AP.
     *
     * Turn it on only when the AP on the other end is a GW_ROLE_RELAY node's
     * HaLow AP rather than a Pi. That AP's own netif is created by
     * mmhalow_init() as ESP_NETIF_DEFAULT_WIFI_STA() - a DHCP *client* shape -
     * regardless of which mode (STA or AP) the driver is actually running, so
     * esp_netif never allocates it a DHCP *server* (esp_netif_lwip.c only
     * calls dhcps_new() when ESP_NETIF_DHCP_SERVER was set at esp_netif_new()
     * time, which mmhalow_init() never requests). There is no public API to
     * retrofit that flag after the fact. Rather than hand-roll a DHCP server
     * to work around an SDK gap that's unproven on real hardware either way,
     * a relay's HaLow AP gets one fixed static address
     * (gw_halow_ap_config_t.ip) and every leaf that associates to it needs
     * its own fixed address in that same subnet, assigned here instead of
     * negotiated. Operator's responsibility to keep them unique - same as
     * SSIDs and node_ids already are. */
    bool use_static_ip;
    char static_ip[GW_IP4_STR_MAX_LEN];
    char static_gateway[GW_IP4_STR_MAX_LEN];
    char static_netmask[GW_IP4_STR_MAX_LEN];

    /* Empty means "no DNS server configured" - the pre-existing, still-valid
     * state for a leaf whose uplink is a real Pi under normal DHCP (that
     * case never reaches this field at all - see uplink_halow.c's
     * apply_static_ip(), only called when use_static_ip is set).
     *
     * A statically-addressed uplink has no DHCP lease to learn a DNS server
     * from at all, so without this field ip_forward_nat.c's propagate_dns()
     * always finds nothing to offer downstream clients - confirmed on real
     * hardware 2026-08-30 (design/ROADMAP.md), where it silently broke every
     * hostname lookup for a phone behind a leaf, while raw IP (including the
     * leaf's own web UI) kept working, which looked exactly like "no
     * internet" rather than "no DNS".
     *
     * For a leaf associating to a GW_ROLE_RELAY node's HaLow AP, this should
     * be set to that relay's own downlink address (gw_halow_ap_config_t.ip,
     * e.g. 172.16.60.1) - main/dns_forward.c runs a small forwarder there
     * that relays queries out through the relay's own uplink, using
     * whatever real DNS server *that* hop actually has (learned via DHCP,
     * same as this project's existing DNS-propagation logic already trusts
     * for a GW_ROLE_CLIENT node's SoftAP). This is deliberately not a
     * hardcoded public resolver: it stays correct if the relay's own
     * upstream network's DNS server ever changes, with no reprovisioning of
     * every leaf. */
    char static_dns[GW_IP4_STR_MAX_LEN];
} gw_uplink_config_t;

typedef struct {
    char ssid[GW_SSID_MAX_LEN + 1];
    char psk[GW_PSK_MAX_LEN + 1]; /* empty => open AP */
    uint8_t channel;
    uint8_t max_connections;

    /* If false, use esp_netif's default SoftAP subnet (192.168.4.1/24).
     * If true, apply ip/gateway/netmask below instead. */
    bool use_custom_subnet;
    char ip[GW_IP4_STR_MAX_LEN];
    char gateway[GW_IP4_STR_MAX_LEN];
    char netmask[GW_IP4_STR_MAX_LEN];
} gw_softap_config_t;

/* GW_ROLE_RELAY's uplink: the native ESP32-S3 2.4GHz radio, in plain STA
 * mode, joining a Pi's already-existing local Wi-Fi AP (`br-ahwlan` in
 * OpenMANET's own terms - every mesh point exposes one, no Pi-side change
 * needed). Ordinary WPA2-PSK, unlike the HaLow security enum above - this
 * hop is regular Wi-Fi, not HaLow. */
typedef struct {
    char ssid[GW_SSID_MAX_LEN + 1];
    char psk[GW_PSK_MAX_LEN + 1]; /* empty => open network */
} gw_wifi_uplink_config_t;

static inline bool gw_wifi_uplink_is_configured(const gw_wifi_uplink_config_t *wifi_uplink)
{
    return wifi_uplink != NULL && wifi_uplink->ssid[0] != '\0';
}

/* GW_ROLE_RELAY's downlink: the HaLow radio in AP mode, so client-role XIAOs
 * (unmodified - they just point gwcfg-set-uplink at this SSID instead of a
 * Pi's) have something to associate to. See design/ROADMAP.md item 8.
 *
 * `security` reuses gw_security_mode_t, but AP mode supports fewer values
 * than STA mode - the real morsemicro/halow SDK header says so explicitly
 * (mmwlan.h, mmwlan_ap_enable() docs, v2.11.2-esp32-2): "OWE security is not
 * currently supported for AP mode." Validated in provisioning_validate();
 * GW_SECURITY_OPEN and GW_SECURITY_SAE only. */
typedef struct {
    char ssid[GW_SSID_MAX_LEN + 1];
    char psk[GW_PSK_MAX_LEN + 1]; /* only used when security == GW_SECURITY_SAE */
    gw_security_mode_t security;

    /* Channel selection. Unlike the STA side, AP mode cannot auto-pick a
     * channel when no STA is concurrently active on the same HaLow radio
     * (which is always true here - a relay's HaLow radio only ever runs AP
     * mode) - confirmed against Morse Micro's own reference "softap" example
     * (esp-halow v2.11.2-esp32-2), which sets both explicitly from Kconfig
     * rather than leaving them at MMWLAN_AP_ARGS_INIT's 0/0. `op_class` and
     * `s1g_chan_num` are the pair mmwlan_ap_args actually wants; both are
     * looked up from the regulatory channel list already loaded by
     * mmhalow_init() (CONFIG_HALOW_COUNTRY_CODE), the same table
     * uplink_halow_scan() reads results from - see
     * downlink_halow_ap_list_channels(). */
    int16_t op_class;
    uint8_t s1g_chan_num;

    uint8_t max_stas; /* 0 => MMWLAN_DEFAULT_AP_MAX_STAS */

    /* This radio's own fixed address. No DHCP server runs here - see the
     * long comment on gw_uplink_config_t.use_static_ip for why - so every
     * leaf XIAO that associates needs its own static_ip in this subnet,
     * configured on its own uplink. Default suggested: 172.16.60.1/24, a
     * different /24 than the 172.16.50.0/24 the client role's own SoftAP
     * uses, purely so logs and ARP tables from the two roles are never
     * confusable if someone greps them side by side. */
    char ip[GW_IP4_STR_MAX_LEN];
    char netmask[GW_IP4_STR_MAX_LEN];
} gw_halow_ap_config_t;

static inline bool gw_halow_ap_is_configured(const gw_halow_ap_config_t *halow_ap)
{
    return halow_ap != NULL && halow_ap->ssid[0] != '\0';
}

/* Has an operator ever told this node which HaLow AP to join?
 *
 * Deliberately derived from the SSID rather than stored as its own flag: an
 * empty SSID cannot associate with anything, so the two can never disagree,
 * and there is no second piece of state to keep in sync across the console,
 * the web UI and the NVS load path.
 *
 * This gates real behaviour - an unconfigured node does not start the
 * reconnect loop at all (see uplink_halow_start()), which keeps the radio free
 * for scanning during setup and stops the node reporting "searching" for an AP
 * nobody ever named. */
static inline bool gw_uplink_is_configured(const gw_uplink_config_t *uplink)
{
    return uplink != NULL && uplink->ssid[0] != '\0';
}

typedef struct {
    char group[GW_IP4_STR_MAX_LEN]; /* multicast group, e.g. "239.2.3.1" */
    uint16_t port;                  /* e.g. 6969 */
} gw_cot_config_t;

/* Per-device HTTPS identity for the web UI (review finding F02,
 * design/PROJECT_REVIEW_2026-09-10.md). No CA can issue a certificate for a
 * private IP, so this is a self-signed ECDSA P-256 keypair/certificate
 * generated on-device at first boot (main/tls_identity.c) - trust is
 * established the way SSH host keys are: the operator reads the
 * certificate's fingerprint off the serial console (gwcfg-show-cert) once
 * and compares it against what their browser shows before accepting the
 * warning, not by chasing a CA.
 *
 * Stored as DER, not PEM: mbedtls_x509_crt_parse()/mbedtls_pk_parse_key()
 * both auto-detect DER vs PEM by scanning for a "-----BEGIN" header
 * (esp-idf v5.5.1 mbedtls/library/x509_crt.c L1414-1421) and fall straight
 * through to the DER parser when it's absent - confirmed against that
 * source before relying on it - so there is no need to spend flash on the
 * PEM-write code path or NVS space on base64/armor overhead. Buffer sizes
 * are generous, not tight: a P-256 SEC1 private key is ~121-138 bytes and a
 * minimal self-signed cert with this shape (short CN, P-256 key + ECDSA-
 * SHA256 signature, one basic-constraints extension) is ~350-450 bytes;
 * tls_identity_init() checks the real mbedtls-reported length against these
 * bounds rather than assuming they hold. */
typedef struct {
    bool identity_set; /* same pattern as gw_auth_config_t.password_set - an
                         * all-zero buffer is not a valid cert/key, but the
                         * explicit flag is what says "never generated" vs.
                         * "somehow zero-length", same reasoning as that
                         * field's own comment. */
    uint16_t key_der_len;
    uint8_t key_der[256];
    uint16_t cert_der_len;
    uint8_t cert_der[700];
} gw_tls_identity_t;

/* An admin credential for the web UI (design/ROADMAP.md item 1). Never the
 * plaintext password - stored_key is PBKDF2-HMAC-SHA256(password, salt,
 * iterations, 32), computed client-side in web_ui.html's bundled crypto.
 * main/auth.c only ever verifies against stored_key with one HMAC-SHA256
 * call; it never derives a key and never sees the plaintext password. */
typedef struct {
    /* Explicit rather than derived from stored_key being non-zero: an
     * all-zero PBKDF2 output is a theoretically valid hash, and conflating
     * it with "unset" would be a real bug - unlike gw_uplink_is_configured()'s
     * SSID-derived flag above, there's no other field here "unset" can
     * safely mean. */
    bool password_set;
    uint8_t salt[16];       /* esp_random(), regenerated on every password set/change */
    uint32_t iterations;    /* PBKDF2 round count used for *this* credential - stored
                              * per-credential rather than assumed to equal auth.c's
                              * current default, so raising (or lowering, if the chosen
                              * default proves too slow on real phones) the constant
                              * later doesn't strand already-provisioned devices */
    uint8_t stored_key[32]; /* PBKDF2-HMAC-SHA256 output - see this struct's own comment */

    /* First-ownership onboarding window (review finding F03,
     * design/PROJECT_REVIEW_2026-09-10.md). A shared default SoftAP
     * passphrase means anyone in radio range who knows it (every unit ships
     * with the same one) could otherwise claim the admin account before the
     * legitimate owner does - forced password setup stops a universal admin
     * password but does nothing to establish *who* gets to set it first.
     * Before `password_set`, claiming also requires presenting
     * `setup_secret` - read off the serial console
     * (`gwcfg-show-setup-secret`, and logged once at boot), the same
     * physical-presence trust model `tls_identity.c`'s certificate
     * fingerprint already uses.
     *
     * `onboarding_ever_started` and `onboarding_open` are two separate
     * bools, not one, because "never opened yet" (fresh/reset device) and
     * "opened, then closed by running out of boots" must be
     * distinguishable: both leave `onboarding_open == false`, but only the
     * first should cause auth_init() to open a fresh window. Collapsing
     * them into one flag would either reopen a timed-out window on every
     * boot (defeating the timeout) or fail to ever open one on a
     * legitimately fresh device - see auth_init()'s own comment (main/auth.c)
     * for the exact state machine. */
    bool onboarding_ever_started;
    bool onboarding_open;
    uint8_t setup_secret[16]; /* esp_fill_random(), regenerated whenever onboarding (re)opens */
    /* Decremented once per boot while onboarding_open - closes the window
     * after a bounded number of boots rather than a wall-clock timeout,
     * because this board has no RTC battery (same reasoning
     * tls_identity.c's fixed certificate validity window already uses): a
     * boot-relative timer can't measure elapsed time across a power cycle,
     * but a persisted boot count can, with no clock dependency at all. */
    uint16_t onboarding_boots_remaining;
} gw_auth_config_t;

typedef struct {
    /* Must stay first and must not change type - provisioning_load() reads
     * these before trusting anything after them. */
    uint32_t magic;   /* GW_CONFIG_MAGIC */
    uint32_t version; /* GW_CONFIG_VERSION */

    char node_id[GW_NODE_ID_MAX_LEN + 1];
    gw_node_role_t role; /* GW_ROLE_CLIENT or GW_ROLE_RELAY - see the enum above */

    /* Off by default. reject_if_remote() (main/web_ui.c) treats a request as
     * authorized only if it arrives on this node's downlink - the SoftAP for
     * GW_ROLE_CLIENT, the HaLow AP for GW_ROLE_RELAY - because there is still
     * no real authentication, so network position is the only thing standing
     * between the config UI and anyone who reaches it. A GW_ROLE_RELAY node
     * runs no SoftAP of its own, so once it has joined its Wi-Fi uplink and
     * an operator isn't in HaLow range with a second radio, that boundary
     * also cuts off the only path back in.
     *
     * Turning this on makes reject_if_remote() also accept requests arriving
     * on this node's own uplink (HaLow STA for GW_ROLE_CLIENT, native Wi-Fi
     * STA for GW_ROLE_RELAY) - an explicit, per-node trade an operator makes
     * for a specific deployment (e.g. a relay's own Pi AP they already
     * trust), not a default: it extends the unauthenticated config UI to
     * whatever network that uplink joins, mesh included. */
    bool allow_uplink_management;
    gw_uplink_config_t uplink;             /* GW_ROLE_CLIENT: HaLow STA uplink */
    gw_softap_config_t softap;             /* GW_ROLE_CLIENT: local 2.4GHz SoftAP */
    gw_wifi_uplink_config_t wifi_uplink;   /* GW_ROLE_RELAY: 2.4GHz STA uplink to the Pi */
    gw_halow_ap_config_t halow_ap;         /* GW_ROLE_RELAY: HaLow AP downlink */
    gw_cot_config_t cot;
    gw_auth_config_t auth;                 /* web UI admin credential - see its own comment */
    gw_tls_identity_t tls;                 /* web UI HTTPS identity - see its own comment */
} gw_config_t;

#ifdef __cplusplus
}
#endif
