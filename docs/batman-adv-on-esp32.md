# batman-adv on the ESP32: what it would take to join an OpenMANET mesh

Research notes, 2026-10-18. Sources: the Linux kernel's `net/batman-adv` (the only
implementation that exists), its wire format header `include/uapi/linux/batadv_packet.h`,
OpenMANET's docs and OpenWrt fork release notes, and the open-mesh mailing list. The
open-mesh.org wiki is behind a bot wall, so everything below comes from the code itself.
Numbers are from the kernel at the time of writing (batman-adv 2025/2026); OpenMANET ships
batman-adv 2024.3, same compat version, same protocol.

## What OpenMANET actually runs

- OpenWrt on a Pi (or HaLowLink 2, Heltec HT-HD01, Gateworks) with a Morse Micro HaLow radio
  as an **802.11s mesh point** (`hostapd_s1g`/`wpa_supplicant_s1g`, tuned live by
  `openmanet_mesh11sd`). All radios must share mesh ID, password and channel.
- **batman-adv, routing algorithm BATMAN_V** (`batctl` shows `BATMAN_V`; the OpenWrt fork's
  notes say they moved from IV to V for interface bonding). `bat0` on every node is bridged
  into `br-ahwlan` with the Pi's 2.4/5 GHz access point and Ethernet.
- One flat Layer 2 domain: `10.41.0.0/16`. Every mesh point runs its own DHCP scope (16
  leases from offset 351 by default), gets a reserved static `bat0` address like
  `10.41.113.1`, a fresh node starts at `10.41.254.1`, gateways live in `10.41.0.0/24`, the
  "mesh gate" NATs to its WAN. mDNS works across the mesh. `openmanetd` (Go) does gateway
  selection and the UI/API; `alfred` carries telemetry.
- batman-adv "multicast optimizations" are on (their docs say so for TAK, ADS-B and mDNS).
- Still unknown without a real node: the mesh SAE password handling, channel/bandwidth, and
  whether 802.11s forwarding (`mesh_fwding`) is off. With batman-adv on top it normally is.

## Where batman-adv sits

batman-adv is a Layer 2 routing protocol. It takes Ethernet frames from `bat0`, wraps them in
its own headers, and sends them as Ethernet frames with **ethertype 0x4305** to a *direct
neighbour* over the "hard interface" (here: the 802.11s mesh interface). Multi-hop is
batman-adv's job, not 802.11s's. So for a XIAO:

