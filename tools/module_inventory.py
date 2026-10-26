#!/usr/bin/env python3
"""Inventory the admin C modules declared in tools/modules.json."""
from __future__ import annotations
import argparse
import json
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
METADATA = ROOT / "tools" / "modules.json"
def load_modules() -> list[dict[str, object]]:
 data = json.loads(METADATA.read_text(encoding="utf-8"))
 modules = data.get("modules")
 if not isinstance(modules, list):
  raise SystemExit("modules.json: missing modules array")
 return modules
def rows() -> list[dict[str, object]]:
 result: list[dict[str, object]] = []
 for module in load_modules():
  path = ROOT / str(module["file"])
  if path.exists():
   text = path.read_text(encoding="utf-8")
   lines = len(text.splitlines())
   longest = max((len(line) for line in text.splitlines()), default=0)
  else:
   lines = 0
   longest = 0
  result.append({
   "name": module["name"],
   "file": module["file"],
   "command": module["command"],
   "summary": module["summary"],
   "lines": lines,
   "max_line": longest,
   "exists": path.exists(),
  })
 return result
def main() -> int:
 parser = argparse.ArgumentParser(description="List admin C modules.")
 parser.add_argument("--json", action="store_true", help="emit JSON")
 args = parser.parse_args()
 items = rows()
 if args.json:
  print(json.dumps({"modules": items}, indent=2, sort_keys=True))
  return 0
 print(f"{'module':18} {'lines':>6} {'max':>4}  file")
 for item in items:
  mark = "" if item["exists"] else " MISSING"
  print(f"{item['name']:18} {item['lines']:6} {item['max_line']:4}  "
        f"{item['file']}{mark}")
 return 0 if all(item["exists"] for item in items) else 1
if __name__ == "__main__":
 raise SystemExit(main())
