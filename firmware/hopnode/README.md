# hopnode

The disposable relay. A XIAO ESP32S3 plus a Wio-WM6180 HaLow module running two things:

- `main/mesh.c`: the 802.11s mesh link. Beacons with a Mesh ID and Mesh Configuration,
  the peering handshake, mesh data frames, HWMP path replies. In this firmware it runs in layer 2 mode:
  no network interface of its own, no relaying, no flooding; every frame goes up to
  batman-adv and batman-adv decides where things go.
- `main/batadv.c`: batman-adv, BATMAN_V, the routing OpenMANET runs. ELP hellos, OGM2
  announcements, route selection and forwarding, the translation table, unicast, broadcast,
  fragments, echo replies for `batctl ping` and `traceroute`. Written from the Linux kernel
  source and tested against it over Wi-Fi (`../batman-probe`) and over HaLow (below).

No hotspot, no web page, no chat. `bat0` gets an address (`10.41.250.<last byte of the
bat address>` by default) so the node can be pinged and pings the nodes it knows every 15 s,
logging the result on the serial console. Mesh name, channel, TX power and the subnet are
build-time settings (`idf.py menuconfig`, "hopnode").

## Status (2026-10-21)

Two boards, 65 inches apart on the desk: both peer within a second of boot, see each other
as batman neighbours and originators, pull each other's translation table, and ping each
other's bat0 across the HaLow mesh (4 of 4, 3 of 4 in the first minutes). ELP and OGM
broadcasts arrive about 70 to 80 percent of the time on this bench, which batman-adv is
designed to tolerate.

With a Linux mesh point on the bench (a Morse Micro HaLowLink 1 on its stock OpenWrt 23.05
Morse 2.7.6 firmware, put into open mesh point mode with `docs/halowlink1-mesh.sh` and
running batman-adv over that interface with `docs/halowlink1-batman.sh`): both XIAOs show as
`ESTAB` peers in `iw dev wlan0 station dump`, `batctl n`, `batctl o` and `batctl tg` list
them with their bat0 clients, and from the Linux side IP pings pass 10 of 10 to each XIAO,
1400-byte pings 5 of 5, `batctl ping` 5 of 5, `batctl traceroute` one hop. Round trips 15 to
200 ms, the same per-boot variance seen between XIAOs.

The XIAO pings every translation-table client at `10.41.250.<last byte of its MAC>`, which
is only right for other XIAOs; the HaLowLink answers at 10.41.250.3, so that one ping in the
log fails by design until nodes get real addressing.

Not yet: the mesh password (SAE), which a real OpenMANET net requires; a third board to show
a packet crossing a hop; power and enclosure work for an outdoor, forgotten node;
provisioning without a build (mesh name and password from a page or the USB console).

## Build and flash

```bash
. ~/esp/esp-idf/export.sh
cd firmware/hopnode
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```
