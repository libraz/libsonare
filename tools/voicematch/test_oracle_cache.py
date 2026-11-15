"""The oracle cache: what may leave, what may stand in, and what a cached fit scores."""

from __future__ import annotations

import dataclasses
import gzip
import json
import shutil
import types
from pathlib import Path

import autofit
import autofit_resolve
import corpus as corpus_module
import numpy as np
import objective
import oracle_cache
import pytest
import render_rows
from autofit_test_fixtures import _fit_args, _write_corpus
from oracle_cache import CacheError


def _canon(value) -> str:
    return json.dumps(oracle_cache.plain(value), sort_keys=True)


def _args(corpus_dir, cache_dir, **kwargs):
    base = {
        "program": 0,
        "pattern": "sustain",
        "notes": "60",
        "velocities": "100",
        "corpus": str(corpus_dir),
        "corpus_timbre": "t",
        "room": "none",
        "oracle_cache": str(cache_dir),
    }
    base.update(kwargs)
    return _fit_args(**base)


def _mini(tmp_path, **kwargs) -> Path:
    kwargs.setdefault("notes", (60,))
    kwargs.setdefault("velocities", (100,))
    kwargs.setdefault("gate_ms", 500)
    kwargs.setdefault("seconds", 2.0)
    return _write_corpus(tmp_path / "mini_capture", **kwargs)


@pytest.fixture(autouse=True)
def _fresh_ledger():
    oracle_cache.reset_consumed()


def forbid_corpus_audio(monkeypatch):
    def refuse(*a, **k):
        raise AssertionError("corpus_oracle was called in cache mode")

    monkeypatch.setattr(autofit, "corpus_oracle", refuse)


def _body(rows=None, room=None) -> dict:
    return {
        "identity": {
            "capture": "c",
            "timbre": "t",
            "pattern": "corpus",
            "notes": "60",
            "velocities": "100",
            "drum_note": None,
            "drum_gate_ms": 0,
            "mono_mode": "mean",
            "room": "none",
            "role": "fit",
        },
        "digests": {
            "corpus": "a" * 64,
            "code": oracle_cache.code_digest(),
            "reference": "absent",
            "objective": "b" * 16,
        },
        "descriptor": None,
        "rows": rows
        if rows is not None
        else [{"note": 60, "velocity": 100, "tnr_db": float("nan")}],
        "room": room,
        "band_edge": 8000.0,
        "channel_correlation": 0.5,
    }


def _write(path: Path, descriptor: dict, **overrides) -> str:
    body = _body()
    body.update(overrides)
    return oracle_cache.write(
        path,
        identity=body["identity"],
        digests=body["digests"],
        descriptor=descriptor,
        rows=body["rows"],
        room=body["room"],
        band_edge=body["band_edge"],
        channel_correlation=body["channel_correlation"],
    )


def _descriptor(tmp_path) -> dict:
    return corpus_module.corpus_descriptor(corpus_module.load_corpus(_mini(tmp_path), "t"))


# --------------------------------------------------------------------------- #
# The whitelist
# --------------------------------------------------------------------------- #
def test_the_whitelist_refuses_an_unknown_key_by_name(tmp_path):
    path = tmp_path / "x.json.gz"
    with pytest.raises(CacheError, match="unknown key 'spectrogram'"):
        _write(path, _descriptor(tmp_path), rows=[{"note": 60, "spectrogram": [1.0]}])
    assert not path.exists()
    desc = _descriptor(tmp_path / "other")
    desc["source"] = "x"
    with pytest.raises(CacheError, match="unknown key 'source'"):
        _write(path, desc)


def test_the_whitelist_refuses_a_long_array_and_anything_that_is_not_a_number(tmp_path):
    desc = _descriptor(tmp_path)
    path = tmp_path / "x.json.gz"
    with pytest.raises(CacheError, match="array too large to leave"):
        _write(path, desc, rows=[{"note": 60, "bands_db": [0.0] * (oracle_cache.MAX_ARRAY + 1)}])
    with pytest.raises(CacheError, match="array too large to leave"):
        _write(path, desc, rows=[{"note": 60, "evolution_db": [[0.0] * 500]}])
    with pytest.raises(CacheError, match="may not leave"):
        _write(path, desc, rows=[{"note": 60, "bands_db": b"\x00\x01"}])
    with pytest.raises(CacheError, match="may not leave"):
        _write(path, desc, rows=[{"note": 60, "f0_hz": "440"}])
    _write(path, desc, rows=[{"note": 60, "bands_db": [0.0] * oracle_cache.MAX_ARRAY}])


