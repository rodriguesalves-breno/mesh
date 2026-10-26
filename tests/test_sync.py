#!/usr/bin/env python3
"""HTTP sync regressions: safe materialization, conflicts, errors and traversal."""
from __future__ import annotations
import contextlib
import glob
import os
import stat
import subprocess
import sys
import time
import urllib.request
from collections.abc import Iterator
from meshtest import (
 check,
 fail,
 project_root,
 read_text,
 run_mesh,
 run_mesh_ok,
 workspace,
 write_text,
)
def start_server(temp: str, mode: str) -> tuple[subprocess.Popen[bytes], int]:
 server_script = os.path.join(project_root(), "tests", "http_server.py")
 port_file = os.path.join(temp, f"port-{mode}")
 try:
  os.unlink(port_file)
 except FileNotFoundError:
  pass
 process = subprocess.Popen(
  [sys.executable, "-B", server_script, "--mode", mode, "--port-file", port_file],
  stdout=subprocess.DEVNULL,
  stderr=subprocess.DEVNULL,
 )
 for _ in range(100):
  if process.poll() is not None:
   fail("test server exited before listening")
  if os.path.exists(port_file) and os.path.getsize(port_file)>0:
   port_text = read_text(port_file)
   if port_text is not None and port_text.strip().isdigit():
    return process, int(port_text.strip())
  time.sleep(0.05)
 process.terminate()
 try:
  process.wait(timeout=5)
 except subprocess.TimeoutExpired:
  process.kill()
 fail("test server did not start")
def stop_server(process: subprocess.Popen[bytes]) -> None:
 if process.poll() is not None:
  return
 process.terminate()
 try:
  process.wait(timeout=5)
 except subprocess.TimeoutExpired:
  process.kill()
  process.wait(timeout=5)
def post_count(port: int) -> int:
 with urllib.request.urlopen(f"http://127.0.0.1:{port}/post-count") as response:
  return int(response.read().decode("ascii"))
@contextlib.contextmanager
def peer(temp: str, mode: str) -> Iterator[int]:
 process, port = start_server(temp, mode)
 try:
  yield port
 finally:
  stop_server(process)
