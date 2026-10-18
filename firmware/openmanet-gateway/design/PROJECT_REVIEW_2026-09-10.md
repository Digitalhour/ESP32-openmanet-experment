# XIAO HaLow Gateway: architecture, code, routing and security review

**Review date:** 2026-09-10  
**Source baseline:** `ba4898e7b3dbffc925fe65d98de3c421e4d7d840`  
**Audience:** coding agent implementing the next reliability/security milestone  
**Scope:** review and implementation instructions; firmware recommendations are not implemented by this report.

## 1. Decision and executive assessment

Continue with the XIAO ESP32-S3 + WM6108 hardware, and prioritize reliable, secure access into OpenMANET. The implementation is a promising working prototype with considerable bring-up knowledge, but it is not ready to be treated as an unattended, secure field gateway. The immediate work is correctness and recovery, followed by management security and multi-node traffic control. More radio features or a new mesh protocol would distract from defects that can already interrupt ordinary operation.

The most urgent findings are:

1. **The status endpoint can assert before the CoT relay starts.** It unconditionally takes a mutex that is still NULL on a fresh/unconnected node.
2. **Management authentication runs over HTTP.** Password setup sends a reusable authentication key, and subsequent cookies and configuration secrets lack transport protection. A password-derived challenge does not protect the session or the downloaded JavaScript.
3. **DNS forwarding confuses clients with the same transaction ID, accepts replies without validating the sender, and silently truncates large messages.** The existing comment promising TCP fallback does not match the code.
4. **Radio control is called from multiple tasks despite the vendor API's serialization requirement.** Scan callbacks also retain access to caller-owned JSON across a timeout race.
5. **Datapath startup is one-shot.** Recovery after address changes, partial failure and downlink timing races is incomplete.
6. **Input validation can silently select open security or accept unusable subnet settings.** Some numeric fields wrap before validation.
7. **CoT forwarding lacks destination filtering, bounded duplicate suppression and traffic budgets.** Self-source filtering only prevents a subset of loops.

The project already does several things well: bounded receive buffers; a correct CoT truncation check; NAT on the downlink; explicit default routing; authentication and subnet/Host guards on functional HTTP endpoints; no password echo in configuration responses; independent provisioning/recovery; and useful radio/heap/stack diagnostics. Preserve these while repairing their lifecycle and trust boundaries.

### Evidence labels

- **Confirmed / source:** the defect follows directly from the inspected implementation and, where relevant, pinned upstream source. This is not a claim of a live-device reproduction.
- **Confirmed / host:** an isolated host program compiled the relevant original C functions and reproduced the behavior.
- **Risk / interleaving:** an unsafe concurrency schedule exists; actual timing and consequences need instrumented testing.
- **Hardware gate:** field behavior has not been established by this review.

Severity reflects impact and preconditions. P0 means fix before further release candidates; P1 means required before unattended field use; P2 means controlled optimization or hardening after correctness. A P0 designation is not a CVSS score or a claim of unauthenticated Internet exploitation.

## 2. What the project actually builds

### Implemented topology

```text
Phone / ATAK
  | 2.4 GHz Wi-Fi; private phone subnet
  v
XIAO CLIENT: SoftAP -> IPv4 NAPT + application CoT relay -> HaLow STA
  | 802.11ah infrastructure link
  v
XIAO RELAY: HaLow AP -> IPv4 NAPT + CoT relay + DNS proxy -> 2.4 GHz STA
  | joins an OpenMANET node's local client AP
  v
OpenMANET Linux node -> mesh backbone -> other nodes / optional Internet gate
```

A client can alternatively associate directly with a compatible HaLow infrastructure AP. That AP must actually be available and attached to the intended network. Identical radio silicon does not turn an infrastructure station into an 802.11s mesh peer.

OpenMANET currently documents 802.11s plus BATMAN-V, a shared `10.41.0.0/16` mesh domain, and local Ethernet/Wi-Fi client access. Its networking page says the HaLow AP wizard was removed. These are upstream documentation statements, not a verification of the user's installed Pi image. [OpenMANET networking](https://openmanet.github.io/docs/networking)

**Architectural recommendation:** keep the Linux/OpenMANET node responsible for mesh routing and the XIAOs responsible for access. The relay's ordinary 2.4 GHz station connection is a practical way to preserve the Pi's HaLow backbone without depending on simultaneous AP/mesh operation on its one radio. The firmware contains no BATMAN-V implementation, mesh path metrics, multi-next-hop selection, or route advertisement protocol.

### What “routing optimization” means here

Optimize forwarding correctness, address management, service reachability and recovery. Do not describe the XIAO relay as an autonomous MANET router. Its fixed uplink remains a single point of failure. An upstream backbone route change may be transparent to it; moving to another AP or getting another address is a separate access-link event.

| Mode | Benefits | Costs / prerequisites | Recommendation |
|---|---|---|---|
| Current NAPT access | Private phone subnets; no per-phone routes on Pi; low configuration cost | Typically two XIAO translations; another at Internet gate; inbound phone services unavailable by default | Default for first reliable release |
| Routed access | Can expose phone services and retain endpoint addresses | Unique subnets, return routes, explicit firewall policy, stable next hops, coordinated XIAO forwarding/NAT configuration | Later opt-in feature with a real use case |
| Transparent L2 extension | Flat addressing if genuinely supported | Requires supported driver/link addressing and bridging across both radios; multicast/loop design | Research gate, not a presumed firmware shortcut |
| New mesh routing on ESP32 | Potential independent topology | Major protocol, memory, security and interoperability project | Outside this milestone |

`PI_SIDE.md` and `ROADMAP.md` say a Pi static route alone is the future inbound-unicast fix. That is incomplete. A routed mode must also address the XIAO's current NAPT behavior, return-path consistency, duplicate phone subnets, firewalling and any intermediate relay routes. A route added only to the Internet gate may not affect peers that use a different next hop. Alternatively, carefully scoped port mappings could expose individual services, but this build explicitly disables the port-mapping facility. Neither option is implemented by changing only a sentence in the Pi guide.

