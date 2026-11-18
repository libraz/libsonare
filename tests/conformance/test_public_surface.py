"""The public interface of the checked-out tree, against the surface rules and the ledgers.

One full ``tools/api/check_surface.py`` run over the live tree. Every category it
reports must be empty: names without a domain, ledger validation problems, ledger
rows the tree contradicts, names of the base release gone with no row, and the
findings of each rule check. The checker's own fixture tests live beside it in
``tools/api``; this case is the claim that the tree itself obeys it.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

_API = Path(__file__).resolve().parents[2] / "tools" / "api"
sys.path.insert(0, str(_API))

import check_surface


class PublicSurface(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.code, cls.lines, cls.report = check_surface.run(check_surface.parse_args(["--all"]))

    def test_every_name_has_a_domain(self):
        self.assertEqual(self.report["domain_unassigned"], [])

    def test_the_ledgers_are_well_formed(self):
        self.assertEqual(self.report["problems"], [])

    def test_the_base_release_was_compared(self):
        skipped = [x.strip() for x in self.lines if "not compared: ledger old names" in x]
        skipped += [x.strip() for x in self.lines if "names gone with no row (" in x]
        self.assertEqual(skipped, [])

    def test_every_ledger_row_holds_and_every_gone_name_has_one(self):
        self.assertEqual([check_surface.line_of(f) for f in self.report["ledger_failures"]], [])

    def test_every_rule_check_ran_and_found_nothing(self):
        ran = {c["id"]: c for c in self.report["checks"]}
        self.assertEqual(sorted(ran), sorted(c.id for c in check_surface.CHECKS))
        for cid, c in sorted(ran.items()):
            with self.subTest(check=cid):
                self.assertGreater(c["checked"], 0)
                self.assertEqual([check_surface.line_of(v) for v in c["violations"]], [])

    def test_the_run_exits_clean(self):
        self.assertEqual(self.code, 0, "\n".join(self.lines))


if __name__ == "__main__":
    unittest.main()
