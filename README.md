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

## Status (2026-10-18)

- Mesh chat runs on two boards on the desk: both bring up the mesh on the stock chip
  firmware, find each other from beacons, discover each other's chat in under 20 s, and
  unicast pings across the mesh answer in about 15 ms.
- Relaying through a middle board is implemented but not yet tested (needs a third board
  out of range of the first).
- Not implemented: 802.11s peering, HWMP path selection, encryption. The mesh is open and
  uses flooding with a hop limit, duplicate suppression and routes learned from traffic.
- Throughput is whatever the lowest HaLow rate gives (chat is fine, video is not).

## Quick start

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

**Step 3: a Pi as the door, no changes to the XIAO.** An OpenMANET Pi could run a HaLow
access point on the same radio next to its mesh interface. A XIAO joins that access point as
a plain station (the hub firmware in `firmware/halowchat` already does this) and the Pi
bridges it into the batman network like any other client. This is the quickest way to get a
XIAO's traffic into an OpenMANET net, and it needs to be checked first: whether Morse's Linux
driver allows mesh and access point on one radio at the same time.

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

**Step 6: batman-adv on the ESP32, if possible.** Peering gets a XIAO heard; batman-adv gets
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

- Claude Fable 5.1 (Anthropic): research, firmware and documentation, working with me in
  Claude Code.
- Morse Micro: the MM6108, morselib (microcontroller SDK, now published with source) and
  the open-source Linux driver whose command header provided the mesh command.
  https://github.com/MorseMicro
- Seeed Studio: XIAO ESP32S3 and the Wio-WM6180 module.
- Teapot Labs `mm-esp32-halow` for the bench power lessons on this board.

## Licence

Everything we wrote here is 0BSD: free and open, with love for the community. Use it,
copy it, change it, build on it, sell it, for anything. Credit is welcome, never required.
See `LICENSE`.

The Morse Micro parts are theirs, not ours. The build fetches `morselib` (their SDK library,
GPL-3.0-or-later or a commercial licence from Morse Micro) and their chip firmware (their
binary distribution licence). This repository does not contain either. Follow Morse Micro's
rules for those, especially if you redistribute built firmware.
