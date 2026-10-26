#!/usr/bin/env python3
"""Render the dependency graph of the admin C modules."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
METADATA = ROOT / "tools" / "modules.json"
def load_modules() -> list[dict[str, object]]:
 data = json.loads(METADATA.read_text(encoding="utf-8"))
 modules = data.get("modules")
 if not isinstance(modules, list):
  raise SystemExit("modules.json: missing modules array")
 return modules
def edges(modules: list[dict[str, object]]) -> list[tuple[str, str]]:
 names = {str(module["name"]) for module in modules}
 result: list[tuple[str, str]] = []
 for module in modules:
  source = str(module["name"])
  dependencies = module.get("depends_on", [])
  if not isinstance(dependencies, list):
   raise SystemExit(f"modules.json: {source} depends_on is not a list")
  for target in dependencies:
   if target not in names:
    raise SystemExit(f"modules.json: {source} depends on unknown {target}")
   result.append((source, str(target)))
 return result
def main() -> int:
 parser = argparse.ArgumentParser(description="Render the admin module graph.")
 parser.add_argument("--json", action="store_true", help="emit edge JSON")
 args = parser.parse_args()
 modules = load_modules()
 links = edges(modules)
 if args.json:
  print(json.dumps({"nodes": [module["name"] for module in modules],
                    "edges": links}, indent=2, sort_keys=True))
  return 0
 print("digraph mesh_admin {")
 print("  rankdir=LR;")
 for module in modules:
  print(f'  "{module["name"]}" [label="{module["name"]}"];')
 for source, target in links:
  print(f'  "{source}" -> "{target}";')
 print("}")
 return 0
if __name__ == "__main__":
 raise SystemExit(main())
