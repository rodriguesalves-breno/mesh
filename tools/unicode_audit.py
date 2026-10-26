#!/usr/bin/env python3
"""Audit repository names for Unicode-normalization/case-fold collisions.

The C core remains authoritative when saving/loading manifests. This module is
an operator-facing, read-only preflight tool for finding names that would be
rejected by that policy before moving a tree between filesystems.
"""
from __future__ import annotations

import argparse
import os
import sys
import unicodedata
from collections import defaultdict
from pathlib import Path
from typing import Iterable


def collision_key(path: str) -> str:
    """Return Mesh's conservative NFC + default case-fold comparison key."""
    path.encode("utf-8", "strict")
    if "\x00" in path:
        raise ValueError("NUL is not a valid path character")
    return unicodedata.normalize("NFC", unicodedata.normalize("NFC", path).casefold())


def find_collisions(paths: Iterable[str]) -> list[tuple[str, ...]]:
    """Return groups of distinct spellings with the same comparison key."""
    groups: dict[str, set[str]] = defaultdict(set)
    for path in paths:
        groups[collision_key(path)].add(path)
    return sorted(
        (tuple(sorted(names)) for names in groups.values() if len(names) > 1),
        key=lambda group: group[0],
    )


def repository_paths(root: Path) -> Iterable[str]:
    """Yield relative paths without following symlinked directories."""
    for current, dirs, files in os.walk(root, topdown=True, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d not in {".mesh", ".git"})
        base = Path(current)
        names = sorted(dirs + files)
        for name in names:
            path = base / name
            yield path.relative_to(root).as_posix()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="repository or directory tree to audit")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f"not a directory: {root}")
    try:
        collisions = find_collisions(repository_paths(root))
    except (UnicodeError, ValueError) as exc:
        print(f"unicode audit: invalid path encoding: {exc}", file=sys.stderr)
        return 2
    if collisions:
        for group in collisions:
            print("collision: " + " <=> ".join(group))
        print(f"{len(collisions)} Unicode path collision group(s) found", file=sys.stderr)
        return 1
    print(f"Unicode path audit passed: {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
