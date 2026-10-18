# Roadmap

Where this project is, what's next, and the decisions that shouldn't be re-made. **Start here if
you're picking the project back up.**

- **2026-09-10 review:** [`PROJECT_REVIEW_2026-09-10.md`](PROJECT_REVIEW_2026-09-10.md)
  contains the code, routing, security and optimization audit, prioritized implementation stages,
  and acceptance tests. Recommendations are pending implementation; its new build/host checks
  do not replace the hardware validation gates below.

- Companion docs: [`HARDWARE.md`](HARDWARE.md) (what to buy, how to build one, how to bring it up),
  [`PI_SIDE.md`](PI_SIDE.md) (the other end of the link)
- Architecture diagram and repo layout: [`../README.md`](../README.md)
- **Last updated:** 2026-09-11 (2026-09-10 review tracker: Stages A-D complete, Stage E mostly so -
  every finding scoped to its concrete, demonstrable bug and verified on real hardware or (F16)
  locally, see the "2026-09-10 review tracker" section below for the full per-finding story. F16
  grew into three follow-up passes this session: CI hardening (every GitHub Action SHA-pinned, an
  injection-prone ref interpolation fixed, deploy permissions scoped, build provenance recorded),
  time-based pinning (`.github/dependabot.yml`, 14-day cooldown), and a host-side automated
  regression suite (`main/net_validate.c` + `main/host_tests/`, wired into a new
  `host-tests.yml` CI workflow) - the review's "no maintained regression suite" finding. A CI
  self-inflicted parse failure from that pass (a literal empty `${{ }}` inside an explanatory
  comment - GitHub's Actions parser evaluates `run:` block text including comments, before the
  shell ever sees it) was caught and fixed the same day, `actionlint`-verified. Stage E's secure
  boot/flash encryption (F14 production profile) is a deliberate no, not built - see that item's
  own entry for the cost/benefit reasoning. Stage F's status-cache and async-scan items are both
  done (`link_history.c` now caches both uplinks' RSSI from its existing periodic sample;
  `uplink_halow_scan()` is now a thin blocking wrapper over a non-blocking start/poll/cache-read
  primitive, so `web_ui.c`'s scan endpoint no longer holds the single-threaded httpd task hostage
  for a scan's full duration), both hardware-verified. The one still-open P0 is a power-delivery
  regression tracked under F06, seen on both bench boards - a powered hub is the next planned test)

Keep this file current: tick the checklist when a step passes, move an item out of "not built yet"
when it lands, and add to "settled decisions" rather than re-arguing one. Historical detail
belongs in git history, not here — this file describes the present and the plan.

## Status at a glance

**Target hardware: Seeed XIAO ESP32-S3 + Seeed XIAO WM6108, whose radio module is a Quectel
FGH100M-H — 902–928 MHz, US only.** One build, `CONFIG_HALOW_COUNTRY_CODE="US"`; see "Settled
decisions" and [`HARDWARE.md`](HARDWARE.md) "Regulatory domain".

`idf.py build` **passes end-to-end** against ESP-IDF v5.5.1 with the real `morsemicro/halow`
component: **zero errors, zero warnings**, binary **~1.84 MB (`0x1d7bd0`)**, **39% free** in the
3 MB app slot on confirmed 8 MB flash. Verified by actually running the build, not by reading code.
(The 2% drop since the last figure below is `esp_https_server` + mbedtls's X.509/PK write code for
the 2026-09-10 HTTPS switch - see "Settled decisions → Authentication → TLS" - ~71 KB, still noise
next to the margin this slot has.)

That is 182,464 bytes (178 KB, 9.1%) smaller than the ~1.92 MB / 36% this sat at through the
GW_ROLE_RELAY work, from two changes measured together on one build: `-Os` instead of ESP-IDF's
default `-Og` (~145 KB, and it applies to esp-halow's vendored hostapd/wpa_supplicant fork as much
as to this project's own code) and gzipping the embedded web UI (33,694 bytes). The slot now has
more headroom than it did before AP mode landed.

**First hardware bring-up is under way.** Steps 1 and 4 have passed on a real XIAO ESP32-S3 +
WM6108: the MM6108 answers over SPI with its version banner, and the SoftAP leases addresses and
serves the web UI. A compiling build proves the code is internally consistent against the real
APIs; it does not prove the radio associates, DHCP completes, or NAT and the CoT relay pass
traffic. Everything remaining in the checklist below is hardware-only from here.

**Stability confirmed under real, concurrent load.** Running off a PC's USB cable (no Pi present)
with a phone associated to the SoftAP and the HaLow radio initialized and periodically scanning,
the node stays up indefinitely — no reboot loop, no crash. This was the failure mode the
`CONFIG_HALOW_PS_MODE` fix (see "Settled decisions") targeted, and it holds with the SoftAP,
DHCP server, web UI and HaLow radio all running at once, not just at idle. **Next phase is
bringing up the Pi and testing the HaLow uplink against it — checklist steps 0 through 3.**

The first thing hardware taught us wasn't in the firmware at all: the HaLow HAT's unpopulated
WAKE/BUSY links make the SDK's *default* power-save setting reboot the node in a loop, which
presents identically to a dead radio. See `CONFIG_HALOW_PS_MODE` under "Settled decisions".

**It also doesn't prove the code means what it says.** Three separate passes have now found bugs in
clean-compiling firmware that would each have failed silently on hardware and looked like a radio
problem:

- the CoT relay matched `IP_PKTINFO`'s `ipi_addr` (the packet's *destination*, always the multicast
  group) instead of `ipi_ifindex`, dropping 100% of traffic while logging success;
- NAPT was enabled on the uplink netif when ESP-IDF requires it on the SoftAP netif;
- SoftAP clients were handed no DNS server, so every hostname lookup failed while raw IP worked;
- the CoT relay passed `recvmsg()`'s return value straight to its forwarding `sendto()`. On a UDP
  socket lwIP returns the *datagram's* length there, not the bytes copied into the iovec
  (`if (datagram_len > buflen) msg_flags |= MSG_TRUNC; ... return (int)datagram_len` — esp-lwip
  `2.2.0-esp`, `src/api/sockets.c` L1411-1417), so any CoT event over 1500 bytes made the relay
  read past its `.bss` receive buffer and transmit whatever followed it onto the mesh. Now dropped
  on `MSG_TRUNC`, with a log line;
- `use_static_ip` was fully plumbed — console command, web UI field, validation, `gwcfg-status`,
  `/api/config` — and **never read by `uplink_halow.c`**. Since a relay's HaLow AP deliberately
  runs no DHCP server, a leaf configured exactly as documented could only associate, wait out two
  30-second lease timeouts, disconnect and loop forever. It now applies the address at bring-up,
  and `esp_netif_action_connected()` raises `IP_EVENT_STA_GOT_IP` for it on every link-up.

All five were found by checking this firmware's assumptions against upstream ESP-IDF/lwIP source,
not by re-reading this repo. Assume the same class of error exists elsewhere. See
[`../CLAUDE.md`](../CLAUDE.md) for the working rule this produced. Note what the last two have in
common with the first three: every one of them compiles clean, and every one presents on hardware
as a radio or link problem.

**A fourth was found only by running it** (2026-08-17, item 8): the datapath bring-up was correct
against every API it called, and still crashed — because of *where* it ran, not what it did. Called
from an uplink state callback, it executed on the `sys_evt` event-loop task and overflowed that
task's 2816-byte stack, panicking the node into a reboot loop. Reading the call site tells you
nothing here; the bug lives in the execution context the call site inherits. When adding work to any
callback, check which task will run it and what stack that task has.

## What's implemented

| Module | File | Status |
|---|---|---|
| Local SoftAP + DHCP | `main/downlink_softap.c` | 2.4 GHz AP + DHCP server for phones/tablets/ATAK devices. GW_ROLE_CLIENT only. |
| HaLow STA uplink | `main/uplink_halow.c` | STA association, reconnect/backoff, bounded DHCP wait with disconnect-and-retry. Exposes a four-state link state, RSSI, a blocking scan wrapper, and radio version readback. GW_ROLE_CLIENT only. |
| Wi-Fi STA uplink | `main/uplink_wifi.c` | GW_ROLE_RELAY only - native `esp_wifi` STA joining the Pi's own local AP directly (item 8 below). Same link-state/RSSI/callback shape as `uplink_halow.c`, event-driven reconnect against standard ESP-IDF STA events rather than a blocking task. |
| HaLow AP downlink | `main/downlink_halow_ap.c` | GW_ROLE_RELAY only - HaLow radio in AP mode (`CONFIG_HALOW_AP_MODE`) so other XIAOs can associate to this node instead of a Pi (item 8 below). Static IP, no DHCP server - see the file's own header comment for why. Also exposes the regulatory channel table so an operator can pick a legal (op_class, s1g_chan_num) pair. **Confirmed on real hardware as of 2026-08-30**: starts cleanly, a leaf XIAO associates over HaLow, the downlink netif reports up (fixed - see "What the Aug 30 datapath-race run proved" below), and NAT + the CoT relay both come up behind it. `mmhalow_wifi_start()` itself still returns no code to confirm the AP came up (hence `gwcfg-status`'s "best-effort" wording for that one specific claim), but everything downstream of it now has independent confirmation. `mmwlan_tx_pkt` used to intermittently log "Unable to infer VIF ID" for outbound frames toward a leaf, confirmed 2026-08-30 to actually block real CoT delivery *and* all NAT'd internet reply traffic through the mesh, not just IGMP housekeeping - see "Third" and "Fourth Aug 30 finding" below. **Worked around locally as of 2026-08-30** (`patch_vendored_halow.py`, applied automatically at CMake configure time) - verified 0/6 -> 6/6 packets forwarded across the fix. Morse's own AP-mode API is still marked alpha, and this remains a bug filed with them, not a real upstream fix. |
| NAT / IP forwarding | `main/ip_forward_nat.c` | All three steps of ESP-IDF's NAT recipe: DNS propagation into the SoftAP's DHCP offers, uplink as default route, NAPT on the downlink. |
| CoT multicast relay | `main/cot_relay.c` | One socket joined to 239.2.3.1:6969 on both netifs, `IP_PKTINFO`/`recvmsg()` for arrival interface, loop prevention via `IP_MULTICAST_LOOP` off + own-source drop. Tracks per-side rx/tx packet and byte counters (`cot_relay_get_counters()`), surfaced in `/api/status`'s `cot.uplink_side`/`cot.downlink_side` - groundwork for a real throughput number once traffic is flowing. Deliberately not a generic per-netif counter - see `cot_relay_counters_t`'s doc comment for why lwIP's MIB2 stats aren't reachable from application code here. Counters also printed by `gwcfg-status` now, and `cot_relay_inject()` gets its first real caller via the `gwcfg-cot-test <count> <interval_ms>` bench command - see the Seventh Aug 30 finding below for what it's for. |
| Uplink RSSI history | `main/link_history.c` | Samples the active uplink's RSSI every 30s into a 240-sample (2h) ring, served at `/api/rssi-history` and drawn as a sparkline in the web UI's uplink card. Role-agnostic - tries both `uplink_halow_get_rssi()` and `uplink_wifi_get_rssi()` each tick and keeps whichever isn't reporting its idle sentinel. **Confirmed on real hardware 2026-08-30, with a caveat**: on a GW_ROLE_CLIENT leaf associated to a relay's HaLow AP, `uplink_halow_get_rssi()` reads a flat 0 dBm rather than a real value - see "Second Aug 30 finding" below the datapath-race section. The relay's own Wi-Fi-uplink RSSI (`uplink_wifi_get_rssi()`) reads correctly. |
| Web UI authentication | `main/auth.c` / `.h` | Challenge-response login, RAM-only sessions, lockout/backoff, and the first-use/change password flow behind six new endpoints in `main/web_ui.c` - see item 1 under "Not built yet" for the full design and what's still pending hardware verification. |
| DNS forwarding for HaLow leaves | `main/dns_forward.c` | GW_ROLE_RELAY only - a leaf's statically-addressed uplink has no DHCP lease to learn a DNS server from at all (`gw_uplink_config_t.static_dns`'s own comment), so this listens on the relay's own downlink (HaLow AP) address, port 53, and forwards queries out through the relay's own uplink using whatever real DNS server *that* hop actually has (read fresh via `esp_netif_get_dns_info()` per query, not cached - tracks a lease renewal automatically). Single task, `select()` over a listen socket and an upstream socket, an 8-slot pending-query table keyed by DNS transaction ID. A leaf points at it via `gwcfg-set-uplink-static-ip <ip> <gateway> <netmask> <relay's-own-downlink-ip>`. **Confirmed on real hardware 2026-08-30**: forwarder starts and binds correctly (`DNS forwarder listening on 172.16.60.1:53`); the actual query/response round-trip needs a device on a leaf's own SoftAP to test, which this project's own dev machine has no network path to - see the Fifth Aug 30 finding below for why this exists at all. |
| Provisioning | `main/provisioning.c` | NVS config blob (magic + version stamped, validated on load and save) plus `gwcfg-*` console commands over USB Serial/JTAG. `gwcfg-set-role` selects GW_ROLE_CLIENT/GW_ROLE_RELAY at runtime - one firmware image, no separate relay build. |
| Web config UI | `main/web_ui.c` / `.html` | `esp_http_server` + embedded HTML. `GET /api/status`, `GET`/`POST /api/config`, `GET /api/log`, `GET /api/tasks`, `POST /api/scan`, `POST /api/reboot`. Same NVS config as the console. Downlink clients only by default (SoftAP for GW_ROLE_CLIENT, HaLow AP for GW_ROLE_RELAY); **no authentication yet**. `allow_uplink_management` (off by default, `gwcfg-set-uplink-mgmt on\|off` or the web UI) opts a node into also accepting requests addressed to its own uplink IP - confirmed on real hardware 2026-08-30 (a relay reachable from its Wi-Fi uplink's subnet once enabled; a client on an unrelated third network that only routes there was correctly still refused - that's peer-subnet matching working as scoped, not a bug). |
| Status LED | `main/status_led.c` | On-board GPIO21 LED blinks the uplink link state. The only instrument needing neither cable nor phone. |
| Factory reset | `main/factory_reset.c` | 5 s BOOT-button hold restores defaults and reboots; LED acknowledges at 1.5 s. |
| Stack headroom | `main/task_stats.c` | Worst-case free stack per task via `uxTaskGetStackHighWaterMark()`, surfaced as `gwcfg-tasks` and `GET /api/tasks`. Turns "is this close to overflowing?" into a number - see "Stack budgets" below. |
| Log ring buffer | `main/log_buffer.c` | `esp_log_set_vprintf` tee into a 6 KB RAM ring, served at `/api/log`. Chains to the previous handler, so serial output is unaffected. |
| Chip temperature | `main/chip_temp.c` | ESP32-S3 internal die temp via `esp_driver_tsens`, installed once at boot (20-100°C range) and read on each `/api/status` request; surfaced in the web UI's Hardware & Diagnostics card, colored as a warning at ≥80°C. **The HaLow module has no equivalent** — the vendored Morse Micro SDK (`mmwlan.h`/`mmhal_wlan.h`/`mmhal_app.h`/`mmosal.h`) exposes no thermal API for the MM6108/FGH100M-H at all, so its temperature isn't software-readable without external sensor hardware. **`temperature_sensor_install()` confirmed on real hardware 2026-08-30** (`Range [20C ~ 100C], error < 2` logged on boot) on both the relay and the leaf; `/api/status`'s `chip_temp_c` itself not yet checked over the network. |
| Memory headroom: PSRAM + heap guard | `sdkconfig.defaults`, `main/heap_guard.c` / `.h` | Added 2026-08-30 after tracing lwIP's `PBUF_POOL` type: under ESP-IDF's `MEMP_MEM_MALLOC=1`/`MEM_LIBC_MALLOC=1` (`components/lwip/port/include/lwipopts.h`), it's not a fixed-size pool at all - every pbuf is a plain heap `malloc()`, shared unmodified across every netif and consumer (SoftAP, HaLow, the CoT relay, the web UI/auth's TCP). On this board that heap was internal SRAM alone (~512 KB, no PSRAM) with `CONFIG_LWIP_STATS` off, so a burst on any one interface could starve every other one silently. Two changes: (1) `sdkconfig.defaults` now enables the on-module 8 MB octal PSRAM (`CONFIG_SPIRAM`) with `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, so lwIP/WiFi allocations that don't fit in internal SRAM spill into PSRAM instead of failing - small allocations (under `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`'s 16 KB default, which covers every pbuf this firmware ever allocates) still prefer fast internal SRAM first. (2) `heap_guard.c` samples combined free heap every 5s and, below `GW_HEAP_COT_SHED_BYTES`, has `cot_relay.c`'s relay task drop-and-count datagrams instead of forwarding them (CoT is UDP/best-effort and the highest-volume consumer; the web UI's TCP/auth traffic is low-volume and human-facing, so it's protected first); below the lower `GW_HEAP_NODE_SHED_BYTES` it also pauses the SoftAP's DHCP server so no new phone associates onto an already-degraded node. Both thresholds and both heap figures (combined + internal-only) are surfaced in `/api/status`. **Confirmed on real hardware 2026-08-30**, both the relay and the leaf: boots cleanly, all stored config (role, uplink SSID, HaLow AP SSID/channel) survives the flash unchanged, both radios stay up (Wi-Fi uplink, HaLow AP with its leaf still associated). Combined free heap jumped from ~135KB/~157KB (pre-PSRAM baseline, measured on the previously-flashed build) to **~8.39MB/~8.41MB** on both nodes - PSRAM is live. Free *internal* SRAM alone reads ~109KB (relay) / ~130KB (leaf), close to each node's pre-flash combined figure as expected, confirming `GW_HEAP_COT_SHED_BYTES`/`GW_HEAP_NODE_SHED_BYTES` (64KB/32KB) sit at a plausible fraction of real headroom rather than an arbitrary guess - though still not yet exercised under an actual traffic burst. |
| App wiring | `main/app_main.c` | Brings up log buffer, LED, factory-reset watcher, console and web UI immediately; then one of two role-specific bring-up paths (`bring_up_client_role()` / `bring_up_relay_role()`). NAT + CoT relay come up via a shared helper once whichever uplink holds a usable IP, retrying on the next reconnect if that fails. |
| Web flasher + CI | `docs/`, `.github/workflows/` | ESP Web Tools page, single US build. GitHub Actions builds `sdkconfig.defaults` unmodified and deploys to Pages; PRs build but don't deploy. |

Everything above compiles clean. **Steps 1 and 4 have now passed on hardware** (see the checklist);
the rest is unrun.

## Build-order checklist

Procedure for each step — what to run, what a pass looks like, how to tell identical-looking
failures apart — is in [`HARDWARE.md`](HARDWARE.md) Part 2. This is the tracker; that is the
runbook. **The step numbers are shared** — if you renumber one, renumber the other, or a recorded
"step 5 passed" stops meaning one thing.

**Steps 1 and 4 passed at `-Og`, not at `-Os`.** The size pass (2026-08-21) put every component
through different codegen, the morselib SPI shims that step 1 exercises included. Re-run both on
the first `-Os` flash before reading any new failure as a regression somewhere else — they are the
two cheapest steps here and they re-establish the baseline the rest is measured against. Tick them
again in place; there is no separate step for this.

- [ ] **Step 0** — Confirm the Pi's HaLow radio config: AP mode, SSID, security mode, country,
      DHCP behaviour. See [`PI_SIDE.md`](PI_SIDE.md) "Still to verify". The Pi has to be on **US**
      (902–928 MHz) — that's the only domain this hardware can reach.
- [x] **Step 1** — Radio responds over SPI (`gwcfg-radio`). **Passed.** On a Seeed XIAO ESP32-S3 +
      WM6108 (Quectel FGH100M-H), the boot banner reports BCF API 8.0.0, morselib 2.11.2, Morse
      firmware 1.17.8 and chip ID `0x0306`, so the `CONFIG_MM_*` pin map, the BCF file and the chip
      selection are all confirmed against physical hardware. Note the BCF's board description reads
      `mf16858`, *not* a Quectel part number — that is Morse Micro's internal board ID inside
      `bcf_fgh100mhaamd.bin` itself (`.board_desc` in the shipped file), not a mismatch.
- [ ] **Step 2** — The Pi's AP is visible (`gwcfg-scan`). Proves the radio receives, and says
      whether the AP is on a channel this build may legally use.
- [ ] **Step 3** — HaLow STA associates, then gets a DHCP lease. Two separate milestones, reported
      separately (`searching` vs. `associated, no lease`) because they have different causes.
- [x] **Step 4** — Local SoftAP and DHCP validated standalone (phones join, get a lease, reach the
      web UI). **Passed** — `xiao-gateway` comes up on channel 6, a client associates and is leased
      `172.16.50.2`, and the web UI is reachable. Doesn't depend on steps 0–3 — the natural first
      hardware test. Since re-confirmed running for an extended period on USB power with a client
      associated and the HaLow radio concurrently up and scanning — no reboot loop, no crash.
      **Next up: steps 0–3, which need the Pi side present.**
- [ ] **Step 5** — NAT validated: a phone gets outbound mesh reach. **Test IP reachability and
      name resolution separately** — they fail independently, and DNS propagation was missing
      entirely until recently. Confirm translated source addresses actually appear mesh-side.
- [ ] **Step 5a** — **Plain multicast over HaLow validated, before involving the relay.** Confirm
      an IGMP join on the Morse Micro interface receives group traffic and that the Pi-side mesh
      forwards 239.2.3.1 at all. Multicast over mesh routing is a classic silent-drop point and
      fails identically to a broken relay — isolate them or you can't tell which is at fault.
- [ ] **Step 6** — CoT relay validated: ATAK on a phone sees mesh CoT and vice versa.
- [x] **Step 7** — Web UI authentication. **Not a bench step** — development work, listed here
      because it gates shipping and OTA delivery, so it has no entry in the runbook. **Implemented
      and confirmed on real hardware 2026-08-30** — challenge-response login, forced password-set on
      first use, lockout/backoff, `gwcfg-reset-auth` recovery. See item 1 under "Not built yet" for
      the full design and what's still only exercised in the lab (mobile-browser PBKDF2 timing,
      session-idle timeout, the lockout's later backoff windows).

## 2026-09-10 review tracker

Tracks implementation of [`PROJECT_REVIEW_2026-09-10.md`](PROJECT_REVIEW_2026-09-10.md) (codex,
external review — source baseline `ba4898e`). Tick a finding only once its own acceptance criteria
in the review pass, not merely when the diff compiles — same discipline as the build-order checklist
above. Findings are grouped into the review's own stages (A–F); a stage's exit gate is in the
review's §8 table.

**Before Stage C: F02 conflicts with a settled decision.** F02 recommends HTTPS for management;
"Settled decisions" → Authentication → TLS above says **No**, with reasons (no CA for a private IP,
a self-signed cert trains users past browser warnings, RAM cost) that the review does not appear to
have read or address. Don't implement F02 as written until that's resolved one way or the
other — either the settled decision gets overturned with a recorded reason, or F02 gets re-scoped to
work inside it (e.g. protecting the existing challenge-response flow further without TLS).

### Stage A — immediate correctness (no dependencies) — **done, 2026-09-10**
- [x] F01 — P0 — `cot_relay_get_counters()` reads an uninitialized relay mutex. Fixed: returns
      zero counters when `s_send_lock` is still NULL instead of taking it.
- [x] F04 — P0 — DNS forwarder response matching insufficient. Fixed: the forwarder now rewrites
      the transaction ID to one it chose (removes the multi-client-same-ID collision), and a reply
      must match that ID, the exact resolver address:port queried, and the original question bytes
      before it's relayed - RFC 5452 §9.2's three fields, all three now checked.
- [x] F05 — P1 — DNS truncation handling doesn't match the TCP-fallback comment. Fixed: both the
      query and reply paths use `recvmsg()` and drop (rather than forward) anything the real
      datagram length shows was truncated; the misleading "TC bit" comment is corrected. Full TCP
      fallback is still not implemented - an oversize reply is dropped, not served, which is the
      honest behavior but not complete DNS service. Left as future work if a real deployment needs
      EDNS0-sized replies.
- [x] F10 — P1 — **fully fixed, 2026-09-10.** `provisioning_parse_security()` now returns
      success/failure instead of defaulting unknown strings to open, and every caller (console +
      web UI) rejects on failure. Web UI JSON numeric fields (softap channel, HaLow AP
      op_class/s1g_chan_num/max_stas, CoT port) validate type/integer-ness/range against the full
      `double` before narrowing, instead of casting `cJSON`'s `valueint` directly. A new
      `validate_host_subnet()` helper (`main/provisioning.c`) rejects non-contiguous masks,
      all-zero masks, an IP that's the network or broadcast address of its own subnet, and a
      gateway outside that subnet - applied to the uplink's static IP, the SoftAP's custom subnet,
      and the HaLow AP's subnet. A new `subnets_overlap()` check rejects a client-role static
      uplink whose subnet overlaps its own SoftAP's (the one cross-config overlap that's actually
      checkable at validate time - `wifi_uplink` is DHCP-only and unknown until associated, and
      `softap`/`halow_ap` never coexist on one node). The two bit-tricks (contiguous-mask test,
      overlap test) were spot-checked against a standalone host program before trusting them,
      including this project's own real default subnets (172.16.50.0/24 SoftAP vs. 172.16.60.0/24
      HaLow AP correctly non-overlapping). **Still open, lower priority**: the same
      integer-narrowing pattern in `provisioning.c`'s console setters (`atoi()` results cast
      straight to narrow types) - a physically-present serial operator is a materially different
      threat model than an HTTP client, so left for a future pass rather than this one.
- [x] F17 (boundary bug only) — P2 — log ring `s_wrapped` misses an exact-boundary wrap. Fixed:
      replaced the wrap-detecting bool with an explicit valid-byte count (`s_count`), capped at
      `LOG_RING_SIZE`, so a full ring reads back full regardless of how many writes it took to fill it.
- [x] Safe auth-init failure. Fixed: added `auth_is_ready()`; `web_ui_start()` now refuses to start
      (logs and returns `ESP_ERR_INVALID_STATE`) rather than serve a management interface no auth
      call can safely gate against a NULL mutex.

Verified by a real `idf.py build` after each change (per this file's own discipline) - zero
errors, zero warnings, binary size unchanged at 41% free. Not yet verified on hardware.

### Stage B — ownership and recovery (needs A)
- [x] F06 — P0 — non-TX Morse radio API calls have no shared owner/serialization. Fixed with the
      full task-queue design (user's explicit choice over a lighter mutex-only alternative): a new
      `radio_control` module (`main/radio_control.c/.h`) owns a dedicated FreeRTOS task and a
      depth-1 request queue; every non-TX `mmwlan_`/`mmhalow_` call in the firmware now goes through
      `radio_control_run(fn, ctx, timeout_ms)` instead of being called directly from five previously
      independent, unsynchronized contexts (boot task, `halow_reconnect`, the `esp_timer` service
      task via `link_history.c`'s RSSI sampler, `console_repl`, and `httpd`). `ctx` must be
      static/persistent, never a stack local - the same rule `uplink_halow.c`'s pre-existing
      `s_scan_ctx` already established, reused here because a caller that times out and returns can
      leave the owner task still holding a pointer into memory it no longer owns. Both
      `uplink_halow.c` and `downlink_halow_ap.c` were rewired; `downlink_halow_ap_init()`'s
      `s_channels_lock` mutex creation was moved out of a lazily-initialized, TOCTOU-racy first-call
      site into `downlink_halow_ap_init()` itself (a genuine single-caller boot-time context) as
      part of the same change.

      **Real hardware bug found and fixed, not just a clean compile**: `radio_control_task` was
      first created at this project's usual worker-task priority (5) - the same priority
      `cot_relay`/`dns_forward`/`wifi_reconnect`/`halow_reconnect`/`datapath` already use. On real
      hardware this crash-looped `GW_ROLE_RELAY`'s HaLow AP bring-up 100% reproducibly: a watchdog
      panic (`TG1WDT_SYS_RST`, "interrupt watchdog") inside `mmint_morse_pagesets_work` (a
      closed-source morselib symbol - confirmed via `xtensa-esp32s3-elf-addr2line` against the
      actual built ELF showing `??:?` for that frame, unlike the surrounding frames which resolved
      real file/line) on the vendor's own `drv` task, "Panic handler entered multiple times" on
      every consecutive boot. Root cause, read from `mmwlan.h`'s own "Thread priorities" doc comment
      (L16-28): the vendor's `spi_irq`/`drv`/`evtloop` threads all run at `MMOSAL_TASK_PRI_HIGH`,
      which `mmosal_shim_freertos_esp32.c`'s `mmosal_task_create()` maps to FreeRTOS priority
      `tskIDLE_PRIORITY(0) + 4 = 4` (`mmosal.h`'s `enum mmosal_task_priority`: IDLE=0, MIN=1, LOW=2,
      NORM=3, HIGH=4) - and that same comment states outright "it is expected that application
      threads run at a lower priority." `radio_control_task` at priority 5 was *above* all three
      vendor threads instead of below them, backwards from the documented contract, and during the
      relay's AP bring-up - which fires several `radio_control_run()` calls back-to-back with
      essentially no gap - that let it preempt `drv` mid-pageset-work repeatedly. Fixed by dropping
      `radio_control_task` to priority 3 (`MMOSAL_TASK_PRI_NORM`, the same level morselib's own lwIP
      tcpip/ip threads use). Confirmed on real hardware across many consecutive reboots after the
      fix: `downlink_halow_ap_init()` completes, the HaLow AP starts (`xiao-relay-1`, op_class 2,
      chan 26), and HTTPS/web UI comes up - zero crashes, where before it was 100% reproducible.
      `GW_ROLE_CLIENT`'s STA connect path never hit this (different, more interleaved call timing),
      which is a difference in exposure, not evidence priority 5 was ever actually safe there either.

      **A second, separate, pre-existing bug was uncovered as a direct result of this fix, not
      caused by it**: with HaLow AP bring-up no longer crashing, `GW_ROLE_RELAY`'s boot sequence now
      reaches `uplink_wifi_init()` (the native ESP32 2.4 GHz Wi-Fi uplink) for the first time in a
      long while - and crashes there instead, 100% reproducibly, with the *same* `TG1WDT_SYS_RST`
      reset reason, inside `wifi_lmac_init`/`wDev_Rxbuf_Init` (Espressif's own closed `libnet80211`/
      `libpp` blob - also `??:?` under `addr2line`). Reordering `uplink_wifi_init()` before
      `downlink_halow_ap_init()` as a diagnostic (not committed) moved the crash rather than fixing
      it, and produced what looks like a genuine power-on reset loop instead (USB CDC fully
      re-enumerating with a new device number every few seconds, `esp_reset_reason()` reporting
      `power-on` rather than any watchdog) - consistent with the two radios' peak startup current
      colliding, not a pure scheduling issue. The original call order was restored (this file's own
      diagnostic reorder is not in the tree). Notably, `design/ROADMAP.md`'s own Aug 29 relay-run
      notes below record this exact node successfully reaching "native Wi-Fi uplink associated and
      got a DHCP lease" on that date - so this is a **regression introduced since Aug 29**, not a
      bug that's always been there. **Internal-RAM starvation from F02/F13 was the first suspect but
      is now ruled out by direct measurement**: `heap_caps_get_free_size()`/`_get_largest_free_block()`
      logged immediately before `uplink_wifi_init()` showed 190,907 bytes free internal
      (118,784 largest contiguous block) and 183,119 bytes free DMA-capable - identical across every
      crash, and far more than `esp_wifi_init()` needs. Not yet root-caused or fixed - open as its
      own item below Stage B rather than folded into F06, since F06's own scope (owning/serializing
      the *HaLow* radio API) is what's actually fixed and hardware-verified.
- [ ] Native Wi-Fi uplink bring-up crashes `GW_ROLE_RELAY` (regression since Aug 29, found while
      hardware-verifying F06) — P0 — `uplink_wifi_init()`'s `esp_wifi_init()`/`esp_wifi_start()`
      call trips a `TG1WDT_SYS_RST` interrupt watchdog inside Espressif's own closed
      `wifi_lmac_init`/`wDev_Rxbuf_Init`, 100% reproducibly, immediately after the HaLow AP has
      already started successfully. **Current leading hypothesis: a physical power-delivery limit,
      not a firmware bug** - see the evidence trail below before trying more code-side fixes.

      **2026-09-11 update, supports the power theory further:** switching the relay node to a
      shorter USB cable produced a clean run - HaLow AP up, native Wi-Fi uplink associated, DHCP
      lease, datapath fully up with a leaf associated, sustained with no reboot for as long as it
      was observed. Not yet called fixed: no code changed between that run and the flaky ones before
      it, so this is corroborating evidence for the power theory, not a confirmed root-cause fix. A
      powered USB hub is the planned next test, to get a real, deliberate before/after comparison
      rather than relying on cable-quality variance.

      **Second node affected (2026-09-11, during F12 verification):** the leaf board - previously
      stable through the entire 3h39m stability soak and the F09 adversarial test earlier the same
      session - began showing the identical signature (rapid USB re-enumeration, stuck at "Morse
      Micro HaLow NetIF: Setting Channel List US", `reset reason: power-on`, no firmware panic ever
      reached) while being reflashed for an unrelated change (`heap_guard.c`/`.h`, pure
      comparison-threshold logic, no new timing or syscalls). The leaf also brings up two radios
      close together at boot - native Wi-Fi SoftAP, then HaLow STA - the same structural shape as the
      relay's own crash, just the other role. Consistent with the power-delivery theory extending
      beyond one specific board/cable rather than a code regression: nothing shipped in the F09/F12
      changes touches radio bring-up timing. Left for the same powered-hub test already planned
      above, not separately investigated.

      A second diagnostic reorder (native Wi-Fi before the HaLow AP, matching `bring_up_client_role()`'s
      already-working structure of "native radio first, HaLow second") was tried and also reverted
      (not committed). It didn't clean up the crash - it changed its signature: every failure now
      reports `esp_reset_reason() == ESP_RST_POWERON` ("power-on", not any watchdog) on a strikingly
      regular ~3-4s cadence (confirmed against `journalctl -k`'s USB re-enumeration timestamps, not
      just the firmware's own claim), and the ROM bootloader banner (`ESP-ROM:esp32s3...`) never
      appears at all across 30s of captured serial output - unlike the original order's crash, where
      the banner and `rst:0x8 (TG1WDT_SYS_RST)` line came through cleanly on every single cycle. That
      difference matters: it means the reordered failure is severe enough to disrupt the USB
      peripheral's own enumeration, i.e. a lower-level reset than a software-detected watchdog. It
      also got further at least once before failing again - `uplink_wifi: Wi-Fi uplink associated
      (RSSI -66 dBm), waiting for DHCP lease...` - so this isn't a hard, instant failure either order.
      Taken together (healthy heap either way; a regular, fast reset cadence; a signature that gets
      *worse*, not better, when the two radios' power-up windows are pushed closer together; and this
      exact board/cable pairing already flagged once this session for unrelated flaky-USB symptoms -
      see "they devices keep restarting" in git/session history), the leading theory is that
      concurrent HaLow-beacon-plus-native-WiFi-PHY-calibration current draw exceeds what this specific
      USB port/cable can source, and the *original* order's `TG1WDT_SYS_RST` may itself be a milder
      symptom of the same marginal supply (a voltage dip enough to desync timing without crossing the
      full POR threshold) rather than a pure scheduling bug.

      **Next step needs hands on the hardware, not more code**: retest the relay board (original,
      committed call order) on a different USB cable/port, ideally a powered hub or a bench supply
      capable of a clean current spike, and see whether the crash disappears entirely. If it does,
      this was never a firmware bug. If it persists on genuinely clean power, that reopens the
      software investigation - candidates at that point would include a deliberate delay/settling
      window between the two radios' power-up (tried once as a reorder, not yet tried as a delay on
      top of the original order), or a Kconfig-level look at `esp_wifi_init()`'s own buffer/DMA
      allocation options. Blocks full `GW_ROLE_RELAY` hardware verification either way - the HaLow AP
      downlink now works, but the Wi-Fi uplink to the Pi does not.
- [x] F07 — P1 — scan timeout doesn't synchronize against callback lifetime. Confirmed as a real bug
      by reading both call sites, not just the review's description: `web_ui.c`'s `scan_post_handler()`
      calls `uplink_halow_scan(scan_result_cb, array, ...)` where `array` is a `cJSON*` it deletes
      shortly after the call returns; `scan_rx_cb()` (invoked from the driver's own scan/event task,
      not the caller's) checked a generation counter before touching `sc->cb`/`sc->ctx`, but nothing
      stopped a callback that had already passed that check from being preempted, resuming after the
      caller timed out and freed/reused that memory, and then calling `cb()` through it - a real
      use-after-free into a cJSON tree, plus concurrent, unsynchronized mutation of that same tree
      from two tasks even short of the free case (cJSON has no internal locking). Checking generation
      once, as the old code did, cannot close either problem - F06's own radio_control serialization
      doesn't help here either, since it only owns the *submission* call, not the driver's later,
      asynchronous delivery of results.

      Fixed by removing the cross-task sharing entirely rather than adding more synchronization
      around it: `scan_rx_cb()` no longer touches a caller's `cb`/`ctx` at all - it only writes into
      a new module-owned, bounded static array (`UPLINK_HALOW_SCAN_MAX_RESULTS = 32`, matching the
      review's "cap record count... count overflow" acceptance criterion). `uplink_halow_scan()`
      itself bumps the generation (retiring any further writes into that array) *before* reading it
      back, then delivers every result to `cb(ctx)` synchronously, on its own caller's task, after
      its wait on the driver has already returned - success or timeout. `cb`/`ctx` are now plain local
      variables in that one function, never written from another task, so there is nothing left for a
      late callback to race against. `main/uplink_halow.h`'s `uplink_scan_cb_t` doc comment was wrong
      about this ("invoked... from the driver's scan task") and is corrected.

      Verified on real hardware, not just a clean build: flashed the client-role leaf node
      (`xiao-gw-2e40`) and ran `gwcfg-scan` at the console - found the relay's AP twice (two
      configured bandwidths), delivered both results correctly, logged `scan complete: 2 AP(s) found`,
      returned cleanly, and the node went on to associate and bring its datapath up immediately after
      - no crash, no hang, no behavior change visible to either caller (`web_ui.c`'s cJSON path shares
      the exact same, now-fixed delivery mechanism in `uplink_halow.c`, so this exercises the part
      that actually changed).
- [x] F08 (retry-on-late-downlink only, deliberately scoped down from the review's full
      network-supervisor redesign) — P1 — datapath bring-up is one-shot; recovery after
      address/partial-failure/timing races is incomplete. **Fixed the one gap in this list that is a
      concrete, demonstrable bug rather than a design tradeoff already recorded elsewhere**:
      `datapath_task()` used to only retry a failed `bring_up_datapath()` on the *next*
      `on_uplink_state(true)` call. If the uplink was already connected and stayed connected - the
      common case, since an already-associated uplink reconnecting isn't what unblocks a slow
      downlink - and the downlink then took longer than `wait_for_downlink_up()`'s
      `DOWNLINK_UP_TIMEOUT_MS` (5s) to come up, nothing would ever wake the task again: the node
      would sit with both radios up but no NAT/CoT relay configured until a manual reboot. Not
      hypothetical - this session's own hardware testing measured the relay's HaLow AP downlink netif
      taking anywhere from ~2.5s to several seconds past boot to report up, run to run, comfortably
      within range of racing an uplink that's already connected well before it.

      Fixed by giving `datapath_task()` its own bounded retry timer
      (`DATAPATH_RETRY_INTERVAL_MS = 3000`) in addition to the existing wake-on-reconnect: once
      `s_datapath_up` is set the task still parks on `portMAX_DELAY` and self-deletes exactly as
      before (that resource-reclaim behavior is unchanged), but until then it retries every 3s on its
      own regardless of uplink transitions. Safe to retry blindly - `ip_forward_nat_init()`,
      `cot_relay_start()` and `dns_forward_start()` are all idempotent, cheap, state-setting calls
      that already treat "already running" as success (confirmed by reading `ip_forward_nat.c` before
      relying on it, not assumed). Verified with a real build (zero errors/warnings) and a full
      reboot of both physical nodes: role client and role relay both came up clean, HaLow AP + Wi-Fi
      uplink + CoT relay all running, no behavior change on the normal (fast) bring-up path.

      **What this does *not* cover, deliberately**: the review's fuller F08 scope - a real
      `network_supervisor` with typed link/address/service state, jittered backoff per failure class,
      and reacting to a *changed* IP on reconnect (not just a repeated "up") - is still open. The
      changed-IP case specifically is not a silent gap: it's already recorded as a deliberate v1
      limitation in `bring_up_datapath()`'s own comment and in "Settled decisions" below, not
      something this pass tried to re-litigate. The review's own acceptance criteria for the full
      redesign (100 outage/recovery cycles, controlled interleaving tests) are beyond what's
      practical to force on this bench setup - this fix targets the one gap that's a straightforward,
      demonstrable defect rather than an architectural gap needing that scale of testing.
- [x] F11 (lost-update fix only, deliberately scoped down from the review's full desired/active
      config-store redesign) — P1 — saved configuration and active network state are conflated.
      **Confirmed by reading every writer, not just the one the review names**: `web_ui.c`'s
      `config_post_handler()` is the *only* place in the firmware that reads a snapshot of the live
      config, unlocks (deliberately - so a slow JSON parse doesn't hold `provisioning_config_lock()`
      and stall the console/every other reader), then later re-locks and writes the *entire* snapshot
      back. Every one of provisioning.c's 7 console setters (`gwcfg-set-node`, `-uplink`, `-softap`,
      `-role`, `-uplink-mgmt`, `-uplink-static-ip`, `-wifi-uplink`, `-halow-ap`) and
      `auth_commit_password()` in `auth.c` already do their own read-validate-write as one unbroken
      lock hold - they cannot race each other or lose an update, and don't need this fix. So a
      concurrent console edit landing in `config_post_handler()`'s unlocked window was the one real,
      demonstrable gap: the HTTP write-back would silently clobber it with its own now-stale snapshot,
      not just for the field the console changed but for *every* field neither side explicitly
      touched - a true lost update, exactly as the review describes, not a hypothetical.

      Fixed with an in-RAM revision counter rather than the review's fuller `desired_config`/
      `active_config`/typed-patch design: a new `provisioning_config_commit()` (`main/provisioning.c`)
      is now the *only* function allowed to write the live `gw_config_t`, and it bumps a
      `provisioning_config_revision()` counter every time. All 8 existing write sites (7 console
      setters + `auth_commit_password()`) were switched from a raw `*s_cfg = work`/`memcpy()` to call
      it - mechanical, one line each, no behavior change for any of them. `config_post_handler()`
      records the revision under the same lock as its initial snapshot, then checks it again under
      the lock it re-takes before saving; a mismatch means something else committed while this
      request was in flight, and the request is refused (500 with a clear message - `esp_http_server`
      has no 409, same constraint noted elsewhere in this file) rather than guessing at a merge or
      clobbering silently. The counter is deliberately not persisted and resets to 0 every boot - it
      only has to mean "changed since I looked" within one boot's lifetime, not survive a reboot.

      Verified with a real build (zero errors/warnings) and on real hardware: reflashed both physical
      nodes, ran `gwcfg-set-node` on the relay through the refactored console path and confirmed the
      new value stuck via `gwcfg-show` with the node otherwise unaffected (Wi-Fi uplink, HaLow AP, CoT
      relay all still running) - proving the 8-site mechanical refactor didn't break the already-atomic
      writers. **Not independently verified on hardware**: forcing the actual HTTP-vs-console race
      itself would need a full PBKDF2/HMAC login client built just for this test (the config API sits
      behind F03's session auth) - disproportionate effort for a change this mechanically simple to
      reason about (a plain integer compare-and-reject, reusing a lock this file already trusted for
      every other read/write here). Flagged rather than silently assumed, matching this file's own
      "verify, don't guess" discipline.

      **Left open, deliberately**: the review's fuller redesign - typed patches applied against the
      *latest* desired config rather than rejected outright, separate credential-vs-network-config
      write paths, and reporting active-vs-pending/`reboot_required` to the UI - none of that is
      built. This fix stops the silent data-loss case; it does not add patch semantics or a UI-visible
      pending/active distinction.
- [x] F13 (explicit budget + margin) — P1 — **landed 2026-09-10, ahead of the rest of Stage B at the
      user's direction.** The socket budget is now written down and computed, not implicit:
      GW_ROLE_RELAY needs exactly 7 (HTTPS: `HTTPD_SSL_CONFIG_DEFAULT()`'s `max_open_sockets=4` + 3
      esp_http_server.h reserves internally) + 1 (CoT) + 2 (DNS forwarder) = 10 sockets - precisely
      `CONFIG_LWIP_MAX_SOCKETS`'s old Kconfig default, meaning the relay role was booting with *zero*
      spare sockets for anything else lwIP might transiently need. Raised to 14 (+4 margin) in
      `sdkconfig.defaults`, with the full accounting in its own comment. Real cost measured on
      hardware (a GW_ROLE_RELAY node, idle, uplink associated, one HaLow leaf attached), not guessed:
      free internal heap 103,279 → 101,579 bytes for the 4 extra slots (~425 bytes/socket) -
      `MEMP_NUM_NETCONN == CONFIG_LWIP_MAX_SOCKETS` preallocates a real `struct netconn` per slot
      regardless of whether it's ever opened, confirmed against `components/lwip/port/include/
      lwipopts.h`. Reflashed both bench nodes after the change; relay/leaf reassociated and HTTPS
      kept working (verified with `curl`) with no regression. **Still open from F13's original
      scope**: "reserve datapath capacity before accepting management sessions" (an explicit
      ordering/reservation guarantee, not just headroom - `web_ui_start()` still runs before
      `datapath_task_start()` in `app_main.c`'s bring-up, so a burst of early browser connections
      could in principle still race the datapath's own socket allocation at boot) and admission-
      failure visibility/reject-cheaply for socket allocation itself (distinct from the HTTP-level
      `max_open_sockets` LRU purge, which the review already confirmed rejects overload cleanly - see
      the "Settled decisions" TLS row's load-test numbers). Both are more naturally part of F08's
      supervisor work than a standalone follow-up.

### Stage C — management security (needs A; B's snapshots/budgets) — **TLS conflict above now resolved**
- [x] F02 (HTTPS transport) — P0 — **landed 2026-09-10, ahead of Stage B at the user's direction.** Web UI now
      serves HTTPS only (`main/tls_identity.c`, `web_ui_start()`), self-signed per-device cert with
      serial-console fingerprint verification - see the "Settled decisions → Authentication → TLS" row
      above for the full design and hardware confirmation so far. Session cookies gained `Secure`;
      credential/session/config responses gained `Cache-Control: no-store`. A real TLS client's
      handshake and served-certificate fingerprint were verified end-to-end against a live two-node
      bench pair, and `httpd` stack headroom (34% worst-case) was measured under real concurrent
      TLS-handshake + CoT-injection load, not idle - see the "Settled decisions" row for the full
      numbers. **Still open from F02's original scope**: reject-a-substituted-identity /
      logout-revocation / persistence-failure test coverage, and revisiting the socket/RAM budget
      once Stage B's F13 work lands (this session's 8-parallel-client test confirmed the existing
      `max_open_sockets=4` cap rejects overload cleanly rather than exhausting memory, but F13 is
      still where the deliberate budget across HTTP+DNS+CoT gets written down formally). The stored
      key itself is
      still password-equivalent-adjacent cleartext in NVS pending F14 - unchanged from before, not a
      new gap this introduced.
- [x] F03 — P1 — **landed 2026-09-10.** Claiming admin ownership (`POST /api/auth/password`,
      first-use only) now also requires a per-device `setup_secret` - a random 128-bit value
      generated at first unclaimed boot, logged once at boot and available any time via
      `gwcfg-show-setup-secret` (the same physical-presence/TOFU model `tls_identity.c`'s
      certificate fingerprint already established). The window closes after
      `AUTH_ONBOARDING_BOOT_BUDGET` (10) boots without a claim - a boot count, not a wall-clock
      timeout, since this board has no RTC - and stays closed across a plain reboot; only
      `gwcfg-reopen-onboarding` (console, physically-present-only) reopens it. `GW_CONFIG_VERSION`
      bumped 8→9 for the new `gw_auth_config_t` fields.
      **A real ordering bug was caught by testing the actual HTTPS endpoint, not by review**: the
      first implementation consumed the one-time PBKDF2 salt offer before checking the setup
      secret, so a single wrong-secret attempt burned the salt and made the *next* attempt - even
      with the correct secret - fail with `no_pending_salt`. Fixed by checking the setup secret
      first, confirmed with a real end-to-end claim over HTTPS (`curl`/Python + real PBKDF2)
      against a live bench relay: wrong secret correctly rejected (401 `bad_setup_secret`) without
      disturbing the salt offer, then the same salt offer with the correct secret succeeded and
      returned a working session cookie. `auth_init()`'s boot-time bookkeeping deliberately mutates
      the global config in place rather than using a `gw_config_t`-sized stack local, for the same
      thin-"main"-task reason documented on `GW_STACK_TLS_IDENTITY_GEN` (task_stats.h) - not
      re-tested to failure this time, fixed proactively from that lesson instead.
      **Unrelated environmental finding during this work, worth recording**: a flaky USB
      cable/power connection on the bench caused repeated `power-on` resets (confirmed via the
      logged reset reason and `journalctl` USB disconnect/reconnect events) that looked like a
      firmware crash loop but was not one - every captured boot completed cleanly with no panic.
      The same flaky power window most likely corrupted an in-progress NVS write, since both bench
      nodes came back at default config after cable reseating and had to be reprovisioned - a
      real-world case for F14's eventual "power loss during save recovers coherently" acceptance
      criterion, not a defect introduced here.
- [x] F15 (challenge table + absolute session cap; credential-revision race checked and found already
      safe) — P2 — auth challenge state is single/global, not per-transaction. **Verified the
      review's three named issues individually rather than assuming all applied**: (1) a single
      global `s_pending_challenge` meant a second client's `GET /api/auth/challenge` silently
      replaced a first client's in-flight one - real, confirmed on hardware below; (2) a wrong/stale
      nonce cleared whatever challenge happened to be pending, not necessarily the sender's own -
      same root cause as (1), fixed by the same change; (3) sessions had only a sliding idle timeout,
      no absolute cap, so a continuously-touched session (an auto-refreshing tab, say) never actually
      expired - real, also confirmed below. A fourth concern the review raises - "recheck credential
      revision before issuing a session so a concurrent local reset cannot authorize an old
      verification result" - turned out to be **already handled correctly**: `auth_verify_login()`
      reads `s_cfg->auth.stored_key` fresh, under the lock, at verify time, not a cached copy from
      when the challenge was issued, so a password change mid-login makes the client's HMAC response
      (computed against the *old* key) fail comparison against the *new* one - correctly rejected as
      bad credentials, not incorrectly authorized. No fix needed there; confirmed by reading the code
      rather than assumed.

      Fixed (1)/(2) by replacing the single global challenge with a small table
      (`s_pending_challenges[AUTH_CHALLENGE_MAX=4]`, `main/auth.c`), mirroring the existing
      `s_sessions[]` table's own evict-oldest-when-full pattern (an expired-but-not-yet-reclaimed
      slot counts as free, so a burst of abandoned challenges from one client can't evict a
      different client's still-valid one ahead of its own TTL). `auth_verify_login()` now scans for
      the *one* slot matching the submitted nonce and clears only that slot, never a shared one.
      Fixed (3) by adding `issued_us` to `auth_session_t` alongside the existing `last_seen_us`, and
      checking both in `auth_check_session()` - `AUTH_SESSION_ABSOLUTE_MAX_US` (12h) matches
      `web_ui.c`'s existing cookie `Max-Age`, making that already-stated lifetime actually enforced
      server-side instead of merely advisory (a client that simply keeps resending the same cookie
      past its `Max-Age` wasn't previously stopped by anything on this end).

      **Verified on real hardware with genuine before/after evidence, not just a clean build**: this
      session already had network access to a physical relay node's HTTPS management API (the same
      LAN as its Wi-Fi uplink), so a full test client was written in Python (`hashlib.pbkdf2_hmac` +
      `hmac` - both stdlib, matching `web_ui.html`'s bundled client-side crypto exactly) to drive the
      real onboarding-claim and login flows end to end, not just the console side. The first test run
      landed on the node's *previous* firmware (F11's build - flashing the F15 build came after, not
      before, by mistake) and reproduced the exact bug being fixed: issuing challenge A, then B, then
      completing login with B, then attempting A failed - proof the old single-slot design really did
      let one client's challenge silently clobber another's. Reflashing with the F15 build and
      repeating the identical sequence four consecutive times all succeeded (both A and B
      independently valid), confirmed via temporary diagnostic logging that A and B land in separate
      table slots and are matched/cleared independently. The absolute session cap was verified the
      same way: temporarily set to 5s (reverted after), a session was touched every 2s (which would
      keep a pure idle-timeout session alive indefinitely) and still correctly expired at the 5s mark
      regardless - `authenticated` flipped from `true` to `false` in `/api/auth/status` right on
      schedule. Both diagnostics (log lines, the shortened constant) were reverted before the final
      build; the relay node was confirmed still fully healthy afterward (Wi-Fi uplink, HaLow AP, CoT
      relay all running) with the restored 12h constant.

      **Left open, deliberately**: `AUTH_CHALLENGE_MAX`/`AUTH_SESSION_MAX` (both 4) are a fixed
      bound, not the review's fuller "admission limits" with separate abuse-rate accounting; a
      challenge-issuance flood still only competes for table slots, it isn't independently
      rate-limited. Judged proportionate for a single-admin device already gated by
      `reject_if_remote()` (only same-subnet clients can reach any of this) rather than built out
      further.
- [x] F14 (migration portion only - dev/field provisioning profiles, secure boot, flash/NVS
      encryption and core-dump redaction are Stage E's "production profile" item, explicitly not
      this) — P1 — schema-bump/corrupt-config paths can silently reset ownership. **Confirmed as a
      real, not hypothetical, bug against this project's own history**: `provisioning_load()`'s
      fallback-to-defaults path ran on a `GW_CONFIG_MAGIC`/`GW_CONFIG_VERSION` mismatch, a
      wrong-size/unreadable blob, or failed validation - and every one of those called
      `provisioning_get_defaults()`, which leaves `password_set == false` *and* `onboarding_open ==
      true`. That's indistinguishable from a genuinely fresh, never-owned device. This session alone
      bumped `GW_CONFIG_VERSION` three times (v7→v8→v9, for F02 and F03) - every one of those, on a
      real device with a real stored credential, would have silently un-owned it on the next boot,
      reopening the unauthenticated first-use claim flow to anyone with SoftAP access.

      Fixed in two parts, the second found only by testing the first on real hardware:

      1. A new `provisioning_get_recovery_defaults()` (`main/provisioning.c`) - same networking
         defaults as `provisioning_get_defaults()`, but forces onboarding permanently closed instead
         of open. `provisioning_load()`'s three "a blob existed but is unusable" fallback branches
         (wrong size, magic/version mismatch, failed validation) now use it instead of the open
         version; only a genuinely-never-written key (`ESP_ERR_NVS_NOT_FOUND`) still gets the open,
         fresh-device defaults. The device still boots and still serves SoftAP/console/management -
         it is not bricked - but nobody can claim it through the web UI. The only way back in is the
         existing physical factory-reset button (`factory_reset.c`, already built and hardware-proven
         earlier this project's life), which calls the *open* `provisioning_get_defaults()`
         deliberately, on physical possession - exactly the review's "closed recovery state requiring
         local owner action," reusing an existing mechanism rather than building a new one.
         `provisioning_init()`'s own NVS-erase-on-corruption (`NO_FREE_PAGES`/`NEW_VERSION_FOUND`)
         was deliberately left alone - that's ESP-IDF's own standard, unavoidable recovery pattern
         for whole-partition-level corruption (nothing partial is readable at that layer at all),
         a fundamentally different and rarer scenario than a per-blob schema mismatch.

      2. **Found while verifying part 1 on real hardware, not predicted**: closing onboarding wasn't
         enough on its own. `tls_identity_init()`'s boot-time "no identity yet, generate and save one"
         logic ran during the same recovery boot and persisted the *entire* live (recovery-defaulted)
         struct in the process - permanently overwriting the real blob still sitting in NVS at that
         point, before any operator ever touched anything. Simulating a real version bump against a
         board with a real, just-set marker config (`gwcfg-set-node f14-test-marker` +
         `gwcfg-set-role relay`, saved) reproduced this exactly: after the round trip back to the
         matching version, the marker was gone, replaced by client-role factory defaults. Fixed by a
         `provisioning_in_recovery_mode()` flag, set by `provisioning_get_recovery_defaults()`, that
         gates *only* `tls_identity.c`'s automatic (non-operator, `force == false`) persist -
         `tls_identity_regenerate()`'s explicit `gwcfg-reset-tls-identity` console path (`force ==
         true`) stays exempt, since console access already requires physical presence, the same bar
         the review asks recovery to enforce. During a gated boot the freshly generated identity is
         still used in RAM for that boot's HTTPS server - the device isn't left without one - it just
         isn't persisted over the original blob. Deliberately not a general "block every save" gate:
         every console command and the web UI's own config write are already safe here for their own,
         different reasons (physical access; `auth_require_session()` has no session to check with no
         password ever set) - this exists for the one call site that genuinely runs unattended.

      **Verified on real hardware with a genuine, reproducible before/after**: the exact same
      simulated-version-bump procedure against the exact same board, before fix part 2 landed,
      destroyed `f14-test-marker`/`role: relay`; after it landed, run again start to finish, the
      marker and role survived the round trip completely intact. Onboarding-closure (part 1) was
      separately confirmed via `gwcfg-show-setup-secret` reporting "onboarding is closed - use
      gwcfg-reopen-onboarding to claim this device" during the simulated-mismatch boot, instead of
      handing out a setup code as it would for a genuinely fresh device. `GW_CONFIG_VERSION` was
      returned to its real value (9) after each test; `git diff` confirmed `gw_config.h` clean before
      committing.

### Stage D — multi-node forwarding (needs A/B)
- [x] F09 (destination-group validation only - dedup cache, TTL policy, token-bucket rate budgets and
      a redundant-relay envelope protocol are all still open, see below) — P1 — CoT relay has no
      destination-group check, dedup cache, or rate budget. **Fixed the one gap in this finding that
      is a genuine open-relay vulnerability, not a design tradeoff**: `cot_relay_start()`'s socket
      binds `INADDR_ANY:port` (needed - it has to receive multicast arriving on either interface),
      but `relay_task()` never checked the received datagram's actual *destination* address against
      the configured CoT group - only which interface it arrived on and whether the source was this
      node's own. That means a plain **unicast** datagram sent straight at either interface's own
      IP:port would be picked up by this socket and faithfully re-transmitted as multicast to the
      whole other side - the node acting as an open UDP relay for anyone who can reach either
      interface, not just a CoT forwarder for the multicast group it's configured to carry.

      Fixed by reading `ipi_addr` out of the same `IP_PKTINFO` ancillary data `relay_task()` already
      parses for `ipi_ifindex` (no new syscall, no new cmsg loop), and dropping anything whose
      destination doesn't match the configured group before it ever reaches `is_own_address()` or
      `send_via()`. The existing comment on why `ipi_addr` can't identify the *arrival interface*
      (it reflects the packet's destination, not which netif it came in on, so it's always the
      multicast group for legitimate traffic) turns out to be exactly why it's the right signal for
      *this* check - lwIP fills it from the datagram's real destination field, so a stray
      unicast/broadcast/wrong-group packet correctly reads back as whatever it was actually addressed
      to, not the group. Drops are counted, not logged per-packet (`cot_relay_get_wrong_dest_drops()`)
      - a flood of these, deliberate or not, shouldn't become a logging flood itself, matching this
      project's existing "aggregate repetitive errors" discipline (F17). Wired into both
      `gwcfg-status` and `/api/status` alongside the existing CoT counters.

      **Verified on real hardware with a direct, adversarial-shaped test, not just a clean build**:
      with the relay+leaf bench pair up and freshly confirmed doing normal multicast CoT relay (50/50
      delivered, `wrong_dest_drops: 0`), a single **unicast** UDP datagram was sent from this dev
      machine straight at the relay's own home-network IP on the CoT port. Before this fix that
      packet would have gone straight through `send_via()` onto the HaLow side. After it: the relay's
      `cot uplink`/`cot downlink` counters did not move at all, `wrong_dest_drops` went 0 → 1 exactly
      matching the one probe sent, and the leaf's own `cot downlink rx` confirmed zero packets
      actually arrived as a result of it - the probe was identified and dropped, not amplified.

      **Left open, deliberately** (the review's fuller "Implement" list): a dedup/fingerprint cache,
      explicit multicast TTL policy, per-ingress token-bucket rate limiting, and a
      hop-budget/origin-ID envelope for genuinely redundant multi-relay topologies. None of those
      apply to this project's current single-relay-pair deployment the way the open-relay gap did -
      they're real hardening for a future multi-node/redundant-gateway topology (Stage D's own
      broader scope), not a demonstrated bug against what's actually running today.
- [x] F12 (honest logging + hysteresis only - measured radio/client admission control, moving work off
      the shared timer task, bounded service queues and explicit reservations are all still open,
      see below) — P1 — heap-shedding's DHCP pause doesn't actually stop new associations/traffic.
      **Fixed the two concrete, narrowly-scoped bugs the review confirmed, not the full admission-
      control redesign it also suggests**: (1) the log line printed when the SoftAP's DHCP server
      paused said "pausing new SoftAP associations" - factually wrong, confirmed by reading what
      `esp_netif_dhcps_stop()` actually does. It stops the DHCP server, nothing else: 802.11
      association at the radio is untouched, and an already-leased or static-IP client keeps working
      exactly as before. All this ever did was refuse a *new* DHCP lease, which is still a real,
      useful admission control - just not the "no new associations" guarantee the wording implied to
      whoever read the log. (2) both `apply_node_shed()` (DHCP pause) and `sample_timer_cb()` (CoT
      shed) compared free internal heap against one shared threshold for both engaging *and* clearing
      the degraded state, so a node whose heap happened to sit right at that line would flap in and
      out of it - "shed engaged"/"cleared" every 5s sample - which is itself the kind of instability
      an admission-control mechanism should prevent, not cause.

      Fixed (1) by rewording the log lines to say what actually happens ("pausing/resuming new DHCP
      leases"). Fixed (2) with real hysteresis: each of the two shed decisions now has a separate
      `_ENTER_BYTES` (lower) and `_EXIT_BYTES` (higher) threshold, and which one is compared against
      depends on the *current* latched state - once engaged, heap has to climb back past the higher
      exit line before clearing, not just tick back over the same line it dropped below. `_ENTER_BYTES`
      values are unchanged from before (64KiB/32KiB); `_EXIT_BYTES` adds a deliberately-chosen gap
      (80KiB/48KiB) - a starting choice, not yet independently measured against real fragmentation
      behavior, same caveat the original numbers already carried.

      **Verified on real hardware, not just reasoned about**: temporarily set both CoT-shed thresholds
      to bracket the relay's actual observed free-heap reading (110KiB/120KiB, comfortably above the
      real ~97-103KiB baseline this session measured repeatedly), so the shed state would engage
      immediately and stay engaged under real, live heap noise rather than a simulated one. Captured
      the exact expected transition on boot (`CoT relay shed engaged (free internal heap 103507
      bytes, enter threshold 112640)`), then watched 7 further 5s samples (35s) produce *zero*
      further "cleared"/"engaged" log lines despite whatever normal heap drift occurred in that
      window - directly confirming the gap holds under real observed noise, not just in theory.
      Thresholds reverted to their real values immediately after; both boards reflashed and confirmed
      not spuriously shedding under healthy heap (~97-118KiB internal free, both well above either
      enter threshold).

      **New data point surfaced while reflashing for this verification, not caused by it**: the leaf
      board began exhibiting the same crash-loop signature already tracked as an open P0 under F06
      (rapid USB re-enumeration, stuck at HaLow radio init, `reset reason: power-on`, no firmware
      panic reached) - previously only seen on the relay. The leaf also brings up two radios close
      together at boot (native Wi-Fi SoftAP + HaLow STA uplink), so this is consistent with the same
      power-delivery theory, not a new regression - `heap_guard.c`/`.h`'s changes here are pure
      comparison-threshold logic with no new timing or syscalls, and F12's own verification above was
      captured cleanly on the relay *before* this appeared on the leaf. Left for the same physical
      remediation (powered hub) already planned under F06's tracker entry, not re-investigated here.

      **Left open, deliberately** (the review's fuller "Implement" list): measured radio/client-level
      admission control (the actual "stop new associations" guarantee the old log wording implied but
      never delivered), monitoring largest free block alongside total free bytes, moving the
      DHCP/radio calls off the shared `esp_timer` service task, bounded per-client service queues, and
      explicit reservations for CoT/DNS/management traffic ahead of allocation pressure. All real
      hardening for sustained load this project hasn't load-tested yet, not a demonstrated defect in
      what ships today beyond the two fixed above.

### Stage E — deployment hardening (needs B/C/D)
- [ ] F14 (production profile) — P1 — flash/NVS encryption, secure boot, core-dump handling as one
      lifecycle. **Deliberately deferred, not forgotten (2026-09-11 decision)**: secure boot and
      flash encryption on this chip mean burning eFuses - irreversible. Get key custody or the
      signing pipeline wrong and a board is permanently bricked with no recovery, which is exactly
      why the review's own "Implement" text for this finding warns "do not burn eFuses... merely
      because this report recommends a field profile; key custody, replacement and recovery must
      first be tested on designated hardware" - i.e. sacrificial boards this project doesn't have.
      That's real engineering days plus irreversible hardware risk, to defend against an attacker
      with *physical possession* of a device who wants to persist malicious firmware undetected -
      not this project's actual deployment today (a bench-tested private mesh gateway the operator
      controls, not hardware handed to untrusted parties). Physical possession already grants the
      factory-reset button and a USB console; secure boot doesn't change that calculus much. This
      session's actual work went where the real, demonstrated attack surface was instead - network-
      facing findings (F02/F03/F06/F07/F08/F09/F11/F12/F15, F14's own migration portion) - which is
      a better return on the same hours. Revisit if/when this project moves toward hardware given to
      people who aren't the operator, at which point budget real time for the "test on designated
      hardware" step the review insists on rather than rushing it.
- [x] F16 (`patch_vendored_halow.py`'s partial-patch bug, plus a follow-up CI-hardening pass - the
      discarded `mmhalow_wifi_start()` return status, a container-identity pin, and an automated
      regression suite are still open, see below) — P2 — vendor-patch verification is marker-only;
      build identity isn't recorded/reproducible.

      `patch_file()` in `patch_vendored_halow.py` applied `mmhalow.c`'s two separate replacements
      (`halow_transmit()`'s VIF fix and `mmhalow_wifi_start()`'s `ap_mode_enabled = true`) under one
      whole-file marker check: if the marker string appeared *anywhere* in the file, the function
      returned immediately, without checking - let alone applying - the second replacement. A file
      where only the first replacement had ever landed (an interrupted prior run, a hand-edit, a
      partially-written checkout) would be silently accepted as "already patched, nothing to do" -
      exit 0, no error - while `mmhalow_wifi_start()` never actually set `ap_mode_enabled`. That's a
      silent reintroduction of the exact VIF-inference bug (design/ROADMAP.md's own Aug 30 findings)
      this script exists to prevent, with nothing telling anyone it happened.

      Fixed by checking each replacement's own already-applied (`new`) text independently instead of
      one shared marker: a replacement already present is skipped on its own merits; a replacement
      whose text matches neither the pre-patch nor post-patch form (a genuinely changed/unknown
      upstream shape) fails loudly, exit 1, rather than guessing - the review's own "reject
      mixed/unknown states" ask.

      **Verified locally against the real vendored files, not a synthetic fixture** (no hardware
      needed - this is a pure Python/file-content fix): backed up the real, fully-patched
      `mmhalow.c`/`.h`, then exercised all four states directly against them - (1) fully patched:
      unchanged, "nothing to do"; (2) fully unpatched (both files reverted to their pre-patch form):
      applies cleanly, byte-identical to the known-good patched state afterward; (3) **the actual bug
      scenario** - `halow_transmit()` patched, `mmhalow_wifi_start()` reverted to its pre-patch form
      alone - confirmed the *old* logic (checked directly, not assumed) would have seen the marker
      from the first replacement and silently accepted the file as fully patched, leaving
      `mmhalow_wifi_start()` broken; the *new* logic correctly detected and completed exactly that
      one missing replacement, byte-identical to the known-good state afterward; (4) a deliberately
      unrecognizable edit to `mmhalow_wifi_start()`: correctly failed loudly (exit 1) rather than
      silently accepting or guessing. Real vendored files restored exactly (diffed byte-for-byte
      against the pre-test backup) before a real `idf.py build` confirmed zero errors/warnings and an
      unchanged binary size - only the Python script changed, no C source did.

      **Follow-up CI-hardening pass, same session, `.github/workflows/build-firmware.yml`** - the
      review's remaining "Implement" items for this finding, weighed and mostly landed:

      1. **Every third-party action was pinned to a mutable version tag** (`actions/checkout@v4`,
         `espressif/esp-idf-ci-action@v1`, etc.) - a tag can be silently moved to point at different
         code, the exact supply-chain risk the review names ("CI pins IDF 5.5.1 but action tags
         remain mutable"). Worse than assumed for one of them: `espressif/esp-idf-ci-action@v1`
         isn't a tag at all - `v1.0.0`/`v1.1.0`/`v1.2.0` exist as tags, but `v1` itself is a
         *branch*, more routinely force-moved than a tag ever should be. Fixed by resolving each
         `@vN` to its actual current commit SHA via the GitHub API and pinning to that, with the
         version kept as a trailing comment for readability - confirmed for every action used
         (`actions/checkout`, `actions/cache`, `actions/upload-artifact`,
         `actions/download-artifact`, `actions/upload-pages-artifact`, `actions/deploy-pages`,
         `espressif/esp-idf-ci-action`), not assumed to already be safe.
      2. **Untrusted ref interpolation, the exact pattern the review names** - `REF_NAME="${{
         github.ref_name }}"` inside a `run:` shell block. GitHub template-substitutes `${{ }}`
         expressions directly into the script's *text* before the shell ever runs it, so a
         crafted branch/tag name becomes part of the command line, not a quoted value - the
         standard GitHub Actions script-injection pattern. Fixed by reading
         `$GITHUB_REF_NAME`/`$GITHUB_REF_TYPE` instead - environment variables GitHub already sets
         for every step, so the shell only ever sees the value as data, never as script text.
      3. **Deployment permissions were workflow-wide** (`pages: write`/`id-token: write` inherited
         by every job, including `build`, which never touches Pages) instead of scoped to the job
         that actually needs them. Fixed by moving those two to just the `deploy` job -
         `actions/upload-pages-artifact` (used by `assemble`) just packages a regular workflow
         artifact under a reserved name and needs no elevated permission; only
         `actions/deploy-pages` calls the actual Pages API.
      4. **Build provenance**: the manifest already recorded the git SHA/date/title; extended it
         with `idfVersion`/`target` (read from one new job-level `env:` block that also now backs
         the `esp_idf_version`/`target` action inputs and the ccache keys, replacing three
         previously-independent hardcoded copies of the same two strings) and
         `halowComponentHash` (read directly out of `dependencies.lock`, not re-derived - the exact
         vendored `morsemicro/halow` build this binary was compiled against). `dirty: false` is
         recorded explicitly too, since a CI build is always from a clean checkout, unlike a local
         `idf.py build`'s `-dirty` suffix.

      **Verified without needing a CI run**: the workflow YAML parses (`python3 -c "import
      yaml; yaml.safe_load(...)"`), and the entire "Assemble manifest + binaries" shell block -
      the one with real logic, not just `uses:` lines - was dry-run locally against this session's
      actual build artifacts and real git history (both the default-branch and the tag-release code
      paths), producing a correct `current-build.json`/`manifest.json` with the real commit SHA,
      title, and the real `halowComponentHash` pulled from `dependencies.lock`.

      **Time-based pinning added, 2026-09-11** (`.github/dependabot.yml`, new): SHA-pinning alone
      doesn't defend against adopting a *freshly published* malicious release the moment it lands -
      most supply-chain compromises are caught and pulled within days once the community notices, so
      a deliberate delay before a newly-published version is even proposed is cheap insurance against
      exactly that window. `cooldown.default-days: 14` on the `github-actions` ecosystem (confirmed
      against the authoritative schema, `schemastore.org/dependabot-2.0.json` - the field only exists
      as documented there, not assumed from memory) means Dependabot won't open a PR bumping to a new
      action release until it's been public for two weeks. Scoped to `github-actions` only - it's the
      one dependency surface here Dependabot actually understands; the ESP-IDF version and the
      vendored `morsemicro/halow` component are both managed by ESP-IDF's own component manager,
      which Dependabot doesn't recognize as an ecosystem, so the same wait-before-bumping discipline
      for those two has to stay a documented manual policy rather than automated - noted in this
      item's own comment for whoever bumps either by hand.

      **PlatformIO tested, confirmed incompatible, 2026-09-11** - not "left open," a settled no: a
      `.pio/` directory in this repo shows it was actually tried, and it doesn't build against the
      vendored `morsemicro/halow` component as this project uses it. Dropped from this finding's scope
      entirely rather than carried forward as still-open work.

      **Left open, deliberately**: `mmhalow_wifi_start()` itself still discards
      `mmwlan_ap_enable()`'s return status (a C/firmware change, a different kind of fix than this
      script or its CI); and the `espressif/idf` Docker image used for the actual build is pinned by
      version tag inside `esp-idf-ci-action`, not independently re-pinned to a digest here (would
      need to track that action's own image-selection logic to pin correctly rather than duplicate
      its version string).