1. The HaLow radio must be an 802.11s mesh point that the Pi has **peered** with (open,
   confirm, and with OpenMANET's password: SAE plus authenticated peering plus keys). Linux
   drops data frames from un-peered mesh neighbours. Roadmap steps 4 and 5. Not covered here.
2. Over that one-hop link, the XIAO speaks batman-adv. Our `mesh.c` already moves Ethernet
   frames between neighbours in 4-address mesh data frames; in "batman mode" it would stop
   relaying and flooding (TTL 1, no route learning) and just offer "send frame to neighbour X"
   and "send frame to all neighbours". Everything in this document runs on top of that.

The protocol work is independent of the radio. It can be built and tested first over the
XIAO's 2.4 GHz Wi-Fi against a Linux box on the same LAN (see "Test rig").

## Wire format (compat version 15)

All multi-byte fields big-endian. Every packet starts with `packet_type` and `version`
(always 15); a receiver drops anything with another version.

| Type | Value | Use |
|---|---|---|
| ELP | 0x03 | neighbour discovery, every 500 ms, broadcast |
| OGM2 | 0x04 | route announcements, every 1000 ms, flooded |
| BCAST | 0x01 | flooded data (broadcast and, for legacy nodes, multicast) |
| UNICAST | 0x40 | routed data |
| UNICAST_4ADDR | 0x42 | routed data with the sender's originator address and a subtype (DAT, speedy join) |
| UNICAST_FRAG | 0x41 | fragments of a unicast packet |
| UNICAST_TVLV | 0x44 | translation table requests and responses, roaming |
| ICMP | 0x43 | `batctl ping`, optional |
| IV_OGM 0x00, CODED 0x02, MCAST 0x05 | | not needed (IV, network coding, mcast packet type) |

Structures, in field order:

- **ELP** (16 bytes): type, version, orig[6], seqno u32, elp_interval u32 (ms). Sent to
  ff:ff:ff:ff:ff:ff.
- **OGM2** (20 bytes): type, version, ttl, flags (0), seqno u32, orig[6], tvlv_len u16,
  throughput u32 (units of 100 kbit/s). Followed by `tvlv_len` bytes of TVLVs.
- **TVLV header** (4 bytes): type, version, len u16; then `len` bytes. Types: GW 1, DAT 2,
  NC 3, **TT 4**, ROAM 5, MCAST 6. Keep lengths even: an odd `tvlv_len` ends aggregate parsing.
- **UNICAST** (10 bytes): type, version, ttl, ttvn, dest[6] (the destination *originator*,
  not the client). Followed by the full Ethernet frame.
- **UNICAST_4ADDR** (18 bytes): the unicast header, src[6], subtype (DATA = 1), reserved.
- **BCAST** (14 bytes): type, version, ttl, reserved, seqno u32, orig[6]. Then the frame.
- **UNICAST_FRAG** (20 bytes): type, version, ttl, one byte of `no:4 priority:3 reserved:1`,
  dest[6], orig[6], seqno u16, total_size u16. Then the fragment payload.
- **UNICAST_TVLV** (20 bytes): type, version, ttl, reserved, dst[6], src[6], tvlv_len u16,
  align u16. Then TVLVs.
- **TT TVLV data**: flags, ttvn, num_vlan u16, then per VLAN {crc u32, vid u16, reserved u16},
  then zero or more changes {flags, reserved[3], addr[6], vid u16}. TT flags: OGM_DIFF 0x01,
  REQUEST 0x02, RESPONSE 0x04, FULL_TABLE 0x10. Untagged traffic uses vid 0; a tagged VLAN
  sets bit 15 (`BATADV_VLAN_HAS_TAG`).

Defaults that matter: TTL 50, jitter 20 ms, hop penalty 30/255, ELP 500 ms, OGM 1000 ms,
OGM aggregation up to 512 bytes or 100 ms, sequence windows 64, originator purge 200 s,
TT local timeout 600 s, TT request timeout 3 s, fragments up to 16 of at most 1280 bytes,
fragment timeout 10 s, multicast fanout 16, bat0 MTU 1500 with fragmentation on.

## What a minimal node must do

A "leaf" node: one or a few neighbours, a hotspot's worth of clients behind it, no gateway
duties, no bridge loop avoidance, no network coding, no DAT.

### 1. ELP: be seen

- Send an ELP every 500 ms (plus or minus 20 ms jitter) with our originator MAC, a random
  starting seqno incremented each time, and our interval.
- On receiving one: remember the neighbour (MAC, last seen, interval). Ignore seqnos within
  64 behind the last. A neighbour not heard for 200 s is gone.
- Linux also sends unicast ELP "probes" (2 per neighbour, 200 bytes) to measure Wi-Fi
  throughput. We can ignore received probes and send none; the Pi measures us from its own
  side (station expected throughput from the driver, or 1 Mbit/s default).
- Our own link throughput estimate to each neighbour: 10 (1 Mbit/s) is what Linux uses when
  it has no better number. We can derive something from beacon RSSI later.

### 2. OGM2: announce ourselves, learn routes

Originating: every 1000 ms (plus or minus 20 ms), one OGM2 with ttl 50, seqno incremented,
orig = our MAC, throughput = 0xFFFFFFFF, flags 0, followed by our TT TVLV. Send on every
link as broadcast.

Receiving an OGM2 from neighbour N about originator O:

1. Drop if orig is us, or the sender is us, or throughput is 0, or we have no ELP neighbour
   entry for the sender (ELP must come first), or the seqno is outside the window (older than
   the last by more than 64 drops it; a jump of 64 or more ahead resets if the last reset was
   over 30 s ago).
2. `path = min(ogm.throughput, link_throughput_to_N)`.
3. Keep per (O, N) the throughput and seqno. The router (next hop) for O becomes N if N's
   throughput is strictly higher than the current router's, or N's seqno is at least 5 ahead
   of the router's. A neighbour is "similar or better" if its throughput exceeds three
   quarters of the best.
4. Forward if ttl > 1 and N is our current router for O and we have not forwarded this seqno
   yet: decrement ttl, set throughput to the (O, N) value after penalties: times
   (255 minus hop penalty 30) over 255, and halved when the frame goes back out the same
   half-duplex radio it came in on. Re-send as broadcast on all links. Linux delays and
   aggregates OGMs for up to 100 ms into 512-byte frames; a leaf can send them one at a time.
5. Hand the TVLVs to the TT code (below). Unknown TVLV types in an OGM are ignored.

Originators not heard for 200 s are purged.

### 3. The translation table: who is behind whom

batman-adv routes to *originators*; clients (phones on our hotspot) are mapped to originators
by the translation table. If we get this wrong, nothing reaches our phones.

- **Local table**: our own bat MAC (the mesh interface address, flagged NOPURGE) and every
  client MAC that sends through us (learned from source addresses; the WIFI flag is set for
  wireless clients). Each add or delete bumps our `ttvn` (one byte, wraps) when the next OGM
  is built, and the changes ride along in that OGM as `OGM_DIFF` entries.
- **CRC per VLAN**: for each entry (skipping ones flagged NEW): `c = crc32c(0, htons(vid), 2)`,
  `c = crc32c(c, flags & SYNC_MASK, 1)`, `c = crc32c(c, addr, 6)`; the VLAN CRC is the XOR of
  all entry values. SYNC_MASK keeps only the flags that are synchronised across the mesh
  (WIFI 0x10 and ISOLA 0x20; the others such as DEL 0x01, ROAM 0x02, NOPURGE 0x100, NEW 0x200,
  PENDING 0x400, TEMP 0x800 are local). crc32c is the Castagnoli polynomial, not the zip
  CRC32. This must match byte for byte or every node will keep asking us for our table.
- **TT TVLV in our OGM**: flags OGM_DIFF, our ttvn, num_vlan 1 (vid 0, our CRC), then the
  pending changes if any.
- **Answering TT_REQUEST** (unicast TVLV addressed to us): reply with TT_RESPONSE plus
  FULL_TABLE, our ttvn, the VLAN data, and every valid local entry as a change entry (flags
  as stored, no DEL). If the request's ttvn matches ours and we still have the last change
  set, Linux may send just the diff; always sending the full table is allowed and simpler.
- **Learning other nodes' tables**: from the TT TVLV in each OGM: if the originator's ttvn is
  exactly ours plus one and the OGM carries changes, apply them and verify the CRC; if the
  ttvn is 1 and we know nothing yet, same; otherwise, or on a CRC mismatch, send a TT_REQUEST
  with FULL_TABLE to that originator and apply the TT_RESPONSE (purge our copy of its entries,
  insert the new ones, store its ttvn). Entries flagged ROAM or TEMP do not count in the CRC.
- **Lookup for sending**: destination client MAC to originator, via the global table. Unknown
  destination: Linux floods it as broadcast. We do the same.
- **Roaming**: a client that moves from node A to node B causes B to unicast a ROAM TVLV to
  A. A leaf can ignore received ROAM messages at first (its phones do not roam).

### 4. Unicast data

Sending a frame from a client to a destination MAC: look up the originator for the
destination, build UNICAST (ttl 50, ttvn = that originator's last known ttvn, dest = the
originator) plus the frame, send to our router (next hop neighbour) for that originator. If
the total exceeds the link MTU minus headers, fragment (below).

Receiving: if dest is our MAC, deliver the inner frame to our stack (and to our hotspot).
Otherwise, if ttl is at least 2, decrement and forward to our router for dest. Linux also
re-routes packets whose ttvn is stale by looking up the inner destination again; a leaf can
skip that at first. Unicast copies of multicast (inner destination 01:00:5e:...) must be
accepted: that is how batman-adv delivers 239.x.x.x traffic such as ATAK CoT to nodes that
do not announce the MCAST TVLV (it sends each such node its own unicast copy, up to
16 nodes, then falls back to flooding).

### 5. Broadcast data

Sending: BCAST with ttl 50, our bcast seqno, orig = us, then the frame, to every neighbour.
Multicast from our side goes the same way (we announce no MCAST TVLV, so classic flooding
is what the others expect from us).

Receiving: drop if orig or sender is us, if ttl < 2, if the originator is unknown, or if the
seqno is a duplicate or outside the 64-window (per originator bitmap). Otherwise decrement
ttl, re-send to every neighbour (Linux re-sends each broadcast a few times with delays; one
copy is acceptable), and deliver locally.

### 6. Fragmentation

bat0's MTU is 1500 and the HaLow link's MTU is 1500, so a full-size IP packet always
arrives as 2 fragments. Reassembly is mandatory: chains keyed by seqno, fragments ordered
by `no` (the sender numbers tail fragments 0, 1, ... and sends the head last with the highest
number; the chain is kept from highest to lowest and concatenated in that order), total_size
must match across fragments, give up after 10 s, at most 16 fragments of at most 1280 bytes.
Sending fragments: split so each fragment fits `min(link MTU, 1280) - 20`, roughly equal
sizes, same seqno, ttl 50. Alternatively keep our own IP MTU at 1280 - 10 - 14 and never
fragment on the way out; incoming fragments still need reassembly.

### 7. Getting an address

bat0 is one Ethernet domain, so once the tables are in place we run a normal DHCP client on
it: the DHCP broadcast floods the mesh, the nearest mesh point's DHCP scope answers, and we
get a `10.41.x.x` address with the mesh gate as gateway. No manual addressing. ARP works
the same way (flooded); DAT is an optimisation we can live without.

### 8. Things we skip

DAT (distributed ARP), BLA (bridge loop avoidance), network coding, the GW TVLV (gateway
announcements; the mesh gate's DHCP handles our default route), the MCAST TVLV, ICMP, ELP
probes, OGM aggregation, ttvn-based re-routing, roaming advertisements. None of them is
required for the other nodes to accept us.

## Size and effort

Roughly: ELP 150 lines, OGM2 and routing tables 500, TT with CRC and request/response 700,
unicast and broadcast paths 400, fragmentation 200, DHCP glue and the link adapter 300,
plus a status page. Around 2500 lines of C, written against the kernel source as the
reference. Two to four weeks of focused work for the first version that pings a Linux
batman-adv node, assuming a Linux peer to test against from day one. The hard part is not
the code, it is matching the kernel's exact behaviour in the TT CRC and the sequence windows;
`batctl` on the Linux side (`batctl n`, `batctl o`, `batctl tg`, `batctl tl`) shows
immediately whether we are seen, routed and translated.

## Test rig without a Pi

batman-adv does not care what the link is. Plan:

1. **Linux peer on the LAN.** The Umbrel box runs Debian 13 with kernel 6.12 and has
   `batman-adv.ko` installed; it needs `sudo modprobe batman-adv`, `apt install batctl`, then
   `batctl meshif bat0 interface add enp4s0`, `batctl ra BATMAN_V` before creating `bat0`,
   and an address on `bat0`. (One sudo password away. The MCP access to that box cannot
   do it.) Any Linux laptop or VM with a wired port works the same.
2. **XIAO on the home Wi-Fi.** Our batman-adv code sends and receives ethertype 0x4305 frames
   on the ESP32's 2.4 GHz station interface (ESP-IDF's internal Wi-Fi transmit and raw receive
   callbacks give Layer 2 access). The home access point bridges them to the wired LAN like
   any other frame. `batctl n` on the Linux box should list the XIAO within a second of its
   first ELP; `batctl o` after its first OGM; `batctl tg` after the first TT exchange; then
   `ping` across `bat0`.
3. **Then HaLow.** Move the same code onto the mesh link once peering (step 4) and the mesh
   password (step 5) exist, and test against a real OpenMANET node.

Step 2 proves the whole protocol implementation with no radio work at all, and it is the
part that can start tomorrow.

## Open questions to settle on a real OpenMANET node

- Mesh password: SAE on the mesh, and what `openmanet_mesh11sd` sets (`mesh_fwding`,
  `mesh_rssi_threshold`, beacon interval). Needs a look at their OpenWrt fork's config files
  or a running node's `uci show wireless`.
- Their multicast firewall rules and whether `multicast_mode` fanout changes.
- Whether their `bat0` MTU or fragmentation differ from the defaults above.
- batman-adv version 2024.3 versus current: the wire format is unchanged (compat 15 since
  2013), so no issue expected.

## References

- Kernel wire format: `include/uapi/linux/batadv_packet.h`
- ELP: `net/batman-adv/bat_v_elp.c`; OGM2: `bat_v_ogm.c`; neighbour comparison: `bat_v.c`
- Translation table: `translation-table.c`; routing: `routing.c`; fragments:
  `fragmentation.c`; multicast fallback: `multicast.c`; defaults: `mesh-interface.c`,
  `main.h`, `hard-interface.c`
- OpenMANET docs: https://openmanet.github.io/docs/networking and
  https://github.com/OpenMANET/openwrt/releases
- A 2018 mailing-list thread about a FreeRTOS/ESP32 port of BATMAN IV (never published)
  is the only prior attempt found; nobody has done BATMAN_V on a microcontroller.
