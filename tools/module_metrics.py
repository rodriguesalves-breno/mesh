#!/usr/bin/env python3
"""Measure size and shape of the admin C modules."""
from __future__ import annotations
import argparse
import json
import re
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
METADATA = ROOT / "tools" / "modules.json"
FUNCTION = re.compile(
 r"(?m)^(?:static\s+)?(?:int|void|bool|char|size_t|struct\s+\w+|"
 r"const\s+char\*)\s+([a-z_][a-z0-9_]*)\s*\([^;{}]*\)\s*\{")
def load_modules() -> list[dict[str, object]]:
 data = json.loads(METADATA.read_text(encoding="utf-8"))
 modules = data.get("modules")
 if not isinstance(modules, list):
  raise SystemExit("modules.json: missing modules array")
 return modules
def measure(module: dict[str, object]) -> dict[str, object]:
 path = ROOT / str(module["file"])
 text = path.read_text(encoding="utf-8") if path.exists() else ""
 lines = text.splitlines()
 code = [line for line in lines if line.strip()]
 comments = [line for line in lines if line.lstrip().startswith("*")]
 return {
  "name": module["name"],
  "file": module["file"],
  "lines": len(lines),
  "code_lines": len(code),
  "comment_lines": len(comments),
  "functions": len(FUNCTION.findall(text)),
  "max_line": max((len(line) for line in lines), default=0),
 }
def main() -> int:
 parser = argparse.ArgumentParser(description="Measure admin modules.")
 parser.add_argument("--json", action="store_true", help="emit JSON")
 args = parser.parse_args()
 items = [measure(module) for module in load_modules()]
 if args.json:
  print(json.dumps({"modules": items}, indent=2, sort_keys=True))
  return 0
 print(f"{'module':18} {'lines':>6} {'code':>6} {'func':>5} {'max':>4}")
 for item in items:
  print(f"{item['name']:18} {item['lines']:6} {item['code_lines']:6} "
        f"{item['functions']:5} {item['max_line']:4}")
 print(f"{'total':18} {sum(int(item['lines']) for item in items):6} "
       f"{sum(int(item['code_lines']) for item in items):6} "
       f"{sum(int(item['functions']) for item in items):5}")
 return 0
if __name__ == "__main__":
 raise SystemExit(main())
