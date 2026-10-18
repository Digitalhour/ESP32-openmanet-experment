#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "web_ui.h"

#include "auth.h"
#include "chip_temp.h"
#include "cot_relay.h"
#include "downlink_halow_ap.h"
#include "heap_guard.h"
#include "link_history.h"
#include "log_buffer.h"
#include "provisioning.h"
#include "task_stats.h"
#include "tls_identity.h"
#include "uplink_halow.h"
#include "uplink_wifi.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "web_ui";

extern const char web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const char web_ui_html_end[] asm("_binary_web_ui_html_end");

#define POST_BODY_MAX 1024

/* The new auth endpoints' request bodies are a couple of fixed-length hex
 * strings each - far smaller than POST_BODY_MAX, but its own bound so a
 * malformed/oversized body is rejected before ever reaching cJSON_Parse(). */
#define AUTH_BODY_MAX 256

#define AUTH_COOKIE_NAME "gwsess"

/* Long enough for the JSON response to be written to the socket and read by
 * the browser before the CPU resets underneath it. */
#define REBOOT_DELAY_US 500000

/* A HaLow scan sweeps every channel in the regulatory domain and dwells on
 * each, so it is measured in seconds, not milliseconds. This bound has to sit
 * below the socket timeouts set in web_ui_start() or the browser gives up
 * before the handler answers. */
#define WEB_UI_SCAN_TIMEOUT_MS 8000

/* Live in-RAM config, shared with the console (provisioning.c) and app_main.c.
 * All access goes through provisioning_config_lock(). */
static gw_config_t *s_cfg = NULL;

/* The SoftAP netif, used to decide whether a request came from a local
 * client or from the mesh - see request_is_local(). */
static esp_netif_t *s_softap_netif = NULL;

/* Extracts the peer's IPv4 address from a request's socket. esp_http_server
 * may be listening on an IPv6 socket depending on lwIP config, in which case
 * IPv4 clients appear as IPv4-mapped addresses. */
static bool peer_ipv4(httpd_req_t *req, uint32_t *out_addr)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return false;
    }

    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    if (getpeername(sockfd, (struct sockaddr *)&peer, &len) < 0) {
        return false;
    }

    if (peer.ss_family == AF_INET) {
        *out_addr = ((struct sockaddr_in *)&peer)->sin_addr.s_addr;
        return true;
    }
#if LWIP_IPV6
    if (peer.ss_family == AF_INET6) {
        struct sockaddr_in6 *peer6 = (struct sockaddr_in6 *)&peer;
        if (IN6_IS_ADDR_V4MAPPED(&peer6->sin6_addr)) {
            memcpy(out_addr, &peer6->sin6_addr.s6_addr[12], sizeof(*out_addr));
            return true;
        }
    }
#endif
    return false;
}

/* This node's own uplink netif, for allow_uplink_management - HaLow STA for
 * GW_ROLE_CLIENT, native Wi-Fi STA for GW_ROLE_RELAY. NULL (never matches)
 * if s_cfg isn't set yet, same fail-closed shape as everything else here. */
static esp_netif_t *uplink_netif(void)
{
    if (s_cfg == NULL) {
        return NULL;
    }
    return (s_cfg->role == GW_ROLE_RELAY) ? uplink_wifi_get_netif() : uplink_halow_get_netif();
}

/* Is peer_addr on netif's own subnet? Fails closed: a netif with no address
 * yet (or NULL) has no subnet for anything to be on. */
static bool netif_owns_peer(esp_netif_t *netif, uint32_t peer_addr)
{
    if (netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }
    return (peer_addr & ip_info.netmask.addr) == (ip_info.ip.addr & ip_info.netmask.addr);
}

/* httpd_start() binds every interface, so once the HaLow uplink is up these
 * endpoints would otherwise be reachable from the entire mesh - including
 * unauthenticated POST /api/config and POST /api/reboot. There's no auth on
 * this UI yet (design/ROADMAP.md item 1), so the downlink's subnet *is* the
 * authorization boundary: enforce it explicitly rather than relying on the
 * mesh being friendly.
 *
 * s_cfg->allow_uplink_management additionally accepts this node's own
 * uplink subnet - an explicit per-node opt-in (see gw_config.h), off by
 * default, because accepting it trades away this exact protection for
 * whatever network the uplink joins. */
static bool request_is_local(httpd_req_t *req)
{
    uint32_t peer_addr;
    if (!peer_ipv4(req, &peer_addr)) {
        return false;
    }

    if (netif_owns_peer(s_softap_netif, peer_addr)) {
        return true;
    }

    return s_cfg != NULL && s_cfg->allow_uplink_management && netif_owns_peer(uplink_netif(), peer_addr);
}

/* Does the request's Host header name netif's own address? Shared by
 * host_is_self() below for whichever netif(s) request_is_local() accepted
 * this request on. */
static bool host_matches_netif(httpd_req_t *req, esp_netif_t *netif)
{
    if (netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    char self[16];
    snprintf(self, sizeof(self), IPSTR, IP2STR(&ip_info.ip));

    char host[32]; /* fits "255.255.255.255:65535"; anything longer isn't us */
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    size_t self_len = strlen(self);
    if (strncmp(host, self, self_len) != 0) {
        return false;
    }
    /* Exact match, or the same address with an explicit :port. */
    return host[self_len] == '\0' || host[self_len] == ':';
}

/* The request's Host header must name whichever of this node's own
 * addresses it actually arrived on. This is the anti-DNS-rebinding half of
 * the interim CSRF defence (see reject_if_remote below): a rebinding attack
 * resolves an attacker's hostname to this device's address, so the
 * browser's requests are same-origin from its own point of view and arrive
 * here with full local-peer credentials - but carrying Host:
 * attacker.example, which is the one part of the request the attacker
 * cannot forge away.
 *
 * Checks the downlink first, then (only when allow_uplink_management is on)
 * the uplink - same pair request_is_local() checks, so a request accepted by
 * one can't be rejected here just because it arrived on the other. Fails
 * closed on a missing or oversized Host header (HTTP/1.1 requires one; every
 * browser and curl sends it). If mDNS or a captive-portal hostname is ever
 * added (ROADMAP item 4), this check must learn those names too or the UI
 * becomes unreachable through them. */
static bool host_is_self(httpd_req_t *req)
{
    if (host_matches_netif(req, s_softap_netif)) {
        return true;
    }
    return s_cfg != NULL && s_cfg->allow_uplink_management && host_matches_netif(req, uplink_netif());
}

/* Guard for every handler. Returns true if the request should be refused,
 * having already sent the error response.
 *
 * Checks two independent things: the peer must be on the downlink's subnet,
 * or the uplink's when allow_uplink_management is on (authorization boundary
 * while there's no auth), and the Host header must name this device
 * (anti-DNS-rebinding, see host_is_self). Both are interim hardening, not
 * authentication - a hostile client on either accepted network can still do
 * everything the UI can until ROADMAP item 1 lands. */
static bool reject_if_remote(httpd_req_t *req)
{
    if (request_is_local(req)) {
        if (host_is_self(req)) {
            return false;
        }
        ESP_LOGW(TAG, "refused %s with a foreign Host header", req->uri);
    } else {
        /* The peer's address, not just the URI: "outside the subnet" is
         * meaningless to whoever's reading this back without it, and it's
         * the first thing this exact class of report needs (2026-08-30 -
         * confirmed on real hardware that "unreachable" here can mean either
         * a genuine bug or simply a client on a third network that merely
         * routes to an accepted one, which the operator can't always tell
         * apart from the browser side). */
        uint32_t peer_addr;
        esp_ip4_addr_t peer_ip = { .addr = peer_ipv4(req, &peer_addr) ? peer_addr : 0 };
        ESP_LOGW(TAG, "refused %s from outside the authorized subnet(s) (peer " IPSTR ")", req->uri,
                 IP2STR(&peer_ip));
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "config is only reachable from the local Wi-Fi");
    return true;
}

/* Guard for state-changing POSTs, after reject_if_remote. Requiring a JSON
 * Content-Type makes a cross-origin fetch() a non-"simple" CORS request, so
 * the browser sends an OPTIONS preflight first - which nothing here answers
 * with Access-Control-Allow-* headers, so the POST itself is never sent.
 * Without this, any web page open on a SoftAP client can fire a text/plain
 * POST at these endpoints with no preflight at all (the handlers never read
 * the Content-Type, so the body would parse fine). Complements host_is_self:
 * that one stops rebinding (where the request *is* same-origin to the
 * browser), this one stops plain cross-origin forgery. */
static bool reject_if_not_json(httpd_req_t *req)
{
    char ctype[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) == ESP_OK &&
        strncasecmp(ctype, "application/json", 16) == 0) {
        return false;
    }
    ESP_LOGW(TAG, "refused %s without a JSON Content-Type", req->uri);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Type must be application/json");
    return true;
}

