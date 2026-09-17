"""Stdlib self-tests for the WASM feature-gate-scope checker.

The checker's own failure mode is a false clean, and it has two distinct
sources.  The derivation can under-reach -- a gate whose sources live in a
directory it shares with always-compiled code owns nothing, so a leak there is
invisible -- or over-reach, claiming a directory that merely sounds like the
subsystem.  Both were present in the first draft, in one run: it read
``analysis/acoustic/`` as the acoustic-sim gate and missed the pitch editor
entirely.  These tests pin the derivation against synthetic CMake text where the
answer is known by construction, rather than against the repository, where a
wrong answer still looks plausible.

The allowlist has its own pair, and they pull in opposite directions: an entry
must excuse a unit that is really linked, and must not be retired merely because
the build under test left its gate on.  A fixture covering one cannot show the
other, so they are separate.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest import mock

_CHECKER = Path(__file__).resolve().parent / "check_wasm_feature_gate_scope.py"
_spec = importlib.util.spec_from_file_location("check_wasm_feature_gate_scope", _CHECKER)
assert _spec is not None and _spec.loader is not None
gate_scope = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gate_scope)


@contextmanager
def _with_cmake(text: str):
    """Point the checker at synthetic CMake text for the duration of a test."""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "CMakeLists.txt"
        path.write_text(text, encoding="utf-8")
        with mock.patch.object(gate_scope, "CMAKELISTS", path):
            yield


class DerivationTest(unittest.TestCase):
    def test_a_directory_wholly_inside_a_gate_belongs_to_it(self) -> None:
        with _with_cmake("if(BUILD_THING)\n  thing/one.cpp\n  thing/two.cpp\nendif()\n"):
            self.assertEqual(gate_scope.gated_directories(), {"thing/": {"THING"}})

    def test_one_ungated_file_does_not_retire_the_directory(self) -> None:
        # The case this check exists for: a subsystem directory carrying one
        # deliberately always-compiled unit. Demanding every file be gated would
        # report the directory as unowned and never look at it again.
        with _with_cmake(
            "core/always.cpp\nthing/shared.cpp\n"
            "if(BUILD_THING)\n" + "".join(f"  thing/g{i}.cpp\n" for i in range(9)) + "endif()\n"
        ):
            self.assertEqual(gate_scope.gated_directories().get("thing/"), {"THING"})

    def test_a_directory_mostly_ungated_is_not_claimed(self) -> None:
        # Over-reach direction: one gated file in a shared directory must not
        # hand the whole directory to that gate.
        with _with_cmake(
            "".join(f"shared/u{i}.cpp\n" for i in range(9))
            + "if(BUILD_THING)\n  shared/one.cpp\nendif()\n"
        ):
            self.assertNotIn("shared/", gate_scope.gated_directories())

    def test_a_directory_shared_by_two_gates_belongs_to_both(self) -> None:
        with _with_cmake(
            "if(BUILD_A)\n  both/a1.cpp\n  both/a2.cpp\nendif()\n"
            "if(BUILD_B)\n  both/b1.cpp\n  both/b2.cpp\nendif()\n"
        ):
            self.assertEqual(gate_scope.gated_directories().get("both/"), {"A", "B"})

    def test_a_nested_gate_needs_both(self) -> None:
        with _with_cmake(
            "if(BUILD_OUTER)\n  nest/one.cpp\n"
            "  if(BUILD_INNER)\n    nest/two.cpp\n  endif()\nendif()\n"
        ):
            self.assertEqual(gate_scope.gated_directories().get("nest/"), {"OUTER", "INNER"})

    def test_a_non_gate_conditional_does_not_swallow_the_endif(self) -> None:
        # Without a frame pushed for `if(APPLE)`, its endif() pops the gate and
        # every following source reads as ungated -- a silent under-reach.
        with _with_cmake(
            "if(BUILD_THING)\n  thing/one.cpp\n"
            "  if(APPLE)\n    thing/mac.cpp\n  endif()\n"
            "  thing/two.cpp\nendif()\n"
        ):
            self.assertEqual(gate_scope.gated_directories().get("thing/"), {"THING"})

    def test_packaging_gates_own_nothing(self) -> None:
        with _with_cmake("if(BUILD_SHARED)\n  pack/one.cpp\n  pack/two.cpp\nendif()\n"):
            self.assertEqual(gate_scope.gated_directories(), {})


class SourceResolutionTest(unittest.TestCase):
    def test_an_object_resolves_to_its_repository_source(self) -> None:
        obj = Path("/b/src/CMakeFiles/sonare_core_objects.dir/mixing/downmix.cpp.o")
        self.assertEqual(gate_scope._source_of(obj), "mixing/downmix.cpp")

    def test_an_out_of_tree_unit_is_not_claimed_as_a_repository_source(self) -> None:
        obj = Path("/b/tests/CMakeFiles/t.dir/__/src/c_api/sonare_c.cpp.o")
        self.assertIsNone(gate_scope._source_of(obj))


class AllowlistTest(unittest.TestCase):
    CMAKE = "if(BUILD_THING)\n  thing/one.cpp\n  thing/two.cpp\nendif()\n"

    def _check(self, linked: list[str], off: set[str], allowed: dict[str, str]):
        objects = [Path(f"/b/src/CMakeFiles/x.dir/{src}.o") for src in linked]
        with (
            _with_cmake(self.CMAKE),
            mock.patch.object(gate_scope, "module_objects", return_value=objects),
            mock.patch.object(gate_scope, "disabled_gates", return_value=off),
            mock.patch.dict(gate_scope.ALLOWED_RESIDUE, allowed, clear=True),
        ):
            return gate_scope.check(Path("/b"))

    def test_an_unlisted_unit_behind_a_disabled_gate_is_reported(self) -> None:
        findings, reach = self._check(["thing/one.cpp"], {"THING"}, {})
        self.assertEqual(len(findings), 1)
        self.assertIn("thing/one.cpp", findings[0])
        self.assertEqual(reach["sources_resolved"], 1)

    def test_a_listed_unit_behind_a_disabled_gate_passes(self) -> None:
        findings, reach = self._check(["thing/one.cpp"], {"THING"}, {"thing/one.cpp": "why"})
        self.assertEqual(findings, [])
        self.assertEqual(reach["residue_allowed"], 1)

    def test_an_entry_whose_gate_is_off_but_is_unlinked_is_retired(self) -> None:
        findings, _ = self._check([], {"THING"}, {"thing/one.cpp": "why"})
        self.assertEqual(len(findings), 1)
        self.assertIn("excuses nothing", findings[0])

    def test_an_entry_is_not_retired_where_its_gate_is_on(self) -> None:
        # The parity allowlist's rule: an entry in front of a check that never
        # ran was never consulted, so an unused one here says nothing about it.
        findings, reach = self._check(["thing/one.cpp"], set(), {"thing/one.cpp": "why"})
        self.assertEqual(findings, [])
        self.assertEqual(reach["residue_applicable"], 0)


class ReachTest(unittest.TestCase):
    def test_a_run_that_resolved_nothing_reports_zero_rather_than_clean(self) -> None:
        with (
            _with_cmake("if(BUILD_THING)\n  thing/one.cpp\n  thing/two.cpp\nendif()\n"),
            mock.patch.object(gate_scope, "module_objects", return_value=[]),
            mock.patch.object(gate_scope, "disabled_gates", return_value={"THING"}),
        ):
            _, reach = gate_scope.check(Path("/b"))
        # main() turns this into a failure; the point here is that the number is
        # carried out at all, so "found nothing" cannot read as "looked at
        # nothing" from the outside.
        self.assertEqual(reach["sources_resolved"], 0)


if __name__ == "__main__":
    unittest.main()
