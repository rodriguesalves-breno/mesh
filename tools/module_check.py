#!/usr/bin/env python3
"""Check the admin C modules against the Alves layout contract."""
from __future__ import annotations
import json
import re
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
METADATA = ROOT / "tools" / "modules.json"
BANNED = re.compile(r"\b(?:strcpy|strcat|sprintf|gets)\s*\(")
def load_modules() -> list[dict[str, object]]:
 data = json.loads(METADATA.read_text(encoding="utf-8"))
 modules = data.get("modules")
 if not isinstance(modules, list):
  raise SystemExit("modules.json: missing modules array")
 return modules
def strip_c(text: str) -> str:
 out = list(text)
 state = "normal"
 i = 0
 while i < len(text):
  c = text[i]
  if state == "normal":
   if c == "/" and i + 1 < len(text) and text[i + 1] == "/":
    out[i] = " "
    out[i + 1] = " "
    state = "line"
    i += 2
    continue
   if c == "/" and i + 1 < len(text) and text[i + 1] == "*":
    out[i] = " "
    out[i + 1] = " "
    state = "block"
    i += 2
    continue
   if c == '"':
    out[i] = " "
    state = "string"
   elif c == "'":
    out[i] = " "
    state = "char"
   i += 1
   continue
  if state == "line":
   if c == "\n":
    state = "normal"
   else:
    out[i] = " "
   i += 1
   continue
  if state == "block":
   if c == "*" and i + 1 < len(text) and text[i + 1] == "/":
    out[i] = " "
    out[i + 1] = " "
    state = "normal"
    i += 2
    continue
   if c != "\n":
    out[i] = " "
   i += 1
   continue
  if c == "\\" and i + 1 < len(text):
   out[i] = " "
   out[i + 1] = " "
   i += 2
   continue
  if (state == "string" and c == '"') or (state == "char" and c == "'"):
   state = "normal"
  if c != "\n":
   out[i] = " "
  i += 1
 return "".join(out)
def brace_depth(text: str) -> int:
 depth = 0
 maximum = 0
 for c in strip_c(text):
  if c == "{":
   depth += 1
   maximum = max(maximum, depth)
  elif c == "}":
   depth -= 1
 return maximum
def check_file(module: dict[str, object], errors: list[str]) -> None:
 path = ROOT / str(module["file"])
 if not path.exists():
  errors.append(f"{path}: missing")
  return
 text = path.read_text(encoding="utf-8")
 lines = text.splitlines()
 for number, line in enumerate(lines, 1):
  label = f"{path}:{number}"
  if "\t" in line:
   errors.append(f"{label}: tab")
  if line.rstrip() != line:
   errors.append(f"{label}: trailing whitespace")
  if not line.strip():
   errors.append(f"{label}: blank line")
  if line[:1].isspace():
   errors.append(f"{label}: left margin")
  if len(line) > 100:
   errors.append(f"{label}: {len(line)} columns")
  if BANNED.search(line):
   errors.append(f"{label}: banned string function")
 maximum = brace_depth(text)
 if maximum > 4:
  errors.append(f"{path}: brace depth {maximum}, control nesting is too deep")
def check_registry(modules: list[dict[str, object]], errors: list[str]) -> None:
 if len(modules) != 13:
  errors.append(f"modules.json: expected 13 modules, got {len(modules)}")
 names = [str(module["name"]) for module in modules]
 if len(set(names)) != len(names):
  errors.append("modules.json: duplicate module names")
 expected = {Path(str(module["file"])).name for module in modules}
 actual = {path.name for path in (ROOT / "src").glob("admin*.c")}
 if expected != actual:
  errors.append(f"src: admin C files differ: {sorted(actual ^ expected)}")
 makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
 for module in modules:
  if str(module["file"]) not in makefile:
   errors.append(f"Makefile: missing {module['file']}")
def main() -> int:
 errors: list[str] = []
 modules = load_modules()
 check_registry(modules, errors)
 for module in modules:
  check_file(module, errors)
 if errors:
  for error in errors:
   print(f"FAIL {error}", file=sys.stderr)
  print(f"module check: {len(errors)} failure(s)", file=sys.stderr)
  return 1
 print(f"module check: {len(modules)} modules passed")
 return 0
if __name__ == "__main__":
 raise SystemExit(main())
