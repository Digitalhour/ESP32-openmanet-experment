# hopnode

The disposable relay. A XIAO ESP32S3 plus a Wio-WM6180 HaLow module running two things:

- `main/mesh.c`: the 802.11s mesh link. Beacons with a Mesh ID and Mesh Configuration,
  the peering handshake, 4-address data frames. In this firmware it runs in layer 2 mode:
  no network interface of its own, no relaying, no flooding; every frame goes up to
  batman-adv and batman-adv decides where things go.
- `main/batadv.c`: batman-adv, BATMAN_V, the routing OpenMANET runs. ELP hellos, OGM2
  announcements, route selection and forwarding, the translation table, unicast, broadcast,
  fragments. Written from the Linux kernel source and tested against it over Wi-Fi
  (`../batman-probe`).

No hotspot, no web page, no chat. `bat0` gets an address (`10.41.250.<last byte of the
bat address>` by default) so the node can be pinged and pings the nodes it knows every 15 s,
logging the result on the serial console. Mesh name, channel, TX power and the subnet are
build-time settings (`idf.py menuconfig`, "hopnode").

## Status (2026-10-19)

Two boards, 65 inches apart on the desk: both peer within a second of boot, see each other
as batman neighbours and originators, pull each other's translation table, and ping each
other's bat0 across the HaLow mesh (4 of 4, 3 of 4 in the first minutes). ELP and OGM
broadcasts arrive about 70 to 80 percent of the time on this bench, which batman-adv is
designed to tolerate.

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