def test_the_row_whitelist_is_the_measurement_records_plus_what_the_probe_adds():
    from metrics_hit import HitMetrics
    from metrics_note import NoteMetrics
    from room import Room

    recorded = {f.name for f in dataclasses.fields(NoteMetrics)}
    recorded |= {f.name for f in dataclasses.fields(HitMetrics)}
    added = {
        "skeleton",
        "attack_hf_db",
        "attack_lf_db",
        "attack_peaks",
        "peak_dbfs",
        "held_rms_dbfs",
        "held_crest_db",
        "loop_unweighted",
    }
    assert set(oracle_cache.ROW_KEYS) == recorded | added
    assert len(oracle_cache.ROW_KEYS) == len(set(oracle_cache.ROW_KEYS))
    assert set(oracle_cache.ROOM_KEYS) == {f.name for f in dataclasses.fields(Room)}


# --------------------------------------------------------------------------- #
# Files
# --------------------------------------------------------------------------- #
def test_a_cache_reads_back_what_was_written(tmp_path):
    from room import Room

    desc = _descriptor(tmp_path)
    room = Room(rt60_s=1.5, hf_ratio=0.6, tail_db=-12.0, predelay_ms=7.0, tail_window_s=2.0)
    path = tmp_path / "c" / "x.json.gz"
    digest = _write(path, desc, room=room)
    body = _body()
    cached = oracle_cache.read(path, {"identity": body["identity"], "corpus_digest": None})
    assert _canon(cached.rows) == _canon(body["rows"])
    assert cached.room == room
    assert (cached.band_edge, cached.channel_correlation) == (8000.0, 0.5)
    assert cached.descriptor == desc
    assert cached.digest == digest
    assert oracle_cache.consumed()[0]["digest"] == digest


def test_a_cache_written_twice_is_byte_identical(tmp_path):
    desc = _descriptor(tmp_path)
    _write(tmp_path / "a.json.gz", desc)
    _write(tmp_path / "b.json.gz", desc)
    assert (tmp_path / "a.json.gz").read_bytes() == (tmp_path / "b.json.gz").read_bytes()


def test_a_cache_for_another_question_is_refused(tmp_path):
    path = tmp_path / "x.json.gz"
    _write(path, _descriptor(tmp_path))
    asked = _body()["identity"] | {"notes": "60,72"}
    with pytest.raises(CacheError, match=r"different question.*notes: cache '60', run '60,72'"):
        oracle_cache.read(path, {"identity": asked})


def test_a_cache_measured_by_other_code_is_refused(tmp_path, monkeypatch):
    path = tmp_path / "x.json.gz"
    _write(path, _descriptor(tmp_path))
    monkeypatch.setattr(oracle_cache, "code_digest", lambda *a: "f" * 64)
    with pytest.raises(CacheError, match="stale: the code that measured it"):
        oracle_cache.read(path, {"identity": _body()["identity"]})


def test_a_cache_of_another_reference_profile_is_refused(tmp_path):
    path = tmp_path / "x.json.gz"
    _write(path, _descriptor(tmp_path))
    with pytest.raises(CacheError, match="committed reference profile"):
        oracle_cache.read(path, {"identity": _body()["identity"], "reference_digest": "c" * 64})


def test_a_missing_cache_names_the_file(tmp_path):
    with pytest.raises(FileNotFoundError, match="no oracle cache at"):
        oracle_cache.read(tmp_path / "nope.json.gz")