/* Third member of the guard trio, after reject_if_remote/reject_if_not_json
 * (design/ROADMAP.md item 1). Defends against a different attacker than
 * either of those: someone already on the accepted subnet with the right
 * Host header - i.e. a teammate who has the Wi-Fi passphrase but shouldn't
 * be an administrator. The frontend only ever reads the status code here,
 * never a body, so a plain httpd_resp_send_err() is correct - unlike the new
 * auth endpoints below, which return structured JSON on failure and
 * therefore can't use it - see this file's other new comment on why. */
static bool auth_require_session(httpd_req_t *req)
{
    char token[AUTH_TOKEN_HEX_LEN + 1];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, AUTH_COOKIE_NAME, token, &len) == ESP_OK &&
        auth_check_session(token)) {
        return false;
    }
    ESP_LOGW(TAG, "refused %s without a valid session", req->uri);
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
    return true;
}

/* Applied to every credential/session/config response (review finding F02,
 * design/PROJECT_REVIEW_2026-09-10.md) so neither the browser's own cache
 * nor a shared/corporate proxy in front of it retains a copy - config
 * responses never carry passphrases (see config_get_handler's own comment),
 * but SSIDs, node identity and session-authenticated state have no business
 * sitting in a cache either. Must be called before any httpd_resp_send*(). */
static void set_no_store(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

/* httpd_resp_send_err() always wraps its message in an HTML error page
 * (confirmed against esp_http_server.h) - fine for the plain-text guard
 * rejections above, wrong for anything the frontend needs to JSON.parse(),
 * like a lockout's retry_after_s. There is also no 429 or 409 member in
 * httpd_err_code_t (confirmed by reading it directly - the same gap this
 * project already hit for HTTPD_503/HTTPD_409, see CLAUDE.md), so a custom
 * status line has to go through httpd_resp_set_status() instead, which takes
 * an arbitrary string. This is the one shared helper every new auth endpoint
 * error path below uses instead of httpd_resp_send_err() - takes ownership
 * of body (always deletes it) and serializes it the same way every other
 * handler in this file already does, just with an optional custom status
 * line set first. */
static esp_err_t send_auth_json(httpd_req_t *req, const char *status, cJSON *body)
{
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    set_no_store(req); /* every auth_*_handler funnels its response through here */
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

static cJSON *make_auth_error(const char *error_code)
{
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddStringToObject(root, "error", error_code);
    }
    return root;
}

/* Sets the session cookie. httpd_resp_set_hdr() stores the pointer verbatim,
 * not a copy (see root_get_handler's own comment on the same function below)
 * - cookie_buf must stay in scope until the handler's httpd_resp_send*()
 * call actually flushes headers, i.e. callers build this on their own stack
 * and must not return or reuse the buffer before sending the response. */
static void set_session_cookie(httpd_req_t *req, char *cookie_buf, size_t cookie_buf_size,
                                const char *token_hex)
{
    /* Secure added now that the transport is actually HTTPS (review finding
     * F02, design/PROJECT_REVIEW_2026-09-10.md) - without it a browser will
     * still send this cookie over a plain-HTTP connection if one existed,
     * which none does here, but the attribute is what makes that a browser
     * guarantee rather than just true by construction today. */
    snprintf(cookie_buf, cookie_buf_size,
             "%s=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=43200", AUTH_COOKIE_NAME,
             token_hex);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_buf);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }
    const size_t len = web_ui_html_end - web_ui_html_start;
    httpd_resp_set_type(req, "text/html");
    /* The embedded asset is gzip, not HTML - main/CMakeLists.txt runs
     * minify_web_ui.py --gzip over web_ui.html and embeds the compressed
     * result (52,818 bytes of source become 10,668 in flash). Content-Type
     * still describes the *decoded* body, per RFC 9110 8.4; Content-Encoding
     * is what tells the browser to inflate it first. Without this header the
     * page renders as binary garbage.
     *
     * Safe to serve unconditionally: every browser sends
     * `Accept-Encoding: gzip` and has since HTTP/1.1, and there is no
     * uncompressed copy in flash to fall back to anyway. Worth knowing during
     * bring-up: bare `curl http://172.16.50.1/` does *not* advertise gzip and
     * will dump the compressed bytes - use `curl --compressed`.
     *
     * The header value must outlive the call: httpd_resp_set_hdr() stores the
     * caller's pointers verbatim rather than copying the strings
     * (esp_http_server/src/httpd_txrx.c L189-190 at v5.5.1,
     * `ra->resp_hdrs[n].field = field; ... .value = value;`). String literals
     * satisfy that; a stack buffer would not. */
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, web_ui_html_start, len);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    provisioning_config_lock();
    cJSON_AddStringToObject(root, "node_id", s_cfg->node_id);
    cJSON_AddStringToObject(root, "role", provisioning_role_name(s_cfg->role));
    cJSON_AddBoolToObject(root, "allow_uplink_management", s_cfg->allow_uplink_management);

    cJSON *uplink = cJSON_AddObjectToObject(root, "uplink");
    cJSON_AddStringToObject(uplink, "ssid", s_cfg->uplink.ssid);
    cJSON_AddStringToObject(uplink, "security", provisioning_security_name(s_cfg->uplink.security));
    /* Passphrases are deliberately never echoed back - see web_ui.html's
     * "leave blank to keep current" fields and copy_json_str() below. */
    cJSON_AddBoolToObject(uplink, "use_static_ip", s_cfg->uplink.use_static_ip);
    cJSON_AddStringToObject(uplink, "static_ip", s_cfg->uplink.static_ip);
    cJSON_AddStringToObject(uplink, "static_gateway", s_cfg->uplink.static_gateway);
    cJSON_AddStringToObject(uplink, "static_netmask", s_cfg->uplink.static_netmask);
    cJSON_AddStringToObject(uplink, "static_dns", s_cfg->uplink.static_dns);

    cJSON *softap = cJSON_AddObjectToObject(root, "softap");
    cJSON_AddStringToObject(softap, "ssid", s_cfg->softap.ssid);
    cJSON_AddNumberToObject(softap, "channel", s_cfg->softap.channel);

    cJSON *wifi_uplink = cJSON_AddObjectToObject(root, "wifi_uplink");
    cJSON_AddStringToObject(wifi_uplink, "ssid", s_cfg->wifi_uplink.ssid);

    cJSON *halow_ap = cJSON_AddObjectToObject(root, "halow_ap");
    cJSON_AddStringToObject(halow_ap, "ssid", s_cfg->halow_ap.ssid);
    cJSON_AddStringToObject(halow_ap, "security", provisioning_security_name(s_cfg->halow_ap.security));
    cJSON_AddNumberToObject(halow_ap, "op_class", s_cfg->halow_ap.op_class);
    cJSON_AddNumberToObject(halow_ap, "s1g_chan_num", s_cfg->halow_ap.s1g_chan_num);
    cJSON_AddNumberToObject(halow_ap, "max_stas", s_cfg->halow_ap.max_stas);
    cJSON_AddStringToObject(halow_ap, "ip", s_cfg->halow_ap.ip);
    cJSON_AddStringToObject(halow_ap, "netmask", s_cfg->halow_ap.netmask);

    cJSON *cot = cJSON_AddObjectToObject(root, "cot");
    cJSON_AddStringToObject(cot, "group", s_cfg->cot.group);
    cJSON_AddNumberToObject(cot, "port", s_cfg->cot.port);
    provisioning_config_unlock();

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    set_no_store(req);
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

