#!/usr/bin/env python3
"""Smoke-test the admin C modules through the mesh binary."""
from __future__ import annotations
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
def binary() -> Path:
 value = os.environ.get("BIN")
 path = Path(value) if value else ROOT / "mesh"
 if not path.exists():
  subprocess.run(["make", "-s", "mesh"], cwd=ROOT, check=True)
  path = ROOT / "mesh"
 return path.resolve()
def run(executable: Path, args: list[str], cwd: Path,
        env: dict[str, str]) -> subprocess.CompletedProcess[str]:
 result = subprocess.run([str(executable), *args], cwd=str(cwd), env=env,
                         text=True, capture_output=True)
 if result.returncode != 0:
  raise RuntimeError(f"{' '.join(args)} failed: {result.stderr.strip()}")
 return result
def expect(condition: bool, message: str) -> None:
 if not condition:
  raise RuntimeError(message)
def main() -> int:
 executable = binary()
 root = Path(tempfile.mkdtemp(prefix="mesh-admin-tools-"))
 home = root / "home"
 (home / ".config").mkdir(parents=True)
 env = os.environ.copy()
 env["HOME"] = str(home)
 env["XDG_CONFIG_HOME"] = str(home / ".config")
 env["BIN"] = str(executable)
 try:
  run(executable, ["start"], root, env)
  (root / "hello.txt").write_text("admin smoke\n", encoding="utf-8")
  run(executable, ["save", "admin smoke"], root, env)
  modules = run(executable, ["admin", "modules"], root, env).stdout
  expect("admin_support" in modules, "modules output is incomplete")
  run(executable, ["admin", "doctor"], root, env)
  verify = run(executable, ["admin", "verify"], root, env).stdout
  expect("failures: 0" in verify, "verify reported a failure")
  inventory = json.loads(
   run(executable, ["admin", "inventory", "--json"], root, env).stdout)
  expect(int(inventory["snapshots"]) >= 1, "inventory has no snapshots")
  run(executable, ["admin", "refs"], root, env)
  run(executable, ["admin", "status"], root, env)
  run(executable, ["admin", "audit", "tools smoke"], root, env)
  audit = run(executable, ["admin", "audit"], root, env).stdout
  expect("tools smoke" in audit, "audit entry was not recorded")
  run(executable, ["admin", "quota", "1048576"], root, env)
  run(executable, ["admin", "quota"], root, env)
  run(executable, ["admin", "config"], root, env)
  run(executable, ["admin", "history"], root, env)
  exported = json.loads(
   run(executable, ["admin", "export", "-"], root, env).stdout)
  expect(exported["name"] == "Mesh", "export has no Mesh marker")
  run(executable, ["admin", "gc"], root, env)
  fake_dir = root / ".mesh" / "store" / "objects" / "aa"
  fake_dir.mkdir(parents=True, exist_ok=True)
  fake = fake_dir / ("a" * 126)
  fake.write_bytes(b"unreachable\n")
  run(executable, ["admin", "gc", "--apply"], root, env)
  expect(not fake.exists(), "gc did not remove the unreachable object")
  shard = root / ".mesh" / "store" / "objects" / "aa"
  outside = root / "outside-shard"
  outside.mkdir()
  external = outside / ("a" * 126)
  external.write_bytes(b"must survive\n")
  shard.rmdir()
  shard.symlink_to(outside, target_is_directory=True)
  gc_escape = subprocess.run(
   [str(executable), "admin", "gc", "--apply"], cwd=str(root), env=env,
   text=True, capture_output=True)
  expect(gc_escape.returncode != 0, "gc followed a symlinked object shard")
  expect(external.exists(), "gc removed an object outside the repository")
  scaffold = subprocess.run(
   [sys.executable, str(ROOT / "tools" / "module_scaffold.py"),
    "scaffold_regression", "--summary", "safe */\n#include <stdlib.h>"],
   cwd=ROOT, text=True, capture_output=True, check=True)
  expect("safe * / #include <stdlib.h>" in scaffold.stdout,
         "scaffold summary was not neutralized")
  expect("\n#include <stdlib.h>\n" not in scaffold.stdout,
         "scaffold summary injected a C preprocessor directive")
  print("admin module tests: ok")
  return 0
 finally:
  shutil.rmtree(root, ignore_errors=True)
if __name__ == "__main__":
 try:
  raise SystemExit(main())
 except RuntimeError as error:
  print(f"FAIL: {error}", file=sys.stderr)
  raise SystemExit(1)
