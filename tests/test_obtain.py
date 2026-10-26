#!/usr/bin/env python3
"""Archive extraction and resumable download regressions for mesh obtain.
The archive builder is deliberately small and explicit: it writes USTAR
headers, a PAX path record and gzip framing directly.  That keeps the safety
cases (traversal, reserved namespace, symlink escape) independent from the C
implementation under test.
"""
from __future__ import annotations
import gzip
import json
import os
import re
import stat
import subprocess
import sys
import time
from meshtest import (
 check,
 fail,
 mesh_binary,
 project_root,
 read_bytes,
 read_text,
 run_mesh,
 run_mesh_ok,
 workspace,
 write_bytes,
 write_text,
)
Entry = tuple[bytes, bytes, bytes, int]
def put_field(header: bytearray, offset: int, length: int, value: bytes) -> None:
 header[offset:offset+length] = value[:length].ljust(length, b"\0")
def split_ustar(name: bytes) -> tuple[bytes, bytes]:
 if len(name)<=100:
  return name, b""
 for index in range(len(name)-1,0,-1):
  if name[index:index+1]!=b"/":
   continue
  prefix = name[:index]
  field = name[index+1:]
  if len(field)<=100 and len(prefix)<=155:
   return field, prefix
 return name[:100], b""
def tar_entry(name: bytes, kind: bytes, data: bytes, mode: int) -> bytes:
 field, prefix = split_ustar(name)
 payload = data if kind in (b"0", b"x") else b""
 header = bytearray(b"\0"*512)
 put_field(header, 0, 100, field)
 header[100:108] = f"{mode:07o}\0".encode("ascii")
 header[108:116] = b"0000000\0"
 header[116:124] = b"0000000\0"
 header[124:136] = f"{len(payload):011o}\0".encode("ascii")
 header[136:148] = f"{1700000000:011o}\0".encode("ascii")
 header[148:156] = b"        "
 header[156:157] = kind
 put_field(header, 157, 100, data if kind==b"2" else b"")
 header[257:263] = b"ustar\0"
 header[263:265] = b"00"
 put_field(header, 345, 155, prefix)
 checksum = sum(header)
 header[148:156] = f"{checksum:06o}\0 ".encode("ascii")
 padding = b"\0"*((512-(len(payload)%512))%512)
 return bytes(header)+payload+padding
def pax_record(key: bytes, value: bytes) -> bytes:
 record = key+b"="+value+b"\n"
 length = len(record)+2
 while True:
  next_length = len(record)+len(str(length))+1
  if next_length==length:
   break
  length = next_length
 return str(length).encode("ascii")+b" "+record
def safe_entries() -> list[Entry]:
 entries: list[Entry] = [
  (b"repo-HEAD/", b"5", b"", 0o755),
  (b"repo-HEAD/README.md", b"0", b"hello\n", 0o644),
  (b"repo-HEAD/bin/", b"5", b"", 0o755),
  (b"repo-HEAD/bin/run.sh", b"0", b"#!/bin/sh\necho run\n", 0o755),
  (b"repo-HEAD/link", b"2", b"README.md", 0o777),
  (b"repo-HEAD/empty", b"0", b"", 0o644),
  (b"repo-HEAD/.github/workflows/ci.yml", b"0", b"name: ci\n", 0o644),
  (b"repo-HEAD/nested/deep/file.txt", b"0", b"deep\n", 0o644),
 ]
 prefix = b"repo-HEAD/prefix/"+(b"ustar-segment-"*6)+b"file.txt"
 entries.append((prefix, b"0", b"prefix\n", 0o644))
 long_name = b"repo-HEAD/very/"+(b"long-segment-"*8)+b"file.txt"
 entries.append((b"PaxHeader", b"x", pax_record(b"path", long_name), 0o644))
 entries.append((b"repo-HEAD/pax-placeholder", b"0", b"pax\n", 0o644))
 return entries