- [x] Automated regression suite, host-side (2026-09-11) — P2 — review finding F16's last major open
      item: "no maintained automated firmware regression suite was found." **Scoped to pure logic
      that can run on a development machine, not hardware-in-the-loop** - GitHub Actions has no
      physical XIAO boards, so a CI-runnable suite has to be the pieces that don't need one. Two
      pieces landed, both real - not scaffolding for a suite to fill in later:

      1. **`main/net_validate.c`/`.h` (new)**: `validate_host_subnet()`, `subnets_overlap()` (both
         previously `static` inside `provisioning.c`, review finding F10) and
         `provisioning_parse_security()` (previously also in `provisioning.c`, already public via
         `provisioning.h`) extracted verbatim - same logic, same comments, same citations - into
         their own file with zero ESP-IDF dependency beyond `esp_err_t` itself, which is shimmed to
         match `esp_err.h`'s real values (`#ifdef ESP_PLATFORM`, the macro ESP-IDF's own build
         system already defines) when building off-target. `gw_config.h` already had no ESP-IDF
         dependency of its own (checked, not assumed), so pulling in `gw_security_mode_t` didn't
         reintroduce the thing this extraction exists to avoid. `provisioning.c` is otherwise
         unchanged apart from the two now-relocated functions and one new `#include` - still the
         file that owns `provisioning_validate()` and everything NVS/console/HTTP-shaped, which
         stays genuinely untestable off-target without far heavier stubbing than this narrow
         extraction needed.

         `main/host_tests/test_net_validate.c`: 23 cases, plain C, no test-framework dependency
         (a name, a boolean, and a running pass/fail count - matches this project's own
         "don't add a dependency for something this small" instinct). Directly encodes the review
         finding F10 bug as a named, permanent regression case ("saee" (typo) must never again
         silently mean open) and this project's own real, shipped default subnets (SoftAP
         172.16.50.0/24 vs HaLow AP 172.16.60.0/24, already recorded elsewhere in this file as
         "correctly non-overlapping on real hardware") as a `subnets_overlap()` case - a false
         positive there would make every relay unconfigurable at boot, not just fail a synthetic
         edge case.

      2. **`main/host_tests/test_patch_vendored_halow.py` (new)**: formalizes the exact manual
         verification this session already did for F16's `patch_vendored_halow.py` fix (fully
         patched/unpatched/partially-patched/unrecognized, four cases) into a real, saved,
         `unittest`-based suite instead of a one-off that would have been lost. Uses temp-file
         fixtures, not the real (gitignored, network-fetched) `managed_components/` tree, so it
         doesn't depend on a full component-manager fetch just to run.

      **Both suites proved they have real teeth, not just that they pass**: deliberately
      reintroduced the exact F10 bug into a scratch copy of `net_validate.c` (unrecognized security
      strings silently returning "open" again) and confirmed 4 of 23 cases correctly failed; then
      reverted `patch_vendored_halow.py` to its pre-fix, single-whole-file-marker logic and confirmed
      2 of 5 Python cases correctly failed (one of them via an *uncaught* `SystemExit` - the old code
      couldn't even reach the "reject unknown state" path in that scenario, its own separate bug).
      Both real files restored byte-for-byte afterward, confirmed via `diff`.

      **Wired into a new, separate, fast CI workflow** (`.github/workflows/host-tests.yml`) rather
      than an extra job in `build-firmware.yml` - this needs nothing but `gcc` and `python3`, no
      ESP-IDF Docker image and no ~10 minute build, so it reports pass/fail independently rather than
      being bundled behind the slow build's own status. `net_validate.c`'s host compile uses
      `-Wall -Wextra -Werror`, matching this project's zero-warnings bar for the real firmware build
      - it's compiled both ways (`ESP_PLATFORM` defined for the target, undefined here), so a warning
      that only shows up in one of the two is exactly what this is for catching.

      **Verified on real hardware too, not just the host suite** (the production code moved, so this
      needed the same discipline as any other firmware change this session): a real `idf.py build`
      (zero errors/warnings, binary size effectively unchanged) followed by flashing a physical node
      and exercising both moved functions live over the console - `gwcfg-set-uplink ... saee`
      correctly rejected ("security mode 'saee' not recognized"), and
      `gwcfg-set-uplink-static-ip 172.16.60.0 ...` (a real network address, not a usable host)
      correctly rejected, with a valid address accepted right after - matching the host suite's
      results exactly, not just building cleanly.

      **Left open, deliberately**: hardware-in-the-loop testing (would need self-hosted runners
      wired to real boards - a different, larger kind of infrastructure than this) and extending
      coverage to the other pure-logic candidates noted but not pulled in this pass - `dns_forward.c`'s
      question-parsing/truncation logic (F04/F05) and `cot_relay.c`'s destination-group check (F09)
      are the next-best candidates by the same "pure logic, real bug already found here" bar this
      pass used to choose its first two.

### Stage F — measured optimization (needs A–D)
- [x] Cache radio/status into one supervisor snapshot (removes concurrent driver calls from HTTP/timer paths) -
      **landed 2026-09-11**. `link_history.c` already ran a 30s periodic timer sampling uplink RSSI into the
      history ring (`sample_uplink_rssi()`); it now also latches the two raw per-source readings
      (`s_latest_halow_rssi`/`s_latest_wifi_rssi`) from that same sample into two new getters,
      `link_history_get_latest_halow_rssi()`/`_wifi_rssi()` (`main/link_history.h`/`.c`). `gwcfg-status`
      (`provisioning.c`) and `/api/status` (`web_ui.c`) - previously each calling `uplink_halow_get_rssi()`/
      `uplink_wifi_get_rssi()` live, competing with each other and with `link_history.c`'s own timer for the
      single-owner `radio_control` task on every request - now read the cached values instead, at the cost of
      up to 30s staleness (the same tradeoff the history ring itself already makes; this is a status
      *display*, not a control-loop input). Verified on real hardware: `idf.py build` clean (zero
      errors/warnings), flashed to the relay node, `gwcfg-status` over the console showed the expected
      first-sample staleness immediately after boot (`wifi RSSI: (not associated)` while `wifi uplink: up`),
      then the correct live value (`-64 dBm`, later `-66 dBm`) once the first periodic sample landed 30s
      later - confirming the cache populates and updates, not just compiles.
- [x] Make scans asynchronous and bounded (job-ID pattern instead of blocking the HTTP task) -
      **landed 2026-09-11**. `uplink_halow_scan()` used to block its caller for the scan's full
      duration (`WEB_UI_SCAN_TIMEOUT_MS` = 8s in `web_ui.c`) - and since esp_http_server serves one
      request at a time on this project's single httpd task, that meant every other request,
      including a page just polling `/api/status`, stalled for the same 8s. Split into
      `uplink_halow_scan_start()`/`_poll()`/`_get_cached_results()` (`main/uplink_halow.c`/`.h`):
      start submits the scan and returns immediately (bounded only by the existing
      `RADIO_CONTROL_DEFAULT_TIMEOUT_MS` = 3s every other radio call already accepts blocking on);
      poll is a non-blocking check (a couple of non-blocking semaphore takes and a timestamp
      compare, never a `radio_control_run()`) that finalizes the scan into a small result cache once
      it completes or times out. There's only ever one scan job globally (the radio only supports
      one at a time, same constraint the old `ESP_ERR_INVALID_STATE` already enforced), so there's
      no numeric job ID to track - "is a scan running right now" is the whole of the job's state.
      `web_ui.c`'s `POST /api/scan` now only starts the job; a new `GET /api/scan` polls it, and
      `web_ui.html`'s scan button polls that endpoint (500ms interval, 30-attempt/15s client-side
      backstop - server-side the finalize deadline is still `WEB_UI_SCAN_TIMEOUT_MS`) instead of
      waiting on one long request. `uplink_halow_scan()` itself stays (built on the same
      start/poll pair internally) since it's still the right shape for `gwcfg-scan`, a synchronous
      console command. Verified on real hardware: `idf.py build` clean (zero errors/warnings),
      flashed to both bench boards, `gwcfg-scan` over the console on the client node found the
      relay's HaLow AP (`xiao-relay-1`, two BSS entries at 914.5/915.0 MHz) exactly as before -
      confirming the rebuilt blocking wrapper still delivers correct results end-to-end through the
      new async primitives, not just that it compiles. The HTTP polling path itself
      (`POST`/`GET /api/scan`) wasn't exercised live in this session - it's a thin JSON wrapper
      around the same `uplink_halow_scan_start()`/`_poll()`/`_get_cached_results()` calls the console
      path already proved, but reaching it from a browser needs a device associated to the SoftAP,
      not available in this environment.
