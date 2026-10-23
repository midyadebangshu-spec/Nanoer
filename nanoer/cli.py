"""Command-line interface: ``nanoer pack | info | play | serve | cat | unpack``."""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import webbrowser

from . import __version__
from .codecs import available_codecs
from .format import FormatError
from .packer import DEFAULT_CHUNK_SIZE, pack
from .reader import Archive
from .server import ArchiveServer

_UNITS = {"": 1, "k": 2**10, "m": 2**20, "g": 2**30, "t": 2**40}
PLAYERS = ("mpv", "vlc", "ffplay")


def parse_size(text):
    """Parse sizes such as ``50M``, ``50MB``, ``1.5g`` or ``4096`` into bytes."""
    match = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*([kmgt]?)(?:i?b)?\s*", text, re.I)
    if not match:
        raise argparse.ArgumentTypeError(f"invalid size: {text!r}")
    number, unit = match.groups()
    value = int(float(number) * _UNITS[unit.lower()])
    if value <= 0:
        raise argparse.ArgumentTypeError("size must be positive")
    return value


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if abs(n) < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024


def _event_logger(enabled):
    if not enabled:
        return None

    def log(kind, i):
        print(f"[nanoer] {kind:5} chunk {i}", file=sys.stderr, flush=True)

    return log


def cmd_pack(args):
    out = args.output or args.input + ".nnr"
    size = os.path.getsize(args.input)
    started = time.monotonic()

    def progress(read, written):
        if sys.stderr.isatty():
            pct = 100 * read / size if size else 100
            print(f"\r{pct:5.1f}%  {human(read)} -> {human(written)}",
                  end="", file=sys.stderr, flush=True)

    total, archive_size, count = pack(
        args.input, out, chunk_size=args.chunk_size, codec=args.codec,
        level=args.level, jobs=args.jobs, progress=progress,
    )
    if sys.stderr.isatty():
        print(file=sys.stderr)
    ratio = archive_size / total if total else 1
    print(f"{args.input}: {human(total)} -> {human(archive_size)} "
          f"({ratio:.1%}) in {count} chunks of {human(args.chunk_size)}, "
          f"{time.monotonic() - started:.1f}s -> {out}")


def cmd_info(args):
    with Archive(args.archive) as archive:
        stored = sum(1 for c in archive.chunks if c.flags & 1)
        print(f"name:        {archive.name}")
        print(f"codec:       {archive.codec.name}")
        print(f"original:    {human(archive.size)} ({archive.size} bytes)")
        print(f"archive:     {human(archive.archive_size)} "
              f"({archive.archive_size / max(archive.size, 1):.1%} of original)")
        print(f"chunk size:  {human(archive.chunk_size)}")
        print(f"chunks:      {len(archive.chunks)} "
              f"({stored} stored raw because they did not compress)")
        if args.chunks:
            for c in archive.chunks:
                print(f"  #{c.index:<5} {human(c.size):>10} -> {human(c.length):>10}"
                      f"{'  (stored)' if c.flags & 1 else ''}")


def _open_archive(args):
    return Archive(args.archive, safe_space=args.safe_space,
                   on_event=_event_logger(args.verbose))


def _describe_window(archive):
    return (f"safe space: {archive.window} chunk(s) x {human(archive.chunk_size)} "
            f"= {human(archive.window * archive.chunk_size)}, "
            f"prefetching {archive.prefetch} ahead")


def cmd_serve(args):
    with _open_archive(args) as archive:
        server = ArchiveServer(archive, args.host, args.port, verbose=args.verbose)
        print(f"Streaming {archive.name} ({human(archive.size)}) at {server.url}")
        print(_describe_window(archive))
        print("Open that URL in any media player. Ctrl+C to stop.")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()


def _player_command(player, url):
    if player == "mpv":
        return ["mpv", "--force-seekable=yes", url]
    if player == "vlc":
        return ["vlc", "--play-and-exit", url]
    if player == "ffplay":
        return ["ffplay", "-autoexit", url]
    return [player, url]


