"""Regression tests for scalar reader classification in the WASM source scan."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_wasm_narrowing_scope.py"
SPEC = importlib.util.spec_from_file_location("wasm_narrowing_checker", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class PointerReturnClassificationTest(unittest.TestCase):
    def test_pointer_return_functions_are_not_numeric_readers_but_casts_are_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            wasm = Path(temporary) / "src" / "wasm"
            source = wasm / "bindings" / "domain" / "readers.cpp"
            source.parent.mkdir(parents=True)
            source.write_text(
                """
#include <emscripten/val.h>
using emscripten::val;

const float* floatPointer(val value) {
  const int checked = value.as<int>();
  return nullptr;
}
int* integerPointer(val value) { return nullptr; }
const uint32_t* unsignedPointer(val value) { return nullptr; }
int storage;
int& referenceReader(val value) { return storage; }
const float constFloatReader(val value) { return value.as<float>(); }
constexpr int constexprScalarReader(const val& value) { return value.as<int>(); }
int scalarReader(val value) { return value.as<int>(); }
float floatReader(val value) { return value.as<float>(); }
""",
                encoding="utf-8",
            )
            scan = CHECKER.Scan(wasm)

        self.assertEqual(
            sorted(reader.name for reader in scan.readers),
            ["constFloatReader", "constexprScalarReader", "floatReader", "scalarReader"],
        )
        container_returns = {container.name: container.returns for container in scan.containers}
        self.assertEqual(
            {
                name: container_returns[name]
                for name in ("floatPointer", "integerPointer", "unsignedPointer")
            },
            {"floatPointer": "float*", "integerPointer": "int*", "unsignedPointer": "uint32_t*"},
        )
        self.assertEqual(container_returns["referenceReader"], "int&")
        pointer_cast = [
            site
            for site in scan.sites
            if site.container is not None and site.container.name == "floatPointer"
        ]
        self.assertEqual(len(pointer_cast), 1)
        self.assertEqual(scan.orphans, [])
        self.assertEqual(scan.reconcile(), [])


if __name__ == "__main__":
    unittest.main()