Likewise, CoT forwarding provides one UDP group/port service, not full ATAK compatibility. Discovery, file transfer, voice, video and mission-package paths need separate acceptance cases. Prefer an endpoint-initiated connection to a trusted TAK service where that meets the application need; it generally fits the existing NAT topology better than exposing every phone.

## 3. Current evidence and limits

### Reviewed material

Inspected the architecture/roadmap/hardware/Pi documents; configuration and provisioning; HTTP UI and authentication; both uplinks and downlinks; NAT, DNS and CoT paths; heap/log/history diagnostics; vendor patching; partition/configuration files; PlatformIO/CMake integration; and the GitHub build/deploy workflow. Followed high-risk API assumptions into the locally installed ESP-IDF **v5.5.1** and fetched `morsemicro/halow` **2.11.2-esp32-2** source.

The worktree initially contained untracked `.vscode/` files; these were left alone. A lightweight memory lookup found no project-specific records, so this review relies on the current repository and current upstream material.

### Build performed for this review

A fresh build directory under `/tmp/xiao-gateway-review-20260910` used copies of source, `sdkconfig.defaults`, partitions and the existing fetched vendor component. It did not use the existing `build/` or a copied generated `sdkconfig`.

```sh
source /home/d3mo/esp/esp-idf/export.sh
idf.py -C /tmp/xiao-gateway-review-20260910 -DIDF_TARGET=esp32s3 build
```

Result: **exit 0**, no compiler `warning:` or `error:` diagnostics, app size **`0x1c6550`**, app slot **`0x300000`**, free **`0x139ab0` / 41%**. The isolated directory has no Git metadata, so its generated application version is `1`; this artifact is compilation evidence, not a release image. The baseline lockfile records IDF 5.5.0 while CI and this test use 5.5.1. The isolated resolver selected 5.5.1. This was a clean compile using cached dependencies, not an empty-cache download/reproducibility test. PlatformIO was inspected but not built in this review.

### Host checks actually executed

A temporary C harness compiled the original `provisioning_validate`, `provisioning_parse_security`, `pending_find`, and `ring_append` functions with minimal host definitions. It produced:

```text
zero SoftAP mask accepted: 1
unknown security becomes open: 1
JSON-style channel 262 cast becomes 6 and accepted: 1
DNS response for second client ID selects first port: 1111
exact-full log ring visible bytes: 0
```

The channel check models the exact narrowing operation in the HTTP handler; it does not execute HTTP or cJSON. DNS testing covers table selection, not live sockets. Temporary reproduction sources are `/tmp/xiao_review_host_checks.py` and `.c`; they are session artifacts. The regression specifications below are the durable handoff and should become maintained tests of the repaired code.

**Not performed:** flashing, packet injection against a device, physical power/RF measurements, browser tests, Pi configuration changes, mesh-loop reproduction, sanitizer testing of the embedded runtime, or validation of every transitive dependency. Historical bring-up claims in the roadmap remain historical evidence; none are presented as newly verified here. In particular, the roadmap itself still leaves a full leaf-phone DNS round trip and range testing open.

## 4. Findings and implementation instructions

### F01 — P0: status reads an uninitialized relay mutex

**Confirmed / source.** `main/cot_relay.c:367` (`cot_relay_get_counters`) calls `xSemaphoreTake(s_send_lock, portMAX_DELAY)` unconditionally. The mutex starts NULL and is created only in `cot_relay_start`. `main/web_ui.c:477` invokes the getter even when the preceding `running` field is false. Freshly provisioned nodes, absent uplinks, and failed relay starts all reach this state. ESP-IDF's `components/freertos/FreeRTOS-Kernel/queue.c:1697` (`xQueueSemaphoreTake`) asserts that the queue pointer is non-NULL.

**Implement:** separate statistics lifetime from the relay socket lifetime. Initialize a persistent short-duration statistics lock before the UI starts, or provide an initialization-safe snapshot with a protected readiness state. Return zero counters and explicit inactive state before startup. Do not delete a lock that getters can still reference. Separate counter protection from potentially blocking socket sends; a dashboard request must not wait indefinitely behind `sendto`.

**Acceptance:** status polling before any uplink configuration, during failed group joins, after a failed task allocation, and during reconnect/stop must never assert or hang. Concurrent snapshots must be internally consistent, including 64-bit byte counters. Test repeated init/fail/retry, not only successful startup.

### F02 — P0: HTTP exposes a reusable credential and session

**Confirmed / source.** `main/auth.c:315` (`auth_commit_password`) stores the submitted `stored_key`; `auth_verify_login` uses that same key to calculate HMAC over a challenge. `main/web_ui.c:1344` accepts it over HTTP. `set_session_cookie` at line 321 produces a bearer cookie, and `web_ui_start` uses `httpd_start`, not TLS. Configuration POSTs also carry radio passphrases over this channel.

An observer able to read the setup exchange obtains a key sufficient for future login responses without learning the human password. Observing a later session can expose its bearer cookie. An active on-path party can modify the HTTP-delivered JavaScript or configuration. Radio link encryption helps against some outsiders but does not replace authenticated management transport, especially when management traverses a leaf, relay or shared network. Captured challenge exchanges also permit offline password verification; online lockout does not prevent that. These are trust-boundary defects, not a weakness in SHA-256 itself.

