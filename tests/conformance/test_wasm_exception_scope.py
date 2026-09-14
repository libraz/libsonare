#!/usr/bin/env python3
"""Stdlib self-tests for the WASM exception-scope checker.

The checker's own failure mode is a false clean: every defect it has had made it
report fewer catching units than the build contains.  These tests therefore
pin the three ways a catch can hide -- in a sibling static library, behind a
header, behind a macro -- plus the two ways the checker must not overreact: a
catch written in prose, and a configuration that legitimately catches nowhere.

The two directions are exercised separately, and deliberately so.  A fixture
that reddens both at once cannot tell "both directions hold" from "one direction
fires twice", so each direction has a fixture where the other stays empty.
"""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_wasm_exception_scope.py"
SPEC = importlib.util.spec_from_file_location(
    "libsonare_wasm_exception_scope_checker", CHECKER_PATH
)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)

REAL_BUILD = ROOT / "bindings" / "wasm" / "build-wasm"
REAL_ANALYSIS_BUILD = ROOT / "bindings" / "wasm" / "build-wasm-analysis"

# The units SONARE_WASM_EXCEPTION_SOURCES flags whose only catching header the
# analysis-only gate removes from the closure.  Dead weight in that build, and
# load-bearing in the full one -- which is the whole reason the rule is a union.
GATED_IN_ANALYSIS_BUILD = frozenset(
    {
        "wasm/bindings.cpp",
        "wasm/bindings/common/common.cpp",
        "wasm/bindings/analysis/quick.cpp",
        "wasm/bindings/analysis/quick_detailed.cpp",
        "wasm/bindings/features/core.cpp",
        "wasm/bindings/features/music.cpp",
        "wasm/bindings/features/pitch.cpp",
        "wasm/bindings/features/spectral.cpp",
        "wasm/bindings/features/spectrogram.cpp",
        "wasm/bindings/metering/metering.cpp",
    }
)


class LexicalTest(unittest.TestCase):
    def test_prose_about_catching_is_not_a_catch(self) -> None:
        source = (
            "// relying on the caller to catch (as the render path does).\nint f();\n"
        )
        self.assertFalse(CHECKER.FileScan(source).direct_catch)

    def test_literal_mentioning_catch_is_not_a_catch(self) -> None:
        source = 'const char* msg = "catch (const Error&)";\n'
        self.assertFalse(CHECKER.FileScan(source).direct_catch)

    def test_raw_literal_mentioning_catch_is_not_a_catch(self) -> None:
        source = 'auto s = R"json({"hint": "catch (x)"})json";\n'
        self.assertFalse(CHECKER.FileScan(source).direct_catch)

    def test_block_comment_spanning_lines_is_not_a_catch(self) -> None:
        source = "/* one\n * catch (...) in a sketch\n */\nint f();\n"
        self.assertFalse(CHECKER.FileScan(source).direct_catch)

    def test_real_catch_still_matches(self) -> None:
        source = "void f() {\n  try { g(); } catch (const std::exception&) {}\n}\n"
        self.assertTrue(CHECKER.FileScan(source).direct_catch)


