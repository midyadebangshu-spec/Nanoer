"""On-disk layout of a ``.nnr`` archive.

    +----------------------+
    | header               |  magic, version, codec, chunk size, name length
    | original file name   |  UTF-8, used for the Content-Type when streaming
    +----------------------+
    | chunk 0              |  each chunk compressed on its own, so it can be
    | chunk 1              |  decompressed without touching any other chunk
    | ...                  |
    +----------------------+
    | index                |  one entry per chunk: offset, length, CRC, flags
    +----------------------+
    | trailer              |  index offset, original size, chunk count, magic
    +----------------------+

The index sits at the end so an archive can be written in a single pass, even
to a pipe. Readers find it through the fixed-size trailer.
"""

import struct

MAGIC = b"NANOER\x1a\n"
END_MAGIC = b"NNR-END\n"
VERSION = 1

# magic, version, codec id, reserved, chunk size, name length
HEADER = struct.Struct("<8sBBHQH")
# offset of compressed data, compressed length, CRC-32 of the original bytes, flags
INDEX_ENTRY = struct.Struct("<QIIB3x")
# index offset, original size, chunk count, end magic
TRAILER = struct.Struct("<QQI4x8s")

# The chunk was stored uncompressed because compressing it did not help.
FLAG_STORED = 0x01


class FormatError(Exception):
    """The file is not a valid Nanoer archive."""


class CorruptChunkError(FormatError):
    """A chunk failed to decompress or did not match its checksum."""