- [ ] Profile before touching the 40 MHz SPI clock or NAPT table size
- [ ] Pick and document an explicit Wi-Fi power-save policy, measured
- [ ] Station-limit testing at 1/2/4 leaves before any "supports N clients" claim
- [ ] Evaluate a supported AP-netif adapter before writing a DHCP server from scratch

Regression suite T01–T12 and the acceptance targets in the review's §8 are the durable test list —
add them as real tests alongside each stage's fixes, not as a follow-up pass.

## Not built yet

Only one hard dependency exists in this list, and it's a security one: **authentication must land
before any firmware-upload path.** Everything else can be done in any order — let hardware testing
decide. Nothing here should start before the checklist above passes; features built against an
unproven link get debugged twice.

### 1. Web UI authentication — **implemented and confirmed on real hardware 2026-08-30**

Association with the SoftAP used to be the only credential — subnet-based *authorization*, not
authentication, so a device already on the SoftAP could do everything the UI could. Landed as a
challenge-response scheme per the settled decisions below:

- **`gw_config_t` gained `gw_auth_config_t auth`** (`password_set`, `salt[16]`, `iterations`,
  `stored_key[32]`) — `GW_CONFIG_VERSION` bumped `5u -> 6u`.
- **PBKDF2-HMAC-SHA256 runs client-side only**, in a ~2KB bundled JS crypto block in
  `web_ui.html` (SHA-256 core adapted from Chris Veness's public-domain reference
  implementation, verified against NIST/RFC 4231 test vectors and cross-checked against Python's
  `hashlib.pbkdf2_hmac` before use) — the device only ever verifies one `mbedtls_md_hmac()` call
  against a stored key, never derives one, keeping the 100,000-round KDF off the ESP32 entirely.