def test_the_code_digest_covers_the_modules_that_measure_the_oracle():
    modules = objective.objective_modules(oracle_cache.HERE, oracle_cache.CODE_ROOTS)
    for name in (
        "loss",
        "corpus",
        "room",
        "loop_record",
        "metrics_note",
        "metrics_hit",
        "oracle_cache",
    ):
        assert name in modules
    assert len(oracle_cache.code_digest()) == 64


def test_cache_files_are_named_per_capture_timbre_and_role(tmp_path):
    key = oracle_cache.capture_key(str(tmp_path / "piano_grand"))
    assert key == "piano_grand"
    assert oracle_cache.capture_key(str(tmp_path / "piano_grand" / "manifest.json")) == key
    assert oracle_cache.cache_file("d", key, "", "fit").name == "piano_grand__default__fit.json.gz"
    assert oracle_cache.cache_file("d", key, "t1", "holdout").name == (
        "piano_grand__t1__holdout.json.gz"
    )


# --------------------------------------------------------------------------- #
# A run with the corpus, and the same run from the cache alone
# --------------------------------------------------------------------------- #
def test_the_oracle_from_a_cache_equals_the_oracle_from_the_corpus(tmp_path, monkeypatch):
    corpus_dir = _mini(tmp_path)
    cache_dir = tmp_path / "caches"
    plain_rows, _, plain_room, plain_edge = autofit.oracle_reference(
        _args(corpus_dir, cache_dir, oracle_cache="")
    )
    written, _, _, _ = autofit.oracle_reference(_args(corpus_dir, cache_dir))
    assert _canon(written) == _canon(plain_rows)
    manifest_corpus = corpus_module.load_corpus(corpus_dir, "t")
    shutil.rmtree(corpus_dir)
    forbid_corpus_audio(monkeypatch)

    rows, audio, room, edge = autofit.oracle_reference(_args(corpus_dir, cache_dir))
    assert audio is None
    assert _canon(rows) == _canon(plain_rows)
    assert room == plain_room
    assert edge == plain_edge
    source = oracle_cache.source_record()
    assert source["source"] == "cache" and len(source["cache_digest"]) == 64

    stood_in = autofit_resolve.resolve_corpus(_args(corpus_dir, cache_dir))
    assert stood_in.cache_file == cache_dir / "mini_capture__t__fit.json.gz"
    assert stood_in.root == corpus_dir
    assert _used(stood_in) == _used(manifest_corpus)


def test_a_cache_is_refused_for_a_corpus_that_changed_under_it(tmp_path):
    corpus_dir = _mini(tmp_path)
    cache_dir = tmp_path / "caches"
    autofit.oracle_reference(_args(corpus_dir, cache_dir))
    forced = _args(corpus_dir, cache_dir, oracle_cache_only=True)
    rows, audio, _, _ = autofit.oracle_reference(forced)
    assert audio is None and rows
    wav = next(corpus_dir.rglob("*.wav"))
    wav.write_bytes(wav.read_bytes() + b"\x00\x00\x00\x00")
    with pytest.raises(CacheError, match="corpus digest"):
        autofit.oracle_reference(forced)


def test_the_manifest_is_part_of_the_corpus_digest(tmp_path):
    corpus_dir = _mini(tmp_path)
    cache_dir = tmp_path / "caches"
    autofit.oracle_reference(_args(corpus_dir, cache_dir))
    manifest = corpus_dir / "manifest.json"
    body = json.loads(manifest.read_text())
    body["gate_ms"] = 400
    manifest.write_text(json.dumps(body))
    with pytest.raises(CacheError, match="corpus digest"):
        autofit.oracle_reference(_args(corpus_dir, cache_dir, oracle_cache_only=True))


def test_a_cache_for_other_notes_is_refused_without_the_corpus(tmp_path):
    corpus_dir = _mini(tmp_path, notes=(60, 72))
    cache_dir = tmp_path / "caches"
    autofit.oracle_reference(_args(corpus_dir, cache_dir))
    shutil.rmtree(corpus_dir)
    with pytest.raises(CacheError, match="different question"):
        autofit.oracle_reference(_args(corpus_dir, cache_dir, notes="60,72"))


