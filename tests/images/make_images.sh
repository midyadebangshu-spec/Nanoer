#!/bin/bash
# Makes an upgrade test image with an older build of Nanoer (M2 to M5):
#
#   make_images.sh BIN_DIR NAME LEVEL OUT_DIR
#
# BIN_DIR holds that build's mkfs.nanoer, nanoerfs and nanoer. LEVEL says
# what it can store: 2 (M2, M3: files only), 4 (M4: also dedup and packs of
# small files), 5 (M5: also folder policies and cold data). Writes
# OUT_DIR/NAME.img.xz and OUT_DIR/NAME.manifest (manifest.py's listing of
# what the image holds, made over that build's own mount).
#
# The image ends as a crash left it: the last change was fsynced, then
# nanoerfs was killed, so an upgrade must replay a log after the checkpoint.
# Run as root (FUSE). The images in this directory were made from the
# commits named in KNOWLEDGE.md (M6 step 4).
set -euo pipefail

BIN=$1
NAME=$2
LEVEL=$3
OUT=$4
HERE=$(cd "$(dirname "$0")" && pwd)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nanoer-img.XXXXXX")
IMG=$WORK/$NAME.img
MNT=$WORK/mnt
mkdir "$MNT"
PID=
cleanup() {
  [ -z "$PID" ] || kill -9 "$PID" 2>/dev/null || true
  fusermount3 -u -z "$MNT" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

mount_fs() {
  "$BIN/nanoerfs" -f -o allow_other "$IMG" "$MNT" >>"$WORK/nanoerfs.log" 2>&1 &
  PID=$!
  for _ in $(seq 100); do
    mountpoint -q "$MNT" && return 0
    sleep 0.1
  done
  cat "$WORK/nanoerfs.log"
  return 1
}
unmount() {
  fusermount3 -u "$MNT"
  wait "$PID"
  PID=
}

"$BIN/mkfs.nanoer" --size 64M --segment-size 1M "$IMG" >/dev/null
mount_fs

# Everything any version can store.
python3 -I - "$MNT" "$LEVEL" <<'EOF'
import os, random, sys
root, level = sys.argv[1], int(sys.argv[2])
rng = random.Random(6)
def write(path, data, mode=None):
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    if mode is not None:
        os.chmod(path, mode)
def text(n):
    words = [b"nanoer", b"segment", b"chunk", b"zstd", b"inode", b"log", b"tree", b"\n"]
    out = bytearray()
    while len(out) < n:
        out += rng.choice(words) + b" "
    return bytes(out[:n])
def noise(n):
    return bytes(rng.getrandbits(8) for _ in range(n))
os.chdir(root)
os.makedirs("d1/d2/d3")
write("d1/hello.txt", b"hello, upgrade\n")
write("d1/numbers.txt", b"".join(b"%d\n" % i for i in range(6000)))
write("big.bin", text(400 << 10) + noise(40 << 10))       # several chunks
write("zeros.bin", bytes(300 << 10))                        # all zeros: holes
write("empty", b"")
with open("sparse.bin", "wb") as f:                         # a hole, then data
    f.truncate(5 << 20)
    f.seek(3 << 20)
    f.write(text(4096))
    os.fsync(f.fileno())
write("d1/d2/d3/deep.txt", text(9000), 0o600)
os.chmod("d1/d2", 0o750)
os.link("d1/hello.txt", "hardlink.txt")
os.symlink("d1/hello.txt", "link")
os.mkfifo("fifo")
os.setxattr("d1/hello.txt", "user.color", b"blue")
os.setxattr("d1", "user.big", noise(3000))
write("renamed-from", text(2000))
os.rename("renamed-from", "d1/renamed-to")
write("gone", text(5000))
os.unlink("gone")
if level >= 4:
    os.makedirs("small")
    for i in range(80):                                     # packed together
        write("small/f%02d.txt" % i, text(500 + 37 * i))
    write("copy-of-big.bin", open("big.bin", "rb").read())  # deduplicated
    # A record shared by dedup whose first owner then goes away: the copy
    # is its only user, and the record's header names the original.
    write("orig.bin", noise(140 << 10))   # two chunks
    write("dup.bin", open("orig.bin", "rb").read())
    os.unlink("orig.bin")
if level >= 5:
    os.makedirs("archive")
    os.setxattr("archive", "user.nanoer.policy", b"max")
    write("archive/log.txt", text(300 << 10))
EOF
unmount

if [ "$LEVEL" -ge 5 ]; then
  "$BIN/nanoer" tier --age 0 "$IMG" >/dev/null  # cold data
fi

# A last change, fsynced, then a crash: the log goes on after the checkpoint.
mount_fs
python3 -I - "$MNT" <<'EOF'
import os, sys
with open(os.path.join(sys.argv[1], "after-checkpoint.txt"), "wb") as f:
    f.write(b"written after the last checkpoint\n" * 100)
    f.flush()
    os.fsync(f.fileno())
EOF
python3 -I "$HERE/manifest.py" "$MNT" >"$OUT/$NAME.manifest"
kill -9 "$PID"
wait "$PID" 2>/dev/null || true
PID=
fusermount3 -u -z "$MNT" 2>/dev/null || true

python3 -I -c 'import lzma, sys; open(sys.argv[2], "wb").write(lzma.compress(open(sys.argv[1], "rb").read(), preset=9))' \
  "$IMG" "$OUT/$NAME.img.xz"
echo "$NAME: $(wc -l <"$OUT/$NAME.manifest") entries, $(stat -c %s "$OUT/$NAME.img.xz") bytes compressed"
