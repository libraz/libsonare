"""Extract the WASM binding surface (bindings/wasm/src/index.ts + generated)."""

from __future__ import annotations

from pathlib import Path

from model import Extraction

from .ts_common import extract_ts


def extract(root: Path) -> Extraction:
    return extract_ts(
        root,
        surface="wasm",
        index_rel="bindings/wasm/src/index.ts",
        generated_glob="bindings/wasm/src/generated",
    )
