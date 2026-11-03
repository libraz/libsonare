"""The listening run: every bank voice in working order, and fresh pages for it.

The QUEUE is the bank in `policy.json`'s working order — tier, the group inside
the tier, then number, every capital and kit before any variation
(`status.placement`). A voice leaves it once it carries a current verdict by
`heard.py`'s own freshness rule, so a voice that moves comes back by itself and
nothing here keeps a second record of what was heard.

The RENDERER keeps the pages the run is about to play current, because a
verdict on a page that trails the bank describes a voice that no longer ships:
one `make_audition.py` job at a time, niced, each program attempted once per
server so a render that still does not come out current is played and
reported rather than retried forever.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import threading
from pathlib import Path

import heard
from _repo import REPO_ROOT

#: The library a render loads. A tuning build is what names the patch and
#: records the render path, without which no note on the page can be dated.
DEFAULT_LIB = REPO_ROOT / "build-tuning" / "lib" / "libsonare.dylib"
MAKE_AUDITION = REPO_ROOT / "tools" / "voicematch" / "make_audition.py"
PYPROJECT = REPO_ROOT / "bindings" / "python" / "pyproject.toml"


def _status():
    tools = REPO_ROOT / "tools" / "voicematch"
    if str(tools) not in sys.path:
        sys.path.append(str(tools))
    import status

    return status


def job_key(voice: dict) -> str:
    """One render job per program or kit: a program's run renders its variations too."""
    return f"k{voice['program']}" if voice.get("kit") else f"p{voice['program']}"


#: `heard_current` by (set, log length, last entry, manifest mtime): a digest
#: reads the registry per note and the queue is asked for every few seconds.
_HEARD: dict[tuple, bool] = {}


def heard_current(set_id: str, entries: list[dict], manifest: Path | None = None) -> bool:
    """True once a verdict on this voice is about the recording that ships now."""
    if not entries:
        return False
    try:
        stamp = manifest.stat().st_mtime_ns if manifest is not None else 0
    except OSError:
        stamp = 0
    key = (set_id, len(entries), str(entries[-1].get("id")), str(entries[-1].get("at")), stamp)
    if key not in _HEARD:
        digest = heard.digest(set_id, entries)
        _HEARD[key] = any(p.get("n_current") for p in digest.get("populations") or [])
    return _HEARD[key]


def ordered(voices: list[dict], policy: dict) -> list[tuple[dict, dict]]:
    """The bank's voices with their placement, in the order they are listened to."""
    status = _status()
    placed = [
        (
            v,
            status.placement(
                policy, int(v["program"]), int(v.get("bank") or 0), kit=bool(v.get("kit"))
            ),
        )
        for v in voices
        if isinstance(v.get("program"), int)
    ]
    placed.sort(key=lambda vp: vp[1]["key"])
    return placed


def build_queue(
    voices: list[dict],
    policy: dict,
    sets: dict[str, dict],
    logs: dict[str, list[dict]],
    jobs: dict[str, dict],
) -> list[dict]:
    """Every voice still owed a verdict, in order, with whether its page can be played.

    `sets` is the served index by id, `logs` the feedback by set id, `jobs` the
    renderer's state by job key. `page` is `ready` (current, or already
    re-rendered once this run), `render` (needs a render nobody has started),
    `rendering` (queued or running), `failed`, or `checking` (the registries are
    still being read, so whether it needs one is not yet known).
    """
    out = []
    for voice, place in ordered(voices, policy):
        slug = voice.get("slug") or ""
        listed = sets.get(slug)
        where = Path(listed["path"]) / "manifest.json" if listed and listed.get("path") else None
        if heard_current(slug, logs.get(slug) or [], where):
            continue
        ships = ((listed or {}).get("ships") or {}).get("state")
        job = jobs.get(job_key(voice)) or {}
        if job.get("state") in ("queued", "running"):
            page = "rendering"
        elif job.get("state") == "failed":
            page = "failed"
        elif listed and (ships == "current" or job.get("state") == "done"):
            page = "ready"
        elif ships == "checking":
            page = "checking"
        else:
            page = "render"
        out.append(
            {
                "set": slug,
                "name": voice.get("name") or slug,
                "program": voice["program"],
                "bank": voice.get("bank") or 0,
                "kit": bool(voice.get("kit")),
                "tier": place["tier"],
                "group": place["group"],
                "page": page,
                "ships": ships or "",
                "noted": len(logs.get(slug) or []),
                "log": job.get("log") or "",
                "error": job.get("error") or "",
            }
        )
    return out


