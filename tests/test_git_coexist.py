#!/usr/bin/env python3
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "git_coexist.py"
MESH = os.environ.get("MESH_BIN", str(ROOT / "mesh"))


def run(args: list[str], cwd: Path, ok: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if ok and result.returncode:
        raise AssertionError(f"{args}: {result.stderr}{result.stdout}")
    return result


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="mesh-git-coexist-") as temp:
        root = Path(temp) / "repo"
        root.mkdir()
        run(["git", "init", "-q", str(root)], root)
        run(["git", "-C", str(root), "-c", "user.name=Mesh Test", "-c",
             "user.email=mesh@example.invalid", "commit", "--allow-empty", "-m", "git root"], root)
        run([MESH, "start"], root)
        before = run(["git", "-C", str(root), "status", "--porcelain=v1"], root).stdout
        report = run([sys.executable, str(TOOL), "--mesh", MESH, "--json", str(root)], root)
        data = json.loads(report.stdout)
        assert data["coexistence"] is True, data
        assert data["git"]["available"] is True, data
        assert data["mesh"]["repository"] is True, data
        assert "read-only" in data["policy"]
        after = run(["git", "-C", str(root), "status", "--porcelain=v1"], root).stdout
        assert before == after, "read-only coexistence audit changed Git status"
        info = run([MESH, "git-info"], root)
        assert "Git repository: detected" in info.stdout, info.stdout
        assert "independent store" in info.stdout, info.stdout
    print("git coexistence tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
