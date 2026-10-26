#!/usr/bin/env python3
"""Regression tests for tools.unicode_audit."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import unicode_audit


class UnicodeAuditTests(unittest.TestCase):
    def test_nfc_and_full_casefold_collisions(self) -> None:
        groups = unicode_audit.find_collisions(
            ["caf\u00e9.txt", "cafe\u0301.txt", "Stra\u00dfe", "STRASSE"]
        )
        self.assertEqual(len(groups), 2)

    def test_distinct_unicode_names_remain_distinct(self) -> None:
        self.assertEqual(
            unicode_audit.find_collisions(["alpha", "beta", "caf\u00e9", "cafe"]), []
        )

    def test_invalid_surrogate_is_rejected(self) -> None:
        with self.assertRaises(UnicodeEncodeError):
            unicode_audit.collision_key("bad\udcff")

    def test_repository_walk_excludes_mesh_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "visible").mkdir()
            (root / "visible" / "file").write_text("x", encoding="utf-8")
            (root / ".mesh").mkdir()
            (root / ".mesh" / "private").write_text("x", encoding="utf-8")
            self.assertEqual(list(unicode_audit.repository_paths(root)), ["visible", "visible/file"])


if __name__ == "__main__":
    unittest.main()