class Renderer:
    """One render at a time, in the order asked for; each job attempted once per run."""

    def __init__(self, lib: Path | None = None, runner: list[str] | None = None) -> None:
        self.lib = lib if lib is not None else (DEFAULT_LIB if DEFAULT_LIB.exists() else None)
        self.runner = runner
        self.jobs: dict[str, dict] = {}
        self._order: list[tuple[str, dict]] = []
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._thread: threading.Thread | None = None
        self._proc: subprocess.Popen | None = None

    def command(self, voice: dict) -> list[str]:
        if self.runner is not None:
            base = list(self.runner)
        elif shutil.which("rye"):
            base = ["rye", "run", "--pyproject", str(PYPROJECT), "python"]
        else:
            base = [sys.executable]
        # Niced: a render shares the machine with whatever else is running.
        nice = ["nice", "-n", "10"] if shutil.which("nice") else []
        cmd = [*nice, *base, str(MAKE_AUDITION)]
        cmd += (
            ["--kits", str(voice["program"])]
            if voice.get("kit")
            else ["--program", str(voice["program"])]
        )
        if self.lib is not None:
            cmd += ["--lib", str(self.lib)]
        return cmd

    def want(self, voice: dict) -> None:
        key = job_key(voice)
        with self._lock:
            if key in self.jobs:
                return
            self.jobs[key] = {"state": "queued", "log": "", "error": ""}
            self._order.append((key, voice))
        self._start()
        self._wake.set()

    def snapshot(self) -> dict[str, dict]:
        with self._lock:
            return {k: dict(v) for k, v in self.jobs.items()}

    def _start(self) -> None:
        if self._thread is None or not self._thread.is_alive():
            self._thread = threading.Thread(target=self._loop, daemon=True)
            self._thread.start()

    def _next(self) -> tuple[str, dict] | None:
        with self._lock:
            return self._order.pop(0) if self._order else None

    def _loop(self) -> None:
        while True:
            job = self._next()
            if job is None:
                self._wake.wait(timeout=30)
                self._wake.clear()
                continue
            self.run(*job)

    def run(self, key: str, voice: dict) -> None:
        log_dir = heard.SCRATCH_ROOT / "wizard"
        log_dir.mkdir(parents=True, exist_ok=True)
        log = log_dir / f"{key}.log"
        with self._lock:
            self.jobs[key].update(state="running", log=str(log))
        env = dict(os.environ)
        if self.lib is not None:
            env["SONARE_LIB_PATH"] = str(self.lib)
        cmd = self.command(voice)
        try:
            with log.open("w", encoding="utf-8") as fh:
                fh.write(" ".join(cmd) + "\n\n")
                fh.flush()
                self._proc = subprocess.Popen(
                    cmd,
                    cwd=REPO_ROOT,
                    env=env,
                    stdout=fh,
                    stderr=subprocess.STDOUT,
                )
                code = self._proc.wait()
                self._proc = None
        except OSError as exc:
            code, why = -1, str(exc)
        else:
            why = ""
        if code != 0 and not why:
            tail = log.read_text(encoding="utf-8", errors="replace").strip().splitlines()
            why = tail[-1] if tail else f"exit {code}"
        with self._lock:
            self.jobs[key].update(state="done" if code == 0 else "failed", error=why)

    def stop(self) -> None:
        """End the render in flight: a render must not outlive the server that asked for it."""
        proc = self._proc
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