/* Adds "<name>": "a.b.c.d" for a netif's current address, or null if the
 * interface has no address yet (normal for the uplink before DHCP). */
static void add_netif_ip(cJSON *parent, const char *name, esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        cJSON_AddNullToObject(parent, name);
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
    cJSON_AddStringToObject(parent, name, buf);
}

/* Live device state, polled by the page. This is the bring-up diagnostic
 * surface: without it, "is the uplink actually up, and did the relay start?"
 * is only answerable over the serial console, which is exactly the thing you
 * don't have when the device is deployed somewhere awkward. Read-only - it
 * exposes no secrets (no passphrases), but is still gated to SoftAP clients
 * like every other handler. */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    provisioning_config_lock();
    cJSON_AddStringToObject(root, "node_id", s_cfg->node_id);
    gw_node_role_t role = s_cfg->role;
    cJSON_AddStringToObject(root, "role", provisioning_role_name(role));
    cJSON *cot = cJSON_AddObjectToObject(root, "cot");
    cJSON_AddStringToObject(cot, "group", s_cfg->cot.group);
    cJSON_AddNumberToObject(cot, "port", s_cfg->cot.port);
    provisioning_config_unlock();

    cJSON_AddBoolToObject(cot, "running", cot_relay_is_running());

    /* Not a generic per-netif traffic counter - see cot_relay_counters_t's
     * own doc comment for why that isn't available through public ESP-IDF
     * API. This counts the CoT payload itself moving through the one
     * datapath this project fully owns, which is the traffic this project
     * actually exists to move. */
    cot_relay_counters_t cot_uplink_counters, cot_downlink_counters;
    cot_relay_get_counters(&cot_uplink_counters, &cot_downlink_counters);
    cJSON *cot_up = cJSON_AddObjectToObject(cot, "uplink_side");
    cJSON_AddNumberToObject(cot_up, "rx_packets", cot_uplink_counters.rx_packets);
    cJSON_AddNumberToObject(cot_up, "rx_bytes", (double)cot_uplink_counters.rx_bytes);
    cJSON_AddNumberToObject(cot_up, "tx_packets", cot_uplink_counters.tx_packets);
    cJSON_AddNumberToObject(cot_up, "tx_bytes", (double)cot_uplink_counters.tx_bytes);
    cJSON *cot_down = cJSON_AddObjectToObject(cot, "downlink_side");
    cJSON_AddNumberToObject(cot_down, "rx_packets", cot_downlink_counters.rx_packets);
    cJSON_AddNumberToObject(cot_down, "rx_bytes", (double)cot_downlink_counters.rx_bytes);
    cJSON_AddNumberToObject(cot_down, "tx_packets", cot_downlink_counters.tx_packets);
    cJSON_AddNumberToObject(cot_down, "tx_bytes", (double)cot_downlink_counters.tx_bytes);
    /* Review finding F09 - see cot_relay_get_wrong_dest_drops()'s own comment. */
    cJSON_AddNumberToObject(cot, "wrong_dest_drops", cot_relay_get_wrong_dest_drops());

    cJSON *uplink = cJSON_AddObjectToObject(root, "uplink");
    /* "connected" tracks the DHCP lease, not raw 802.11 association - the
     * same distinction ip_event_handler() draws, and the one that actually
     * determines whether NAT and the relay could start. */
    cJSON_AddBoolToObject(uplink, "connected", uplink_halow_is_connected());

    /* "state" is the finer-grained version, and the one that makes this panel
     * a bring-up instrument rather than a health light: "searching" and
     * "associated, no lease" are the two milestones of step 3 in
     * design/HARDWARE.md failing respectively, with entirely different causes
     * (RF/country/credentials vs. DHCP on the Pi). A single boolean cannot
     * tell them apart. */
    uplink_link_state_t link_state = uplink_halow_get_link_state();
    cJSON_AddStringToObject(uplink, "state", uplink_halow_link_state_name(link_state));
    cJSON_AddBoolToObject(uplink, "associated", link_state >= UPLINK_LINK_ASSOCIATED);
    cJSON_AddBoolToObject(uplink, "radio_ready", uplink_halow_is_ready());
    /* Lets the page tell "you haven't set this up yet" apart from "setup is
     * done and something is wrong", which are the same shape of empty status
     * panel but need opposite advice from the operator. */
    cJSON_AddBoolToObject(uplink, "configured", link_state != UPLINK_LINK_UNCONFIGURED);

    /* Null rather than a sentinel number when unknown, so the page can render
     * "-" instead of a misleading -2147483648. Cached, not live - see
     * link_history.h's link_history_get_latest_halow_rssi() comment: this
     * endpoint always queries both roles' RSSI unconditionally (comment
     * below), and a live call here would mean every page load contends with
     * link_history.c's own timer for the single-owner radio_control task. */
    int32_t rssi = link_history_get_latest_halow_rssi();
    if (rssi == INT32_MIN) {
        cJSON_AddNullToObject(uplink, "rssi");
    } else {
        cJSON_AddNumberToObject(uplink, "rssi", rssi);
    }

    add_netif_ip(uplink, "ip", uplink_halow_get_netif());

    cJSON *softap = cJSON_AddObjectToObject(root, "softap");
    add_netif_ip(softap, "ip", s_softap_netif);
    wifi_sta_list_t sta_list;
    cJSON_AddNumberToObject(softap, "clients",
                            esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK ? sta_list.num : -1);

    /* GW_ROLE_RELAY's status - reported unconditionally, same reasoning as
     * the rest of this endpoint always including every section: the getters
     * below are safe to call regardless of active role (uplink_wifi_init()/
     * downlink_halow_ap_init() simply never ran on a GW_ROLE_CLIENT node, so
     * they report their own "not configured"/not-ready idle state rather
     * than anything misleading), and it's simpler for the page to always
     * receive both shapes and render whichever the "role" field says is
     * live. */
    cJSON *wifi_uplink = cJSON_AddObjectToObject(root, "wifi_uplink");
    uplink_wifi_link_state_t wifi_state = uplink_wifi_get_link_state();
    cJSON_AddStringToObject(wifi_uplink, "state", uplink_wifi_link_state_name(wifi_state));
    cJSON_AddBoolToObject(wifi_uplink, "connected", uplink_wifi_is_connected());
    int8_t wifi_rssi = link_history_get_latest_wifi_rssi();
    if (wifi_rssi == INT8_MIN) {
        cJSON_AddNullToObject(wifi_uplink, "rssi");
    } else {
        cJSON_AddNumberToObject(wifi_uplink, "rssi", wifi_rssi);
    }
    add_netif_ip(wifi_uplink, "ip", uplink_wifi_get_netif());

    cJSON *halow_ap = cJSON_AddObjectToObject(root, "halow_ap");
    cJSON_AddBoolToObject(halow_ap, "ready", downlink_halow_ap_is_ready());
    /* Best-effort only - mmhalow_wifi_start() returns void, so this reflects
     * "we called it", not confirmation the AP is actually on air. See
     * downlink_halow_ap.h. */
    cJSON_AddBoolToObject(halow_ap, "started", downlink_halow_ap_is_started());
    /* Unlike "started" above, this *is* proof the AP is on air - see
     * downlink_halow_ap_get_sta_count()'s own comment. */
    cJSON_AddNumberToObject(halow_ap, "connected_clients", downlink_halow_ap_get_sta_count());
    add_netif_ip(halow_ap, "ip", downlink_halow_ap_get_netif());

    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    uint32_t heap_free, heap_min, heap_free_internal;
    heap_guard_get_stats(&heap_free, &heap_min, &heap_free_internal);
    cJSON_AddNumberToObject(sys, "heap_free", heap_free);
    cJSON_AddNumberToObject(sys, "heap_min", heap_min);
    /* Free *internal* SRAM alone - see heap_guard.h. Worth its own field
     * because CONFIG_SPIRAM_USE_MALLOC=y makes heap_free/heap_min above a
     * combined internal+PSRAM figure that can look healthy while internal
     * SRAM (which DMA descriptors and task stacks are pinned to) is actually
     * tight. */
    cJSON_AddNumberToObject(sys, "heap_free_internal", heap_free_internal);
    /* Whether the CoT relay is currently dropping datagrams under heap
     * pressure rather than forwarding them - see GW_HEAP_COT_SHED_ENTER_BYTES. */
    cJSON_AddBoolToObject(sys, "cot_relay_shed_active", heap_guard_should_shed_cot());
    /* ESP32-S3 die temperature, not the HaLow module's - see chip_temp.h for
     * why the latter isn't readable at all. Null (rather than a fabricated
     * number) when the sensor never initialized or a read fails. */
    float chip_celsius;
    if (chip_temp_read_celsius(&chip_celsius)) {
        cJSON_AddNumberToObject(sys, "chip_temp_c", chip_celsius);
    } else {
        cJSON_AddNullToObject(sys, "chip_temp_c");
    }
    /* Build-time regulatory domain. Worth surfacing because it cannot be
     * changed at runtime and a mismatch with the mesh Pi is the failure that
     * blocks association outright (design/PI_SIDE.md item 3). */
    cJSON_AddStringToObject(sys, "country", CONFIG_HALOW_COUNTRY_CODE);

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(sys, "version", desc ? desc->version : "unknown");

    /* Which OTA slot is running - meaningless today (always ota_0) but the
     * first thing you want to see once OTA updates exist. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(sys, "partition", running ? running->label : "unknown");

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

/* Copies a JSON string field into out (bounded, NUL-terminated) if present
 * and non-empty; a missing/empty field means "keep the current value" and
 * out is left untouched. Sets *too_long and leaves out untouched if the
 * value doesn't fit - callers must check this and reject the request. */
