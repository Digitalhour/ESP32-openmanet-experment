# HalowChat hub version (v1)

This is the first HalowChat firmware: one board is the hub (a HaLow access point), the others join it as stations. It works and stays here as a fallback. The mesh version in `../halowmesh` replaces it: no hub, boards relay for each other. The rest of this file is the original description.


A group chat over Wi-Fi HaLow for phones, using Seeed XIAO ESP32S3 + Wio-WM6180 boards. Every board runs the same firmware; each one gives a phone a Wi-Fi hotspot and a chat page, and the boards pass messages between themselves over HaLow. Started 2026-10-10 on the hardware from the parked Ottercam project (`../Ottercam`).

## How it works

- **Roles decide themselves.** At boot a board scans for the `HalowChat` HaLow network. If it finds one it joins as a member; if not, it starts the network as the hub. A member that loses the hub for 2 minutes reboots and takes over as the hub. A board can also be pinned as hub or member (setting `CHAT_ROLE`, or `role` in `POST /api/settings`), which the bench needs: the board on the dock's weak port can only be the hub.
- **Phone hotspot per board:** `HalowChat-XXXX` (XXXX from the board's own address), password `halowchat` (Kconfig `CHAT_WIFI_PSK`), page at `http://10.42.100.1`.
- **Messages** are UDP broadcasts on the HaLow network with an ID. Every board rebroadcasts each new packet once (up to 4 hops), so a message reaches everyone even if the hub doesn't forward broadcasts between members. Unacknowledged messages are re-sent up to 3 times.
- **Two receipts:** "delivered to N boards" when boards have the message; "seen by Chris (01C4)" once the page on that board has shown it to a person.
- **History:** the last 50 messages are saved on each board and restored after a reboot. Up to 300 are kept in PSRAM while running.
- **Peers:** boards announce themselves every 20 s; the page shows who is online.

## Build and flash

Same toolchain as Ottercam (ESP-IDF v5.4.4, `morsemicro/halow 2.11.2-esp32-2`):

```bash
. ~/esp/esp-idf/export.sh
```

```bash
cd ~/apps/HalowChat/firmware/halowchat && idf.py set-target esp32s3 && idf.py -p /dev/cu.usbmodemXXXX build flash
```

Settings are under `idf.py menuconfig` → HalowChat: network name and passphrase, channel (default 26, 2 MHz), TX power cap (default 8 dBm; keep it low on the desk), role, hotspot password and channel, ports.

## Bench rules (learned the hard way; details in `../Ottercam/firmware/halow-bridge/README.md`)

- Power each board from a port that can deliver current. On weak USB ports a board resets at its first scan probe; that board can still be the hub.
- Keep boards a few feet apart at low TX power; a few inches apart at full power they overload each other.
- Expect about 1–2 Mbit/s over HaLow between these boards; plenty for text.

## Not done yet

- Photos (removed after a first attempt; the link is slow for full-size images, and the page needs a fallback for formats the browser can't decode).
- A third and fourth board (needs one more XIAO and a good Wio-WM6180 each).
- Multi-hop without a hub (802.11s mesh via the Teapot Labs esp-halow fork; see the Ottercam roadmap, section 4).
