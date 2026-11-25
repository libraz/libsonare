"""Self-test for ts_exports.cjs, the TypeScript export extractor.

Run from the repository root: ``python3 -m unittest tools/api/test_ts_exports.py``.
Runs node over a fixture under tools/api/testdata/ts with the WASM package's
TypeScript; skipped when either is absent.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import unittest
from functools import cache
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PKG = ROOT / "bindings" / "wasm"
FIXTURE = HERE / "testdata" / "ts" / "optional_method.ts"
NESTED = HERE / "testdata" / "ts" / "nested_object.ts"
PLATFORM = HERE / "testdata" / "ts" / "platform_mirror.ts"


def _entry(fixture: Path) -> str:
    return Path("..", "..", "..", fixture.relative_to(ROOT)).as_posix()


@cache
def _extract_fixtures() -> dict[str, list[dict]]:
    entries = [_entry(fixture) for fixture in (FIXTURE, NESTED, PLATFORM)]
    proc = subprocess.run(
        ["node", str(HERE / "ts_exports.cjs"), str(PKG), *entries],
        capture_output=True,
        text=True,
        check=True,
    )
    rows = json.loads(proc.stdout)
    return {entry: [row for row in rows if row["entry"] == entry] for entry in entries}


def _extract(fixture: Path) -> list[dict]:
    return _extract_fixtures()[_entry(fixture)]


@unittest.skipUnless(
    shutil.which("node") and (PKG / "node_modules" / "typescript").is_dir(),
    "node or the WASM package's typescript is not installed",
)
class OptionalMethods(unittest.TestCase):
    def test_an_optional_method_keeps_its_parameters(self) -> None:
        rows = {row["name"]: row for row in _extract(FIXTURE)}
        members = {m["name"]: m for m in rows["OptionalMethodHost"]["members"]}
        for name in ("required", "optional"):
            with self.subTest(name):
                params = [p["name"] for sig in members[name]["sigs"] for p in sig["params"]]
                self.assertEqual(params, ["group", "offsetInBlockSamples"])
        self.assertTrue(members["optional"]["optional"])
        self.assertFalse(members["required"]["optional"])


@unittest.skipUnless(
    shutil.which("node") and (PKG / "node_modules" / "typescript").is_dir(),
    "node or the WASM package's typescript is not installed",
)
class InlineObjectMembers(unittest.TestCase):
    def test_an_inline_object_member_is_a_field_under_a_dotted_owner(self) -> None:
        import sys

        sys.path.insert(0, str(HERE))
        import inventory

        rows = _extract(NESTED)
        recs = inventory.ts_records("node", rows, {r["entry"]: "." for r in rows})
        fields = {r["name"]: r for r in recs if r["kind"] == "field"}
        nested = sorted(n for n in fields if n.count(".") > 1)
        self.assertEqual(
            nested,
            [
                "NestedHost.bands.gainDb",
                "NestedHost.loudness.ceilingDb",
                "NestedHost.loudness.inner",
                "NestedHost.loudness.inner.depth",
                "NestedHost.mixed.a",
                "NestedHost.mixed.b",
            ],
        )
        ceiling = fields["NestedHost.loudness.ceilingDb"]
        self.assertEqual(
            (ceiling["owner"], ceiling["member"]), ("NestedHost.loudness", "ceilingDb")
        )
        self.assertIn("dBTP", ceiling["doc_text"])
        self.assertEqual(fields["NestedHost.mixed.a"]["type"], "number | string")


if __name__ == "__main__":
    unittest.main()


@unittest.skipUnless(
    shutil.which("node") and (PKG / "node_modules" / "typescript").is_dir(),
    "node or the WASM package's typescript is not installed",
)
class PlatformMirrors(unittest.TestCase):
    def test_a_tagged_type_lists_only_the_members_the_platform_interface_declares(self) -> None:
        rows = {row["name"]: row for row in _extract(PLATFORM)}
        self.assertEqual(
            rows["AudioClockLike"]["platformMirror"],
            {"interface": "AudioContext", "members": ["outputLatency"]},
        )
        self.assertIsNone(rows["PlainRecord"]["platformMirror"])
