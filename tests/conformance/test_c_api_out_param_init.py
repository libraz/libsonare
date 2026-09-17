"""Stdlib self-tests for the C-ABI out-parameter initialisation checker.

The checker's failure mode is a false clean: every way it can break -- a body
truncated before its early returns, a write pattern that stops matching, a
preprocessor branch read as straight-line code -- makes it report fewer sites
than the tree contains, and an empty report is indistinguishable from a healthy
one.  So these tests pin two things separately.  The classification tests fix
the shape of each rule on hand-written bodies.  The tree tests pin a nonzero
floor on what the scan resolves at all, and demonstrate non-vacuity by copying
the real translation units, deleting one known zero-write, and requiring the
checker to name exactly that site and nothing else.

The copy is the point: the break has to happen on the real text to prove
anything, and it must not happen in the shared tree, where another session
compiling in that window would inherit a defect with no error to show for it.
"""

from __future__ import annotations

import importlib.util
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_c_api_out_param_init.py"
SPEC = importlib.util.spec_from_file_location(
    "libsonare_c_api_out_param_init_checker", CHECKER_PATH
)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
# Registered before execution because the checker defines dataclasses, which
# resolve their annotations through sys.modules at class-creation time.
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)

SOURCE_DIR = ROOT / "src" / "c_api"
HEADER_DIR = ROOT / "include" / "sonare"

# The site the non-vacuity break removes: a feature-disabled branch that does
# define its out-parameter before returning NOT_SUPPORTED.
BREAK_FILE = "sonare_c_engine_midi.cpp"
BREAK_LINE = "*out_count = 0;"
BREAK_FUNCTION = "sonare_engine_midi_instrument_count"
BREAK_PARAMETER = "out_count"

# Floors, not targets.  They exist so a pattern that silently stops matching
# fails here instead of reporting a clean tree.  Raise them only alongside a
# real widening of the scan.
MIN_ENTRY_POINTS = 500
MIN_RESOLVED_OUT_PARAMS = 250


def _scan(body_source: str, *, header_dir: Path | None = None):
    """Run the checker over one synthetic translation unit."""
    with tempfile.TemporaryDirectory() as tmp:
        source_dir = Path(tmp) / "c_api"
        source_dir.mkdir()
        (source_dir / "unit.cpp").write_text(body_source, encoding="utf-8")
        return CHECKER.audit(source_dir, header_dir or Path(tmp) / "absent")


