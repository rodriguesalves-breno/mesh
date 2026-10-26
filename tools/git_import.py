#!/usr/bin/env python3
"""Import a Git ref's first-parent tree history into a fresh Mesh repository.

This deliberately uses Git plumbing and the Mesh CLI rather than editing
private .mesh files. The conversion preserves the tree sequence and commit
subjects in Mesh snapshots; Git OIDs, merge parents, refs, signatures,
committer metadata and exact commit timestamps are not representable yet.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))
from unicode_audit import find_collisions


def run(argv: list[str], *, cwd: Path | None = None, text: bool = True) -> str:
    result = subprocess.run(
        argv, cwd=cwd, check=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=text,
    )
    if not text:
        return result.stdout  # type: ignore[return-value]
    return result.stdout.strip()


def safe_parts(name: str) -> tuple[str, ...]:
    path = PurePosixPath(name)
    parts = path.parts
    if not parts or path.is_absolute() or any(p in {"", ".", ".."} for p in parts):
        raise ValueError(f"unsafe Git path: {name!r}")
    if any(p.casefold() in {".mesh", ".git"} for p in parts):
        raise ValueError(f"reserved metadata path in Git tree: {name!r}")
    return parts


def extract_git_tree(source: Path, commit: str, destination: Path) -> None:
    """Extract git archive with explicit path/type validation (no extractall)."""
    destination.mkdir()
    with tempfile.TemporaryFile() as archive:
        subprocess.run(
            ["git", "-C", str(source), "archive", "--format=tar", commit],
            check=True, stdout=archive, stderr=subprocess.PIPE,
        )
        archive.seek(0)
        with tarfile.open(fileobj=archive, mode="r:") as tar:
            members = tar.getmembers()
            names: list[str] = []
            validated: list[tuple[tarfile.TarInfo, tuple[str, ...]]] = []
            for member in members:
                parts = safe_parts(member.name.rstrip("/"))
                if not (member.isdir() or member.isfile() or member.issym()):
                    raise ValueError(f"unsupported Git archive entry type: {member.name!r}")
                names.append("/".join(parts))
                validated.append((member, parts))
            collisions = find_collisions(names)
            if collisions:
                pretty = " <=> ".join(collisions[0])
                raise ValueError(f"Unicode-equivalent Git paths collide: {pretty}")
            for member, parts in validated:
                target = destination.joinpath(*parts)
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.issym():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    os.symlink(member.linkname, target)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source_file = tar.extractfile(member)
                    if source_file is None:
                        raise ValueError(f"missing content for {member.name!r}")
                    with source_file, target.open("xb") as output:
                        shutil.copyfileobj(source_file, output, length=1024 * 1024)
                    target.chmod(0o755 if member.mode & 0o111 else 0o644)


def replace_worktree(work: Path, tree: Path) -> None:
    for child in work.iterdir():
        if child.name == ".mesh":
            continue
        if child.is_dir() and not child.is_symlink():
            shutil.rmtree(child)
        else:
            child.unlink()
    for child in tree.iterdir():
        target = work / child.name
        if child.is_dir() and not child.is_symlink():
            shutil.copytree(child, target, symlinks=True)
        elif child.is_symlink():
            os.symlink(os.readlink(child), target)
        else:
            shutil.copy2(child, target, follow_symlinks=False)


def import_history(source: Path, destination: Path, mesh_bin: str, ref: str) -> int:
    source = source.resolve()
    destination = destination.resolve()
    if run(["git", "-C", str(source), "rev-parse", "--is-inside-work-tree"]) != "true":
        raise ValueError(f"not a Git working tree: {source}")
    if destination.exists():
        raise FileExistsError(f"destination already exists: {destination}")
    if destination == source or source in destination.parents or destination in source.parents:
        raise ValueError("source and destination must not overlap")
    commits = run([
        "git", "-C", str(source), "rev-list", "--first-parent", "--reverse", ref
    ]).splitlines()
    if not commits:
        raise ValueError(f"Git ref has no commits: {ref}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".mesh-git-import-", dir=destination.parent) as tmp:
        stage = Path(tmp)
        work = stage / "repo"
        work.mkdir()
        run([mesh_bin, "start"], cwd=work)
        tree = stage / "tree"
        for index, commit in enumerate(commits, 1):
            if tree.exists():
                shutil.rmtree(tree)
            extract_git_tree(source, commit, tree)
            replace_worktree(work, tree)
            subject = run([
                "git", "-C", str(source), "show", "-s", "--format=%s", commit
            ])
            message = f"Git {commit[:12]}: {subject}"[:500]
            run([mesh_bin, "save", message], cwd=work)
            print(f"[{index}/{len(commits)}] imported {commit[:12]} {subject}")
        run([mesh_bin, "admin", "verify"], cwd=work)
        os.rename(work, destination)
    return len(commits)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="local Git working tree")
    parser.add_argument("destination", type=Path, help="new Mesh repository path")
    parser.add_argument("--ref", default="HEAD", help="Git ref to import (default: HEAD)")
    parser.add_argument("--mesh-bin", default=os.environ.get("MESH_BIN", "mesh"))
    args = parser.parse_args(argv)
    try:
        count = import_history(args.source, args.destination, args.mesh_bin, args.ref)
    except (OSError, subprocess.CalledProcessError, tarfile.TarError, ValueError) as exc:
        detail = getattr(exc, "stderr", None)
        if isinstance(detail, bytes):
            detail = detail.decode("utf-8", "replace")
        print(f"git import: {exc}" + (f"\n{detail.strip()}" if detail else ""), file=sys.stderr)
        return 1
    print(f"Imported {count} first-parent Git commit(s) into {args.destination}.")
    print("Note: merge parents, branches/tags, exact author dates and Git OIDs are not retained.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
