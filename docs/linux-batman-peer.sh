#!/bin/bash
# Turns a Linux box into a BATMAN_V peer on its wired LAN port, for testing batman-probe.
# Debian/Ubuntu with the in-kernel batman-adv module (Debian 13 on the Umbrel has it).
# Run as root. Nothing persists across a reboot. Undo: bash linux-batman-peer.sh down
set -e
IFACE="${IFACE:-enp4s0}"        # the LAN port
BAT_IP="${BAT_IP:-10.41.250.1/16}"

if [ "$1" = "down" ]; then
    batctl meshif bat0 interface del "$IFACE" 2>/dev/null || true
    ip link del bat0 2>/dev/null || true
    rmmod batman_adv 2>/dev/null || true
    echo "batman-adv removed"
    exit 0
fi

command -v batctl >/dev/null || { apt-get update -qq && apt-get install -y -qq batctl; }
modprobe batman-adv
batctl ra BATMAN_V                      # must be set before bat0 is created
batctl meshif bat0 interface add "$IFACE"
ip link set up dev bat0
ip addr replace "$BAT_IP" dev bat0
echo "== routing algorithm =="; batctl ra
echo "== hard interfaces =="; batctl meshif bat0 interface
echo "== bat0 =="; ip -br addr show bat0
echo
echo "Watch the XIAO appear with:  batctl n ; batctl o ; batctl tg ; ping 10.41.250.2"
