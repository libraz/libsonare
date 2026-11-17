"""Domain rule for the public-interface inventory.

Every public record belongs to exactly one of six domains. Resolution order:
an override by name, the CLI command table, the declaring file's basename, a
short list of key patterns when the basename names no domain, the symbol key for
files whose declarations span domains, and finally ``core``.
Members (methods, fields, options, values, flags) inherit their owner's domain;
the inventory applies that, this module classifies owners only.
"""

from __future__ import annotations

import fnmatch
import re
from pathlib import PurePosixPath

DOMAINS = ("core", "analysis", "effects", "mastering", "engine", "project")

# Ordered first-match patterns over a lowercased file basename or snake_case key.
AREAS: tuple[tuple[str, str], ...] = (
    (
        "project",
        (
            r"project|vocal_edit|vocal_project|vocalproject|arrangement|take_align|align_take"
            r"|clip_page|opfs|timeline|annotate|external_stem|part_rig"
        ),
    ),
    (
        "engine",
        (
            r"engine|playback|sample_bank|synth|web_midi|midi|worklet|live_audio|instrument|hrtf"
            r"|renderer|transport"
        ),
    ),
    (
        "mastering",
        (
            r"master|meter|mixer|mixing|assist|loudness|lufs|limiter|streaming_processors"
            r"|streaming_mixing|stream_eq|dynamics"
        ),
    ),
    (
        "effects",
        (
            r"effects|repair|voice_chang|voicechang|acoustic|dereverb|denois|declick|declip|hpss"
            r"|separation|time_stretch|pitch_shift|note_ops|percussive|trim"
        ),
    ),
    (
        "analysis",
        (
            r"analy|feature|stream|transcribe|polyphony|chord|audio|pitch|spectr|rhythm|beat|tempo"
            r"|bpm|key|onset|mel|mfcc|chroma|stft|cqt|resample|scale|decompose|inverse|units|framing"
        ),
    ),
)

# Overrides by name, matched on the compact form (lowercase, no underscores, no
# leading `sonare_` of a C function). Glob patterns allowed.
OVERRIDES: dict[str, str] = {
    "audio": "core",
    "capabilities": "core",
    "sonarecapabilities": "core",
    "hasffmpegsupport": "core",
    "vocaleditavailable": "core",
    "abiversion": "core",
    "*abiversion": "core",
    "matrix2d": "core",
    "sonareerror": "core",
    "sonarevalueerror": "core",
    "errorcode": "core",
    "codename": "core",
    "issonareerror": "core",
    "lasterrorcode": "core",
    "lasterrormessage": "core",
    "errorcodename": "core",
    "version": "core",
    "mixer": "engine",
    "streamanalyzer": "analysis",
}

# Key patterns tried when the declaring file's basename names no domain.
KEY_PATTERNS: tuple[tuple[str, str], ...] = (
    (r"^engine_mixer_", "engine"),
    (r"worklet_scope", "engine"),
)

# Files whose declarations span domains: classify by symbol key instead.
AGGREGATE_FILES: frozenset[str] = frozenset(
    {
        "bindings/python/src/libsonare/__init__.pyi",
        "bindings/python/src/libsonare/__init__.py",
        "bindings/python/src/libsonare/types.pyi",
        "bindings/python/src/libsonare/analyzer.pyi",
        "bindings/python/src/libsonare/analyzer.py",
        "bindings/python/src/libsonare/_conversions.py",
        "bindings/python/src/libsonare/_types_enums.py",
        "bindings/python/src/libsonare/_ffi_types_core.py",
        "bindings/wasm/src/index.ts",
        "bindings/wasm/src/analysis.ts",
        "bindings/wasm/src/public_types.ts",
        "bindings/wasm/src/public_types_music.ts",
        "bindings/wasm/src/public_types_realtime.ts",
        "bindings/wasm/src/worklet/messages.ts",
        "bindings/wasm/src/validation.ts",
        "bindings/node/src/index.ts",
        "bindings/node/src/types.ts",
        "include/sonare/sonare_c.h",
        "include/sonare/sonare_c_types_functions.h",
        "include/sonare/sonare_c_types.h",
        "include/sonare/sonare_c_types_enums.h",
    }
)