def main() -> int:
 os.environ.pop("XDG_CONFIG_HOME", None)
 os.environ.pop("EMAIL", None)
 temp = workspace("mesh-sync-test")
 home = os.path.join(temp, "home")
 safe = os.path.join(temp, "safe")
 bad = os.path.join(temp, "bad")
 fetch = os.path.join(temp, "fetch")
 for directory in (home, safe, bad, fetch):
  os.makedirs(directory, exist_ok=True)
 run_mesh_ok("home start", home, {"HOME": home}, "start")
 with peer(temp, "safe") as port:
  run_mesh_ok("safe start", safe, {"HOME": home}, "start")
  run_mesh_ok("safe sync", safe, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/")
  fetch_status, _, _ = run_mesh(
   fetch, {}, "obtain", f"http://127.0.0.1:{port}/error", "-o", "err.bin"
  )
  check(fetch_status!=0, "obtain accepted an HTTP error response")
  try:
   error_size = os.path.getsize(os.path.join(fetch, "err.bin"))
  except OSError:
   error_size = 0
  check(error_size==0, "obtain left an error body in the destination")
 regular = read_text(os.path.join(safe, "dir", "file.txt"))
 check(regular=="remote contents\n", "regular file was not materialized")
 check(os.path.islink(os.path.join(safe, "link")), "symlink was not materialized")
 link_target = os.readlink(os.path.join(safe, "link"))
 check(link_target=="dir/file.txt", "symlink target changed")
 check(os.access(os.path.join(safe, "run.sh"), os.X_OK),
   "executable mode was not preserved")
 objects = glob.glob(os.path.join(safe, ".mesh", "store", "objects", "*", "*"))
 check(len(objects)>0, "sync stored no objects")
 for obj in objects:
  object_mode = stat.S_IMODE(os.stat(obj).st_mode)
  check(object_mode==0o444, f"downloaded object is not read-only: {obj}")
 a1 = os.path.join(temp, "a1")
 os.makedirs(a1, exist_ok=True)
 run_mesh_ok("a1 start", a1, {"HOME": home}, "start")
 write_text(os.path.join(a1, "file.txt"), "local\n")
 run_mesh_ok("a1 save", a1, {"HOME": home}, "save", "local snapshot")
 with peer(temp, "error-echo") as port:
  a1_status, a1_out, a1_err = run_mesh(
   a1, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/"
  )
  check(a1_status!=0, "sync accepted an HTTP 500 response")
  check("HTTP 500" in a1_err, "sync did not report the HTTP 500 status")
  check("already up to date" not in a1_out,
    "HTTP 500 with an echoed hash was masked as up to date")
 m4 = os.path.join(temp, "m4")
 os.makedirs(m4, exist_ok=True)
 run_mesh_ok("m4 start", m4, {"HOME": home}, "start")
 write_text(os.path.join(m4, "file.txt"), "local\n")
 run_mesh_ok("m4 save", m4, {"HOME": home}, "save", "local snapshot")
 write_text(os.path.join(m4, ".mesh", "HEAD"), ("f"*128)+"\n")
 m4_status, _, m4_err = run_mesh(m4, {"HOME": home}, "sync", "http://127.0.0.1:1/")
 check(m4_status!=0, "sync accepted HEAD pointing at a missing snapshot")
 check("missing from history" in m4_err, "sync did not explain the dangling HEAD")
 m5 = os.path.join(temp, "m5")
 os.makedirs(m5, exist_ok=True)
 run_mesh_ok("m5 start", m5, {"HOME": home}, "start")
 write_text(os.path.join(m5, "file.txt"), "local\n")
 run_mesh_ok("m5 save", m5, {"HOME": home}, "save", "local snapshot")
 with peer(temp, "spurious-object") as port:
  m5_status, _, m5_err = run_mesh(
   m5, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/"
  )
  check(m5_status==0, f"sync failed when the peer requested a foreign object: {m5_err}")
 check("outside the current tree" in m5_err,
    "sync did not warn about the peer-requested foreign object")
 cas = os.path.join(temp, "cas-stale")
 os.makedirs(cas, exist_ok=True)
 run_mesh_ok("CAS start", cas, {"HOME": home}, "start")
 write_text(os.path.join(cas, "local.txt"), "local snapshot\n")
 run_mesh_ok("CAS save", cas, {"HOME": home}, "save", "local snapshot")
 with peer(temp, "cas-stale") as port:
  cas_status, _, cas_err = run_mesh(
   cas, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/"
  )
  check(cas_status!=0, "sync accepted a stale conditional publish")
  check("remote changed since GET" in cas_err,
   "sync did not explain the stale CAS response")
  check(post_count(port)==1, "sync did not attempt the conditional POST")
 conflict = os.path.join(temp, "conflict")
 os.makedirs(conflict, exist_ok=True)
 write_text(os.path.join(conflict, "run.sh"), "#!/bin/sh\necho local\n")
 os.chmod(os.path.join(conflict, "run.sh"), 0o755)
 with peer(temp, "conflict") as port:
  run_mesh_ok("conflict start", conflict, {"HOME": home}, "start")
  run_mesh_ok("conflict save", conflict, {"HOME": home}, "save", "local snapshot")
  conflict_status, _, conflict_err = run_mesh(
   conflict, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/"
  )
  check(conflict_status!=0, "sync reported success with an unresolved conflict")
  check("snapshot not published" in conflict_err,
    "sync did not explain why the snapshot was not published")
  check(post_count(port)==0, "sync published a snapshot with a new conflict")
  pending_status, _, _ = run_mesh(
   conflict, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/"
  )
  check(pending_status!=0, "sync ignored an already pending conflict")
  check(post_count(port)==0, "sync published while a conflict was already pending")
  run_mesh_ok("choose beta", conflict, {"HOME": home}, "choose", "beta")
  run_mesh_ok("save resolution", conflict, {"HOME": home}, "save", "resolve")
  run_mesh_ok("sync resolved", conflict, {"HOME": home}, "sync",
    f"http://127.0.0.1:{port}/")
  check(post_count(port)==1, "sync did not publish after conflict resolution and save")
  chosen = read_text(os.path.join(conflict, "run.sh"))
  check(chosen=="#!/bin/sh\necho remote-conflict\n",
    "beta variant was not selected")
  check(os.access(os.path.join(conflict, "run.sh"), os.X_OK),
    "conflict resolution lost executable mode")
 type_file = os.path.join(temp, "type-conflict-file")
 os.makedirs(type_file, exist_ok=True)
 type_file_path = os.path.join(type_file, "run.sh")
 write_text(type_file_path, "local regular file\n")
 run_mesh_ok("type file start", type_file, {"HOME": home}, "start")
 run_mesh_ok("type file save", type_file, {"HOME": home}, "save", "local file")
 with peer(temp, "symlink-conflict") as port:
  type_status, _, _ = run_mesh(
   type_file, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/"
  )
  check(type_status!=0, "sync reported success with an unresolved type conflict")
  check(read_text(type_file_path)=="local regular file\n",
    "symlink/regular conflict changed the local regular file")
  check(os.path.exists(os.path.join(type_file, ".mesh", "store", "conflicts", "index")),
    "symlink/regular conflict did not stage typed variants")
  run_mesh_ok("choose remote symlink", type_file, {"HOME": home}, "choose", "beta")
  check(os.path.islink(type_file_path) and os.readlink(type_file_path)=="remote-link-target",
    "choosing beta did not materialize the remote symlink")
  check(post_count(port)==0, "sync published after a symlink/regular conflict")
 type_link = os.path.join(temp, "type-conflict-link")
 os.makedirs(type_link, exist_ok=True)
 type_link_path = os.path.join(type_link, "run.sh")
 os.symlink("local-target", type_link_path)
 run_mesh_ok("type link start", type_link, {"HOME": home}, "start")
 run_mesh_ok("type link save", type_link, {"HOME": home}, "save", "local link")
 with peer(temp, "conflict") as port:
  type_status, _, _ = run_mesh(
   type_link, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/"
  )
  check(type_status!=0, "sync reported success with an unresolved reverse type conflict")
  check(os.path.islink(type_link_path) and os.readlink(type_link_path)=="local-target",
    "regular/symlink conflict changed the local symlink")
  check(os.path.exists(os.path.join(type_link, ".mesh", "store", "conflicts", "index")),
    "regular/symlink conflict did not stage typed variants")
  run_mesh_ok("choose local symlink", type_link, {"HOME": home}, "choose", "alpha")
  check(os.path.islink(type_link_path) and os.readlink(type_link_path)=="local-target",
    "choosing alpha did not preserve the local symlink")
  check(post_count(port)==0, "sync published after a regular/symlink conflict")
 type_link_beta = os.path.join(temp, "type-conflict-link-beta")
 os.makedirs(type_link_beta, exist_ok=True)
 type_link_beta_path = os.path.join(type_link_beta, "run.sh")
 os.symlink("local-target", type_link_beta_path)
 run_mesh_ok("type link beta start", type_link_beta, {"HOME": home}, "start")
 run_mesh_ok("type link beta save", type_link_beta, {"HOME": home}, "save", "local link")
 with peer(temp, "conflict") as port:
  type_status, _, _ = run_mesh(
   type_link_beta, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/"
  )
  check(type_status!=0, "sync did not stage the reverse type conflict")
  run_mesh_ok("choose remote regular", type_link_beta, {"HOME": home}, "choose", "beta")
  check(not os.path.islink(type_link_beta_path),
    "choosing beta did not replace the local symlink with a regular file")
  check(read_text(type_link_beta_path)=="#!/bin/sh\necho remote-conflict\n",
    "choosing beta did not restore remote regular-file content")
 with peer(temp, "traversal") as port:
  run_mesh_ok("bad start", bad, {"HOME": home}, "start")
  traversal_status, _, _ = run_mesh(
   bad, {"HOME": home}, "sync", f"http://127.0.0.1:{port}/"
  )
  check(traversal_status!=0, "unsafe remote manifest was accepted")
 check(not os.path.exists(os.path.join(bad, ".git", "pwned")),
   "traversal wrote .git/pwned")
 print("sync tests: ok")
 return 0
if __name__=="__main__":
 sys.exit(main())
