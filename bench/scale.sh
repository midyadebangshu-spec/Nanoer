#!/bin/bash
# nanoerfs's memory on a large filesystem, on a real mount (docs/M6_PLAN.md
# §1, done-when 1: within its budgets plus 64 MiB, whatever the file count).
#
#   scale.sh BIN_DIR IMAGE
#
# IMAGE: made by `nanoer-scale make` (directories d0, d1, ... of files
# f0, f1, ...). Mounts it with the default budgets (safe space 256 MiB,
# metadata cache 64 MiB, dedup index at most 128 MiB), then reports
# nanoerfs's resident memory (VmRSS, and its peak VmHWM) after: mounting;
# the background dedup rebuild; `find` with every file's attributes; reading
# 1 in 100 files whole; writing 1,000 new files; and how long mounting took.
# Run as root (FUSE, dropping caches).
set -euo pipefail

BIN=$1
IMG=$2
MNT=$(mktemp -d "${TMPDIR:-/tmp}/nanoer-scale.XXXXXX")
PID=
cleanup() {
  [ -z "$PID" ] || { fusermount3 -u "$MNT" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; }
  rmdir "$MNT"
}
trap cleanup EXIT

mem() {  # LABEL
  local rss hwm
  rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$PID/status")
  hwm=$(awk '/^VmHWM:/ {print $2}' "/proc/$PID/status")
  printf "%-44s RSS %7.1f MiB   peak %7.1f MiB\n" "$1" "$(echo "$rss / 1024" | bc -l)" "$(echo "$hwm / 1024" | bc -l)"
}
seconds() { date +%s.%N; }

sync
echo 3 >/proc/sys/vm/drop_caches
start=$(seconds)
"$BIN/nanoerfs" -f -o allow_other "$IMG" "$MNT" 2>/dev/null &
PID=$!
until mountpoint -q "$MNT"; do sleep 0.01; done
printf "%-44s %.2f s\n" "mount (until the mount point answers)" "$(echo "$(seconds) - $start" | bc)"
mem "after mount"

# The dedup index is rebuilt in the background, a slice every second (a
# few seconds in all for 4M chunks, measured in-process by nanoer-scale).
sleep 30
mem "30 s later (the dedup index rebuilt)"

start=$(seconds)
n=$(find "$MNT" -type f -printf '%s\n' | wc -l)
printf "%-44s %.2f s (%d files)\n" "find with every file's attributes" "$(echo "$(seconds) - $start" | bc)" "$n"
mem "after find"

start=$(seconds)
find "$MNT" -type f | awk 'NR % 100 == 0' | xargs -d '\n' cat >/dev/null
printf "%-44s %.2f s\n" "read 1 in 100 files whole" "$(echo "$(seconds) - $start" | bc)"
mem "after reading"

mkdir "$MNT/new"
start=$(seconds)
for i in $(seq 1000); do head -c 300000 /dev/urandom >"$MNT/new/f$i"; done
sync
printf "%-44s %.2f s\n" "write 1,000 files of 300 KB" "$(echo "$(seconds) - $start" | bc)"
mem "after writing"
rm -rf "$MNT/new"
fusermount3 -u "$MNT"
wait "$PID"
PID=
