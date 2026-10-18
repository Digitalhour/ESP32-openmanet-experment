# HalowChat

Hubless group chat over Wi-Fi HaLow (802.11ah, 915 MHz) on cheap boards: Seeed XIAO ESP32S3
plus the Seeed Wio-WM6180 HaLow module (Morse Micro MM6108). Every board runs the same
firmware, gives a phone a Wi-Fi hotspot and a chat page, and relays messages for the other
boards over a HaLow mesh. No access point, no pairing, no Linux box.

The part that did not exist before: an 802.11s-style mesh on the MM6108 from a
microcontroller. Morse Micro's SDK offers station and access point only, and Morse has said
mesh is not supported on their microcontroller offering. The chip can do it (Linux does), and
this repository shows the recipe for the ESP32 side: three small hooks into the SDK and one
extra chip command. See `firmware/halowmesh/README.md` for how it works and
`firmware/mesh-probe/README.md` for the experiments that found it.

## Who made this, and a warning

I am a PHP and Python developer. I know very little about the ESP32, ESP-IDF, or the Morse
Micro SDK. This project was built with Claude Fable 5.1 (Anthropic's AI model) doing the
research, the firmware code and most of the documentation, while I ran the hardware, tested,
and decided what to build next. Read the code with that in mind: it works on my two boards,
on my desk, and that is the extent of the testing so far.

Use at your own risk. This software is provided as is, with no warranty of any kind. I am
not responsible for any damage, loss, or regulatory trouble that may come from using it.
In particular: it drives a radio transmitter, so check your local rules for the 900 MHz band
(the firmware is set up for the US channel plan), it talks to the HaLow chip in ways its
vendor does not support, and it can reset or crash the chip and the board. If that worries
you, do not flash it.

## Layout

| Directory | What |
|---|---|
| `firmware/halowmesh` | The mesh chat firmware. This is the one to flash. |
| `firmware/mesh-probe` | The experiments: interface types, mesh beacons, over-the-air checks, peering frames, 4-address data. Each one documented with its result. |
| `firmware/halowchat` | The earlier hub version (one board is a HaLow access point, the rest join). Works, kept as a fallback. |
| `firmware/openmanet-gateway` | Will Shields' OpenMANET-XIAO-Gateway, imported with its full history (GPL-3.0). The XIAO as an OpenMANET gateway: phone hotspot, NAT, DNS forwarding, ATAK CoT multicast relay, web UI with login, OTA partitions. Being joined to the mesh layer; see below. |

## Status (2026-10-18)

- **The gateway runs on the mesh.** `firmware/openmanet-gateway` has a third node role,
  `mesh`, on by default: Will Shields' phone hotspot, NAT and ATAK CoT multicast relay, with
  the HaLow radio as a mesh point instead of a station. Two boards on the desk: both come up
  as mesh nodes, see each other, NAT their hotspots onto the mesh and join the CoT group on
  both sides. Phones on different boards should see each other in ATAK. Untested with ATAK
  itself so far.

- Mesh chat runs on two boards: both bring up the mesh on the stock chip firmware, find
  each other from beacons, discover each other's chat in under 20 s, and unicast pings
  across the mesh answer in about 15 ms.
- First range test (2026-10-18): a phone on one board chatted with the other board 245 ft
  away through the wall of a house with aluminium siding, with the TX power still capped
  at 8 dBm (the bench setting; the chip allows far more).
- Relaying through a middle board is implemented but not yet tested (needs a third board
  out of range of the first).
- Not implemented: 802.11s peering, HWMP path selection, encryption. The mesh is open and
  uses flooding with a hop limit, duplicate suppression and routes learned from traffic.
- Photos work and double as a speed test: a 300 KB photo crosses from one board to the
  other in 2.6 s, about 900 kbit/s on the desk at a 2 MHz channel, with rate selection by
  signal strength and 32 KB TCP windows. At the SDK's defaults it was 200 kbit/s. Chat and
  photos are fine, video is not; the ceiling is the ESP32 feeding the chip one frame at a
  time, not the radio.

## Quick start

Two firmwares to choose from. The gateway is the one for ATAK and OpenMANET work; the chat
is the one with the chat page and photos.

**Gateway (mesh role):**

```bash
. ~/esp/esp-idf/export.sh
cd firmware/openmanet-gateway
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Join `xiao-gateway-xxxx` (password `openmanet`), open `http://172.16.50.1/`, set a login
password on first use. The overview shows the HaLow Mesh card: state, mesh name and channel,
IP on the mesh, neighbours with signal, routes, frame counters. ATAK on a phone on one board
and ATAK on a phone on another board should see each other through the CoT relay. The
serial console's `gwcfg-status` prints the same. Mesh name, channel and power are build-time
settings under "OpenMANET XIAO Gateway: mesh role" in `idf.py menuconfig`; the chat and the
gateway can share a mesh if both use the same name and channel.

**Chat:**

```bash
. ~/esp/esp-idf/export.sh
cd firmware/halowmesh
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Flash every board with the same build. On a phone, join `HalowChat-XXXX` (password
`halowchat`), open `http://10.42.100.1`, type a name, chat. Each board shows its radio
neighbours, the boards it can reach, and a ping time to each.

Requirements: ESP-IDF v5.4.4, the `morsemicro/halow` component 2.11.2-esp32-2 (fetched by
the build), a 2 A power supply per board for full power in the field.

## Roadmap: joining the OpenMANET ecosystem

The hope is for these cheap boards to become part of the OpenMANET world: XIAO nodes
meshing among themselves and, where a Raspberry Pi node is present, handing traffic into an
OpenMANET network. OpenMANET runs OpenWrt on a Pi with a HaLow HAT, 802.11s mesh with a mesh
password, and batman-adv (BATMAN_V) routing on top. To talk to it, a XIAO has to speak both
layers. Here is the plan, in the order it will be attempted. None of it is promised; each
step gets its own probe first, the way everything above was done.

**Step 1, done: the radio.** Mesh interface, Mesh ID beacons, peering frames and 4-address
data frames all work on the chip from the ESP32. This repository.

**Step 2, done: our own mesh.** Flooding, duplicate suppression, learned routes, IP on top,
chat on top of that. Works between two boards; relaying through a third is next to test.

**Step 3: the gateway, joined to the mesh.** Will Shields' OpenMANET-XIAO-Gateway (now in
`firmware/openmanet-gateway`) already does the XIAO side of an OpenMANET node: phone hotspot,
NAT, DNS forwarding, an ATAK CoT multicast relay, a web UI with login, OTA partitions. It was
designed to join a HaLow access point on a Pi, and its own research found the catch: no
OpenMANET role exposes the Pi's HaLow radio as a plain access point, and a Pi with one HaLow
HAT has only one radio, which is busy being a mesh point. Their notes call it a real gap in
the ecosystem. Our mesh layer is the other way round that gap: the gateway's uplink becomes
the mesh instead of a station link, so XIAOs form an ATAK-capable net among themselves now,
and join the Pi's mesh once steps 4 to 6 land. The work: a `uplink_mesh` module in the
gateway that wraps `mesh.c`, a third node role next to its client and relay roles.

