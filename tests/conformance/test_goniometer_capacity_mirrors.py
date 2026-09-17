"""Stdlib self-tests for the goniometer capacity mirror check.

Every case drives :func:`check_goniometer_capacity_mirrors.evaluate` -- the
function the shipping entry point calls -- over a copy of the real files, so a
perturbation is demonstrated rather than asserted about. Each mirror is broken
on its own: a guard that only ever notices one of them is half a guard.

The case that matters most is the missing core constant. A check that locates
nothing and reports success is worse than no check, because it reads as
coverage, so the unlocatable core is required to fail with its own message.
"""

from __future__ import annotations

import importlib.util
import re
import shutil
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_goniometer_capacity_mirrors",
    Path(__file__).resolve().parent / "check_goniometer_capacity_mirrors.py",
)
assert _SPEC and _SPEC.loader
check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check)

NODE, PYTHON = check.MIRRORS
WASM = check.QUALIFIED_USE[0]
TELEMETRY = "src/engine/meter_telemetry.h"

FILES = (check.CORE.path, NODE.path, PYTHON.path, WASM, TELEMETRY)

# A second class in the core header declaring the same name with a different
# value: the qualification, not the file boundary, is what keeps them apart.
SECOND_DECLARATION = """class MeterTelemetryTapFixture {
 public:
  static constexpr size_t kGoniometerCapacity = 512;
};

"""


class _CopiedTree(unittest.TestCase):
    """A throwaway tree holding the real files, optionally with one edit each."""

    def tree(self, edits: dict[str, tuple[str, str]] | None = None) -> Path:
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root)
        for relative in FILES:
            text = (check.ROOT / relative).read_text(encoding="utf-8")
            if edits and relative in edits:
                old, new = edits[relative]
                self.assertIn(old, text, f"{relative}: the fixture anchor is gone")
                text = text.replace(old, new, 1)
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(text, encoding="utf-8")
        return root

    def only(self, failures: list[str], *fragments: str) -> str:
        self.assertEqual(len(failures), 1, f"expected one failure, got: {failures}")
        for fragment in fragments:
            self.assertIn(fragment, failures[0])
        return failures[0]


class ShippingTreeTest(unittest.TestCase):
    def test_the_tree_as_it_stands_passes(self) -> None:
        self.assertEqual(check.evaluate(), [])

    def test_the_core_value_is_read_from_the_declaring_class(self) -> None:
        core = check.find(check.ROOT, check.CORE)
        self.assertIsNotNone(core)
        self.assertEqual((core.scope, core.value), ("ChannelStrip", 4096))
        self.assertEqual(core.path, "src/mixing/channel_strip.h")

    def test_both_mirrors_are_located_and_read(self) -> None:
        for mirror in check.MIRRORS:
            with self.subTest(mirror=mirror.name):
                declaration = check.find(check.ROOT, mirror)
                self.assertIsNotNone(declaration)
                self.assertEqual(declaration.value, 4096)
                self.assertGreaterEqual(check.reads(check.ROOT, mirror, declaration), 1)


class SameNameConstantTest(_CopiedTree):
    """The trap: a second `kGoniometerCapacity`, on another class, holding 512."""

    def test_the_bare_identifier_is_genuinely_ambiguous(self) -> None:
        naive = re.compile(r"\bkGoniometerCapacity\s*=\s*(\d+)")
        values = {
            path: naive.search((check.ROOT / path).read_text(encoding="utf-8")).group(1)
            for path in (check.CORE.path, TELEMETRY)
        }
        self.assertEqual(set(values.values()), {"4096", "512"})

    def test_the_other_declaration_is_reachable_by_its_own_class(self) -> None:
        telemetry = check.Source(TELEMETRY, "kGoniometerCapacity", "c++", scope="MeterTelemetryTap")
        self.assertEqual(check.find(check.ROOT, telemetry).value, 512)

    def test_the_same_name_in_the_core_header_does_not_shadow_the_core(self) -> None:
        """Both declarations in one file, the 512 first, and the core still reads 4096."""
        root = self.tree(
            {
                check.CORE.path: (
                    "class ChannelStrip : public rt::ProcessorBase {",
                    SECOND_DECLARATION + "class ChannelStrip : public rt::ProcessorBase {",
                )
            }
        )
        header = (root / check.CORE.path).read_text(encoding="utf-8")
        same_name = [
            declaration
            for declaration in check.cpp_declarations(header, check.CORE.path)
            if declaration.name == check.CORE.name
        ]
        self.assertEqual(next((d.scope, d.value) for d in same_name), ("MeterTelemetryTapFixture", 512))
        self.assertEqual(len(same_name), 2)
        self.assertEqual(check.find(root, check.CORE).value, 4096)
        self.assertEqual(check.evaluate(root), [])


