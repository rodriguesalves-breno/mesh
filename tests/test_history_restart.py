#!/usr/bin/env python3
"""Integration tests for the explicit, backup-first history restart tool."""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MESH = Path(os.environ.get("MESH_BIN", str(ROOT / "mesh"))).resolve()
TOOL = ROOT / "tools" / "history_restart.py"


def run(argv: list[str], cwd: Path) -> str:
    result = subprocess.run(argv, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise AssertionError(f"command failed: {argv}\n{result.stderr}")
    return result.stdout


class HistoryRestartTests(unittest.TestCase):
    def make_repo(self, root: Path) -> bytes:
        run([str(MESH), "start"], root)
        (root / "file.txt").write_text("one\n", encoding="utf-8")
        run([str(MESH), "save", "first"], root)
        (root / "file.txt").write_text("two\n", encoding="utf-8")
        run([str(MESH), "save", "second"], root)
        return (root / ".mesh" / "history").read_bytes()

    def test_dry_run_leaves_repository_unchanged(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mesh-history-restart-") as tmp:
            base = Path(tmp); root = base / "repo"; root.mkdir()
            history = self.make_repo(root)
            backup = base / "history-backup"
            result = subprocess.run([
                sys.executable, str(TOOL), "--mesh-bin", str(MESH),
                "--root", str(root), "--backup", str(backup),
            ], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(backup.exists())
            self.assertEqual((root / ".mesh" / "history").read_bytes(), history)

    def test_apply_preserves_backup_and_restarts_active_history(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mesh-history-restart-") as tmp:
            base = Path(tmp); root = base / "repo"; root.mkdir()
            old_history = self.make_repo(root)
            backup = base / "mesh-backup"
            run([
                sys.executable, str(TOOL), "--mesh-bin", str(MESH),
                "--root", str(root), "--backup", str(backup), "--apply",
            ], base)
            self.assertEqual((backup / "history").read_bytes(), old_history)
            self.assertEqual((root / "file.txt").read_text(encoding="utf-8"), "two\n")
            active_history = (root / ".mesh" / "history").read_text(encoding="utf-8")
            self.assertEqual(len([line for line in active_history.splitlines() if line]), 1)
            run([str(MESH), "admin", "verify"], root)
            self.assertIn("History restart", run([str(MESH), "log"], root))


if __name__ == "__main__":
    unittest.main()
