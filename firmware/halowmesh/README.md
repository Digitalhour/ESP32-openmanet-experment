# HalowChat mesh firmware

Group chat over a Wi-Fi HaLow mesh. Every board runs this firmware, gives a phone a Wi-Fi
hotspot and a chat page, and passes messages to the other boards over 915 MHz HaLow with no
hub: each board beacons like an 802.11s mesh point, finds its neighbours from their beacons,
and relays frames for boards it cannot hear directly.

Hardware: Seeed XIAO ESP32S3 + Seeed Wio-WM6180 Wi-Fi HaLow module (Morse Micro MM6108).
Toolchain: ESP-IDF v5.4.4 and the `morsemicro/halow` component 2.11.2-esp32-2.

Written with Claude Fable 5.1 by someone who is not an embedded developer; tested on two
boards on a desk. Use at your own risk. See the warning in the top-level README.

## What is in here

| File | Job |
|---|---|
| `main/mesh.c`, `main/mesh.h` | The mesh layer. Brings the mesh interface up on the chip, beacons with a Mesh ID, learns neighbours, carries Ethernet frames in 4-address mesh data frames, floods broadcasts with a hop limit and duplicate suppression, learns routes for unicast, and presents itself as an esp_netif so IP works on top. |
| `main/chat_main.c` | The chat: phone hotspot, web page and API, UDP chat protocol over the mesh (messages, delivered and seen receipts, retries, saved history, pings between boards), and photos: the phone uploads to its board, the board announces it, the other boards pull it over the mesh with HTTP and show how long it took. |
| `main/chat_ui.html` | The chat page served to the phone. |
| `main/Kconfig.projbuild` | Settings: mesh name, channel, TX power cap, hop limit, hotspot password, ports. |

## How the mesh layer works

Morse Micro's microcontroller SDK (morselib) has no mesh mode. The chip does: the Linux
driver runs 802.11s on the same chip firmware. The mesh layer reuses morselib's access-point
code and changes three things on the way to the chip, through linker wraps on morselib's
internal functions (the esp-halow build renames them with an `mmint_` prefix):

1. **Interface type.** When morselib asks the chip for its access-point interface, the wrap on
   `mmint_mmdrv_add_if` sends the same request with interface type MESH (5) instead of AP (2).
2. **Mesh beaconing.** One extra chip command, `MESH_CONFIG` (0x0039, start, beaconing on),
   taken from the Linux driver's command header with the Linux driver's default timing values.
   Without it the chip accepts the interface but never schedules beacons. With zeroed timing
   fields it crashes the chip, so a flag in NVS makes sure a crash cannot loop.
3. **Mesh ID.** The chip asks the host for every beacon; morselib builds it with an SSID
   element. The wrap on `mmint_umac_ap_get_beacon` rewrites that element's id byte into a Mesh
   ID element (114). Same length, one byte changed.

Data frames are built by `mesh.c` itself: QoS data, To DS and From DS set (four addresses),
a Mesh Control field (flags, TTL, 32-bit sequence), LLC/SNAP, then the Ethernet payload. They
go out through morselib's management transmit helper, which fills in the chip metadata, and a
wrap on `mmint_mmdrv_tx_frame` marks them as data. Every frame the chip hands the host passes
through a wrap on `mmint_mmdrv_host_process_rx_frame`: beacons with our Mesh ID feed the
neighbour table, 4-address data frames go to the mesh task, everything else goes on to
morselib as before.

**Peering (802.11s Mesh Peering Management, open security).** Beacons carry the Mesh
Configuration element (HWMP, airtime metric, no congestion control, neighbour offset sync,
open authentication, accepting peerings). Hearing a matching beacon from a new neighbour
starts a handshake after a short random delay: Mesh Peering Open with our link id, answered
with Open plus Confirm; the state machine (IDLE, OPN_SNT, OPN_RCVD, CNF_RCVD, ESTAB, HOLDING)
follows 802.11-2020 14.3.3, with the Open resent on a timer that doubles from 250 ms up to
four times when a frame is lost, and Close on retry or confirm timeouts. Data frames from a
neighbour that is not in ESTAB are dropped, routes only use ESTAB next hops, and a peer whose
beacons stop for 45 s is closed. Two boards peer within a second of boot. `CHAT_MESH_PEERING`
turns the requirement off (trust on sight), which is what the earlier builds did.

Forwarding rules, in the mesh task:

- Drop frames whose source is us (our own flood coming back) and frames already seen
  (source address plus mesh sequence, ring of 128).
- Learn a route: a frame from source S that arrived via transmitter T means T is a next hop
  towards S, at (hop limit minus TTL plus 1) hops. Fewer hops wins; stale routes are replaced.
- Deliver frames addressed to us or to a group address to lwIP as an Ethernet frame.
- Relay group frames always (TTL minus 1, broadcast), and unicast frames only when we were
  the chosen next hop (towards the learned next hop, or flooded if none is known).

Transmit from lwIP: group addresses are flooded; unicast goes to the learned next hop, or is
flooded when no route is known yet. The first packet to a new board floods, the reply teaches
the route, and from then on it is unicast with 802.11 acknowledgements.

