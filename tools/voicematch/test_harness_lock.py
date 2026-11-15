"""The harness lock: a run keeps the code it started with."""

from __future__ import annotations

import os
import re
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import autofit
import harness_lock
import pytest
import refit
import render_rows

HERE = Path(__file__).resolve().parent


def _function_level_imports(module: str) -> set[str]:
    local = {p.stem for p in HERE.glob("*.py")}
    found = set()
    for line in (HERE / f"{module}.py").read_text().splitlines():
        m = re.match(r"\s+(?:import|from)\s+(\w+)", line)
        if m and line.startswith(" ") and m.group(1) in local:
            found.add(m.group(1))
    return found


def test_closure_includes_function_level_imports():
    lazy = _function_level_imports("autofit")
    assert "refit" in lazy
    assert lazy <= set(harness_lock.closure(("autofit",)))
    assert {"gate_migrate", "loss_weights", "render_model"} <= set(
        harness_lock.closure(("autofit",))
    )


def test_closure_follows_imports_inside_functions(tmp_path):
    (tmp_path / "ra.py").write_text("def f():\n    import rb\n")
    (tmp_path / "rb.py").write_text("def g():\n    from rc import x\n")
    (tmp_path / "rc.py").write_text("x = 1\n")
    (tmp_path / "rd.py").write_text("x = 1\n")
    assert harness_lock.closure(("ra",), tmp_path) == ["ra", "rb", "rc"]


def test_digest_ignores_prose_and_follows_code(tmp_path):
    (tmp_path / "ma.py").write_text('"""doc."""\nx = 1  # c\n')
    first = harness_lock.digest(("ma",), tmp_path)
    (tmp_path / "ma.py").write_text('"""other."""\nx = 1\n')
    assert harness_lock.digest(("ma",), tmp_path) == first
    (tmp_path / "ma.py").write_text("x = 2\n")
    assert harness_lock.digest(("ma",), tmp_path) != first


def test_lock_refuses_a_module_first_imported_later(tmp_path, monkeypatch):
    (tmp_path / "hl_early.py").write_text("x = 1\n")
    (tmp_path / "hl_late.py").write_text("x = 1\n")
    monkeypatch.syspath_prepend(str(tmp_path))
    try:
        got = harness_lock.lock(("hl_early",), tmp_path)
        assert got == harness_lock.digest(("hl_early",), tmp_path)
        assert harness_lock.lock(("hl_early",), tmp_path) == got
        import hl_early  # noqa: F401  (already loaded: reused)

        with pytest.raises(ImportError, match="harness module hl_late first imported after"):
            import hl_late
        harness_lock.unlock()
        import hl_late  # noqa: F401
    finally:
        harness_lock.unlock()
        for name in ("hl_early", "hl_late"):
            sys.modules.pop(name, None)


@pytest.mark.parametrize("root", ["autofit", "refit"])
def test_every_reachable_module_is_loaded_after_lock(root):
    harness_lock.lock((root,))
    missing = [m for m in harness_lock.closure((root,)) if m not in sys.modules]
    assert not missing, f"imported late under the lock: {missing}"


def test_unlock_lets_a_new_module_in(tmp_path, monkeypatch):
    (tmp_path / "hl_one.py").write_text("x = 1\n")
    (tmp_path / "hl_two.py").write_text("x = 1\n")
    monkeypatch.syspath_prepend(str(tmp_path))
    try:
        harness_lock.lock(("hl_one",), tmp_path)
        harness_lock.unlock()
        import hl_two  # noqa: F401
    finally:
        harness_lock.unlock()
        for name in ("hl_one", "hl_two"):
            sys.modules.pop(name, None)


def test_refit_stops_when_the_harness_moves_under_it(tmp_path, monkeypatch):
    monkeypatch.setattr(harness_lock, "digest", lambda roots, directory=HERE: "new")
    monkeypatch.setattr(refit, "_PIN", "old")
    monkeypatch.setattr(refit, "_STOPPED", None)
    spawned = []
    monkeypatch.setattr(refit.subprocess, "run", lambda *a, **k: spawned.append(a))
    for slug in ("one", "two"):
        got = refit.run_voice({"slug": slug}, tmp_path, "build", 1)
        assert got["decision"] == "stopped"
        assert "(old -> new); restart it" in got["why"]
        written = (tmp_path / slug / "verdict.json").read_text()
        assert "the harness changed on disk since this refit started" in written
    assert not spawned


