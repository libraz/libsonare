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
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PKG = ROOT / "bindings" / "wasm"
FIXTURE = HERE / "testdata" / "ts" / "optional_method.ts"


def _extract(fixture: Path) -> list[dict]:
    entry = Path("..", "..", "..", fixture.relative_to(ROOT)).as_posix()
    proc = subprocess.run(
        ["node", str(HERE / "ts_exports.cjs"), str(PKG), entry],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(proc.stdout)


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


if __name__ == "__main__":
    unittest.main()
