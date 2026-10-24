#!/bin/bash
# Real workloads, timed, on a target directory: run once on a plain
# directory (e.g. on ext4) and once on a Nanoer mount, and compare.
#
#   workloads.sh TARGET_DIR SOURCE_REPO [REMOUNT_CMD]
#
# SOURCE_REPO is a git repository to clone and build (Nanoer itself).
# REMOUNT_CMD, if given, is run before the "cold" read workloads so they
# read from the filesystem rather than the kernel's page cache. STATS_CMD,
# if set, runs before the final delete (to record space used).
set -euo pipefail

T=$1
REPO=$2
REMOUNT=${3:-}
DATA=$(mktemp -d "${TMPDIR:-/tmp}/nanoer-wl.XXXXXX")
trap 'rm -rf "$DATA"' EXIT

# Inputs prepared outside the target, so their cost is not measured.
tar -C /usr -cf "$DATA/tree.tar" include lib/python3.12
ffmpeg -v error -f lavfi -i "testsrc2=duration=60:size=1280x720:rate=30" \
  -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p "$DATA/video.mp4"
head -c $((512 << 20)) /dev/urandom >"$DATA/random.bin"

drop_caches() {
  sync
  echo 3 >/proc/sys/vm/drop_caches 2>/dev/null || true
}

timed() {
  local name=$1
  shift
  local start end
  start=$(date +%s.%N)
  "$@" >/dev/null 2>&1
  end=$(date +%s.%N)
  printf "%-34s %8.2f s\n" "$name" "$(echo "$end - $start" | bc)"
}

echo "Target: $T"
timed "untar 104 MB (headers, Python)" sh -c "mkdir -p '$T/tree' && tar -C '$T/tree' -xf '$DATA/tree.tar' && sync"
timed "copy 512 MB incompressible" sh -c "cp '$DATA/random.bin' '$T/' && sync"
timed "copy 60 s 720p H.264 video" sh -c "cp '$DATA/video.mp4' '$T/' && sync"
timed "git clone (local)" git clone -q "$REPO" "$T/repo"
timed "cmake configure" cmake -S "$T/repo" -B "$T/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNANOER_BUILD_TESTS=OFF
timed "compile Nanoer (4 jobs)" ninja -C "$T/build" -j4
timed "git status" git -C "$T/repo" status
timed "find all files" find "$T" -type f
timed "grep -r (warm)" grep -r -c "include" "$T/tree"

if [ -n "$REMOUNT" ]; then eval "$REMOUNT"; else drop_caches; fi
timed "read 512 MB (cold)" cat "$T/random.bin"
timed "decode the video (cold)" ffmpeg -v error -i "$T/video.mp4" -f null -
timed "grep -r (cold)" grep -r -c "include" "$T/tree"
# (Through cat: GNU tar does not read file contents when writing to /dev/null.)
timed "tar the tree (cold)" sh -c "tar -C '$T' -cf - tree | cat >/dev/null"
if [ -n "${STATS_CMD:-}" ]; then eval "$STATS_CMD"; fi
timed "rm -rf everything" sh -c "rm -rf '$T/tree' '$T/repo' '$T/build' '$T/random.bin' '$T/video.mp4' && sync"
