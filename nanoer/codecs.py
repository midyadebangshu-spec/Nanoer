"""Compression codecs usable for chunk payloads.

Every chunk is compressed independently, so any codec that can round-trip a
single buffer works. Only standard-library codecs are required; zstd is used
when either Python 3.14's ``compression.zstd`` or the ``zstandard`` package is
installed.
"""

import bz2
import lzma
import zlib


class Codec:
    def __init__(self, codec_id, name, compress, decompress, default_level):
        self.id = codec_id
        self.name = name
        self._compress = compress
        self._decompress = decompress
        self.default_level = default_level

    def compress(self, data, level=None):
        return self._compress(data, self.default_level if level is None else level)

    def decompress(self, data):
        return self._decompress(data)


def _lzma_compress(data, level):
    return lzma.compress(data, format=lzma.FORMAT_XZ, preset=level)


def _zstd_functions():
    try:
        from compression import zstd  # Python 3.14+

        return (
            lambda data, level: zstd.compress(data, level=level),
            zstd.decompress,
        )
    except ImportError:
        pass
    try:
        import zstandard
    except ImportError:
        return None
    return (
        lambda data, level: zstandard.ZstdCompressor(level=level).compress(data),
        lambda data: zstandard.ZstdDecompressor().decompress(data),
    )


CODECS = {}


def _register(codec):
    CODECS[codec.id] = codec
    CODECS[codec.name] = codec


_register(Codec(0, "none", lambda data, level: bytes(data), bytes, 0))
_register(Codec(1, "zlib", zlib.compress, zlib.decompress, 6))
_register(Codec(2, "bz2", bz2.compress, bz2.decompress, 9))
_register(Codec(3, "lzma", _lzma_compress, lzma.decompress, 6))

_zstd = _zstd_functions()
if _zstd is not None:
    _register(Codec(4, "zstd", _zstd[0], _zstd[1], 9))


def get_codec(key):
    """Look a codec up by numeric id (from an archive) or by name (from the CLI)."""
    try:
        return CODECS[key]
    except KeyError:
        if key in (4, "zstd"):
            raise ValueError(
                "zstd needs Python 3.14+ or `pip install zstandard`"
            ) from None
        raise ValueError(f"unknown codec: {key!r}") from None


def available_codecs():
    return sorted(name for name in CODECS if isinstance(name, str))
