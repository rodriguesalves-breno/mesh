"""Shared helpers for the Python integration tests.
The C unit test stays self-contained in tests/unit.c.  This module gives the
Python side a common base: a temporary workspace, deterministic subprocess
execution, binary and text file access and a small set of assertions.  Only the
Python standard library is required.
"""
from __future__ import annotations
import atexit
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
def project_root() -> str:
 return str(Path(__file__).resolve().parent.parent)
def mesh_binary() -> str:
 root = project_root()
 binary = Path(os.environ.get("BIN") or os.path.join(root, "mesh"))
 if binary.exists():
  binary = binary.resolve()
 check(os.access(binary, os.X_OK), f"mesh binary not built: {binary}")
 return str(binary)
def workspace(name: str) -> str:
 path = tempfile.mkdtemp(prefix=f"{name}-", dir=tempfile.gettempdir())
 atexit.register(shutil.rmtree, path, True)
 return path
def fail(message: str) -> None:
 print(f"FAIL: {message}", file=sys.stderr)
 raise SystemExit(1)
def check(condition: object, message: str) -> None:
 if not condition:
  fail(message)
def read_bytes(path: str) -> bytes|None:
 try:
  return Path(path).read_bytes()
 except OSError:
  return None
def read_text(path: str) -> str|None:
 try:
  return Path(path).read_text(encoding="utf-8", errors="surrogateescape")
 except OSError:
  return None
def write_bytes(path: str, data: bytes|str) -> None:
 payload = data.encode("utf-8") if isinstance(data, str) else data
 try:
  Path(path).write_bytes(payload)
 except OSError as exc:
  fail(f"cannot write {path}: {exc}")
def write_text(path: str, data: str) -> None:
 write_bytes(path, data)
def run_command(
 directory: str,
 environment: dict[str, str]|None,
 command: str,
 *arguments: str,
) -> tuple[int, str, str]:
 env = os.environ.copy()
 if environment:
  env.update({str(key): str(value) for key, value in environment.items()})
 completed = subprocess.run(
  [command, *arguments],
  cwd=directory,
  env=env,
  stdout=subprocess.PIPE,
  stderr=subprocess.PIPE,
  check=False,
 )
 return (
  completed.returncode,
  completed.stdout.decode("utf-8", "replace"),
  completed.stderr.decode("utf-8", "replace"),
 )
def run_mesh(
 directory: str,
 environment: dict[str, str]|None,
 *arguments: str,
) -> tuple[int, str, str]:
 return run_command(directory, environment, mesh_binary(), *arguments)
def run_mesh_ok(
 label: str,
 directory: str,
 environment: dict[str, str]|None,
 *arguments: str,
) -> str:
 status, stdout, stderr = run_mesh(directory, environment, *arguments)
 if status!=0:
  fail(f"{label}: exit {status}: {stderr}")
 return stdout
def run_mesh_fail(
 label: str,
 directory: str,
 environment: dict[str, str]|None,
 *arguments: str,
) -> tuple[str, str]:
 status, stdout, stderr = run_mesh(directory, environment, *arguments)
 if status==0:
  fail(f"{label}: command unexpectedly succeeded")
 return stdout, stderr
def find_command(name: str) -> str|None:
 return shutil.which(name)
def blake2b_hex(data: bytes|str) -> str:
 payload = data.encode("utf-8") if isinstance(data, str) else data
 return hashlib.blake2b(payload, digest_size=64).hexdigest()
