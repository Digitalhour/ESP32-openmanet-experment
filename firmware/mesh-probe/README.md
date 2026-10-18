# mesh-probe: can the MM6108 on a XIAO do 802.11s mesh?

A one-evening experiment, run on 2026-10-18 on a XIAO ESP32S3 + Seeed Wio-WM6180
(Morse Micro MM6108, module MAC 3c:1a:cc:4c:85:12).

## Question

Linux + mac80211 runs 802.11s mesh on this chip (that is what OpenMANET and the HaLowLink
use). Morse Micro says mesh "is handled in the upper MAC" and that they "don't yet support
mesh for our microcontroller offering". The microcontroller SDK (morselib) only ever asks
the chip for STA and AP interfaces, and `mmdrv_add_if()` refuses anything else before the
request reaches the chip. Morselib's command set still defines `MON = 3`, `ADHOC = 4` and
`MESH = 5`. So: does the chip firmware the ESP32 loads (`mm6108.mbin`) accept them?

## Method

`main/probe_main.c` boots the chip the normal way (`mmhalow_init`), then uses a linker
wrap on morselib's command path (`mmint_morse_cmd_tx`; the ESP build renames morselib's
internal symbols with an `mmint_` prefix) to send its own `ADD_INTERFACE` requests with a
locally administered MAC, read the chip's status, and remove each interface again with
`mmint_mmdrv_rm_if()`. AP is sent first and last as the control. The probe never scans,
connects or transmits.

## Result

```
chip firmware 1.17.8, morselib 2.11.2, chip MM6108A1
  AP     add: SUCCESS   chip status 0, vif 1, removed OK
  MESH   add: SUCCESS   chip status 0, vif 1, removed OK
  ADHOC  add: SUCCESS   chip status 0, vif 1, removed OK
  MON    add: SUCCESS   chip status 0, vif 1, removed OK
  AP     add: SUCCESS   chip status 0, vif 1, removed OK
```

The IoT chip firmware creates a MESH interface (and ADHOC and MONITOR) without complaint,
and keeps answering afterwards. The door is open at the interface level.

## What this does and does not show

- Shows: the chip-side command set the microcontroller build uses is the same family the
  Linux driver uses, and the firmware does not reject the mesh interface type.
- Does not show: that mesh beacons, mesh peering frames, 4-address data frames and mesh
  path selection work through this firmware. Those are the next probes, in this order:
  1. `BSS_CONFIG` + `start_beaconing` on the mesh VIF with a beacon carrying a Mesh ID
     element; check it on a second board in MON mode (or on a Linux HaLow box).
  2. Raw management receive on the mesh VIF (mesh peering open/confirm from a Linux peer).
  3. 4-address data frames in both directions.
  4. Only then port the host side: wpa_supplicant's `mesh.c`/`mesh_mpm.c`/`mesh_rsn.c`
     (already in the SDK tree, not compiled) and mac80211's HWMP path selection.

## Running it again

