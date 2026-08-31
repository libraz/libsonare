"""The captured single-note corpus as a probe, and as the oracle for one.

`capture.py corpus` plays a reference instrument one note at a time and writes a
WAV per (note, velocity) with a manifest beside them. `profile.py` then measures
those into a reference profile and scores a model against it as a table. That
path has no search in it, and the path that does have a search — `autofit.py` —
was rendering its own oracle from a different stimulus: three notes, one
velocity, held two seconds, against a corpus captured over fifteen notes, four
velocities, held eight. A value fitted on the first cannot be read off the
second, and neither table can tell anyone that.

This module is the bridge. It lays the corpus grid out as one probe timeline —
each note in its own slot, spaced by exactly the length of its own capture — and
assembles the captured WAVs onto that same timeline as the oracle render. Every
downstream piece then works unchanged: the model renders the identical score,
`probe_rows` measures both sides note by note, and the multi-scale term compares
two signals that are sample-aligned by construction.

Two properties are worth stating because the rest of the harness depends on
them. Slot spacing is the capture's own length rather than a chosen gap, so a
note's analysis window is precisely the audio that was captured for it and never
reaches into its neighbour. And the assembled oracle is silence wherever the
corpus has no recording, so a grid filtered down to a few notes stays on the
same timeline as the full one — a fit and its hold-out are laid out by the same
rule.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from capture import (
    RIG_BAKED,
    RIG_NONE,
    RIG_UNCLASSIFIED,
    RIG_VALUES,
    parse_seconds,
    rig_capable,
)
from patterns import Pattern
from smf import Note
from wavio import read_wav

#: One-based MIDI channel 10, the GM drum channel. `write_smf` counts from zero.
PERCUSSION_CHANNEL = 10


@dataclass(frozen=True)
class Corpus:
    """One timbre of a captured corpus: its manifest, and where its WAVs are."""

    root: Path
    timbre: str
    sample_rate: int
    gate_s: float
    preroll_s: float
    #: The longest slot in the grid, and the window for any slot the manifest
    #: recorded no length for. Read `slot_for` instead wherever a particular
    #: note is in hand — a grid with a per-note tail has no single slot length.
    slot_s: float
    #: (note, velocity) -> path, for this timbre only.
    renders: dict[tuple[int, int], Path]
    notes: tuple[int, ...]
    velocities: tuple[int, ...]
    label: str = ""
    #: What the capture config asserted about the reference's effects. A dry
    #: capture is the instrument; a wet one has a room baked in that the model
    #: has to be placed in before any decay metric means anything.
    dry: bool = False
    #: What the capture config answered about a rig — an amplifier, a cabinet, a
    #: rotary speaker — standing between the instrument and the microphone.
    #: `unclassified` where nobody has answered, which is not `none`. A different
    #: question from `dry`, which is about a room and cannot see a cabinet.
    #: `check_rig` is what reads it.
    rig: str = RIG_UNCLASSIFIED
    #: One-based MIDI channel saying what this timbre's note numbers MEAN. 10 is
    #: the drum channel, where a note number selects an instrument rather than a
    #: pitch — which decides both the channel the model's probe is written on and
    #: which metric set can measure it. Not the rack slot the timbre was
    #: addressed at, which is `capture.slot_channel` and says nothing about the
    #: instrument. `_note_channel` is what keeps the two apart here.
    channel: int = 1
    #: The capture's own families of notes — the tom series, the hi-hat trio —
    #: as sets of note numbers. Empty for a capture that named none, which is
    #: every pitched one. See `capture.note_groups` and `loss._kit_terms`.
    groups: dict[str, tuple[int, ...]] = field(default_factory=dict)
    #: (note, velocity) -> the audio captured for that slot, less the preroll.
    #: A grid captured at one flat tail has the same value in every entry; one
    #: that recorded eight seconds for its cymbals and two for the rest does
    #: not, and a single number for both truncates the cymbals to the kick's
    #: window — which is the whole reason the longer tail was captured.
    slots: dict[tuple[int, int], float] = field(default_factory=dict)

    def slot_count(self) -> int:
        return len(self.notes) * len(self.velocities)

    def slot_for(self, note: int, velocity: int) -> float:
        """The analysis window for one slot: exactly what was recorded for it."""
        return self.slots.get((note, velocity), self.slot_s)

    def percussive(self) -> bool:
        """Whether this corpus's note numbers select instruments, not pitches."""
        return self.channel == PERCUSSION_CHANNEL