static void copy_json_str(const cJSON *parent, const char *key, char *out, size_t out_size, bool *too_long)
{
    *too_long = false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        return;
    }
    if (strlen(item->valuestring) >= out_size) {
        *too_long = true;
        return;
    }
    strlcpy(out, item->valuestring, out_size);
}

/* Copies a JSON numeric field into *out (only on success) if present.
 * Missing means "keep the current value" - *out is left untouched and
 * *bad_value is not set. Present-but-invalid (wrong type, fractional, or
 * outside [min,max]) sets *bad_value and leaves *out untouched - callers
 * must check this and reject the request, exactly like copy_json_str's
 * too_long.
 *
 * Deliberately doesn't use cJSON's own valueint: it's computed as
 * (int)valuedouble at parse time, and casting a double outside int's range
 * to int is undefined behavior (C11 6.3.1.4) - and even where it isn't UB
 * in practice, blindly narrowing an in-range-for-int-but-not-for-the-target
 * value (e.g. valueint 262 cast to uint8_t by the caller) silently wraps to
 * a small, legal-looking number instead of being rejected. Bounds are
 * checked against valuedouble directly, before anything narrower ever casts
 * it. Review finding F10, design/PROJECT_REVIEW_2026-09-10.md ("channel 262
 * cast becomes 6 and accepted"). */
