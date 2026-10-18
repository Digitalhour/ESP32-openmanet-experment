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

## Credits and references

- Claude Fable 5.1 (Anthropic): research, firmware and documentation, working with me in
  Claude Code.
- Morse Micro: the MM6108, morselib (microcontroller SDK, now published with source) and
  the open-source Linux driver whose command header provided the mesh command.
  https://github.com/MorseMicro
- Seeed Studio: XIAO ESP32S3 and the Wio-WM6180 module.
- Teapot Labs `mm-esp32-halow` for the bench power lessons on this board.

## Licence

GPL-3.0-or-later. See `LICENSE`.

Why that one: `morselib`, the Morse Micro SDK library this firmware links against, is
published under GPL-3.0-or-later (or a commercial licence from Morse Micro), and the
esp-halow wrapper is Apache-2.0. A GPL-compatible licence for this code keeps the whole
build consistent. The build fetches morselib itself; it is not part of this repository.