def load_corpus(manifest_path: Path | str, timbre: str = "") -> Corpus:
    """Read a capture manifest and resolve one timbre's renders.

    `timbre` defaults to the first the manifest lists, which is the capture
    config's own order — for the piano corpus that is the comparison target the
    reference profile is anchored on.
    """
    import json

    path = Path(manifest_path).expanduser().resolve()
    if path.is_dir():
        path = path / "manifest.json"
    if not path.exists():
        raise FileNotFoundError(
            f"no capture manifest at {path} — run `capture.py corpus` first, or point "
            f"--corpus at the directory one was written to"
        )
    manifest = json.loads(path.read_text())
    root = path.parent

    available = [t["id"] if isinstance(t, dict) else str(t) for t in manifest.get("timbres", [])]
    chosen = timbre or (available[0] if available else "")
    if not chosen:
        raise ValueError(f"{path} lists no timbres")
    if chosen not in available:
        raise ValueError(
            f"timbre {chosen!r} is not in {path.name} (it has {', '.join(available)})"
        )

    renders: dict[tuple[int, int], Path] = {}
    for rec in manifest.get("renders", []):
        if rec.get("timbre") != chosen:
            continue
        renders[(int(rec["note"]), int(rec["velocity"]))] = root / rec["path"]
    if not renders:
        raise ValueError(f"{path.name} has no renders for timbre {chosen!r}")

    preroll_s = float(manifest.get("preroll_ms", 0)) / 1000.0
    gate_s = float(manifest["gate_ms"]) / 1000.0
    # The slot is the capture's own render length, so a note's analysis window is
    # exactly what was recorded for it — per slot, because `tail_by_note` lets one
    # grid hold several. The flat gate + tail is the fallback for a manifest
    # written before per-render lengths were recorded.
    slots = {
        (int(rec["note"]), int(rec["velocity"])): float(rec["seconds"]) - preroll_s
        for rec in manifest.get("renders", [])
        if rec.get("timbre") == chosen and "seconds" in rec
    }
    slot_s = (max(slots.values()) if slots
              else gate_s + _tail_seconds(manifest.get("tail", "2s")))

    entry = next(
        (t for t in manifest.get("timbres", [])
         if isinstance(t, dict) and t.get("id") == chosen),
        {},
    )
    label = entry.get("label", chosen)
    return Corpus(
        root=root,
        timbre=chosen,
        sample_rate=int(manifest.get("sample_rate", 48000)),
        gate_s=gate_s,
        preroll_s=preroll_s,
        slot_s=slot_s,
        renders=renders,
        notes=tuple(sorted({n for n, _ in renders})),
        velocities=tuple(sorted({v for _, v in renders})),
        label=label,
        dry=_dryness(manifest),
        rig=_rig(manifest),
        channel=_note_channel(manifest, entry),
        groups=_groups(manifest),
        slots=slots,
    )


def _note_channel(manifest: dict, entry: dict) -> int:
    """What one timbre's note numbers mean, as a one-based MIDI channel.

    A manifest's timbre block is copied from the capture definition, so a corpus
    captured before the rack slot had a name of its own carries the slot under
    `channel` — and believing it makes a banjo in slot 10 a drum map.

    `slot_channel` in the block is what says the two were separated when it was
    written, so `channel` beside it is the meaning. Without it the copy cannot
    answer and the definition the manifest names does, which is the same two-step
    as `_dryness`; from its tracked half alone, since a clone has no overlay. A
    definition that cannot be read leaves only the block, which is every manifest
    written by hand or by a test.
    """
    import json

    if "slot_channel" in entry:
        return int(entry.get("channel", 1))
    for candidate in _config_paths(manifest.get("config", "")):
        try:
            tracked = json.loads(candidate.read_text())
        except (OSError, ValueError):
            continue
        for timbre in tracked.get("timbres") or []:
            if timbre.get("id") == entry.get("id"):
                return int(timbre.get("channel", 1))
    return int(entry.get("channel", 1))


def _config_paths(config: str):
    """The capture definition a manifest names, found from any directory.

    A manifest records the path repo-relative, so a bare `Path.exists()` answers
    differently depending on where the harness was invoked from. `_dryness` is
    the one caller not resolved this way, and its miss is safe — an unread config
    reads wet, and a dry reference measured wet still comes back dry. A miss in
    `_note_channel` restores the ambiguity the lookup exists to resolve, and one
    in `_rig` is indistinguishable from a capture nobody has answered for.
    """
    if not config:
        return
    path = Path(config)
    if path.exists():
        yield path
    if not path.is_absolute():
        from _repo import REPO_ROOT

        rooted = Path(REPO_ROOT) / path
        if rooted.exists():
            yield rooted