def test_a_hold_out_reads_and_writes_its_own_cache(tmp_path, monkeypatch):
    corpus_dir = _mini(tmp_path, notes=(60, 72))
    cache_dir = tmp_path / "caches"
    fit = _args(corpus_dir, cache_dir, notes="60")
    held = _args(corpus_dir, cache_dir, notes="72", oracle_cache_role="holdout")
    fit_rows = autofit.oracle_reference(fit)[0]
    held_rows = autofit.oracle_reference(held)[0]
    assert sorted(p.name for p in cache_dir.iterdir()) == [
        "mini_capture__t__fit.json.gz",
        "mini_capture__t__holdout.json.gz",
    ]
    shutil.rmtree(corpus_dir)
    forbid_corpus_audio(monkeypatch)
    assert _canon(autofit.oracle_reference(fit)[0]) == _canon(fit_rows)
    assert _canon(autofit.oracle_reference(held)[0]) == _canon(held_rows)
    assert [r["note"] for r in held_rows] == [72]


# --------------------------------------------------------------------------- #
# The descriptor
# --------------------------------------------------------------------------- #
def _used(corpus) -> dict:
    skip = {"root", "renders", "label", "cache_file"}
    out = {
        f.name: getattr(corpus, f.name) for f in dataclasses.fields(corpus) if f.name not in skip
    }
    out["render_keys"] = sorted(corpus.renders)
    return out


def test_a_descriptor_corpus_equals_the_manifest_corpus_on_every_field_a_fit_uses(tmp_path):
    corpus_dir = _mini(
        tmp_path,
        notes=(60, 72),
        velocities=(56, 120),
        seconds={60: 2.0, 72: 3.0},
        channel=1,
        groups={"pair": [60, 72]},
        note_map={60: 61},
        rig="none",
        room="none",
    )
    manifest = json.loads((corpus_dir / "manifest.json").read_text())
    for rec in manifest["renders"]:
        if rec["note"] == 72:
            rec["window_end_ms"] = 900
    (corpus_dir / "manifest.json").write_text(json.dumps(manifest))
    built = corpus_module.load_corpus(corpus_dir, "t")
    assert built.gates and built.groups and built.note_map and len(built.slots) == 4

    desc = json.loads(json.dumps(corpus_module.corpus_descriptor(built)))
    standing = corpus_module.corpus_from_descriptor(desc, corpus_dir)
    assert _used(standing) == _used(built)
    assert set(standing.renders) == set(built.renders)
    assert standing.label == "t" and built.label == "mini timbre"
    assert standing.played_notes() == built.played_notes()
    assert standing.gate_for(72, 56) == built.gate_for(72, 56)
    assert autofit._corpus_identity(standing) == autofit._corpus_identity(built)
    assert corpus_module.corpus_pattern(standing).notes == corpus_module.corpus_pattern(built).notes


def test_the_label_and_id_come_from_the_tracked_definition_and_never_the_manifest(tmp_path):
    corpus_dir = _mini(tmp_path)
    manifest = json.loads((corpus_dir / "manifest.json").read_text())
    manifest["timbres"][0]["label"] = "Secret Product 9"
    (corpus_dir / "manifest.json").write_text(json.dumps(manifest))
    built = corpus_module.load_corpus(corpus_dir, "t")
    assert built.label == "Secret Product 9"
    definitions = tmp_path / "capture"
    definitions.mkdir()
    (definitions / "mini.json").write_text(
        json.dumps({"id": "mini", "timbres": [{"id": "t", "label": "Plain module recording"}]})
    )
    desc = corpus_module.corpus_descriptor(built, definitions)
    assert desc["label"] == "Plain module recording" and desc["capture_id"] == "mini"
    assert corpus_module.corpus_descriptor(built, tmp_path / "none")["label"] == "t"


def test_the_label_is_read_for_printing_only():
    readers = []
    for path in sorted(Path(__file__).resolve().parent.glob("*.py")):
        if path.name.startswith("test_"):
            continue
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if "corpus.label" in line or "built.label" in line:
                readers.append(f"{path.name}:{number}")
    assert [r.split(":")[0] for r in readers] == ["corpus.py", "corpus.py"]


