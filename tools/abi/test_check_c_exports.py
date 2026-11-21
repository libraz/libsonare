"""Stdlib self-tests for the C export check's pure parts."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_c_exports as check


class ExportedEntriesTest(unittest.TestCase):
    def test_only_defined_sonare_text_symbols_are_read(self) -> None:
        elf = (
            "0000000000012340 T sonare_version\n"
            "0000000000012350 T helper_not_ours\n"
            "0000000000012360 D sonare_table\n"
            "                 U sonare_imported\n"
        )
        self.assertEqual(check.exported_entries(elf, mach_o=False), {"sonare_version"})

    def test_the_mach_o_underscore_is_dropped(self) -> None:
        macho = "0000000000001000 T _sonare_version\n0000000000001010 T __Z6helperv\n"
        self.assertEqual(check.exported_entries(macho, mach_o=True), {"sonare_version"})


class CompareTest(unittest.TestCase):
    def test_agreeing_sets_report_nothing(self) -> None:
        self.assertEqual(check.compare({"sonare_a"}, {"sonare_a"}), [])

    def test_a_missing_and_an_undeclared_export_are_both_named(self) -> None:
        self.assertEqual(
            check.compare({"sonare_a", "sonare_b"}, {"sonare_a", "sonare_c"}),
            ["declared but not exported: sonare_b", "exported but not declared: sonare_c"],
        )

    def test_an_empty_side_fails_rather_than_agreeing(self) -> None:
        self.assertEqual(len(check.compare(set(), set())), 2)

    def test_the_tree_headers_are_read(self) -> None:
        declared = check.declared_entries()
        self.assertGreater(len(declared), 1000)
        self.assertIn("sonare_engine_clip_init", declared)


if __name__ == "__main__":
    unittest.main()