**Step 4: 802.11s peering.** The real handshake with a neighbour: Mesh Peering Open, Confirm
and Close with link ids, timers and retries. The frames already fly; the state machine does
not exist yet. With this, a XIAO is a proper mesh point and could peer with a Linux mesh
point that has security off.

**Step 5: the mesh password.** OpenMANET nodes encrypt the mesh: SAE authentication between
peers, then the authenticated peering exchange, then per-neighbour keys loaded into the
chip. The chip does the encryption; the handshake code is the work. The mesh versions of
that code exist in the SDK tree uncompiled (`mesh_rsn.c`, `mesh_mpm.c`) but are written
against Linux driver calls. Weeks of careful crypto plumbing, and the step that lets a XIAO
peer with a real OpenMANET node at the radio layer.

**Step 6: batman-adv on the ESP32, if possible.** (Researched in detail:
`docs/batman-adv-on-esp32.md`, with the wire format, what a minimal node must do, and a
test plan that needs no Pi.) Peering gets a XIAO heard; batman-adv gets
it understood. Everything OpenMANET sends rides inside batman-adv frames (ethertype 0x4305):
ELP hellos every half second, OGMv2 route announcements, translation tables with checksums
that say which client addresses sit behind which node, and its own unicast and broadcast
wrapping. The plan is a minimal BATMAN_V node, not a port of the Linux module: send ELP and
OGMv2 so other nodes see us, learn originators and next hops from their OGMv2, announce our
own address (and our phone hotspot clients) in the translation table, answer translation
table requests, wrap and unwrap unicast and broadcast. The Linux batman-adv source and the
open-mesh.org protocol documents are the reference; a Pi running OpenMANET is the test rig.
This is the biggest piece and the least certain. If it works, a XIAO becomes a first-class
OpenMANET node with no Pi needed except where internet or the heavy apps live.

**Step 7: the ecosystem.** OpenMANET's apps run over IP on the mesh: position sharing, ATAK
over multicast, push-to-talk. Once a XIAO is a batman node, the first targets are passing
ATAK multicast between a phone on the XIAO's hotspot and the rest of the net, and a small
status beacon so OpenMANET's UI can show XIAO nodes. Where the mesh needs a Pi (gateway,
camera, voice), the XIAOs extend its reach cheaply.

Help is welcome on any step, especially from anyone who knows batman-adv internals, 802.11s
security, or the Morse Micro Linux driver.

## Credits and references

- Will Shields (d3mocide) for OpenMANET-XIAO-Gateway, imported here with its full history:
  https://github.com/d3mocide/OpenMANET-XIAO-Gateway. The phone hotspot, NAT, DNS forwarding,
  ATAK CoT relay, web UI, provisioning and OTA work in `firmware/openmanet-gateway` is his
  (built, as his commit log shows, with Claude as well), as is the research in his
  `design/PI_SIDE.md` on what an OpenMANET Pi actually exposes. His project was found through
  the fork at https://github.com/Yverr0y/OpenMANET-XIAO-Gateway.
- Claude Fable 5.1 (Anthropic): research, firmware and documentation, working with me in
  Claude Code.
- Morse Micro: the MM6108, morselib (microcontroller SDK, now published with source) and
  the open-source Linux driver whose command header provided the mesh command.
  https://github.com/MorseMicro
- Seeed Studio: XIAO ESP32S3 and the Wio-WM6180 module.
- Teapot Labs `mm-esp32-halow` for the bench power lessons on this board.

## Licence

GPL-3.0-or-later for the repository as a whole. See `LICENSE`.

Why: `firmware/openmanet-gateway` is Will Shields' work under GPL-3.0, and the build pulls in
Morse Micro's `morselib` (GPL-3.0-or-later or a commercial licence from Morse Micro) and their
chip firmware (their binary distribution licence). This repository does not contain the Morse
Micro parts; follow their rules for those, especially if you redistribute built firmware.

The files we wrote ourselves (`firmware/halowmesh`, `firmware/mesh-probe`, `firmware/halowchat`,
marked `SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later`) are additionally offered under
0BSD, see `LICENSES/0BSD.txt`: take those on their own for anything, no conditions, credit
welcome and never required. Open with love for the community.