def traversal_entries() -> list[Entry]:
 return [
  (b"repo-HEAD/", b"5", b"", 0o755),
  (b"repo-HEAD/../../mesh-escape.txt", b"0", b"escaped\n", 0o644),
 ]
def reserved_entries() -> list[Entry]:
 return [
  (b"repo-HEAD/", b"5", b"", 0o755),
  (b"repo-HEAD/.mesh/credentials", b"0", b"hijack\n", 0o644),
 ]
def symlink_entries() -> list[Entry]:
 return [
  (b"repo-HEAD/", b"5", b"", 0o755),
  (b"repo-HEAD/link", b"2", b"../../outside", 0o777),
  (b"repo-HEAD/link/owned.txt", b"0", b"pwned\n", 0o644),
 ]
def pax_overflow_entries() -> list[Entry]:
 return [
  (b"repo-HEAD/", b"5", b"", 0o755),
  (b"PaxHeader", b"x", pax_record(b"size", b"18446744073709551616"), 0o644),
  (b"repo-HEAD/overflow", b"0", b"x", 0o644),
 ]
ARCHIVE_BUILDERS = {
 "safe": safe_entries,
 "traversal": traversal_entries,
 "reserved": reserved_entries,
 "symlink": symlink_entries,
 "pax-overflow": pax_overflow_entries,
}
def build_archive(path: str, kind: str) -> None:
 builder = ARCHIVE_BUILDERS.get(kind)
 if builder is None:
  fail(f"unknown archive kind {kind}")
 entries = builder()
 archive = b"".join(tar_entry(*entry) for entry in entries)+b"\0"*1024
 try:
  with gzip.open(path, "wb") as stream:
   stream.write(archive)
 except OSError as exc:
  fail(f"cannot create {path}: {exc}")
def start_server(
 temp: str, archive: str, mode: str, servers: list[subprocess.Popen[bytes]]
) -> int:
 server_script = os.path.join(project_root(), "tests", "obtain_server.py")
 port_file = os.path.join(temp, f"server-{len(servers)}.port")
 try:
  os.unlink(port_file)
 except FileNotFoundError:
  pass
 process = subprocess.Popen(
  [
   sys.executable,
   "-B",
   server_script,
   "--archive",
   archive,
   "--mode",
   mode,
   "--port-file",
   port_file,
  ],
  stdout=subprocess.DEVNULL,
  stderr=subprocess.DEVNULL,
 )
 servers.append(process)
 for _ in range(200):
  if process.poll() is not None:
   fail("obtain server exited before listening")
  if os.path.exists(port_file) and os.path.getsize(port_file)>0:
   port_text = read_text(port_file)
   if port_text is not None and port_text.strip().isdigit():
    return int(port_text.strip())
  time.sleep(0.05)
 process.terminate()
 fail("obtain server did not write its port file")
def stop_servers(servers: list[subprocess.Popen[bytes]]) -> None:
 for process in servers:
  if process.poll() is None:
   process.terminate()
 for process in servers:
  try:
   process.wait(timeout=5)
  except subprocess.TimeoutExpired:
   process.kill()
   process.wait(timeout=5)
def contents_equal(path: str, wanted: bytes|str, label: str) -> None:
 data = read_bytes(path)
 check(data is not None, f"{label}: missing file {path}")
 expected = wanted.encode("utf-8") if isinstance(wanted, str) else wanted
 check(data==expected, f"{label}: unexpected contents in {path}")
