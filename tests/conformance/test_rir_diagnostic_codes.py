"""Self-tests for the RIR diagnostic-code check.

The check reports a clean repository, so its own power is what has to be shown.
The first draft reported both TypeScript surfaces as NOT COMPARED against docs
that were already correct: it looked for its anchor line by line, and at this
tree's column width the anchor wraps. That draft would have passed the day the
real defect was introduced and failed forever after for an unrelated reason --
and "no doc block contains the anchor" reads identically whether the doc is
missing or the reader is blind.

So the cases below drive the scanner with inputs whose answer is known,
including the exact text that shipped with the code missing.
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

_spec = importlib.util.spec_from_file_location(
    "check_rir_diagnostic_codes", Path(__file__).with_name("check_rir_diagnostic_codes.py")
)
assert _spec and _spec.loader
check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check)

PRODUCER = """
  result.diagnostics.push_back({Diagnostic::Severity::Warning, "acoustic.ism_order_clamped", m});
  result.diagnostics.push_back(
      {Diagnostic::Severity::Warning, "acoustic.rir_length_clamped", clamp_message});
  result.diagnostics.push_back(
      {Diagnostic::Severity::Warning, "acoustic.rir_length_floored", m});
  result.diagnostics.push_back({Diagnostic::Severity::Warning, "acoustic.no_late_tail", m});
  result.diagnostics.push_back({Diagnostic::Severity::Error, "acoustic.invalid_rir_config", m});
"""

# The wording that shipped: three of the four codes, and the anchor wrapped
# across lines exactly as the real files wrap it.
DOC_BEFORE_THE_FIX = """
interface RoomMorphResult {
  /**
   * Every diagnostic the target-room synthesis reported, in order. Each says the
   * morph went through a room other than the one requested — an image-source
   * order reduced to the safe maximum (`acoustic.ism_order_clamped`), a tail cut
   * against `maxSeconds` (`acoustic.rir_length_clamped`), a request that produced
   * no diffuse tail (`acoustic.no_late_tail`) — and is otherwise invisible.
   */
  diagnostics: RirDiagnostic[];
}
"""

DOC_AFTER_THE_FIX = DOC_BEFORE_THE_FIX.replace(
    "a request that produced",
    "extended to fit it (`acoustic.rir_length_floored`), a request that produced",
)


class RirDiagnosticCodes(unittest.TestCase):
    def test_the_repository_names_every_warning_code_on_every_surface(self):
        self.assertEqual(check.main(), 0)

    def test_the_producer_reader_separates_warnings_from_errors(self):
        self.assertEqual(
            check.emitted(PRODUCER, "Warning"),
            {
                "acoustic.ism_order_clamped",
                "acoustic.rir_length_clamped",
                "acoustic.rir_length_floored",
                "acoustic.no_late_tail",
            },
        )
        self.assertEqual(check.emitted(PRODUCER, "Error"), {"acoustic.invalid_rir_config"})

    def test_the_wording_that_shipped_is_reported_as_missing_one_code(self):
        """The historical positive control: the defect this check exists for."""
        findings, comparisons = scan_one(DOC_BEFORE_THE_FIX)
        self.assertGreater(comparisons, 0)
        self.assertEqual(
            [f for f in findings if "rir_length_floored" in f and f.startswith("MISSING")],
            ["MISSING acoustic.rir_length_floored [node]: emitted but not named in the doc"],
        )

    def test_the_repaired_wording_is_clean(self):
        findings, comparisons = scan_one(DOC_AFTER_THE_FIX)
        self.assertEqual(findings, [])
        # One surface, and the fixture's own population: 4 warnings + 1 error.
        self.assertEqual(comparisons, 5)

    def test_an_anchor_that_wraps_is_still_found(self):
        """The first draft failed exactly here, against correct documentation."""
        block = check.doc_block(DOC_AFTER_THE_FIX, "the morph went through a room")
        self.assertIsNotNone(block)
        self.assertIn("acoustic.rir_length_floored", block)

    def test_pasting_an_error_code_in_does_not_satisfy_the_check(self):
        """Both directions: the doc must name the warnings and only the warnings."""
        doc = DOC_AFTER_THE_FIX.replace(
            "and is otherwise invisible",
            "and is otherwise invisible (`acoustic.invalid_rir_config`)",
        )
        findings, _ = scan_one(doc)
        self.assertEqual(
            findings,
            [
                (
                    "UNEXPECTED acoustic.invalid_rir_config [node]: "
                    "an error code named in a warning-only doc"
                )
            ],
        )

    def test_a_producer_the_reader_cannot_reach_fails_rather_than_passing(self):
        findings, comparisons = check.scan("", {"node": DOC_AFTER_THE_FIX})
        self.assertEqual(comparisons, 0)
        self.assertTrue(findings[0].startswith("UNREACHED producer"), findings)

    def test_a_surface_whose_block_is_absent_is_reported_not_skipped(self):
        findings, _ = scan_one("interface RoomMorphResult { diagnostics: RirDiagnostic[]; }")
        self.assertEqual(len(findings), 1)
        self.assertTrue(findings[0].startswith("NOT COMPARED [node]"), findings)


def scan_one(doc: str):
    """Drive the scanner over one surface, keyed 'node' so its anchor applies."""
    return check.scan(PRODUCER, {"node": doc})


if __name__ == "__main__":
    unittest.main()
