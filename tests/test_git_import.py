#!/usr/bin/env python3
"""Integration test for the Git first-parent importer."""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MESH = Path(os.environ.get("MESH_BIN", str(ROOT / "mesh"))).resolve()
IMPORTER = ROOT / "tools" / "git_import.py"


def run(argv: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(argv, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise AssertionError(f"command failed: {argv}\n{result.stderr}")
    return result.stdout


class GitImporterTests(unittest.TestCase):
    def test_imports_first_parent_tree_history_atomically(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mesh-git-import-test-") as tmp:
            base = Path(tmp)
            source = base / "git source"
            destination = base / "mesh destination"
            source.mkdir()
            run(["git", "init", "-q"], source)
            run(["git", "config", "user.name", "Import Test"], source)
            run(["git", "config", "user.email", "import@example.invalid"], source)
            (source / "file.txt").write_text("one\n", encoding="utf-8")
            run(["git", "add", "file.txt"], source)
            run(["git", "commit", "-q", "-m", "first"], source)
            (source / "file.txt").write_text("two\n", encoding="utf-8")
            run(["git", "commit", "-q", "-am", "second"], source)

            run([
                sys.executable, str(IMPORTER), "--mesh-bin", str(MESH),
                str(source), str(destination),
            ])
            self.assertEqual((destination / "file.txt").read_text(encoding="utf-8"), "two\n")
            history = run([str(MESH), "log"], destination)
            self.assertIn("first", history)
            self.assertIn("second", history)
            run([str(MESH), "admin", "verify"], destination)

    def test_refuses_existing_destination_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mesh-git-import-test-") as tmp:
            base = Path(tmp)
            source = base / "source"
            destination = base / "existing"
            source.mkdir()
            destination.mkdir()
            run(["git", "init", "-q"], source)
            result = subprocess.run([
                sys.executable, str(IMPORTER), "--mesh-bin", str(MESH),
                str(source), str(destination),
            ], text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(destination.is_dir())
            self.assertEqual(list(destination.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
