#!/bin/bash
# The M2/M4 workloads (tests/integration/workloads.sh) on two builds of
# Nanoer, a fresh filesystem each time, ROUNDS rounds alternating which
# goes first; prints the median and range of each workload per build
# (docs/M6_PLAN.md §1, done-when 3: within 10 % of M5).
#
#   workloads_vs.sh A_NAME A_BIN_DIR B_NAME B_BIN_DIR SOURCE_REPO [ROUNDS]
#
# Run as root (FUSE, dropping caches). Needs ffmpeg, ninja, cmake.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
NAMES=("$1" "$3")
BINS=("$2" "$4")
REPO=$5
ROUNDS=${6:-3}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nanoer-vs.XXXXXX")
chmod 755 "$WORK"
trap 'fusermount3 -u "$WORK/mnt" 2>/dev/null || true; sleep 1; rm -rf "$WORK"' EXIT

run() {  # INDEX ROUND
  local name=${NAMES[$1]} bin=${BINS[$1]}
  local img=$WORK/$name.img mnt=$WORK/mnt
  rm -f "$img"
  mkdir -p "$mnt"
  "$bin/mkfs.nanoer" --size 16G "$img" >/dev/null
  "$bin/nanoerfs" -o allow_other "$img" "$mnt"
  until mountpoint -q "$mnt"; do sleep 0.05; done
  "$HERE/../tests/integration/workloads.sh" "$mnt" "$REPO" "$HERE/remount.sh '$bin' '$img' '$mnt'" |
    grep -v "^Target" | sed "s/^/$name\t$2\t/" >>"$WORK/results.tsv"
  fusermount3 -u "$mnt"
  while pgrep -f "nanoerfs.* $img " >/dev/null; do sleep 0.1; done
  rm -f "$img"
}

for round in $(seq "$ROUNDS"); do
  if [ $((round % 2)) = 1 ]; then order="0 1"; else order="1 0"; fi
  for i in $order; do
    echo "round $round: ${NAMES[$i]}" >&2
    run "$i" "$round"
  done
done

python3 -I - "$WORK/results.tsv" "${NAMES[0]}" "${NAMES[1]}" <<'PY'
import collections, re, statistics, sys
path, a, b = sys.argv[1:4]
times = collections.defaultdict(list)
order = []
for line in open(path):
    name, rnd, rest = line.rstrip("\n").split("\t", 2)
    m = re.match(r"(.*?)\s+([\d.]+) s$", rest)
    if not m:
        continue
    work = m.group(1).strip()
    if work not in order:
        order.append(work)
    times[(name, work)].append(float(m.group(2)))
print("| Workload | %s median (range) | %s median (range) | %s ÷ %s |" % (a, b, b, a))
print("|---|---:|---:|---:|")
for w in order:
    x, y = times[(a, w)], times[(b, w)]
    mx, my = statistics.median(x), statistics.median(y)
    ratio = my / mx if mx else float("nan")
    print("| %s | %.2f (%.2f–%.2f) | %.2f (%.2f–%.2f) | %.2f |" % (w, mx, min(x), max(x), my, min(y), max(y), ratio))
PY