def test_refit_passes_the_pin_to_the_child(tmp_path, monkeypatch):
    monkeypatch.setattr(harness_lock, "digest", lambda roots, directory=HERE: "same")
    monkeypatch.setattr(refit, "_PIN", "same")
    monkeypatch.setattr(refit, "_STOPPED", None)
    seen = {}

    def fake_run(cmd, **kw):
        seen.update(kw["env"])

    monkeypatch.setattr(refit.subprocess, "run", fake_run)
    voice = {"slug": "v", "program": 0, "bank": 0, "corpus": "c", "timbre": "t", "drum_note": 36}
    with pytest.raises(RuntimeError, match="wrote no report"):
        refit._fit(voice, tmp_path / "fit.json", "build", 1)
    assert seen[harness_lock.ENV] == "same"


def test_autofit_refuses_a_mismatched_pin(monkeypatch, capsys):
    monkeypatch.setenv(harness_lock.ENV, "0000000000000000")
    assert autofit.run(SimpleNamespace()) == 2
    assert "restart the run" in capsys.readouterr().err


def test_autofit_accepts_the_matching_pin(monkeypatch):
    here = harness_lock.digest(("autofit",))
    monkeypatch.setenv(harness_lock.ENV, here)
    assert harness_lock.mismatch(here) is None


def _locked_worker(tmp_path, monkeypatch):
    (tmp_path / "hl_main.py").write_text("x = 1\n")
    (tmp_path / "hl_worker.py").write_text("import hl_dep\n")
    (tmp_path / "hl_dep.py").write_text("y = 1\n")
    monkeypatch.syspath_prepend(str(tmp_path))
    monkeypatch.setattr(harness_lock, "WORKER_ROOTS", ("hl_worker",))
    harness_lock.lock(("hl_main",), tmp_path)


def test_a_moved_worker_file_stops_the_next_render(tmp_path, monkeypatch):
    _locked_worker(tmp_path, monkeypatch)
    spawned = []
    monkeypatch.setattr(render_rows.subprocess, "run", lambda *a, **k: spawned.append(a))
    try:
        harness_lock.check_workers()
        dep = tmp_path / "hl_dep.py"
        stamp = dep.stat().st_mtime_ns
        dep.write_text("y = 2\n")
        os.utime(dep, ns=(stamp + 10**9, stamp + 10**9))
        with pytest.raises(harness_lock.HarnessFileMoved, match="harness file hl_dep.py changed"):
            render_rows.render_model_rows_subprocess(tmp_path, 0, "p", "60")
        assert not spawned
        # Not a RuntimeError, so a render-failure handler cannot score it as one.
        assert not issubclass(harness_lock.HarnessFileMoved, RuntimeError)
    finally:
        harness_lock.unlock()
        for name in ("hl_main", "hl_worker", "hl_dep"):
            sys.modules.pop(name, None)


def test_a_deleted_worker_file_counts_as_moved(tmp_path):
    f = tmp_path / "a.py"
    f.write_text("x = 1\n")
    snap = {str(f): f.stat().st_mtime_ns}
    assert harness_lock.moved_since(snap) is None
    f.unlink()
    assert harness_lock.moved_since(snap) == "a.py"


def test_no_lock_means_no_check(tmp_path, monkeypatch):
    harness_lock.unlock()
    harness_lock.check_workers()
    monkeypatch.setattr(render_rows, "library_path", lambda b: None)
    with pytest.raises(RuntimeError, match="no libsonare shared library"):
        render_rows.render_model_rows_subprocess(tmp_path, 0, "p", "60")


def test_stat_check_cost_on_the_real_closure():
    roots = harness_lock.WORKER_ROOTS
    start = time.perf_counter()
    snap = harness_lock.snapshot_mtimes(roots)
    taken = time.perf_counter() - start
    runs = 200
    start = time.perf_counter()
    for _ in range(runs):
        assert harness_lock.moved_since(snap) is None
    per_call = (time.perf_counter() - start) / runs
    print(
        f"\nsnapshot of {len(snap)} files: {taken * 1e3:.2f} ms; moved_since: {per_call * 1e6:.1f} us"
    )
    assert snap
