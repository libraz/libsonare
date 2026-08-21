#!/usr/bin/env python3
"""Stdlib self-tests for the WASM exception-scope checker.

The checker's own failure mode is a false clean: every defect it has had made it
report fewer catching units than the build contains.  These tests therefore
pin the three ways a catch can hide -- in a sibling static library, behind a
header, behind a macro -- plus the two ways the checker must not overreact: a
catch written in prose, and a configuration that legitimately catches nowhere.
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

    def _tree(self, root: Path, *, with_catches: bool) -> Path:
        src = root / "src"
        build = root / "build"
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
                entries.append(
                    {
                        "directory": str(bindir),
                        "command": f"em++ -O3 -o CMakeFiles/{target}.dir/{name}.o -c {src / name}",
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

    def test_sibling_archive_and_header_reachable_catches_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self._tree(root, with_catches=True)
            covered, uncovered, unanalysable, linked = self._audit(root, build)

            self.assertEqual(linked, 3, "the archive member must be followed")
            self.assertEqual(covered, [])
            self.assertEqual(unanalysable, [])
            names = "\n".join(uncovered)
            # A member of a sibling static library, reached through the archive.
            self.assertIn("lib: lib_unit.cpp", names)
            # A unit whose only catch lives in an included header.
            self.assertIn("mod: module_unit.cpp", names)
            self.assertIn("inline_catch.h", names)
            # Prose is not a catch.
            self.assertNotIn("comment_unit.cpp", names)

    def test_no_catching_unit_is_a_pass_not_a_broken_database(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self._tree(root, with_catches=False)
            covered, uncovered, unanalysable, linked = self._audit(root, build)
            self.assertEqual((covered, uncovered, unanalysable), ([], [], []))
            self.assertEqual(linked, 3)
            argv = ["check_wasm_exception_scope.py", "--build-dir", str(build)]
            with self._rooted(root), mock.patch("sys.argv", argv):
                with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
                    self.assertEqual(CHECKER.main(), 0)

    def test_missing_dependency_file_is_unanalysable_not_clean(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self._tree(root, with_catches=True)
            (build / "sub/CMakeFiles/lib.dir/lib_unit.cpp.o.d").unlink()
            _, _, unanalysable, _ = self._audit(root, build)
            self.assertEqual(len(unanalysable), 1)
            self.assertIn("no dependency file", unanalysable[0])

    def test_fexceptions_moves_a_unit_from_uncovered_to_covered(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self._tree(root, with_catches=True)
            database = build / "compile_commands.json"
            entries = json.loads(database.read_text())
            for entry in entries:
                entry["command"] += " -fexceptions"
            database.write_text(json.dumps(entries))
            covered, uncovered, _, _ = self._audit(root, build)
            self.assertEqual(uncovered, [])
            self.assertEqual(len(covered), 2)


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
        covered, uncovered, cls.unanalysable, cls.linked = CHECKER.audit(REAL_BUILD)
        cls.catching = "\n".join(covered + uncovered)

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


if __name__ == "__main__":
    unittest.main()
