#!/bin/sh
# Put the HaLowLink 1's mesh (set up by halowlink1-mesh.sh) behind a mesh password: SAE, CCMP,
# the same thing OpenMANET nodes run. Give the password as the script's argument so it never
# sits in a file:
#
#   ssh root@<halowlink> 'sh -s -- "the mesh password"' < docs/halowlink1-mesh-sae.sh
#
# Back to an open mesh: run halowlink1-mesh.sh again.

PW="$1"
if [ -z "$PW" ]; then
    echo "usage: sh -s -- <mesh password>" >&2
    exit 1
fi

set -e
uci set wireless.default_radio1.encryption="sae"
uci set wireless.default_radio1.key="$PW"
uci commit wireless
/etc/init.d/network reload >/dev/null 2>&1 || true
sleep 12

echo "== iw dev wlan0 info"; iw dev wlan0 info || true
echo "== supplicant mesh settings"
grep -E "key_mgmt|mode=|ieee80211w|pmf|sae_pwe|mesh_fwding" /var/run/wpa_supplicant-wlan0.conf 2>/dev/null || true
echo "== recent log"
logread | grep -i -E "wlan0|mesh|sae|wpa_supp" | grep -v -i "password\|key=" | tail -15