static void copy_json_int_range(const cJSON *parent, const char *key, long min, long max, long *out,
                                 bool *bad_value)
{
    *bad_value = false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (item == NULL) {
        return;
    }
    if (!cJSON_IsNumber(item)) {
        *bad_value = true;
        return;
    }
    double v = item->valuedouble;
    if (v < (double)min || v > (double)max) {
        *bad_value = true;
        return;
    }
    long iv = (long)v;
    if ((double)iv != v) {
        *bad_value = true; /* fractional, e.g. channel 6.5 */
        return;
    }
    *out = iv;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || reject_if_not_json(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= POST_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or empty");
        return ESP_FAIL;
    }

    char buf[POST_BODY_MAX];
    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to read body");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    /* cJSON parses nested containers recursively, and ESP-IDF compiles it with
     * the upstream default CJSON_NESTING_LIMIT of 1000 (components/json builds
     * cJSON.c with no override; cJSON.c v1.7.x checks the limit only *after*
     * recursing). 1000 frames is far more than this task's stack, so a ~1 KB
     * body of "[[[[..." would panic before the limit is ever reached. A valid
     * config body contains at most 6 objects (uplink, softap, wifi_uplink,
     * halow_ap, cot, plus root) and no arrays, so cap total containers well
     * below that before recursion depth can matter. */
    int container_budget = 16;
    for (const char *p = buf; *p != '\0'; p++) {
        if ((*p == '{' || *p == '[') && --container_budget < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too many nested JSON containers");
            return ESP_FAIL;
        }
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Validate into a scratch copy first so a rejected request never
     * partially mutates the live config. seen_revision is read in the same
     * lock scope as the snapshot it's paired with - see its use below and
     * provisioning_config_revision()'s own comment (review finding F11,
     * design/PROJECT_REVIEW_2026-09-10.md). */
    provisioning_config_lock();
    gw_config_t work;
    memcpy(&work, s_cfg, sizeof(work));
    uint32_t seen_revision = provisioning_config_revision();
    provisioning_config_unlock();

    bool too_long = false;
    bool bad_value = false;

    copy_json_str(root, "node_id", work.node_id, sizeof(work.node_id), &too_long);

    const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
    if (cJSON_IsString(role) && role->valuestring != NULL) {
        work.role = provisioning_parse_role(role->valuestring);
    }

    const cJSON *allow_uplink_mgmt = cJSON_GetObjectItemCaseSensitive(root, "allow_uplink_management");
    if (cJSON_IsBool(allow_uplink_mgmt)) {
        work.allow_uplink_management = cJSON_IsTrue(allow_uplink_mgmt);
    }

    const cJSON *uplink = cJSON_GetObjectItemCaseSensitive(root, "uplink");
    if (!too_long && cJSON_IsObject(uplink)) {
        copy_json_str(uplink, "ssid", work.uplink.ssid, sizeof(work.uplink.ssid), &too_long);
        if (!too_long) {
            copy_json_str(uplink, "psk", work.uplink.psk, sizeof(work.uplink.psk), &too_long);
        }
        const cJSON *sec = cJSON_GetObjectItemCaseSensitive(uplink, "security");
        if (cJSON_IsString(sec) && sec->valuestring != NULL &&
            !provisioning_parse_security(sec->valuestring, &work.uplink.security)) {
            bad_value = true;
        }
        const cJSON *use_static = cJSON_GetObjectItemCaseSensitive(uplink, "use_static_ip");
        if (cJSON_IsBool(use_static)) {
            work.uplink.use_static_ip = cJSON_IsTrue(use_static);
        }
        if (!too_long) {
            copy_json_str(uplink, "static_ip", work.uplink.static_ip, sizeof(work.uplink.static_ip), &too_long);
        }
        if (!too_long) {
            copy_json_str(uplink, "static_gateway", work.uplink.static_gateway,
                          sizeof(work.uplink.static_gateway), &too_long);
        }
        if (!too_long) {
            copy_json_str(uplink, "static_netmask", work.uplink.static_netmask,
                          sizeof(work.uplink.static_netmask), &too_long);
        }
        if (!too_long) {
            copy_json_str(uplink, "static_dns", work.uplink.static_dns,
                          sizeof(work.uplink.static_dns), &too_long);
        }
    }

    const cJSON *softap = cJSON_GetObjectItemCaseSensitive(root, "softap");
    if (!too_long && cJSON_IsObject(softap)) {
        copy_json_str(softap, "ssid", work.softap.ssid, sizeof(work.softap.ssid), &too_long);
        if (!too_long) {
            copy_json_str(softap, "psk", work.softap.psk, sizeof(work.softap.psk), &too_long);
        }
        long channel = work.softap.channel;
        bool channel_bad;
        copy_json_int_range(softap, "channel", 0, 255, &channel, &channel_bad);
        bad_value = bad_value || channel_bad;
        work.softap.channel = (uint8_t)channel;
    }

    const cJSON *wifi_uplink = cJSON_GetObjectItemCaseSensitive(root, "wifi_uplink");
    if (!too_long && cJSON_IsObject(wifi_uplink)) {
        copy_json_str(wifi_uplink, "ssid", work.wifi_uplink.ssid, sizeof(work.wifi_uplink.ssid), &too_long);
        if (!too_long) {
            copy_json_str(wifi_uplink, "psk", work.wifi_uplink.psk, sizeof(work.wifi_uplink.psk), &too_long);
        }
    }

    const cJSON *halow_ap = cJSON_GetObjectItemCaseSensitive(root, "halow_ap");
    if (!too_long && cJSON_IsObject(halow_ap)) {
        copy_json_str(halow_ap, "ssid", work.halow_ap.ssid, sizeof(work.halow_ap.ssid), &too_long);
        if (!too_long) {
            copy_json_str(halow_ap, "psk", work.halow_ap.psk, sizeof(work.halow_ap.psk), &too_long);
        }
        const cJSON *ap_sec = cJSON_GetObjectItemCaseSensitive(halow_ap, "security");
        if (cJSON_IsString(ap_sec) && ap_sec->valuestring != NULL &&
            !provisioning_parse_security(ap_sec->valuestring, &work.halow_ap.security)) {
            bad_value = true;
        }
        long op_class = work.halow_ap.op_class;
        bool op_class_bad;
        copy_json_int_range(halow_ap, "op_class", INT16_MIN, INT16_MAX, &op_class, &op_class_bad);
        bad_value = bad_value || op_class_bad;
        work.halow_ap.op_class = (int16_t)op_class;

        long chan_num = work.halow_ap.s1g_chan_num;
        bool chan_num_bad;
        copy_json_int_range(halow_ap, "s1g_chan_num", 0, 255, &chan_num, &chan_num_bad);
        bad_value = bad_value || chan_num_bad;
        work.halow_ap.s1g_chan_num = (uint8_t)chan_num;

        long max_stas = work.halow_ap.max_stas;
        bool max_stas_bad;
        copy_json_int_range(halow_ap, "max_stas", 0, 255, &max_stas, &max_stas_bad);
        bad_value = bad_value || max_stas_bad;
        work.halow_ap.max_stas = (uint8_t)max_stas;
        if (!too_long) {
            copy_json_str(halow_ap, "ip", work.halow_ap.ip, sizeof(work.halow_ap.ip), &too_long);
        }
        if (!too_long) {
            copy_json_str(halow_ap, "netmask", work.halow_ap.netmask, sizeof(work.halow_ap.netmask), &too_long);
        }
    }

    const cJSON *cot = cJSON_GetObjectItemCaseSensitive(root, "cot");
    if (!too_long && cJSON_IsObject(cot)) {
        copy_json_str(cot, "group", work.cot.group, sizeof(work.cot.group), &too_long);
        long port = work.cot.port;
        bool port_bad;
        copy_json_int_range(cot, "port", 0, 65535, &port, &port_bad);
        bad_value = bad_value || port_bad;
        work.cot.port = (uint16_t)port;
    }

    cJSON_Delete(root);

    if (too_long) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a field value is too long");
        return ESP_FAIL;
    }
    if (bad_value) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "a field has an invalid type, value, or out-of-range number");
        return ESP_FAIL;
    }

    /* Semantic validation on top of the length checks above: this is what
     * stops a 4-character Wi-Fi passphrase or channel 99 from being saved
     * and taking the SoftAP - and with it this very UI - down at next boot. */
    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, reason);
        return ESP_FAIL;
    }

    /* Save and write-back under one lock hold so NVS and the live config can't
     * end up disagreeing if a console edit interleaves (the console already
     * holds this lock across its own NVS write in cmd_gwcfg_save, so blocking
     * briefly on flash I/O here is precedented).
     *
     * Review finding F11 (design/PROJECT_REVIEW_2026-09-10.md): `work` above
     * was built from a snapshot read *before* the parsing/validation this
     * request just did, with the lock released in between (deliberately - see
     * that snapshot's own comment on why: holding it across a slow JSON parse
     * would stall every other task waiting on the same config, including the
     * console). If a console command (or another request) committed a change
     * in that window, blindly writing `work` back here would silently
     * overwrite it with this request's now-stale view - "last writer wins" on
     * fields this request never even looked at, not just the ones it meant to
     * change. provisioning_config_revision() is what actually detects that:
     * every write anywhere in the firmware bumps it (see
     * provisioning_config_commit()'s own comment), so a mismatch here means
     * exactly one thing - something else committed while this request was in
     * flight - and this request refuses rather than guesses at a merge. The
     * operator (or the page, on a retry) sees current values and can resubmit
     * cleanly instead of a change silently vanishing. */
    provisioning_config_lock();
    if (provisioning_config_revision() != seen_revision) {
        provisioning_config_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                             "config was changed by another session while this request was being "
                             "processed - reload and try again");
        return ESP_FAIL;
    }
    esp_err_t err = provisioning_save(&work);
    if (err == ESP_OK) {
        provisioning_config_commit(&work);
    }
    provisioning_config_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioning_save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"saved\"}");
}

/* Serves the in-RAM log ring as plain text, newest content last.
 *
 * Read-only and SoftAP-gated like everything else. Note the logs may contain
 * SSIDs and IP addresses but never passphrases - nothing in this firmware logs
 * one, and that must stay true. */
static esp_err_t log_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    size_t cap = log_buffer_capacity() + 1;
    char *buf = malloc(cap);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    size_t len = log_buffer_read(buf, cap);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    return err;
}