- **Six new endpoints** (`main/web_ui.c`): `GET /api/auth/status`, `GET /api/auth/challenge`,
  `POST /api/auth/login`, `POST /api/auth/logout`, `GET /api/auth/new-salt`,
  `POST /api/auth/password` (the last two double as the first-use password-set flow and, once a
  password exists, the change-password flow). All nine existing functional handlers gained a
  third guard, `auth_require_session()`, alongside the unchanged `reject_if_remote()` and
  `reject_if_not_json()` — auth is additive, not a replacement, exactly as planned.
- **Sessions**: RAM-only, 4-slot table, `HttpOnly`/`SameSite=Strict` cookie, 30-minute sliding
  idle timeout, in a new `main/auth.c`/`auth.h` with zero dependency on `esp_http_server.h` (same
  split as `chip_temp.c`/`link_history.c`) — `web_ui.c` does all cookie/HTTP glue.
- **Lockout**: 5 failures locks for 30s, doubling per subsequent failed window, capped at 5
  minutes.
- **Recovery**: `gwcfg-reset-auth` console command, saves and drops all sessions immediately (no
  reboot needed) — deliberately diverges from `gwcfg-reset`'s edit-then-`gwcfg-save` convention
  since this is a locked-out operator's recovery path, not a routine edit. BOOT-button factory
  reset clears the credential for free (already zeroes the whole config).