def _slot_table(directory: Path, rows: list[dict]) -> None:
    directory.mkdir(exist_ok=True)
    (directory / "mini_slots.json").write_text(json.dumps(rows))


def test_the_slot_table_hook_refuses_a_mismatch_and_passes_without_a_table(tmp_path):
    desc = _descriptor(tmp_path)
    reference = tmp_path / "reference"
    corpus_module.check_against_slot_table(desc, "mini", reference)
    seconds = desc["slots"][0][2] + desc["preroll_s"]
    right = [{"timbre": "t", "note": 60, "velocity": 100, "seconds": seconds}]
    _slot_table(reference, right)
    corpus_module.check_against_slot_table(desc, "mini", reference)
    _slot_table(reference, [{**right[0], "seconds": seconds + 0.5}])
    with pytest.raises(ValueError, match="differ from mini_slots.json"):
        corpus_module.check_against_slot_table(desc, "mini", reference)
    _slot_table(reference, [*right, {**right[0], "note": 61}])
    with pytest.raises(ValueError, match="n61/v100"):
        corpus_module.check_against_slot_table(desc, "mini", reference)


# --------------------------------------------------------------------------- #
# The multi-scale term and the report
# --------------------------------------------------------------------------- #
def test_a_noise_program_from_a_cache_needs_an_explicit_zero_mss(tmp_path, monkeypatch):
    corpus_dir = _mini(tmp_path)
    cache_dir = tmp_path / "caches"
    autofit.oracle_reference(_args(corpus_dir, cache_dir, program=119))
    shutil.rmtree(corpus_dir)
    forbid_corpus_audio(monkeypatch)
    with pytest.raises(ValueError, match=r"mss\) needs the corpus audio.*--w-mss 0"):
        autofit.oracle_reference(_args(corpus_dir, cache_dir, program=119))
    zeroed = _args(corpus_dir, cache_dir, program=119, w_mss=0.0)
    rows, audio, _, _ = autofit.oracle_reference(zeroed)
    assert rows and audio is None
    record = autofit.oracle_source_record(zeroed)
    assert record["scored_without"] == ["mss"]
    assert record["oracle"]["source"] == "cache"
    assert record["oracle"]["cache_digest"] == oracle_cache.source_record()["cache_digest"]


def test_an_ordinary_program_from_a_cache_runs_with_no_flag_and_scores_the_same_objective(
    tmp_path,
):
    corpus_dir = _mini(tmp_path)
    cache_dir = tmp_path / "caches"
    args = _args(corpus_dir, cache_dir)
    autofit.oracle_reference(args)
    assert autofit.oracle_source_record(args) == {}
    shutil.rmtree(corpus_dir)
    rows, audio, _, _ = autofit.oracle_reference(args)
    assert rows and audio is None
    record = autofit.oracle_source_record(args)
    assert record["oracle"]["source"] == "cache"
    assert "scored_without" not in record


def test_a_run_without_a_cache_reports_no_oracle_source(tmp_path):
    corpus_dir = _mini(tmp_path)
    args = _args(corpus_dir, tmp_path, oracle_cache="")
    autofit.oracle_reference(args)
    assert autofit.oracle_source_record(args) == {}


def test_a_cache_flag_without_a_corpus_is_refused():
    with pytest.raises(ValueError, match="--oracle-cache-only needs --oracle-cache"):
        autofit_resolve.oracle_cache_plan(_fit_args(oracle_cache_only=True), None)
    with pytest.raises(ValueError, match="applies to a corpus oracle"):
        autofit_resolve.oracle_cache_plan(_fit_args(oracle_cache="somewhere"), None)


# --------------------------------------------------------------------------- #
# What the file holds
# --------------------------------------------------------------------------- #
def _walk(node, keys, strings, lists):
    if isinstance(node, dict):
        for k, v in node.items():
            keys.add(k)
            _walk(v, keys, strings, lists)
    elif isinstance(node, list):
        lists.append(node)
        for v in node:
            _walk(v, keys, strings, lists)
    elif isinstance(node, str):
        strings.append(node)


