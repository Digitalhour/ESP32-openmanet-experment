# HalowChat mesh firmware

Group chat over a Wi-Fi HaLow mesh. Every board runs this firmware, gives a phone a Wi-Fi
hotspot and a chat page, and passes messages to the other boards over 915 MHz HaLow with no
hub: each board beacons like an 802.11s mesh point, finds its neighbours from their beacons,
and relays frames for boards it cannot hear directly.

Hardware: Seeed XIAO ESP32S3 + Seeed Wio-WM6180 Wi-Fi HaLow module (Morse Micro MM6108).
Toolchain: ESP-IDF v5.4.4 and the `morsemicro/halow` component 2.11.2-esp32-2.

## What is in here

| File | Job |
|---|---|
| `main/mesh.c`, `main/mesh.h` | The mesh layer. Brings the mesh interface up on the chip, beacons with a Mesh ID, learns neighbours, carries Ethernet frames in 4-address mesh data frames, floods broadcasts with a hop limit and duplicate suppression, learns routes for unicast, and presents itself as an esp_netif so IP works on top. |
| `main/chat_main.c` | The chat: phone hotspot, web page and API, UDP chat protocol over the mesh (messages, delivered and seen receipts, retries, saved history, pings between boards). |
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

Rates: frames currently use morselib's management rate table (the lowest MCS). Plenty for
chat, slow for anything bigger. A data rate table is the obvious next improvement.

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
`POST /api/send {name, text, ts}`, `POST /api/settings {name, tx_dbm}`, `POST /api/reboot`.

## Results so far (2026-10-18)

Two boards on the desk, 2 MHz channel 26, 8 dBm:

- Both bring the mesh up on the stock chip firmware (1.17.8) and hear each other's Mesh ID
  beacons within a few seconds.
- Boards discover each other through the chat's UDP broadcast within 20 s.
- Unicast pings over the mesh: 14 to 29 ms, 6 of 6 answered.

Multi-hop relaying is implemented but untested: it needs a third board placed out of range
of the first. Mesh peering (the 802.11s handshake), path selection (HWMP) and encryption are
not implemented; the mesh is open and uses flooding plus learned routes.

## Research behind it

`../mesh-probe` holds the experiments that found the recipe: which interface types the chip
accepts, the beacon scheduling command, over-the-air confirmation with a second board,
peering action frames both ways and 4-address data frames both ways. Its README has the
details and the numbers.
