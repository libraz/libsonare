"""Reference and model signals, cached, with one subprocess per override set.

The tuning override layer reads its table when the library loads, so a candidate
cannot be evaluated in the process that evaluated the last one. Every model
render therefore goes through a fresh interpreter with `SONARE_TUNING_OVERRIDES`
in its environment; the cost is a process launch per candidate, which is small
against the render itself and is what makes the search parallelisable at all.

Reference signals come from the capture manifest through `corpus.load_corpus`,
so the note grid, the velocities, the gate and the sample rate are the capture's
and not this module's. Nothing here knows what instrument it is looking at.
"""

from __future__ import annotations

import hashlib
import itertools
import json
import os
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path

import numpy as np

_WORKER = r'''
import json, sys
import numpy as np
sys.path.insert(0, "tools"); sys.path.insert(0, "tools/voicematch")
pairs = [tuple(p) for p in json.loads(sys.argv[1])]
out, root, program, gate_s, seconds = sys.argv[2], sys.argv[3], int(sys.argv[4]), \
    float(sys.argv[5]), float(sys.argv[6])
channel = int(sys.argv[7])
if root:
    from corpus import load_corpus
    from wavio import read_wav
    c = load_corpus(root)
    def get(n, v):
        x, _ = read_wav(c.renders[(n, v)])
        m = np.asarray(x, dtype=np.float64)
        return (m.mean(axis=1) if m.ndim > 1 else m).astype(np.float32)
else:
    from render_model import render_model
    from smf import Note, write_smf
    def get(n, v):
        smf = write_smf([Note(n, v, 0.1, gate_s)], program=program, end_pad=2.0,
                        channel=channel)
        a = np.asarray(render_model(smf, seconds, 48000), dtype=np.float32)
        return a.mean(axis=1) if a.ndim > 1 else a
np.savez(out, **{f"{n}_{v}": get(n, v) for n, v in pairs})
'''


