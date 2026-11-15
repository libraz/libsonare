"""The remote-fit driver: what it refuses, what it sends, and how its confirm pair decides."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import remote_fit

_real_tree = remote_fit.sendaway_tree


@pytest.fixture(autouse=True)
def _marked_tree(monkeypatch, tmp_path):
    """Every test sends from a marked copy of its own, never from the checkout running it."""
    monkeypatch.setattr(remote_fit, "sendaway_tree", lambda: tmp_path)


ARGV = ["--spec", "auto", "--program", "12", "--corpus", "/x/marimba", "--corpus-timbre", "t"]


def _quality(loss, **terms):
    return {
        "absolute_weighted_mean_units": loss,
        "terms": {k: {"weighted_units": v} for k, v in terms.items()} | {"env": {}},
    }


def _report(start=10.0, best=9.0):
    quality = _quality(best, modes=4.0)
    quality["baseline"] = _quality(start, modes=5.0)
    return {
        "knobs": [{"key": "marimba.modal.a"}, {"key": "marimba.b"}],
        "selected": {"values": [0.123456789012, 3.0], "quality": quality},
        "renders": 400,
    }


@pytest.mark.parametrize(
    "flag",
    ["--lib", "--out", "--workers", "--oracle-cache", "--dry-run", "--cache-dir", "--no-cache"],
)
def test_a_flag_the_driver_sets_itself_is_refused(flag):
    with pytest.raises(ValueError, match="the driver sets"):
        remote_fit.check_args([*ARGV, flag, "x"])


def test_a_fit_without_a_corpus_is_refused():
    with pytest.raises(ValueError, match="give --corpus"):
        remote_fit.check_args(["--spec", "auto", "--program", "12"])


@pytest.mark.parametrize("mode", sorted(remote_fit.HOST_REFUSED_ENGINES))
def test_self_sustaining_engines_are_refused(mode):
    assert "fit it on this Mac" in remote_fit.engine_refusal(mode)


@pytest.mark.parametrize("mode", ["modal", "karplus_strong", "plucked_string", "piano", None])
def test_linear_engines_pass(mode):
    assert remote_fit.engine_refusal(mode) is None


def test_the_host_line_runs_the_search_from_the_cache_with_the_host_library():
    line = remote_fit.host_command(ARGV, "p012", 12, "abc")
    root = str(remote_fit.REPO_ROOT)
    assert f"--lib {root}/{remote_fit.HOST_LIB}" in line
    assert f"--oracle-cache {remote_fit.cache_dir('p012')}" in line
    assert "--workers 12" in line and "--dry-run" in line
    assert f"--out {root}/.remote-fit-out/p012/fit.json" in line
    assert "nice -n 10" in line and "PYTHONPATH=" in line and "host.json" in line
    assert f"--cache-dir {root}/.remote-fit-out/p012" in line
    assert line.startswith(remote_fit.ON_HOST_ONLY)


def test_a_host_line_sendaway_ran_on_this_machine_refuses_before_touching_anything(tmp_path):
    out = tmp_path / ".remote-fit-out" / "p012"
    out.mkdir(parents=True)
    (out / "fit.log").write_text("kept\n")
    line = remote_fit.host_command(ARGV, "p012", 12, "abc").replace(
        str(remote_fit.REPO_ROOT), str(tmp_path)
    )
    proc = subprocess.run(["bash", "-c", line.replace("$(uname -s)", "Darwin")], check=False)
    assert proc.returncode == remote_fit.FELL_BACK
    assert (out / "fit.log").read_text() == "kept\n"


def test_the_selected_point_is_handed_back_at_full_precision():
    assert remote_fit.start_overrides(_report()) == "marimba.modal.a=0.123456789012,marimba.b=3.0"


def test_remote_points_read_the_start_and_the_selection_per_term():
    got = remote_fit.remote_points(_report())
    assert got == {
        "start": {"loss": 10.0, "terms": {"modes": 5.0}},
        "best": {"loss": 9.0, "terms": {"modes": 4.0}},
    }


def _confirm(tmp_path, monkeypatch, here_start, here_best):
    out = tmp_path / "out"
    out.mkdir()
    (out / "fit.json").write_text(json.dumps(_report()))
    seen = []

    def fake(argv, lib, out, start):
        seen.append((argv, start))
        loss = here_best if start else here_start
        return {"renders": 2, "selected": {"quality": _quality(loss, modes=loss / 2)}}

    monkeypatch.setattr(remote_fit, "score_once", fake)
    return out, seen


def test_an_agreeing_pair_is_written_into_the_report(tmp_path, monkeypatch):
    out, seen = _confirm(tmp_path, monkeypatch, 10.02, 9.1)
    block = remote_fit.confirm(ARGV, "p012", out, Path("lib"))
    assert [s for _, s in seen] == [None, remote_fit.start_overrides(_report())]
    assert block["agrees"] and block["improves"] and block["renders"] == 4
    assert json.loads((out / "fit.json").read_text())["confirm"] == block


def test_a_start_the_machines_score_apart_refuses_the_report(tmp_path, monkeypatch):
    out, _ = _confirm(tmp_path, monkeypatch, 10.2, 9.1)
    with pytest.raises(RuntimeError, match="apart"):
        remote_fit.confirm(ARGV, "p012", out, Path("lib"))
    assert "confirm" not in json.loads((out / "fit.json").read_text())
    assert not json.loads((out / "confirm_refused.json").read_text())["agrees"]


def test_a_refused_leave_check_sends_nothing(tmp_path, monkeypatch):
    calls = tmp_path / "calls"
    fake = tmp_path / "sendaway"
    fake.write_text(f'#!/bin/sh\necho "$@" >> {calls}\n')
    fake.chmod(0o755)
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "x.json.gz").write_bytes(b"not gzip")
    monkeypatch.setattr(remote_fit, "voice_engine", lambda argv, lib: "modal")
    monkeypatch.setattr(remote_fit, "write_caches", lambda argv, slug, lib: cache)
    with pytest.raises(RuntimeError, match="nothing was sent"):
        remote_fit.run_remote(ARGV, "p012", tmp_path / "out", Path("lib"), 12, sendaway=fake)
    assert not calls.exists()


def test_pull_writes_the_host_files_into_the_output_dir(tmp_path):
    fake = tmp_path / "sendaway"
    body = json.dumps({"fit.json": "{}", "fit.log": "eval #1\n"})
    fake.write_text(f"#!/bin/sh\ncat <<'EOF'\n{body}\nEOF\n")
    fake.chmod(0o755)
    assert remote_fit.pull("p012", tmp_path / "out", fake) == ["fit.json", "fit.log"]
    assert (tmp_path / "out" / "fit.log").read_text() == "eval #1\n"


def _sendaway(tmp_path, files, rc=0):
    fake = tmp_path / "sendaway"
    body = json.dumps(files)
    fake.write_text(f"#!/bin/sh\ncat <<'EOF'\n{body}\nEOF\nexit {rc}\n")
    fake.chmod(0o755)
    return fake


def test_a_search_still_running_on_the_host_is_not_collected(tmp_path):
    fake = _sendaway(tmp_path, {"fit.log": "eval #3\n"})
    with pytest.raises(RuntimeError, match="has not finished"):
        remote_fit.collect("p012", tmp_path / "out", fake)


def test_a_failed_host_search_is_reported_with_its_exit(tmp_path):
    fake = _sendaway(tmp_path, {"host.json": '{"wall_s": 5, "rc": 2}', "fit.log": "x\n"})
    with pytest.raises(RuntimeError, match="exit 2"):
        remote_fit.collect("p012", tmp_path / "out", fake)


def test_a_finished_host_search_is_collected_with_its_wall_time(tmp_path):
    log = "".join(f"  eval #{n} builds=0 loss=1\n" for n in range(1, 11))
    fake = _sendaway(tmp_path, {"host.json": '{"wall_s": 5, "rc": 0}', "fit.log": log})
    got = remote_fit.collect("p012", tmp_path / "out", fake)
    assert got == {
        "host": {
            "wall_s": 5,
            "rc": 0,
            "attempts": [{"log": "fit.log", "evals_new": 10, "evals_replayed": 0}],
            "evals_per_s": 2.0,
        },
        "pulled": ["fit.log", "host.json"],
    }


def _driver(tmp_path, monkeypatch, script):
    """A fake sendaway running `script` for host lines; a pull answers with a finished search."""
    calls = tmp_path / "calls"
    fake = tmp_path / "sendaway"
    bundle = json.dumps(
        {
            "host.json": '{"wall_s": 10, "rc": 0}',
            "fit.attempt1.log": "  eval #1 builds=0 loss=1\n  eval #2 builds=0 loss=1\n",
            "fit.log": "  eval #1 stored loss=1\n  eval #2 stored loss=1\n"
            "  eval #3 builds=0 loss=1\n",
        }
    )
    fake.write_text(
        "#!/bin/bash\n"
        f'if [ "$2" = bash ]; then printf "%s\\n" "$4" >> {calls}; {script}; fi\n'
        f"cat <<'EOF'\n{bundle}\nEOF\n"
    )
    fake.chmod(0o755)
    cache = tmp_path / "cache"
    cache.mkdir()
    leaves = []
    monkeypatch.setattr(remote_fit, "voice_engine", lambda argv, lib: "modal")
    monkeypatch.setattr(remote_fit, "write_caches", lambda argv, slug, lib: cache)
    monkeypatch.setattr(remote_fit, "mac_head", lambda: "abc")
    monkeypatch.setattr(remote_fit, "leave", lambda d: leaves.append(d) or "leave-check PASS")
    return fake, calls, leaves


def test_a_host_lost_on_every_attempt_says_how_to_resume(tmp_path, monkeypatch):
    fake, calls, leaves = _driver(tmp_path, monkeypatch, f"exit {remote_fit.SENDAWAY_LOST}")
    waits = []
    with pytest.raises(RuntimeError, match="run --resume --slug p012"):
        remote_fit.run_remote(
            ARGV, "p012", tmp_path / "out", Path("lib"), 12, sendaway=fake, wait=waits.append
        )
    lines = calls.read_text().splitlines()
    assert len(lines) == len(leaves) == remote_fit.ATTEMPTS
    assert "rm -rf" in lines[0] and not any("rm -rf" in line for line in lines[1:])
    assert waits == [remote_fit.BACKOFF_S * 2**k for k in range(remote_fit.ATTEMPTS - 1)]


def test_a_dropped_connection_is_re_issued_and_each_attempt_counted(tmp_path, monkeypatch):
    lost = remote_fit.SENDAWAY_LOST
    script = f'[ "$(wc -l < {tmp_path / "calls"})" -eq 1 ] && exit {lost}'
    fake, calls, leaves = _driver(tmp_path, monkeypatch, script)
    got = remote_fit.run_remote(
        ARGV, "p012", tmp_path / "out", Path("lib"), 12, sendaway=fake, wait=lambda s: None
    )
    assert len(calls.read_text().splitlines()) == len(leaves) == 2
    first, second = got["host"]["attempts"]
    assert first["log"] == "fit.attempt1.log" and first["rc"] == lost
    assert (first["evals_new"], first["evals_replayed"]) == (2, 0)
    assert second["log"] == "fit.log" and second["rc"] == 0
    assert (second["evals_new"], second["evals_replayed"]) == (1, 2)
    assert got["host"]["evals_per_s"] == 0.1


def test_a_resumed_run_keeps_what_the_host_holds(tmp_path, monkeypatch):
    fake, calls, _ = _driver(tmp_path, monkeypatch, "true")
    remote_fit.run_remote(
        ARGV, "p012", tmp_path / "out", Path("lib"), 12, resume=True, sendaway=fake
    )
    assert "rm -rf" not in calls.read_text()


def test_a_later_attempt_keeps_the_store_and_moves_the_log_to_the_next_free_name(
    tmp_path, monkeypatch
):
    monkeypatch.setattr(remote_fit, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(remote_fit, "HOST_PYTHON", "true")
    out = tmp_path / ".remote-fit-out" / "p012"
    (out / "eval-cache").mkdir(parents=True)
    (out / "eval-cache" / "s.jsonl").write_text("{}\n")
    (out / "fit.attempt1.log").write_text("first\n")
    (out / "fit.log").write_text("second\n")
    line = remote_fit.host_command(ARGV, "p012", 12, "abc", fresh=False)
    line = line.replace("$(uname -s)", "Linux").replace("/proc/loadavg", "/dev/null")
    subprocess.run(["bash", "-c", line], check=False)
    assert (out / "eval-cache" / "s.jsonl").read_text() == "{}\n"
    assert (out / "fit.attempt1.log").read_text() == "first\n"
    assert (out / "fit.attempt2.log").read_text() == "second\n"
    assert (out / "host.json").exists()


def test_the_host_line_clears_the_previous_output_first():
    line = remote_fit.host_command(ARGV, "p012", 12, "abc")
    out = f"{remote_fit.REPO_ROOT}/.remote-fit-out/p012"
    assert line.index(f"rm -rf {out}") < line.index(f"mkdir -p {out}")


def _scored(tmp_path, monkeypatch, evaluations=1):
    seen = []

    def fake_run(cmd, **kw):
        seen.append(cmd)
        out = Path(cmd[cmd.index("--out") + 1])
        out.write_text(json.dumps({"evaluations": evaluations}))
        return subprocess.CompletedProcess(cmd, 0)

    monkeypatch.setattr(remote_fit.subprocess, "run", fake_run)
    return seen


def test_one_confirm_evaluation_drops_the_per_knob_budget_and_the_given_start(
    tmp_path, monkeypatch
):
    seen = _scored(tmp_path, monkeypatch)
    argv = [*ARGV, "--evals-per-knob", "4", "--start-overrides", "a=1"]
    remote_fit.score_once(argv, Path("lib"), tmp_path / "s.json", "b=2")
    cmd = seen[0]
    assert "--evals-per-knob" not in cmd and "a=1" not in cmd
    assert cmd[cmd.index("--start-overrides") + 1] == "b=2"
    assert cmd[cmd.index("--max-evals") + 1] == "1"


def test_a_confirm_evaluation_that_became_a_search_is_refused(tmp_path, monkeypatch):
    _scored(tmp_path, monkeypatch, evaluations=400)
    with pytest.raises(RuntimeError, match="ran 400 evaluations, not 1"):
        remote_fit.score_once(ARGV, Path("lib"), tmp_path / "s.json", None)


def test_the_host_line_writes_its_wall_time_head_and_load(tmp_path, monkeypatch):
    loadavg = tmp_path / "loadavg"
    loadavg.write_text("1.50 2.25 3.00 1/2 345\n")
    monkeypatch.setattr(remote_fit, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(remote_fit, "HOST_PYTHON", "true")
    line = remote_fit.host_command(ARGV, "p012", 12, "abc-dirty").replace("$(uname -s)", "Linux")
    subprocess.run(["bash", "-c", line.replace("/proc/loadavg", str(loadavg))], check=True)
    host = json.loads((tmp_path / ".remote-fit-out" / "p012" / "host.json").read_text())
    assert host["rc"] == 0 and host["mac_head"] == "abc-dirty" and host["wall_s"] >= 0
    assert host["load_start"] == host["load_end"] == [1.5, 2.25, 3.0]


def test_collect_rates_only_what_the_last_attempt_rendered(tmp_path):
    log = "  eval #1 stored loss=1\n" * 300 + "  eval #301 builds=0 loss=1\n" * 100
    fake = _sendaway(tmp_path, {"host.json": '{"wall_s": 50, "rc": 0}', "fit.log": log})
    got = remote_fit.collect("p012", tmp_path / "out", fake)
    assert got["host"]["evals_per_s"] == 2.0
    assert json.loads((tmp_path / "out" / "host.json").read_text())["evals_per_s"] == 2.0


def test_a_tree_without_its_own_sendaway_config_is_refused(tmp_path):
    with pytest.raises(RuntimeError, match="no .sendaway.yml of its own"):
        _real_tree(tmp_path)


def test_an_unmarked_tree_is_refused_and_a_marked_one_passes(tmp_path):
    (tmp_path / ".sendaway.yml").write_text("remote: []\n")
    with pytest.raises(RuntimeError, match="not marked"):
        _real_tree(tmp_path)
    mark = tmp_path / remote_fit.TREE_MARK.relative_to(remote_fit.REPO_ROOT)
    mark.parent.mkdir(parents=True)
    mark.touch()
    assert _real_tree(tmp_path) == tmp_path
