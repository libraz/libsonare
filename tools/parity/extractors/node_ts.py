"""Extract the Node binding surface (bindings/node/src/index.ts + generated)."""

from __future__ import annotations

from pathlib import Path

from model import Extraction

from .ts_common import extract_ts
from .ts_records import extract_records


def extract(root: Path) -> Extraction:
    ex = extract_ts(
        root,
        surface="node",
        index_rel="bindings/node/src/index.ts",
        generated_glob="bindings/node/src/generated",
    )
    extract_records(root, "node", "bindings/node/src", ex)
    return ex
