#!/bin/sh
# Run on the MPC (copy to /data/hacks/ together with cvsniff.so). Restarts the MPC app, so save first.
#   sh sniff-try.sh        start the app with cvsniff.so preloaded (log: /tmp/cvsniff.log, FIFO: /tmp/cvinject)
#   sh sniff-try.sh stop   back to the stock launcher
# The launcher /usr/bin/az01-launch-MPC is on the read-only rootfs, so a copy with cvsniff.so prepended
# to LD_PRELOAD is bind-mounted over it. The mount is gone after a reboot. If the app doesn't come up
# with the shim, this reverts on its own.
HACKS=/data/hacks
LAUNCHER=/usr/bin/az01-launch-MPC

umount "$LAUNCHER" 2>/dev/null
if [ "$1" = stop ]; then
    systemctl restart acvs
    echo "stock launcher restored"
    exit 0
fi

sed "s#export LD_PRELOAD=\"#export LD_PRELOAD=\"$HACKS/cvsniff.so #" "$LAUNCHER" > "$HACKS/az01-launch-MPC.sniff"
chmod +x "$HACKS/az01-launch-MPC.sniff"
mount --bind "$HACKS/az01-launch-MPC.sniff" "$LAUNCHER"
systemctl restart acvs
sleep 20
if ! pidof MPC >/dev/null || ! grep -q cvsniff /proc/$(pidof MPC)/maps; then
    echo "sniff start FAILED, reverting"
    umount "$LAUNCHER"
    systemctl reset-failed acvs; systemctl restart acvs
else
    echo "sniff running pid=$(pidof MPC)"
fi
