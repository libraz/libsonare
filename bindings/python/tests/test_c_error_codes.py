"""Python's ErrorCode names every C failure code the cross-surface corpus lists."""

from __future__ import annotations

import json
import re
from pathlib import Path

import libsonare

CORPUS = Path(__file__).resolve().parents[3] / "tests/conformance/c_error_codes.json"


def test_every_corpus_code_is_an_error_code_under_its_name() -> None:
    rows = json.loads(CORPUS.read_text(encoding="utf-8"))["codes"]
    assert len(rows) >= 12
    for row in rows:
        member = libsonare.ErrorCode(row["code"])
        assert member.name == re.sub(r"(?<!^)(?=[A-Z])", "_", row["codeName"]).upper()
