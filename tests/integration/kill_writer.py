#!/usr/bin/env python3
"""Writer and checker for the kill -9 test (run_mounted.sh kill).

  kill_writer.py write DIR LOG ROUND   write files under DIR until killed
  kill_writer.py check DIR LOG         check every durable file in LOG

The writer records an operation in LOG (on another filesystem) only after
fsync() on the Nanoer mount returned: from then on it must survive a crash.
A delete is recorded before it starts too, so a crash halfway through may go
either way. Live data is kept under LIVE_LIMIT (KILL_LIVE_LIMIT_MB in the environment)
by deleting older files.
"""
import hashlib
import os
import random
import sys

LIVE_LIMIT = int(os.environ.get("KILL_LIVE_LIMIT_MB", "128")) << 20


def log_line(log, line):
    log.write(line + "\n")
    log.flush()
    os.fsync(log.fileno())


def read_log(log_path):
    """Returns ({name: (digest, size)} durable, names maybe deleted)."""
    files, maybe_gone = {}, set()
    with open(log_path) as log:
        for line in log:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "file":
                files[parts[1]] = (parts[2], int(parts[3]))
            elif parts[0] == "deleting":
                maybe_gone.add(parts[1])
            elif parts[0] == "deleted":
                files.pop(parts[1], None)
                maybe_gone.discard(parts[1])
    return files, maybe_gone


def delete(top, log, name):
    log_line(log, f"deleting {name}")
    try:
        os.unlink(os.path.join(top, name))
    except FileNotFoundError:
        pass  # the crash came after the delete was durable
    fd = os.open(os.path.dirname(os.path.join(top, name)), os.O_RDONLY | os.O_DIRECTORY)
    os.fsync(fd)
    os.close(fd)
    log_line(log, f"deleted {name}")


def write(top, log_path, round_no):
    rng = random.Random(round_no)
    d = os.path.join(top, f"r{round_no}")
    os.makedirs(d, exist_ok=True)
    files, maybe_gone = read_log(log_path)
    live = sorted(files)
    live_bytes = sum(size for _, size in files.values())
    with open(log_path, "a") as log:
        for name in sorted(maybe_gone):  # deletes a crash interrupted
            delete(top, log, name)
        i = 0
        while True:
            i += 1
            if live and (live_bytes > LIVE_LIMIT or rng.random() < 0.2):
                victim = live.pop(0 if live_bytes > LIVE_LIMIT else rng.randrange(len(live)))
                delete(top, log, victim)
                live_bytes -= files.pop(victim)[1]
                continue
            name = f"r{round_no}/f{i}"
            size = rng.choice([0, 1, 4095, 4096, 70000, 300000])
            # Half compressible, half not, so both data paths are used.
            if rng.random() < 0.5:
                data = os.urandom(size)
            else:
                data = (f"{name} line {i}\n" * (size // 16 + 1)).encode()[:size]
            with open(os.path.join(top, name), "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            digest = hashlib.sha256(data).hexdigest()
            log_line(log, f"file {name} {digest} {size}")
            live.append(name)
            files[name] = (digest, size)
            live_bytes += size


def check(top, log_path):
    files, maybe_gone = read_log(log_path)
    bad = 0
    for name, (digest, _) in sorted(files.items()):
        path = os.path.join(top, name)
        if not os.path.exists(path):
            if name in maybe_gone:
                continue
            print(f"missing: {name}")
            bad += 1
            continue
        with open(path, "rb") as f:
            got = hashlib.sha256(f.read()).hexdigest()
        if got != digest:
            print(f"wrong contents: {name}")
            bad += 1
    print(f"{len(files)} durable files checked, {bad} bad")
    return 1 if bad else 0


if __name__ == "__main__":
    if sys.argv[1] == "write":
        try:
            write(sys.argv[2], sys.argv[3], int(sys.argv[4]))
        except OSError:
            pass  # the filesystem went away under us: expected
    else:
        sys.exit(check(sys.argv[2], sys.argv[3]))
