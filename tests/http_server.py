#!/usr/bin/env python3
"""Deterministic Mesh peer used by tests/test_sync.py.
The manifest body is controlled by --mode.  Unsafe paths are included on
purpose so the test can prove that materialization rejects them before any
working-tree write happens.
"""
from __future__ import annotations
import argparse
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit
from meshtest import blake2b_hex
MODES = ("safe", "traversal", "conflict", "symlink-conflict", "error-echo", "spurious-object", "cas-stale")
def build_fixture(mode: str) -> tuple[dict[str, bytes], bytes, str, str]:
 if mode=="traversal":
  entries = [(b".git/pwned", b"owned\n", 0o644, b"f")]
 else:
  if mode=="symlink-conflict":
   run = b"remote-link-target"
   run_kind = b"l"
   run_mode = 0o777
  else:
   run = (
    b"#!/bin/sh\necho remote-conflict\n"
    if mode=="conflict"
    else b"#!/bin/sh\necho remote\n"
   )
   run_kind = b"f"
   run_mode = 0o755
  entries = [
   (b"dir/file.txt", b"remote contents\n", 0o644, b"f"),
   (b"run.sh", run, run_mode, run_kind),
  ]
 objects: dict[str, bytes] = {}
 lines: list[bytes] = []
 for path, data, modebits, kind in entries:
  digest = blake2b_hex(data)
  objects[digest] = data
  lines.append(
   b"F %s %d 1 0 %o %s %s\n"%(digest.encode("ascii"), len(data), modebits, kind, path)
  )
 if mode!="traversal":
  target = b"dir/file.txt"
  digest = blake2b_hex(target)
  objects[digest] = target
  lines.append(
   b"F %s %d 1 0 777 l link\n"%(digest.encode("ascii"), len(target))
  )
 manifest = b"".join(lines)
 tree = blake2b_hex(manifest)
 head = "a"*128
 return objects, manifest, tree, head
class PeerHandler(BaseHTTPRequestHandler):
 protocol_version = "HTTP/1.1"
 server_version = "MeshPeer/1.0"
 objects: dict[str, bytes] = {}
 manifest = b""
 tree = ""
 head = ""
 mode = "safe"
 post_count = 0
 def log_message(self, format: str, *args: object) -> None:
  return
 def reply(
  self,
  status: int,
  reason: str,
  content_type: str,
  body: bytes,
  extra: dict[str, str]|None = None,
 ) -> None:
  self.send_response(status, reason)
  self.send_header("Content-Type", content_type)
  for name in sorted((extra or {}).keys()):
   self.send_header(name, str(extra[name]))
  self.send_header("Content-Length", str(len(body)))
  self.send_header("Connection", "close")
  self.end_headers()
  if body:
   self.wfile.write(body)
 def do_GET(self) -> None:
  path = urlsplit(self.path).path
  if path=="/post-count":
   self.reply(200, "OK", "text/plain", str(self.post_count).encode("ascii"))
   return
  if self.mode=="error-echo":
   self.reply(
    500,
    "Internal Server Error",
    "text/plain",
    b"server error\n",
    {
     "X-Mesh-Hash": self.headers.get("X-Mesh-Hash", self.head),
     "X-Mesh-Tree": self.headers.get("X-Mesh-Tree", self.tree),
    },
   )
   return
  if path=="/error":
   self.reply(500, "Internal Server Error", "text/plain", b"server error\n")
   return
  match = re.fullmatch(r"/objects/([0-9a-f]{128})", path)
  if match:
   data = self.objects.get(match.group(1))
   if data is None:
    self.reply(404, "Not Found", "text/plain", b"")
    return
   self.reply(200, "OK", "application/octet-stream", data)
   return
  self.reply(
   200,
   "OK",
   "text/plain",
   self.manifest,
   {
    "X-Mesh-Hash": self.head,
    "X-Mesh-Tree": self.tree,
    **({"X-Mesh-CAS": "if-match", "ETag": '"remote-v0"'}
       if self.mode=="cas-stale" else {}),
   },
  )
 def do_POST(self) -> None:
  type(self).post_count += 1
  if self.mode=="error-echo":
   self.reply(
    500,
    "Internal Server Error",
    "text/plain",
    b"server error\n",
    {
     "X-Mesh-Hash": self.headers.get("X-Mesh-Hash", self.head),
     "X-Mesh-Tree": self.headers.get("X-Mesh-Tree", self.tree),
    },
   )
   return
  if self.mode=="cas-stale" and self.headers.get("If-Match")!='"remote-v1"':
   self.rfile.read(int(self.headers.get("Content-Length", "0")))
   self.reply(412,"Precondition Failed","text/plain",b"remote version changed\n")
   return
  length = self.headers.get("Content-Length", "0")
  if not length.isdigit():
   length = "0"
  self.rfile.read(int(length))
  missing = '"'+("b"*128)+'"' if self.mode=="spurious-object" else ""
  body = ('{"ok":true,"missing_objects":['+missing+"]}").encode("ascii")
  self.reply(200, "OK", "application/json", body)
 def do_PUT(self) -> None:
  self.reply(405, "Method Not Allowed", "text/plain", b"method not allowed\n")
 def do_DELETE(self) -> None:
  self.reply(405, "Method Not Allowed", "text/plain", b"method not allowed\n")
 def do_PATCH(self) -> None:
  self.reply(405, "Method Not Allowed", "text/plain", b"method not allowed\n")
def main() -> int:
 parser = argparse.ArgumentParser()
 parser.add_argument("--mode", default="safe", choices=MODES)
 parser.add_argument("--port-file", required=True)
 options = parser.parse_args()
 objects, manifest, tree, head = build_fixture(options.mode)
 PeerHandler.objects = objects
 PeerHandler.manifest = manifest
 PeerHandler.tree = tree
 PeerHandler.head = head
 PeerHandler.mode = options.mode
 PeerHandler.post_count = 0
 server = ThreadingHTTPServer(("127.0.0.1", 0), PeerHandler)
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