def test_the_file_holds_no_source_no_config_no_sample_and_no_long_list(tmp_path):
    corpus_dir = _mini(tmp_path, notes=(60, 72), velocities=(56, 120))
    manifest = json.loads((corpus_dir / "manifest.json").read_text())
    manifest.update(
        source="Secret Synth", config="capture/secret_plugin.json", params=[{"id": "Secret"}]
    )
    manifest["sf2_sample"] = "secretfont.sf2"
    manifest["preset"] = "Secret Preset"
    manifest["timbres"][0]["label"] = "Secret Product 9"
    (corpus_dir / "manifest.json").write_text(json.dumps(manifest))
    cache_dir = tmp_path / "caches"
    autofit.oracle_reference(_args(corpus_dir, cache_dir, notes="60,72", velocities="56,120"))

    (path,) = cache_dir.glob("*.json.gz")
    text = gzip.decompress(path.read_bytes()).decode()
    for needle in ("Secret", "secretfont", "capture/secret", str(tmp_path)):
        assert needle not in text
    keys, strings, lists = set(), [], []
    _walk(json.loads(text), keys, strings, lists)
    for banned in ("source", "config", "params", "sf2_sample", "preset", "plugin", "audio"):
        assert not [k for k in keys if banned in k.lower()], banned
    assert lists and max(len(x) for x in lists) <= oracle_cache.MAX_TABLE
    flat = [x for x in lists if all(not isinstance(i, (list, dict)) for i in x)]
    assert max(len(x) for x in flat) <= oracle_cache.MAX_ARRAY
    assert all(len(s) <= 120 for s in strings)
    assert set(json.loads(text)) == {"schema", "identity", "digests", "corpus", "oracle"}
    assert set(json.loads(text)["digests"]) == {"corpus", "code", "reference", "objective"}


def test_a_kit_corpus_writes_hit_rows_that_pass_the_whitelist(tmp_path):
    corpus_dir = _mini(tmp_path, channel=10, notes=(38,), velocities=(100,))
    cache_dir = tmp_path / "caches"
    args = _args(
        corpus_dir,
        cache_dir,
        notes="38",
        drum_note=38,
        percussive=True,
        pattern="drum",
        mono_mode="left",
    )
    rows, _, _, _ = autofit.oracle_reference(args)
    assert rows and "bands_db" in rows[0]
    cached = oracle_cache.read(
        cache_dir / "mini_capture__t__fit.json.gz",
        {"identity": oracle_cache.identity_of(args, "mini_capture", "t", "fit")},
    )
    assert _canon(cached.rows) == _canon(rows)
    assert cached.descriptor["channel"] == 10


# --------------------------------------------------------------------------- #
# The model render's child
# --------------------------------------------------------------------------- #
def test_the_render_child_is_handed_the_cache_when_there_is_no_manifest(tmp_path, monkeypatch):
    corpus_dir = _mini(tmp_path)
    cache_dir = tmp_path / "caches"
    autofit.oracle_reference(_args(corpus_dir, cache_dir))
    shutil.rmtree(corpus_dir)
    standing = autofit_resolve.resolve_corpus(_args(corpus_dir, cache_dir))
    seen = {}

    def fake_run(cmd, **kwargs):
        seen["cmd"] = cmd
        return types.SimpleNamespace(returncode=1, stderr="stop", stdout="")

    monkeypatch.setattr(render_rows.subprocess, "run", fake_run)
    monkeypatch.setattr(render_rows, "library_path", lambda build_dir: tmp_path / "libsonare.so")
    monkeypatch.setattr(render_rows.harness_lock, "check_workers", lambda: None)
    with pytest.raises(RuntimeError):
        render_rows.render_model_rows_subprocess(tmp_path, 0, "corpus", "60", corpus=standing)
    cmd = seen["cmd"]
    assert cmd[cmd.index("--corpus-cache") + 1] == str(standing.cache_file)
    assert "--corpus" not in cmd


