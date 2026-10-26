#!/usr/bin/env python3
"""Static archive peer used by tests/test_obtain.py.
The archive is exposed at the Gitea-style path and at the explicit archive
path.  Every other archive path is a 404, which makes an incorrect URL mapping
visible.  Redirect, request-metadata and gzip-encoded endpoints exercise the
curl-compatible download options.  Range requests are supported so the
download resume path is exercised.
"""
from __future__ import annotations
import argparse
import gzip
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit
MODES = ("safe", "corrupt", "missing")
class ArchiveHandler(BaseHTTPRequestHandler):
 protocol_version = "HTTP/1.1"
 server_version = "MeshObtainPeer/1.0"
 body = b""
 mode = "safe"
 def log_message(self, format: str, *args: object) -> None:
  return
 def reply(
  self,
  status: int,
  reason: str,
  content_type: str,
  body: bytes,
  extra: tuple[str, str]|None = None,
  send_body: bool = True,
 ) -> None:
  self.send_response(status, reason)
  self.send_header("Content-Type", content_type)
  if extra:
   self.send_header(extra[0], extra[1])
  self.send_header("Content-Length", str(len(body)))
  self.send_header("Connection", "close")
  self.end_headers()
  if body and send_body:
   self.wfile.write(body)
 def do_HEAD(self) -> None:
  path = urlsplit(self.path).path
  if path=="/redirect":
   self.reply(
    302,
    "Found",
    "text/plain",
    b"",
    ("Location", "/source.tar.gz"),
    send_body=False,
   )
   return
  if self.mode=="missing" or not self._archive_path(path):
   self.reply(
    404,
    "Not Found",
    "text/plain",
    b"not found\n",
    send_body=False,
   )
   return
  self.reply(
   200,
   "OK",
   "application/gzip",
   self.body,
   send_body=False,
  )
 def do_GET(self) -> None:
  path = urlsplit(self.path).path
  if path=="/redirect":
   self.reply(
    302,
    "Found",
    "text/plain",
    b"",
    ("Location", "/source.tar.gz"),
   )
   return
  if path=="/cookie-required":
   if "session=ok" not in self.headers.get("Cookie", ""):
    self.reply(403, "Forbidden", "text/plain", b"missing cookie\n")
    return
   self.reply(200, "OK", "application/gzip", self.body)
   return
  if path=="/set-cookie":
   self.reply(
    200,
    "OK",
    "application/gzip",
    self.body,
    ("Set-Cookie", "session=ok; Path=/"),
   )
   return
  if path=="/curl-options":
   expected = {
    "X-Mesh-Test": "yes",
    "User-Agent": "mesh-curl-test",
    "Referer": "https://example.test/page",
   }
   if any(self.headers.get(name)!=value for name, value in expected.items()):
    self.reply(403, "Forbidden", "text/plain", b"missing option\n")
    return
   self.reply(200, "OK", "application/gzip", self.body)
   return
  if path=="/compressed.txt":
   payload = gzip.compress(b"compressed download\n")
   self.reply(
    200,
    "OK",
    "text/plain",
    payload,
    ("Content-Encoding", "gzip"),
   )
   return
  if self.mode=="missing" or not self._archive_path(path):
   self.reply(404, "Not Found", "text/plain", b"not found\n")
   return
  body = b"not a gzip stream\n" if self.mode=="corrupt" else self.body
  range_header = self.headers.get("Range", "")
  match = re.fullmatch(r"bytes=(\d+)-\s*", range_header, re.IGNORECASE)
  if match:
   start = int(match.group(1))
   if start>0:
    if start>=len(body):
     self.reply(
      416,
      "Range Not Satisfiable",
      "text/plain",
      b"",
      ("Content-Range", f"bytes */{len(body)}"),
     )
     return
    part = body[start:]
    self.reply(
     206,
     "Partial Content",
     "application/gzip",
     part,
     ("Content-Range", f"bytes {start}-{len(body)-1}/{len(body)}"),
    )
    return
  self.reply(200, "OK", "application/gzip", body)
 @staticmethod
 def _archive_path(path: str) -> bool:
  return bool(
   re.fullmatch(r"/[^/]+/[^/]+/archive/.+\.tar\.gz", path)
   or path in ("/source.tar.gz", "/owner/repo")
  )
def main() -> int:
 parser = argparse.ArgumentParser()
 parser.add_argument("--archive", required=True)
 parser.add_argument("--mode", default="safe", choices=MODES)
 parser.add_argument("--port-file", required=True)
 options = parser.parse_args()
 ArchiveHandler.body = Path(options.archive).read_bytes()
 ArchiveHandler.mode = options.mode
 server = ThreadingHTTPServer(("127.0.0.1", 0), ArchiveHandler)
 server.daemon_threads = True
 Path(options.port_file).write_text(str(server.server_port), encoding="ascii")
 try:
  server.serve_forever()
 except KeyboardInterrupt:
  pass
 finally:
  server.server_close()
 return 0
if __name__=="__main__":
 sys.exit(main())