class PerturbationTest(_CopiedTree):
    def test_an_untouched_copy_passes(self) -> None:
        self.assertEqual(check.evaluate(self.tree()), [])

    def test_moving_the_core_reports_both_mirrors_with_both_values(self) -> None:
        root = self.tree(
            {check.CORE.path: ("kGoniometerCapacity = 4096", "kGoniometerCapacity = 2048")}
        )
        failures = check.evaluate(root)
        self.assertEqual(len(failures), 2, failures)
        for mirror, failure in zip((NODE, PYTHON), failures):
            self.assertIn(mirror.name, failure)
            self.assertIn("is 4096", failure)
            self.assertIn("is 2048", failure)

    def test_moving_the_node_mirror_alone_is_reported(self) -> None:
        root = self.tree(
            {NODE.path: ("kGoniometerReadCap = 4096", "kGoniometerReadCap = 8192")}
        )
        self.only(check.evaluate(root), NODE.name, "is 8192", "is 4096")

    def test_moving_the_python_mirror_alone_is_reported(self) -> None:
        root = self.tree(
            {PYTHON.path: ("_GONIOMETER_READ_CAP = 4096", "_GONIOMETER_READ_CAP = 1024")}
        )
        self.only(check.evaluate(root), PYTHON.name, "is 1024", "is 4096")

    def test_a_renamed_mirror_is_reported(self) -> None:
        root = self.tree(
            {PYTHON.path: ("_GONIOMETER_READ_CAP = 4096", "_GONIOMETER_CAP = 4096")}
        )
        self.only(check.evaluate(root), PYTHON.path, f"no `{PYTHON.name}`")

    def test_a_mirror_that_bounds_nothing_is_reported(self) -> None:
        root = self.tree(
            {PYTHON.path: ("min(max_points, _GONIOMETER_READ_CAP)", "max_points")}
        )
        self.only(check.evaluate(root), PYTHON.name, "never read")

    def test_a_literal_replacing_the_wasm_qualified_read_is_reported(self) -> None:
        root = self.tree(
            {WASM: ("sonare::mixing::ChannelStrip::kGoniometerCapacity", "size_t{4096}")}
        )
        self.only(check.evaluate(root), WASM, check.QUALIFIED_USE[1])


class UnlocatableCoreTest(_CopiedTree):
    """The vacuity case: nothing found has to be a failure, never a quiet pass."""

    def test_a_renamed_core_constant_fails(self) -> None:
        root = self.tree(
            {check.CORE.path: ("kGoniometerCapacity = 4096", "kGoniometerRingPoints = 4096")}
        )
        self.only(check.evaluate(root), check.CORE.path, "no `kGoniometerCapacity`")

    def test_a_deleted_core_constant_fails(self) -> None:
        root = self.tree(
            {check.CORE.path: ("  static constexpr size_t kGoniometerCapacity = 4096;\n", "")}
        )
        self.only(check.evaluate(root), "no `kGoniometerCapacity`", "leaves them unmeasured")

    def test_a_core_constant_moved_off_the_class_fails(self) -> None:
        """Still in the file, still 4096, no longer where the mirrors are anchored."""
        root = self.tree(
            {check.CORE.path: ("class ChannelStrip : public", "class ChannelStripV2 : public")}
        )
        self.only(check.evaluate(root), f"declared by `{check.CORE.scope}`")


if __name__ == "__main__":
    unittest.main()
