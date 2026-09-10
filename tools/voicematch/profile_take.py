"""The whole-take path: where a phrase is measured, and what its room asks of libsonare."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

from metrics import _db, _spectrum, to_mono
from phrases import build_takes
from room import Room, match_sends, measurable_room
from wavio import read_wav

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/ for _repo

from _repo import REPO_ROOT  # noqa: E402


# --------------------------------------------------------------------------
# phrase takes: the half of an instrument that only exists between notes

#: Skipped after the last note-off before the tail window opens, so the release
#: transient of the final note is not measured as part of what follows it.
TAKE_TAIL_LEAD_S = 0.08
SUSTAIN_CC = 64
#: Two decades either side of the middle. Wide bands on purpose: this is a
#: balance reading over a whole phrase, and a narrow one would report which
#: pitches the phrase happens to contain.
TAKE_BANDS = ((40.0, 160.0), (160.0, 640.0), (640.0, 2560.0), (2560.0, 10240.0))

#: Where a broadband layer added to fill a decay tail is heard instead as a
#: veil over the note. Deliberately not the tail's own band: the two are
#: different jobs and a single layer serving both is the shape a missing
#: mechanism takes here -- during the note the high end should be the string's
#: partials, after it the board's own field.
SUSTAIN_TONALITY_BAND = (2000.0, 8000.0)

#: Below this, nothing in this harness radiates and whatever a source shows is
#: its own recording chain. Removed before any tail number is taken, because the
#: tail is where it stops being negligible: a sampled instrument's floor is
#: recorded INTO the samples and plays back with them, so it is a fixed level
#: under a decaying signal and its share grows as the note dies. Measured on the
#: piano take references it is worth 1.4 to 2.5 dB at the top of the tail and up
#: to 10 by the end of it, which is most of the slope being fitted through it.
#: The band readings below already start at 40 Hz and were never affected; the
#: broadband ones had no such bound.
TAKE_SUBSONIC_HZ = 40.0
#: Amplitude under which a source is not quiet but absent. These renders finish
#: before their files do -- every piano take reference reaches exact digital
#: silence about a second before the take's nominal end -- and a tail slope
#: fitted across that edge is a measurement of the edge.
TAKE_SILENCE = 1.0e-7

TAKE_LABELS = {
    "sustain_tonality": "2-8 kHz during the note, + = partials over a veil (dB)",
    "s2560": "2560-10240 Hz during the note (dB of the note)",
    "tail": "what is left after the last note (dB under the take's peak)",
    "tail_slope": "how fast that falls (dB/s)",
    "tail_tonality": "tail tonality, + = resolvable modes rather than a smear (dB)",
    "damped": "left 200 ms after the sustain pedal releases (dB of the tail)",
    "floor_rise": "floor before the last onset vs the first (dB)",
    "b40": "tail 40-160 Hz (dB of the tail)",
    "b160": "tail 160-640 Hz",
    "b640": "tail 640-2560 Hz",
    "b2560": "tail 2560-10240 Hz",
}


def take_windows(take) -> dict:
    """Where to measure this phrase, read off the phrase's own schedule.

    Never a fixed time. A take is an arbitrary phrase and the interesting
    windows are defined by its events: the tail is whatever is still sounding
    after the last note-off, and on an instrument with a sustain pedal it ends
    when the pedal lifts rather than when the render does -- measuring past that
    point averages the ring together with the dampers landing on it, which are
    opposite mechanisms at the two ends of one window.
    """
    last_off = max(n.start + n.dur for n in take.notes)
    release = next((t for t, cc, v in sorted(take.cc_events)
                    if cc == SUSTAIN_CC and v < 64 and t > last_off), None)
    tail0 = last_off + TAKE_TAIL_LEAD_S
    tail1 = release if release is not None else last_off + take.tail_s
    windows = {"tail": (tail0, tail1)} if tail1 - tail0 > 0.15 else {}
    if release is not None:
        windows["damped"] = (release + 0.20, release + 0.40)
    # Where the phrase leaves a gap, the level just before the next onset is
    # what the notes before it left behind. On a coupled instrument that floor
    # RISES through a phrase; on a model whose voices do not interact it falls,
    # and no per-note measurement can tell the two apart.
    gaps = []
    onsets = sorted({n.start for n in take.notes})
    for onset in onsets[1:]:
        prev_off = max((n.start + n.dur for n in take.notes if n.start + n.dur <= onset),
                       default=None)
        if prev_off is not None and onset - prev_off > 0.15:
            gaps.append((onset - 0.06, onset - 0.005))
    windows["_gaps"] = gaps
    # The held part of the take's longest note. A layer added to fill the decay
    # tail is audible over the NOTE as well, and measuring only the tail cannot
    # tell an improvement from a veil; this is the other side of that trade, on
    # the same phrase and the same render. Staccato takes have no such window
    # and are simply not measured on it.
    longest = max(take.notes, key=lambda n: n.dur)
    if longest.dur > 0.5:
        windows["sustain"] = (longest.start + 0.15, longest.start + longest.dur)
    return windows


def signal_end_s(x: np.ndarray, sr: int, threshold: float = TAKE_SILENCE) -> float:
    """When this source stops having anything in it at all, in seconds.

    Not a quietness test. A tail sixty decibels under the body is an ordinary
    decay and cutting there ends the window on whichever voice decays fastest;
    what has to be excluded is a render that has RUN OUT -- a sampled instrument
    reaching the end of its sample and being faded to nothing, and then a file
    that carries on in exact silence.
    """
    live = np.flatnonzero(np.abs(np.asarray(x, dtype=np.float64)) > threshold)
    return float(live[-1] + 1) / sr if live.size else 0.0


def usable_tail(sources: list[np.ndarray], sr: int,
                window: tuple[float, float]) -> tuple[float, float] | None:
    """@p window clipped to where every source still has signal.

    Taken across the REFERENCES and applied to the model too, so both sides are
    read over one span. Per-source would be worse than useless here: a synthetic
    render runs to the end of its buffer, so it would keep a window the
    instrument it is being compared against left two seconds earlier.
    """
    ends = [signal_end_s(x, sr) for x in sources]
    hi = min([window[1]] + ends)
    return (window[0], hi) if hi - window[0] > 0.2 else None


def highpass(x: np.ndarray, sr: int, hz: float) -> np.ndarray:
    """Everything above @p hz, through a filter rather than through the spectrum.

    Emphatically NOT a brick wall on the rfft, which is what this was first. A
    rectangular window in frequency is a sinc in time, so zeroing a band spreads
    its truncation error across the whole buffer -- and on a take whose loudest
    moment is seventy decibels over its tail, that error IS the tail. Measured:
    the model's post-damper tail decays cleanly from -72 to -130 dB, and the
    same tail behind a brick wall sits flat at -81 and then RISES toward the end
    as the circular wrap brings the phrase's opening back round. It reads as a
    voice with a non-decaying tail, which is a finding, and it is the filter.

    A windowed sinc instead, convolved LINEARLY (the transform is zero-padded to
    the full convolution length, so nothing wraps). The window is Blackman-
    Harris, whose sidelobes are 92 dB down -- the dynamic range this has to
    survive is the seventy between a take's peak and its tail, and a Blackman's
    58 would not.
    """
    if hz <= 0.0:
        return x
    x = np.asarray(x, dtype=np.float64)
    taps = 8193
    n = np.arange(taps) - (taps - 1) // 2
    fc = hz / sr
    lp = np.sinc(2.0 * fc * n) * 2.0 * fc
    a = (0.35875, 0.48829, 0.14128, 0.01168)
    k = 2.0 * np.pi * np.arange(taps) / (taps - 1)
    lp *= a[0] - a[1] * np.cos(k) + a[2] * np.cos(2 * k) - a[3] * np.cos(3 * k)
    lp /= lp.sum()
    h = -lp
    h[(taps - 1) // 2] += 1.0
    size = len(x) + taps - 1
    y = np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(h, size), size)
    return y[(taps - 1) // 2:(taps - 1) // 2 + len(x)]


def measure_take(audio: np.ndarray, sr: int, windows: dict) -> dict:
    """One phrase reduced to the numbers a per-note grid cannot carry.

    Everything under `TAKE_SUBSONIC_HZ` is removed first, on both sides, since
    no instrument here radiates there and a recording that does would otherwise
    be compared against a render's silence.
    """
    x = highpass(to_mono(np.asarray(audio, dtype=np.float64)), sr, TAKE_SUBSONIC_HZ)
    peak = float(np.abs(x).max())
    if peak <= 0.0:
        return {}

    def seg(window):
        a, b = int(window[0] * sr), int(window[1] * sr)
        return x[max(0, a):min(len(x), b)]

    def rms(window):
        s = seg(window)
        return float(np.sqrt(np.mean(s ** 2))) if s.size else 0.0

    out: dict = {}
    # What the note-and-gap windows carry is measured first, because it does not
    # depend on there being a tail. A harpsichord's every reference stops within
    # a tenth of a second of its last damper, so the tail block below drops out
    # for that whole instrument -- and taking the rest of the take with it would
    # lose the two readings about the NOTE that are the reason those takes exist.
    if "sustain" in windows:
        held = seg(windows["sustain"])
        if held.size > sr // 8:
            sf, smag = _spectrum(held, sr)
            sp = np.asarray(smag, dtype=np.float64) ** 2
            lo, hi = SUSTAIN_TONALITY_BAND
            sq = np.maximum(sp[(sf >= lo) & (sf < hi)], 1e-30)
            if sq.size:
                out["sustain_tonality"] = round(
                    float(10 * np.log10(sq.mean() / np.exp(np.log(sq).mean()))), 2)
            stotal = max(float(sp.sum()), 1e-30)
            share = float(sp[(sf >= 2560.0) & (sf < 10240.0)].sum()) / stotal
            out["s2560"] = round(float(10 * np.log10(max(share, 1e-12))), 2)
    gaps = windows.get("_gaps") or []
    if len(gaps) >= 2:
        first, last = _db(rms(gaps[0]) / peak), _db(rms(gaps[-1]) / peak)
        out["floor_rise"] = round(float(last - first), 2)

    tail = windows.get("tail")
    if tail is None or rms(tail) <= 0.0:
        return out
    out["tail"] = round(float(_db(rms(tail) / peak)), 2)
    mid = 0.5 * (tail[0] + tail[1])
    span = 0.5 * (tail[1] - tail[0])
    if span > 0.05:
        out["tail_slope"] = round(
            float((_db(rms((mid, tail[1]))) - _db(rms((tail[0], mid)))) / span), 2)
    if "damped" in windows:
        out["damped"] = round(float(_db(rms(windows["damped"]) / max(rms(tail), 1e-12))), 2)
    body = seg(tail)
    freqs, mag = _spectrum(body, sr)
    p = np.asarray(mag, dtype=np.float64) ** 2
    band = (freqs >= 100.0) & (freqs < 5000.0)
    q = np.maximum(p[band], 1e-30)
    if q.size:
        out["tail_tonality"] = round(float(10 * np.log10(q.mean() / np.exp(np.log(q).mean()))), 2)
    total = max(float(p.sum()), 1e-30)
    for lo, hi in TAKE_BANDS:
        share = float(p[(freqs >= lo) & (freqs < hi)].sum()) / total
        out[f"b{int(lo)}"] = round(float(10 * np.log10(max(share, 1e-12))), 2)
    return out


def archived_take_references(archive: Path, capture_id: str, take_id: str, sr: int) -> dict:
    """The reference renders of one phrase, back at the level they were made at.

    These come from the audition archive rather than from the note corpus,
    because a phrase is not in the note corpus: the corpus is one note at a
    time, which is exactly the condition under which everything measured here
    is inactive.
    """
    index = archive / "index.json"
    if not index.exists():
        return {}
    meta = json.loads(index.read_text()).get(capture_id, {}).get(take_id)
    if not meta:
        return {}
    gain = 10.0 ** (float(meta["gain_db"]) / 20.0)
    out = {}
    for path in sorted((archive / capture_id / take_id).glob("*.wav")):
        audio, file_sr = read_wav(path)
        if file_sr == sr:
            out[path.stem] = np.asarray(audio, dtype=np.float64) / gain
    return out


def archived_take_ids(archive: Path, capture_id: str) -> set[str]:
    """Which phrases the archive holds a reference for, by take id."""
    index = archive / "index.json"
    if not index.exists():
        return set()
    return set(json.loads(index.read_text()).get(capture_id, {}))


def room_match(cfg: dict, *, archive: Path, take_id: str, program: int, verbose: bool) -> int:
    """What libsonare's OWN ambience controls can do about the reference's room.

    The complementary question to everything else here. `compare` and `takes`
    put the model in the reference's space so the timbre is read without the
    building; this asks what the shipped library would have to be told to be in
    that building by itself, which it answers in the only terms it takes — a
    CC91 send and the GS tank's decay, not an RT60. The send is what a program's
    `gm_fallback_sends` weight scales, so the answer converts straight into that
    table.

    Driven off a phrase rather than off a synthesised probe, unlike
    `voicematch.py room-match`: the reference for a phrase is already in the
    archive, so this needs neither the plugin nor the machine it runs on, and
    the room is measured on the same audio the listening page plays.
    """
    from voicematch import DECAY_SCALE_KEY

    sr = int(cfg.get("sample_rate", 48000))
    if cfg.get("dry", True):
        print("the capture is dry: there is no room to reproduce", file=sys.stderr)
        return 0
    wanted = [t for t in build_takes(cfg["takes"], program) if t.id == take_id]
    if not wanted:
        print(f"no take {take_id!r} in the {cfg['takes']!r} set", file=sys.stderr)
        return 2
    take = wanted[0]
    refs = archived_take_references(archive, cfg["id"], take.id, sr)
    if not refs:
        print(f"{take.id}: no archived reference", file=sys.stderr)
        return 2
    spans = [(n.start, n.start + n.dur) for n in take.notes]

    # Every reference, not the first: two recordings of one instrument disagree
    # about their building by more than this search can resolve, and aiming at
    # whichever came first picks a tank the other contradicts.
    targets: list[Room] = []
    for name, audio in refs.items():
        room = measurable_room(np.asarray(audio, dtype=np.float64), sr, spans)
        if room is None:
            print(f"{take.id}: {name} measures no room this phrase can support",
                  file=sys.stderr)
            continue
        targets.append(room)
        print(f"target ({name} on {take.id}): RT60 {room.rt60_s:.2f}s  "
              f"tail level {room.tail_db:+.1f}dB  HF ratio {room.hf_ratio:.2f}")
    if not targets:
        return 2
    if len(targets) > 1:
        print(f"  span: RT60 {min(t.rt60_s for t in targets):.2f}-"
              f"{max(t.rt60_s for t in targets):.2f}s  tail level "
              f"{min(t.tail_db for t in targets):+.1f} to "
              f"{max(t.tail_db for t in targets):+.1f}dB  "
              f"(anything inside it scores zero)")

    # One render per grid point, each in its own interpreter: the override table
    # is read once when the library loads, so a sweep inside one process would
    # measure the first tank setting at every point.
    child = (
        "import sys; sys.path.insert(0, %r)\n"
        "import json\n"
        "from phrases import build_takes\n"
        "from smf import write_smf\n"
        "from room import estimate_room\n"
        "from render_model import render_model\n"
        "prog, cc91, tid, tset = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]\n"
        "t = [x for x in build_takes(tset, prog) if x.id == tid][0]\n"
        "smf = write_smf(t.notes, program=prog, end_pad=t.tail_s, "
        "cc_events=t.cc_events, channel=t.channel, sends=(cc91, 0, 0))\n"
        "a = render_model(smf, t.duration(), 48000)\n"
        "r = estimate_room(a, 48000, [(n.start, n.start + n.dur) for n in t.notes])\n"
        "print(f'{r.rt60_s} {r.tail_db} {r.hf_ratio}')\n"
    ) % (str(Path(__file__).resolve().parent),)

    def measure(cc91: int, decay_scale: float):
        env = dict(os.environ)
        env["SONARE_TUNING_OVERRIDES"] = f"{DECAY_SCALE_KEY}={decay_scale}"
        proc = subprocess.run(
            [sys.executable, "-c", child, str(program), str(cc91), take.id, cfg["takes"]],
            env=env, capture_output=True, text=True, cwd=str(REPO_ROOT))
        if proc.returncode:
            raise SystemExit(proc.stderr[-1200:])
        rt, tail, hf = (float(v) for v in proc.stdout.strip().split()[-3:])
        return Room(rt60_s=rt, hf_ratio=hf, tail_db=tail, predelay_ms=15.0)

    print("searching libsonare's ambience controls (one render per point)...")
    result = match_sends(targets, measure, log=print if verbose else None)
    print(f"\nclosest: CC91 {result['cc91']}, reverb_decay {result['reverb_decay']} "
          f"({DECAY_SCALE_KEY}={result['decay_scale']})")
    print(f"  reached RT60 {result['measured']['rt60_s']:.2f}s  "
          f"tail {result['measured']['tail_db']:+.1f}dB   residual {result['residual']}")
    print(f"  -> MULTIPLY program {program}'s gm_fallback_sends reverb weight "
          f"by {result['send_factor']} (the shipped weight is already in the "
          f"measurement; this is not the weight to write)")
    if result["residual"] > 1.5:
        print("  the tank cannot reach this space: the reference's room is outside "
              "the range libsonare's own reverb spans, so no send weight fixes it")
    return 0