def main() -> int:
 os.environ.pop("XDG_CONFIG_HOME", None)
 os.environ.pop("EMAIL", None)
 os.environ.pop("MESH_TOKEN", None)
 temp = workspace("mesh-obtain")
 home = os.path.join(temp, "home")
 work = os.path.join(temp, "work")
 os.makedirs(home, exist_ok=True)
 os.makedirs(work, exist_ok=True)
 env = {"HOME": home}
 def run_obtain(directory: str, *arguments: str) -> tuple[int, str, str]:
  return run_mesh(directory, env, "obtain", *arguments)
 servers: list[subprocess.Popen[bytes]] = []
 try:
  archive = os.path.join(temp, "safe.tar.gz")
  build_archive(archive, "safe")
  safe_port = start_server(temp, archive, "safe", servers)
  url = f"http://127.0.0.1:{safe_port}/owner/repo"
  status, _, err = run_obtain(work, url)
  check(status==0, f"default obtain failed: {err}")
  clone = os.path.join(work, "repo")
  check(os.path.isdir(os.path.join(clone, ".mesh")), "default destination has .mesh")
  contents_equal(os.path.join(clone, "README.md"), "hello\n", "README")
  contents_equal(os.path.join(clone, "empty"), "", "empty file")
  contents_equal(os.path.join(clone, "bin", "run.sh"),
      "#!/bin/sh\necho run\n", "executable")
  contents_equal(os.path.join(clone, ".github", "workflows", "ci.yml"),
      "name: ci\n", "dot directory")
  contents_equal(os.path.join(clone, "nested", "deep", "file.txt"),
      "deep\n", "nested file")
  long_rel = "very/"+("long-segment-"*8)+"file.txt"
  contents_equal(os.path.join(clone, *long_rel.split("/")), "pax\n", "PAX path")
  prefix_rel = "prefix/"+("ustar-segment-"*6)+"file.txt"
  contents_equal(os.path.join(clone, *prefix_rel.split("/")), "prefix\n", "USTAR prefix")
  check(os.path.islink(os.path.join(clone, "link")),
    "symlink extracted as a symlink")
  target = os.readlink(os.path.join(clone, "link"))
  check(target=="README.md", "symlink target preserved")
  check((os.stat(os.path.join(clone, "bin", "run.sh")).st_mode&0o111)!=0,
    "executable bit preserved")
  check(stat.S_IMODE(os.stat(clone).st_mode)==0o755, "repository root is 0755")
  check(not os.path.exists(os.path.join(clone, ".mesh", "store", "tmp", "obtain.tar.gz")),
    "archive removed from the store")
  reflog = run_mesh_ok("reflog", clone, env, "reflog")
  check("obtain http:" in reflog, "snapshot message names the source")
  empty = os.path.join(work, "empty-destination")
  os.makedirs(empty, exist_ok=True)
  status, _, err = run_obtain(work, url, "empty-destination")
  check(status==0, f"empty destination rejected: {err}")
  check(os.path.isdir(os.path.join(empty, ".mesh")), "empty destination published")
  explicit = os.path.join(work, "custom-name")
  status, _, err = run_obtain(
   work,
   "--archive",
   f"http://127.0.0.1:{safe_port}/source.tar.gz?token=test",
   url,
   explicit,
  )
  check(status==0, f"explicit archive failed: {err}")
  check(os.path.isdir(os.path.join(explicit, ".mesh")), "explicit directory published")
  ref_dest = os.path.join(work, "ref-clone")
  status, _, err = run_obtain(work, "--ref", "release/v1", url, ref_dest)
  check(status==0, f"ref clone failed: {err}")
  check(os.path.isdir(os.path.join(ref_dest, ".mesh")), "ref clone published")
  expected = read_bytes(archive)
  check(expected is not None, "cannot read built archive")
  assert expected is not None
  download = os.path.join(work, "download.bin")
  status, out, err = run_obtain(
   work, f"http://127.0.0.1:{safe_port}/source.tar.gz?token=test", download
  )
  check(status==0, f"file download failed: {err}")
  contents_equal(download, expected, "downloaded file")
  check(re.search(r"^obtained ", out, re.MULTILINE) is not None, "download output is unified")
  check(re.search(r"blake2b: [0-9a-f]{128}", out) is not None,
    "download reports a digest")
  resume = os.path.join(work, "resume.bin")
  write_bytes(resume, expected[:10])
  status, out, err = run_obtain(
   work, f"http://127.0.0.1:{safe_port}/source.tar.gz", resume
  )
  check(status==0, f"resumed download failed: {err}")
  contents_equal(resume, expected, "resumed download")
  check("resumed from: 10 bytes" in out, "download reports the resume offset")
  forced = os.path.join(work, "forced.bin")
  status, _, err = run_obtain(
   work, "--file", f"http://127.0.0.1:{safe_port}/owner/repo", "-o", forced
  )
  check(status==0, f"forced file mode failed: {err}")
  contents_equal(forced, expected, "forced download")
  redirect = os.path.join(work, "redirect.bin")
  status, _, _ = run_obtain(
   work, f"http://127.0.0.1:{safe_port}/redirect", "-o", redirect
  )
  check(status!=0, "redirects are disabled by default")
  check(not os.path.exists(redirect), "default redirect left no file")
  status, _, err = run_obtain(
   work, "-L", f"http://127.0.0.1:{safe_port}/redirect", "-o", redirect
  )
  check(status==0, f"redirect download failed: {err}")
  contents_equal(redirect, expected, "redirected download")
  output_dir = os.path.join(work, "downloads")
  status, _, err = run_obtain(
   work,
   "--create-dirs",
   "-O",
   "--output-dir",
   output_dir,
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
  )
  check(status==0, f"output-dir download failed: {err}")
  contents_equal(os.path.join(output_dir, "source.tar.gz"), expected,
      "output-dir download")
  nested = os.path.join(work, "created", "deep", "file.bin")
  status, _, err = run_obtain(
   work,
   "--create-dirs",
   "-o",
   nested,
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
  )
  check(status==0, f"create-dirs download failed: {err}")
  contents_equal(nested, expected, "create-dirs download")
  curl_options = os.path.join(work, "curl-options.bin")
  status, _, err = run_obtain(
   work,
   "-H",
   "X-Mesh-Test: yes",
   "-A",
   "mesh-curl-test",
   "-e",
   "https://example.test/page",
   "-o",
   curl_options,
   f"http://127.0.0.1:{safe_port}/curl-options",
  )
  check(status==0, f"custom header/agent/referer failed: {err}")
  contents_equal(curl_options, expected, "custom request metadata")
  compressed = os.path.join(work, "compressed.txt")
  status, _, err = run_obtain(
   work,
   "--compressed",
   "-o",
   compressed,
   f"http://127.0.0.1:{safe_port}/compressed.txt",
  )
  check(status==0, f"compressed download failed: {err}")
  contents_equal(compressed, "compressed download\n", "compressed download")
  raw_env = dict(os.environ)
  raw_env.update(env)
  stdout_url = f"http://127.0.0.1:{safe_port}/source.tar.gz"
  streamed = subprocess.run(
   [mesh_binary(), "obtain", "-o", "-", stdout_url],
   cwd=work,
   env=raw_env,
   stdout=subprocess.PIPE,
   stderr=subprocess.PIPE,
   check=False,
  )
  check(streamed.returncode==0, "stdout download failed")
  check(streamed.stdout==expected, "stdout download is byte exact")
  check(b"obtained stdout" in streamed.stderr,
    "stdout download reports metadata on stderr")
  check(b"blake2b:" in streamed.stderr,
    "stdout download reports a digest on stderr")
  silent_stream = subprocess.run(
   [mesh_binary(), "obtain", "-s", "-o", "-", stdout_url],
   cwd=work,
   env=raw_env,
   stdout=subprocess.PIPE,
   stderr=subprocess.PIPE,
   check=False,
  )
  check(silent_stream.returncode==0, "silent stdout download failed")
  check(silent_stream.stdout==expected, "silent stdout body is exact")
  check(silent_stream.stderr==b"", "silent stdout suppresses metadata")
  stream_write = subprocess.run(
   [mesh_binary(), "obtain", "-s", "-o", "-", "-w",
    "%{http_code}\\n", stdout_url],
   cwd=work,
   env=raw_env,
   stdout=subprocess.PIPE,
   stderr=subprocess.PIPE,
   check=False,
  )
  check(stream_write.returncode==0, "stdout write-out failed")
  check(stream_write.stdout==expected+b"200\n",
    "write-out follows a stdout body")
  writeout = os.path.join(work, "writeout.bin")
  status, out, err = run_obtain(
   work,
   "-w",
   "%{http_code} %{size_download} %{url_effective} %{filename_effective}\\n",
   "-o",
   writeout,
   stdout_url,
  )
  check(status==0, f"write-out download failed: {err}")
  fields = out.strip().split(" ")
  check(len(fields)==4, f"write-out fields missing: {out!r}")
  check(fields[0]=="200", f"write-out http_code: {out!r}")
  check(fields[1]==str(len(expected)), f"write-out size_download: {out!r}")
  check(fields[2]==stdout_url, f"write-out url_effective: {out!r}")
  check(fields[3]==writeout, f"write-out filename_effective: {out!r}")
  contents_equal(writeout, expected, "write-out download")
  status, out, err = run_obtain(
   work, "-s", "-w", "%{http_code}\\n", "-o", writeout, stdout_url
  )
  check(status==0, f"silent write-out failed: {err}")
  check(out=="200\n", f"silent write-out is still printed: {out!r}")
  check(err=="", f"silent write-out is quiet: {err!r}")
  missing_write = os.path.join(work, "writeout-missing.bin")
  status, out, _ = run_obtain(
   work,
   "-w",
   "%{http_code} %{exitcode}\\n",
   "-o",
   missing_write,
   f"http://127.0.0.1:{safe_port}/missing",
  )
  check(status!=0, "write-out error download unexpectedly succeeded")
  check(out=="404 1\n", f"write-out reports the failed response: {out!r}")
  check(not os.path.exists(missing_write), "failed write-out left no file")
  head_file = os.path.join(work, "headers.txt")
  status, out, err = run_obtain(
   work,
   "-I",
   "-o",
   head_file,
   "-w",
   "%{http_code}\\n",
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
  )
  check(status==0, f"HEAD download failed: {err}")
  check(out=="200\n", f"HEAD write-out: {out!r}")
  head_text = read_text(head_file)
  check(head_text is not None, "HEAD output file missing")
  assert head_text is not None
  check("HTTP/1.1 200" in head_text, "HEAD status line is printed")
  check("Content-Length:" in head_text, "HEAD content-length is printed")
  status, out, err = run_obtain(
   work, "-I", "-L", f"http://127.0.0.1:{safe_port}/redirect"
  )
  check(status==0, f"redirected HEAD failed: {err}")
  check("302" in out and "200" in out, "redirected HEAD prints both responses")
  status, out, _ = run_obtain(
   work, "-I", f"http://127.0.0.1:{safe_port}/missing"
  )
  check(status==0, "HEAD without --fail accepts an HTTP error")
  check("404" in out, "HEAD without --fail prints the error headers")
  status, out, _ = run_obtain(
   work, "-I", "-f", f"http://127.0.0.1:{safe_port}/missing"
  )
  check(status!=0, "HEAD with --fail rejects an HTTP error")
  check("404" in out, "HEAD --fail still prints the error headers")
  cookie_jar = os.path.join(work, "cookies.txt")
  cookie_seed = os.path.join(work, "cookie-seed.bin")
  status, _, err = run_obtain(
   work,
   "-c",
   cookie_jar,
   "-o",
   cookie_seed,
   f"http://127.0.0.1:{safe_port}/set-cookie",
  )
  check(status==0, f"cookie jar download failed: {err}")
  contents_equal(cookie_seed, expected, "cookie jar seed download")
  jar_text = read_text(cookie_jar)
  check(jar_text is not None and "session" in jar_text,
    "cookie jar was not written")
  cookie_download = os.path.join(work, "cookie-download.bin")
  status, _, err = run_obtain(
   work,
   "-b",
   cookie_jar,
   "-o",
   cookie_download,
   f"http://127.0.0.1:{safe_port}/cookie-required",
  )
  check(status==0, f"cookie jar replay failed: {err}")
  contents_equal(cookie_download, expected, "cookie jar replay")
  direct_cookie = os.path.join(work, "direct-cookie.bin")
  status, _, err = run_obtain(
   work,
   "-b",
   "session=ok",
   "-o",
   direct_cookie,
   f"http://127.0.0.1:{safe_port}/cookie-required",
  )
  check(status==0, f"direct cookie failed: {err}")
  contents_equal(direct_cookie, expected, "direct cookie")
  no_cookie = os.path.join(work, "no-cookie.bin")
  status, _, _ = run_obtain(
   work,
   "-o",
   no_cookie,
   f"http://127.0.0.1:{safe_port}/cookie-required",
  )
  check(status!=0, "missing cookie is rejected")
  check(not os.path.exists(no_cookie), "cookie rejection left no file")
  proxied = os.path.join(work, "proxied.bin")
  status, _, err = run_obtain(
   work,
   "-x",
   f"http://127.0.0.1:{safe_port}",
   "-o",
   proxied,
   "http://example.invalid/source.tar.gz",
  )
  check(status==0, f"proxy download failed: {err}")
  contents_equal(proxied, expected, "proxy download")
  insecure = os.path.join(work, "insecure.bin")
  status, _, err = run_obtain(work, "-k", "-o", insecure, stdout_url)
  check(status==0, f"insecure flag download failed: {err}")
  check("TLS certificate verification is disabled" in err,
    "insecure flag emits a warning")
  contents_equal(insecure, expected, "insecure flag download")
  insecure_repo = os.path.join(work, "insecure-repo")
  token_env = dict(env)
  token_env["MESH_TOKEN"] = "test-token"
  status, _, err = run_mesh(
   work,
   token_env,
   "obtain",
   "-k",
   "--repo",
   url,
   insecure_repo,
  )
  check(status!=0, "insecure repository import with credentials is rejected")
  check("refusing to disable TLS verification" in err,
    "insecure credential rejection is explicit")
  check(not os.path.exists(insecure_repo),
    "insecure credential rejection left no destination")
  clustered = os.path.join(work, "clustered.bin")
  status, out, err = run_obtain(
   work, "-sSL", "-o", clustered, f"http://127.0.0.1:{safe_port}/redirect"
  )
  check(status==0, f"clustered option download failed: {err}")
  check(out=="", "silent suppresses successful output")
  contents_equal(clustered, expected, "clustered option download")
  explicit_resume = os.path.join(work, "explicit-resume.bin")
  write_bytes(explicit_resume, expected[:10])
  status, out, err = run_obtain(
   work,
   "-C",
   "-",
   "-o",
   explicit_resume,
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
  )
  check(status==0, f"continue-at download failed: {err}")
  contents_equal(explicit_resume, expected, "continue-at download")
  check("resumed from: 10 bytes" in out, "-C - reports the resume offset")
  url_option = os.path.join(work, "url-option.bin")
  status, _, err = run_obtain(
   work,
   "--url",
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
   "-o",
   url_option,
  )
  check(status==0, f"--url download failed: {err}")
  contents_equal(url_option, expected, "--url download")
  numeric_resume = os.path.join(work, "numeric-resume.bin")
  write_bytes(numeric_resume, expected[:20])
  status, out, err = run_obtain(
   work,
   "-C",
   "10",
   "-o",
   numeric_resume,
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
  )
  check(status==0, f"numeric continue-at failed: {err}")
  contents_equal(numeric_resume, expected, "numeric continue-at")
  check("resumed from: 10 bytes" in out,
    "numeric -C reports the requested offset")
  short_resume = os.path.join(work, "short-resume.bin")
  write_bytes(short_resume, expected[:5])
  status, _, _ = run_obtain(
   work,
   "-C",
   "10",
   "-o",
   short_resume,
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
  )
  check(status!=0, "numeric -C beyond the local file is rejected")
  contents_equal(short_resume, expected[:5], "rejected numeric resume is untouched")
  restart_resume = os.path.join(work, "restart-resume.bin")
  write_bytes(restart_resume, expected)
  status, out, err = run_obtain(
   work,
   "-C",
   "0",
   "-o",
   restart_resume,
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
  )
  check(status==0, f"zero continue-at failed: {err}")
  contents_equal(restart_resume, expected, "zero continue-at")
  check("resumed from:" not in out, "zero continue-at is a clean restart")
  status, _, _ = run_obtain(
   work, "-O", "-o", "one.bin", f"http://127.0.0.1:{safe_port}/source.tar.gz"
  )
  check(status!=0, "--remote-name conflicts with --output")
  repo_redirect = os.path.join(work, "repo-redirect")
  status, _, err = run_obtain(
   work,
   "-L",
   "--archive",
   f"http://127.0.0.1:{safe_port}/redirect",
   url,
   repo_redirect,
  )
  check(status==0, f"repository redirect failed: {err}")
  check(os.path.isdir(os.path.join(repo_redirect, ".mesh")),
      "repository redirect published")
  cred_dest = os.path.join(work, "credential-redirect")
  cred_env = dict(env)
  cred_env["MESH_TOKEN"] = "test-token"
  status, _, _ = run_mesh(
   work,
   cred_env,
   "obtain",
   "-L",
   f"http://127.0.0.1:{safe_port}/owner/repo",
   cred_dest,
  )
  check(status!=0, "redirects with MESH_TOKEN are rejected")
  check(not os.path.exists(cred_dest), "credential redirect left no destination")
  auto_dir = os.path.join(work, "auto")
  os.makedirs(auto_dir, exist_ok=True)
  status, _, err = run_obtain(
   auto_dir, "--file", f"http://127.0.0.1:{safe_port}/owner/repo"
  )
  check(status==0, f"default download name failed: {err}")
  contents_equal(os.path.join(auto_dir, "repo"), expected, "default download name")
  forced_repo = os.path.join(work, "forced-repo")
  status, _, err = run_obtain(work, "--repo", url, forced_repo)
  check(status==0, f"forced repository mode failed: {err}")
  check(os.path.isdir(os.path.join(forced_repo, ".mesh")), "forced repository published")
  dl_repo = os.path.join(work, "download-repo")
  os.makedirs(dl_repo, exist_ok=True)
  run_mesh_ok("download repo start", dl_repo, env, "start")
  run_mesh_ok(
   "download repo obtain",
   dl_repo,
   env,
   "obtain",
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
   "-o",
   "saved.tar.gz",
  )
  monitor = read_text(os.path.join(dl_repo, ".mesh", "monitor", "status.json"))
  check(monitor is not None, "download did not write status")
  assert monitor is not None
  try:
   status_json = json.loads(monitor)
  except json.JSONDecodeError as exc:
   fail(f"download status is not valid JSON: {exc}")
  check(status_json.get("last_operation")=="obtain",
    "download records the obtain operation")
  check(status_json.get("last_path")=="saved.tar.gz",
    "download records the destination")
  alias = os.path.join(work, "alias.bin")
  run_mesh_ok(
   "fetch alias",
   work,
   env,
   "fetch",
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
   "-o",
   alias,
  )
  contents_equal(alias, expected, "fetch alias")
  fetch_help = run_mesh_ok("fetch help", work, env, "fetch", "--help")
  check("usage: mesh fetch" in fetch_help, "fetch help uses the wrong command name")
  check(
   "Deprecated alias for 'mesh obtain --file'." in fetch_help,
   "fetch help does not explain the alias",
  )
  check("Repository mode" not in fetch_help, "fetch help advertises repository mode")
  status, _, _ = run_obtain(
   work, "--file", "--ref", "main", f"http://127.0.0.1:{safe_port}/source.tar.gz"
  )
  check(status!=0, "file mode rejects --ref")
  status, _, _ = run_obtain(work, "--repo", "-o", "out.bin", url)
  check(status!=0, "repository mode rejects --output")
  status, _, _ = run_obtain(
   work,
   "-o",
   "one.bin",
   f"http://127.0.0.1:{safe_port}/source.tar.gz",
   "two.bin",
  )
  check(status!=0, "download rejects --output and DEST together")
  existing = os.path.join(work, "existing")
  os.makedirs(existing, exist_ok=True)
  write_text(os.path.join(existing, "keep.txt"), "keep\n")
  status, _, _ = run_obtain(work, url, "existing")
  check(status!=0, "non-empty destination rejected")
  contents_equal(os.path.join(existing, "keep.txt"), "keep\n", "existing file kept")
  traversal = os.path.join(temp, "traversal.tar.gz")
  build_archive(traversal, "traversal")
  traversal_port = start_server(temp, traversal, "safe", servers)
  traversal_dest = os.path.join(work, "traversal")
  status, _, _ = run_obtain(
   work, f"http://127.0.0.1:{traversal_port}/owner/repo", traversal_dest
  )
  check(status!=0, "path traversal rejected")
  check(not os.path.exists(traversal_dest), "traversal destination not published")
  check(not os.path.exists(os.path.join(work, "mesh-escape.txt")),
    "traversal wrote outside")
  reserved = os.path.join(temp, "reserved.tar.gz")
  build_archive(reserved, "reserved")
  reserved_port = start_server(temp, reserved, "safe", servers)
  reserved_dest = os.path.join(work, "reserved")
  status, _, _ = run_obtain(
   work, f"http://127.0.0.1:{reserved_port}/owner/repo", reserved_dest
  )
  check(status!=0, "reserved .mesh entry rejected")
  check(not os.path.exists(reserved_dest), "reserved destination not published")
  symlink = os.path.join(temp, "symlink.tar.gz")
  build_archive(symlink, "symlink")
  symlink_port = start_server(temp, symlink, "safe", servers)
  symlink_dest = os.path.join(work, "symlink")
  status, _, _ = run_obtain(
   work, f"http://127.0.0.1:{symlink_port}/owner/repo", symlink_dest
  )
  check(status!=0, "symlink escape rejected")
  check(not os.path.exists(symlink_dest), "symlink destination not published")
  check(not os.path.exists(os.path.join(work, "outside")), "symlink escape wrote outside")
  pax_overflow = os.path.join(temp, "pax-overflow.tar.gz")
  build_archive(pax_overflow, "pax-overflow")
  pax_overflow_port = start_server(temp, pax_overflow, "safe", servers)
  pax_overflow_dest = os.path.join(work, "pax-overflow")
  status, _, _ = run_obtain(
   work, f"http://127.0.0.1:{pax_overflow_port}/owner/repo", pax_overflow_dest
  )
  check(status!=0, "overflowing PAX size rejected")
  check(not os.path.exists(pax_overflow_dest), "PAX overflow destination not published")
  corrupt = os.path.join(temp, "corrupt.tar.gz")
  write_text(corrupt, "not a tar archive\n")
  corrupt_port = start_server(temp, corrupt, "corrupt", servers)
  corrupt_dest = os.path.join(work, "corrupt")
  status, _, _ = run_obtain(
   work, f"http://127.0.0.1:{corrupt_port}/owner/repo", corrupt_dest
  )
  check(status!=0, "corrupt archive rejected")
  check(not os.path.exists(corrupt_dest), "corrupt destination not published")
  status, _, _ = run_obtain(work, "ftp://example.com/owner/repo")
  check(status!=0, "non-HTTP source rejected")
  status, _, _ = run_obtain(work, url, ".")
  check(status!=0, "current directory destination rejected")
  bad_env = dict(env)
  bad_env["MESH_TOKEN"] = "bad token"
  status, _, _ = run_mesh(
   work, bad_env, "obtain", url, os.path.join(work, "bad-token")
  )
  check(status!=0, "invalid token rejected")
  check(not os.path.exists(os.path.join(work, "bad-token")),
    "invalid token destination not published")
 finally:
  stop_servers(servers)
 print("obtain tests: ok")
 return 0
if __name__=="__main__":
 sys.exit(main())