Rates: broadcasts go at MCS0 like morselib's management frames. Unicast frames start higher
when the next hop's beacons are strong (MCS5 above -55 dBm, down to MCS1 near -86 dBm) and fall
back down a three-step chain to MCS0 if the chip cannot get them through. Measured with a
300 KB photo between two boards on the desk: about 500 kbit/s with this, 130 to 200 kbit/s
at MCS0 only.

## Addresses

Each board's mesh address is its HaLow module's MAC with the locally administered bit set
(the address morselib's access-point code uses). Its IP on the mesh is `10.42.0.<last byte
of that address>`, netmask /24, no gateway. The phone hotspot is `HalowChat-XXXX` (XXXX from
the ESP32's own MAC), password `halowchat`, page at `http://10.42.100.1`. Phones only ever
talk to their own board; the boards talk to each other over the mesh.

## Build and flash

```bash
. ~/esp/esp-idf/export.sh
cd firmware/halowmesh
idf.py set-target esp32s3
idf.py menuconfig        # optional: mesh name, channel, hotspot password
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Flash every board with the same build. There is nothing to pair: boards with the same mesh
name on the same channel find each other within about 10 s of boot.

Bench rules learned the hard way (see the Ottercam bridge README): keep the TX power cap at
8 dBm or below with boards a few feet apart, and feed the boards from a 2 A supply for full
power in the field. Weak USB ports reset the module when it transmits long frames.

## What the page shows

- `mesh · 2 near`: the mesh is up and two radio neighbours are heard. `mesh · alone` means no
  beacons from anyone else yet.
- Boards online, with a ping time over the mesh to each one (unicast, every 10 s).
- Radio neighbours with RSSI and when they were last heard.
- Mesh frame counters: sent, failed, received, relayed, duplicates dropped, routes known.

`GET /api/status` returns all of it as JSON. `GET /api/messages?after=N&name=You`,
`POST /api/send {name, text, ts}`, `POST /api/photo?name=You&ts=... (image as the body)`,
`GET /api/photo/<id>`, `POST /api/settings {name, tx_dbm}`, `POST /api/reboot`.

Under a received photo the page shows "300 KB in 5.1 s, 481 kbit/s over the mesh": that is
the real transfer time between the two boards, a direct measure of what the mesh carries.

## Results so far (2026-10-18)

Two boards on the desk, 2 MHz channel 26, 8 dBm:

- Both bring the mesh up on the stock chip firmware (1.17.8) and hear each other's Mesh ID
  beacons within a few seconds.
- Boards discover each other through the chat's UDP broadcast within 20 s.
- Unicast pings over the mesh: 14 to 29 ms, 6 of 6 answered.
- Range: chat between a phone on one board and the other board 245 ft away, through an
  aluminium-sided house wall, at the 8 dBm bench power cap.
- Photos: a 300 KB photo crosses board to board in 2.6 s (935 kbit/s) on the 2 MHz
  channel with 32 KB TCP windows. Phones shrink photos to 1024 px JPEG before upload; the
  limit is 400 KB per photo, six photos kept per board in PSRAM, not kept across a reboot.

### Speed, and why it is not 15 Mbit/s

HaLow's headline numbers are the radio's raw rate on an 8 MHz channel at its highest
modulation in perfect conditions. What a transfer actually gets depends on the channel
width, the modulation the link can hold, how the host feeds the chip, and TCP. Measured
here with a 300 KB photo between two boards on the desk:

| Channel | TCP window | Rate selection | 300 KB took | Speed |
|---|---|---|---|---|
| 2 MHz | 5.7 KB (default) | MCS0 only | 12.0 s | 205 kbit/s |
| 2 MHz | 5.7 KB | by RSSI | 5.1 s | 481 kbit/s |
| 2 MHz | 32 KB | by RSSI | 2.6 s | 935 kbit/s |
| 8 MHz | 32 KB | by RSSI | 3.2 s | 777 kbit/s, lossier pings |

The 8 MHz channel (opclass 71, channel 44) quadruples the radio's rate per modulation but
needs 6 dB more signal, so it did not help on this path and would cost range in the field.
The remaining ceiling, around 1 Mbit/s, is the ESP32 side: every frame is built by the host
and handed to the chip one at a time, with no frame aggregation, the same limit the Ottercam
bridge hit on this hardware. The next steps for speed, in order, are frame aggregation
(A-MPDU through morselib's data path), short guard interval, and fewer copies in the
receive path.

TX power is a different knob: it buys range, not speed. The bench cap of 8 dBm keeps boards
on one desk from overloading each other; in the field, raise it from the page.

Multi-hop relaying is implemented but untested: it needs a third board placed out of range
of the first. The 802.11s peering handshake is implemented (open security); the mesh
password (SAE and authenticated peering) and HWMP path selection are not. Routing is flooding
plus learned routes.

Open item: the ping round trip between two boards varies from 15 ms to 90 ms from one boot
to the next, with beacon reception between 65 and 100 percent, while throughput stays
around 800 to 900 kbit/s. The host side was measured clean (chip transmit call 0.3 ms,
receive hand-off 30 µs), so the variation is in the chip or on the air; the likely cause is
the two boards' beacon times landing close together after some boots, which a packet capture
on a Linux HaLow node (the HaLowLink) will settle.

## Research behind it

`../mesh-probe` holds the experiments that found the recipe: which interface types the chip
accepts, the beacon scheduling command, over-the-air confirmation with a second board,
peering action frames both ways and 4-address data frames both ways. Its README has the
details and the numbers.