def _dryness(manifest: dict) -> bool:
    """Whether the capture asserted that every effect section was switched off.

    The manifest records what was played rather than what the config asked for,
    so the assertion lives in the config file it names. A capture whose config
    has moved away answers False, which is the safe direction: the room is then
    measured from the audio, and a measurement of a genuinely dry reference
    comes back dry and skips the correction. The opposite default would silently
    fit a model to a reference recorded in a hall.
    """
    import json

    if "dry" in manifest:
        return bool(manifest["dry"])
    config = manifest.get("config", "")
    if not config:
        return False
    path = Path(config)
    if not path.exists():
        return False
    try:
        return bool(json.loads(path.read_text()).get("dry", False))
    except (OSError, ValueError):
        return False


def _rig(manifest: dict) -> str:
    """What the capture answered about a rig, from the manifest or from its config.

    Same two-step as `_dryness`, and for the same reason. Absent, unreadable and
    unrecognised all come back `unclassified`, which is the only safe direction:
    that is the answer a fit refuses to run on where a rig is possible, and
    `none` is the one that would let a rigged reference through.
    """
    import json

    if "rig" in manifest:
        value = manifest.get("rig")
        return str(value) if value in RIG_VALUES else RIG_UNCLASSIFIED
    # Through `_config_paths`, because every corpus rendered before the field
    # existed can only be answered from its config, and a manifest records that
    # path repo-relative. Resolving it against the working directory instead
    # makes the answer reachable from the repo root and nowhere else, and the
    # miss looks exactly like a capture nobody has answered for.
    for candidate in _config_paths(manifest.get("config", "")):
        try:
            value = json.loads(candidate.read_text()).get("rig")
        except (OSError, ValueError):
            continue
        if value in RIG_VALUES:
            return str(value)
    return RIG_UNCLASSIFIED


def check_rig(corpus: Corpus, program: int, *, allow: bool = False) -> None:
    """Refuse a fit whose reference carries an amplifier, or might.

    A rig is nonlinear, so unlike a room there is no inverse to correct with: a
    fit run against a rigged reference reproduces the amplifier with the
    instrument's own parameters, closes cleanly on every metric, and is lost the
    moment the rig is put back where it belongs.

    `none` is a reference captured at the instrument's own boundary and is what a
    fit is for. `baked` is an acceptance target and never a fit target. The
    absence of an answer stops a fit only where the family could carry a rig at
    all — a piano or a wind is not waiting on anyone — because there the missing
    record is a question nobody has answered rather than a "no".

    Comparing, auditioning and `--diagnose` are unaffected; they read the
    reference rather than moving the voice towards it.
    """
    if corpus.rig == RIG_NONE:
        return
    what = corpus.label or corpus.timbre
    if corpus.rig == RIG_BAKED:
        why = (
            f"the {what} reference carries a rig: the amplifier and the cabinet are in "
            f"the recording, and a rig has no inverse, so nothing can take them back out "
            f"the way a room is measured and convolved. Fitting program {program} against "
            f"it would reproduce an amplifier with the instrument's own parameters. Fit "
            f"against a reference captured at the instrument's boundary — a DI for an "
            f"electric string — and keep this one as the acceptance check"
        )
    elif rig_capable(program):
        why = (
            f"nothing says whether the {what} reference carries a rig, and program "
            f"{program} is a family that can: a cabinet is a filter rather than a space, "
            f"so an amplified take passes every dryness test with the whole rig inside it. "
            f"Answer it in the capture definition — \"rig\": \"none\" for a reference "
            f"captured at the instrument's boundary, \"baked\" for one recorded through an "
            f"amplifier, which stays an acceptance target — and re-run"
        )
    else:
        return
    if allow:
        print(f"--allow-rigged-oracle: {why}. Proceeding; the values this produces "
              f"transfer to nothing once the rig is a stage of its own.", file=sys.stderr)
        return
    raise ValueError(f"{why}. --allow-rigged-oracle overrides.")


def _groups(manifest: dict) -> dict[str, tuple[int, ...]]:
    """The capture's families of notes, from the manifest or from its config.

    Same two-step as `_dryness`, and for the same reason: the block was added
    after corpora had already been captured, and re-rendering a 282-note grid to
    pick up one header field is not a reasonable thing to ask of anyone. A
    manifest that carries it answers from itself; one that does not reads the
    config it names.

    Absent everywhere means no kit relations are scored, which is exactly right
    for a capture whose families were never identified — the alternative would
    be guessing them from note numbers, and a guessed family scores a relation
    between instruments that are not related.
    """
    import json

    if "groups" in manifest:
        raw = manifest.get("groups") or {}
    else:
        config = manifest.get("config", "")
        path = Path(config) if config else None
        if path is None or not path.exists():
            return {}
        try:
            raw = json.loads(path.read_text()).get("groups") or {}
        except (OSError, ValueError):
            return {}
    return {str(name): tuple(int(n) for n in notes) for name, notes in raw.items()}


def _tail_seconds(tail) -> float:
    """Read the manifest's tail field, which is written as '500ms', '2s' or a number."""
    return parse_seconds(tail)


