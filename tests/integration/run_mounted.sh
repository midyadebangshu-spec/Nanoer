#!/bin/bash
# Mounts a fresh Nanoer filesystem and runs a test against it.
#
#   run_mounted.sh BIN_DIR MODE [ARGS...]
#
# BIN_DIR holds mkfs.nanoer, nanoerfs, nanoer and nanoer-fsx. MODE is one of
#   posix TEST_BINARY   run the POSIX suite on the mount
#   fsx                 run nanoer-fsx, then check the file survives a remount
#   persist             copy a directory tree in, remount, compare
#   kill                kill -9 nanoerfs while a writer fsyncs (and fsx, an
#                       untar and a build run), KILL_ROUNDS times; after
#                       each, fsck must find it clean and every fsynced file
#                       must be intact
#   command CMD...      run any command with NANOER_TEST_DIR set
# NANOER_MKFS_OPTS replaces the default "--size 8G" (say, a small store with
# small segments, so garbage collection runs all the time). NANOER_MOUNT_OPTS
# adds mount options (say, cold_after=0,idle=0: recompress all the time).
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
  [ -z "${NANOER_MOUNT_OPTS:-}" ] || opts="$opts,$NANOER_MOUNT_OPTS"
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

# shellcheck disable=SC2086  # word splitting of the options is intended
"$BIN/mkfs.nanoer" ${NANOER_MKFS_OPTS:---size 8G} "$IMG" >/dev/null
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
    cp -a "$SRC" "$MNT/copy"  # stored again: dedup
    unmount
    mount_fs
    diff -r --no-dereference "$SRC" "$MNT/tree"
    diff -r --no-dereference "$SRC" "$MNT/copy"
    echo "tree identical after remount"
    # The statistics of a mounted filesystem, through user.nanoer.stats.
    "$BIN/nanoer" stats "$MNT" | tee "$WORK/stats.txt"
    grep -q "records shared" "$WORK/stats.txt"
    ;;
  kill)
    WRITER=$(dirname "$0")/kill_writer.py
    LOG=$WORK/durable.log
    : >"$LOG"
    build() {  # many small compiles and an archive, as a build does
      mkdir -p "$1"
      for i in $(seq 1000); do
        printf 'int f%d(int x) { return x * %d; }\n' "$i" "$i" >"$1/f$i.c"
        cc -c -o "$1/f$i.o" "$1/f$i.c" && ar rcs "$1/lib.a" "$1/f$i.o"
      done
    }
    for round in $(seq "${KILL_ROUNDS:-5}"); do
      python3 "$WRITER" write "$MNT" "$LOG" "$round" &
      writer=$!
      # Load nobody checks, except fsck: more kinds of operation in flight.
      # KILL_LOAD picks which (default: all).
      load=()
      if [[ " ${KILL_LOAD:-fsx tar build} " == *" fsx "* ]]; then
        # (In a subshell: its mmap dies with SIGBUS when the mount goes away,
        # and the shell would report that.)
        ("$BIN/nanoer-fsx" -N 100000000 -S "$round" "$MNT/load-$round.fsx"; true) >/dev/null 2>&1 &
        load+=($!)
      fi
      if [[ " ${KILL_LOAD:-fsx tar build} " == *" tar "* ]]; then
        (mkdir -p "$MNT/load-$round" && tar -C /usr -cf - include | tar -C "$MNT/load-$round" -xf -) \
          >/dev/null 2>&1 &
        load+=($!)
      fi
      if [[ " ${KILL_LOAD:-fsx tar build} " == *" build "* ]]; then
        build "$MNT/load-$round/build" >/dev/null 2>&1 &
        load+=($!)
      fi
      # A random moment: between commits, mid-write, mid-checkpoint.
      sleep "$(awk -v r="$RANDOM" 'BEGIN { printf "%.2f", 1 + r % 300 / 100 }')"
      kill -9 "$PID"
      wait "$PID" 2>/dev/null || true
      PID=
      fusermount3 -u -z "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
      wait "$writer" || true
      if [ ${#load[@]} -gt 0 ]; then
        kill "${load[@]}" 2>/dev/null || true
        wait "${load[@]}" 2>/dev/null || true
      fi
      if ! "$BIN/nanoer" fsck --deep "$IMG"; then
        echo "round $round: fsck found errors after kill -9"
        exit 1
      fi
      mount_fs
      python3 "$WRITER" check "$MNT" "$LOG"
      # The filesystem must be writable again, not read-only.
      echo "round $round" >"$MNT/after-$round"
      rm -rf "$MNT"/load-*
      echo "round $round: recovered, $(grep -c '^file' "$LOG") files written so far"
    done
    if [[ "${NANOER_MOUNT_OPTS:-}" == *cold_after=0* ]]; then
      # Recompressing ran too: some data is in its cold form.
      sleep 3
      "$BIN/nanoer" stats "$MNT" | grep "cold:" | tee "$WORK/cold.txt"
      if grep -q "cold: *0 B" "$WORK/cold.txt"; then
        echo "no data was recompressed"
        exit 1
      fi
    fi
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
