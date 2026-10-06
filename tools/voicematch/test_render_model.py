"""Tests for the model renderer's library-staleness warning and render evidence.

Every number the harness reports comes out of one dylib, the render carries no
mark of which, and the default is a build directory nothing keeps current — so
a whole bank-wide sweep can measure the previous generation of a voice with
each reading looking entirely ordinary. These pin the one line that says so.

    rye run --pyproject bindings/python/pyproject.toml \
        python -m pytest tools/voicematch/test_render_model.py
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import render_model
from boundary import canonical_digest

HOUR = 3600.0


def _stamp(when: float) -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(when))


@pytest.fixture(autouse=True)
def _fresh_process(monkeypatch):
    """Undo the once-per-process latch so each case starts unwarned."""
    monkeypatch.setattr(render_model, "_staleness_checked", False)


def _library(tmp_path: Path, *, built_at: float) -> str:
    lib = tmp_path / "libsonare.dylib"
    lib.write_bytes(b"")
    os.utime(lib, (built_at, built_at))
    return str(lib)


def test_a_library_newer_than_the_sources_says_nothing(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(render_model, "_newest_source_mtime", lambda: 1000.0)
    render_model.warn_if_stale(_library(tmp_path, built_at=1000.0 + HOUR))
    assert capsys.readouterr().err == ""


def test_a_library_older_than_a_source_names_both_times_and_the_refresh(
    tmp_path, monkeypatch, capsys
):
    monkeypatch.setattr(render_model, "_newest_source_mtime", lambda: 1000.0 + HOUR)
    render_model.warn_if_stale(_library(tmp_path, built_at=1000.0))
    err = capsys.readouterr().err
    assert "measures the older library" in err
    assert render_model.REFRESH_HINT in err
    # Both stamps, so the reader sees how far apart they are rather than only
    # that they differ.
    assert _stamp(1000.0) in err
    assert _stamp(1000.0 + HOUR) in err


def test_the_warning_is_said_once_per_process(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(render_model, "_newest_source_mtime", lambda: 1000.0 + HOUR)
    lib = _library(tmp_path, built_at=1000.0)
    render_model.warn_if_stale(lib)
    capsys.readouterr()
    render_model.warn_if_stale(lib)
    assert capsys.readouterr().err == ""


def test_no_resolved_library_is_not_a_staleness_finding(monkeypatch, capsys):
    def _unreachable() -> float:  # pragma: no cover - asserts it is never called
        raise AssertionError("the source tree must not be walked for an empty path")

    monkeypatch.setattr(render_model, "_newest_source_mtime", _unreachable)
    render_model.warn_if_stale("")
    assert capsys.readouterr().err == ""


def test_a_library_that_is_not_there_is_not_a_staleness_finding(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(render_model, "_newest_source_mtime", lambda: 1000.0 + HOUR)
    render_model.warn_if_stale(str(tmp_path / "absent.dylib"))
    assert capsys.readouterr().err == ""


def test_the_render_is_pinned_to_the_rate_the_capture_was_recorded_at(monkeypatch):
    """A bounce rate that differs from the project's own is refused outright.

    `project_bounce.cpp` rejects the pair rather than resampling, and a fresh
    project does not start at the rate a capture happens to hold. Every capture
    whose `source_class` is `module` records 44.1 kHz, so before the project was
    told the rate none of those 19 references could be compared against the
    model at all — `profile.py compare` raised INVALID_PARAMETER on the first
    note and nothing downstream distinguished that from a voice that does not
    render.
    """
    seen: dict[str, object] = {}

    class _Project:
        def set_sample_rate(self, rate):
            seen["project_rate"] = rate

        def import_smf(self, _data):
            pass

        def bounce_with_sf2_instrument(self, _cfg, *, total_frames, sample_rate):
            seen["bounce_rate"] = sample_rate
            seen["frames"] = total_frames
            return [[0.0, 0.0]] * total_frames

        def soundfont_manifest(self):
            return {}

        def close(self):
            pass

    fake = type(sys)("libsonare")
    fake.Project = _Project
    fake.Sf2InstrumentConfig = lambda **_kw: object()
    monkeypatch.setitem(sys.modules, "libsonare", fake)
    monkeypatch.setattr(render_model, "ensure_lib_path", lambda: None)
    monkeypatch.setattr(render_model, "check_gm_fallback", lambda _m: None)

    render_model.render_model(b"", 0.4, 44100, rig=False)

    assert seen["project_rate"] == 44100.0
    # The two have to be the same number, which is the whole of the contract.
    assert seen["bounce_rate"] == 44100
    assert seen["frames"] == round(0.4 * 44100)


def test_a_request_renders_at_its_own_rig_rate_and_window(monkeypatch):
    from boundary import RenderRequest

    seen = {}

    def fake_render(smf, seconds, sr, *, rig, preset):
        seen.update(smf=smf, seconds=seconds, sr=sr, rig=rig, preset=preset)
        return "audio"

    monkeypatch.setattr(render_model, "render_model", fake_render)
    request = RenderRequest(program=27, seconds=1.5, smf=b"MThd", rig=False, sample_rate=44100)
    assert render_model.render_request(request) == "audio"
    assert seen == {"smf": b"MThd", "seconds": 1.5, "sr": 44100, "rig": False, "preset": ""}


def test_request_rejects_overrides_that_disagree_with_the_render_environment(monkeypatch):
    from boundary import RenderRequest
    from smf import write_smf

    monkeypatch.setenv("SONARE_TUNING_OVERRIDES", "voice.knob=1")
    request = RenderRequest(
        program=27, seconds=0.01, smf=write_smf([], program=27), overrides="voice.knob=2"
    )
    for render in (render_model.render_request, render_model.render_request_rendered):
        with pytest.raises(ValueError, match="SONARE_TUNING_OVERRIDES"):
            render(request)


def _fake_bounce_library(monkeypatch, tmp_path, *, record: dict | None):
    """A stand-in libsonare whose bounce writes `record` where a tuning build would."""
    seen: dict[str, list] = {"dump_paths": []}

    class _Project:
        def set_sample_rate(self, _rate):
            pass

        def import_smf(self, _data):
            pass

        def bounce_with_sf2_instrument(self, _cfg, *, total_frames, sample_rate):
            dump = os.environ.get(render_model.PATH_DUMP_ENV)
            seen["dump_paths"].append(dump)
            if record is not None:
                Path(dump).write_text(json.dumps(record))
            return [[0.0, 0.0]] * total_frames

        def soundfont_manifest(self):
            return {}

        def close(self):
            pass

    fake = type(sys)("libsonare")
    fake.Project = _Project
    fake.Sf2InstrumentConfig = lambda **_kw: object()
    monkeypatch.setitem(sys.modules, "libsonare", fake)
    monkeypatch.setattr(render_model, "ensure_lib_path", lambda: None)
    monkeypatch.setattr(render_model, "check_gm_fallback", lambda _m: None)
    monkeypatch.delenv(render_model.PATH_DUMP_ENV, raising=False)
    lib = tmp_path / "libsonare.dylib"
    lib.write_bytes(b"a library binary")
    monkeypatch.setattr(render_model, "loaded_library_path", lambda: str(lib))
    return seen, hashlib.sha256(lib.read_bytes()).hexdigest()


def test_a_path_record_is_returned_with_the_audio_and_names_the_bank(monkeypatch, tmp_path):
    record = {
        "schema": 1,
        "bank_registry_digest": "ab" * 32,
        "library_version": "1.8.1",
        "complete": True,
        "reason": None,
        "events": [{"frame": 0, "kind": "topology", "parts": [], "units": []}],
    }
    seen, lib_sha = _fake_bounce_library(monkeypatch, tmp_path, record=record)

    rendered = render_model.render_model_rendered(b"MThd", 0.01, 48000, request_id="req")
    again = render_model.render_model_rendered(b"MThd", 0.01, 48000, request_id="req")

    ev = rendered.evidence
    assert ev["status"] == "recorded" and ev["reason"] is None
    assert ev["path"] == record and ev["complete"] is True
    assert ev["request_id"] == "req"
    assert ev["build_id"] == canonical_digest(
        {"library_sha256": lib_sha, "bank_registry_digest": "ab" * 32}
    )
    assert again.evidence == ev
    assert rendered.audio.shape == (480, 2)
    # One path per render, and the variable does not outlive it.
    assert len(set(seen["dump_paths"])) == 2 and None not in seen["dump_paths"]
    assert render_model.PATH_DUMP_ENV not in os.environ


def test_no_path_record_leaves_the_evidence_unknown_and_says_why(monkeypatch, tmp_path):
    _, lib_sha = _fake_bounce_library(monkeypatch, tmp_path, record=None)

    ev = render_model.render_model_rendered(b"MThd", 0.01, 48000).evidence

    assert ev["status"] == "unknown" and ev["path"] is None and ev["complete"] is False
    assert ev["reason"] == render_model.NO_PATH_RECORD
    assert ev["build_id"] == canonical_digest(
        {"library_sha256": lib_sha, "bank_registry_digest": None}
    )
    # Without a request, the identity is the arguments actually rendered.
    other = render_model.render_model_rendered(b"MThd", 0.01, 48000, rig=False).evidence
    assert ev["request_id"] != other["request_id"]


def test_a_record_of_another_schema_is_not_read_as_evidence(monkeypatch, tmp_path):
    _fake_bounce_library(monkeypatch, tmp_path, record={"schema": 2, "complete": True})
    ev = render_model.render_model_rendered(b"MThd", 0.01, 48000).evidence
    assert ev["status"] == "unknown" and ev["path"] is None
    assert "schema 2" in ev["reason"]


def test_a_request_renders_with_its_own_fingerprint_as_request_id(monkeypatch):
    from boundary import RenderRequest

    seen = {}

    def fake_rendered(smf, seconds, sr, *, rig, preset, request_id):
        seen.update(rig=rig, request_id=request_id)
        return "rendered"

    monkeypatch.setattr(render_model, "render_model_rendered", fake_rendered)
    request = RenderRequest(program=27, seconds=1.5, smf=b"MThd", rig=False, sample_rate=44100)
    assert render_model.render_request_rendered(request) == "rendered"
    assert seen == {"rig": False, "request_id": request.fingerprint()}