static void scan_result_cb(const uplink_scan_result_t *result, void *ctx)
{
    cJSON *array = (cJSON *)ctx;

    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return;
    }
    cJSON_AddStringToObject(item, "ssid", result->ssid);
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", result->bssid[0],
             result->bssid[1], result->bssid[2], result->bssid[3], result->bssid[4],
             result->bssid[5]);
    cJSON_AddStringToObject(item, "bssid", bssid);
    cJSON_AddNumberToObject(item, "rssi", result->rssi);
    /* kHz as a whole number, not MHz as a fraction: cJSON prints
     * integer-valued numbers with %d and everything else with %g, and %g is
     * one `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` away from emitting malformed
     * JSON. The page divides for display. */
    cJSON_AddNumberToObject(item, "freq_khz", (double)(result->freq_hz / 1000u));
    cJSON_AddNumberToObject(item, "bw_mhz", result->bw_mhz);
    cJSON_AddItemToArray(array, item);
}

/* Starts a HaLow scan and returns immediately - answers "is the Pi's AP even
 * there?" without a serial cable or a Pi-side capture, without blocking this
 * endpoint's caller for the scan's duration to find out.
 *
 * Stage F review finding (design/PROJECT_REVIEW_2026-09-10.md): "make scans
 * asynchronous and bounded". This used to call the blocking uplink_halow_scan()
 * directly and hold the connection open for up to WEB_UI_SCAN_TIMEOUT_MS - and
 * since esp_http_server serves one request at a time on this single httpd
 * task (config.httpd.lru_purge_enable is about connection reuse, not
 * concurrency), that meant every other client, including a page just polling
 * /api/status, stalled for the same 8s. POST here only submits the scan
 * (uplink_halow_scan_start(), bounded by the same radio_control_run() budget
 * every other radio call in this file already accepts blocking on); the page
 * polls GET /api/scan below to find out when it's done.
 *
 * Two things worth knowing about the eventual result, both surfaced in the
 * page's help text:
 *
 *  - it only covers channels legal in this build's CONFIG_HALOW_COUNTRY_CODE,
 *    so an empty list is evidence about the region setting as much as about
 *    the AP;
 *  - scanning briefly takes the radio away from the uplink, so a connected
 *    node may drop and re-associate afterwards. That's acceptable for a
 *    deliberate operator action, and the reconnect loop handles it.
 */
static esp_err_t scan_post_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || reject_if_not_json(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    /* esp_http_server's httpd_err_code_t has no 503 or 409 (the enum stops at
     * 431 and omits both), so these use 500 with a specific message rather
     * than a status code the browser could act on. The message is what the
     * page surfaces to the operator, and it's the part that matters here. */
    if (!uplink_halow_is_ready()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "the HaLow radio isn't initialized - nothing to scan with");
        return ESP_FAIL;
    }

    esp_err_t scan_err = uplink_halow_scan_start(WEB_UI_SCAN_TIMEOUT_MS);
    if (scan_err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "a scan is already running");
        return ESP_FAIL;
    }
    if (scan_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to start the scan");
        return ESP_FAIL;
    }

    /* 202: accepted, not yet done - GET /api/scan below is where the result
     * shows up. HTTPD_500_INTERNAL_SERVER_ERROR above is the only named
     * status esp_http_server's httpd_err_code_t offers for the error paths;
     * a literal status line is used here for the same reason, since a plain
     * httpd_resp_send_err() would print "202" as an error page body. */
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

/* Polls the scan started by POST /api/scan above. Non-blocking -
 * uplink_halow_scan_poll() never calls into the radio, so this is cheap
 * enough for the page to call on a short timer while a scan is running. */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    uplink_halow_scan_state_t state = uplink_halow_scan_poll();

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root ? cJSON_AddArrayToObject(root, "aps") : NULL;
    if (array == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root, "running", state == UPLINK_HALOW_SCAN_RUNNING);

    /* Always the *last finalized* scan's results, per
     * uplink_halow_scan_get_cached_results()'s own comment - including while
     * "running" is true and a newer scan is already under way, so a page that
     * polls mid-scan still has something to show rather than a blank panel
     * flickering back in once this one finishes. "complete" stays false (not
     * just absent) until a first scan actually finishes, same shape as every
     * other bool field here - simpler for the page than a tri-state. */
    bool complete = false;
    uplink_halow_scan_get_cached_results(scan_result_cb, array, &complete);
    cJSON_AddBoolToObject(root, "complete", complete);
    cJSON_AddStringToObject(root, "country", CONFIG_HALOW_COUNTRY_CODE);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

static void channel_collect_cb(const halow_ap_channel_t *chan, void *ctx)
{
    cJSON *array = (cJSON *)ctx;
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return;
    }
    cJSON_AddNumberToObject(item, "op_class", chan->op_class);
    cJSON_AddNumberToObject(item, "chan_num", chan->s1g_chan_num);
    cJSON_AddNumberToObject(item, "freq_hz", chan->freq_hz);
    cJSON_AddNumberToObject(item, "bw_mhz", chan->bw_mhz);
    cJSON_AddItemToArray(array, item);
}

/* Returns the list of legal (op_class, s1g_chan_num) channels for this build's
 * regulatory domain (US 902-928MHz) so the web UI can populate channel dropdowns. */
static esp_err_t channels_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root ? cJSON_AddArrayToObject(root, "channels") : NULL;
    if (array == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "country", CONFIG_HALOW_COUNTRY_CODE);
    downlink_halow_ap_list_channels(channel_collect_cb, array);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

static void task_stack_collect_cb(const task_stack_info_t *info, void *ctx)
{
    cJSON *array = (cJSON *)ctx;

    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return;
    }
    cJSON_AddStringToObject(item, "name", info->name);
    cJSON_AddBoolToObject(item, "present", info->present);
    cJSON_AddNumberToObject(item, "stack", (double)info->stack_total);
    /* Absent tasks report free = 0, which the page must not draw as "0 bytes
     * left" - hence the explicit `present` flag above rather than leaving the
     * client to infer it from a sentinel value. */
    cJSON_AddNumberToObject(item, "free", (double)(info->present ? info->stack_free_min : 0));
    cJSON_AddItemToArray(array, item);
}

/* Worst-case stack headroom per task, the same data as the console's
 * gwcfg-tasks.
 *
 * Deliberately its own endpoint rather than another field on /api/status: the
 * page polls status on a timer, and this costs an xTaskGetHandle() name walk
 * plus a stack scan per row (see task_stats.h). It's a diagnostic you open,
 * not a gauge that ticks.
 *
 * Reachable without a serial cable on purpose. The overflow this exists to
 * predict took down a node that had no console attached, which is the normal
 * case for anything deployed - see design/ROADMAP.md item 8. */
static esp_err_t tasks_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root ? cJSON_AddArrayToObject(root, "tasks") : NULL;
    if (array == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    task_stats_each_stack(task_stack_collect_cb, array);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

/* Separate endpoint rather than another /api/status field: at
 * LINK_HISTORY_SAMPLES entries this is heavier than the rest of the status
 * payload combined, and the data only changes once every
 * LINK_HISTORY_INTERVAL_S seconds - polling it on the same cadence as the
 * rest of /api/status (a few seconds) would mean re-sending the same numbers
 * repeatedly for nothing. */
static esp_err_t rssi_history_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    static int16_t samples[LINK_HISTORY_SAMPLES];
    size_t n = link_history_get_rssi(samples, LINK_HISTORY_SAMPLES);

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root ? cJSON_AddArrayToObject(root, "rssi_dbm") : NULL;
    if (array == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    cJSON_AddNumberToObject(root, "interval_s", LINK_HISTORY_INTERVAL_S);

    /* null rather than a sentinel number, same reasoning as every other RSSI
     * field in this file - a page shouldn't have to know INT16_MIN is the
     * "no reading" value too. */
    for (size_t i = 0; i < n; i++) {
        if (samples[i] == INT16_MIN) {
            cJSON_AddItemToArray(array, cJSON_CreateNull());
        } else {
            cJSON_AddItemToArray(array, cJSON_CreateNumber(samples[i]));
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "rebooting on web UI request");
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || reject_if_not_json(req) || auth_require_session(req)) {
        return ESP_FAIL;
    }

    /* Restarting inline would reset the CPU before the response drains out
     * of the socket, so the browser sees a connection reset instead of the
     * acknowledgement. Hand off to a one-shot timer and return normally. */
    const esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .name = "web_ui_reboot",
    };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_once(timer, REBOOT_DELAY_US);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't schedule reboot: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to schedule reboot");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");
}

