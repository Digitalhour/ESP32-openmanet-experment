# ESP32 HaLow hop nodes for OpenMANET

Cheap, disposable relays for a Wi-Fi HaLow mesh (802.11ah, 915 MHz), built on a Seeed XIAO
ESP32-S3 and the Seeed Wi-Fi HaLow module for XIAO (Morse Micro MM6108). About 22 dollars of
boards. The aim: put one high up, in a tree, on a pole, on a roof, wherever an OpenMANET mesh
needs another hop, and not care much if it is lost or forgotten. Phones, ATAK and the heavy
work stay on the Raspberry Pi nodes; these extend the reach.

Two things in this repository did not exist before:

- **An 802.11s-style mesh on the MM6108 from a microcontroller.** Morse Micro's SDK offers
  station and access point only, and Morse has said mesh is not supported on their
  microcontroller offering. The chip does it (that is what Linux runs), and the ESP32 side
  turned out to be three small hooks into the SDK and one extra chip command, all on the
  stock chip firmware. Beacons with a Mesh ID, the peering handshake, 4-address data frames.
- **batman-adv (BATMAN_V), the routing OpenMANET uses, running on an ESP32.** Reimplemented
  from the Linux kernel source. A Linux batman-adv node sees it as a peer: neighbour,
  originator, translation table, pings at every packet size. It runs over the HaLow mesh link
  between XIAOs.

`firmware/hopnode` is those two layers and nothing else: the hop node.

## What works today (2026-10-21)

| | Result |
|---|---|
| Mesh interface, Mesh ID beacons, mesh data frames on the chip | works, stock chip firmware |
| 802.11s peering (Open, Confirm, Close, retries), data gated on it | works, two XIAOs peer in about 1 s; a Morse HaLowLink 1 (Linux, stock OpenWrt) lists both XIAOs as established peers |
| batman-adv against a Linux batman-adv node, over 2.4 GHz Wi-Fi | seen, routed, translated; pings 20 of 20, full-size and 8000-byte pings pass |
| batman-adv between two XIAOs over HaLow | both route each other, bat0 pings across the mesh |
| batman-adv with a Linux node on the HaLow mesh (HaLowLink 1 running batman-adv over its mesh interface) | `batctl` shows both XIAOs as neighbours, originators and translation-table clients; IP pings 10 of 10 to each XIAO, 1400-byte pings 5 of 5, `batctl ping` 5 of 5, traceroute one hop |
| Chat with photos over the XIAO mesh (no batman) | 245 ft through an aluminium-sided wall at 8 dBm; 300 KB photo in 2.6 s |
| Will Shields' OpenMANET gateway (hotspot, NAT, ATAK CoT relay) on the mesh | both boards come up as mesh nodes, NAT on, CoT group joined; untested with ATAK itself |
| The mesh password (SAE) a real OpenMANET net requires | not yet |
| Relaying through a middle node | implemented, needs a third board to prove |

## Who made this, and a warning

