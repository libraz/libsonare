"""The listening run's queue and its renderer.

The queue is the whole point of the run: it decides which voice the ear hears
next, so an ordering slip spends the listener's attention on the long tail
while the piano waits, and a page wrongly reported ready plays a voice that no
longer ships. The renderer is the other half: a render that is retried forever
or outlives its server is a process nobody asked for.
"""

from __future__ import annotations

import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import heard
import wizard

POLICY = {
    "tiers": [
        {
            "name": "core",
            "rank": 1,
            "groups": [
                {"name": "acoustic piano", "programs": [0]},
                {"name": "pipe organ", "programs": [19]},
                {"name": "drums", "kits": [0]},
            ],
        },
        {"name": "common", "rank": 2, "default": True},
    ]
}


def _voice(slug: str, program: int, bank: int = 0, kit: bool = False) -> dict:
    return {"slug": slug, "program": program, "bank": bank, "kit": kit, "name": slug}


VOICES = [
    _voice("p040-violin", 40),
    _voice("kit000-standard-kit", 0, kit=True),
    _voice("p000b008-piano", 0, bank=8),
    _voice("p019-church-organ", 19),
    _voice("p000-piano", 0),
]


def _sets(**states: str) -> dict[str, dict]:
    return {slug: {"id": slug, "path": "", "ships": {"state": s}} for slug, s in states.items()}


def test_the_queue_follows_the_policy_order_not_the_program_numbers():
    """Piano, organ, drums, then the common tier, and the variation after every capital."""
    queue = wizard.build_queue(VOICES, POLICY, {}, {}, {})
    assert [s["set"] for s in queue] == [
        "p000-piano",
        "p019-church-organ",
        "kit000-standard-kit",
        "p040-violin",
        "p000b008-piano",
    ]
    assert queue[1]["group"] == "pipe organ" and queue[3]["tier"] == "common"


def test_a_page_plays_only_when_it_ships_or_was_rendered_this_run():
    sets = _sets(
        **{
            "p000-piano": "current",
            "p019-church-organ": "stale",
            "p040-violin": "checking",
            "kit000-standard-kit": "stale",
        }
    )
    jobs = {"k0": {"state": "done"}, "p19": {"state": "running"}}
    page = {s["set"]: s["page"] for s in wizard.build_queue(VOICES, POLICY, sets, {}, jobs)}
    assert page["p000-piano"] == "ready"
    assert page["p019-church-organ"] == "rendering"
    # Rendered once and still not current: played rather than rendered again.
    assert page["kit000-standard-kit"] == "ready"
    assert page["p040-violin"] == "checking"
    # Never rendered at all: there is no page to play.
    assert page["p000b008-piano"] == "render"


def test_a_voice_with_a_current_verdict_leaves_the_queue():
    """The rule is heard.py's: only a verdict on the recording that ships counts."""
    seen = {}

    def fake(set_id, entries, manifest=None):
        seen[set_id] = len(entries)
        return set_id == "p000-piano"

    real, wizard.heard_current = wizard.heard_current, fake
    try:
        logs = {"p000-piano": [{"grade": "ok"}], "p019-church-organ": [{"grade": "ok"}]}
        queue = wizard.build_queue(VOICES, POLICY, {}, logs, {})
    finally:
        wizard.heard_current = real
    assert "p000-piano" not in [s["set"] for s in queue]
    assert "p019-church-organ" in [s["set"] for s in queue]
    assert seen["p019-church-organ"] == 1


def test_one_job_per_program_because_a_program_run_renders_its_variations():
    assert wizard.job_key(_voice("p000b008-piano", 0, bank=8)) == "p0"
    assert wizard.job_key(_voice("kit000", 0, kit=True)) == "k0"


def test_the_renderer_runs_each_job_once_and_reports_a_failure():
    r = wizard.Renderer(
        lib=Path("/nonexistent.dylib"),
        runner=[sys.executable, "-c", "import sys; sys.exit(3)", "--"],
    )
    voice = _voice("p019-church-organ", 19)
    real = heard.SCRATCH_ROOT
    with tempfile.TemporaryDirectory() as tmp:
        heard.SCRATCH_ROOT = Path(tmp)
        try:
            r.want(voice)
            r.want(voice)
            deadline = time.time() + 20
            while time.time() < deadline and r.snapshot()["p19"]["state"] in ("queued", "running"):
                time.sleep(0.05)
        finally:
            heard.SCRATCH_ROOT = real
    job = r.snapshot()["p19"]
    assert job["state"] == "failed", job
    assert job["log"].endswith("p19.log")
    assert len(r.snapshot()) == 1
    # A second request after the failure starts nothing: once per run.
    r.want(voice)
    assert r.snapshot()["p19"]["state"] == "failed"


def test_the_render_command_names_the_slot_and_the_library():
    r = wizard.Renderer(lib=Path("/lib/libsonare.dylib"), runner=["py"])
    assert r.command(_voice("kit", 0, kit=True))[-4:] == [
        "--kits",
        "0",
        "--lib",
        "/lib/libsonare.dylib",
    ]
    cmd = r.command(_voice("p", 19))
    assert cmd[cmd.index("--program") + 1] == "19"


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"FAIL {t.__name__}: {type(e).__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