def corpus_pattern(
    corpus: Corpus,
    *,
    notes: tuple[int, ...] | None = None,
    velocities: tuple[int, ...] | None = None,
) -> Pattern:
    """The corpus grid as one probe timeline, ordered note-major then velocity.

    Every slot is analysed: a captured single note in eight seconds of its own
    silence is the cleanest analysis window this harness has, which is the whole
    reason the corpus exists.

    A grid this size is not free — sixty slots of ten seconds is ten minutes of
    audio per render — so `notes` and `velocities` cut it, and the caller is
    expected to report the length it chose.
    """
    picked_notes = tuple(notes) if notes else corpus.notes
    picked_vels = tuple(velocities) if velocities else corpus.velocities
    missing = [
        (n, v) for n in picked_notes for v in picked_vels if (n, v) not in corpus.renders
    ]
    if missing:
        shown = ", ".join(f"n{n}/v{v}" for n, v in missing[:6])
        raise ValueError(
            f"the {corpus.timbre} corpus has no capture for {len(missing)} of the requested "
            f"slots ({shown}{', ...' if len(missing) > 6 else ''}); it covers notes "
            f"{','.join(str(n) for n in corpus.notes)} at velocities "
            f"{','.join(str(v) for v in corpus.velocities)}"
        )

    seq = []
    t = 0.0
    last_slot = corpus.slot_s
    for n in picked_notes:
        for v in picked_vels:
            seq.append(Note(n, v, t, corpus.gate_s))
            last_slot = corpus.slot_for(n, v)
            t += last_slot
    # The trailing pad is the final slot's own, not the grid's longest: it is
    # there so the last note is not cut off, and every earlier note is spaced by
    # the slot it was captured in.
    tail = max(0.0, last_slot - corpus.gate_s)
    # A kit corpus is captured on the drum channel, and the model's probe has to
    # be written on the same one or its note numbers sound pitches of program 0
    # while the oracle plays the kit. `percussive` then carries into which metric
    # set can measure the pair.
    return Pattern("corpus", seq, analysis_notes=list(seq), tail=tail,
                   channel=PERCUSSION_CHANNEL - 1 if corpus.percussive() else 0,
                   percussive=corpus.percussive())


def corpus_oracle(corpus: Corpus, pattern: Pattern, sr: int) -> np.ndarray:
    """Assemble the captured WAVs onto the probe's timeline as one render.

    The capture's preroll is dropped rather than kept: it exists so the plugin's
    first buffer is not the note's attack, and leaving it in would place every
    captured onset a preroll late against the model's, which the per-note
    windows would then read as an attack that arrives slow on every note of the
    grid.

    Returned as (frames, channels) to match what an oracle route hands back;
    `to_mono` downmixes it exactly as it does an AudioUnit render.
    """
    if sr != corpus.sample_rate:
        raise ValueError(
            f"the {corpus.timbre} corpus was captured at {corpus.sample_rate} Hz and the "
            f"probe renders at {sr} Hz; re-capture at the render rate rather than "
            f"resampling a reference"
        )
    last = max(pattern.notes, key=lambda n: n.start)
    total = int(round((last.start + corpus.slot_for(last.note, last.velocity)) * sr))
    out: np.ndarray | None = None
    skip = int(round(corpus.preroll_s * sr))
    for note in pattern.notes:
        path = corpus.renders.get((note.note, note.velocity))
        if path is None:
            continue
        audio, wav_sr = read_wav(path)
        if wav_sr != sr:
            raise ValueError(f"{path.name} is {wav_sr} Hz, not the probe's {sr} Hz")
        if audio.ndim == 1:
            audio = audio[:, None]
        if out is None:
            out = np.zeros((total, audio.shape[1]), dtype=np.float64)
        seg = audio[skip:]
        start = int(round(note.start * sr))
        slot = corpus.slot_for(note.note, note.velocity)
        room = min(len(seg), total - start, int(round(slot * sr)))
        if room > 0:
            out[start : start + room] += seg[:room]
    if out is None:
        raise ValueError(f"no captured audio found for the {corpus.timbre} probe")
    return out


def describe(corpus: Corpus, pattern: Pattern) -> str:
    """One line naming what a run is about to score against, and what it costs."""
    last = max(pattern.notes, key=lambda n: n.start)
    seconds = last.start + corpus.slot_for(last.note, last.velocity)
    return (
        f"corpus oracle: {corpus.label} — {len(pattern.notes)} slots "
        f"({len({n.note for n in pattern.notes})} notes x "
        f"{len({n.velocity for n in pattern.notes})} velocities), "
        f"{corpus.gate_s:.0f} s gate, {seconds / 60.0:.1f} min of audio per render"
    )
