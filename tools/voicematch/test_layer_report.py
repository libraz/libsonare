"""layer_report lists the captures with no declared class and the voices with no on-layer capture.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_layer_report.py -q
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import layer_report
import policy
import refit

AIMED = {
    "reference_layer": {
        "default": {"timbre": "instrument", "behaviour": "instrument"},
        "machine_defined": {"timbre": "machine", "behaviour": "machine", "programs": [55]},
    }
}


def _world(tmp_path, monkeypatch):
    defs = tmp_path / "capture"
    defs.mkdir()
    for name, cfg in {
        "declared": {"id": "declared", "source_class": "library"},
        "bare": {"id": "bare"},
        "mod": {"id": "mod", "source_class": "module"},
    }.items():
        (defs / f"{name}.json").write_text(json.dumps(cfg))
    status = tmp_path / "voice-status.json"
    rows = [
        {"slug": "p000", "program": 0, "bank": 0, "kit": False, "patch": "grand"},
        {"slug": "p001", "program": 1, "bank": 0, "kit": False, "patch": "bright"},
        {"slug": "p055", "program": 55, "bank": 0, "kit": False, "patch": "hit"},
    ]
    status.write_text(json.dumps({"voices": rows}))
    reaching = {"p000": ["declared", "bare"], "p001": ["bare"], "p055": ["declared"]}
    monkeypatch.setattr(layer_report.bank, "CAPTURE_DIR", defs)
    monkeypatch.setattr(refit, "STATUS", status)
    monkeypatch.setattr(refit, "struct_copies", dict)
    monkeypatch.setattr(
        layer_report.bank,
        "captures",
        lambda: [
            argparse.Namespace(id=i, source_class=c)
            for i, c in (("declared", "library"), ("bare", None), ("mod", "module"))
        ],
    )
    monkeypatch.setattr(
        refit,
        "capital_gates",
        lambda row, *a, **k: [{"capture": c} for c in reaching[row["slug"]]],
    )
    monkeypatch.setattr(policy, "load", lambda path=None: AIMED)


def test_both_groups_are_listed(tmp_path, monkeypatch):
    _world(tmp_path, monkeypatch)
    report = layer_report.build()
    assert report["undeclared_count"] == 1
    assert report["undeclared_captures"] == [{"capture": "bare", "definition": "bare.json"}]
    # p000 has a library capture on its wanted layer; p001 has only the unclassified one; the
    # library capture answering a machine-defined slot is off-layer.
    assert [v["slug"] for v in report["voices_without_on_layer"]] == ["p001", "p055"]
    assert report["no_on_layer_count"] == 2
    p001 = report["voices_without_on_layer"][0]
    assert p001["wanted"] == "instrument"
    assert p001["captures"] == [{"capture": "bare", "source_class": None, "layer": "off-layer"}]


def test_the_text_names_both_groups(tmp_path, monkeypatch):
    _world(tmp_path, monkeypatch)
    out = layer_report.text(layer_report.build())
    assert "captures with no source_class declared: 1" in out and "bare (bare.json)" in out
    assert "voices with no on-layer capture: 2" in out
    assert "p055 (patch hit): wanted machine, have declared=library" in out
    assert "p001 (patch bright): wanted instrument, have bare=unclassified" in out