def cmd_play(args):
    player = args.player
    if player is None:
        player = next((p for p in PLAYERS if shutil.which(p)), "browser")
    with _open_archive(args) as archive:
        server = ArchiveServer(archive, verbose=args.verbose)
        server.start_background()
        print(f"Playing {archive.name} ({human(archive.size)}) via {server.url}")
        print(_describe_window(archive))
        try:
            if player == "browser":
                webbrowser.open(server.url)
                print("Opened in the browser. Ctrl+C to stop.")
                while True:
                    time.sleep(3600)
            else:
                subprocess.run(_player_command(player, server.url))
        except KeyboardInterrupt:
            pass
        finally:
            server.shutdown()
            server.server_close()
        s = archive.stats
        print(f"Done: {s.decompressed} chunk decompressions, {s.hits} window hits, "
              f"peak {human(s.peak_resident)} decompressed in memory "
              f"(original is {human(archive.size)}).")


def cmd_cat(args):
    with _open_archive(args) as archive, archive.open() as f:
        f.seek(args.offset)
        remaining = archive.size - args.offset if args.length is None else args.length
        out = sys.stdout.buffer
        try:
            while remaining > 0:
                data = f.read(min(archive.chunk_size, remaining))
                if not data:
                    break
                out.write(data)
                remaining -= len(data)
            out.flush()
        except BrokenPipeError:
            # The consumer (e.g. a player reading stdin) quit early.
            sys.stderr.close()


def cmd_unpack(args):
    with Archive(args.archive) as archive, archive.open() as f:
        out = args.output or (args.archive[:-4] if args.archive.endswith(".nnr")
                              else archive.name or args.archive + ".out")
        if os.path.exists(out) and not args.force:
            sys.exit(f"nanoer: {out} exists (use --force to overwrite)")
        with open(out, "wb") as dst:
            shutil.copyfileobj(f, dst, archive.chunk_size)
        print(f"Extracted {human(archive.size)} -> {out}")


def build_parser():
    parser = argparse.ArgumentParser(
        prog="nanoer",
        description="Chunked compression with live extraction: play or read "
                    "huge compressed files while holding only a small window "
                    "of decompressed data.",
    )
    parser.add_argument("--version", action="version", version=__version__)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("pack", help="compress a file into a .nnr archive")
    p.add_argument("input")
    p.add_argument("-o", "--output", help="archive path (default: INPUT.nnr)")
    p.add_argument("-c", "--chunk-size", type=parse_size, default=DEFAULT_CHUNK_SIZE,
                   help="decompressed size of each chunk, e.g. 16M or 50MB "
                        "(default: 16M)")
    p.add_argument("--codec", choices=available_codecs(), default="lzma")
    p.add_argument("-l", "--level", type=int, help="codec compression level")
    p.add_argument("-j", "--jobs", type=int, help="parallel compression threads")
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("info", help="describe an archive")
    p.add_argument("archive")
    p.add_argument("--chunks", action="store_true", help="list every chunk")
    p.set_defaults(func=cmd_info)

    def window_options(p):
        p.add_argument("archive")
        p.add_argument("-s", "--safe-space", type=parse_size,
                       help="most decompressed data to hold at once "
                            "(default: two chunks)")
        p.add_argument("-v", "--verbose", action="store_true",
                       help="log every chunk load, hit and eviction")

    p = sub.add_parser("play", help="play an archived video with live extraction")
    window_options(p)
    p.add_argument("-p", "--player",
                   help="mpv, vlc, ffplay, browser or any command "
                        "(default: first one installed)")
    p.set_defaults(func=cmd_play)

    p = sub.add_parser("serve", help="stream an archive over HTTP with seeking")
    window_options(p)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8642)
    p.set_defaults(func=cmd_serve)

    p = sub.add_parser("cat", help="write (part of) the original data to stdout")
    window_options(p)
    p.add_argument("--offset", type=int, default=0)
    p.add_argument("--length", type=int)
    p.set_defaults(func=cmd_cat)

    p = sub.add_parser("unpack", help="fully extract an archive")
    p.add_argument("archive")
    p.add_argument("-o", "--output")
    p.add_argument("-f", "--force", action="store_true")
    p.set_defaults(func=cmd_unpack)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        args.func(args)
    except (FormatError, OSError, ValueError) as exc:
        sys.exit(f"nanoer: {exc}")


if __name__ == "__main__":
    main()