I am a PHP and Python developer. I know very little about the ESP32, ESP-IDF, or the Morse
Micro SDK. This project was built with Claude Fable 5.1 (Anthropic's AI model) doing the
research, the firmware code and most of the documentation, while I ran the hardware, tested,
and decided what to build next. Read the code with that in mind: it works on my two boards,
on my desk and across my house, and that is the extent of the testing so far.

Use at your own risk. This software is provided as is, with no warranty of any kind. I am
not responsible for any damage, loss, or regulatory trouble that may come from using it.
In particular: it drives a radio transmitter, so check your local rules for the 900 MHz band
(the firmware is set up for the US channel plan), it talks to the HaLow chip in ways its
vendor does not support, and it can reset or crash the chip and the board. If that worries
you, do not flash it.

## The idea: cheap, disposable hop nodes

At Seeed's prices as of October 2026 the two boards come to about 22 dollars: the Wi-Fi HaLow
transceiver for XIAO at 14.90 and the XIAO ESP32-S3 at 7.49. Add a battery and a box. Each
node relays for the whole mesh and announces itself the way every other OpenMANET node does.

**Build your own, cheaper.** Nothing here is tied to the XIAO. It is plain ESP-IDF plus Morse
Micro's `morsemicro/halow` component, which Morse tests on the ESP32-C3, C6 and C5 as well as
the S3. The hop node needs no PSRAM, no USB and no 2.4 GHz radio. A bare ESP32-C3 module and
the HaLow module on a small carrier board with a battery connector is a relay for well under
20 dollars; the only firmware change is the pin map in `sdkconfig.defaults`. If you make one,
tell us what you built.

## The firmwares

| Directory | What |
|---|---|
| `firmware/hopnode` | **The hop node.** 802.11s mesh link with peering plus batman-adv (BATMAN_V). No hotspot, no page. Two of them peer, route over HaLow and ping each other across bat0; a Linux mesh point running batman-adv (HaLowLink 1) peers and routes with them too. |
| `firmware/openmanet-gateway` | Will Shields' OpenMANET-XIAO-Gateway, imported with its full history (GPL-3.0), plus a `mesh` node role: his phone hotspot, NAT, DNS forwarding, ATAK CoT multicast relay, web UI with login and OTA partitions, with the HaLow radio as a mesh point. |
| `firmware/halowmesh` | HalowChat: group chat with photos over the XIAO mesh, a phone hotspot and chat page per board. Doubles as a range and speed tester. |
| `firmware/batman-probe` | batman-adv on the ESP32 over 2.4 GHz Wi-Fi, for testing against a Linux batman-adv node on a LAN (`docs/linux-batman-peer.sh` sets one up). |
| `firmware/mesh-probe` | The experiments that found the mesh recipe: interface types, beacon scheduling, over-the-air checks, peering frames, 4-address data. Each one written up with its numbers. |
| `firmware/halowchat` | The first chat, hub based (one board is a HaLow access point). Works, kept as a fallback. |
| `docs/` | `batman-adv-on-esp32.md`: the protocol as implemented, from the kernel source, with results. `halowlink1-mesh.sh` and `halowlink1-batman.sh`: put a Morse HaLowLink 1 into open mesh point mode and run batman-adv over it, the Linux peer used for the tests. `linux-batman-peer.sh`: the same for a LAN test over Wi-Fi. |

Shared pieces: `mesh.c` (the mesh link) and `batadv.c` (batman-adv) are the same files in
every firmware that uses them.

## Quick start

Requirements: ESP-IDF v5.4.4, the `morsemicro/halow` component 2.11.2-esp32-2 (fetched by
the build), a 2 A power supply per board for full power in the field. Boards on a desk
should sit a few feet apart; closer than that they overload each other's receivers.

**Hop node:**

```bash
. ~/esp/esp-idf/export.sh
cd firmware/hopnode
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Flash every node with the same build. Mesh name, channel, TX power and the bat0 subnet are
under "hopnode" in `idf.py menuconfig`. The serial log shows mesh neighbours and their
peering state, batman neighbours, originators and clients, and a ping to every known node
every 15 s. A Linux batman-adv node on the same mesh would list it in `batctl n`, `batctl o`
and `batctl tg`.

**Gateway (mesh role):**

```bash
cd firmware/openmanet-gateway
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Join `xiao-gateway-xxxx` (password `openmanet`), open `http://172.16.50.1/`, set a login
password on first use. The overview shows the HaLow Mesh card. ATAK on a phone on one board
and ATAK on a phone on another should see each other through the CoT relay. The serial
console's `gwcfg-status` prints the same.

**Chat:**

```bash
cd firmware/halowmesh
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

On a phone, join `HalowChat-XXXX` (password `halowchat`), open `http://10.42.100.1`, type a
name, chat, send photos. Each board shows its radio neighbours, peering state, the boards it
can reach and a ping time to each. The chat and the gateway can share a mesh if both use the
same mesh name and channel; the hop node too.

## How the mesh works on this chip

Morselib's access-point code does almost everything a mesh point needs: it configures the
BSS, builds every beacon on the host, and transmits management frames the host builds. Three
hooks change the rest, through linker wraps on morselib's internal functions (the esp-halow
build prefixes them with `mmint_`):

1. When morselib asks the chip for its access-point interface, send the same request with
   interface type MESH (5) instead of AP (2).
2. Send one extra chip command, `MESH_CONFIG` (0x0039, start, beaconing on), taken from the
   Linux driver's command header, with the Linux driver's default timing values. Without it
   the chip accepts the interface but never schedules beacons; with zeroed timing fields it
   crashes the chip.
3. In every beacon morselib builds, rewrite the SSID element into a Mesh ID element and
   append a Mesh Configuration element.

Data frames are built by `mesh.c`: QoS data, four addresses for unicast and three for group
frames, a Mesh Control field, LLC/SNAP, the Ethernet payload. They go out through morselib's
management transmit helper and a wrap marks them as data. Every frame the chip hands the host
passes through a wrap on the receive entry, which takes the mesh frames and leaves the rest
to morselib. Peering is the standard 802.11s Open, Confirm and Close exchange with link ids
and retries. Details, numbers and the probes that found each piece:
`firmware/mesh-probe/README.md` and `firmware/halowmesh/README.md`.