**Implement:** use authenticated HTTPS for field management, with unique per-device identity and an operator-verifiable trust/bootstrap process. A certificate warning that operators routinely bypass is not a complete identity design. For small custom builds, a USB provisioning step that establishes the device certificate/fingerprint is reasonable. Evaluate the IDF HTTPS server on target before choosing connection limits. Keep the current challenge flow only as a transitional mechanism inside protected transport; do not invent another custom encryption layer around HTTP. [ESP-IDF HTTPS server source/docs](https://github.com/espressif/esp-idf/blob/v5.5.1/docs/en/api-reference/protocols/esp_https_server.rst)

Set `Secure`, `HttpOnly`, `SameSite=Strict` cookies under HTTPS; return `Cache-Control: no-store` for credential/session/config responses. Preserve subnet and Host checks, and add explicit same-origin protection for mutations. Bound session lifetime absolutely as well as by inactivity. Zero temporary key material with a non-optimizable library primitive after use. State clearly that the stored key is password-equivalent and belongs in the secret inventory.

**Acceptance:** a test capture of management traffic must contain no readable credential, session cookie or radio passphrase. Verify trusted identity on supported phones, reject a substituted identity, test logout/password-change revocation and persistence failures, and measure TLS handshake memory under simultaneous CoT/DNS traffic. Do not enable OTA before this boundary is established.

### F03 — P1: first ownership is claimable using a public Wi-Fi secret

**Confirmed / source.** `main/provisioning.c:137` sets a shared default SoftAP PSK. Before an admin password exists, `auth_new_salt_get_handler` and `auth_password_post_handler` require only subnet/Host/JSON checks. A nearby party who can join an unclaimed unit can claim its admin identity first. Forced password setup avoids a universal admin password but does not establish who owns the device.

**Implement:** generate a strong random per-device setup secret and expose it through USB/local labeling or an equivalent owner-only channel. Require a bounded physical-presence onboarding window. Close it after claim; reopen only through deliberate local recovery. Avoid MAC-derived passwords. Ensure an invalid/migrated configuration does not silently reopen public onboarding or resume forwarding with insecure defaults.

**Acceptance:** a second station without the setup secret cannot claim a new unit; timeout closes onboarding; reboot does not accidentally reopen it; reset is deliberate and documented. Validate radio-password policy independently from admin-password policy.

### F04 — P0: DNS response matching is insufficient

**Confirmed / source and host.** `main/dns_forward.c:89` matches only `txn_id`. `dns_forward_task` at line 154 stores the client's original ID unchanged. Two simultaneous clients can use the same ID; the first table match wins. At line 185 the code receives an upstream address but never checks it against the resolver queried, nor validates the DNS question or response flags. Messages as short as two bytes are accepted. A reachable attacker can inject candidates; actual off-path success still depends on discovering/guessing the socket tuple and ID.

**Implement:** either eliminate this proxy by giving leaves a verified reachable resolver, or repair it before shipping. If retained, assign an unpredictable upstream ID unique among active requests and remember original client IP/port/ID, resolver IP/port, normalized question identity and deadline. Restore the client ID only after validating a response. Reject short headers, invalid query/response direction, mismatched resolver, unmatched/expired ID and mismatched question. Apply bounded per-client/global admission; do not silently evict an unrelated request on pressure. Account for resolver changes without accepting a new server's reply to an old server's transaction. Use a bounded socket/port strategy and document the remaining entropy budget. [RFC 5452, query matching and port/ID entropy](https://www.rfc-editor.org/rfc/rfc5452.html)

**Acceptance:** two clients with identical IDs and different questions receive only their own answer, including reordered replies. Wrong source IP, wrong source port, wrong question, expired replies, duplicates and malformed headers are rejected. Pressure on the eight-slot table produces explicit controlled failure and observable counters.

### F05 — P1: DNS truncation is not a valid TCP fallback

**Confirmed / source.** `main/dns_forward.c:15` claims the TC bit tells clients to retry over TCP. Both receive paths actually call `recvfrom` into 512 bytes, do not set TC, and implement no TCP listener. The local pinned lwIP `src/api/sockets.c:1285` returns `min(buffer length, datagram length)` for `recvfrom`: oversized DNS is silently shortened here. **This is a malformed-packet bug, not the old CoT out-of-bounds read**; the APIs have different length semantics.

**Implement:** prefer passing DNS directly to the Pi's established resolver when its address can be provisioned/reached reliably. If the proxy remains necessary, define bounded EDNS/UDP behavior and implement a working TCP fallback path or a maintained forwarding component. Use `recvmsg` truncation flags, validate minimum header/question, and never forward arbitrary chopped bytes as a complete DNS message. A larger buffer alone does not solve oversize replies or TCP-only queries. Do not announce a local resolver whose fallback transport is missing.

**Acceptance:** normal A/AAAA replies; EDNS queries; messages of 511, 512, 513 and >1500 bytes; valid TC responses; real TCP retries; unreachable resolver; absent resolver; and DNS changes after reconnect. Check end-to-end name resolution from a phone behind both XIAOs.

### F06 — P0: radio API calls lack a shared owner

**Confirmed / upstream contract; hardware consequences unmeasured.** The fetched `framework/morselib/include/mmwlan.h:12` prohibits concurrent API calls except the documented TX APIs. This application calls connect/disconnect from `halow_reconnect`, scan from HTTP/console, and `mmwlan_get_rssi` from UI/history/console paths. `s_scan_lock` only serializes scans against other scans. It does not serialize them against reconnection or status reads. [Pinned Morse component](https://components.espressif.com/components/morsemicro/halow/versions/2.11.2-esp32-2/readme)

**Implement:** introduce one radio-control task and a bounded command queue for non-TX Morse calls after initialization. Serve status from cached snapshots rather than calling the driver from HTTP or timer callbacks. Keep vendor-permitted TX in its existing network path. Inventory every non-TX call, including diagnostics and VIF callbacks; do not assume a getter is exempt. Callbacks should copy bounded events and wake the owner, never wait on a lock held across an API that invokes callbacks.

**Acceptance:** repeatedly overlap user scans, RSSI sampling, association loss and reconnect. Instrument task identity at every non-exempt Morse call and assert single ownership. Driver callbacks must remain short and allocation-bounded. Test queue full/cancellation behavior and prove the console remains usable if the radio stops answering.

### F07 — P1: scan timeout does not synchronize callback lifetime

**Risk / interleaving.** `main/uplink_halow.c:610–697` uses a generation number to reject late callbacks, but callbacks read it without sharing a synchronization barrier with timeout cleanup. A callback can pass the generation check, then be preempted while the caller times out and returns. It can subsequently touch `sc->cb`/`sc->ctx` while HTTP serializes or frees the cJSON result, or while a new scan replaces the context. Checking generation once is not sufficient to protect an already-admitted callback. `scan_complete_cb` also ignores completion state. The callback API is shared with console use.

**Implement:** driver callbacks copy fixed-size scan records into storage owned by the radio module. HTTP/console copy a completed or frozen snapshot; callbacks never hold a pointer to HTTP JSON. Cap record count, deduplicate by BSSID as appropriate, count overflow and distinguish completed/aborted/timed-out/failed. Use an explicit synchronized publication step and generation ownership. Prefer an asynchronous scan job endpoint so HTTP does not block for the entire scan.

**Acceptance:** a controlled scheduler test pauses a callback immediately after generation validation, times out the caller, then resumes it. No freed or changed caller context is accessed. Test late completion during the next scan and AP-dense environments exceeding capacity. The UI must distinguish radio failure from “another scan is running.”

### F08 — P1: datapath lifecycle is one-shot and partially coupled

**Confirmed / source; extent of recovery failure needs device tests.** `main/app_main.c:199–257` enables services, treats DNS failure as nonfatal, and marks the entire datapath up when NAT and CoT succeed. `datapath_task` then deletes itself; `on_uplink_state` ignores down events. Startup failure retries depend on another uplink transition, so a downlink becoming ready after the wait expires may never retry. Both uplinks' `set_has_ip` suppress repeated true notifications, including a GOT_IP carrying a changed address. The Wi-Fi reconnect worker logs an immediate `esp_wifi_connect` failure without independently scheduling another attempt, and has no application DHCP deadline equivalent to HaLow's.

**Implement:** retain a small long-lived network supervisor. Model link, address and service states separately; events include interface identity, IP/mask/gateway/DNS snapshot and generation. Distinguish uplink-down, address-changed, downlink-ready, service-failed and retry-deadline. Retry failed services independently with capped jittered backoff. Coordinate membership refresh/socket rebind and NAPT state handling against verified IDF behavior; do not claim every address change necessarily breaks NAT, or promise preservation of old TCP flows. DNS failure must remain visible and retryable after NAT succeeds.

**Acceptance:** cold boot without upstream; upstream appearing later; delayed downlink; same-IP reconnect; changed-IP reconnect; DHCP server absent/restored; repeated synchronous connect errors; DNS socket allocation failure; and 100 outage/recovery cycles. Fresh traffic recovers without power cycling. Record whether old flows are reset, retained or timed out.

### F09 — P1: CoT relay admits unintended traffic and cannot bound distributed loops

**Confirmed / source for missing guards; distributed loop is topology-dependent.** `main/cot_relay.c:125–241` checks ingress interface and own source but does not compare `IP_PKTINFO.ipi_addr` with the configured group. The socket binds `INADDR_ANY` on the configured port. Thus a unicast datagram addressed to that port can be retransmitted as multicast. There is no duplicate cache, hop metadata, configured TTL or rate budget. Other gateways re-source forwarded packets, so an application reflection cycle need not return with this gateway's own source. A simple tree need not loop; redundant gateways/reflectors require explicit testing. Re-sending UDP also creates a fresh IP header, so ordinary IP TTL is not an end-to-end application hop budget.

**Implement:** validate destination group, ingress interface, ancillary-data length/truncation and configured service policy. Set an explicit multicast TTL appropriate to the adjacent domain; do not blindly increase it. Add a bounded time-based exact-payload fingerprint cache that suppresses recently forwarded copies without confusing every update for the same CoT UID. Define how identical legitimate retransmissions are handled. Include global and per-ingress packet/byte token buckets, bounded burst allowance and reason-specific drop counters. If true redundant relay topology is required, consider a separately specified envelope between controlled gateways with origin ID, sequence and hop budget, stripping it at ATAK edges. Do not change native CoT wire format silently.

**Acceptance:** unrelated unicast/wrong-group traffic produces no multicast output; one intended event reaches each receiver within the chosen duplication policy; two relays and a controlled reflector cannot sustain circulation after the source stops. Flood tests remain bounded while management and DNS are usable. Validate payloads around 1500 bytes and fragmented IP delivery; retain the existing oversize drop until a measured requirement justifies a bounded increase.

### F10 — P1: configuration parser silently changes security and numeric values

**Confirmed / host and source.** `main/provisioning.c:439` returns OPEN for any security string other than `owe`/`sae`. A typo such as `saee` can silently configure an open HaLow AP. `provisioning_validate` does not reject unknown enum values. `main/web_ui.c:724,748,752,756` narrows numbers before validating; channel 262 becomes 6. Fractions can also be accepted through `valueint`. IP validation mostly checks parsing, and admits zero/noncontiguous masks, unusable host addresses, overlaps and off-subnet gateways. A zero downlink mask is also dangerous to `netif_owns_peer`, whose mask comparison then matches all IPv4 peers if applied.

**Implement:** parsers return success/error separately from enum values; reject unknown roles/security modes. Validate JSON type, integer-ness and full numeric range before narrowing. Require bounded NUL termination of every persisted string before using `strlen`, formatting or copies; validate NVS blobs independently of HTTP bounds. Validate contiguous masks, usable unicast hosts, gateway relationships, DHCP range and disjoint active subnets. Check dynamic uplink overlap after leases arrive and keep forwarding disabled with an actionable error. Validate channel/class pairs against the vendor regulatory table. Coordinate the native 2.4 GHz country/channel policy with the fixed US product profile; do not assume the HaLow country setting configures the native Wi-Fi radio.

**Acceptance:** unknown strings, out-of-range enums, negative/huge/fractional values, embedded NULs, unterminated stored strings, zero/noncontiguous masks, network/broadcast addresses, self/remote gateways, subnet overlap and invalid channel pairs are rejected without modifying RAM/NVS. Explicitly chosen open mode, if retained for bench use, must require a deliberate choice and be clearly visible.

### F11 — P1: saved configuration and active network state are conflated

**Confirmed / source.** `config_post_handler` snapshots the whole config, unlocks during parsing, then overwrites the whole struct later (`main/web_ui.c:665–804`). A concurrent console edit/reset can be lost. In addition, `request_is_local` and `uplink_netif` consult mutable role/management flags immediately while actual radios continue using boot-time snapshots until reboot. The UI can describe saved values as though they were active. Auth commits have better persistence discipline, but unrelated writes should not carry an old credential snapshot.

**Implement:** keep `desired_config`, `active_config` and a monotonic revision. Parse a typed patch, then apply it to the latest desired config under one transaction, or reject a stale expected revision. Separate credentials from generic network configuration writes. Authorization should use an explicit active policy, with deliberate semantics for immediate management revocation. Report `reboot_required` and active vs pending values. Establish lock order across auth/config and protect shared cross-task state using atomics or short critical sections rather than relying on `volatile` or word-sized reads.

**Acceptance:** interleave console edits, HTTP save and password reset; unrelated settings and credentials are not resurrected/lost. Saving a role change cannot select the wrong live netif for management checks before reboot. NVS failures leave prior active and desired state coherent.

### F12 — P1: heap shedding does not enforce its stated admission policy

**Confirmed / source.** `main/heap_guard.c:30–66` pauses DHCP and logs that it is pausing new associations. Stopping DHCP does not prevent radio association or traffic from existing/static-IP clients; it may disrupt lease renewal. A five-second sample can miss a fast flood, and using identical enter/exit thresholds invites oscillation. CoT shedding occurs after packets already consumed network receive resources. DHCP changes also race DNS propagation's stop/start sequence.

**Implement:** describe DHCP pause honestly or remove it in favor of measured radio/client/traffic admission controls. Use separate enter/recover thresholds and recovery dwell time. Monitor largest internal free block as well as free/minimum heap. Move blocking DHCP/radio work out of the shared timer task and into the supervisor. Prioritize bounded service queues and per-client traffic limits before allocation pressure. Decide the actual service policy: limited CoT continuity, DNS and management availability should have explicit reservations, not an accidental priority inversion.

**Acceptance:** sustained traffic, brief bursts, repeated connects, static-IP clients and DHCP renewals under pressure. No repeated DHCP flapping; counters explain shedding; existing clients do not mysteriously lose leases. Both client and relay roles have pressure tests.

### F13 — P1: socket budgets conflict with maximum HTTP concurrency

**Confirmed / configuration; exhaustion impact needs target measurement.** Generated configuration has `CONFIG_LWIP_MAX_SOCKETS=10`. `HTTPD_DEFAULT_CONFIG` sets seven open clients and documents three sockets reserved for HTTP internal operation (`esp_http_server.h:62,196`). Relay mode adds one CoT and two DNS sockets. The configured maxima therefore cannot all be available simultaneously. HTTP's LRU policy is not a global resource reservation. Socket exhaustion can also cause the permanent partial startup state in F08.

**Implement:** write an explicit socket budget for both roles, including TLS, control sockets and any TCP DNS fallback. Reduce HTTP concurrency or increase the lwIP socket ceiling only with measured memory cost. Reserve datapath capacity before accepting management sessions. Put deadlines on sends/receives and preserve the ability to reject work cheaply.

**Acceptance:** maximum supported browser sessions plus DNS and CoT, stalled/slow HTTP clients and failed allocations. Data services must start/recover without reboot, and admission failure must be visible. Remember that forwarded NAPT flows are not each application sockets; size the socket and NAT tables separately.

### F14 — P1: physical compromise and credential-reset boundaries are undocumented

**Confirmed / configuration.** `sdkconfig` disables secure boot, flash encryption and NVS encryption. Config is stored as a raw NVS blob containing radio passphrases and the password-equivalent admin key. Flash ELF core dumps are enabled; even without whole-DRAM capture, task stacks can contain temporary credentials. `provisioning_load` falls back to defaults on version/validation failure, and `provisioning_init` erases NVS on specific errors. A routine schema bump can therefore reset ownership as well as networking.

**Implement:** define development and field provisioning profiles. Preserve administrative identity across supported network-config migrations; on unsupported/corrupt config, enter a closed recovery state requiring local owner action. Review encrypted NVS, flash encryption, signed firmware/secure boot, debug/download access and core-dump handling as one lifecycle. Keep diagnostic artifacts private and redact exports. Do not burn eFuses or disable recovery merely because this report recommends a field profile; key custody, replacement and recovery must first be tested on designated hardware. [ESP-IDF security overview](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/security/security.html)

**Acceptance:** upgrade/migration does not create an unowned device; power loss during save recovers coherently; recovery restores owner access without shared defaults. Verify where credentials persist, including Wi-Fi driver NVS and dumps. A production-profile flash read must not expose cleartext secrets. Signing proves authorized firmware; confidentiality and rollback protection are separate requirements.

### F15 — P2: authentication challenge state is globally replaceable

**Confirmed / source.** `main/auth.c` has one pending login challenge and one password salt for the whole device. A second client can replace the first client's challenge; a wrong/stale nonce clears the current challenge before returning. The global lockout protects guessing but also lets a reachable client impair everyone else's login. Session activity has only an idle deadline, while the cookie has a separate lifetime.

**Implement:** use a small bounded per-login transaction table with unpredictable IDs, expiry, admission limits and credential revision. Consume only the matching challenge. Bind password-change offers to an authenticated session or the explicit onboarding transaction. Keep global abuse protection, but separate malformed/stale traffic from genuine credential failures and rate-limit challenge issuance. Add absolute session expiration and invalidate pending challenges on password changes/reset. Recheck credential revision before issuing a session so a concurrent local reset cannot authorize an old verification result.

**Acceptance:** two browsers can log in without canceling each other; stale/wrong transaction messages do not destroy unrelated work; challenge floods are bounded; password changes revoke sessions and pending transactions. Verify policy with injected time rather than waiting thirty minutes.

### F16 — P2: vendor patch verification and builds need reproducible identities

**Confirmed / source.** `patch_vendored_halow.py:108` treats any occurrence of its marker as proof that an entire file is patched. A partially patched source can bypass validation. The patch adds an AP flag and explicit VIF selection, but `mmhalow_wifi_start` still discards the AP-enable return status. `main/idf_component.yml` uses a compatible-range dependency; the lockfile pins the currently resolved component. CI pins IDF 5.5.1 but action tags remain mutable. PlatformIO has its own version/integration path. No maintained automated firmware regression suite was found in the tracked file inventory.

**Implement:** verify every expected old/new hunk and dependency identity, reject mixed/unknown states, stage both files before replacement, and test idempotence. Maintain a small documented adapter/fork if ongoing fixes exceed a narrow patch. Expose AP start failure in an application-safe way and validate callback-driven readiness independently. Record compiler, IDF/component/BCF versions and hashes, config digest, patch digest, application revision and dirty state in build artifacts. Pin CI actions/container identities for release builds; give deployment permissions only to deployment jobs. Avoid directly interpolating untrusted ref names into shell scripts. Test the supported build frontend(s) explicitly rather than assuming they are equivalent.

**Acceptance:** clean dependency fetch, already-patched tree, partially patched file, changed upstream hunk and unsupported version all have deterministic outcomes. Verify both client and AP TX using real devices before removing the VIF workaround. Version alignment does not substitute for security-advisory review; no claim is made here that all vendored hostapd/WPA/lwIP code is free of known vulnerabilities.

### F17 — P2: diagnostics need precise semantics and a small boundary fix

**Confirmed / host/source.** `main/log_buffer.c:47–66` sets `s_wrapped` only when an append crosses the end. If appends land exactly at the end, `s_head` becomes zero while `s_wrapped` remains false, so a full ring appears empty. The host check reproduced this with 24 writes of 256 bytes. CoT counters count receive eligibility and socket-send success, not delivery at the peer. `cot_relay_inject` reports ESP_OK even when either send fails. AP mode readiness, relay socket existence and useful end-to-end service are different states. Historical documentation also reports suspicious HaLow RSSI of 0 dBm; do not present that as trustworthy range evidence.

**Implement:** use an explicit count for the log ring; test exact-boundary wrap. Make relay send results and reason-specific drops observable. Publish a synchronized network health snapshot with link/lease/DNS/CoT readiness, reconnect cause, resource pressure and active configuration generation. Mark questionable/stale radio measurements explicitly. Aggregate repetitive errors instead of logging every dropped packet, since the existing log hook still performs synchronous console output before its nonblocking ring write.

**Acceptance:** empty/partial/full/exact-wrap/multi-wrap rings; failed sends; counter monotonicity; disabled services; stale RSSI; and boot without radio. No status value should imply end-to-end delivery solely because `sendto` succeeded.

## 5. Security model to implement

The practical threat model includes an outsider within Wi-Fi/HaLow range, a peer with shared network credentials but no admin rights, an upstream/mesh peer, a browser visiting an unrelated website, malformed/flooding clients, and physical access to a lost node. Separate these capabilities; a NAT boundary is not proof that all of them are excluded.

| Boundary | Required policy | Implementation direction |
|---|---|---|
| Owner setup | Physical possession + unique secret | Bounded local onboarding; no universal network key |
| Management | Authenticated admin on explicitly allowed interface/path | HTTPS identity, session controls, Host/origin checks, default upstream-management denial |
| Forwarded unicast | Defined client-to-mesh/Internet service access | Explicit forwarding policy; verify ingress semantics and return traffic in pinned lwIP |
| DNS | Authorized client requests to trusted configured resolver | Direct resolver where practical, otherwise validated bounded proxy |
| CoT | Explicit group/port, bounded relay trust | Destination filtering, duplicate control, rate budgets; endpoint security for identity/confidentiality |
| Stored secrets | Confidential owner credentials and radio keys | Field provisioning profile, encrypted storage, controlled dumps/recovery |
| Firmware | Only intended signed releases on field nodes | Reproducible release identity, signing and tested recovery before OTA |

`reject_if_remote` is useful, but it compares the peer's IP subnet; it is not proof of packet ingress interface. Keep it, reject overlapping/zero masks, and enforce interface-level policy through supported network-layer facilities. An IP allowlist alone cannot authenticate a station. In relay mode a leaf may NAT several phones behind one source address, limiting per-phone enforcement there; apply per-phone controls on the leaf and per-leaf controls on the relay.

Do not silently expose IPv6 as if it has the IPv4 NAT/security policy. Explicitly declare this release's supported forwarding families and test IPv6 local service exposure, neighbor/RA behavior and management binding. Do not add broad mDNS/SSDP relaying as a convenience feature before evaluating multicast load and service exposure.

## 6. Optimization plan: measure the access path

The largest immediate optimization is eliminating avoidable retries, malformed forwarding, duplicate traffic and unnecessary driver calls. Existing `-Os`, gzip UI embedding, PSRAM use and bounded service structures are sensible. Do not reduce task stacks because a build fits; worst-case runtime headroom is the evidence.

### Recommended measurement matrix

Use controlled generated traffic and at least one real ATAK workflow. Capture at the phone side, HaLow hop where feasible, and Pi client/backbone boundary. Preserve anonymized evidence with build identity; keep raw location-bearing CoT private.

| Scenario | Measurements | Decision it supports |
|---|---|---|
| Single leaf, clean close-range link | TCP goodput both directions; UDP delivery/loss/jitter; RTT; heap/stack; CPU where supported | Baseline and bottleneck location |
| 2 and 4 leaves, then higher counts only if healthy | Aggregate/per-leaf throughput, fairness, DNS latency, UI latency, retry/drop counters | Supported station limit and traffic budgets |
| CoT plus concurrent bulk transfer | CoT end-to-end delay/loss/duplicates and browser responsiveness | Whether application budgets preserve useful operations |
| 1 MHz vs 2 MHz with fixed placement | Same traffic, RF settings, supply, antennas and firmware | Range/airtime/latency tradeoff; no inferred universal winner |
| Link outage and renewed address | Recovery time; failed/stale flows; DNS behavior; memberships | Supervisor correctness |
| NAT flow churn | Active mapping pressure, new-flow failure, memory, latency | NAT table sizing/timeouts |
| USB power vs proposed portable supply | Average and peak input power; rail droop; temperature; actual delivery | Portable power design and runtime |
| Maximum management load | TLS handshake peaks, sockets, largest internal free block, response deadlines | Safe HTTP/TLS connection ceiling |

### Specific optimization candidates

1. **Cache status in one supervisor/radio snapshot.** It removes concurrent driver calls and makes UI polling cheaper. Pause/reduce polling when the browser is hidden, and keep log/history transfers demand-driven.
2. **Make scans asynchronous and bounded.** The current HTTP scan waits up to its full scan deadline on the server task; this serializes unrelated management work. A job ID/status result avoids paying another large task stack per request.
3. **Profile the actual bottleneck before tuning SPI.** The vendor shim currently requests 40 MHz SPI (`components/shims/mmhal_wlan.c:100`). Faster SPI is not automatically useful on a narrow RF channel; higher speed also needs signal-integrity evidence.
4. **Benchmark native Wi-Fi power-save deliberately.** `uplink_wifi.c` has no explicit `esp_wifi_set_ps` policy. Measure latency and watts with documented IDF modes and fix the chosen policy explicitly. Keep HaLow power-save disabled on the current DNP WAKE/BUSY board until the hardware path is changed and proven.
5. **Budget NAPT independently of TCP endpoint buffers.** Pinned lwIP defaults include `IP_NAPT_MAX=512` and UDP timeout 2000 ms (`lwip/lwip_napt.h:54–70`). Validate what this build actually compiles and how the table ages. Long-lived UDP applications may need reconnect/keepalive policy or measured timeout changes. Do not blindly enlarge the table. Increasing the ESP's own TCP receive window does not enlarge a phone's end-to-end TCP window for traffic merely forwarded by NAT.
6. **Use traffic evidence for station limits.** The vendor AP has default four and API maximum twenty stations; those are not proof that twenty leaves, each with several phones, meet application latency. Start acceptance with one, two and four leaves.
7. **Defer custom DHCP implementation.** The current STA-shaped HaLow netif lacks the built-in server allocation. This is an SDK integration limitation, not proof that the only scalable solution is a new DHCP server. Evaluate an upstream-supported AP netif adapter/fork that creates the right flags and uses IDF's existing DHCP server. A new DHCP stack adds lease, broadcast, restart and malformed-input obligations to a project whose simpler DNS proxy already needs repair. Keep validated static allocation as the initial bounded fallback.
8. **Remove double NAT only when it solves a measured need.** For normal outbound access, correctness and bounded state are the first concerns. Routed mode should be a separate reviewed feature with address allocation and return-route tests, not an optimization switch without network coordination.

## 7. Hardware and cost assessment

The inexpensive concept is credible because the ESP32 supplies the local 2.4 GHz radio and MCU, and the separate module supplies HaLow. Retain the current S3 baseline while software matures. A cheaper/newer MCU is not a saving if it requires redoing driver, board and RF qualification.

Seeed documents this XIAO module as SPI-attached and requires both 3.3 V digital and 5 V RF-front-end supplies. A module may receive/load firmware with inadequate RF supply yet fail to transmit at rated performance. Verify the actual board revision and rails rather than using successful SPI startup as power validation. [Seeed hardware guide](https://wiki.seeedstudio.com/getting_started_with_wifi_halow_module_for_xiao/)

The repository's board analysis identifies unpopulated WAKE/BUSY links and a specific FGH100M-H BCF. Preserve the fixed board profile and US-only design until a separately verified hardware profile exists. Do not substitute generic advertised maximum PHY rates, range or power figures for this exact assembly's throughput/range evidence. Regulatory approval of a final custom product is not established by selecting a country string or module calibration file.

Add a priced deployment BOM at the time of purchase, with actual quotes for MCU, HaLow board, both antennas, pigtails/connectors, power conversion/battery, enclosure, cable and assembly. This review did not fetch purchase quotations and makes no current dollar-cost claim. Count the relay and OpenMANET node as shared infrastructure: a deployment with N leaves and R relay units needs N+R XIAO/radio pairs plus its backbone. Compare total cost per **working supported endpoint**, including debug/provisioning effort.

For portable power, measure watts through the entire intended conversion path. Estimate runtime as usable battery watt-hours divided by measured average input watts, adjusted for conversion loss and low-voltage cutoff. Verify transmit bursts, cable loss, safe charging and thermal behavior separately. A battery attached to the XIAO is not proof that the HAT receives the required RF rail. The antenna/enclosure/placement and supply system may offer more practical reliability per dollar than CPU optimization.

## 8. Implementation sequence for the coding agent

Use small reviewable changes. Each stage must state what is now proven and what remains a hardware gate. Do not tick a roadmap feature merely because it compiles.

| Stage | Findings / deliverable | Dependencies | Exit gate |
|---|---|---|---|
| A: immediate correctness | F01, F04/F05, F10, F17 boundary bug; safe auth-init failure | None | Host regressions + IDF build; boot/UI without upstream on hardware |
| B: ownership and recovery | F06/F07 radio owner; F08 supervisor; F11 active/desired config; F13 resource budgets | A | Controlled event/interleaving tests and 100 recovery cycles |
| C: management security | F02/F03/F15; migration portion of F14 | A; B snapshots/budgets | Protected owner bootstrap, trusted HTTPS, auth negative tests, memory measurements |
| D: multi-node forwarding | F09, F12, ingress/service policy and observability | A/B | Multi-leaf and reflector/flood tests; bounded loss/latency under declared load |
| E: deployment hardening | F14 production profile, F16 reproducible build/patch/advisory process | B/C/D | Signed release identity and tested recovery on designated hardware |
| F: measured optimization | Section 6 experiments; optional supported AP-netif/DHCP adapter | A–D | Recorded improvement without regression in security/recovery |

Suggested internal interfaces, to refine against pinned APIs:

- `radio_control`: bounded requests/results; owns non-TX Morse calls and scan storage.
- `network_supervisor`: durable service state, events/deadlines, active address/config generation.
- `network_snapshot`: initialization-safe immutable copy for UI, console and diagnostics.
- `config_store`: typed patches, revision checking, migration and separate credential handling.
- `cot_policy`: pure destination/admission/dedup decisions plus bounded socket worker.
- `dns_transactions`: pure validated mapping logic if a proxy remains necessary.

Do not combine socket lifecycle locks with counters or config locks. Define task ownership and lock order in headers. Use failure injection to cover allocation and persistence errors; for example `auth_init` currently logs mutex allocation failure and continues, while later auth methods assume a valid mutex. Management must fail closed and return a controlled unavailable state if initialization fails.

### Durable regression suite to add

| Test ID | Required regression |
|---|---|
| T01 | Pre-start/failed-start/stop status getters; null-handle and concurrent snapshot safety |
| T02 | DNS same-ID collisions, reply reordering, resolver tuple/question matching, expiry and table pressure |
| T03 | DNS oversize/EDNS/TC/TCP behavior and resolver changes |
| T04 | Config enum/type/range/narrowing checks, bounded stored strings, masks/gateways/overlaps and regulatory pairs |
| T05 | Scan callback paused across timeout/new-generation; bounded AP record overflow |
| T06 | Serialized radio API calls under scan/status/reconnect overlap |
| T07 | Datapath event permutations, changed leases, service partial failure, backoff and restart |
| T08 | HTTPS/bootstrap/session/challenge concurrency, revision changes and failed persistence |
| T09 | CoT wrong destination, self/duplicate handling, distributed reflection, quotas, truncation and send failure |
| T10 | Heap/socket pressure with both roles and slow HTTP/TLS clients |
| T11 | Exact-boundary log ring and snapshot/drop-counter semantics |
| T12 | Upgrade/migration/factory recovery, patch partial states and build identity |

Use host tests for pure parsing/state/policy functions, stubbed scheduling for ownership/lifecycle, and actual IDF builds for API compatibility. Sanitizers and fuzzing should focus on config/DNS/CoT boundary parsing and scan lifetime, with seed inputs based on real valid packets. Hardware tests establish radio behavior, DHCP/NAT forwarding and field recovery; host tests cannot replace them.

### Proposed initial acceptance targets

These are suggested release targets, **not measurements or product claims**. Adjust once a baseline is recorded, then keep the chosen thresholds explicit:

- 24-hour soak per role with zero unexpected resets and no continuing internal-heap decline after warm-up.
- 100 upstream interruption/recovery cycles with fresh DNS/unicast/CoT traffic recovering without manual reset.
- At one, two and four leaves, a declared offered load and payload mix with reported per-leaf loss, duplicate rate and p95/p99 delay; no unspecified “supports twenty clients” claim.
- Ordinary management responses under one second in the controlled unloaded bench case; scans asynchronous; overloaded service rejects boundedly instead of hanging.
- Under deliberate multicast/reflection input, traffic stops within the documented dedup/hop-policy bound after the source stops, and management remains reachable.
- Invalid credentials/configuration/packets do not change persisted state, leak secrets or force a reboot.

## 9. Documentation reconciliation

Add a short link to this review from `ROADMAP.md`; retain old measurements as historical records. In the implementation PRs, update the authoritative sections instead of leaving contradictory claims beside new code:

- Identify the product as an access gateway; describe the current relay topology and fixed-uplink limitation.
- Correct “static route alone fixes inbound access” and document what each ATAK workflow requires.
- Replace the DNS automatic-TC/fallback claim and initialization-only evidence with real end-to-end results.
- Replace “DHCP pause blocks associations” with the actual implemented policy.
- Separate initialized/socket-created/associated/addressed from end-to-end service health.
- Replace universal statements that API defaults imply security/compliance with explicit tested properties.
- Reconsider the planned from-scratch DHCP server after evaluating a supported AP-netif integration.
- Reconcile build versions and document production identity/migrations before automatic updates.

This report is intentionally an additional design document because the user explicitly requested an in-depth implementation handoff here; the older `CLAUDE.md` convention limiting `design/` to three files should not prevent retaining the requested review.

## 10. Source references and reproducibility notes

Local file/line references identify the baseline revision above; use function names after edits shift lines. External pages were consulted on 2026-09-10. Current upstream docs do not establish installed-device configuration.

- [OpenMANET networking](https://openmanet.github.io/docs/networking): backbone/access separation and currently documented topology. Its examples should be checked against actual Pi UCI/interface state, not copied as a universal address plan.
- [Seeed XIAO HaLow guide](https://wiki.seeedstudio.com/getting_started_with_wifi_halow_module_for_xiao/): physical interface and supply guidance. Its older tutorial IDF version is not the build dependency authority for this repository.
- [Morse component 2.11.2-esp32-2](https://components.espressif.com/components/morsemicro/halow/versions/2.11.2-esp32-2/readme): pinned vendor integration; inspect its actual `mmhalow.c`, `mmwlan.h` and board profile before edits.
- [ESP-IDF v5.5.1 FreeRTOS queue implementation](https://github.com/espressif/esp-idf/blob/v5.5.1/components/freertos/FreeRTOS-Kernel/queue.c): non-NULL semaphore assertion.
- [ESP-IDF v5.5.1 HTTP server header](https://github.com/espressif/esp-idf/blob/v5.5.1/components/esp_http_server/include/esp_http_server.h): HTTP defaults and socket reservation.
- [ESP-IDF v5.5.1 HTTPS server documentation source](https://github.com/espressif/esp-idf/blob/v5.5.1/docs/en/api-reference/protocols/esp_https_server.rst): maintained transport implementation to evaluate.
- [ESP-IDF v5.5.1 security overview](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/security/security.html): distinct firmware authenticity, storage protection and provisioning controls.
- [RFC 5452](https://www.rfc-editor.org/rfc/rfc5452.html): DNS query/reply matching and forgery resistance.
- [ESP-IDF v5.5.1 lwIP submodule](https://github.com/espressif/esp-idf/tree/v5.5.1/components/lwip/lwip): follow the pinned submodule for exact `src/api/sockets.c`, `src/include/lwip/lwip_napt.h` and `src/core/ipv4/ip4_napt.c` semantics. These were checked locally in the v5.5.1 tree, rather than inferred from POSIX or another lwIP version.

Before changing the Pi, gather its actual firmware/build identity, interface membership, route table, firewall zones, DHCP/DNS configuration, multicast behavior and radio/VIF capabilities. Redact credentials from any collected config. Before signing off fixes, attach command exit codes, test names, exact firmware identities and anonymized packet/result summaries. A successful build remains compilation evidence; a functioning gateway requires the corresponding device/network evidence.
