"""Note patterns the voice-match harness renders through both synths.

Each pattern is a plain note list plus the subset of notes that are clean
enough for per-note spectral analysis (isolated, long enough to have a stable
sustain window). Both renderers consume the identical list, so any audible or
measured difference comes from the synth, not the score.
"""

from __future__ import annotations

import inspect
from dataclasses import dataclass, field

from smf import Note

# Register presets per GM program (MIDI note numbers for low/mid/high probes).
# Chosen to sit inside each instrument's real playing range so the reference
# SoundFont plays natural samples rather than extreme transpositions. Programs
# not listed fall back to their 8-program family entry, then to C3/C4/C5.
_DEFAULT_REGISTERS = (48, 60, 72)

_PROGRAM_REGISTERS: dict[int, tuple[int, int, int]] = {
    # 32-39 basses
    **{p: (28, 40, 52) for p in range(32, 40)},
    # 40-47 strings & orchestral
    40: (55, 67, 79),   # violin
    41: (48, 60, 72),   # viola
    42: (36, 48, 60),   # cello
    43: (28, 40, 52),   # contrabass
    44: (48, 60, 72),   # tremolo strings
    45: (48, 60, 72),   # pizzicato strings
    46: (48, 60, 72),   # harp
    47: (41, 48, 55),   # timpani
    # 56-63 brass
    56: (54, 66, 76),   # trumpet
    57: (40, 52, 64),   # trombone
    58: (28, 40, 52),   # tuba
    59: (54, 66, 76),   # muted trumpet
    60: (41, 53, 65),   # french horn
    61: (48, 60, 72),   # brass section
    62: (48, 60, 72),   # synth brass 1
    63: (48, 60, 72),   # synth brass 2
    # 64-71 reeds
    64: (56, 68, 80),   # soprano sax
    65: (49, 61, 73),   # alto sax
    66: (44, 56, 68),   # tenor sax
    67: (36, 48, 60),   # baritone sax
    68: (58, 70, 82),   # oboe
    69: (52, 64, 76),   # english horn
    70: (34, 46, 58),   # bassoon
    71: (50, 62, 74),   # clarinet
    # 72-79 pipes
    72: (74, 82, 90),   # piccolo
    73: (60, 72, 84),   # flute
    74: (60, 72, 84),   # recorder
    75: (60, 72, 84),   # pan flute
    76: (60, 72, 84),   # blown bottle
    77: (60, 72, 84),   # shakuhachi
    78: (72, 79, 86),   # whistle
    79: (60, 72, 84),   # ocarina
}


