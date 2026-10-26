#!/usr/bin/env python3
"""Read-only inventory of Git and Mesh state in a shared working tree.

This helper delegates Git interpretation to the installed Git CLI and never
runs commands that write refs, index entries, configuration, or worktree files.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def run(argv: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["GIT_OPTIONAL_LOCKS"] = "0"
    return subprocess.run(argv, cwd=cwd, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False)


def inspect(root: Path, mesh_bin: str, git_bin: str) -> dict[str, object]:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError(f"not a directory: {root}")
    git = shutil.which(git_bin)
    if not git:
        raise RuntimeError(f"Git executable not found: {git_bin}")
    mesh = shutil.which(mesh_bin) if os.path.sep not in mesh_bin else str(Path(mesh_bin).resolve())
    if not mesh or not Path(mesh).is_file():
        raise RuntimeError(f"Mesh executable not found: {mesh_bin}")

    top = run([git, "-C", str(root), "rev-parse", "--show-toplevel"], root)
    git_root = top.stdout.strip() if top.returncode == 0 else None
    git_state: dict[str, object] = {"repository": git_root, "available": git_root is not None}
    if git_root:
        branch = run([git, "-C", str(root), "branch", "--show-current"], root)
        status = run([git, "-C", str(root), "status", "--porcelain=v1", "--untracked-files=all"], root)
        git_state.update({
            "branch": branch.stdout.strip() if branch.returncode == 0 else None,
            "dirty": bool(status.stdout),
            "status_entries": status.stdout.splitlines() if status.returncode == 0 else [],
        })

    mesh_state: dict[str, object] = {"repository": (root / ".mesh").is_dir()}
    if mesh_state["repository"]:
        report = run([mesh, "git-info"], root)
        mesh_state.update({"git_info_exit": report.returncode,
                           "git_info": (report.stdout + report.stderr).strip()})

    return {
        "root": str(root),
        "coexistence": bool(git_root and mesh_state["repository"]),
        "git": git_state,
        "mesh": mesh_state,
        "policy": "read-only; does not modify Git metadata, Mesh metadata, or working files",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", default=".", help="working-tree directory (default: current directory)")
    parser.add_argument("--mesh", default=os.environ.get("MESH_BIN", "mesh"), help="Mesh executable")
    parser.add_argument("--git", default="git", help="Git executable")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args(argv)
    try:
        result = inspect(Path(args.root), args.mesh, args.git)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"git-coexist: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print(f"Root: {result['root']}")
        print(f"Git: {'present' if result['git']['available'] else 'not detected'}")
        print(f"Mesh: {'present' if result['mesh']['repository'] else 'not detected'}")
        print(f"Coexisting repositories: {'yes' if result['coexistence'] else 'no'}")
        if result["git"]["available"]:
            print(f"Git branch: {result['git']['branch'] or '(detached HEAD)'}")
            print(f"Git working tree: {'modified' if result['git']['dirty'] else 'clean'}")
        print(result["policy"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