class ClassificationTest(unittest.TestCase):
    def test_validation_return_before_the_write_is_reported(self) -> None:
        report = _scan(
            "SonareError sonare_f(int n, SonareResult* out) {\n"
            "  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  if (n <= 0) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  *out = {};\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertEqual(len(report.findings), 1)
        finding = report.findings[0]
        self.assertEqual((finding.kind, finding.function, finding.parameter), ("validation", "sonare_f", "out"))
        self.assertEqual(finding.return_line, 3)

    def test_null_guard_of_the_same_parameter_is_not_a_finding(self) -> None:
        report = _scan(
            "SonareError sonare_f(const float* in, SonareResult* out) {\n"
            "  if (!in || !out) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  *out = {};\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])

    def test_null_guard_in_a_later_operand_still_excludes(self) -> None:
        # The combined-condition case: a scan that inspects only the first
        # operand reports this, and every one of those reports is wrong.
        report = _scan(
            "SonareError sonare_f(const float* in, size_t n, SonareResult* out) {\n"
            "  if (in == nullptr || n == 0 || out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  *out = {};\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])

    def test_a_long_body_is_measured_by_brace_balance(self) -> None:
        filler = "  int pad = 0;\n" * 400
        report = _scan(
            "SonareError sonare_f(int n, SonareResult* out) {\n"
            f"{filler}"
            "  if (n <= 0) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  *out = {};\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertEqual(len(report.findings), 1, "a fixed window would miss this return")

    def test_a_lambda_return_is_not_an_exit_from_the_entry_point(self) -> None:
        report = _scan(
            "SonareError sonare_f(const float* in, SonareResult* out) {\n"
            "  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  *out = {};\n"
            "  return run_offline(in, [&](const Audio& a) -> SonareError {\n"
            "    if (a.empty()) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "    return SONARE_OK;\n"
            "  });\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])

    def test_a_tail_return_whose_lambda_holds_the_write_is_not_an_early_return(self) -> None:
        report = _scan(
            "SonareError sonare_f(const float* in, SonareResult* out) {\n"
            "  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  return run_offline(in, [&](const Audio& a) -> SonareError {\n"
            "    *out = {};\n"
            "    return SONARE_OK;\n"
            "  });\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])

    def test_a_handle_read_before_it_is_written_is_not_an_out_parameter(self) -> None:
        report = _scan(
            "SonareError sonare_f(SonareMixer* mixer, const char* id) {\n"
            "  if (!mixer || !id) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  for (const auto& bus : mixer->buses) {\n"
            "    if (bus.id == id) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  }\n"
            "  mixer->dirty = true;\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])
        self.assertEqual(report.in_place, 1)

    def test_a_const_input_pointer_is_not_an_out_parameter(self) -> None:
        report = _scan(
            "SonareError sonare_f(const float* in, size_t n) {\n"
            "  if (n == 0) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertEqual((report.findings, report.resolved), ([], 0))

    def test_a_write_reachable_only_through_a_call_is_unanalysable_not_clean(self) -> None:
        report = _scan(
            "SonareError sonare_f(const float* in, SonareResult* out) {\n"
            "  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  return fill_result(in, out);\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])
        self.assertEqual(len(report.unanalysable), 1)
        self.assertIn("sonare_f(out)", report.unanalysable[0])

    def test_a_branch_that_never_writes_is_reported_even_though_a_sibling_does(self) -> None:
        report = _scan(
            "SonareError sonare_f(SonareResult* out) {\n"
            "  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "#if defined(SONARE_WITH_THING)\n"
            "  *out = {};\n"
            "  return SONARE_OK;\n"
            "#else\n"
            "  return SONARE_ERROR_NOT_SUPPORTED;\n"
            "#endif\n"
            "}\n"
        )
        self.assertEqual([f.kind for f in report.findings], ["feature-disabled"])
        self.assertEqual(report.findings[0].return_line, 7)

    def test_a_branch_that_defines_the_parameter_first_is_clean(self) -> None:
        report = _scan(
            "SonareError sonare_f(SonareResult* out) {\n"
            "  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "#if defined(SONARE_WITH_THING)\n"
            "  *out = {};\n"
            "  return SONARE_OK;\n"
            "#else\n"
            "  *out = {};\n"
            "  return SONARE_ERROR_NOT_SUPPORTED;\n"
            "#endif\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])

    def test_a_stub_macro_counts_as_a_return(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source_dir = Path(tmp) / "c_api"
            source_dir.mkdir()
            (source_dir / "guard.h").write_text(
                "#define SONARE_C_STUB_NOT_SUPPORTED(...)   \\\n"
                "  do {                                     \\\n"
                "    ignore_args(__VA_ARGS__);              \\\n"
                "    return SONARE_ERROR_NOT_SUPPORTED;     \\\n"
                "  } while (false)\n",
                encoding="utf-8",
            )
            (source_dir / "unit.cpp").write_text(
                "SonareError sonare_f(SonareResult* out) {\n"
                "#if defined(SONARE_WITH_THING)\n"
                "  *out = {};\n"
                "  return SONARE_OK;\n"
                "#else\n"
                "  SONARE_C_STUB_NOT_SUPPORTED(out);\n"
                "#endif\n"
                "}\n",
                encoding="utf-8",
            )
            report = CHECKER.audit(source_dir, Path(tmp) / "absent")
        self.assertEqual([f.kind for f in report.findings], ["feature-disabled"])

    def test_the_exception_translation_macro_is_not_a_return(self) -> None:
        # Its returns are all on catch arms, so treating it as an unconditional
        # exit would report every guarded body as failing before its write.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "guard.h"
            path.write_text(
                "#define SONARE_C_CATCH                \\\n"
                "  }                                   \\\n"
                "  catch (const std::exception& e) {   \\\n"
                "    return SONARE_ERROR_UNKNOWN;      \\\n"
                "  }\n"
                "#define SONARE_C_STUB_NOT_SUPPORTED(...) return SONARE_ERROR_NOT_SUPPORTED\n",
                encoding="utf-8",
            )
            macros = CHECKER.unconditional_return_macros([path])
        self.assertEqual(macros, {"SONARE_C_STUB_NOT_SUPPORTED"})

    def test_a_declaration_documented_success_only_is_excluded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            header_dir = Path(tmp) / "sonare"
            header_dir.mkdir(parents=True)
            (header_dir / "api.h").write_text(
                "/// @brief Does a thing.\n"
                "/// @param out Receives the result, written only on success.\n"
                "SonareError sonare_f(int n, SonareResult* out);\n",
                encoding="utf-8",
            )
            report = _scan(
                "SonareError sonare_f(int n, SonareResult* out) {\n"
                "  if (n <= 0) return SONARE_ERROR_INVALID_PARAMETER;\n"
                "  *out = {};\n"
                "  return SONARE_OK;\n"
                "}\n",
                header_dir=header_dir,
            )
        self.assertEqual(report.findings, [])

    def test_an_inline_reference_does_not_truncate_the_param_body(self) -> None:
        # `@p` is prose, not a block command: splitting on it would cut the
        # body before the sentence carrying the contract.
        with tempfile.TemporaryDirectory() as tmp:
            header_dir = Path(tmp) / "sonare"
            header_dir.mkdir(parents=True)
            (header_dir / "api.h").write_text(
                "/// @brief Does a thing.\n"
                "/// @param out Holds up to @p n entries, and is untouched on failure.\n"
                "SonareError sonare_f(int n, SonareResult* out);\n",
                encoding="utf-8",
            )
            report = _scan(
                "SonareError sonare_f(int n, SonareResult* out) {\n"
                "  if (n <= 0) return SONARE_ERROR_INVALID_PARAMETER;\n"
                "  *out = {};\n"
                "  return SONARE_OK;\n"
                "}\n",
                header_dir=header_dir,
            )
        self.assertEqual(report.findings, [])

    def test_an_opaque_handle_is_a_receiver_not_an_out_parameter(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            header_dir = Path(tmp) / "sonare"
            header_dir.mkdir(parents=True)
            (header_dir / "api.h").write_text(
                "typedef struct SonareThing SonareThing;\n"
                "SonareError sonare_thing_load(SonareThing* thing, int n);\n",
                encoding="utf-8",
            )
            report = _scan(
                "SonareError sonare_thing_load(SonareThing* thing, int n) {\n"
                "  if (n <= 0) return SONARE_ERROR_INVALID_PARAMETER;\n"
                "  thing->field = n;\n"
                "  return SONARE_OK;\n"
                "}\n",
                header_dir=header_dir,
            )
        self.assertEqual(report.findings, [])
        self.assertEqual(report.receivers, 1)

    def test_a_handle_out_parameter_stays_in_the_scan(self) -> None:
        # `SonareThing**` is a slot the caller owns, however opaque the pointee.
        with tempfile.TemporaryDirectory() as tmp:
            header_dir = Path(tmp) / "sonare"
            header_dir.mkdir(parents=True)
            (header_dir / "api.h").write_text(
                "typedef struct SonareThing SonareThing;\n"
                "SonareError sonare_thing_create(int n, SonareThing** out);\n",
                encoding="utf-8",
            )
            report = _scan(
                "SonareError sonare_thing_create(int n, SonareThing** out) {\n"
                "  if (n <= 0) return SONARE_ERROR_INVALID_PARAMETER;\n"
                "  *out = new SonareThing{};\n"
                "  return SONARE_OK;\n"
                "}\n",
                header_dir=header_dir,
            )
        self.assertEqual([f.parameter for f in report.findings], ["out"])
        self.assertEqual(report.receivers, 0)

    def test_a_defined_type_is_not_an_opaque_handle(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            header_dir = Path(tmp) / "sonare"
            header_dir.mkdir(parents=True)
            (header_dir / "api.h").write_text(
                "typedef struct SonareThing SonareThing;\n"
                "struct SonareThing {\n  int field;\n};\n"
                "SonareError sonare_thing_fill(SonareThing* out, int n);\n",
                encoding="utf-8",
            )
            report = _scan(
                "SonareError sonare_thing_fill(SonareThing* out, int n) {\n"
                "  if (n <= 0) return SONARE_ERROR_INVALID_PARAMETER;\n"
                "  out->field = n;\n"
                "  return SONARE_OK;\n"
                "}\n",
                header_dir=header_dir,
            )
        self.assertEqual([f.parameter for f in report.findings], ["out"])
        self.assertEqual(report.receivers, 0)


class TreeTest(unittest.TestCase):
    """The scan against the shipped C-ABI sources."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.report = CHECKER.audit(SOURCE_DIR, HEADER_DIR)

    def test_the_scan_resolves_a_nonzero_population(self) -> None:
        self.assertGreaterEqual(self.report.entry_points, MIN_ENTRY_POINTS)
        self.assertGreaterEqual(self.report.resolved, MIN_RESOLVED_OUT_PARAMS)

    def test_the_break_site_is_clean_as_written(self) -> None:
        # A precondition of the non-vacuity test below: the site it breaks must
        # define its out-parameter today, or the break proves nothing.
        named = [
            f
            for f in self.report.findings
            if f.function == BREAK_FUNCTION and f.parameter == BREAK_PARAMETER
        ]
        self.assertEqual(named, [])

    def test_deleting_one_zero_write_reports_exactly_that_site(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "c_api"
            shutil.copytree(SOURCE_DIR, copy)
            path = copy / BREAK_FILE
            lines = path.read_text(encoding="utf-8").split("\n")
            index = next(
                i
                for i, line in enumerate(lines)
                if line.strip() == BREAK_LINE and "SONARE_WITH_ARRANGEMENT" in lines[i - 1]
            )
            del lines[index]
            path.write_text("\n".join(lines), encoding="utf-8")

            before = {self._key(f) for f in CHECKER.audit(copy, HEADER_DIR).findings}
            baseline = {self._key(f) for f in self.report.findings}
            new = sorted(before - baseline)

        self.assertEqual(
            new,
            [("feature-disabled", BREAK_FILE, BREAK_FUNCTION, BREAK_PARAMETER)],
            "the break must produce exactly one new finding, naming the site",
        )

    @staticmethod
    def _key(finding) -> tuple[str, str, str, str]:
        return (finding.kind, Path(finding.file).name, finding.function, finding.parameter)


if __name__ == "__main__":
    unittest.main()