# Every command of both CLI front-ends. A command missing here is a checker failure.
CLI_COMMANDS: dict[str, str] = {
    "acoustic": "effects",
    "amplitude-to-db": "analysis",
    "analyze": "analysis",
    "beats": "analysis",
    "boundaries": "analysis",
    "bpm": "analysis",
    "chirp": "analysis",
    "chords": "analysis",
    "chroma": "analysis",
    "clicks": "analysis",
    "clipping": "mastering",
    "cqt": "analysis",
    "db-to-amplitude": "analysis",
    "db-to-power": "analysis",
    "declip": "effects",
    "decompose-stems": "analysis",
    "deemphasis": "analysis",
    "doctor": "core",
    "downbeats": "analysis",
    "dynamic-range": "mastering",
    "dynamics": "analysis",
    "eq": "mastering",
    "estimate-room": "effects",
    "fade": "effects",
    "filter": "effects",
    "fix-frames": "analysis",
    "fix-length": "analysis",
    "fourier-tempogram": "analysis",
    "frame-signal": "analysis",
    "frames-to-samples": "analysis",
    "gain": "effects",
    "hpss": "effects",
    "info": "core",
    "key": "analysis",
    "lufs": "mastering",
    "master": "mastering",
    "mastering": "mastering",
    "mastering-chain": "mastering",
    "mastering-pair-analyses": "mastering",
    "mastering-pair-analyze": "mastering",
    "mastering-pair-processor": "mastering",
    "mastering-pair-processors": "mastering",
    "mastering-presets": "mastering",
    "mastering-processor": "mastering",
    "mastering-processors": "mastering",
    "mastering-profile": "mastering",
    "mastering-stereo-analyses": "mastering",
    "mastering-stereo-analyze": "mastering",
    "mastering-streaming": "mastering",
    "mastering-suggest": "mastering",
    "mel": "analysis",
    "mel-to-audio": "analysis",
    "melody": "analysis",
    "meter": "mastering",
    "mfcc-to-audio": "analysis",
    "midi-render": "engine",
    "mix": "engine",
    "mix-strip": "engine",
    "mixing-preset": "engine",
    "mixing-presets": "engine",
    "nnls-chroma": "analysis",
    "normalize": "effects",
    "note-move": "effects",
    "note-stretch": "effects",
    "onset-env": "analysis",
    "onset-envelope": "analysis",
    "onsets": "analysis",
    "pad-center": "analysis",
    "pcen": "analysis",
    "peak-pick": "analysis",
    "phase": "mastering",
    "pitch": "analysis",
    "pitch-correct": "effects",
    "pitch-correct-timevarying": "effects",
    "pitch-shift": "effects",
    "playback": "engine",
    "plp": "analysis",
    "polyphonic-notes": "analysis",
    "polyphonic-render": "effects",
    "power-to-db": "analysis",
    "preemphasis": "analysis",
    "project": "project",
    "project.abi": "project",
    "project.align-takes": "project",
    "project.bounce": "project",
    "project.compile": "project",
    "project.export-midi2": "project",
    "project.export-smf": "project",
    "project.import-midi2": "project",
    "project.import-smf": "project",
    "project.new": "project",
    "project.synth-presets": "project",
    "project.validate": "project",
    "repair": "effects",
    "resample": "analysis",
    "rhythm": "analysis",
    "room-morph": "effects",
    "samples-to-frames": "analysis",
    "scale-quantize": "analysis",
    "sections": "analysis",
    "spectral": "analysis",
    "split-silence": "effects",
    "stereo": "mastering",
    "suggest-mix": "mastering",
    "synthesize-rir": "effects",
    "system-info": "core",
    "tempogram": "analysis",
    "tempogram-ratio": "analysis",
    "timbre": "analysis",
    "time-stretch": "effects",
    "tone": "analysis",
    "tonnetz": "analysis",
    "transcribe": "analysis",
    "trim-silence": "effects",
    "tune-to-midi": "effects",
    "vector-normalize": "analysis",
    "version": "core",
    "vocal-notes": "analysis",
    "voice-change": "effects",
    "voice-preset": "effects",
    "voice-preset-validate": "effects",
    "voice-presets": "effects",
    "vqt": "analysis",
}

PATHS = (
    "override",
    "cli-table",
    "declaring-file",
    "key-pattern",
    "aggregate-key",
    "default",
    "inherited",
)

_CAMEL = re.compile(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])")


class UnknownCommandError(KeyError):
    """Raised for a CLI command the command table does not list."""


def snake(name: str) -> str:
    """CamelCase / kebab / dotted name to snake_case."""
    return _CAMEL.sub("_", name).replace("-", "_").replace(".", "_").lower()


def compact(name: str) -> str:
    """Override key: lowercase, separators removed, C function prefix dropped."""
    name = name.removeprefix("sonare_")
    return re.sub(r"[^a-z0-9*]", "", name.lower())


def symbol_key(name: str) -> str:
    """The snake_case key the AREAS patterns run against for aggregate files."""
    if name.startswith("sonare_"):
        name = name[len("sonare_") :]
    elif name.startswith("Sonare") and len(name) > 6 and name[6].isupper():
        name = name[6:]
    return snake(name)


def area_of(text: str) -> str | None:
    low = text.lower()
    for domain, pattern in AREAS:
        if re.search(pattern, low):
            return domain
    return None


def override_for(name: str, overrides: dict[str, str] | None = None) -> str | None:
    table = OVERRIDES if overrides is None else overrides
    key = compact(name)
    if key in table:
        return table[key]
    for pattern, domain in sorted(table.items()):
        if "*" in pattern and fnmatch.fnmatchcase(key, pattern):
            return domain
    return None


def classify(
    surface: str,
    name: str,
    file: str,
    *,
    overrides: dict[str, str] | None = None,
    aggregates: frozenset[str] | None = None,
    commands: dict[str, str] | None = None,
) -> tuple[str, str]:
    """Return (domain, path) for a top-level record.

    @throws UnknownCommandError for a CLI command the table does not list.
    """
    domain = override_for(name, overrides)
    if domain:
        return domain, "override"
    if surface.startswith("cli"):
        table = CLI_COMMANDS if commands is None else commands
        if name not in table:
            raise UnknownCommandError(name)
        return table[name], "cli-table"
    agg = AGGREGATE_FILES if aggregates is None else aggregates
    if file and file not in agg:
        domain = area_of(PurePosixPath(file).name.replace("-", "_"))
        if domain:
            return domain, "declaring-file"
        key = symbol_key(name)
        for pattern, domain in KEY_PATTERNS:
            if re.search(pattern, key):
                return domain, "key-pattern"
    if file in agg or not file:
        domain = area_of(symbol_key(name))
        if domain:
            return domain, "aggregate-key"
    return "core", "default"
