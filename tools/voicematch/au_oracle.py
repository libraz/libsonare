"""Oracle audio from a real AudioUnit instrument, rendered offline by aubounce.

This is route C of `render_oracle`, wired: instead of fluidsynth playing a
SoundFont, an AudioUnit plugin installed on this machine plays the probe and
the render comes back over the same interface — SMF bytes in, a (frames, 2)
float32 array covering exactly the probe's timeline out. Everything downstream
(`voicematch compare`, `autofit`, `room`) stays route-blind.

`aubounce` (https://github.com/libraz/aubounce) is the host. It is a separate
binary rather than a library because a plugin is not reliably identical to
itself across instances, and one render per process is what keeps a batch
comparable.

Three failure modes of a disk-streaming sampler are guarded here, because each
one produces a *plausible* file rather than an error:

- **Rendered faster than real time**, the plugin plays the part it holds in
  memory and goes silent mid-note. aubounce reports that as `dropout_ms`;
  anything non-zero is refused.
- **Settled too briefly**, the plugin accepts its preset and renders near
  silence at the right length and the right shape. A peak below `min_peak` is
  refused.
- **Leaking on load**, energy appears in the preroll window that no note
  explains. That is reported rather than refused, since a plugin is allowed a
  tail from whatever it was doing.

A fourth is prevented rather than detected, because there is nothing in the
file to detect it by: a large sampler plays the **first note it is asked for**
differently from every later one, at the right length, with a clean preroll and
a peak that comes out higher rather than lower. `warmup` strikes one note and
throws it away before recording, which is aubounce's `--warmup`.

Measured on Steinberg The Grand 3 (see `capture.py` for the recipe): without
`--realtime` a 2 s C4 drops out for 1544 ms; at `--settle-ms 1000` it renders
a peak of 0.0011 — a file that looks entirely reasonable and contains no note.
At 2000 ms and above the renders are identical to each other. The defaults
here are twice that minimum.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from wavio import read_wav

HERE = Path(__file__).resolve().parent
DEFAULT_CACHE_DIR = HERE / "out" / "au_cache"

# Where macOS keeps the presets a plugin installs alongside itself.
PRESET_ROOTS = (
    Path("/Library/Audio/Presets"),
    Path.home() / "Library" / "Audio" / "Presets",
)

# Effect sections to switch off for a dry capture. A name that the plugin does
# not advertise is skipped rather than failing, so one list covers every
# instrument.
DRY_PARAM_CANDIDATES = (
    "Reverb On/Off",
    "Equalizer On/Off",
    "Modulation On/Off",
    "Tremolo On/Off",
)


class AuRenderError(RuntimeError):
    """An AU render that produced a file the harness must not fit against."""


def find_aubounce() -> Path:
    """Locate the aubounce binary: AUBOUNCE, then PATH, then a sibling checkout."""
    env = os.environ.get("AUBOUNCE")
    if env:
        p = Path(env).expanduser()
        if not p.exists():
            raise FileNotFoundError(f"AUBOUNCE points at a missing file: {p}")
        return p
    which = shutil.which("aubounce")
    if which:
        return Path(which)
    from _repo import REPO_ROOT

    for rel in ("target/release/aubounce", "target/debug/aubounce"):
        p = REPO_ROOT.parent / "aubounce" / rel
        if p.exists():
            return p
    raise FileNotFoundError(
        "aubounce not found. Install it (`cargo install --git "
        "https://github.com/libraz/aubounce`), put it on PATH, or set AUBOUNCE "
        "to the binary."
    )


def resolve_preset(spec: str) -> Path:
    """Resolve a `.vstpreset` path, or a unique fragment of one, to a file.

    A full path is taken as given. Anything else is matched case-insensitively
    against every preset under the macOS preset roots, and has to select
    exactly one — an ambiguous fragment lists the candidates rather than
    picking the first, since which piano was captured is the whole point.
    """
    if not spec:
        return Path()
    direct = Path(spec).expanduser()
    if direct.exists():
        return direct
    # macOS stores a filename decomposed, so a preset written "Bosendorfer" with
    # a combining diaeresis on disk does not contain the composed character this
    # spec was typed with. Both sides are normalised or the two never meet.
    needle = unicodedata.normalize("NFC", spec).lower()
    hits = [
        p
        for root in PRESET_ROOTS
        if root.is_dir()
        for p in root.rglob("*.vstpreset")
        if needle in unicodedata.normalize("NFC", str(p)).lower()
    ]
    if not hits:
        raise FileNotFoundError(f"no preset matches {spec!r} under {', '.join(map(str, PRESET_ROOTS))}")
    if len(hits) > 1:
        listing = "\n  ".join(str(p) for p in sorted(hits)[:12])
        more = f"\n  ... and {len(hits) - 12} more" if len(hits) > 12 else ""
        raise ValueError(f"preset fragment {spec!r} matches {len(hits)} files:\n  {listing}{more}")
    return hits[0]


@dataclass(frozen=True)
class AuSource:
    """Everything that decides what an AU render sounds like.

    Frozen and fully serialisable on purpose: its digest is the cache key, so a
    changed preset, a changed parameter or a changed settle time is a different
    recording rather than a stale hit.
    """

    plugin: str
    preset: str = ""
    state: str = ""
    params: tuple[str, ...] = ()
    program: int | None = None
    channel: int = 1
    settle_ms: int = 4000
    realtime: bool = True
    #: Strike the probe's first note once and discard it before recording.
    #:
    #: On by default because the failure it prevents is silent and lands on the
    #: measurement: a large sampler plays the first note it is asked for
    #: differently from every later one, and a probe sweeps velocity upwards, so
    #: what gets corrupted is the softest hit — the axis a drum fit is validated
    #: along. Measured on HALion 7, one drum note struck at 64/100/127: without
    #: it the first strike reads peak 0.939 and RMS 0.081 where the same
    #: velocity reads 0.610 and 0.117 once the plugin has been struck.
    warmup: bool = True
    preroll_ms: int = 100
    tail: str = ""
    sample_rate: int = 48000
    min_peak: float = 0.005
    extra: tuple[str, ...] = field(default=())

    def identity(self) -> dict:
        """The dict that goes into the cache key and the capture manifest.

        The preset's digest is in here rather than its path: a preset edited in
        place is a different sound at the same path, and a cache keyed on the
        path would hand back the old recording of it.
        """
        preset = resolve_preset(self.preset) if self.preset else None
        identity = {
            "plugin": self.plugin,
            "preset": str(preset) if preset else "",
            "preset_sha256": _sha256_file(preset) if preset and preset.is_file() else "",
            "state": self.state,
            "params": list(self.params),
            "program": self.program,
            "settle_ms": self.settle_ms,
            "realtime": self.realtime,
            "warmup": self.warmup,
            "preroll_ms": self.preroll_ms,
            "tail": self.tail,
            "sample_rate": self.sample_rate,
            "extra": list(self.extra),
        }
        # Recorded only when it is not the default. The channel is a later
        # addition, and putting it in unconditionally would change the digest
        # of every recording made before it existed — a cache miss and a
        # re-render for captures whose sound did not move.
        if self.channel != 1:
            identity["channel"] = self.channel
        return identity

    def argv(self, out_wav: Path, *, midi: Path | None = None) -> list[str]:
        """The aubounce command line for one render."""
        argv = [str(find_aubounce()), "bounce", self.plugin]
        if self.preset:
            argv += ["--preset", str(resolve_preset(self.preset))]
        if self.state:
            argv += ["--state", self.state]
        for spec in self.params:
            argv += ["--param", spec]
        if self.program is not None:
            argv += ["--program", str(self.program)]
        # A multitimbral rack saved as one file answers to a different sound on
        # each of its channels, so there the channel is what selects a timbre,
        # where a single-timbre plugin is selected with a preset.
        if self.channel != 1:
            argv += ["--channel", str(self.channel)]
        if midi is not None:
            argv += ["--midi", str(midi)]
        argv += [
            "--sample-rate", str(self.sample_rate),
            "--preroll-ms", str(self.preroll_ms),
            "--settle-ms", str(self.settle_ms),
        ]
        if self.tail:
            argv += ["--tail", self.tail]
        if self.realtime:
            argv.append("--realtime")
        if self.warmup:
            argv.append("--warmup")
        argv += list(self.extra)
        argv += ["--json", "-o", str(out_wav)]
        return argv


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def dry_params(plugin: str, *, candidates=DRY_PARAM_CANDIDATES) -> tuple[str, ...]:
    """`--param NAME=0` for every effect section this plugin actually has.

    Asks the plugin what it advertises rather than assuming, so the same call
    produces a dry capture of an instrument with a reverb, an instrument with a
    reverb and a chorus, and an instrument with neither.
    """
    proc = subprocess.run(
        [str(find_aubounce()), "info", plugin, "--params"],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise AuRenderError(f"aubounce info failed for {plugin!r}:\n{proc.stderr.strip()}")
    advertised = proc.stdout
    return tuple(f"{name}=0" for name in candidates if name in advertised)


def bounce(
    source: AuSource,
    out_wav: Path,
    *,
    midi: Path | None = None,
    verbose: bool = False,
    attempts: int = 4,
) -> dict:
    """Render through aubounce and return its JSON summary, guarded and retried.

    Raises `AuRenderError` on the two failures that leave a plausible file
    behind (a dropout, and a render too quiet to be the instrument).

    Retried because on The Grand 3 the quiet one is a race inside the plugin
    rather than a setting: the same render succeeds, then comes back near
    silent, then succeeds again, at roughly one failure in three, and a longer
    settle does not reduce it. What is not retried away is the detection — a
    render that never loads raises rather than being fitted against.
    """
    out_wav.parent.mkdir(parents=True, exist_ok=True)
    last: AuRenderError | None = None
    for attempt in range(attempts):
        try:
            return _bounce_once(source, out_wav, midi=midi, verbose=verbose)
        except AuRenderError as exc:
            last = exc
            if verbose or attempt:
                print(f"  retrying ({attempt + 1}/{attempts}): {exc}", file=sys.stderr)
    raise last if last else AuRenderError("no attempts were made")


def _bounce_once(
    source: AuSource,
    out_wav: Path,
    *,
    midi: Path | None = None,
    verbose: bool = False,
) -> dict:
    argv = source.argv(out_wav, midi=midi)
    if verbose:
        print("  " + " ".join(repr(a) if " " in a else a for a in argv), file=sys.stderr)
    proc = subprocess.run(argv, capture_output=True, text=True)
    if proc.returncode != 0:
        raise AuRenderError(
            f"aubounce failed (rc={proc.returncode}) for {source.plugin!r}:\n{proc.stderr.strip()}"
        )
    try:
        summary = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:  # pragma: no cover - aubounce contract
        raise AuRenderError(f"aubounce did not emit JSON:\n{proc.stdout}\n{proc.stderr}") from exc

    if summary.get("dropout_ms", 0):
        raise AuRenderError(
            f"{out_wav.name}: the plugin went silent for {summary['dropout_ms']} ms inside a note "
            f"it had already started. A disk-streaming sampler does this when it is driven faster "
            f"than real time — render with realtime=True (it is the default here)."
        )
    peak = float(summary.get("peak", 0.0))
    if peak < source.min_peak:
        raise AuRenderError(
            f"{out_wav.name}: peak {peak:.5f} is below {source.min_peak}. The render has the right "
            f"length and no error and contains no instrument: the plugin's samples had not arrived "
            f"when the first note played. Below the plugin's minimum settle time this happens every "
            f"render and settle_ms (currently {source.settle_ms}) is the fix — run "
            f"`capture.py calibrate` to measure that minimum. Above it, it is a race that a retry "
            f"clears and more settling does not: on The Grand 3, 4000 ms and 16000 ms both gave the "
            f"real 0.0122 while 8000 ms in between gave 0.0010."
        )
    if float(summary.get("preroll_peak", 0.0)) > 1e-4:
        print(
            f"note: {out_wav.name} has {summary['preroll_peak']:.5f} peak in the preroll window, "
            f"before any note — the plugin is leaking on load or streaming in late.",
            file=sys.stderr,
        )
    if int(summary.get("sample_rate", source.sample_rate)) != source.sample_rate:
        raise AuRenderError(
            f"{out_wav.name}: rendered at {summary['sample_rate']} Hz, asked for {source.sample_rate}"
        )
    return summary


def render_oracle_au(
    smf_bytes: bytes,
    total_seconds: float,
    sr: int = 48000,
    *,
    source: AuSource,
    cache_dir: Path | None = DEFAULT_CACHE_DIR,
    verbose: bool = False,
) -> np.ndarray:
    """Render SMF bytes through an AudioUnit and return the probe's timeline.

    Interface-compatible with `render_oracle_fluidsynth`. The preroll aubounce
    writes is a *known* offset rather than an estimated one, so the result is
    aligned to the score by construction and never goes near
    `estimate_alignment` — which matters for a piano, where the score's note
    onsets and the render's onset-strength peaks are the only thing an
    estimator has and a soft note can hide under the previous note's decay.
    """
    from render_oracle import fit_length

    if source.sample_rate != sr:
        source = AuSource(**{**source.__dict__, "sample_rate": sr})

    key = hashlib.sha256(
        json.dumps(
            {"smf": hashlib.sha256(smf_bytes).hexdigest(), "src": source.identity(), "sr": sr},
            sort_keys=True,
        ).encode()
    ).hexdigest()[:16]

    cached = (cache_dir / f"{key}.wav") if cache_dir else None
    if cached is not None and cached.exists():
        if verbose:
            print(f"oracle: cache hit {cached}", file=sys.stderr)
        audio, got_sr = read_wav(cached)
        if got_sr == sr:
            return fit_length(_strip_preroll(audio, source.preroll_ms, sr), total_seconds, sr)

    with tempfile.TemporaryDirectory(prefix="au_oracle_") as tmp:
        mid = Path(tmp) / "probe.mid"
        wav = Path(tmp) / "probe.wav"
        mid.write_bytes(smf_bytes)
        summary = bounce(source, wav, midi=mid, verbose=verbose)
        if verbose:
            print(
                f"oracle: {source.plugin} peak {summary['peak']:.4f}, "
                f"{summary['seconds']:.2f}s rendered",
                file=sys.stderr,
            )
        audio, got_sr = read_wav(wav)
        if cached is not None:
            cached.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(wav, cached)
            (cached.with_suffix(".json")).write_text(
                json.dumps({"source": source.identity(), "summary": summary}, indent=2) + "\n"
            )
    if got_sr != sr:
        raise AuRenderError(f"aubounce rendered at {got_sr} Hz, expected {sr}")
    return fit_length(_strip_preroll(audio, source.preroll_ms, sr), total_seconds, sr)


def _strip_preroll(audio: np.ndarray, preroll_ms: int, sr: int) -> np.ndarray:
    """Drop the silence aubounce writes before the first event."""
    if audio.ndim == 1:
        audio = audio[:, None]
    n = int(round(preroll_ms * sr / 1000.0))
    return audio[n:] if n < audio.shape[0] else audio[:0]


def add_au_args(parser) -> None:
    """Register the AU-oracle flags, alongside `render_oracle.add_oracle_args`."""
    parser.add_argument("--au", default="", dest="au",
                        help="render the oracle with this AudioUnit instrument "
                             "(name or type:subtype:manufacturer triple) via aubounce")
    parser.add_argument("--au-preset", default="", dest="au_preset",
                        help="a .vstpreset path, or a unique fragment of one "
                             "(e.g. 'Yamaha C7/Close/Natural Ambience')")
    parser.add_argument("--au-param", action="append", default=[], dest="au_param",
                        help="plugin parameter as 'name=value'; repeatable")
    parser.add_argument("--au-dry", action="store_true", dest="au_dry",
                        help="switch off every effect section the plugin advertises")
    parser.add_argument("--au-settle-ms", type=int, default=4000, dest="au_settle_ms",
                        help="main-thread time before the first note (default 4000; "
                             "a large sampler renders near silence below ~2000)")
    parser.add_argument("--au-no-realtime", action="store_true", dest="au_no_realtime",
                        help="drive the plugin as fast as it will go (a disk-streaming "
                             "sampler drops the middle of a note when you do)")
    parser.add_argument("--au-no-warmup", action="store_true", dest="au_no_warmup",
                        help="record the plugin's first note instead of discarding one first "
                             "(a large sampler plays it differently from every later one, and "
                             "the probe's first note is its softest)")
    parser.add_argument("--au-no-cache", action="store_true", dest="au_no_cache",
                        help="re-render instead of reusing an identical earlier render")


def source_from_args(args) -> AuSource | None:
    """Build an `AuSource` from parsed CLI arguments, or None if `--au` was absent."""
    plugin = getattr(args, "au", "")
    if not plugin:
        return None
    params = tuple(getattr(args, "au_param", []) or ())
    if getattr(args, "au_dry", False):
        params = tuple(dict.fromkeys(dry_params(plugin) + params))
    return AuSource(
        plugin=plugin,
        preset=getattr(args, "au_preset", ""),
        params=params,
        settle_ms=getattr(args, "au_settle_ms", 4000),
        realtime=not getattr(args, "au_no_realtime", False),
        warmup=not getattr(args, "au_no_warmup", False),
    )
