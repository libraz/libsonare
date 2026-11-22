"""The error-code corpus covers every C failure code, and the core never raises a passthrough one.

``AbiMismatch`` and ``Unknown`` exist in ``sonare::ErrorCode`` only so a C code keeps its name
when a binding or the CLI rethrows it; a core throw site naming either would publish a failure
class no core condition stands for.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "tests/conformance/c_error_codes.json"
HEADER = ROOT / "include/sonare/sonare_c_types_enums.h"
PASSTHROUGH = re.compile(r"ErrorCode::(AbiMismatch|Unknown)\b")
# The enum itself, the C mapping both ways, and the WASM code-name table.
ALLOWED = {
    "src/util/types.h",
    "src/c_api/sonare_c_error_mapping.h",
    "src/wasm/bindings.cpp",
}


class CErrorCodes(unittest.TestCase):
    def test_the_corpus_lists_every_c_failure_code(self) -> None:
        enum = HEADER.read_text(encoding="utf-8")
        declared = {
            int(value) for name, value in re.findall(r"\b(SONARE_ERROR_\w+)\s*=\s*(\d+)", enum)
        }
        rows = json.loads(CORPUS.read_text(encoding="utf-8"))["codes"]
        self.assertGreaterEqual(len(declared), 12)
        self.assertEqual({row["code"] for row in rows}, declared)
        self.assertEqual(len({row["codeName"] for row in rows}), len(rows))

    def test_no_core_source_names_a_passthrough_code(self) -> None:
        scanned = 0
        found = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".h", ".hpp", ".cpp"}:
                continue
            scanned += 1
            relative = path.relative_to(ROOT).as_posix()
            if relative in ALLOWED:
                continue
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if PASSTHROUGH.search(line):
                    found.append(f"{relative}:{number}: {line.strip()}")
        self.assertGreater(scanned, 500)
        self.assertEqual(found, [])


if __name__ == "__main__":
    unittest.main()
