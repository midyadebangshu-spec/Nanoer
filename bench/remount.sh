#!/bin/bash
# Unmounts a Nanoer mount, waits for nanoerfs to finish, drops the kernel's
# caches and mounts it again (for "cold" reads in workloads).
#   remount.sh BIN_DIR IMAGE MOUNT_POINT
set -euo pipefail
BIN=$1
IMG=$2
MNT=$3
fusermount3 -u "$MNT"
while pgrep -f "nanoerfs.* $IMG " >/dev/null; do sleep 0.1; done
sync
echo 3 >/proc/sys/vm/drop_caches
"$BIN/nanoerfs" -o allow_other "$IMG" "$MNT"
until mountpoint -q "$MNT"; do sleep 0.05; done