class MacroTest(unittest.TestCase):
    def test_definition_is_not_a_catch_but_its_name_is(self) -> None:
        definition = CHECKER.FileScan(
            "#define GUARD_CATCH   \\\n  } catch (...) { return 1; }\n"
        )
        # The definition alone emits no landing pad, so the defining header must
        # not be reported as catching.
        self.assertFalse(definition.direct_catch)
        self.assertEqual(CHECKER.catching_macros([definition]), {"GUARD_CATCH"})

    def test_macro_in_macro_use_is_followed(self) -> None:
        scan = CHECKER.FileScan(
            "#define GUARD_CATCH } catch (...) { return 1; }\n"
            "#define OUTER_GUARD GUARD_CATCH\n"
        )
        self.assertEqual(
            CHECKER.catching_macros([scan]), {"GUARD_CATCH", "OUTER_GUARD"}
        )


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class SyntheticBuildTest(unittest.TestCase):
    """Drives ``audit`` over a hand-built tree shaped like the real one."""

    def _tree(
        self,
        root: Path,
        *,
        with_catches: bool,
        flagged: tuple[str, ...] = (),
        build_name: str = "build",
    ) -> Path:
        """Write one configuration's build tree over the shared ``src``.

        ``flagged`` names the sources compiled with ``-fexceptions``, and
        ``build_name`` lets a second configuration sit beside the first: the two
        share one source tree and differ only in the header closures their
        ``.d`` files record, which is how a feature gate shows up here.
        """
        src = root / "src"
        build = root / build_name
        bindir = build / "sub"
        objdir = bindir / "CMakeFiles"

        _write(
            src / "guard.h", "#define GUARD_CATCH \\\n  } catch (...) { return 1; }\n"
        )
        _write(
            src / "inline_catch.h",
            "inline int f() noexcept {\n"
            "  try { return g(); } catch (...) { return 0; }\n"
            "}\n",
        )
        _write(src / "plain.h", "int g();\n")
        _write(
            src / "comment_unit.cpp",
            '#include "plain.h"\n// left to the caller to catch (as elsewhere).\nint m() { return g(); }\n',
        )
        if with_catches:
            # The macro is spelled here, not in the header that defines it.
            _write(
                src / "lib_unit.cpp",
                '#include "guard.h"\nint h() { try { return 1; GUARD_CATCH }\n',
            )
            # The catch is only in the header, never in this unit's own text.
            _write(
                src / "module_unit.cpp",
                '#include "inline_catch.h"\nint k() { return f(); }\n',
            )
            closure = {
                "lib_unit.cpp": ["guard.h"],
                "module_unit.cpp": ["inline_catch.h"],
                "comment_unit.cpp": ["plain.h"],
            }
        else:
            _write(src / "lib_unit.cpp", '#include "guard.h"\nint h() { return 1; }\n')
            _write(
                src / "module_unit.cpp", '#include "plain.h"\nint k() { return g(); }\n'
            )
            closure = {
                "lib_unit.cpp": ["guard.h"],
                "module_unit.cpp": ["plain.h"],
                "comment_unit.cpp": ["plain.h"],
            }

        units = {
            "mod": ["module_unit.cpp", "comment_unit.cpp"],
            "lib": ["lib_unit.cpp"],
        }
        entries = []
        for target, sources in units.items():
            for name in sources:
                obj = f"sub/CMakeFiles/{target}.dir/{name}.o"
                flag = " -fexceptions" if name in flagged else ""
                entries.append(
                    {
                        "directory": str(bindir),
                        "command": f"em++ -O3{flag} -o CMakeFiles/{target}.dir/{name}.o -c {src / name}",
                        "file": str(src / name),
                        "output": obj,
                    }
                )
                deps = " \\\n  ".join(str(src / h) for h in closure[name])
                _write(build / f"{obj}.d", f"{obj}: \\\n  {src / name} \\\n  {deps}\n")
        _write(build / "compile_commands.json", json.dumps(entries))
        _write(
            objdir / "mod.dir" / "link.txt",
            "em++ -O3 --bind -o ../bin/module.js "
            "CMakeFiles/mod.dir/module_unit.cpp.o "
            "CMakeFiles/mod.dir/comment_unit.cpp.o ../lib/liblib.a\n",
        )
        _write(
            objdir / "lib.dir" / "link.txt",
            "emar qc ../lib/liblib.a CMakeFiles/lib.dir/lib_unit.cpp.o\n"
            "emranlib ../lib/liblib.a\n",
        )
        return build

    def _rooted(self, root: Path):
        """Re-point the checker's repo-owned trees at the synthetic tree."""
        return mock.patch.multiple(CHECKER, ROOT=root, _OWNED_TREES=(root / "src",))

    def _audit(self, root: Path, build: Path):
        with self._rooted(root):
            return CHECKER.audit(build)

    def _main(self, root: Path, *builds: Path) -> tuple[int, str, str]:
        argv = ["check_wasm_exception_scope.py"]
        for build in builds:
            argv += ["--build-dir", str(build)]
        out, err = StringIO(), StringIO()
        with self._rooted(root), mock.patch("sys.argv", argv):
            with redirect_stdout(out), redirect_stderr(err):
                status = CHECKER.main()
        return status, out.getvalue(), err.getvalue()

    def test_sibling_archive_and_header_reachable_catches_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self._tree(root, with_catches=True)
            result = self._audit(root, build)

            self.assertEqual(result.linked, 3, "the archive member must be followed")
            self.assertEqual(result.covered, [])
            self.assertEqual(result.unanalysable, [])
            names = "\n".join(result.uncovered)
            # A member of a sibling static library, reached through the archive.
            self.assertIn("lib: lib_unit.cpp", names)
            # A unit whose only catch lives in an included header.
            self.assertIn("mod: module_unit.cpp", names)
            self.assertIn("inline_catch.h", names)
            # Prose is not a catch.
            self.assertNotIn("comment_unit.cpp", names)
            # Only the catch-without-flag direction fires: no unit carries a
            # flag here, so a report from this fixture cannot be the other one.
            self.assertEqual(result.idle, [])

    def test_dead_flag_is_reported_with_no_catch_without_flag(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # Both catching units carry the flag, so the only thing left to
            # report is the flagged unit that catches nowhere.
            build = self._tree(
                root,
                with_catches=True,
                flagged=("lib_unit.cpp", "module_unit.cpp", "comment_unit.cpp"),
            )
            result = self._audit(root, build)

            self.assertEqual(result.uncovered, [])
            self.assertEqual(result.unanalysable, [])
            self.assertEqual(len(result.covered), 2)
            self.assertEqual(result.idle, ["mod: comment_unit.cpp"])

    def test_no_catching_unit_is_a_pass_not_a_broken_database(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self._tree(root, with_catches=False)
            result = self._audit(root, build)
            self.assertEqual(
                (result.covered, result.uncovered, result.unanalysable, result.idle),
                ([], [], [], []),
            )
            self.assertEqual(result.linked, 3)
            self.assertEqual(self._main(root, build)[0], 0)

    def test_missing_dependency_file_is_unanalysable_not_clean(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self._tree(root, with_catches=True)
            (build / "sub/CMakeFiles/lib.dir/lib_unit.cpp.o.d").unlink()
            unanalysable = self._audit(root, build).unanalysable
            self.assertEqual(len(unanalysable), 1)
            self.assertIn("no dependency file", unanalysable[0])

    def test_fexceptions_moves_a_unit_from_uncovered_to_covered(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self._tree(
                root, with_catches=True, flagged=("lib_unit.cpp", "module_unit.cpp")
            )
            result = self._audit(root, build)
            self.assertEqual(result.uncovered, [])
            self.assertEqual(len(result.covered), 2)
            self.assertEqual(result.idle, [])

    def _two_configurations(
        self, root: Path, *, catches_in_b: bool
    ) -> tuple[Path, Path]:
        """Two build trees over one source tree, differing only by gate.

        ``module_unit.cpp`` carries the flag in both.  Its catching header is in
        its closure only when ``catches_in_b``, which is what a feature gate
        does to reachability without touching the source list.  ``lib_unit.cpp``
        is flagged only when it catches, so the catch-without-flag direction
        stays silent and whatever is reported comes from this one.
        """
        flagged = (
            ("lib_unit.cpp", "module_unit.cpp")
            if catches_in_b
            else ("module_unit.cpp",)
        )
        build_a = self._tree(
            root, with_catches=False, flagged=flagged, build_name="build-a"
        )
        build_b = self._tree(
            root, with_catches=catches_in_b, flagged=flagged, build_name="build-b"
        )
        return build_a, build_b

    def test_flag_idle_in_one_configuration_only_is_a_note_not_a_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_a, build_b = self._two_configurations(root, catches_in_b=True)
            self.assertEqual(self._audit(root, build_a).idle, ["mod: module_unit.cpp"])
            self.assertEqual(self._audit(root, build_b).idle, [])

            status, out, err = self._main(root, build_a, build_b)
            self.assertEqual(status, 0)
            self.assertIn("catch nowhere in some", out)
            self.assertIn("mod: module_unit.cpp", out)
            self.assertNotIn("module_unit.cpp", err)

    def test_flag_idle_in_every_configuration_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_a, build_b = self._two_configurations(root, catches_in_b=False)
            for build in (build_a, build_b):
                self.assertEqual(
                    self._audit(root, build).idle, ["mod: module_unit.cpp"]
                )

            status, out, err = self._main(root, build_a, build_b)
            self.assertEqual(status, 1)
            self.assertIn("mod: module_unit.cpp", err)
            self.assertIn("SONARE_WASM_EXCEPTION_SOURCES", err)
            self.assertNotIn("catch nowhere in some", out)

    def test_single_configuration_never_fails_on_an_idle_flag(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self._tree(
                root, with_catches=False, flagged=("module_unit.cpp",), build_name="one"
            )
            self.assertEqual(self._audit(root, build).idle, ["mod: module_unit.cpp"])

            status, out, err = self._main(root, build)
            self.assertEqual(status, 0)
            self.assertIn("mod: module_unit.cpp", out)
            self.assertIn("other configurations were not consulted", out)
            self.assertNotIn("module_unit.cpp", err)


@unittest.skipUnless(
    (REAL_BUILD / "compile_commands.json").is_file(),
    "no WASM build tree; run (cd bindings/wasm && yarn build:wasm)",
)
class RealBuildTest(unittest.TestCase):
    """Non-vacuity against the shipped build, phrased to survive the CMake fix.

    Membership of ``uncovered`` is deliberately not asserted -- that set is meant
    to empty out.  What must hold either way is that these units are *classified*
    at all, and that the prose-only unit never is.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.result = CHECKER.audit(REAL_BUILD)
        cls.unanalysable = cls.result.unanalysable
        cls.linked = cls.result.linked
        cls.catching = "\n".join(cls.result.covered + cls.result.uncovered)

    def test_sibling_library_units_are_in_scope(self) -> None:
        self.assertGreater(self.linked, 100)
        self.assertIn("engine/realtime_engine_midi.cpp", self.catching)
        self.assertIn("core/convert.cpp", self.catching)

    def test_header_reachable_catch_is_seen(self) -> None:
        self.assertIn("wasm/bindings/realtime/midi.cpp", self.catching)

    def test_prose_only_unit_is_not_flagged(self) -> None:
        self.assertNotIn("mixing/surround_panner.cpp", self.catching)

    def test_every_linked_unit_is_analysable(self) -> None:
        self.assertEqual(self.unanalysable, [])

    def test_no_flag_is_idle_in_the_full_build(self) -> None:
        self.assertEqual(self.result.idle, [])


@unittest.skipUnless(
    (REAL_ANALYSIS_BUILD / "compile_commands.json").is_file(),
    "no analysis-only WASM build tree; run (cd bindings/wasm && yarn build:wasm:analysis)",
)
class RealAnalysisBuildTest(unittest.TestCase):
    """The configuration that makes the union rule necessary.

    These units catch in the full build and reach no catch here, so judging the
    source list against this configuration alone would demand deleting entries
    the full build needs.  Membership is asserted as a subset so the list can
    legitimately grow or shrink around them.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.result = CHECKER.audit(REAL_ANALYSIS_BUILD)

    def test_the_gated_units_carry_an_idle_flag_here(self) -> None:
        sources = {entry.split(": ", 1)[-1] for entry in self.result.idle}
        self.assertLessEqual(GATED_IN_ANALYSIS_BUILD, sources)

    def test_nothing_catches_without_the_flag_here(self) -> None:
        self.assertEqual(self.result.uncovered, [])

    def test_every_linked_unit_is_analysable(self) -> None:
        self.assertEqual(self.result.unanalysable, [])


if __name__ == "__main__":
    unittest.main()