- **`main/CMakeLists.txt`** gained `mbedtls` in `PRIV_REQUIRES` — it was *not* previously a
  dependency of `main` (confirmed by reading the file directly), correcting this doc's own earlier
  "mbedtls is already linked" framing, which was only ever true of the ESP-IDF build as a whole.
- **`GW_STACK_WEB_UI`** raised `6144 -> 7168` proactively (`main/task_stats.h`) — the existing
  stack-budget measurement below already showed this task at ~74% used before any auth code
  existed.
- Two corrections against actual ESP-IDF v5.5.1 source, caught before they became compile errors
  or shipped a body-less error response: `httpd_err_code_t` has no 429 or 409 member (the same
  gap this project already documented for `HTTPD_503`/`HTTPD_409`), and `httpd_resp_send_err()`
  always sends an HTML-wrapped body, never raw JSON — both auth error paths that need a structured
  JSON body use `httpd_resp_set_status()` + a real cJSON tree instead.

**Confirmed on real hardware 2026-08-30** (GW_ROLE_RELAY unit, driven directly over HTTP with the
same PBKDF2/HMAC-SHA256 crypto the browser bundle uses, independently re-verified in Node against
NIST/RFC 4231 vectors before being trusted to drive a real login):

- First-use forced password-set: `password_set:false` on a freshly reset config, `GET
  /api/status` (and every other functional endpoint) 401s with no session; `GET
  /api/auth/new-salt` -> client-side PBKDF2 -> `POST /api/auth/password` lands directly in an
  authenticated session, no second login needed.
- Login + session gating: a valid session cookie makes every endpoint work; the identical request
  with no cookie gets a clean 401.
- **Stack headroom under real exercised load, not idle**: after a login, a password-set/change,
  and a full-field `POST /api/config` in the same run, `GET /api/tasks` showed the `httpd` task at
  **2524/7168 bytes free (35.2%)** — *better* headroom than the pre-auth baseline (26% free at
  6144 bytes), confirming the proactive `GW_STACK_WEB_UI` raise was the right amount, not just an
  untested guess.
- Heap stable across the whole exercise: ~144-147KB free before and after, no drop suggesting a
  leak from session/auth-state allocation.
- Lockout: 5 wrong login attempts, the 6th returns `{"error":"locked_out","retry_after_s":30}`
  from *both* `/api/auth/challenge` and `/api/auth/login` without even checking the submitted
  response, exactly as designed. An existing session's own requests are unaffected by an active
  lockout (confirmed by hitting `/api/status` with a valid cookie while locked out).
- `gwcfg-reset-auth`: dropped the live session immediately (next request with the old cookie
  401s) with no reboot, and confirmed it clears *only* `auth` — role, Wi-Fi uplink, HaLow AP
  config and the running CoT relay were all still up and untouched afterward.