def registers_for_program(program: int) -> tuple[int, int, int]:
    """Low/mid/high probe notes for a GM program."""
    if program in _PROGRAM_REGISTERS:
        return _PROGRAM_REGISTERS[program]
    family_base = (program // 8) * 8
    return _PROGRAM_REGISTERS.get(family_base, _DEFAULT_REGISTERS)


@dataclass(frozen=True)
class Pattern:
    """A named note sequence plus which notes are analyzable in isolation."""

    name: str
    notes: list[Note]
    analysis_notes: list[Note] = field(default_factory=list)
    tail: float = 1.5  # seconds of silence rendered after the last note-off
    # MIDI channel the probe is written on. 9 (channel 10 in one-based
    # numbering) is the GM drum channel, on which a note number selects a
    # percussion instrument rather than a pitch — libsonare routes it through
    # `drum_note_table()` and a GM reference synth through its drum bank.
    channel: int = 0
    # Score with the percussion metric set (band levels, per-band decay, attack
    # sharpness) instead of the harmonic one. A drum hit has no fundamental to
    # anchor a harmonic ladder on, so every pitch-derived metric would be
    # measuring a frequency the sound does not contain.
    percussive: bool = False


def sustain_pattern(
    program: int,
    *,
    notes: tuple[int, ...] | None = None,
    velocity: int = 100,
    dur: float = 2.0,
    gap: float = 1.0,
) -> Pattern:
    """Isolated long notes across the instrument's range — the default analysis pattern."""
    pitches = notes if notes is not None else registers_for_program(program)
    seq = []
    t = 0.1
    for p in pitches:
        seq.append(Note(p, velocity, t, dur))
        t += dur + gap
    return Pattern("sustain", seq, analysis_notes=list(seq))


def velocity_pattern(
    program: int,
    *,
    note: int | None = None,
    velocities: tuple[int, ...] = (40, 70, 100, 127),
    dur: float = 1.5,
    gap: float = 1.0,
) -> Pattern:
    """One pitch at increasing velocities — probes the dynamics curve."""
    pitch = note if note is not None else registers_for_program(program)[1]
    seq = []
    t = 0.1
    for v in velocities:
        seq.append(Note(pitch, v, t, dur))
        t += dur + gap
    return Pattern("velocity", seq, analysis_notes=list(seq))


def staccato_pattern(
    program: int,
    *,
    note: int | None = None,
    velocity: int = 100,
    count: int = 4,
    dur: float = 0.12,
    gap: float = 0.6,
) -> Pattern:
    """Short repeated notes — probes attack transients and release behaviour.

    Notes are too short for a sustain window, so they are excluded from
    spectral analysis; envelope metrics still apply.
    """
    pitch = note if note is not None else registers_for_program(program)[1]
    seq = []
    t = 0.1
    for _ in range(count):
        seq.append(Note(pitch, velocity, t, dur))
        t += dur + gap
    return Pattern("staccato", seq, analysis_notes=[])


def room_probe_pattern(
    program: int,
    *,
    notes: tuple[int, ...] | None = None,
    velocity: int = 110,
    dur: float = 0.25,
    gap: float = 4.0,
) -> Pattern:
    """Short notes with long silences — the pattern to measure a room from.

    Reverberation is measured from what is left after the player stops, so the
    measurement is only as good as the silence it has to work with. The
    `sustain` pattern leaves one second, which is shorter than the decay of any
    space worth correcting for: the tail is cut off before it has fallen far
    enough to fit a slope to, and what little there is arrives mixed with the
    instrument's own ring from a two-second note.

    Short notes and four-second gaps invert both. The excitation stops long
    before the room does, so the tail is the room; and the notes are too short
    to be worth analysing for timbre, which is why nothing here is an analysis
    note.
    """
    pitches = notes if notes is not None else registers_for_program(program)
    seq = []
    t = 0.1
    for p in pitches:
        seq.append(Note(p, velocity, t, dur))
        t += dur + gap
    return Pattern("room-probe", seq, analysis_notes=[], tail=4.0)


# Which drum note a drum probe defaults to, and the velocities it strikes at.
# Velocity is the axis a drum voice varies along — a melodic voice is probed
# across its register, but a drum note has only one — so the probe sweeps it and
# the held-out set is a second sweep the fit never sees.
DEFAULT_DRUM_NOTE = 38  # Acoustic Snare
DRUM_VELOCITIES = (64, 100, 127)
DRUM_HOLDOUT_VELOCITIES = (48, 88, 112)


def _drum_pattern(
    name: str,
    *,
    notes: tuple[int, ...] | None,
    velocities: tuple[int, ...],
    dur: float,
    gap: float,
    tail: float,
) -> Pattern:
    pitches = notes if notes is not None else (DEFAULT_DRUM_NOTE,)
    seq = []
    t = 0.1
    for p in pitches:
        for v in velocities:
            seq.append(Note(p, v, t, dur))
            t += gap
    return Pattern(name, seq, analysis_notes=list(seq), tail=tail, channel=9, percussive=True)


def drum_pattern(
    program: int,
    *,
    notes: tuple[int, ...] | None = None,
    velocities: tuple[int, ...] = DRUM_VELOCITIES,
    dur: float = 0.05,
    gap: float = 2.0,
) -> Pattern:
    """Isolated one-shot hits of one drum note at increasing velocities.

    Written on the drum channel, so `notes` selects percussion instruments
    rather than pitches: 38 is the acoustic snare, 36 the kick, 46 the open
    hi-hat. `program` selects the drum kit (0 is the standard kit) rather than a
    melodic instrument.

    A drum is a one-shot: the note-off carries no information, so the note is as
    short as the score can make it and the two seconds between hits are what the
    analysis actually reads. That gap is what the longest instrument in the kit
    needs — an open cymbal or a gong rings for most of it — and it also keeps
    each hit's window clear of the next one's onset.
    """
    return _drum_pattern(
        "drum", notes=notes, velocities=velocities, dur=dur, gap=gap, tail=2.0
    )


def drum_holdout_pattern(
    program: int,
    *,
    notes: tuple[int, ...] | None = None,
    velocities: tuple[int, ...] = DRUM_HOLDOUT_VELOCITIES,
    dur: float = 0.05,
    gap: float = 2.0,
) -> Pattern:
    """`drum` at velocities the fit never saw — the generalisation check.

    A drum fit has no register to hold notes out of, so the held-out axis is
    velocity: values fitted at 64/100/127 that are wrong at 48/88/112 are
    overfitted to the probe exactly as a violin fitted on three pitches can be.
    """
    return _drum_pattern(
        "drum-holdout", notes=notes, velocities=velocities, dur=dur, gap=gap, tail=2.0
    )


def scale_pattern(
    program: int,
    *,
    root: int | None = None,
    velocity: int = 96,
    step: float = 0.35,
) -> Pattern:
    """A legato major-scale run — a musical listening check, not analyzed per-note."""
    base = root if root is not None else registers_for_program(program)[1]
    degrees = (0, 2, 4, 5, 7, 9, 11, 12)
    seq = [Note(base + d, velocity, 0.1 + i * step, step * 1.05) for i, d in enumerate(degrees)]
    return Pattern("scale", seq, analysis_notes=[])


PATTERN_BUILDERS = {
    "sustain": sustain_pattern,
    "velocity": velocity_pattern,
    "staccato": staccato_pattern,
    "room-probe": room_probe_pattern,
    "drum": drum_pattern,
    "drum-holdout": drum_holdout_pattern,
    "scale": scale_pattern,
}


# A plural CLI override and the singular parameter it feeds when a builder
# takes only one. `--notes 62` on the velocity pattern names the pitch it sweeps
# velocity at; `--velocities 90` on a sustain pattern names the one it holds.
_SINGULAR_FOR = {"notes": ("note", "root"), "velocities": ("velocity",)}

# Every parameter name either half of that mapping can appear as, for the error
# message a pattern with neither axis has to produce.
_AXIS_PARAMS = frozenset(_SINGULAR_FOR) | {p for ps in _SINGULAR_FOR.values() for p in ps}


def _adapt_kwargs(name: str, builder, kwargs: dict) -> dict:
    """Fit the CLI's plural overrides to one builder's parameters.

    `--pattern` offers all seven builders and `--notes` / `--velocities` are
    written for the ones that sweep that axis, so half the combinations reach a
    builder with no such parameter. A builder that varies the other axis still
    has a singular version of this one — `velocity_pattern(note=...)`,
    `scale_pattern(root=...)` — so a single value is passed there, and anything
    else is refused by name rather than reaching the builder as a `TypeError`
    naming a parameter the flag does not mention.
    """
    params = inspect.signature(builder).parameters
    out: dict = {}
    for key, value in kwargs.items():
        if key in params:
            out[key] = value
            continue
        singular = next((p for p in _SINGULAR_FOR.get(key, ()) if p in params), None)
        if singular is None:
            axes = [p for p in params if p in _AXIS_PARAMS]
            raise ValueError(
                f"pattern '{name}' has no {key} axis; it takes "
                f"{', '.join(axes) if axes else 'neither notes nor velocities'}"
            )
        if len(value) != 1:
            raise ValueError(
                f"pattern '{name}' sounds one {singular}, so {key} takes a single value "
                f"(got {len(value)}: {','.join(str(v) for v in value)})"
            )
        out[singular] = value[0]
    return out


def build_pattern(name: str, program: int, **kwargs) -> Pattern:
    """Build a named pattern for `program`; kwargs pass through to the builder."""
    try:
        builder = PATTERN_BUILDERS[name]
    except KeyError:
        raise ValueError(f"unknown pattern '{name}' (choose from {sorted(PATTERN_BUILDERS)})") from None
    return builder(program, **_adapt_kwargs(name, builder, kwargs))


def pattern_length(pattern: Pattern) -> float:
    """Total render length in seconds (last note-off plus the tail)."""
    end = max((n.start + n.dur) for n in pattern.notes)
    return end + pattern.tail


def analysis_window_end(pattern: Pattern, note: Note) -> float:
    """Where one note's analysis window ends, in seconds.

    Whichever comes first: the end of the note's own release tail, or the next
    note's onset. The percussion path has always clamped a hit's window where
    the next one begins, and a sustained note needs the same clamp for the same
    reason — the default probe holds each note for 2.0 s and adds a 1.5 s tail
    against a 3.0 s onset spacing, so without it every note but the last
    measured half a second of the next note's attack as its own release, which
    is the headline number of the compare report and part of `--w-env`.

    `analyze_note` and `analyze_hit` both cap the window they are handed, so a
    gap longer than the sound costs nothing.
    """
    end = min(note.start + note.dur + pattern.tail, pattern_length(pattern))
    return min([end] + [n.start for n in pattern.notes if n.start > note.start])
