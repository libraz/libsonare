#!/usr/bin/env python3
"""Stdlib self-tests for the zero-default sentinel-filter checker.

Every case drives :func:`check_zero_sentinel_filter.evaluate` -- the function the
shipping entry point calls -- rather than restating its rules, because a class
asserted through a reimplementation would only ever agree with itself.

The half that carries the value is the must-not-flag corpus.  A guard on a `> 0`
comparison is only worth shipping if it can tell a sentinel filter from an
ordinary positivity test, and these trees contain several hundred of the latter,
so the discrimination is asserted against real lines taken from them rather than
argued.  The correctly-spelled `!= 0` sentinel tests are asserted the same way:
they are the population the fix produces, and a guard that flagged them would
report every site it had just been used to repair.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_zero_sentinel_filter",
    Path(__file__).resolve().parent / "check_zero_sentinel_filter.py",
)
assert _SPEC and _SPEC.loader
scope = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(scope)

NO_FLOOR: dict[str, int] = {}
NO_RECORDS: dict[str, list] = {"sites": []}

# Real `> 0` comparisons from the scanned trees, none of which is a sentinel
# filter. Each guards other work with the comparison instead of re-assigning the
# thing it compared, which is the whole discriminator.
POSITIVITY_TESTS = """
void f() {
  if (length > 0) {
    process(buffer, length);
  }
  if (out->n_frames > 0) return SONARE_OK;
  if (count > 0 && limit > 0) total = count * limit;
  for (int i = 0; i < n; ++i) {
    if (weights[i] > 0.0f) sum += weights[i] * gain;
  }
  const bool voiced = confidence > 0.0f;
  size_t frames = length > 0 ? length / hop : 0;
  if (config->sample_rate > 0) cfg.sample_rate = audio.sample_rate();
}
"""

# The correct spelling of the same sentinel, which the fix produces.
CORRECT_SENTINEL = """
void f() {
  if (true_peak_oversample != 0) config.true_peak_oversample = true_peak_oversample;
  cfg.release_ms = sonare::ZeroIsDefault(config->release_ms).or_default(cfg.release_ms);
  cfg.fmin = sonare::ZeroIsDefault(fmin).checked_non_negative(cfg.fmin, "fmin");
}
"""

# The demonstrated site, in the spelling the finding drove it in.
DEMONSTRATED = """
void f() {
  if (true_peak_oversample > 0) config.true_peak_oversample = true_peak_oversample;
}
"""


class _SyntheticTree(unittest.TestCase):
    """Cases that need a tree of their own, one file deep."""

    def tree(self, body: str, name: str = "m.cpp") -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "src" / "c_api").mkdir(parents=True)
        (root / "src" / "c_api" / name).write_text(body, encoding="utf-8")
        return root

    def scan(self, body: str, name: str = "m.cpp") -> scope.Scan:
        return scope.Scan(self.tree(body, name))

    def evaluate(self, body: str, *, records=None, floor=None):
        return scope.evaluate(
            self.scan(body),
            scope.Records(records if records is not None else NO_RECORDS),
            floor if floor is not None else NO_FLOOR,
        )

    def only(self, failures, fragment: str):
        """One class, and the one intended -- not merely at least the one."""
        self.assertEqual(
            len(failures),
            1,
            f"expected one failure class, got: {[heading for heading, _ in failures]}",
        )
        self.assertIn(fragment, failures[0][0])
        return failures[0][1]


class DiscriminationTest(_SyntheticTree):
    """What the guard must not report. The expensive half to get right."""

    def test_ordinary_positivity_tests_are_not_reported(self) -> None:
        self.assertEqual(self.scan(POSITIVITY_TESTS).sites, [])

    def test_the_correct_sentinel_spelling_is_not_reported(self) -> None:
        self.assertEqual(self.scan(CORRECT_SENTINEL).sites, [])

    def test_a_clamp_to_zero_is_not_reported(self) -> None:
        """`x > 0 ? x : 0` replaces nothing the caller chose -- the fallback is
        the sentinel itself, so it clamps rather than selecting a default."""
        body = "void f() { n = num_mod_routings > 0 ? num_mod_routings : 0; }\n"
        self.assertEqual(self.scan(body).sites, [])

    def test_a_self_assignment_is_not_reported(self) -> None:
        body = "void f() { if (gain > 0.0f) gain = gain; }\n"
        self.assertEqual(self.scan(body).sites, [])

    def test_the_idiom_quoted_in_a_comment_is_not_reported(self) -> None:
        """The converted sites explain themselves by naming the shape they no
        longer have, so a scan over raw text would report its own fix notes."""
        body = "// Filtering on `if (x > 0) cfg.field = x;` would discard a request.\nvoid f() {}\n"
        self.assertEqual(self.scan(body).sites, [])


class DetectionTest(_SyntheticTree):
    """What the guard must report."""

    def test_the_demonstrated_site_is_reported(self) -> None:
        lines = self.only(self.evaluate(DEMONSTRATED), "zero-default sentinel")
        self.assertEqual(len(lines), 1)
        self.assertIn("true_peak_oversample", lines[0])

    def test_the_statement_form_is_reported_across_a_line_break(self) -> None:
        body = "void f() {\n  if (fmin > 0.0f)\n    config.fmin = fmin;\n}\n"
        self.assertEqual(len(self.scan(body).sites), 1)

    def test_the_statement_form_is_reported_inside_a_braced_block(self) -> None:
        """Six sites in these trees are written this way, and a pattern that
        stopped at the unbraced spelling would have reported none of them."""
        body = "void f() {\n  if (click_seconds > 0.0) {\n    out.click_seconds = click_seconds;\n  }\n}\n"
        self.assertEqual(len(self.scan(body).sites), 1)

    def test_the_conditional_form_is_reported(self) -> None:
        body = "void f() { int b = options->dither_bits > 0 ? options->dither_bits : 16; }\n"
        self.assertEqual(len(self.scan(body).sites), 1)

    def test_a_record_suppresses_its_own_site_only(self) -> None:
        records = {"sites": [{"file": "src/c_api/m.cpp", "value": "true_peak_oversample"}]}
        root = self.tree(DEMONSTRATED)
        scan = scope.Scan(root)
        # The record is keyed on the shipping tree's paths, so re-key it onto
        # the synthetic one rather than asserting through a second path rule.
        records["sites"][0]["file"] = scope._display(scan.sites[0].path)
        self.assertEqual(scope.evaluate(scan, scope.Records(records), NO_FLOOR), [])

    def test_a_record_that_matches_nothing_is_reported(self) -> None:
        records = {"sites": [{"file": "src/c_api/gone.cpp", "value": "removed_field"}]}
        lines = self.only(self.evaluate(CORRECT_SENTINEL, records=records), "matched nothing")
        self.assertIn("removed_field", lines[0])

    def test_the_floor_fires_on_a_population_that_stopped_matching(self) -> None:
        lines = self.only(
            self.evaluate(CORRECT_SENTINEL, floor={"sites": 1}),
            "no longer finds the population",
        )
        self.assertIn("floor is 1", lines[0])


class ShippingTreeTest(unittest.TestCase):
    def test_the_trees_report_nothing(self) -> None:
        data = scope.load_records()
        scan = scope.Scan()
        self.assertEqual(scope.evaluate(scan, scope.Records(data), data["floor"]), [])

    def test_the_trees_clear_the_floor_they_are_sized_for(self) -> None:
        floor = scope.load_records()["floor"]
        scan = scope.Scan()
        self.assertGreaterEqual(len(scan.sites), floor["sites"])
        self.assertGreaterEqual(scan.comparisons, floor["comparisons"])
        self.assertGreaterEqual(scan.helper_calls, floor["helper_calls"])

    def test_the_reported_share_of_positivity_tests_stays_small(self) -> None:
        """The discrimination, measured on the real tree rather than a fixture.

        Every reported site was read by hand and assigns the expression it
        tested; the comparisons read past are the population a looser pattern
        would have swept in. A pattern that started matching them would show up
        here long before anyone reviewed the report.
        """
        scan = scope.Scan()
        self.assertLess(len(scan.sites), scan.comparisons * 0.25)


if __name__ == "__main__":
    unittest.main()