**Still open, needs a phone in hand rather than a script**: PBKDF2 timing on a **real low/mid-end
mobile browser** (100,000 iterations measured at ~600ms in Node's JIT on the dev machine used to
build this - a real mobile browser, especially an older one, will be slower, and this is the one
number in the whole design that's a judgment call rather than a verified fact); the 30-minute
session idle timeout (not practical to wait out during this test); and the lockout's doubling
schedule beyond the first 30s window (60/120/240/300s across repeated windows - only the initial
trigger was exercised here).

### 2. OTA update delivery — *blocked on 1*

The partition layout is already dual-OTA (two 3 MB slots + `otadata`, ~1.81 MB unallocated on 8 MB
flash). That half was done immediately because retrofitting a partition table onto deployed units
costs a USB cable per unit — exactly the cost OTA exists to remove.

Delivery is deferred deliberately. An OTA endpoint means anyone who can reach the web UI can
replace the firmware; promoting "can change my config" to "can replace my firmware" on an
unauthenticated endpoint is an escalation, not an increment.

In order:

1. Authentication (item 1). Nothing else ships first.
2. An update path — `esp_https_ota` from a known URL, or an authenticated upload endpoint.
3. `esp_ota_mark_app_valid_cancel_rollback()` in the app, **then** enable
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Turning rollback on before that call exists means every
   boot reverts.
4. A real end-to-end test on hardware, **including a deliberately bad image**, before relying on it.

`/api/status` already reports the running partition — meaningless today (always `ota_0`) and the
first thing you'll want once this exists.

### 3. Self-beacon — the node's own CoT

`cot_relay_inject()` is already the generic send primitive, and the relay's own-source drop already
covers injected traffic. What's missing is a producer. Without it the gateway is **invisible on
other people's ATAK screens** — it relays everyone else's situational awareness and contributes
none of its own.

**Scope decision to make.** Pi nodes get position from gpsd; this hardware has no GPS. Two options,
not exclusive:

- **Static/configured position** — a lat/lon in `gw_config_t`, set when the node is placed. Cheap,
  no hardware, genuinely useful for a node on a fixed mast.
- **Real GPS** — a UART module (ATGM336H or similar) on D6/D7 (GPIO 43/44), with D5 (GPIO 6) left
  for PPS. Confirmed to fit; see "What's left for expansion" in [`HARDWARE.md`](HARDWARE.md).
  Better for a mobile node, but new hardware, new config, new failure mode.

Either way the beacon should carry what only this node knows: uplink state, RSSI, client count,
uptime, and battery once item 6 exists. That data is already behind `/api/status`.

CoT is XML; budget a few KB whether you string-build or pull in a formatter.

### 4. Captive portal and mDNS

Today a user must *know* to browse to `172.16.50.1`.

- **mDNS** (`xiao-gw.local`) — small, ESP-IDF ships the component. Works well on iOS/macOS,
  unevenly on Android.
- **Captive portal DNS** — a DNS responder on the SoftAP answering every query with the node's own
  address, so joining the Wi-Fi pops the config page. This is the one that actually solves it, on
  every platform, and it's the standard ESP-IDF pattern.

Worth doing *after* auth: a captive portal landing on a login screen is a much better first-run
experience than one landing on an open config form.

### 5. Multi-group CoT relay

The relay handles exactly one group/port. Some ATAK deployments use additional groups.
`gw_cot_config_t` becomes a short array, `cot_relay_start()` joins each, arrival-interface logic is
unchanged, bump `GW_CONFIG_VERSION`. Do it only if a real deployment needs it — speculative
generality costs config complexity for every user.

### 6. Power management

`DESIGN`-era notes called this "a battery/portable-power node" and the firmware does nothing about
it: no light sleep, no duty cycling, no battery voltage sensing.

**Do not guess before hardware.** Measure first: idle draw with the radio associated but idle; draw
with SoftAP clients attached; whether the MM6108's own power-save modes are usable given the relay
needs to receive multicast promptly. *Then* battery voltage on an ADC pin, reported via
`/api/status` and the beacon, and a considered decision about sleep.

**Two wake-ups are already identified and deliberately unchanged** — recorded so the pass starts
from them instead of re-deriving them (review finding, 2026-08-21):

- **`status_led` wakes at 8 Hz.** `TICK_MS 125` in `main/status_led.c` is the tick the blink
  patterns are built on. It can't just be slowed: the task has to wake to toggle the pin at all, so
  the saving is pattern-shaped (a steady-on or steady-off state needs no tick; a blink does), not a
  constant to raise.
- **`factory_reset` polls the BOOT button at 10 Hz.** `POLL_INTERVAL_MS 100` in
  `main/factory_reset.c`, held time counted in whole poll intervals. An ISR-driven button would
  idle at zero wake-ups, but it puts an interrupt path in front of the one recovery mechanism that
  has to work when everything else is broken — a trade worth making only against a number.

Both were left alone on purpose. Neither is free to change, and nobody has yet measured what either
costs next to a radio that can't use power-save at all (`CONFIG_HALOW_PS_MODE=n` — see "Settled
decisions"). They are two of the first things to put a meter on, not two things to change first.

The same argument applies to throughput. If TCP-through-NAT disappoints on hardware, the knobs are
`CONFIG_LWIP_TCP_WND_DEFAULT` / `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` and the Wi-Fi buffer counts — but
measure first. The MM6108 link over SPI is the ceiling (single-digit Mbps at best, far less at
range) and the S3's lwIP NAT path handles that with headroom.

### 7. XIAO as a real 802.11s mesh point — *v2 scope, not this gateway's, needs Morse Micro*

Raised 2026-08-16 as a fallback if `PI_SIDE.md` item 0 (getting the Pi to beacon a HaLow AP at all)
turns out to be a dead end. **This is not a small addition to the current design — it's a separate,
much larger undertaking**, and most of the hard part isn't ours to build. Two independent gaps stack
on top of each other:

**Gap 1 — 802.11s mesh association itself.** Read directly from Morse Micro's own source
(`github.com/MorseMicro/esp-halow`, `github.com/MorseMicro/mm-iot-sdk`, fetched 2026-08-16):
`esp-halow`'s `hostap` component (`halow/components/hostap/CMakeLists.txt`) vendors a real fork of
upstream `wpa_supplicant`/`hostapd` (file names match hostap.git exactly), and builds genuine STA
code plus genuine AP code gated behind `CONFIG_HALOW_AP_MODE` (`hostapd.c`, `beacon.c`,
`ap_mlme.c`, `ieee802_11_s1g.c`, `NEED_AP_MLME` — the real thing, not a stub; this is what backs
the AP-mode support `HARDWARE.md` already notes exists on ESP32-C5). **`CONFIG_MESH` is never
defined and none of `mesh.c`/`mesh_mpm.c`/`mesh_rsn.c` are in the build** — but those exact files
**do exist**, unused, in the vendored source tree
(`mm-iot-sdk/framework/src/hostap/wpa_supplicant/mesh*.c`). So the gap isn't "Morse never touched
mesh code" — it's "the mesh code was vendored along with everything else and never wired into the
ESP32 build or given a driver to run on."
  That driver is the real blocker. `drivers_morse.c` registers exactly two `wpa_driver_ops` tables
  — `mmwlan_wpas_ops` (STA) and `mmwlan_wpas_ops_ap` (AP) — both `extern`, both "implemented by
  morselib." **`morselib` is Morse Micro's closed-source static library**; nothing in the public
  repos shows what it actually implements. Whether a hypothetical `mmwlan_wpas_ops_mesh` is
  feasible depends on primitives (peer-specific keys, self-protected action frames, mesh beaconing)
  that only Morse Micro can see or add. **We cannot build this ourselves from outside their SDK.**
  The one strong reason to think it's tractable *for them*: the identical silicon (MM6108, and
  specifically this project's exact FGH100M-H part) already runs 802.11s mesh point mode today,
  unmodified, on the Pi side via Linux's `hostapd_s1g`/`wpa_supplicant_s1g` — so this is a host-SDK
  porting gap, not a chip/RF capability wall. That's the pitch worth taking to Morse Micro: they've
  already vendored the mesh source and already proved their AP-mode driver ops can do STA/peer
  management; finishing the mesh port is a bounded ask, not "invent mesh support."
  (Note: an earlier PI_SIDE.md line reading "ESP32 HaLow cannot do STA-to-STA direct links,
  confirmed by Morse's own team" predates this finding and its original context wasn't recovered
  from this repo's history. It may have meant exactly this — not supported *today* — rather than
  "architecturally impossible." Worth re-asking Morse directly with this level of specificity
  before assuming it's a closed door.)

**Gap 2 — batman-adv, or an equivalent, doesn't exist for FreeRTOS/lwIP at all.** Even a fully
working 802.11s mesh association only gets the XIAO to "one MAC-layer peer." OpenMANET's actual
mesh behaviour — the flat L2 domain, multi-hop routing, loop prevention — comes from batman-adv
running on top of the 802.11s interfaces (see "Settled decisions" below, already established). That
is Linux kernel networking code with no FreeRTOS/lwIP counterpart to build from; porting or
reimplementing it is new work with no existing scaffolding, and plausibly a bigger lift than gap 1.
Skipping it and just associating at 802.11s without batman-adv is not a safe substitute — a plain
mesh peer with no OGM exchange sitting next to batman-adv-speaking neighbors is undefined behaviour
this project has not tested, not a known-working "leaf mesh point" mode.

**Where this leaves the plan**: `PI_SIDE.md` item 0 (a Pi-side AP-mode config test) is orders of
magnitude cheaper than this and should be exhausted first — it needs no new code anywhere. This item
is the real fallback, and starts with a conversation with Morse Micro about gap 1, not firmware work
in this repo. Gap 2 needs a considered scoping pass of its own before any code gets written, even if
Morse Micro says yes to gap 1.

### 8. GW_ROLE_RELAY — built 2026-08-16, first hardware run 2026-08-17, **partially confirmed**

Fallback for the worst case on both item 0 (Pi has no HaLow AP mode) and item 7 (Morse Micro won't
add mesh support): route around HaLow entirely for the Pi-facing hop, using only things already
confirmed to exist - `esp-halow`'s real `CONFIG_HALOW_AP_MODE` support (see item 7), the ESP32-S3's
own native 2.4 GHz Wi-Fi (fully separate radio from the HaLow module), and OpenMANET's
already-documented, always-on local AP (`br-ahwlan`, bridged into `bat0`) that every mesh point
exposes with its own DHCP. **Needs zero changes on the Pi or from Morse Micro.**

Landed as a second node **role**, selectable at runtime (`gwcfg-set-role client|relay`, or the web
UI's Node → Role field) - one firmware image serves both, not a separate build:

- **GW_ROLE_CLIENT** is unchanged - today's original design, still the default.
- **GW_ROLE_RELAY** is new: `main/uplink_wifi.c` (native `esp_wifi` STA joining the Pi's local AP,
  event-driven reconnect against standard ESP-IDF STA events - the validation step this project
  needed anyway, since nothing past `gwcfg-scan` had run against a real OpenMANET mesh) plus
  `main/downlink_halow_ap.c` (HaLow radio in AP mode via `mmhalow_set_config(WIFI_IF_AP, ...)` +
  `mmhalow_wifi_start()`, static IP rather than DHCP - see that file's header comment for why
  `esp_netif`'s DHCP server can't be attached to the netif `mmhalow_init()` creates). A
  GW_ROLE_CLIENT leaf needs zero firmware changes to associate to a relay instead of a Pi - it's the
  same `uplink_halow.c`, just pointed at the relay's SSID, with an optional static-IP override
  (`gwcfg-set-uplink-static-ip`) since the relay's HaLow AP doesn't hand out leases. One relay can
  serve multiple leaf XIAOs (HaLow AP mode is multi-client like any AP), and a field-deployed leaf
  keeps HaLow's actual long range to reach the relay - only the relay-to-Pi hop is short-range Wi-Fi,
  which only has to cover "near the Pi."

`gwcfg-set-wifi-uplink` and `gwcfg-set-halow-ap` (plus `gwcfg-list-halow-channels`, since
`mmwlan_ap_args` wants an (op_class, s1g_chan_num) pair with no obvious mapping to a frequency an
operator actually has) configure the two relay-only radios; `gwcfg-status`, `/api/config` and
`/api/status` all became role-aware. `GW_CONFIG_VERSION` bumped to 4 for the new fields - reprovision
after updating, per the usual policy.

**Confirmed by build, not yet by hardware**: `idf.py build` passes clean against ESP-IDF v5.5.1 with
`CONFIG_HALOW_AP_MODE=y` now always on (binary grew from ~1.67 MB/44% free to ~1.89 MB/37% free -
built into every image since role is a runtime choice, not a build-time one). Both figures are
pre-`-Os`; the slot is back to 42% free since - see "Status at a glance". What building does
*not* prove: Morse Micro's own header marks `mmwlan_ap_args`/`mmwlan_ap_enable()` "ALPHA NOTICE:
under development; breaking changes may be introduced," and while ESP32-S3+MM6108 is in
`esp-halow`'s tested-hardware table, its README doesn't break testing out by mode - AP mode
specifically on this exact chip pairing isn't proven on real hardware the way STA mode already is.
Budget bring-up time for GW_ROLE_RELAY like any other untested path in this project (see
`design/HARDWARE.md`'s runbook pattern) before trusting it in the field. First things to check on
real hardware. Procedure and pass/fail criteria for each is in
[`HARDWARE.md`](HARDWARE.md) Part 3 - same relationship as the main checklist above has to
`HARDWARE.md` Part 2: this is the tracker, that is the runbook.

- [x] **Tier 0** - does `mmhalow_wifi_start()` actually bring the HaLow AP up on air at all (it
      returns `void`, so `downlink_halow_ap_is_started()` only means "we called it," not
      confirmation)? Two XIAOs, no Pi, no Wi-Fi network needed - the cheapest, most isolated check
      of the alpha AP-mode API by itself. **Passed, confirmed on real hardware 2026-08-30** — see
      "Sixth Aug 30 finding" below: a leaf's `gwcfg-scan` finds the relay's HaLow AP with a real
      RSSI and frequency, repeated at two different bandwidths.
- [x] **Tier 1** - does a leaf's `gwcfg-set-uplink` against that AP actually associate, and does the
      static-IP-on-both-ends addressing scheme (no DHCP server on a relay's HaLow AP - see
      `main/downlink_halow_ap.h`) actually pass traffic once associated? **Note this could not have
      passed before**: `use_static_ip` was stored and validated but never applied to the netif, so
      the leaf ran a DHCP client against an AP with no DHCP server. Fixed — `apply_static_ip()` in
      `main/uplink_halow.c`, which is also where the derivation lives. **Passed, confirmed on real
      hardware 2026-08-30**: a leaf given a static IP with `gwcfg-set-uplink-static-ip` reassociates
      and reaches `uplink state: up`, and shows up on the relay's own `halow ap stas` count — see
      "Fifth" and "Sixth Aug 30 finding" below.
- [ ] **Tier 2** - the full chain against *any* ordinary Wi-Fi network on the relay's uplink side
      (still no Pi needed) - first real-hardware run of the NAT/DNS/CoT-relay pipeline at all.
      **Attempted 2026-08-17 against an ordinary WPA3-SAE home AP.** The uplink half passed and the
      datapath half found a bug; both are recorded under "What the first relay run proved" below.
      **Substantially advanced 2026-08-30**, not yet complete: the VIF-ID bug that dropped
      downlink-bound multicast/replies is root-caused and worked around (0/6 → 6/6 packets
      forwarded, verified), and DNS forwarding for statically-addressed leaves is implemented and
      confirmed to start/bind — see "Third" through "Fifth Aug 30 finding" below. **Still open**: an
      actual DNS query/response round-trip and real browsing from a phone behind a leaf, which needs
      a device on a leaf's own SoftAP that this project's dev machine has no network path to. Don't
      tick this box until that's confirmed.
- [ ] **Tier 3** - the actual target scenario: swap "any Wi-Fi network" for the Pi's own local AP.
      Only meaningful once item 0's AP-mode workaround is confirmed on the real Pi.

#### What the first relay run proved (2026-08-17)

A relay node configured for a 4 MHz HaLow AP (`op_class 3`, `s1g_chan_num 8`, SAE) with a WPA3-SAE
home AP as its Wi-Fi uplink. Confirmed on hardware, from that boot's serial log:

- **The two-radio netif split works.** `uplink_wifi.c` creates its STA netif under the custom if_key
  `WIFI_STA_NATIVE` precisely because `mmhalow_init()` has already claimed `WIFI_STA_DEF` for the
  HaLow radio. Both netifs coexisted with no duplicate-key panic in `esp_netif_new_api()` — the
  failure that comment was written to prevent. `esp_netif_create_wifi()` +
  `esp_wifi_set_default_wifi_sta_handlers()` is a working substitute for
  `esp_netif_create_default_wifi_sta()`.
- **The Wi-Fi uplink works end-to-end.** Associated to WPA3-SAE at -53 dBm, then took a DHCP lease
  (`sta_native ip: 192.168.50.128`) and raised its state callback. `uplink_wifi.c` is no longer an
  unproven path.
- **`downlink_halow_ap_init()` runs clean** through `mmhalow_wifi_start()` with no error, on top of
  a working SPI link to the MM6108 (version banner printed). This is *not* Tier 0: that call
  returns `void`, so this still only means "we called it," not that the AP is on air.

It also found a real bug, now fixed: **the datapath bring-up overflowed the event loop task's
stack** the moment the uplink got its lease, panicking the node into a reboot loop.
`bring_up_datapath()` (NAT + DNS + CoT relay) was being called straight from the uplink state
callback, which both uplink modules raise from inside an `esp_event` handler — so it ran on
`sys_evt`, whose stack is 2816 bytes total in this build and nowhere near enough. It now runs on its
own 4096-byte `datapath` task and the callback only posts a notification. The full derivation, with
upstream citations, is on `datapath_task()` in `main/app_main.c`.

Two things worth carrying forward from that:

- **This was latent in GW_ROLE_CLIENT too**, not a relay-only bug — that role calls the same
  `bring_up_datapath()` from the same kind of handler in `uplink_halow.c`. It surfaced on the relay
  first only because the relay is what got run. Any new uplink state callback must stay short; the
  typedefs in `uplink_halow.h`/`uplink_wifi.h` now say so.
- **The relay's downlink has no DHCP server, and `ip_forward_nat.c` now checks before assuming
  one.** `mmhalow_init()` builds its netif from `ESP_NETIF_DEFAULT_WIFI_STA()`, so `esp_netif`
  never allocates a `dhcps` handle for it and every DHCP-server call against it fails by
  construction. That is correct — leaf nodes on this hop are statically addressed by design — but
  it was producing two `ESP_LOGE` lines per relay boot about a server that was never meant to
  exist. DNS propagation is now skipped, with a log line saying why, when the downlink netif lacks
  `ESP_NETIF_DHCP_SERVER`.

Both the client and relay roles keep every known HaLow constraint from `PI_SIDE.md`: security must
be `open`/`owe`/`sae` (no PSK), region is fixed `US`, and phones behind any leaf XIAO stay NAT'd
(`172.16.50.x`, not
`10.41.x.x`) exactly as today — L2 bridging is still off the table on ESP32/lwIP regardless of which
radio carries the uplink.

#### What the Aug 22 relay run proved

Two more hardware runs, both in GW_ROLE_RELAY, surfaced two further bugs — now fixed, not yet
re-verified on hardware:

- **A config-save reboot into GW_ROLE_RELAY could crash-loop before the HaLow AP ever came up.**
  Reproduced twice: after saving config from the web UI (which reboots via `esp_restart()`),
  `downlink_halow_ap_init()` hung inside `mmhalow_init()`'s `mmhal_init()`/`mmwlan_init()` — before
  either logs anything — and tripped the interrupt watchdog in task `ipc0`, every time, saving a
  core dump and rebooting straight into the same crash again. Only a true hardware reset (the web
  flasher's own reset button, not this firmware — reset reason "USB peripheral") came up clean,
  both times; every `esp_restart()`-driven reboot (reset reason "software (esp_restart)" or the
  panic handler's own auto-reboot) failed identically. Root cause, read from the actual vendored
  source (`morsemicro/halow` 2.11.2-esp32-2's `mmhal_os.c`/`mmhal_wlan.c`, not memory):
  `mmhal_init()` drives `CONFIG_MM_RESET_N` low, and `mmwlan_boot()` later raises it high again via
  its own internal call to `mmhal_wlan_init()` — with no deliberate delay of its own in between.
  That is a valid reset from a cold boot, where the radio was never running. It is not necessarily
  valid after a software `esp_restart()`, which resets the ESP32's own core but not the radio module
  — so if the radio was still running from before the reboot (it can be; see `uplink_halow.c`'s "up
  and scannable" log line), the low-to-high flip may be too short to actually resync it. Fixed by
  `halow_radio_force_reset()` in `main/app_main.c`: an explicit, deliberate low pulse on
  `CONFIG_MM_RESET_N` before either role's bring-up ever calls into `mmhalow_init()`, on every boot,
  cold or warm.
- **The datapath bring-up could lose a race against the HaLow AP's own bring-up.** On the run where
  the boot-loop above didn't trigger, the Wi-Fi uplink got its DHCP lease ~50ms after `esp_wifi`
  came up — before the HaLow AP downlink netif had been marked up at the lwIP level.
  `esp_netif_napt_enable()` failed as a result (it "fails only if netif is down" — esp-idf
  `esp_netif_lwip.c` L2695/2701-2706 at v5.5.1, confirmed against that source) and
  `cot_relay_start()`'s `IP_ADD_MEMBERSHIP` failed right behind it for the same underlying reason.
  `bring_up_datapath()` only ever gated on the *uplink's* state callback, with nothing checking the
  downlink's own readiness. Fixed by `wait_for_downlink_up()` in `main/app_main.c`: a bounded poll
  (5s, 100ms steps) on `esp_netif_is_netif_up()` for the downlink netif before either NAT or the CoT
  relay touch it.

Neither fix had a confirmed clean hardware run at the time this was written. The boot-loop fix has
since been retested (2026-08-29, two physical nodes, both roles) and needed a second fix on top of
it - see "What the Aug 29 relay run proved" below. The datapath-race fix turned out to be papering
over a different bug entirely - `wait_for_downlink_up()`'s timeout was never going to be long
enough, because the downlink netif was never going to report up at all. See "What the Aug 30
datapath-race run proved" below for the real fix.

#### What the Aug 29 relay run proved

The boot-loop fix above (`halow_radio_force_reset()`) turned out to be necessary but not
sufficient: configuring a node as GW_ROLE_RELAY with a HaLow AP and rebooting from the web UI
reproduced the *same* interrupt-watchdog panic in task `ipc0`, 38 times in a row across a single
test, on two different physical nodes, surviving a factory reset and a full USB power cycle in
between. Symbolized with `idf.py coredump-info` this time (not just the raw backtrace addresses
from the panic handler): the crash is inside `gpio_install_isr_service()` itself -
`esp_intr_alloc() -> ... -> gpio_isr_loop()` - interrupted while installing the shared GPIO ISR,
with the crashing task's own PC landing back inside `gpio_isr_loop`. That is the shared dispatcher
spinning on a pending interrupt with no per-pin handler registered yet to service it - which happens
when a pin is already configured level-triggered and asserted the moment the shared ISR is enabled.

`managed_components/morsemicro__halow/components/shims/mmhal_wlan.c:237` configures exactly one pin
that way - `gpio_set_intr_type(CONFIG_MM_SPI_IRQ, GPIO_INTR_LOW_LEVEL)` - as part of normal AP-mode
bring-up. `halow_radio_force_reset()`'s pulse only resets the *radio chip* via `CONFIG_MM_RESET_N`;
it can't touch this, because it's a register on the ESP32-S3's own GPIO peripheral, not the chip's.
`esp_restart()` triggers what esp-idf's own source calls a "system reset" (`esp_system/esp_system.c`
`esp_restart_noos()`, via the RTC watchdog's `WDT_STAGE_ACTION_RESET_SYSTEM`) - and 38 identical
crashes on pure software reboots, no power cycle between any of them, confirm that reset does not
clear this particular register. So once one boot arms it, every following `esp_restart()` re-arms
the same storm before a handler exists to service it - a genuinely different mechanism from the
RESET_N timing issue the first fix targeted, not a sign that fix was wrong.

Fixed by extending `halow_radio_force_reset()` (`main/app_main.c`) with `gpio_reset_pin
(CONFIG_MM_SPI_IRQ)`, called before `mmhalow_init()` ever runs on any boot. `gpio_reset_pin()`
(esp-idf v5.5.1 `esp_driver_gpio/src/gpio.c:456`) calls `gpio_intr_disable()` first, which writes
`GPIO_INTR_DISABLE` into that same hardware field directly - clearing it regardless of what a
previous, possibly-crashed boot left armed. Confirmed clean on both physical nodes immediately
after: HaLow radio up (real chip ID and MAC, not the zeroed readback a dead SPI transport gives),
HaLow AP started, native Wi-Fi uplink associated and got a DHCP lease - the same
config-then-web-UI-reboot sequence that crash-looped every time before now boots straight through.

#### What the Aug 30 datapath-race run proved

`gwcfg-status` on a live, otherwise-healthy relay (Wi-Fi uplink up, HaLow AP started, a leaf
associated) kept reporting `cot relay: not started`, unrecovered because nothing about the design
retries once the uplink stays connected (see `datapath_task()`'s own comment on why it's
correct to only retry on reconnect). Root cause was not a timing issue at all, despite looking
like one: `wait_for_downlink_up()` polls `esp_netif_is_netif_up()`, which only becomes true once
something calls `esp_netif_action_connected()` on that netif - and for the HaLow AP downlink,
nothing ever did. `mmhalow_init()` only wires `mmwlan_register_link_state_cb()`
(`managed_components/morsemicro__halow/mmhalow.c` L215), and that callback's own doc comment in
`mmwlan.h` says outright: "This link status callback will not be invoked in AP mode." No timeout
value, however large, was ever going to fix this - the netif was never going to report up by that
path, cold boot or warm.

Fixed in `main/downlink_halow_ap.c` by registering `mmwlan_register_vif_state_cb(MMWLAN_VIF_AP, ...)`
instead - the non-deprecated replacement, which carries no such AP-mode exclusion in its doc
comment - and calling `esp_netif_action_connected()`/`_disconnected()` from it directly, mirroring
`mmhalow_link_state()`'s own call shape exactly. Confirmed on real hardware immediately after: the
downlink netif reported up (`sta_native ip: 172.16.60.1`) within ~2.8s of boot, well before the
Wi-Fi uplink even associated, and the boot proceeded straight through to `NAPT enabled on the
SoftAP interface, uplink is default route` and `CoT relay joined 239.2.3.1:6969 on both interfaces`
with no wait at all - the exact success sequence this file has been asking a relay test to produce
since the Aug 22 run.

One new, separate issue surfaced by finally reaching this code path: `mmwlan_tx_pkt` logs
`Unable to infer VIF ID` / `Packet failed to send` roughly every ~55s (first occurrence just before
CoT relay's own join log, so likely lwIP's periodic IGMP membership-report refresh rather than CoT
traffic itself) - a gap in how the vendored AP-mode driver resolves which station a multicast frame
should go to when the destination doesn't uniquely resolve to one. Not yet investigated further;
CoT relay itself starts and stays up regardless, so this doesn't block the fix above, but it may
mean some outbound multicast frames toward HaLow leaves are silently dropped periodically.

#### Third Aug 30 finding: confirmed - the VIF ID bug does drop real CoT traffic, not just IGMP

Tested directly using the new per-side counters (`cot_relay_get_counters()`, see "What's
implemented"): with no Pi available, a laptop on the same network as the relay's native Wi-Fi
uplink (home Wi-Fi, standing in for "the Pi's local AP" - functionally identical to that hop, since
`uplink_wifi.c` just joins whatever AP it's configured for) sent one 81-byte UDP multicast packet
directly to `239.2.3.1:6969`. `/api/status` before and after:

```
"uplink_side":   { "rx_packets": 1, "rx_bytes": 81, "tx_packets": 0, "tx_bytes": 0 }
"downlink_side": { "rx_packets": 0, "rx_bytes": 0,   "tx_packets": 0, "tx_bytes": 0 }
```

Received on the uplink side, **never forwarded** to the downlink (HaLow AP) side toward the leaf.
`/api/log` at the same moment shows exactly the expected failure chain, at the same timestamp as the
test:

```
E (220870) Morse Micro HaLow NetIF: Packet failed to send - 12
W (220870) cot_relay: sendto failed: errno -1
```

This settles the open question from the second finding above: it is **not** cosmetic and **not**
limited to IGMP's own housekeeping - a real CoT event arriving on the uplink side while the VIF
ambiguity is live is silently dropped before it ever reaches a HaLow-associated leaf. Filed as
"Issue 1" in the Morse Micro bug report (`~/morse-micro-bug-reports.md`, submitted 2026-08-30) -
worth adding this exact reproduction (one UDP packet, `rx_packets`/`tx_packets` before-and-after, the
matching log line) as a follow-up comment on that issue, since "not yet confirmed" was the report's
own caveat and this closes it.

#### Fourth Aug 30 finding: the VIF ID bug also blocks NAT'd internet access through the mesh, and a workaround is now landed

Surfaced by an actual end-to-end test: a phone joined a leaf's SoftAP, set its admin password
through the new auth UI (confirming that flow works from a real device, not just this session's
scripted checks), then tried to reach the internet through the mesh - and couldn't. Traced it to
the exact same VIF ambiguity as the finding above, just on a different traffic shape: outbound
(phone → internet) never touches the buggy path at all, but every **reply** has to transit the
relay's HaLow AP downlink on its way back down to the leaf, which is exactly the code path that
drops packets. One-directional failure, which is why outbound-only symptoms (DNS/HTTP requests
leaving fine, nothing ever coming back) looked at first like a NAT misconfiguration rather than
this already-known bug.

**Root cause, traced one level deeper than the finding above**: `mmhalow.c` has exactly one
`mmhalow_netif_driver_t`/netif, shared by both STA and AP modes (`mmhalow_init()` always creates
one STA-shaped netif; `downlink_halow_ap.c` reconfigures the *same* netif into AP mode via
`mmhalow_wifi_start()` -> `mmwlan_ap_enable()`, without ever creating a second one - the "sta_native
ip: 172.16.60.1" wording already seen in this project's own boot logs for a relay's AP-mode netif
is the tell). `halow_transmit()`, the one transmit callback this shared driver uses regardless of
mode, always builds `metadata = { .tid = 0 }`, leaving `.vif` at `MMWLAN_VIF_UNSPECIFIED` - so once
`mmwlan_ap_enable()` brings up a real AP VIF alongside the STA VIF that's always present as
scaffolding, every future call is ambiguous, exactly per the finding above's root-cause chain.

**Local workaround implemented and verified 2026-08-30** (`main/mmhalow.c`/`.h` are Apache-2.0
licensed by Morse Micro - confirmed via their own SPDX headers - freely modifiable for this kind of
local fix): a new `bool ap_mode_enabled` field on `mmhalow_netif_driver_t`, set `true` by
`mmhalow_wifi_start()` right before `mmwlan_ap_enable()`, read by `halow_transmit()` to pass an
explicit `MMWLAN_VIF_AP`/`MMWLAN_VIF_STA` instead of leaving the driver to guess. Verified with the
same multicast-injection method as the finding above: **0/6 packets forwarded to the leaf before
the patch, 6/6 after**, and zero "Unable to infer VIF ID"/"Packet failed to send" log lines across
the whole post-patch run (previously one per packet). No behavior change for GW_ROLE_CLIENT
(STA-only) nodes - `ap_mode_enabled` defaults false there, which resolves to the same
`MMWLAN_VIF_STA` value inference already produced.

**Made durable, since `managed_components/` is gitignored and re-fetched clean on every checkout or
dependency bump**: `patch_vendored_halow.py` (repo root) reapplies this exact edit idempotently,
invoked from the top-level `CMakeLists.txt` via `execute_process()` right after `project(...)`
(which is what triggers the component-manager fetch - the patch step has to run after that, not
before). Fails loudly rather than silently skipping the fix if the vendored file's shape has
changed underneath it (e.g. a `morsemicro/halow` version bump) - matching this project's existing
`minify_web_ui.py` philosophy. **Remove this whole mechanism once Morse Micro ships a real fix** -
added as "Further update" on Issue 1 in `~/morse-micro-bug-reports.md`, since we now have a
concrete, verified proposed fix to hand them, not just a bug report.

#### Fifth Aug 30 finding: DNS was the last thing standing between a phone and real internet access through the mesh

Surfaced immediately after the fix above: a phone joined a leaf's SoftAP, signed in through the
new auth UI (confirming that flow works from a real device - a second win alongside the VIF fix),
but still couldn't reach any external site, run a speed test, or reach anything else by name -
while the leaf's own web UI (a bare IP address, no DNS involved) worked fine. That split - local-IP
traffic fine, anything needing a hostname lookup dead - pointed at DNS specifically rather than a
regression in the fix above.

**Root cause**: `uplink_halow.c`'s `apply_static_ip()` (used by every leaf on a HaLow AP, since
that hop has no DHCP server to lease an address *or* a DNS server from) never gave the uplink a DNS
server at all - `ip_forward_nat.c`'s `propagate_dns()` already logs exactly this
(`uplink DHCP lease carried no DNS server`) and was already documented in this file, but as a
*deliberate* trade ("correct... for a hop whose whole purpose is carrying CoT, which is addressed
by IP") - which stopped being true the moment real cross-mesh internet access became a goal, not
just CoT.

**First design considered and rejected**: hardcode a public resolver (e.g. 8.8.8.8) on the leaf's
static uplink. Works, but two problems: it doesn't adapt if the relay's own upstream network's DNS
server changes, and it needed reprovisioning every leaf individually. **Redirected mid-implementation
by the user** toward a better design: have the leaf point at the *relay*, and have the relay forward
using its own real upstream DNS server (which it already has correctly, via ordinary DHCP on its
own Wi-Fi uplink - confirmed by that hop never showing the missing-DNS warning in any log this
session). Landed as `main/dns_forward.c` (see "What's implemented" above) plus
`gw_uplink_config_t.static_dns` (`GW_CONFIG_VERSION` bumped `6u -> 7u`) so a leaf has somewhere to
point.

**Confirmed on real hardware 2026-08-30**: the forwarder starts and binds
(`dns_forward: DNS forwarder listening on 172.16.60.1:53`), and a leaf reconfigured with
`gwcfg-set-uplink-static-ip 172.16.60.2 172.16.60.1 255.255.255.0 172.16.60.1` reassociates and
shows up on the relay's own `connected_clients` count as before. **Not yet confirmed**: an actual
query/response round-trip - that needs a device on a leaf's own SoftAP, which this project's dev
machine has no network path to (same limitation noted for the VIF fix above). Next session with a
phone in hand should confirm real browsing/speed-test/hostname-based traffic now works end to end.

#### Second Aug 30 finding: HaLow STA RSSI reads a flat 0 dBm against a leaf-facing HaLow AP

Surfaced by testing the RSSI-history feature (item 9's neighbor - see "What's implemented") on the
same two-node relay+leaf pair right after the datapath-race fix above. `gwcfg-status` on the
**leaf** (GW_ROLE_CLIENT, associated to the relay's HaLow AP, not a Pi) reported `uplink RSSI: 0
dBm` immediately on association and **still 0 dBm minutes later**, confirmed twice over the serial
console with real traffic (CoT relay running) in between. The **relay's own** uplink RSSI, over
native Wi-Fi to a real Pi, read normally (-65/-66 dBm, `wifi RSSI` in its own `gwcfg-status`) in the
same run - so this is specific to `mmwlan_get_rssi()` (`main/uplink_halow.c:569`, wraps `mmwlan.h`),
not a general RSSI-plumbing bug in this project's own code.

**Root-cause narrowed the same night, via source and a zero-rebuild diagnostic:**

Read `managed_components/.../umac/datapath/umac_datapath.c` directly:
`mmwlan_get_rssi()` -> `umac_stats_get_rssi()` (`umac_stats.c:329`) returns a field
(`data->rssi`) whose *only* writer in the whole vendored tree is
`umac_datapath_process_s1g_beacon()` (`umac_datapath.c:166-167`), gated on the received frame's
subtype being `DOT11_FC_SUBTYPE_S1G_BEACON` *and* its source address matching the associated BSSID
- and it sets the value straight from `rx_metadata->rssi`. First hypothesis was that this specific
callback never fires against the alpha AP-mode driver's beacons.

That hypothesis doesn't survive a second, independent reading, though: `gwcfg-scan` on the same
leaf, against the same AP, **also** reported `RSSI 0 dBm` for both entries it found (904.5 MHz and
905.0 MHz - two different frequencies, same exact `0`). Scan results come from **Probe Response**
frames (`mmwlan_scan_result.rssi`'s own doc comment, `mmwlan.h` L704-707: "RSSI of the received
frame... within the Probe Response frame") - a completely different code path from the S1G-beacon
callback above, populated by `main/uplink_halow.c:609`'s `out.rssi = result->rssi`. Two unrelated
consumers, both reading zero from the same underlying source, is what pointed the search at what
they share: **both ultimately come from the driver's own per-frame RX metadata
(`rx_metadata->rssi`)**, not from either call site's own logic. That relocates the likely fault from
"a callback that never runs" to "the RX RSSI field the driver stamps on received frames isn't being
populated with a real measurement" - specifically for frames arriving from this alpha AP-mode
transmitter, since the relay's unrelated native-Wi-Fi RSSI (a completely different radio and driver)
reads correctly in the same run.

A live MMLOG-level diagnostic (bumping morselib's own log verbosity to VRB to watch frame RX
directly) was attempted and abandoned: raising `MMLOG_LEVEL_OVRD` project-wide changes codegen
enough to turn a latent, otherwise-silent `-Werror=maybe-uninitialized` in an unrelated vendored file
(`morse_driver/mm6108/pageset.c:753`) into a hard build failure. Reverted cleanly (`CMakeLists.txt`
has no diagnostic left in it) rather than fighting the warning or weakening `-Werror` project-wide
for a one-off test - not worth the risk to the zero-warnings baseline for this.

**Consequence for the RSSI-history/sparkline feature (`main/link_history.c`)**: on a leaf associated
to a relay's HaLow AP, the sparkline renders a flat, plausible-looking `0 dBm` line instead of a gap
or an error - misleading, not a crash. Deliberately **not** patched by treating `0` as a sentinel:
the scan/association evidence narrows *where* the zero comes from, but not yet *why* the driver
isn't measuring it, and one node/one short run isn't enough to know whether `0` is ever legitimate
in some other topology.

**Next diagnostic, and it needs no rebuild**: run `gwcfg-scan` against a **real Pi's** HaLow AP once
one is confirmed to exist (`PI_SIDE.md` item 0) and compare. A real RSSI there would confirm this is
specific to two Morse radios talking to each other with one side in the (alpha) software AP-mode
role; a flat `0` there too would mean the gap is broader - in the STA-side RX RSSI capture itself,
regardless of what's on the other end.

#### Sixth Aug 30 finding: a relay's 8 MHz HaLow AP shows as *two* scan entries on a leaf, neither at 8 MHz - confirmed expected, not a bug

Raised as a live question during the memory-headroom work above: a relay's HaLow AP configured at
`op_class 4, s1g_chan_num 12` (908.000 MHz, 8 MHz - confirmed via `gwcfg-show` on the relay) was
scanned from its associated leaf with `gwcfg-scan`, expecting one entry near 908 MHz / 8 MHz.
Instead, twice, on two different firmware builds (before and after the PSRAM/heap-guard flash
above - fully reproducible, not intermittent):

```
SSID                             BSSID                  RSSI         FREQ  BW
d3MOUS-relay                     f6:ab:5c:df:41:15     0 dBm   904.500 MHz   1 MHz
d3MOUS-relay                     f6:ab:5c:df:41:15     0 dBm   905.000 MHz   2 MHz
```

**Read against the vendored SDK's own S1G Operation element struct**
(`managed_components/morsemicro__halow/.../umac/ies/s1g_operation.h`), this is consistent with
legitimate 802.11ah behaviour, not a driver fault: a wide (≥4 MHz) operating channel is required to
also advertise narrower **primary channels** for discovery, and the struct carries both a
`primary_channel_width_mhz`/`primary_channel_number` pair *and* a separate
`primary_1mhz_channel_loc` field - i.e. the standard expects a wide AP to expose *both* a primary
1 MHz sub-channel and a primary 2 MHz sub-channel, not just one. `mmwlan_scan_result.channel_freq_hz`/
`bw_mhz` are documented as "the channel where the frame was received" (`mmwlan.h`), not the AP's
operating channel - so a client legitimately reports the AP at wherever its Probe Response actually
landed. Cross-checked against the real US regdb table (`mmregdb.c` L319-369): 904.500 MHz/1 MHz and
905.000 MHz/2 MHz are both real, exact table entries, and both frequencies fall inside the AP's
actual 904-912 MHz (908 MHz ± 4 MHz) 8 MHz span - not off in unrelated spectrum.

**Confirmed by direct test, same day**: reconfigured the relay's HaLow AP from `op_class 4,
s1g_chan_num 12` (8 MHz) to `op_class 69, s1g_chan_num 26` (915.000 MHz, 2 MHz) via
`gwcfg-set-halow-ap` + `gwcfg-save` + reboot, matched the leaf's stored uplink credentials with
`gwcfg-set-uplink` + `gwcfg-save` + reboot, confirmed reassociation (`halow ap stas: 1` on the
relay, `uplink state: up` with a real lease on the leaf), then reran `gwcfg-scan` from the leaf:

```
SSID                             BSSID                  RSSI         FREQ  BW
d3MOUS-relay                     f6:ab:5c:df:41:15     0 dBm   915.000 MHz   2 MHz
1 AP(s) found
```

Exactly one entry, exactly matching the AP's real operating channel - no primary/operating split,
because a 2 MHz operating channel is already narrow enough to serve as its own primary. That the
split disappears precisely when the operating channel no longer needs a separate narrower primary
is about as clean a confirmation as this could get without parsing the raw IE. The IE-parsing route
above remains undone and is no longer worth doing for this question specifically - filed only if a
future need for it shows up on its own. `RSSI 0 dBm` reproduced identically at 2 MHz, confirming
that part is unrelated to bandwidth, same driver bug as the Second Aug 30 finding below.

**Project steer, decided the same day**: the relay's bench config now runs 2 MHz
(`op_class 69, s1g_chan_num 26`) instead of 8 MHz, matching the "prefer 1-2 MHz" recommendation
`HARDWARE.md`'s "Channel bandwidth" section already argued for on link-margin/range and
heap-pressure grounds - this test is the real-hardware confirmation that recommendation was
pointing the right way, including a side benefit neither of those original arguments anticipated:
simpler, unambiguous scan/discovery output. One unrelated observation from the same session,
noted but not chased: `gwcfg-scan` from an already-associated leaf knocks its own uplink loose
right as the scan starts, at both 8 MHz and 2 MHz - reconnects on its own within a few seconds
either way.

**What's still a real, separate, tracked bug**: both entries read `RSSI 0 dBm` - the exact
"Second Aug 30 finding" flat-RSSI issue below, reproduced again here on both firmware builds. This
finding doesn't touch that one; they just share a scan.

#### Seventh Aug 30 finding: no measurable CoT loss from a 1W MeshCore repeater 30ft away

Raised as a real-world coexistence question: the bench pair sits roughly 30ft from an active
MeshCore repeater (LoRa, 902-928MHz, same license-exempt band) running at 1W on its US/Canada
default preset - 910.525MHz, BW62.5kHz, fixed frequency (not hopping). With the relay's HaLow AP on
915.000MHz/2MHz (see the Sixth Aug 30 finding above), that's roughly a 3.4MHz clean gap between the
two systems' actual occupied spectrum - no co-channel overlap - but proximity/power alone can still
matter (front-end desensitization, raised noise floor) even without direct overlap, so this was
worth measuring rather than assuming.

**Method**: a new bench-only console command, `gwcfg-cot-test <count> <interval_ms>`
(`main/provisioning.c`), calls the existing `cot_relay_inject()` (previously unused - `cot_relay.h`
already documented it as "the generic send primitive the self-beacon will need," this is its first
real caller) to fire a burst of small datagrams into the CoT multicast group from one node, read
against the *other* node's rx counters (now also printed by `gwcfg-status`, alongside the existing
`/api/status` exposure) - `cot_relay_inject()` has no delivery confirmation of its own (UDP
multicast), so the receiving node's counter is the only real signal.

**Result**: two runs, both 0% loss. 200 packets at 50ms spacing (10s) and 2000 packets at 20ms
spacing (~40s), both injected from the leaf and landing in full on the relay's `cot downlink` rx
counter (200/200, then 2000/2000 more on top). MeshCore repeater running normally throughout, not
specially triggered.

**Caveats worth keeping**: this doesn't prove the repeater was actively transmitting for the full
window - LoRa repeaters aren't continuously keyed, so a clean result partly reflects real traffic
timing on their mesh, not just RF physics. It also doesn't test range: both nodes were still close
together (near each other, both near the repeater), so this confirms *coexistence at short range*,
not *link margin under coexistence at longer range*.

**Planned next, not yet run**: a real range test, discussed the same day but deferred to a later
session. Leaf stays wired to the dev machine (it has no route back to it anyway - static IP on an
isolated 172.16.60.x subnet, confirmed unreachable from here regardless of cabling); the relay moves
to the porch, co-located with the actual MeshCore repeater for the harder coexistence case at real
distance, on 6dBi antennas in place of stock on both nodes. Three open items to settle before
running it, captured here so they aren't re-derived from scratch:

1. **Reading the relay once it's off USB.** This dev machine can already reach the relay's
   home-network IP directly (`192.168.50.117` answered both ping and a plain HTTP GET) - so if its
   Wi-Fi uplink still reaches the home AP from the porch, `/api/status` is reachable *if* the web UI
   login is available for the session (it's behind auth now - see item 1 below). Otherwise, reading
   `gwcfg-status`-equivalent numbers means checking the relay's web UI from a phone on-site and
   reporting them back.
2. **No RSSI signal to watch as distance increases** - the leaf's flat `0 dBm` HaLow RSSI (Second
   Aug 30 finding) isn't fixed by any of this, so the range test is go/no-go plus the
   `gwcfg-cot-test` loss percentage, not a link-budget curve.
3. **6dBi antennas and the regulatory EIRP cap** - every regdb channel entry caps at 36dBm EIRP
   (`mmregdb.c`) regardless of antenna. Whether the firmware's conducted power already accounts for
   a specific antenna gain (and backs off for a higher-gain one) or transmits flat and trusts
   whatever's connected is genuinely unverified - not chased down yet. Low stakes for a private bench
   test, but worth reading `morselib`'s TX power path before treating a 6dBi swap as consequence-free
   on a build meant to ship compliant. (Restated because it's easy to forget mid-swap: never power
   the HaLow radio with no antenna attached, even briefly - risks the PA.)

Test method once those are settled: same `gwcfg-cot-test <count> <interval_ms>` flow as the Seventh
finding above, run from the leaf (stays reachable via USB regardless of where the relay ends up),
read loss off the relay's `cot downlink` rx counter.

#### What the Sep 11 stability soak proved

Run immediately after closing out Stage B and Stage C (F06/F07/F08/F11/F13, F02/F03/F15/F14's
migration portion - see the "2026-09-10 review tracker" section above), as a real-world check that
none of that work regressed basic link stability. Both bench nodes freshly configured end to end via
console (not left over from earlier testing): relay with its real Wi-Fi uplink (home network) and a
HaLow AP (`xiao-relay-1`, SAE, op_class 2/chan 26), leaf associated to it with a static 172.16.60.2
address. A background Python monitor polled `gwcfg-status` on both nodes over USB serial every 2
minutes and ran a 50-packet `gwcfg-cot-test` (leaf → relay) every 10 minutes, watching for the
firmware-version string changing (a reboot) or internal free heap dropping >10KB between samples (a
leak), logging everything with timestamps.

**Result, 3h39m (09:09-12:49), far past the 2h originally asked for**: zero reboots on either node
(firmware version string never changed once), zero heap-leak flags. Internal free heap drifted by
under 500 bytes total on each node over the whole run - relay 98,079 → 97,599 bytes, leaf 118,367 →
117,939 bytes - noise, not a trend. The relay's Wi-Fi uplink RSSI wandered normally (-66 to -75 dBm)
with no degradation over time; the HaLow AP kept the leaf continuously associated (`halow ap stas: 1`)
for the entire run. 21 CoT loss-test rounds (1050 packets total): 6 lost across the first 8 rounds
while the link was apparently still settling, then **13 consecutive rounds (650 packets) at 0% loss**
through to the end - an overall 99.4% delivery rate with no indication of degrading further.

Confirms the whole review-tracker pass (F06's radio_control priority fix, F07's scan-result
delivery rewrite, F08's retry timer, F11's config revision check, F15's challenge table/session cap,
F14's recovery-mode gating) didn't destabilize the two things that actually matter for a bench
pair: the HaLow link staying associated, and CoT traffic actually getting through it. The one
still-open item from this session, the relay's native Wi-Fi uplink crash tied to power delivery (see
the item directly under F06), did not reproduce at all during this run - consistent with the
power-delivery theory, run on the shorter cable the earlier fix used, not yet on a powered hub.

### 9. Persistent log storage and expanded config — scoping notes (2026-08-30)

Raised as "more log storage than anything" during the RSSI-history/CoT-counters work: the log ring
(`main/log_buffer.c`) is 6KB of RAM and gone on every reboot, which is fine for "why did the last
boot fail" but not for anything spanning a multi-hour field test or a power cycle.

**Where it would live:** the ~1.81MB unallocated past the dual-OTA partitions (two 3MB slots +
`otadata` on 8MB flash - see item 2) is enough for a small SPIFFS or FAT partition, no new hardware
and no GPIO cost - see `HARDWARE.md`'s "Other candidates weighed against the same 3 pads" for why
this beats an SD card for this specific need.

**The factory-reset trap to design around from the start, not retrofit:** `factory_reset.c`'s
`do_factory_reset()` clears state by overwriting the *one* `gw_config_t` NVS blob
(`GWCFG_NVS_NAMESPACE`/`GWCFG_NVS_KEY` in `provisioning.c`) with defaults - that's the only
persistent state that exists today, so today's factory reset is already complete. A flash-backed
log or a config namespace added alongside it would **not** be touched by that same call; either
needs its own explicit erase wired into `do_factory_reset()`, symmetric with the existing note on
item 1 ("`factory_reset.c` must clear the stored credential too, or a forgotten password survives
the one recovery path a field-deployed node has"). Same trap, same fix pattern, worth doing for
both credential and log storage in the same pass rather than two.

**Wear**: SPIFFS/FAT on raw flash has no wear leveling of its own guarantee beyond what ESP-IDF's
`wear_levelling` component provides for FAT - worth confirming which is actually in use before
writing at any real frequency (RSSI-history-style periodic samples are fine; per-packet CoT logging
to flash would not be).

**Persistent config "for somethings"**: no concrete second config namespace has been scoped yet -
revisit once there's an actual field that doesn't belong in `gw_config_t` (a single versioned NVS
blob already covers node identity, role, radio credentials, and CoT settings) rather than adding a
second store speculatively.

## Settled decisions

Recorded so they aren't relitigated, and so they aren't accidentally undone.

### Authentication

| Decision | Choice | Why |
|---|---|---|
| Default password | **Forced change on first use** | A fixed default is bad practice and likely non-compliant — California SB-327 and the UK PSTI Act each require a unique per-device credential or a forced change at setup. This board has no screen and no per-unit labelling step, so per-device randomness can't be communicated. |
| TLS | **Yes, since 2026-09-10** — self-signed per-device ECDSA P-256 cert, fingerprint verified like an SSH host key | **Supersedes this row's original "No."** Original reasoning (no CA for a private IP; a self-signed cert trains users to click through warnings; RAM cost) didn't have an answer for the click-through problem or the RAM cost — this does: (1) the device generates its own cert at first boot and prints its SHA-256 fingerprint on the serial console (`gwcfg-show-cert`), so an operator verifies it once out-of-band before trusting the browser warning — TOFU, the same model SSH host keys and every home router/Ubiquiti/pfSense admin panel already use, not blind click-through; (2) `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` routes mbedtls's ~40KB-per-connection buffers into the 8MB PSRAM pool (added 2026-08-30) instead of the ~512KB internal SRAM this project guards closely — see sdkconfig.defaults' own comment for the exact figures and the accepted tradeoff (cleartext in PSRAM without flash encryption, same risk class as the admin credential/radio passphrases already sitting in cleartext NVS). Implemented in `main/tls_identity.c`; review finding F02, `design/PROJECT_REVIEW_2026-09-10.md`.

Confirmed on real hardware, two physical nodes bridged over HaLow (relay on a real home Wi-Fi AP, a leaf associated to the relay's HaLow AP - the same two-node bench pair prior sessions used): HTTPS server starts (`esp_https_server: Server listening on port 443`), identity generates once and persists across reboots (same fingerprint after a reflash), `gwcfg-show-cert`/`gwcfg-reset-tls-identity` work. **A real independent TLS client completed the full handshake and fetched content** (`curl -k` from a machine sharing the relay's Wi-Fi uplink network: `HTTP 200`, handshake ~0.5s) and **its served certificate's SHA-256 fingerprint exactly matched `gwcfg-show-cert`'s console output** (`openssl s_client | openssl x509 -fingerprint -sha256`) - the actual TOFU property this design depends on, verified end-to-end rather than assumed.

**Stack headroom measured under real concurrent load, not idle** (2026-09-10): 8 parallel TLS clients (deliberately over `HTTPD_SSL_CONFIG_DEFAULT()`'s `max_open_sockets=4`) plus a 300-packet CoT injection (`gwcfg-cot-test`) running simultaneously through the same relay. `httpd`'s worst-case-ever mark across the run: **6748/10240 bytes free (34% used)** - comfortably inside this project's safe band, well clear of the "tight" (<512B) and "CRITICAL" (<256B) thresholds "Stack budgets" below defines. Free internal heap dropped from ~103KB to ~49.5KB at the low point (recovered fully after the load stopped) - below `heap_guard.c`'s CoT-shed threshold, so its shedding path was very likely exercised live, not just in theory. The overload itself produced clean, expected errors, not memory corruption: `MBEDTLS_ERR_NET_CONN_RESET` (-0x0050) and `MBEDTLS_ERR_SSL_CONN_EOF` (-0x7280) - both confirmed against real mbedtls source to mean "the client gave up and closed the connection" (curl's own `--max-time 3` triggering exactly as expected against a socket cap sized for ~1-2 real admin browsers, not 8 synthetic ones), not a resource-exhaustion or allocation-failure code. Node fully recovered after the load stopped: heap back to baseline, all subsystems still running, no reboot. |
| Password on the wire | **Challenge-response** — server issues a nonce, client returns `HMAC(stored_key, nonce)` | With WPA2-PSK, anyone who knows the AP passphrase can decrypt other clients' traffic. A team may share the Wi-Fi passphrase without every member being an administrator. |
| Browser crypto | **A bundled ~2 KB SHA-256/HMAC** | ⚠️ `crypto.subtle` is only exposed in *secure contexts*, and `http://172.16.50.1` is not one (only `localhost` is trusted over plain HTTP). **Do not "simplify" this back to WebCrypto later — it will silently be `undefined` on the device.** |
| Storage | PBKDF2-HMAC-SHA256, per-device random salt in NVS | Implemented 2026-08-30 - see item 1. The KDF itself runs client-side (mbedtls is only used on-device for the one HMAC-SHA256 verification at login, added to `main/CMakeLists.txt`'s `PRIV_REQUIRES` - it was not previously a `main` dependency, correcting this row's earlier claim). Never store the password itself. Logins are rare, so err high on iterations. |
| Sessions | `esp_random()` tokens, RAM only, small fixed table, idle timeout, `HttpOnly` + `SameSite=Strict` | Tokens should not survive a reboot. |
| Brute force | Lockout or backoff | The attacker here is already on the LAN. |
| Recovery | `gwcfg-reset-auth` on the serial console, plus the BOOT-button reset | Physically-present-only is the right trust model. **An undocumented recovery path is the same as none** — document it prominently. |

### Networking

- **NAT for general traffic, an explicit relay for CoT multicast.** batman-adv is a Layer 2 mesh
  with no concept of routing to an IP subnet that only exists behind a NAT'd leaf, so a routed
  (non-NAT) design would need either a manual static route on the gateway Pi or a nonexistent
  dynamic mechanism for leaves to announce a subnet. NAT ships now and works; the cost is that
  **nobody on the mesh can originate a connection to a specific phone behind the XIAO**. Multicast
  is handled separately by the relay precisely because it doesn't depend on that unicast path.
  Static routing on the gateway Pi is the v2 fix if it's ever needed — not a firmware change.
- **The XIAO cannot L2-bridge its two radios** the way batman-adv bridges Pi nodes: they're
  physically different PHYs and there's no batman-adv on FreeRTOS/lwIP. L3 forwarding between the
  two `esp_netif`s is the only option, which is why the relay has to exist at all.
- **Default SoftAP subnet is 172.16.50.0/24.** 192.168.x collides with home routers, phone hotspots
  and esp_netif's own 192.168.4.1 default; an overlap between a client's remembered network and
  this one is very hard to diagnose in the field. Every node can safely use the same subnet — each
  NATs behind its own uplink address.
- **A relay's HaLow AP runs no DHCP server; every leaf that joins it needs a manually-assigned
  static IP in the same /24** (`gwcfg-set-uplink-static-ip`, or the web UI's "Use static IP").
  Verified twice against real source, not memory: `mmhalow_init()`
  (`managed_components/morsemicro__halow/mmhalow.c:205-206`) always builds its netif from
  `ESP_NETIF_DEFAULT_WIFI_STA()` and calls `esp_netif_new()` itself, with no caller-supplied config
  and regardless of STA/AP mode, so `esp_netif->dhcps` is never allocated (`esp_netif_lwip.c:837`
  at v5.5.1 only calls `dhcps_new()` when `ESP_NETIF_DHCP_SERVER` was set *at that call*). Calling
  the public `esp_netif_dhcps_start()` on this netif anyway doesn't just fail - `esp_netif_lwip.c:1698`
  unconditionally calls `dhcps_set_new_lease_cb(esp_netif->dhcps, ...)`, a null-pointer dereference,
  a hard crash. The only way around it is either patching the vendored `mmhalow.c` (it's regenerated
  from the component registry - don't) or reaching into `esp_netif_t`'s private internals (fragile
  across ESP-IDF versions - also don't). **The real v2 fix, deliberately deferred (2026-08-29,
  operator feedback that manual bookkeeping doesn't scale past a couple of nodes): a small DHCP
  server written from scratch** - a plain UDP socket on port 67 handling DISCOVER/OFFER/REQUEST/ACK
  for a handful of leases, entirely independent of esp_netif's built-in server. Not started.
- **US-only, 902–928 MHz — and that is a hardware limit.** The module is a Quectel FGH100M-H, a
  902–928 MHz part, and the BCF the firmware loads (`bcf_fgh100mhaamd.bin`) is named "FGH100M-H
  (US)" upstream; it carries real calibration for US alone. The nine-region build matrix that used
  to live in `country-configs/` was checking `mmregdb`'s channel tables and nothing else — four of
  those regions were unreachable frequencies and two had no BCF section at all, all failing
  silently. Deleted. `design/HARDWARE.md` "Regulatory domain" has the section-by-section evidence.
  Don't re-add a region without a BCF that covers it.
- **Region is build-time and cannot be made runtime-configurable.** The SDK reads
  `CONFIG_HALOW_COUNTRY_CODE` from Kconfig before the radio scans; there is no per-connection
  channel argument in the STA connect API. It is now fixed at `"US"` in `sdkconfig.defaults`.
- **`CONFIG_HALOW_PS_MODE=n`, and it is not in the vendor's board file.** The component defaults it
  *on* whenever `CONFIG_MM_WAKE`/`CONFIG_MM_BUSY` are set — which upstream's own Seeed board config
  does — but its help text requires those pins to be physically connected, and on the HAT
  (rev V3.0) R10 and R17 are the board's only DNP resistors. Running power-save against pins that
  reach nothing lets the host stop servicing the SPI interrupt while the module is still awake;
  the resulting bus errors escalate to an `MMOSAL_ASSERT`, which in this SDK calls `esp_restart()`
  rather than returning an error, so the node boot-loops shortly after the connect attempt starts.
  **This looks exactly like a radio failure and is not one** — the radio initializes fine first.
  Do not remove the setting to "match upstream's board file"; the citation chain is in
  `sdkconfig.defaults` and [`HARDWARE.md`](HARDWARE.md). The correct way to get power-save back is
  to populate R10/R17.

- **A node with no uplink configured does not start the reconnect loop.** "Nobody has told this
  node which AP to join" and "the AP we were told about isn't answering" are different situations
  with opposite advice, and the firmware used to report both as `searching` while burning 15-second
  association attempts against a placeholder SSID it shipped with. Now the defaults carry *no*
  uplink, an empty SSID means "not configured" (`gw_uplink_is_configured()`), and that state has
  its own LED pattern, status string and web-UI banner. It also keeps the radio free for scanning,
  which is what an operator is doing at that exact moment. Derived from the SSID rather than a
  separate flag or a user-facing toggle: the two can never disagree, and there is no switch that
  can strand a configured node in the field.

### Observability

- **Flash core dumps are on** (64 KB partition, ELF format). Bring-up means crashes happen
  unattended and away from a terminal; without this a panic leaves nothing behind. Flash cost is
  trivial against 8 MB.
- **The reset reason is logged at every boot, into the RAM log ring.** Hardware bring-up produced a
  reboot loop on a node that could not be powered from a PC's USB port — so there was no serial
  console, and every instrument that survives a reset lives in flash or on the far side of a cable.
  `esp_reset_reason()` costs one line and separates a brownout from a panic from a watchdog, which
  is the first fork in that diagnosis. The core-dump summary is logged next to it but labelled
  separately: it is the last panic *ever* recorded, not necessarily this boot's.
- **Link state is four states, not a boolean.** "Not associated" and "associated but no lease" are
  different subsystems failing, with different fixes. Collapsing them was the single biggest
  diagnosability gap the firmware had.

### Stack budgets

After the `sys_evt` overflow (item 8), every task and callback context in the firmware was audited.
Recorded so the numbers don't have to be re-derived, and so the non-obvious ones aren't "tidied".

**These are the sizes tasks are created with, not the headroom they actually have.** For that, run
`gwcfg-tasks` on the console or open `GET /api/tasks` (Live Log tab → Task Stack Headroom) — both
report `uxTaskGetStackHighWaterMark()`, the least stack each task has ever had spare. Prefer the
measurement to this table when deciding whether a budget is too tight: the table says what was
allocated, the instrument says what got used. Check it after the node has been up a while and
through a reconnect or two, since a high-water mark only reflects paths that have actually run.

| Context | Stack | Notes |
|---|---|---|
| `sys_evt` (esp_event default loop) | **4608** | `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096` (`sdkconfig.defaults`) + 512 `TASK_EXTRA_STACK_SIZE`. Was 2816 (the 2304 Kconfig default, never overridden) until 2026-08-30: measured at 472 bytes free on real hardware on both roles after a reconnect cycle each - inside this project's own "tight, raise it" band (`gwcfg-tasks`: <512 B) - even with the item-8 deep call chain already moved off it. Raised to match this project's other worker tasks, plus a real fix alongside it: `uplink_wifi.c`'s `WIFI_EVENT_STA_START` case called `esp_wifi_connect()` directly rather than deferring to `wifi_reconnect_task` like `schedule_reconnect()` already does for the same call - inconsistent with the discipline below, though not the dominant cost - fixing it only moved the pre-bump measurement from 440 to 472 bytes free, see the `sdkconfig.defaults` comment for the full numbers. Now 2376-2436 bytes free (~49%) on both roles after the stack bump. **Still runs every event handler this firmware registers - budget generously, not exactly.** |
| `esp_timer` task | 4096 | 3584 + 512. Runs `reconnect_timer_cb`, `reboot_timer_cb` — both trivial by design. |
| `datapath` | 4096 | Where NAT + CoT relay bring-up actually runs. **Exits once the datapath is up**, returning the 4 KB — so `absent` is the healthy steady state in `gwcfg-tasks`, and still-present means an attempt is outstanding. |
| `wifi_reconnect`, `halow_reconnect`, `cot_relay`, `factory_reset` | 4096 each | `factory_reset` looked oversized at idle (84% free - the task body is just a GPIO poll) until actually measured mid-reset on real hardware (2026-08-30, physical BOOT-button hold, caught with a rapid `gwcfg-tasks` poll in the ~250ms window before `do_factory_reset()`'s `esp_restart()`): 1764 bytes free (57% used, 43% margin) during the real `provisioning_save()` NVS write - this is the recovery path of last resort (see `factory_reset.c`'s own comment), so that margin is earned, not spare to reclaim. |
| httpd (`web_ui.c`) | 6144 | Raised from esp_http_server's 4096 default. Same "looks oversized at idle" trap: 81% free (20% used) reflected only light `GET` traffic until a real max-field `POST /api/config` was sent to a live relay on 2026-08-30 (curled directly from a dev machine sharing the relay's uplink subnet, `allow_uplink_management` already on) - dropped to 1648 bytes free (74% used, 26% margin), confirming the raise was load-bearing, not generous. **The general lesson from both of these: `uxTaskGetStackHighWaterMark()` is a floor on "used," not a ceiling - a task sitting at high "free" may just mean its worst path hasn't run yet this boot. Don't trim a budget from an idle-since-boot number; exercise the task's actual heaviest path first.** |
| console REPL (`provisioning.c`) | 4096 | `ESP_CONSOLE_REPL_CONFIG_DEFAULT()`. |
| `status_led` | 2048 | Deliberately small — the task body reads an enum and toggles a pin. It must stay that way; it has no room for a log call. |
| morselib `evtloop` (SDK-owned) | 8608 | `umac_evtloop.c` asks for 2152 **words**; the ESP32 shim converts (`stack_size_u32 * 4`, `mmosal_shim_freertos_esp32.c` L216-221). This is what invokes `mm_sta_state_cb` and `scan_rx_cb` — so `web_ui.c`'s cJSON scan collector runs on an SDK task, not ours, and fits. |

Two things that look like ordinary style but are load-bearing:

- **`cot_relay.c`'s 1500-byte `rx_buffer` is `static`.** As a local it would be over a third of that
  task's stack in one object.
- **`log_buffer.c`'s `s_line[256]` is `static`, guarded by the ring's own mutex.** That hook is
  installed via `esp_log_set_vprintf`, so it runs on *whatever task called `ESP_LOGx`* — as a local
  it charged 256 bytes to every task in the firmware on every log call, `sys_evt` included, on top
  of the console handler's `vprintf`. Moving it to `.bss` costs nothing in flash and removes ~9% of
  `sys_evt`'s stack from every logged line.

Anything added to an event handler, an `esp_timer` callback, or a callback the HaLow SDK invokes
inherits one of these budgets rather than getting its own. Check which one before adding work.

## Known limitations — decisions, not bugs

Recorded so they don't get "fixed" by accident.

- **No inbound unicast to a specific client.** See the NAT reasoning above.
- **NAT and the relay initialize against the first uplink IP only.** If a later reconnect leases a
  *different* address they are not re-initialized against it. Whether this matters depends on
  whether lease changes happen in practice, which hardware testing will answer. (A *failed* init
  does retry on the next reconnect — that was a bug and is fixed. Different thing.) Because of
  this, the `datapath` task now exits once both halves are up and returns its 4 KB stack; it stays
  alive only while an attempt is still outstanding. If this limitation is ever lifted, that exit
  has to go with it.
- **A statically-addressed uplink gets no DNS.** `gwcfg-set-uplink-static-ip` means no DHCP
  client, so nothing learns a resolver — and `esp_netif_set_ip_info()` additionally calls
  `dns_clear_servers(true)` on a DHCP-client netif. `ip_forward_nat.c` handles it correctly
  (warns, and leaves the downlink's DHCP DNS option off rather than offering `0.0.0.0`), so leaf
  clients get working IP connectivity and no name resolution. Acceptable on a hop whose purpose is
  CoT, which is addressed by IP. A configurable static DNS server is the fix if it ever matters.
- **The relay's Wi-Fi uplink is DHCP-only.** `gw_wifi_uplink_config_t` (`main/gw_config.h`) holds an
  SSID and a PSK and nothing else, so a GW_ROLE_RELAY node can't be statically addressed on its
  Wi-Fi hop the way a leaf can on its HaLow one. It has never needed to be: that hop joins a Pi's
  ordinary `br-ahwlan` AP, which runs a DHCP server. Adding it costs new fields, a
  `GW_CONFIG_VERSION` bump and a second copy of `apply_static_ip()`'s derivation — do it only if a
  Pi turns up with no DHCP server on that interface.
- **No captive-portal DNS redirect.** A real UX gap, not a defect. See item 4 above.
- **The web UI cannot clear a stored passphrase or switch the SoftAP to open.** A blank password
  field means "keep current" - `GET /api/config` never echoes passphrases back, so an empty field
  can't be distinguished from "clear it", and "keep" is the safe reading. The console can do it:
  `gwcfg-set-softap <ssid> -`. The page says so next to the field.
- **No OTA delivery.** Layout is ready, mechanism is blocked on auth by choice, not effort.
- **No radio power save.** `CONFIG_HALOW_PS_MODE=n` is forced off because the Seeed HAT leaves the
  WAKE and BUSY links unpopulated — see the settled decision below. Current draw is higher than the
  hardware could achieve, and that is the accepted price of not boot-looping.
- **`GW_CONFIG_VERSION` bumps discard stored config.** Deliberate: a layout or default change that
  silently reinterpreted an existing blob would be worse. Reprovision after a bump.

## Open questions

All Pi-side, all tracked in [`PI_SIDE.md`](PI_SIDE.md). **Top of the list as of 2026-08-16:**
whether any current OpenMANET role still exposes a HaLow radio in plain AP mode at all — reading
OpenMANET's own docs, the "HaLow AP wizard" appears to have been removed, and every HaLow radio is
now described identically as a `10.41.0.0/16` backbone member. If that holds, it blocks association
**more fundamentally** than the regulatory domain does, on any Pi, not just once a second one joins
— see `PI_SIDE.md` "Still to verify" item 0 for the citations and what to check on a real Pi before
trusting the rest of this list. Below that: the AP's security mode, DHCP scope and whether it offers
DNS, the regulatory domain (**this one blocks association outright**), and mesh-point + AP
concurrency once a second Pi joins.
