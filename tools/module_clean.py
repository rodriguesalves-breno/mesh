#!/usr/bin/env python3
"""Remove generated artifacts for the admin C modules and tools."""
from __future__ import annotations
import argparse
import shutil
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
def artifacts() -> list[Path]:
 items: list[Path] = []
 items.extend((ROOT / "src").glob("admin*.o"))
 items.extend((ROOT / "src").glob("admin*.d"))
 cache = ROOT / "tools" / "__pycache__"
 if cache.exists():
  items.append(cache)
 return sorted(set(items))
def main() -> int:
 parser = argparse.ArgumentParser(description="Clean admin module artifacts.")
 parser.add_argument("--apply", action="store_true", help="delete the files")
 args = parser.parse_args()
 items = artifacts()
 if not items:
  print("admin artifacts: clean")
  return 0
 for item in items:
  if args.apply:
   if item.is_dir():
    shutil.rmtree(item)
   else:
    item.unlink()
   print(f"removed {item}")
  else:
   print(f"would remove {item}")
 if not args.apply:
  print("dry run; use --apply to remove")
 return 0
if __name__ == "__main__":
 raise SystemExit(main())
