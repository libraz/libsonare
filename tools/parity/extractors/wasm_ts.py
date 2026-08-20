"""Extract the WASM binding surface (bindings/wasm/src/index.ts + generated)."""

from __future__ import annotations

from pathlib import Path

from model import Extraction

from .ts_common import extract_ts
from .ts_records import extract_records


def extract(root: Path) -> Extraction:
    ex = extract_ts(
        root,
        surface="wasm",
        index_rel="bindings/wasm/src/index.ts",
        generated_glob="bindings/wasm/src/generated",
    )
    extract_records(root, "wasm", "bindings/wasm/src", ex)
    return ex
