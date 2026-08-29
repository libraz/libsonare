"""Oracle-side audio: the reference a voice is fitted against.

Route A (default, always available): fluidsynth fast-render with a GM
SoundFont (assets/MuseScore_General.sf3 unless overridden via --sf2 or
VOICEMATCH_SF2).

Route B: a WAV file the user rendered elsewhere — a VST in a DAW, a plugin
host, or a recording of a real instrument playing the probe. Export the probe
SMF with `voicematch.py export-probe`, render it wherever the reference lives,
and hand the result back with `--oracle-wav`. `load_oracle_wav` takes care of
what an external render does not guarantee: sample rate, bit depth, channel
count, a leading silence of unknown length, and a tail that runs long or short.

Route C (`--au`, `au_oracle.py`): an AudioUnit instrument installed on this
machine, played here by aubounce. Route B without the manual step, and without
the estimated alignment — the host writes a known preroll, so the render is
tied to the score by construction.

All three routes return the same thing — a (frames, channels) float32 array
covering exactly the probe's timeline — so everything downstream is
route-blind.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

from wavio import read_wav

ASSETS_DIR = Path(__file__).resolve().parent / "assets"
DEFAULT_SF2 = ASSETS_DIR / "MuseScore_General.sf3"
DEFAULT_AU_CACHE = Path(__file__).resolve().parent / "out" / "au_cache"


def default_soundfont() -> Path:
    """Resolve the oracle SoundFont (env override, then the bundled asset)."""
    env = os.environ.get("VOICEMATCH_SF2")
    if env:
        return Path(env)
    return DEFAULT_SF2


def render_oracle_fluidsynth(
    smf_bytes: bytes,
    total_seconds: float,
    sr: int = 48000,
    *,
    soundfont: Path | None = None,
    gain: float = 0.5,
) -> np.ndarray:
    """Render SMF bytes to a (frames, 2) float32 array via fluidsynth.

    fluidsynth stops at the SMF end-of-track marker, so the caller must have
    written the file with enough `end_pad` to cover the release tail. Output is
    trimmed/zero-padded to exactly `total_seconds`.
    """
    sf2 = soundfont if soundfont is not None else default_soundfont()
    if not sf2.exists():
        raise FileNotFoundError(
            f"oracle SoundFont not found: {sf2} (set VOICEMATCH_SF2 or pass --sf2)"
        )
    with tempfile.TemporaryDirectory(prefix="voicematch_") as tmp:
        mid_path = Path(tmp) / "in.mid"
        wav_path = Path(tmp) / "out.wav"
        mid_path.write_bytes(smf_bytes)
        cmd = [
            "fluidsynth",
            "-ni",                # no shell, no MIDI driver
            "-r", str(sr),
            "-g", str(gain),
            "-R", "0",            # dry render: reverb/chorus tails would
            "-C", "0",            # contaminate release and TNR metrics
            "-F", str(wav_path),  # fast render to file
            str(sf2),
            str(mid_path),
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0 or not wav_path.exists():
            raise RuntimeError(f"fluidsynth failed (rc={proc.returncode}):\n{proc.stderr.strip()}")
        audio, got_sr = read_wav(wav_path)
    if got_sr != sr:
        raise RuntimeError(f"fluidsynth rendered at {got_sr} Hz, expected {sr}")
    return fit_length(audio, total_seconds, sr)


def fit_length(audio: np.ndarray, total_seconds: float, sr: int) -> np.ndarray:
    """Trim or zero-pad a (frames, channels) render to exactly `total_seconds`."""
    want = int(round(total_seconds * sr))
    if audio.shape[0] >= want:
        return audio[:want]
    pad = np.zeros((want - audio.shape[0], audio.shape[1]), dtype=np.float32)
    return np.concatenate([audio, pad], axis=0)


def resample_linear(audio: np.ndarray, src_sr: int, dst_sr: int) -> np.ndarray:
    """Windowed-sinc resample of a (frames, channels) array.

    Only ever a fallback: a rate-converted oracle carries the converter's own
    passband error into every harmonic the fit reads, so rendering the probe at
    the harness rate is always the better answer. Good enough that the error
    sits well below the metrics' resolution (a 32-tap Blackman-windowed kernel,
    with the cutoff lowered on downsampling to keep aliasing out).
    """
    if src_sr == dst_sr:
        return audio
    ratio = dst_sr / src_sr
    n_out = int(round(audio.shape[0] * ratio))
    taps = 32
    cutoff = min(1.0, ratio)  # anti-alias when decimating
    src_pos = np.arange(n_out) / ratio
    base = np.floor(src_pos).astype(np.int64)
    out = np.zeros((n_out, audio.shape[1]), dtype=np.float32)
    offsets = np.arange(-taps // 2 + 1, taps // 2 + 1)
    window = 0.42 - 0.5 * np.cos(2 * np.pi * (np.arange(taps) + 0.5) / taps) + \
        0.08 * np.cos(4 * np.pi * (np.arange(taps) + 0.5) / taps)
    for ch in range(audio.shape[1]):
        col = audio[:, ch]
        acc = np.zeros(n_out, dtype=np.float64)
        norm = np.zeros(n_out, dtype=np.float64)
        for k, off in enumerate(offsets):
            idx = np.clip(base + off, 0, len(col) - 1)
            t = src_pos - (base + off)
            w = cutoff * np.sinc(cutoff * t) * window[k]
            acc += col[idx] * w
            norm += w
        out[:, ch] = (acc / np.maximum(np.abs(norm), 1e-9)).astype(np.float32)
    return out


def add_oracle_args(parser) -> None:
    """Register the oracle-source flags shared by `voicematch` and `autofit`."""
    from au_oracle import add_au_args

    parser.add_argument("--sf2", default="",
                        help="oracle SoundFont path (default: assets/MuseScore_General.sf3)")
    parser.add_argument("--oracle-wav", default="", dest="oracle_wav",
                        help="use an externally rendered WAV of the probe instead of fluidsynth "
                             "(export the probe with `voicematch.py export-probe`)")
    parser.add_argument("--oracle-offset", type=float, default=None, dest="oracle_offset",
                        help="seconds of lead-in to strip from --oracle-wav (default: estimated)")
    parser.add_argument("--oracle-no-align", action="store_true", dest="oracle_no_align",
                        help="take --oracle-wav as-is instead of aligning it to the score")
    parser.add_argument("--oracle-resample", action="store_true", dest="oracle_resample",
                        help="resample --oracle-wav if its rate differs from the harness rate")
    add_au_args(parser)


def oracle_may_carry_room(args) -> bool:
    """Whether this oracle route can arrive with a room baked into it.

    A WAV rendered elsewhere always can, and an AudioUnit does unless every
    effect section was switched off with `--au-dry` — the plugins most worth
    fitting against ship with a hall on by default, which is exactly the case
    the room correction exists for. Only the built-in fluidsynth route is dry by
    construction (`-R 0 -C 0`), and estimating a room from it would invent one.

    A false positive costs nothing: `estimate_room` reports a dry measurement as
    dry and the caller then skips the correction.
    """
    if getattr(args, "oracle_wav", ""):
        return True
    return bool(getattr(args, "au", "")) and not getattr(args, "au_dry", False)


def check_oracle_rig(args, program: int, *, allow: bool = False) -> None:
    """What each captureless oracle route may be fitted against, on a rig-capable family.

    A capture answers the rig question in its own definition and a fit is refused
    on any answer but `none` (`corpus.check_rig`). None of the three routes here
    carries that record, and they do not all mean the same thing by not carrying
    it — so they are not treated the same.

    Routes B and C are **unknown**: a cabinet is a filter rather than a space, so
    an amplified take passes every dryness test with the whole rig inside it, and
    nothing downstream can recover what the chain was. Warned, not refused —
    there is no ground to stop on — but not passed over in silence either, since
    being unclassifiable and being safe are different things.

    Route A is **known**, and that is the difference. General MIDI defines these
    programs by the sound of an amplified instrument, so a GM sample set's
    recording of one has a cabinet in it by construction. Refused on the same
    terms as a `baked` capture, since it is one.

    The certainty is not uniform across the family — 29 and 30 are definitional
    while 33-37 are only usual — but a second table splitting the sure from the
    likely would drift, and erring strict costs one `--allow-rigged-oracle`.
    """
    from capture import rig_capable

    if not rig_capable(program):
        return
    external = ("--oracle-wav" if getattr(args, "oracle_wav", "")
                else f"--au {getattr(args, 'au', '')}" if getattr(args, "au", "") else "")
    if external:
        print(f"{external} carries no record of a rig, and program {program} is a family "
              f"that can have one: if this reference was recorded through an amplifier, the "
              f"fit reproduces the amplifier with the instrument's own parameters and the "
              f"values are lost the moment the rig becomes a stage of its own. Only a "
              f"capture can answer this.", file=sys.stderr)
        return
    why = (
        f"the built-in GM oracle cannot be a DI for program {program}: general MIDI defines "
        f"these programs by the sound of an amplified instrument, so a sample set's "
        f"recording of one has the cabinet in it — known from what the program IS rather "
        f"than merely unrecorded, which is why no capture field can change this answer. A "
        f"rig has no inverse, so the fit would reproduce the amplifier with the "
        f"instrument's own parameters. Fit against a reference captured at the instrument's "
        f"own boundary instead (`--corpus` on a capture that says \"rig\": \"none\")"
    )
    if allow:
        print(f"--allow-rigged-oracle: {why}. Proceeding; the values this produces "
              f"transfer to nothing once the rig is a stage of its own.", file=sys.stderr)
        return
    raise ValueError(f"{why}. --allow-rigged-oracle overrides.")


def obtain_oracle(args, smf_bytes: bytes, total_seconds: float, sr: int, onsets_s) -> np.ndarray:
    """Resolve the oracle audio from whichever route the flags select.

    Three routes, checked in the order a more specific answer beats a more
    general one: a WAV the caller already has, an AudioUnit instrument played
    here, and fluidsynth as the one that always works.
    """
    wav = getattr(args, "oracle_wav", "")
    if not wav:
        from au_oracle import render_oracle_au, source_from_args

        source = source_from_args(args)
        if source is not None:
            return render_oracle_au(
                smf_bytes, total_seconds, sr,
                source=source,
                cache_dir=None if getattr(args, "au_no_cache", False) else DEFAULT_AU_CACHE,
                verbose=True,
            )
        return render_oracle_fluidsynth(
            smf_bytes, total_seconds, sr,
            soundfont=Path(args.sf2) if getattr(args, "sf2", "") else None,
        )
    audio, shift = load_oracle_wav(
        Path(wav), total_seconds, sr,
        onsets_s=onsets_s,
        align=not getattr(args, "oracle_no_align", False),
        offset_s=getattr(args, "oracle_offset", None),
        resample=getattr(args, "oracle_resample", False),
    )
    print(f"oracle: {wav} (lead-in removed: {shift * 1000.0:+.0f} ms)", file=sys.stderr)
    return audio


def _onset_strength(mono: np.ndarray, sr: int, hop: int) -> np.ndarray:
    """Half-wave-rectified log-RMS difference: one value per hop."""
    win = hop * 2
    n = max(0, (len(mono) - win) // hop + 1)
    if n < 2:
        return np.zeros(1)
    frames = np.lib.stride_tricks.sliding_window_view(mono, win)[::hop][:n]
    rms_db = 10.0 * np.log10(np.mean(frames.astype(np.float64) ** 2, axis=1) + 1e-12)
    return np.maximum(0.0, np.diff(rms_db, prepend=rms_db[0]))


def estimate_alignment(
    audio: np.ndarray, sr: int, onsets_s, max_shift_s: float = 3.0,
) -> float:
    """Seconds the WAV must be shifted EARLIER to line up with the probe.

    Correlates the render's onset-strength function against an impulse train at
    the probe's note onsets. The onset function rather than the raw envelope
    because a decaying instrument (piano, plucked string) has almost no energy
    where the score still holds a note, so a gate-vs-envelope correlation would
    be pulled toward the note starts anyway — only less sharply.
    """
    if not len(onsets_s):
        return 0.0
    hop = max(1, int(0.002 * sr))
    strength = _onset_strength(audio.mean(axis=1), sr, hop)
    target = np.zeros(len(strength))
    for t in onsets_s:
        idx = int(round(t * sr / hop))
        if 0 <= idx < len(target):
            target[idx] = 1.0
    if target.sum() == 0.0 or strength.sum() == 0.0:
        return 0.0
    max_lag = int(round(max_shift_s * sr / hop))
    lags = np.arange(-max_lag, max_lag + 1)
    best_lag, best_score = 0, -np.inf
    for lag in lags:
        # score = onset strength found where the score says a note begins
        shifted = np.roll(target, lag)
        if lag > 0:
            shifted[:lag] = 0.0
        elif lag < 0:
            shifted[lag:] = 0.0
        score = float(np.dot(strength, shifted))
        if score > best_score:
            best_score, best_lag = score, int(lag)
    return best_lag * hop / sr


def load_oracle_wav(
    path: Path,
    total_seconds: float,
    sr: int = 48000,
    *,
    onsets_s=(),
    align: bool = True,
    offset_s: float | None = None,
    resample: bool = False,
) -> tuple[np.ndarray, float]:
    """Load an externally rendered probe as an oracle, aligned to the score.

    Returns the (frames, channels) float32 render trimmed to `total_seconds`,
    plus the offset in seconds that was removed (positive: the WAV started
    late). `offset_s` pins the offset instead of estimating it; `align=False`
    keeps the file as-is.
    """
    audio, got_sr = read_wav(path)
    if got_sr != sr:
        if not resample:
            raise RuntimeError(
                f"{path}: rendered at {got_sr} Hz, the harness runs at {sr} Hz. "
                f"Re-render at {sr} Hz (preferred), or pass --oracle-resample."
            )
        print(f"note: resampling oracle {got_sr} -> {sr} Hz", file=sys.stderr)
        audio = resample_linear(audio, got_sr, sr)
    if audio.ndim == 1:
        audio = audio[:, None]

    shift = 0.0
    if offset_s is not None:
        shift = float(offset_s)
    elif align:
        shift = estimate_alignment(audio, sr, list(onsets_s))
    if shift:
        n = int(round(shift * sr))
        if n > 0:
            audio = audio[n:] if n < audio.shape[0] else audio[:0]
        else:
            audio = np.concatenate(
                [np.zeros((-n, audio.shape[1]), dtype=np.float32), audio], axis=0
            )
    return fit_length(audio, total_seconds, sr), shift
