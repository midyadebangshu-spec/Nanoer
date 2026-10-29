"""Prints a manifest of a directory tree: one line per entry, sorted, with
what a filesystem must keep: inode number, type, permissions, owner, link
count, size, modification time, the target of a symlink or a hash of a
file's contents, and extended attributes.

    python3 -I manifest.py DIR

Used to make the upgrade test images (make_images.sh) and to check them.
"""
import os
import stat
import sys


def fnv1a64(path):
    h = 0xCBF29CE484222325
    with open(path, "rb") as f:
        while True:
            block = f.read(1 << 16)
            if not block:
                return h
            for b in block:
                h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF


def describe(root, rel):
    path = os.path.join(root, rel) if rel else root
    st = os.lstat(path)
    mode = st.st_mode
    if stat.S_ISDIR(mode):
        kind, what = "d", ""
    elif stat.S_ISLNK(mode):
        kind, what = "l", os.readlink(path)
    elif stat.S_ISREG(mode):
        kind, what = "f", "%016x" % fnv1a64(path)
    elif stat.S_ISFIFO(mode):
        kind, what = "p", ""
    else:
        kind, what = "?", ""
    xattrs = []
    if not stat.S_ISLNK(mode):
        for name in sorted(os.listxattr(path)):
            if name.startswith("user.nanoer.stats"):
                continue  # served on the root, not stored
            xattrs.append("%s=%s" % (name, os.getxattr(path, name).hex()))
    fields = [
        "/" + rel,
        "ino=%d" % st.st_ino,
        kind,
        "%04o" % stat.S_IMODE(mode),
        "%d:%d" % (st.st_uid, st.st_gid),
        "nlink=%d" % st.st_nlink,
        "size=%d" % (st.st_size if not stat.S_ISDIR(mode) else 0),
        "mtime=%d" % st.st_mtime_ns,
        what,
        ",".join(xattrs),
    ]
    return "\t".join(fields)


def main():
    root = sys.argv[1]
    lines = [describe(root, "")]
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(dirnames + filenames):
            rel = os.path.relpath(os.path.join(dirpath, name), root)
            lines.append(describe(root, rel))
    for line in sorted(lines):
        print(line)


if __name__ == "__main__":
    main()
