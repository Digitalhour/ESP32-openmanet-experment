#!/bin/sh
# Put a Morse Micro HaLowLink 1 (OpenWrt 23.05, Morse 2.7.x) into open 802.11s mesh
# point mode so it can peer with the XIAO hop nodes in this repository.
#
# Run it on the HaLowLink as root:
#   ssh root@<halowlink> 'sh -s' < docs/halowlink1-mesh.sh
#
# What it changes (backups go to /etc/config/*.pre-mesh):
#   radio1 (HaLow)      channel 26 (2 MHz, 915 MHz centre, US)
#   default_radio1      mode mesh, mesh_id OpenMANET-XIAO, no encryption
#   network "mesh"      static 10.41.250.3/16 on the mesh interface
#
# Undo:
#   cp /etc/config/wireless.pre-mesh /etc/config/wireless
#   cp /etc/config/network.pre-mesh /etc/config/network
#   /etc/init.d/network reload

MESH_ID="${MESH_ID:-OpenMANET-XIAO}"
CHANNEL="${CHANNEL:-26}"
MESH_IP="${MESH_IP:-10.41.250.3}"

set -e
[ -f /etc/config/wireless.pre-mesh ] || cp /etc/config/wireless /etc/config/wireless.pre-mesh
[ -f /etc/config/network.pre-mesh ] || cp /etc/config/network /etc/config/network.pre-mesh

uci set wireless.radio1.channel="$CHANNEL"
uci -q delete wireless.radio1.op_class || true
uci set wireless.default_radio1.mode="mesh"
uci set wireless.default_radio1.mesh_id="$MESH_ID"
uci set wireless.default_radio1.encryption="none"
uci -q delete wireless.default_radio1.key || true
uci -q delete wireless.default_radio1.ssid || true
uci -q delete wireless.default_radio1.wds || true
uci set wireless.default_radio1.network="mesh"
uci set network.mesh=interface
uci set network.mesh.proto="static"
uci set network.mesh.ipaddr="$MESH_IP"
uci set network.mesh.netmask="255.255.0.0"
uci commit

/etc/init.d/network reload >/dev/null 2>&1 || true
sleep 12

echo "== iw dev wlan0 info"; iw dev wlan0 info || true
echo "== address"; ip -4 addr show wlan0 || true
echo "== mesh_fwding"; iw dev wlan0 get mesh_param mesh_fwding || true
echo "== recent log"
logread | grep -i -E "wlan0|mesh|wpa_supp|morse_cli" | tail -20