/* ---- Web UI authentication (design/ROADMAP.md item 1) ----
 *
 * Six new endpoints implementing the settled challenge-response design:
 * login against an existing credential, and the first-ever password set (or
 * a later change) against a server-issued salt. auth.c owns all the
 * crypto/session/lockout state; everything here is HTTP-layer glue -
 * cookies, JSON bodies, status codes - exactly the split chip_temp.c and
 * link_history.c already use to stay independent of esp_http_server.h. */

/* Same shape as config_post_handler's inline body read, generalized since
 * three small POST bodies below need it - bounded well under POST_BODY_MAX
 * since a valid body here is one flat object with 1-2 short hex fields. */
static cJSON *read_auth_json_body(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= AUTH_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or empty");
        return NULL;
    }

    char buf[AUTH_BODY_MAX];
    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to read body");
            return NULL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    /* Same defense as config_post_handler's own container-nesting guard,
     * scaled to what a valid body here actually needs: one flat object, no
     * arrays and no real nesting at all. */
    int container_budget = 2;
    for (const char *p = buf; *p != '\0'; p++) {
        if ((*p == '{' || *p == '[') && --container_budget < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too many nested JSON containers");
            return NULL;
        }
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return NULL;
    }
    return root;
}

/* A required string field of exactly hex_len characters. Unlike
 * copy_json_str() (whose missing/empty means "leave the current value"),
 * these fields have no "current value" concept - missing, empty or
 * wrong-length is simply a bad request. Hex-ness itself is checked by
 * auth.c's own hex decoder, not here - this only extracts the string. */
static bool get_json_hex_field(const cJSON *parent, const char *key, char *out, size_t hex_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL || strlen(item->valuestring) != hex_len) {
        return false;
    }
    memcpy(out, item->valuestring, hex_len);
    out[hex_len] = '\0';
    return true;
}

static esp_err_t auth_status_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }

    char token[AUTH_TOKEN_HEX_LEN + 1];
    size_t len = sizeof(token);
    bool authenticated = httpd_req_get_cookie_val(req, AUTH_COOKIE_NAME, token, &len) == ESP_OK &&
                          auth_check_session(token);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "password_set", auth_password_is_set());
    cJSON_AddBoolToObject(root, "authenticated", authenticated);
    /* Lets the first-use screen decide whether to ask for the setup code at
     * all - onboarding can close (F03's boot-budget timeout) while
     * password_set is still false, and the frontend has no other way to
     * tell those two "no password yet" states apart. */
    cJSON_AddBoolToObject(root, "onboarding_open", auth_onboarding_is_open());
    return send_auth_json(req, NULL, root);
}

static esp_err_t auth_challenge_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }

    if (!auth_password_is_set()) {
        return send_auth_json(req, "409 Conflict", make_auth_error("no_password_set"));
    }

    uint32_t retry_after_s;
    if (auth_is_locked_out(&retry_after_s)) {
        cJSON *body = make_auth_error("locked_out");
        if (body != NULL) {
            cJSON_AddNumberToObject(body, "retry_after_s", retry_after_s);
        }
        return send_auth_json(req, "429 Too Many Requests", body);
    }

    char nonce_hex[AUTH_NONCE_HEX_LEN + 1];
    char salt_hex[AUTH_SALT_HEX_LEN + 1];
    uint32_t iterations;
    if (!auth_begin_login_challenge(nonce_hex, salt_hex, &iterations)) {
        /* Password state changed between the check above and here (e.g. a
         * concurrent gwcfg-reset-auth) - vanishingly unlikely, but report it
         * accurately rather than hand out a stale/meaningless challenge. */
        return send_auth_json(req, "409 Conflict", make_auth_error("no_password_set"));
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "salt", salt_hex);
    cJSON_AddNumberToObject(root, "iterations", iterations);
    cJSON_AddStringToObject(root, "nonce", nonce_hex);
    return send_auth_json(req, NULL, root);
}

static esp_err_t auth_login_post_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || reject_if_not_json(req)) {
        return ESP_FAIL;
    }

    cJSON *body = read_auth_json_body(req);
    if (body == NULL) {
        return ESP_FAIL; /* error already sent */
    }

    char nonce_hex[AUTH_NONCE_HEX_LEN + 1];
    char response_hex[AUTH_RESPONSE_HEX_LEN + 1];
    bool ok = get_json_hex_field(body, "nonce", nonce_hex, AUTH_NONCE_HEX_LEN) &&
              get_json_hex_field(body, "response", response_hex, AUTH_RESPONSE_HEX_LEN);
    cJSON_Delete(body);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nonce/response must be present and correctly sized");
        return ESP_FAIL;
    }

    char token_hex[AUTH_TOKEN_HEX_LEN + 1];
    uint32_t retry_after_s;
    switch (auth_verify_login(nonce_hex, response_hex, token_hex, &retry_after_s)) {
    case AUTH_LOGIN_OK: {
        char cookie_buf[128];
        set_session_cookie(req, cookie_buf, sizeof(cookie_buf), token_hex);
        cJSON *root = cJSON_CreateObject();
        if (root != NULL) {
            cJSON_AddStringToObject(root, "status", "ok");
        }
        return send_auth_json(req, NULL, root);
    }
    case AUTH_LOGIN_LOCKED_OUT: {
        cJSON *lockedBody = make_auth_error("locked_out");
        if (lockedBody != NULL) {
            cJSON_AddNumberToObject(lockedBody, "retry_after_s", retry_after_s);
        }
        return send_auth_json(req, "429 Too Many Requests", lockedBody);
    }
    case AUTH_LOGIN_NO_CHALLENGE:
    case AUTH_LOGIN_BAD_CREDENTIALS:
    default:
        /* Never distinguish the two to the client - both are "try again
         * from a fresh challenge", and telling an attacker "no challenge"
         * vs "wrong password" leaks which guesses are even worth scoring. */
        return send_auth_json(req, "401 Unauthorized", make_auth_error("invalid_credentials"));
    }
}

static esp_err_t auth_logout_post_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || reject_if_not_json(req)) {
        return ESP_FAIL;
    }

    char token[AUTH_TOKEN_HEX_LEN + 1];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, AUTH_COOKIE_NAME, token, &len) == ESP_OK) {
        auth_end_session(token);
    }

    char cookie_buf[64];
    snprintf(cookie_buf, sizeof(cookie_buf), "%s=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0",
             AUTH_COOKIE_NAME);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_buf);

    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddStringToObject(root, "status", "ok");
    }
    return send_auth_json(req, NULL, root);
}

/* Reachable without a session only while no password is set yet (first-use
 * flow); once one exists, this doubles as the "change password" entry point
 * and requires one, enforced the same way every functional endpoint already
 * is - see auth_require_session(). */
static esp_err_t auth_new_salt_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }
    if (auth_password_is_set() && auth_require_session(req)) {
        return ESP_FAIL;
    }

    char salt_hex[AUTH_SALT_HEX_LEN + 1];
    uint32_t iterations;
    auth_begin_password_salt(salt_hex, &iterations);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "salt", salt_hex);
    cJSON_AddNumberToObject(root, "iterations", iterations);
    return send_auth_json(req, NULL, root);
}