@dataclass
class Signals:
    """Renders a (note, velocity) grid, from the capture or from the model.

    `program`, `channel` and `gate_s` come from the capture definition rather
    than from a default, because a capture that names a GM program is the only
    statement in the tree about which program the model answers it with. A
    harness that hardcodes program zero can compare exactly one instrument, and
    has.

    `channel` is the one that cannot be left at its default at all. MIDI channel
    10 is what makes a note number select an instrument instead of a pitch, so a
    kit rendered on channel 1 answers note 42 as F#2 on whatever program was
    named — a piano — while the reference side holds a hi-hat. Nothing about
    that fails: both sides render, every term returns a number, and the
    comparison is between two different instruments.
    """

    corpus_root: Path
    program: int
    gate_s: float
    seconds: float
    #: Zero-based MIDI channel the model renders on (9 = the GM drum channel).
    channel: int = 0
    lib_path: str = ""
    cache_dir: Path = Path("/tmp/voicematch-shape")

    def __post_init__(self) -> None:
        self.cache_dir = Path(self.cache_dir)
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self._mem: dict[str, dict] = {}
        self._serial = itertools.count()
        self._tunable: bool | None = None
        self._tunable_lock = threading.Lock()

    def _key(self, pairs, ov: str, ref: bool) -> str:
        blob = json.dumps([sorted(pairs), ov, ref, str(self.corpus_root),
                           self.program, self.channel, self.gate_s, self.seconds,
                           self.lib_path])
        return hashlib.sha1(blob.encode()).hexdigest()[:16]

    def _render(self, pairs, ov: str, ref: bool, out_path: Path,
                extra_env: dict | None = None) -> None:
        env = dict(os.environ)
        if self.lib_path:
            env["SONARE_LIB_PATH"] = self.lib_path
        if ov:
            env["SONARE_TUNING_OVERRIDES"] = ov
        else:
            env.pop("SONARE_TUNING_OVERRIDES", None)
        env.update(extra_env or {})
        tmp = out_path.with_suffix(".partial.npz")
        p = subprocess.run(
            [sys.executable, "-c", _WORKER, json.dumps(pairs), str(tmp),
             str(self.corpus_root) if ref else "", str(self.program),
             str(self.gate_s), str(self.seconds), str(self.channel)],
            capture_output=True, text=True, env=env)
        if p.returncode:
            tmp.unlink(missing_ok=True)
            raise RuntimeError(p.stderr[-4000:])
        tmp.replace(out_path)

    def assert_tunable(self) -> None:
        """Refuse to score an override set against a library that ignores it.

        A tuning override reaches the render only from a `-DBUILD_TUNING=ON`
        build; anywhere else the environment variable is read by nobody and
        every candidate renders the shipped voice. Nothing about that fails.
        The search runs, the descent accepts no move because no move changes
        anything, and an ablation prices all of them at exactly zero -- which
        reads as "these constants do nothing", the opposite of the truth.

        The tree makes this easy to hit, because the library sits in several
        build directories at once and only one of them is the tuning build. A
        run that omits `--lib` takes whichever the loader prefers, and a build
        directory reconfigured between two runs changes the answer without
        changing the command.

        Checked by rendering one note with `SONARE_TUNING_DUMP` pointed at a
        scratch file: only a tuning build writes it, so a build that cannot read
        an override cannot produce it either. One render per process, taken the
        first time an override is actually used.
        """
        with self._tunable_lock:
            if self._tunable is not None:
                if not self._tunable:
                    raise RuntimeError(self._not_tunable)
                return
            probe = self.cache_dir / f"tunable-{os.getpid()}.npz"
            dump = self.cache_dir / f"tunable-{os.getpid()}.txt"
            try:
                self._render([(60, 100)], "", False, probe,
                             extra_env={"SONARE_TUNING_DUMP": str(dump)})
                self._tunable = dump.exists() and dump.stat().st_size > 0
            finally:
                probe.unlink(missing_ok=True)
                dump.unlink(missing_ok=True)
            if not self._tunable:
                raise RuntimeError(self._not_tunable)

    @property
    def _not_tunable(self) -> str:
        where = self.lib_path or "the library the loader picked (SONARE_LIB_PATH unset)"
        return (f"{where} was not built with -DBUILD_TUNING=ON, so every override "
                "would render the shipped voice and score as inert. Point --lib at "
                "a tuning build.")

    def __call__(self, pairs, ov: str = "", ref: bool = False) -> dict:
        """(note, velocity) -> mono signal.

        The reference is kept; a candidate is not. A note grid at this length is
        about fifty megabytes of audio, and a descent evaluates thousands of
        candidates -- cached to disk that is hundreds of gigabytes for renders
        that are each read exactly once, which a first run duly wrote sixty-four
        of before anyone looked. The reference is the opposite case: one grid,
        read by every evaluation, and worth keeping between runs.
        """
        pairs = [tuple(p) for p in pairs]
        if ov and not ref:
            self.assert_tunable()
        key = self._key(pairs, ov, ref)
        if ref:
            if key in self._mem:
                return self._mem[key]
            path = self.cache_dir / f"ref-{key}.npz"
            if not path.exists():
                self._render(pairs, ov, ref, path)
            with np.load(path) as z:
                out = {k: z[f"{k[0]}_{k[1]}"] for k in pairs}
            self._mem[key] = out
            return out
        # A unique name per call rather than per override set: several threads
        # can be evaluating the same candidate, and a shared path would have one
        # reading a file another is still writing.
        path = self.cache_dir / f"cand-{key}-{os.getpid()}-{next(self._serial)}.npz"
        try:
            self._render(pairs, ov, ref, path)
            with np.load(path) as z:
                return {k: z[f"{k[0]}_{k[1]}"] for k in pairs}
        finally:
            path.unlink(missing_ok=True)


def read_overrides(text: str) -> dict[str, float]:
    """Parse a `key=value,key=value` override string."""
    out = {}
    for kv in text.strip().split(","):
        if kv.strip():
            k, v = kv.split("=")
            out[k.strip()] = float(v)
    return out


def write_overrides(state: dict[str, float], base: dict[str, float]) -> str:
    """Render only the coordinates that differ from the shipped defaults.

    Emitting the whole table would work and would also make every log line
    unreadable and every cache key unique, so a run that changed nothing would
    still miss the cache. The difference is also the reviewable artefact: it is
    the list of constants a change would have to justify.
    """
    return ",".join(f"{k}={v!r}" for k, v in sorted(state.items())
                    if k not in base or v != base[k])


def load_knob_dump(path: Path | str, namespaces: tuple[str, ...] = ()) -> dict[str, float]:
    """Every tunable a render consulted, with its compiled-in default.

    Written by the library itself under `SONARE_TUNING_DUMP`, so the coordinate
    list comes from the code rather than from a parse of it. A knob added to a
    voice is swept without anyone remembering to add it here.
    """
    out: dict[str, float] = {}
    for line in Path(path).read_text().splitlines():
        parts = line.strip().split("\t")
        if len(parts) != 2 or line.startswith("#"):
            continue
        name, value = parts
        if namespaces and not name.startswith(namespaces):
            continue
        try:
            out[name] = float(value)
        except ValueError:
            continue
    return out
