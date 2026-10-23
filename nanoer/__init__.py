"""Nanoer: chunked compression with live extraction."""

__version__ = "0.1.0"

from .format import CorruptChunkError, FormatError
from .packer import pack
from .reader import Archive, ArchiveFile

__all__ = ["Archive", "ArchiveFile", "CorruptChunkError", "FormatError", "pack"]