Three things a Linux mesh point (Morse HaLowLink 1, stock OpenWrt 23.05 with Morse 2.7.6)
needed before it would talk to us, all found with its own logs and `tcpdump`:

- The Morse Linux driver drops a Mesh Peering Confirm that has no S1G Capabilities element,
  before mac80211 sees it (`morse_mac_capabilities_validate`). Open and Confirm now carry
  the same element our beacons do.
- mac80211 drops a group-addressed mesh data frame in the four-address form
  (`ieee80211_rx_mesh_check`). Broadcasts use the three-address form the standard asks for:
  From DS only, DA, TA, SA.
- A Linux mesh point will not unicast to a peer until an HWMP Path Request gets a Path
  Reply. `mesh.c` answers requests that name it and keeps no paths of its own.

## Roadmap: joining the OpenMANET ecosystem

OpenMANET runs OpenWrt on a Pi with a HaLow HAT, 802.11s mesh with a mesh password, and
batman-adv (BATMAN_V) routing on top. To be a node in it, a XIAO has to speak both layers.

1. **Done: the radio.** Mesh interface, Mesh ID beacons, peering frames, 4-address data.
2. **Done: our own mesh.** Flooding, duplicate suppression, learned routes, IP, chat on top.
3. **Done as far as it can be without a Pi: the gateway on the mesh.** Will Shields'
   OpenMANET-XIAO-Gateway was designed to join a HaLow access point on a Pi; his own research
   found that no OpenMANET role exposes one, and a Pi with one HaLow HAT has only one radio,
   busy being a mesh point. The mesh layer is the way around that gap: the gateway's uplink
   is the mesh.
4. **Done, open security: 802.11s peering.** Proven against a Linux mesh point: a Morse
   HaLowLink 1 on stock firmware lists both XIAOs as established peers, resolves paths to them
   and exchanges data both ways. The three fixes that took are in the section above.
5. **Next: the mesh password.** SAE between peers, the authenticated peering exchange, then
   per-neighbour keys loaded into the chip. The chip does the encryption; the handshake code
   is the work. The mesh versions of that code exist in the SDK tree uncompiled. This is the
   step that lets a XIAO peer with a real OpenMANET node. Deferred until the HaLowLink is here
   to test against.
6. **Done over Wi-Fi and over HaLow: batman-adv on the ESP32.** `docs/batman-adv-on-esp32.md`.
   With batman-adv running on the HaLowLink's mesh interface, `batctl` on the Linux side shows
   the XIAOs as neighbours, originators and translation-table clients, pings them over IP and
   with `batctl ping`, and traceroutes them. Joining a real OpenMANET net waits on step 5.
7. **The ecosystem.** ATAK multicast and position sharing between phones on XIAO gateways and
   the rest of the net, XIAO nodes visible in OpenMANET's UI, Pis only where internet, camera
   or voice live.

For the hop node itself, after step 5: a third board to show a packet crossing a hop; power
and enclosure for an outdoor, forgotten node; provisioning without a build (mesh name and
password from a page or the USB console). Help is welcome on any of it, especially from
anyone who knows batman-adv internals, 802.11s security, or the Morse Micro Linux driver.

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
- The Linux kernel's `net/batman-adv`, the reference for `batadv.c`.
- OpenMANET: https://openmanet.github.io/docs/
- Seeed Studio: XIAO ESP32S3 and the Wi-Fi HaLow module for XIAO.
- Teapot Labs `mm-esp32-halow` for the bench power lessons on this board.

## Licence

GPL-3.0-or-later for the repository as a whole. See `LICENSE`.

Why: `firmware/openmanet-gateway` is Will Shields' work under GPL-3.0, and the build pulls in
Morse Micro's `morselib` (GPL-3.0-or-later or a commercial licence from Morse Micro) and their
chip firmware (their binary distribution licence). This repository does not contain the Morse
Micro parts; follow their rules for those, especially if you redistribute built firmware.

The files we wrote ourselves (`firmware/hopnode`, `firmware/halowmesh`, `firmware/batman-probe`,
`firmware/mesh-probe`, `firmware/halowchat`, marked `SPDX-License-Identifier: 0BSD OR
GPL-3.0-or-later`) are additionally offered under 0BSD, see `LICENSES/0BSD.txt`: take those on
their own for anything, no conditions, credit welcome and never required. Open with love for
the community.