def test_the_render_child_rebuilds_the_corpus_from_the_descriptor(tmp_path):
    corpus_dir = _mini(tmp_path)
    cache_dir = tmp_path / "caches"
    autofit.oracle_reference(_args(corpus_dir, cache_dir))
    built = corpus_module.load_corpus(corpus_dir, "t")
    cache = cache_dir / "mini_capture__t__fit.json.gz"
    child = corpus_module.corpus_from_descriptor(oracle_cache.read(cache).descriptor, cache.parent)
    assert _used(child) == _used(built)
    assert np.isclose(child.slot_s, built.slot_s)


def test_refit_reads_a_voice_capture_from_its_cache_on_a_host_without_the_manifest(
    tmp_path, monkeypatch
):
    import refit

    corpus_dir = _mini(tmp_path)
    cache_dir = tmp_path / "caches"
    autofit.oracle_reference(_args(corpus_dir, cache_dir))
    built = corpus_module.load_corpus(corpus_dir, "t")
    shutil.rmtree(corpus_dir)
    monkeypatch.setattr(refit, "ORACLE_CACHE", str(cache_dir))
    assert _used(refit._corpus(str(corpus_dir), "t")) == _used(built)
    monkeypatch.setattr(refit, "ORACLE_CACHE", "")
    with pytest.raises(FileNotFoundError):
        refit._corpus(str(corpus_dir), "t")


def test_refit_hands_its_oracle_cache_to_every_fit(tmp_path, monkeypatch):
    import refit

    seen = []

    def fake_run(cmd, **kwargs):
        seen.append(cmd)
        (tmp_path / "fit.json").write_text("{}")

    monkeypatch.setattr(refit.subprocess, "run", fake_run)
    monkeypatch.setattr(refit, "_check_pin", lambda: None)
    voice = {"slug": "p000", "program": 0, "bank": 0, "corpus": "c", "timbre": "t", "notes": [60]}
    voice.update(velocities=[100], validate_notes=[62])
    for cache, expect in (("", None), (str(tmp_path), str(tmp_path))):
        monkeypatch.setattr(refit, "ORACLE_CACHE", cache)
        refit._fit(voice, tmp_path / "fit.json", "build", 1)
        cmd = seen.pop()
        got = cmd[cmd.index("--oracle-cache") + 1] if "--oracle-cache" in cmd else None
        assert got == expect


def test_a_write_only_run_writes_the_fit_and_hold_out_caches_and_renders_nothing(
    tmp_path, monkeypatch
):
    from catalogue import Catalogue
    from test_autofit_lib import _lib, _no_build

    _no_build(monkeypatch)
    catalogue = Catalogue(
        defaults={"p0.modal.brightness": 0.5}, programs={(0, 0): "p0"}, bounds={}, modes={}, rigs={}
    )
    monkeypatch.setattr(autofit, "dump_catalogue", lambda *a, **k: catalogue)

    def refuse(*a, **k):
        raise AssertionError("a write-only run rendered")

    monkeypatch.setattr(autofit, "Evaluator", refuse)
    monkeypatch.setattr(autofit, "_fit_room_ir", refuse)
    corpus_dir = _mini(tmp_path, notes=(60, 72))
    cache_dir = tmp_path / "caches"
    argv = ["--spec", "auto", "--program", "0", "--lib", str(_lib(tmp_path))]
    argv += ["--corpus", str(corpus_dir), "--corpus-timbre", "t", "--notes", "60"]
    argv += ["--velocities", "100", "--validate-notes", "72", "--room", "none", "--short-run"]
    argv += ["--max-evals", "1", "--oracle-cache", str(cache_dir), "--oracle-cache-write"]
    argv += ["--dry-run", "--out", str(tmp_path / "fit.json")]
    assert autofit.run(autofit.build_parser().parse_args(argv), argv) == 0
    names = sorted(p.name for p in cache_dir.iterdir())
    assert names == ["mini_capture__t__fit.json.gz", "mini_capture__t__holdout.json.gz"]


def test_the_write_flag_without_a_cache_directory_is_refused():
    with pytest.raises(ValueError, match="--oracle-cache-write needs --oracle-cache"):
        autofit_resolve.oracle_cache_plan(_fit_args(oracle_cache_write=True), None)