static esp_err_t auth_password_post_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || reject_if_not_json(req)) {
        return ESP_FAIL;
    }
    if (auth_password_is_set() && auth_require_session(req)) {
        return ESP_FAIL;
    }

    cJSON *body = read_auth_json_body(req);
    if (body == NULL) {
        return ESP_FAIL; /* error already sent */
    }

    char stored_key_hex[AUTH_STORED_KEY_HEX_LEN + 1];
    bool ok = get_json_hex_field(body, "stored_key", stored_key_hex, AUTH_STORED_KEY_HEX_LEN);
    /* Only meaningful (and only sent by the frontend) on the first-use claim
     * path - absent here is normal for an authenticated change-password
     * request, not an error; auth_commit_password() only checks it when no
     * password exists yet. */
    char setup_secret_hex[AUTH_SETUP_SECRET_HEX_LEN + 1];
    bool secret_present = get_json_hex_field(body, "setup_secret", setup_secret_hex, AUTH_SETUP_SECRET_HEX_LEN);
    cJSON_Delete(body);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "stored_key must be present and correctly sized");
        return ESP_FAIL;
    }

    char token_hex[AUTH_TOKEN_HEX_LEN + 1];
    switch (auth_commit_password(stored_key_hex, secret_present ? setup_secret_hex : NULL, token_hex)) {
    case AUTH_SET_OK: {
        char cookie_buf[128];
        set_session_cookie(req, cookie_buf, sizeof(cookie_buf), token_hex);
        cJSON *root = cJSON_CreateObject();
        if (root != NULL) {
            cJSON_AddStringToObject(root, "status", "ok");
        }
        return send_auth_json(req, NULL, root);
    }
    case AUTH_SET_NO_PENDING_SALT:
        return send_auth_json(req, "409 Conflict", make_auth_error("no_pending_salt"));
    case AUTH_SET_BAD_KEY:
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "stored_key is not valid hex");
        return ESP_FAIL;
    case AUTH_SET_BAD_SETUP_SECRET:
        return send_auth_json(req, "401 Unauthorized", make_auth_error("bad_setup_secret"));
    case AUTH_SET_SAVE_FAILED:
    default:
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save credential");
        return ESP_FAIL;
    }
}

esp_err_t web_ui_start(gw_config_t *cfg, esp_netif_t *softap_netif)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (softap_netif == NULL) {
        /* Without it there's no subnet to authorize against, and every
         * request would be refused - starting the server would be
         * misleading. */
        ESP_LOGE(TAG, "no SoftAP netif - refusing to start an unreachable web UI");
        return ESP_ERR_INVALID_ARG;
    }
    if (!auth_is_ready()) {
        /* auth_init() failed (e.g. mutex allocation) - every functional
         * handler below calls auth_require_session(), which calls straight
         * into auth.c's session/lockout functions. Those take s_auth_lock
         * unconditionally; with no lock that's a NULL FreeRTOS semaphore
         * handle, which asserts rather than just misbehaving (same class of
         * bug as cot_relay_get_counters() - review finding F01). Refusing to
         * start is "fail closed" for a management interface that can't prove
         * it can gate itself, per the review's "safe auth-init failure" item,
         * design/PROJECT_REVIEW_2026-09-10.md. */
        ESP_LOGE(TAG, "auth subsystem not initialized - refusing to start the web UI rather than "
                      "serve it unauthenticated or crash on first login attempt");
        return ESP_ERR_INVALID_STATE;
    }
    if (!tls_identity_is_ready()) {
        /* Same "fail closed" reasoning as the auth check above: no
         * certificate means httpd_ssl_start() has nothing to serve HTTPS
         * with. tls_identity_init() runs at boot before this function is
         * ever called (main/app_main.c) and only fails this way on a real
         * error (allocation, mbedtls) - not on first boot, which generates
         * one instead of finding it missing. */
        ESP_LOGE(TAG, "no HTTPS identity available - refusing to start the web UI");
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = cfg;
    s_softap_netif = softap_netif;

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.httpd.lru_purge_enable = true;
    /* POST /api/scan used to block for up to WEB_UI_SCAN_TIMEOUT_MS (the scan
     * itself); now it only submits the scan (uplink_halow_scan_start()) and
     * returns, so nothing left in this file blocks longer than a single
     * radio_control_run() call (RADIO_CONTROL_DEFAULT_TIMEOUT_MS, 3s -
     * main/radio_control.h) - see scan_post_handler()'s own comment. Left at
     * 15s anyway rather than trimmed to match: a generous socket timeout
     * costs nothing but a slightly later-detected dead connection, where a
     * too-tight one risks aborting a slow-but-live one. */
    config.httpd.recv_wait_timeout = 15;
    config.httpd.send_wait_timeout = 15;
    /* Default is 8 and there are 17 routes below - raised so adding one
     * doesn't fail registration at runtime instead of at compile time. */
    config.httpd.max_uri_handlers = 20;
    /* The status and scan handlers build and print whole cJSON trees on this
     * stack, on top of the TLS handshake state a plain-HTTP budget never had
     * to carry - see GW_STACK_WEB_UI's own comment (task_stats.h). (Scan
     * *results* are delivered from the driver's own task - see
     * uplink_halow.h - but assembling and serializing the response happens
     * here.) */
    config.httpd.stack_size = GW_STACK_WEB_UI;
    /* max_open_sockets is left at HTTPD_SSL_CONFIG_DEFAULT()'s own reduced
     * default (4, not plain httpd's 7) deliberately - see
     * sdkconfig.defaults' CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC comment for the
     * ~40KB-per-TLS-socket reasoning behind that default. */

    size_t cert_len = 0, key_len = 0;
    const uint8_t *cert_der = tls_identity_get_cert_der(&cert_len);
    const uint8_t *key_der = tls_identity_get_key_der(&key_len);
    /* Both fields accept DER despite the "_pem" naming - mbedtls's own
     * parsers auto-detect the format (see gw_tls_identity_t's own comment,
     * main/gw_config.h). */
    config.servercert = cert_der;
    config.servercert_len = cert_len;
    config.prvtkey_pem = key_der;
    config.prvtkey_len = key_len;
    config.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_ssl_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler },
        { .uri = "/api/channels", .method = HTTP_GET, .handler = channels_get_handler },
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/log", .method = HTTP_GET, .handler = log_get_handler },
        { .uri = "/api/tasks", .method = HTTP_GET, .handler = tasks_get_handler },
        { .uri = "/api/rssi-history", .method = HTTP_GET, .handler = rssi_history_get_handler },
        { .uri = "/api/scan", .method = HTTP_POST, .handler = scan_post_handler },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_get_handler },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler },
        { .uri = "/api/auth/status", .method = HTTP_GET, .handler = auth_status_get_handler },
        { .uri = "/api/auth/challenge", .method = HTTP_GET, .handler = auth_challenge_get_handler },
        { .uri = "/api/auth/login", .method = HTTP_POST, .handler = auth_login_post_handler },
        { .uri = "/api/auth/logout", .method = HTTP_POST, .handler = auth_logout_post_handler },
        { .uri = "/api/auth/new-salt", .method = HTTP_GET, .handler = auth_new_salt_get_handler },
        { .uri = "/api/auth/password", .method = HTTP_POST, .handler = auth_password_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s: %s", routes[i].uri, esp_err_to_name(err));
            httpd_stop(server);
            return err;
        }
    }

    ESP_LOGI(TAG, "web UI started over HTTPS (SoftAP clients only, port %d)", config.port_secure);
    return ESP_OK;
}
