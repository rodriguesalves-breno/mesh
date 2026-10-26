#!/usr/bin/env python3
"""Restart a Mesh history at the current tree while preserving a full backup.

This is intentionally explicit and non-destructive by default. It does not
rewrite old snapshot hashes in place: the active repository gets one new root
snapshot, while the previous .mesh directory is copied to a caller-selected
backup path before an atomic metadata swap.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def invoke(mesh: str, args: list[str], cwd: Path) -> str:
    result = subprocess.run([mesh, *args], cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"mesh {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def find_root(start: Path) -> Path:
    for candidate in (start, *start.parents):
        if (candidate / ".mesh" / "config").is_file():
            return candidate
    raise ValueError(f"no Mesh repository above {start}")


def tree_fingerprint(root: Path) -> str:
    digest = hashlib.blake2b(digest_size=64)
    paths: list[Path] = []
    for current, dirs, files in os.walk(root, topdown=True, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d not in {".mesh", ".git"})
        base = Path(current)
        paths.extend(base / name for name in dirs + sorted(files)
                     if name not in {".mesh", ".git"})
    for path in sorted(paths, key=lambda p: p.relative_to(root).as_posix().encode("utf-8", "surrogateescape")):
        rel = path.relative_to(root).as_posix().encode("utf-8", "surrogateescape")
        st = path.lstat()
        digest.update(len(rel).to_bytes(8, "big")); digest.update(rel)
        if path.is_symlink():
            target = os.readlink(path).encode("utf-8", "surrogateescape")
            digest.update(b"L"); digest.update(len(target).to_bytes(8, "big")); digest.update(target)
        elif path.is_dir():
            digest.update(b"D")
        elif path.is_file():
            digest.update(b"F"); digest.update((st.st_mode & 0o777).to_bytes(4, "big"))
            with path.open("rb") as stream:
                while chunk := stream.read(1024 * 1024):
                    digest.update(chunk)
        else:
            raise ValueError(f"unsupported filesystem object: {rel!r}")
    return digest.hexdigest()


def restart(root: Path, backup: Path, mesh: str, apply: bool) -> Path:
    root = find_root(root.resolve())
    backup = backup.resolve()
    old_meta = root / ".mesh"
    if backup.exists():
        raise FileExistsError(f"backup already exists: {backup}")
    if backup == root or root in backup.parents:
        raise ValueError("backup must be outside the repository")
    if backup in root.parents:
        raise ValueError("backup must not be an ancestor of the repository")
    print(f"Repository: {root}")
    print(f"Backup:     {backup}")
    print("Plan: create a verified one-root-snapshot repository from the current tree.")
    print("Old snapshot IDs, branches and tags remain only in the backup.")
    if not apply:
        print("Dry run only. Re-run with --apply to perform the rewrite.")
        return backup

    lock_path = old_meta / "lock"
    lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o644)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        invoke(mesh, ["admin", "verify"], root)
        before = tree_fingerprint(root)
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(old_meta, backup, symlinks=True)
        if tree_fingerprint(root) != before:
            shutil.rmtree(backup)
            raise RuntimeError("working tree changed during backup; no rewrite was made")

        with tempfile.TemporaryDirectory(prefix=".mesh-history-restart-", dir=root.parent) as tmp:
            stage = Path(tmp)
            work = stage / "work"
            shutil.copytree(root, work, symlinks=True,
                            ignore=shutil.ignore_patterns(".mesh", ".git"))
            invoke(mesh, ["start"], work)
            invoke(mesh, ["save", "History restart: preserve current tree"], work)
            invoke(mesh, ["admin", "verify"], work)
            if tree_fingerprint(root) != before:
                raise RuntimeError("working tree changed during preparation; no rewrite was made")

            old_staged = stage / "old-mesh"
            os.rename(old_meta, old_staged)
            try:
                os.rename(work / ".mesh", old_meta)
            except BaseException:
                os.rename(old_staged, old_meta)
                raise
        print(f"History restarted. Full previous .mesh backup: {backup}")
        return backup
    except BaseException:
        if backup.exists() and not (root / ".mesh").is_dir():
            # Never remove the only remaining copy if a swap failed.
            pass
        raise
    finally:
        os.close(lock_fd)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="path inside the Mesh repository")
    parser.add_argument("--backup", type=Path, required=True, help="new path for a complete .mesh backup")
    parser.add_argument("--mesh-bin", default=os.environ.get("MESH_BIN", "mesh"))
    parser.add_argument("--apply", action="store_true", help="perform the history rewrite; default is dry-run")
    args = parser.parse_args(argv)
    try:
        restart(args.root, args.backup, args.mesh_bin, args.apply)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        print(f"history restart: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
