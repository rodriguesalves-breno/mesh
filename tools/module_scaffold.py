#!/usr/bin/env python3
"""Generate a new admin C module skeleton in Alves style."""
from __future__ import annotations
import argparse
import re
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
NAME = re.compile(r"^[a-z][a-z0-9_]*$")
def normalize(name: str) -> str:
 clean = name.strip().lower().replace("-", "_")
 if not clean.startswith("admin_"):
  clean = "admin_" + clean
 if not NAME.fullmatch(clean):
  raise SystemExit(f"invalid module name: {name}")
 return clean
def clean_summary(summary: str) -> str:
 summary = " ".join(summary.split())
 return summary.replace("*/", "* /")
def template(name: str, summary: str) -> str:
 return (
  "/*\n"
  f"* {name}.c - {summary}.\n"
  "* Keep the command read-only until its state transitions are auditable.\n"
  "*/\n"
  '#include "core.h"\n'
  f"int {name}_run(const struct mesh_repo*repo) {{\n"
  "(void)repo;\n"
  "return 0;\n"
  "}\n"
 )
def main() -> int:
 parser = argparse.ArgumentParser(description="Create an admin C module.")
 parser.add_argument("name", help="module name, with or without admin_")
 parser.add_argument("--summary", default="administrative extension",
                     help="one-line responsibility")
 parser.add_argument("--write", action="store_true", help="write the file")
 parser.add_argument("--force", action="store_true", help="overwrite")
 args = parser.parse_args()
 name = normalize(args.name)
 path = ROOT / "src" / f"{name}.c"
 body = template(name, clean_summary(args.summary).rstrip("."))
 if path.exists() and not args.force:
  print(f"refusing to overwrite {path} (use --force)", file=sys.stderr)
  return 1
 if args.write:
  path.write_text(body, encoding="utf-8")
  print(f"wrote {path}")
  print("next: add the prototype, Makefile entry and modules.json record")
  return 0
 print(body, end="")
 return 0
if __name__ == "__main__":
 raise SystemExit(main())
