#!/bin/bash
# Filesystems made by M2-M5 builds (tests/images), on a real mount:
#
#   upgrade.sh BIN_DIR IMAGES_DIR
#
# For each image: it mounts read-only and holds what its own build wrote
# (the manifest made then); `nanoer upgrade` converts it; it checks clean,
# mounts read-write and holds the same; after garbage collection and
# recompressing everything offline (which move every record), it still
# does; and it takes new writes. Exits 77 (skipped) without FUSE.
set -euo pipefail

BIN=$1
IMAGES=$2

if [ ! -e /dev/fuse ] || ! command -v fusermount3 >/dev/null; then
  echo "FUSE is not available; skipping"
  exit 77
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nanoer-up.XXXXXX")
chmod 755 "$WORK"
MNT=$WORK/mnt
mkdir "$MNT"
PID=
cleanup() {
  if [ -n "$PID" ]; then
    fusermount3 -u "$MNT" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

mount_fs() {  # IMAGE [OPTIONS]
  local opts=allow_other
  [ "$(id -u)" = 0 ] || opts=default_permissions
  [ -z "${2:-}" ] || opts="$opts,$2"
  "$BIN/nanoerfs" -f -o "$opts" "$1" "$MNT" >>"$WORK/nanoerfs.log" 2>&1 &
  PID=$!
  for _ in $(seq 100); do
    mountpoint -q "$MNT" && return 0
    if ! kill -0 "$PID" 2>/dev/null; then
      cat "$WORK/nanoerfs.log"
      if grep -q "fuse: device not found\|Operation not permitted" "$WORK/nanoerfs.log"; then exit 77; fi
      return 1
    fi
    sleep 0.1
  done
  echo "mount timed out"
  return 1
}
unmount() {
  fusermount3 -u "$MNT"
  wait "$PID" || { echo "nanoerfs exited with an error"; cat "$WORK/nanoerfs.log"; return 1; }
  PID=
}
same() {  # NAME WHAT
  python3 -I "$IMAGES/manifest.py" "$MNT" >"$WORK/now.manifest"
  if ! diff -u "$IMAGES/$1.manifest" "$WORK/now.manifest"; then
    echo "$1: $2: not what its build wrote"
    exit 1
  fi
}

for xz in "$IMAGES"/*.img.xz; do
  name=$(basename "$xz" .img.xz)
  img=$WORK/$name.img
  python3 -I -c 'import lzma, sys; open(sys.argv[2], "wb").write(lzma.decompress(open(sys.argv[1], "rb").read()))' \
    "$xz" "$img"

  mount_fs "$img"
  [ -w "$MNT" ] && touch "$MNT/x" 2>/dev/null && { echo "$name: an old filesystem mounted read-write"; exit 1; }
  same "$name" "read-only, before the upgrade"
  unmount

  "$BIN/nanoer" upgrade "$img"
  "$BIN/nanoer" upgrade "$img" | grep -q "already up to date"
  "$BIN/nanoer" fsck --deep "$img" >/dev/null || { echo "$name: fsck after the upgrade"; exit 1; }

  mount_fs "$img"
  same "$name" "after the upgrade"
  unmount

  # Every record moved: garbage collection, then recompressing everything.
  "$BIN/nanoer" gc --max-full 97 "$img" >/dev/null
  "$BIN/nanoer" tier --age 0 "$img" >/dev/null
  "$BIN/nanoer" gc --max-full 97 "$img" >/dev/null
  "$BIN/nanoer" fsck --deep "$img" >/dev/null || { echo "$name: fsck after gc and tier"; exit 1; }
  mount_fs "$img"
  same "$name" "after gc and recompressing"
  echo "new data" >"$MNT/new.txt"
  rm "$MNT/d1/numbers.txt"
  unmount
  "$BIN/nanoer" fsck --deep "$img" >/dev/null || { echo "$name: fsck after writing"; exit 1; }
  mount_fs "$img"
  grep -q "new data" "$MNT/new.txt"
  unmount
  echo "$name: upgraded, every file intact through gc and recompression, writable"
done
