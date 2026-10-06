"""The reference archive has one reader, and it says which archive a reference came from.

`make_audition.py` adopts only v2 entries whose request and source match; the
measurement and readiness readers here also read v1 entries, flagged
historical, because a v1 reference may still be measured but never stands as a
verified one.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_render_evidence.py -q
"""

from __future__ import annotations

import json
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import make_audition
import profile_status
import profile_take
import render_evidence
from phrases import build_takes
from wavio import write_wav

SR = 48000


def test_concurrent_generation_publication_is_idempotent(tmp_path):
    for round_ in range(20):
        parent = tmp_path / str(round_)
        stages = [render_evidence.staging_dir(parent) for _ in range(8)]
        for stage in stages:
            (stage / "complete").write_text("generation")
        barrier = threading.Barrier(len(stages))

        def publish(stage, barrier=barrier, parent=parent):
            barrier.wait()
            return render_evidence.publish_generation(stage, parent, "same-generation")

        with ThreadPoolExecutor(max_workers=len(stages)) as pool:
            targets = list(pool.map(publish, stages))
        assert len(set(targets)) == 1
        assert (targets[0] / "complete").read_text() == "generation"
        assert all(not stage.exists() for stage in stages)


def test_concurrent_archive_publications_keep_every_take(tmp_path):
    archive = tmp_path / "archive"
    barrier = threading.Barrier(8)

    def publish(index):
        barrier.wait()
        _write_v2(archive, f"take-{index}")

    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(publish, range(8)))
    index = render_evidence.read_archive_index(archive)
    assert set(index["takes"]["cap"]) == {f"take-{i}" for i in range(8)}


def _write_v2(archive: Path, take: str, timbre: str = "di", level: float = 0.25) -> None:
    request = render_evidence.reference_request("au", seconds=1.0, sample_rate=SR, smf=b"x")
    identities = {
        timbre: {
            "request": request,
            "request_id": f"req-{take}",
            "source_id": f"src-{take}",
        }
    }
    renders = {timbre: np.full((SR, 2), level, dtype=np.float64)}
    make_audition.archive_references(archive, "cap", take, renders, identities)


def _write_v1(archive: Path, take: str, timbre: str = "di", level: float = 0.5) -> None:
    (archive / "cap" / take).mkdir(parents=True, exist_ok=True)
    write_wav(archive / "cap" / take / f"{timbre}.wav", np.full((SR, 2), level), SR, bits=24)
    index = archive / "index.json"
    data = json.loads(index.read_text()) if index.exists() else {}
    data.setdefault("cap", {})[take] = {"gain_db": 0.0, "timbres": [timbre]}
    index.write_text(json.dumps(data))


def test_records_put_v2_first_and_flag_v1_historical(tmp_path):
    _write_v1(tmp_path, "t")
    _write_v2(tmp_path, "t")
    records = render_evidence.archived_records(tmp_path, "cap", "t")
    assert [r.archive for r in records] == ["v2", "v1"]
    assert not records[0].historical and records[1].historical
    assert "generation" in records[0].describe() and "historical" in records[1].describe()


def test_a_v2_record_whose_file_changed_does_not_load(tmp_path):
    _write_v2(tmp_path, "t")
    record = render_evidence.archived_records(tmp_path, "cap", "t")[0]
    record.path.write_bytes(record.path.read_bytes()[:-6] + b"\x00" * 6)
    assert record.load() is None


def test_profile_take_prefers_v2_and_says_where_each_reference_came_from(tmp_path, capsys):
    _write_v1(tmp_path, "t", level=0.5)
    _write_v2(tmp_path, "t", level=0.25)
    refs = profile_take.archived_take_references(tmp_path, "cap", "t", SR)
    assert list(refs) == ["di"]
    assert refs["di"] == pytest.approx(np.full((SR, 2), 0.25), abs=1e-6)
    err = capsys.readouterr().err
    assert "t/di: archive v2, generation" in err and "v1" not in err


def test_profile_take_measures_a_v1_reference_as_historical(tmp_path, capsys):
    _write_v1(tmp_path, "t", level=0.5)
    refs = profile_take.archived_take_references(tmp_path, "cap", "t", SR)
    assert refs["di"] == pytest.approx(np.full((SR, 2), 0.5), abs=1e-6)
    assert "t/di: archive v1, historical" in capsys.readouterr().err
    assert profile_take.archived_take_references(tmp_path, "cap", "t", 44100) == {}


def test_take_ids_cover_both_archives(tmp_path):
    _write_v1(tmp_path, "old")
    _write_v2(tmp_path, "new")
    assert profile_take.archived_take_ids(tmp_path, "cap") == {"old", "new"}
    assert render_evidence.archived_take_ids(tmp_path, "cap") == {"old": "v1", "new": "v2"}


def test_readiness_counts_both_archives_and_names_the_historical_takes(tmp_path):
    takes_set = "piano"
    take_ids = [t.id for t in build_takes(takes_set, 0)]
    archive, reference_dir = tmp_path / "archive", tmp_path / "reference"
    reference_dir.mkdir()
    (reference_dir / "cap.json").write_text(json.dumps({"rows": [{"note": 60}]}))
    _write_v1(archive, take_ids[0])
    _write_v2(archive, take_ids[1])
    cfg = {"id": "cap", "program": 0, "takes": takes_set}

    out = profile_status.readiness(cfg, archive=archive, reference_dir=reference_dir)

    assert out["takes_archived"] == 2 and out["takes_archived_historical"] == 1
    hint = [line for line in out["next"] if "v1 index" in line]
    assert hint and hint[0].startswith(f"1 of {len(take_ids)} phrase takes")
