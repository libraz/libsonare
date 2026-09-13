"""Self-tests for the binding-layer warning contract check.

The failure classes are asserted against synthetic compilation databases rather
than against the tree, so each one can be tripped on its own: a check that only
ever ran against a green tree would agree with itself.
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_binding_warning_flags as checker  # noqa: E402


def _database(tmp: Path, layer: checker.Layer, entries: list[tuple[str, str]]) -> Path:
    """Write a compilation database for `layer` under a scratch root.

    `entries` are (repository-relative file, command) pairs.
    """
    build = tmp / layer.build_dir
    build.mkdir(parents=True, exist_ok=True)
    (build / "compile_commands.json").write_text(
        json.dumps(
            [
                {"directory": str(build), "file": str(tmp / name), "command": command}
                for name, command in entries
            ]
        ),
        encoding="utf-8",
    )
    return build


class BindingWarningFlagsTest(unittest.TestCase):
    LAYER = checker.LAYERS[1]  # the addon: one source root, the smaller floor

    def setUp(self) -> None:
        import tempfile

        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name).resolve()
        self.addCleanup(self._tmp.cleanup)

    # Built from the checker's own flag list rather than spelled out, so adding
    # a flag to the contract cannot leave these fixtures describing the old one.
    AT_BAR = "clang++ " + " ".join(checker.REQUIRED_FLAGS)
    WITHOUT_WERROR = "clang++ " + " ".join(
        flag for flag in checker.REQUIRED_FLAGS if flag != "-Werror"
    )

    def _entries(self, count: int, command: str) -> list[tuple[str, str]]:
        return [(f"{self.LAYER.sources[0]}unit{i}.cc", command) for i in range(count)]

    def test_a_unit_missing_the_failing_flag_is_named(self) -> None:
        entries = self._entries(self.LAYER.floor, self.AT_BAR)
        entries.append((f"{self.LAYER.sources[0]}loud.cc", self.WITHOUT_WERROR))
        _database(self.root, self.LAYER, entries)

        covered, uncovered = checker.audit(self.LAYER, self.root)
        self.assertEqual(len(covered), self.LAYER.floor)
        self.assertEqual(len(uncovered), 1)
        self.assertIn("loud.cc", uncovered[0])
        self.assertIn("-Werror", uncovered[0])
        failures = checker.evaluate(self.LAYER, covered, uncovered)
        self.assertEqual(len(failures), 1)
        self.assertIn("warning contract", failures[0][0])

    def test_a_layer_entirely_at_the_bar_reports_nothing(self) -> None:
        # The control. Without it the case above passes against a check that
        # reports every unit.
        _database(
            self.root, self.LAYER, self._entries(self.LAYER.floor, self.AT_BAR)
        )
        covered, uncovered = checker.audit(self.LAYER, self.root)
        self.assertEqual(uncovered, [])
        self.assertEqual(checker.evaluate(self.LAYER, covered, uncovered), [])

    def test_a_scan_that_found_almost_nothing_fails_even_when_clean(self) -> None:
        """The vacuity guard: an empty set carries every flag."""
        _database(self.root, self.LAYER, self._entries(1, self.AT_BAR))
        covered, uncovered = checker.audit(self.LAYER, self.root)
        self.assertEqual(uncovered, [])
        failures = checker.evaluate(self.LAYER, covered, uncovered)
        self.assertEqual(len(failures), 1)
        self.assertIn("stopped reading", failures[0][0])

    def test_third_party_units_are_excluded_by_path(self) -> None:
        entries = self._entries(self.LAYER.floor, self.AT_BAR)
        entries.append((f"{self.LAYER.sources[0]}third_party/vendor.c", "clang++ -c vendor.c"))
        _database(self.root, self.LAYER, entries)
        covered, uncovered = checker.audit(self.LAYER, self.root)
        self.assertEqual(uncovered, [])
        self.assertFalse(any("vendor.c" in name for name in covered))

    def test_a_unit_outside_the_layer_is_not_claimed(self) -> None:
        # A database also lists the core libraries this layer links; the addon
        # entry must not report them as its own, or its floor stops meaning
        # what it says.
        entries = self._entries(self.LAYER.floor, self.AT_BAR)
        entries.append(("src/core/audio.cpp", "clang++ -c audio.cpp"))
        _database(self.root, self.LAYER, entries)
        covered, uncovered = checker.audit(self.LAYER, self.root)
        self.assertEqual(uncovered, [])
        self.assertFalse(any("src/core" in name for name in covered))

    def test_an_unbuilt_layer_is_an_unanswered_question(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            checker.audit(self.LAYER, self.root)
        self.assertIn(self.LAYER.rebuild, str(raised.exception))


class RealTreeTest(unittest.TestCase):
    def test_every_built_layer_is_at_the_bar(self) -> None:
        for layer in checker.LAYERS:
            with self.subTest(layer=layer.name):
                if not (checker.ROOT / layer.build_dir / "compile_commands.json").is_file():
                    self.skipTest(f"{layer.name} is not built in this tree")
                covered, uncovered = checker.audit(layer)
                self.assertEqual(uncovered, [])
                self.assertGreaterEqual(len(covered), layer.floor)


if __name__ == "__main__":
    unittest.main()