```bash
cd HalowChat/firmware/mesh-probe
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

The result block prints about 15 s after boot. Afterwards reflash HalowChat (hub build
`CONFIG_CHAT_ROLE=1` on the board that is pinned as hub).

## Experiment 2: beacon on a mesh interface (2026-10-18, board F034)

`main/probe_main.c` with `PROBE_EXPERIMENT 2`. The board whose XIAO MAC ends in F034 is the
BEACONER, any other board is the LISTENER. The beaconer starts morselib's normal access-point
code but swaps the interface type to MESH on the way to the chip (a wrap on
`mmint_mmdrv_add_if`), then tries the extra commands the Linux driver sends for a mesh
interface. Every beacon the chip asks for passes through a wrap on `mmint_umac_ap_get_beacon`,
which counts it and, in the last phase, rewrites the SSID element into a Mesh ID element.

| Step | Chip firmware | Result |
|---|---|---|
| Control: AP code on a real AP interface | 1.17.8 (IoT) | chip asks for a beacon every 100 ms (99 in 10 s) |
| A: AP code on a MESH interface (`BSS_CONFIG` + start beaconing) | 1.17.8 (IoT) | commands accepted, chip asks for one beacon, then never again |
| D: `BSS_BEACON_CONFIG` enable (0x003D) on the mesh interface | 1.17.8 and 1.17.9 | chip status -32768 (unknown command), no change |
| C: `MESH_CONFIG` start, beaconing on (0x0039), all timing fields 0 | 1.17.8 (IoT) | **chip firmware assert**, board reboots ("FW assert 55dac3f589:811") |
| C: `MESH_CONFIG` start, beaconing on, Linux defaults (MBCA TBTT select, gap 25 ms, adjust 60 s, scan 2048 ms) | 1.17.9 (Linux `mm6108.bin`) | **SUCCESS, chip asks for beacons: 183 in 30 s** |
| B: SSID element rewritten to Mesh ID (114) | 1.17.9 | beacons keep flowing, 131 of 131 patched |

So with the Linux chip firmware image, the chip runs a mesh-type interface with host-built
beacons carrying a Mesh ID element. Still unknown: whether the IoT image (1.17.8) also works
once the timing fields are sane (the assert may have been the zeros), and whether the beacons
actually go out over the air, which the listener board answers.

### Using the Linux firmware image on the XIAO

The esp-halow build already knows how to do it: drop `mm6108.bin` from
`managed_components/morsemicro__halow/components/firmware/morse-firmware/firmware/` into the
project root and reconfigure. The build logs `FW_MBIN: exact match not found, substituted
.../mm6108.bin` and converts it to `.mbin` with Morse's own script. Morselib's compatibility
check only requires the same major version and a minor at least as new as its own, and the
image loads into the chip's RAM at every boot, so nothing is permanently changed. Delete the
file and reconfigure to go back to the IoT image.

### Where the mesh commands came from

Morselib's command header (set 57.0.0) lacks them; the Linux driver's `morse_commands.h`
(set 57.13.0, same major) has them:

- `MORSE_CMD_ID_MESH_CONFIG = 0x0039`: opcode (0 start, 1 stop), enable_beaconing,
  mbca_config, min_beacon_gap_ms, mbss_start_scan_duration_ms, tbtt_adj_timer_interval_ms.
- `MORSE_CMD_ID_BSS_BEACON_CONFIG = 0x003D`: enable. Rejected by both images here.

On Linux, mesh beacons go through the same beacon IRQ path as an access point: the chip
raises an interrupt before each target beacon time and the host builds the frame
(`beacon.c` in `morse_driver`). That is exactly what morselib does for its access point, which
is why the AP code could be reused unchanged.

### Over the air (both boards, 2026-10-18 09:06)

Listener 01C4 on the second board, F034 beaconing with the Mesh ID element. The listener
hooks the host's raw receive entry (`mmint_mmdrv_host_process_rx_frame`) because morselib's
scan code only reports probe responses. Result over 90 s of continuous 1 s scans:

- 836 S1G beacons received from 3e:1a:cc:4c:85:12 (F034's mesh interface), about 10 per
  second, every one carrying `114(9)'meshprobe'` (Mesh ID) and no SSID element, plus the
  TIM, S1G capability/operation, short beacon interval and WMM elements morselib's AP code
  adds. Frame control 0x001c, 95 bytes.
- The mesh interface also answers probe requests (probe responses with the SSID element,
  built by morselib's AP code; not patched).
- The receiving chip hands S1G beacons to the host while scanning, even though it is not
  associated with anything.

So the MM6108 on a XIAO transmits 802.11s-style mesh beacons and receives them, with the
Linux firmware image and three extra lines of host code (interface type, MESH_CONFIG, the
Mesh ID element).

### Stock IoT firmware works too (2026-10-18 09:16)

Same run with the IoT image (1.17.8) and the Linux driver's default MESH_CONFIG values:
`MESH_CONFIG` SUCCESS, 292 beacons in 30 s, Mesh ID patched into all of them, and the
listener kept receiving them. The earlier chip assert was caused by the zeroed timing
fields, not by the image. The Linux image is therefore **not required**; the project is back
on the stock image (no `mm6108.bin` in the project root).

## Experiment 3: mesh peering frames both ways (2026-10-18 09:18)

`PROBE_EXPERIMENT 3`, the same build on both boards. Each board brings up its mesh
interface with Mesh ID beacons, learns neighbours from their beacons (address, RSSI), and
every 2 s sends each neighbour a Mesh Peering Open action frame (category 15 self-protected,
with Mesh ID, Mesh Configuration and Mesh Peering Management elements), built on the host
with morselib's `build_mgmt_frame` and sent with `umac_datapath_tx_mgmt_frame_ap`. The raw
receive hook logs action frames.

Result on the desk (boards about 2 ft apart, 8 dBm, RSSI -27):

```
F034: TX mesh peering open #22 to 3e:1a:cc:4c:81:fa: SUCCESS   received 20 from it
01C4: TX mesh peering open #20 to 3e:1a:cc:4c:85:12: SUCCESS   received 18 from it
RX ACTION frame from 3e:1a:cc:4c:81:fa to 3e:1a:cc:4c:85:12: category 15 action 1
   = MESH PEERING OPEN, 54 bytes, rssi -29: 114(9)'meshprobe' 113(7) 117(4)
