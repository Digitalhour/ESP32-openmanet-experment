#!/bin/sh
# Run batman-adv (BATMAN_V) over the HaLowLink 1's 802.11s mesh interface so it routes with
# the XIAO hop nodes. Run docs/halowlink1-mesh.sh first. kmod-batman-adv and batctl ship in
# the stock Morse 2.7.x image.
#
#   ssh root@<halowlink> 'sh -s' < docs/halowlink1-batman.sh
#
# What it changes (backup in /etc/config/network.pre-batman):
#   interface bat0    proto batadv, BATMAN_V, gateway mode off
#   interface mesh    becomes a batadv_hardif slaved to bat0 (the HaLow radio)
#   interface batip   static 10.41.250.3/16 on bat0
#
# Undo:
#   cp /etc/config/network.pre-batman /etc/config/network && /etc/init.d/network reload

BAT_IP="${BAT_IP:-10.41.250.3}"

set -e
[ -f /etc/config/network.pre-batman ] || cp /etc/config/network /etc/config/network.pre-batman

uci set network.bat0=interface
uci set network.bat0.proto="batadv"
uci set network.bat0.routing_algo="BATMAN_V"
uci set network.bat0.gw_mode="off"
uci set network.bat0.orig_interval="1000"

uci set network.mesh=interface
uci set network.mesh.proto="batadv_hardif"
uci set network.mesh.master="bat0"
uci -q delete network.mesh.ipaddr || true
uci -q delete network.mesh.netmask || true

uci set network.batip=interface
uci set network.batip.proto="static"
uci set network.batip.device="bat0"
uci set network.batip.ipaddr="$BAT_IP"
uci set network.batip.netmask="255.255.0.0"
uci commit network

/etc/init.d/network reload >/dev/null 2>&1 || true
sleep 15

echo "== bat0"; ip -4 addr show bat0 || true
echo "== routing algorithm"; batctl meshif bat0 ra || true
echo "== hard interfaces"; batctl meshif bat0 if || true
echo "== neighbours"; batctl meshif bat0 n || true
echo "== originators"; batctl meshif bat0 o || true
echo "== translation table"; batctl meshif bat0 tg || true
for ip in 10.41.250.19 10.41.250.251; do
    echo "== ping $ip"; ping -c 3 -W 2 "$ip" || true
done
