"""Stream an archive to a media player over local HTTP.

Video players (mpv, VLC, ffplay, browsers) already know how to play a file
over HTTP and how to seek within it with ``Range`` requests. The server
answers each request by live-extracting only the chunks that cover the
requested bytes, so the player sees an ordinary file and the disk never
holds the decompressed video.
"""

import mimetypes
import re
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote

WRITE_SIZE = 256 * 1024
_RANGE = re.compile(r"bytes=(\d*)-(\d*)$")


def parse_range(header, size):
    """Return ``(start, end)`` (end inclusive) for a ``Range`` header.

    Returns ``None`` when the whole file should be sent and raises
    ``ValueError`` when the range cannot be satisfied. Only the first range of
    a multi-range request is honoured; players do not send those.
    """
    if not header:
        return None
    match = _RANGE.match(header.split(",")[0].strip())
    if not match:
        return None
    first, last = match.groups()
    if not first and not last:
        return None
    if not first:  # "bytes=-500": the last 500 bytes
        length = int(last)
        if length == 0:
            raise ValueError("empty suffix range")
        return max(0, size - length), size - 1
    start = int(first)
    end = min(int(last), size - 1) if last else size - 1
    if start >= size or end < start:
        raise ValueError("range not satisfiable")
    return start, end


class _Handler(BaseHTTPRequestHandler):
    server_version = "Nanoer"
    protocol_version = "HTTP/1.1"

    def do_HEAD(self):
        self._serve(send_body=False)

    def do_GET(self):
        self._serve(send_body=True)

    def _serve(self, send_body):
        archive = self.server.archive
        size = archive.size
        try:
            byte_range = parse_range(self.headers.get("Range"), size)
        except ValueError:
            self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if byte_range is None:
            start, end = 0, size - 1
            self.send_response(HTTPStatus.OK)
        else:
            start, end = byte_range
            self.send_response(HTTPStatus.PARTIAL_CONTENT)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Type", self.server.content_type)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        if not send_body or size == 0:
            return

        remaining = end - start + 1
        with archive.open() as f:
            f.seek(start)
            try:
                while remaining > 0:
                    data = f.read(min(WRITE_SIZE, remaining))
                    if not data:
                        break
                    self.wfile.write(data)
                    remaining -= len(data)
            except (BrokenPipeError, ConnectionResetError):
                # Players drop connections all the time when they seek.
                self.close_connection = True

    def log_message(self, format, *args):
        if self.server.verbose:
            super().log_message(format, *args)


class ArchiveServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, archive, host="127.0.0.1", port=0, verbose=False):
        super().__init__((host, port), _Handler)
        self.archive = archive
        self.verbose = verbose
        self.content_type = (
            mimetypes.guess_type(archive.name)[0] or "application/octet-stream"
        )

    @property
    def url(self):
        host, port = self.server_address[:2]
        if ":" in host:
            host = f"[{host}]"
        return f"http://{host}:{port}/{quote(self.archive.name or 'stream')}"

    def start_background(self):
        thread = threading.Thread(target=self.serve_forever, daemon=True)
        thread.start()
        return thread
