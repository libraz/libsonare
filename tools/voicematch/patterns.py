"""Note patterns the voice-match harness renders through both synths.

Each pattern is a plain note list plus the subset of notes that are clean
enough for per-note spectral analysis (isolated, long enough to have a stable
sustain window). Both renderers consume the identical list, so any audible or
measured difference comes from the synth, not the score.
"""

from __future__ import annotations

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
    "scale": scale_pattern,
}


def build_pattern(name: str, program: int, **kwargs) -> Pattern:
    """Build a named pattern for `program`; kwargs pass through to the builder."""
    try:
        builder = PATTERN_BUILDERS[name]
    except KeyError:
        raise ValueError(f"unknown pattern '{name}' (choose from {sorted(PATTERN_BUILDERS)})") from None
    return builder(program, **kwargs)


def pattern_length(pattern: Pattern) -> float:
    """Total render length in seconds (last note-off plus the tail)."""
    end = max((n.start + n.dur) for n in pattern.notes)
    return end + pattern.tail
