"""Turn an ordinary file into a chunked ``.nnr`` archive."""

import os
import zlib
from collections import deque
from concurrent.futures import ThreadPoolExecutor

from .codecs import get_codec
from .format import (
    END_MAGIC,
    FLAG_STORED,
    HEADER,
    INDEX_ENTRY,
    MAGIC,
    TRAILER,
    VERSION,
)

DEFAULT_CHUNK_SIZE = 16 * 1024 * 1024
MAX_CHUNK_SIZE = 1024 * 1024 * 1024


def _compress_chunk(codec, level, data):
    crc = zlib.crc32(data)
    packed = codec.compress(data, level)
    if len(packed) >= len(data):
        # Already-compressed data (most video, audio, images) often grows when
        # compressed again. Storing it raw keeps the archive no larger than the
        # original and makes "extracting" that chunk a plain copy.
        return bytes(data), crc, FLAG_STORED
    return packed, crc, 0


def pack(
    src,
    dst,
    chunk_size=DEFAULT_CHUNK_SIZE,
    codec="lzma",
    level=None,
    jobs=None,
    name=None,
    progress=None,
):
    """Compress ``src`` into ``dst`` in independent chunks of ``chunk_size`` bytes.

    ``src`` and ``dst`` may be paths or binary file objects. ``progress`` is
    called as ``progress(bytes_read, bytes_written)`` after every chunk.
    Returns ``(original_size, archive_size, chunk_count)``.
    """
    if not 0 < chunk_size <= MAX_CHUNK_SIZE:
        raise ValueError(f"chunk size must be between 1 byte and {MAX_CHUNK_SIZE}")
    codec = get_codec(codec)
    jobs = jobs or min(4, os.cpu_count() or 1)

    if name is None:
        name = os.path.basename(src) if isinstance(src, (str, os.PathLike)) else ""
    name_bytes = os.fsencode(name) if not isinstance(name, bytes) else name
    name_bytes = name_bytes[:0xFFFF]

    src_file = open(src, "rb") if isinstance(src, (str, os.PathLike)) else src
    dst_file = open(dst, "wb") if isinstance(dst, (str, os.PathLike)) else dst
    try:
        header = HEADER.pack(MAGIC, VERSION, codec.id, 0, chunk_size, len(name_bytes))
        dst_file.write(header)
        dst_file.write(name_bytes)
        offset = len(header) + len(name_bytes)
        total = 0
        index = []

        def write(result):
            nonlocal offset
            payload, crc, flags = result
            dst_file.write(payload)
            index.append(INDEX_ENTRY.pack(offset, len(payload), crc, flags))
            offset += len(payload)
            if progress:
                progress(total, offset)

        # Compress several chunks in parallel (zlib, bz2 and lzma release the
        # GIL) while writing them out strictly in order. At most ``jobs * 2``
        # chunks are in flight, which bounds memory use.
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            pending = deque()
            while True:
                data = src_file.read(chunk_size)
                if not data:
                    break
                while len(data) < chunk_size:
                    more = src_file.read(chunk_size - len(data))
                    if not more:
                        break
                    data += more
                total += len(data)
                pending.append(pool.submit(_compress_chunk, codec, level, data))
                if len(pending) >= jobs * 2:
                    write(pending.popleft().result())
            while pending:
                write(pending.popleft().result())

        index_offset = offset
        dst_file.write(b"".join(index))
        dst_file.write(TRAILER.pack(index_offset, total, len(index), END_MAGIC))
        archive_size = index_offset + len(index) * INDEX_ENTRY.size + TRAILER.size
        return total, archive_size, len(index)
    finally:
        if src_file is not src:
            src_file.close()
        if dst_file is not dst:
            dst_file.close()
