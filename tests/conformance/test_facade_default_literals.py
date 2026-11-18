"""Stdlib self-tests for the facade default-literal check.

Each case builds a small tree with one core constant and two facade sites,
confirms the clean tree passes, then plants one defect and requires the check
to name it.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_facade_default_literals",
    Path(__file__).resolve().parent / "check_facade_default_literals.py",
)
assert _SPEC and _SPEC.loader
check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check)

_FILES = {
    "core.h": "inline constexpr float kThingDefault = 2.0f;\n",
    "a.ts": "  return f(r.gain ?? 2,\n",
    "b.py": "def f(\n    gain: float = 2.0,\n):\n",
}

_ROW = check.Literal(
    constant="kThingDefault",
    header="core.h",
    sites=(
        check.Site("node", "a.ts", r"\br\.gain \?\? ([-\d.]+)", 1),
        check.Site("python", "b.py", r"^\s+gain: float = ([-\d.]+),", 1),
    ),
)


class FacadeDefaultLiteralTests(unittest.TestCase):
    def run_with(self, rel: str = "", old: str = "", new: str = "") -> tuple[list[str], int, int]:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for name, text in _FILES.items():
                if name == rel:
                    self.assertIn(old, text)
                    text = text.replace(old, new)
                (root / name).write_text(text)
            return check.evaluate(root, (_ROW,))

    def test_clean_tree_passes(self) -> None:
        self.assertEqual(self.run_with(), ([], 1, 2))

    def test_a_drifted_literal_is_named(self) -> None:
        problems, _, _ = self.run_with("b.py", "2.0", "1.0")
        self.assertEqual(
            problems, ["kThingDefault: python b.py: literal 1.0 differs from the core's 2"]
        )

    def test_a_moved_core_value_is_named_at_every_site(self) -> None:
        problems, _, _ = self.run_with("core.h", "2.0f", "1.5f")
        self.assertEqual(len(problems), 2)

    def test_wording_that_stops_matching_fails(self) -> None:
        problems, _, matched = self.run_with("a.ts", "r.gain ?? 2", "r.gain || 2")
        self.assertEqual(problems, ["kThingDefault: node a.ts: 0 site(s) match, 1 expected"])
        self.assertEqual(matched, 1)

    def test_an_unlisted_site_in_the_tree_fails_the_count(self) -> None:
        tree_row = _ROW._replace(sites=(check.Site("node", "src", r"\bgain \?\? ([-\d.]+)", 1),))
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "src/sub").mkdir(parents=True)
            (root / "core.h").write_text(_FILES["core.h"])
            (root / "src/a.ts").write_text(_FILES["a.ts"])
            self.assertEqual(check.evaluate(root, (tree_row,)), ([], 1, 1))
            (root / "src/sub/b.ts").write_text("  g(o.gain ?? 2);\n")
            problems, _, _ = check.evaluate(root, (tree_row,))
        self.assertEqual(problems, ["kThingDefault: node src: 2 site(s) match, 1 expected"])

    def test_a_missing_constant_is_named(self) -> None:
        problems, _, matched = self.run_with("core.h", "kThingDefault", "kOther")
        self.assertEqual(problems, ["kThingDefault: not found in core.h"])
        self.assertEqual(matched, 0)

    def test_the_shipped_rows_are_not_empty(self) -> None:
        self.assertGreater(len(check.ROWS), 0)


if __name__ == "__main__":
    unittest.main()
