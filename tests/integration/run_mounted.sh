#!/bin/bash
# Mounts a fresh Nanoer filesystem and runs a test against it.
#
#   run_mounted.sh BIN_DIR MODE [ARGS...]
#
# BIN_DIR holds mkfs.nanoer, nanoerfs and nanoer-fsx. MODE is one of
#   posix TEST_BINARY   run the POSIX suite on the mount
#   fsx                 run nanoer-fsx, then check the file survives a remount
#   persist             copy a directory tree in, remount, compare
#   command CMD...      run any command with NANOER_TEST_DIR set
# Exits 77 (skipped) when FUSE is not usable here.
set -euo pipefail

BIN=$1
MODE=$2
shift 2

if [ ! -e /dev/fuse ] || ! command -v fusermount3 >/dev/null; then
  echo "FUSE is not available; skipping"
  exit 77
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nanoer-it.XXXXXX")
chmod 755 "$WORK"  # mktemp makes it private; other users must reach the mount
IMG=$WORK/fs.img
MNT=$WORK/mnt
mkdir "$MNT"
PID=

unmount() {
  if [ -n "$PID" ]; then
    fusermount3 -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    # Wait for the final commit and checkpoint, so the image is complete.
    wait "$PID" || { echo "nanoerfs exited with an error"; cat "$WORK/nanoerfs.log"; return 1; }
    PID=
  fi
}

cleanup() {
  unmount || true
  rm -rf "$WORK"
}
trap cleanup EXIT

mount_fs() {
  local opts=allow_other
  [ "$(id -u)" = 0 ] || opts=default_permissions  # allow_other needs root or user_allow_other
  "$BIN/nanoerfs" -f -o "$opts" "$IMG" "$MNT" >>"$WORK/nanoerfs.log" 2>&1 &
  PID=$!
  for _ in $(seq 100); do
    mountpoint -q "$MNT" && return 0
    if ! kill -0 "$PID" 2>/dev/null; then
      cat "$WORK/nanoerfs.log"
      if grep -q "fuse: device not found\|Operation not permitted\|fusermount" "$WORK/nanoerfs.log"; then
        echo "cannot mount FUSE here; skipping"
        exit 77
      fi
      return 1
    fi
    sleep 0.1
  done
  echo "mount timed out"
  return 1
}

"$BIN/mkfs.nanoer" --size 8G "$IMG" >/dev/null
mount_fs

case "$MODE" in
  posix)
    NANOER_TEST_DIR=$MNT "$1"
    ;;
  fsx)
    "$BIN/nanoer-fsx" -N "${FSX_OPS:-20000}" -S "${FSX_SEED:-1}" -k "$MNT/fsx.dat"
    before=$(sha256sum <"$MNT/fsx.dat")
    unmount
    mount_fs
    after=$(sha256sum <"$MNT/fsx.dat")
    [ "$before" = "$after" ] || { echo "fsx file changed across a remount"; exit 1; }
    echo "fsx file identical after remount"
    ;;
  persist)
    SRC=${1:-/usr/include}
    cp -a "$SRC" "$MNT/tree"
    unmount
    mount_fs
    diff -r --no-dereference "$SRC" "$MNT/tree"
    echo "tree identical after remount"
    ;;
  command)
    NANOER_TEST_DIR=$MNT "$@"
    ;;
  *)
    echo "unknown mode $MODE"
    exit 2
    ;;
esac
unmount