```

Both directions work: unicast management frames between two mesh interfaces, transmitted
by the host and delivered raw to the host on the other side. One frame in ten was lost at
this spacing, which is normal for unacknowledged bench traffic at receiver-overload RSSI.
Also seen: a board hears Mesh ID beacons on its plain station interface before its own mesh
interface is up, so discovery needs no scan.

Scorecard so far: mesh interface, mesh beacons on air, mesh beacons received, mesh
peering frames both ways. Still to do before any real mesh code: 4-address data frames.

## Experiment 4: 4-address mesh data frames both ways (2026-10-18 09:34)

Added to the mesh node: every 2 s each board sends each neighbour an 802.11s mesh data
frame: QoS data with To DS and From DS set (four addresses), the Mesh Control field (flags,
TTL 31, sequence) in the body, LLC/SNAP, then a text payload. The frame is built on the host
with `build_mgmt_frame`; morselib's management transmit helper fills in the chip metadata and
a wrap on `mmint_mmdrv_tx_frame` flips the frame to the data queue. The raw receive hook
logs 4-address data frames with their payload.

```
F034: TX 4-address data #15 to 3e:1a:cc:4c:81:fa: SUCCESS
01C4: RX 4-ADDRESS DATA #20: RA 3e:1a:cc:4c:81:fa TA 3e:1a:cc:4c:85:12
      DA 3e:1a:cc:4c:81:fa SA 3e:1a:cc:4c:85:12, 82 bytes, QoS + Mesh Control,
      ttl 31, rssi -24: "mesh data #18 from 3e:1a:cc:4c:85:12"
01C4 after 60 s: data sent 29 ok 29 rx 29
F034 after 60 s: data sent 22 ok 22 rx 19
```

Both directions, Mesh Control intact, payload intact, 90 to 100 % delivered on the desk.
The chip transmits and receives four-address data frames on a mesh interface without any
station having been registered with it. An occasional "could not build the peering open
frame" is a momentary packet-buffer shortage in the probe's own loop, not a radio fault.

## Scorecard (2026-10-18)

| Capability | Result |
|---|---|
| Mesh-type interface on the chip | accepted (stock firmware) |
| Mesh beacons scheduled by the chip | yes, after `MESH_CONFIG` with sane timing values |
| Mesh beacons on the air with a Mesh ID element | received by the other board, ~10/s |
| Neighbour discovery from beacons | works, even on a plain station interface |
| Mesh peering (action) frames, both ways | ~90 % delivered |
| 4-address mesh data frames with Mesh Control, both ways | 90 to 100 % delivered |

Everything 802.11s needs from the radio is there, on the shipping chip firmware, with host
code only. What's missing is the host-side protocol: this probe sends fixed frames; a mesh
needs the peering state machine (open/confirm/close with link ids), path selection (HWMP
path requests/replies, or a simpler flooding scheme), forwarding with TTL and sequence
dedupe, and a network interface on top so IP works.

## What a real build would be

1. **Mesh netif** in the HalowChat firmware: a mesh interface instead of the AP/STA pair,
   with the chip set up exactly as experiment 3 does (AP code with the interface type swapped,
   `MESH_CONFIG`, Mesh ID beacons).
2. **Peering**: wpa_supplicant's `mesh_mpm.c` is in the SDK tree but hooks into mac80211
   driver calls; a small hand-written state machine (open, confirm, close, holding timers)
   is less work than wiring that in, since security is off for now.
3. **Forwarding**: receive 4-address frames, dedupe on (SA, mesh sequence), decrement TTL,
   rebroadcast or forward to the next hop; deliver frames addressed to us to lwIP as Ethernet.
4. **Path selection**: start with controlled flooding (what HalowChat does today, one
   layer down), add HWMP later if routes matter.
5. **Transmit**: build 4-address frames from lwIP's Ethernet output, as experiment 4 does.

Morselib's UMAC will keep treating the interface as an access point; that is harmless as
long as nothing associates with it.
