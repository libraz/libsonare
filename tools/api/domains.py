"""Domain rule for the public-interface inventory.

Every public record belongs to exactly one of six domains. Resolution order:
an override by name, the CLI command table, a few name patterns that outrank
the file, the declaring file's basename, a short list of key patterns when the
basename names no domain, the symbol key (AREAS) for files whose declarations
span domains (then that file's basename), and finally ``core``. A vote over those per-surface domains gives each name one
domain (most surfaces, ties C > wasm > node > python, an override pins it); the
result is frozen in domains.json, which the checker applies by name. A
``<function>_result`` / ``_request`` / ``_options`` type then takes the domain of
its function, and a releaser ``free_<struct>`` the domain of the struct it releases.
Members (methods, fields, options, values, flags) inherit their owner's domain,
except the direct members of a MEMBER_KEYED override class, which classify by
their own name; the inventory applies inheritance.
"""

from __future__ import annotations

import fnmatch
import json
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
    "init": "core",
    "isinitialized": "core",
    "masteringrepair*": "effects",
    # The geometry a room estimate returns; no function carries its name.
    "roomgeometryestimate": "effects",
    # The generic AudioWorklet base every processor extends.
    "workletprocessor": "engine",
    "sonareworkletprocessor": "engine",
    "sonaremasteringrepair*": "effects",
    "version": "core",
    "mixer": "engine",
    "streamanalyzer": "analysis",
    "scalename": "analysis",
    "hasfeature": "core",
    # The audio handle's own construction, accessors and release are core on every
    # surface; the C functions that analyse a handle keep their operation's domain.
    "audiofrom*": "core",
    "audiofilechannelcount": "core",
    "audiodestroy": "core",
    "audiodata": "core",
    "audiolength": "core",
    "audiosamplerate": "core",
    "audiodurationsec": "core",
}

# Name patterns that outrank the declaring file: names those files misfile.
NAME_PATTERNS: tuple[tuple[str, str], ...] = (
    (r"^synth(_|$)", "engine"),
    (r"^(meter_estimate|estimate_meter|meter_options)(_|$)", "analysis"),
    (r"(^|_)rir(_|$)", "effects"),
    (
        r"^(analyze|analysis)_dynamics(_|$)|^(free_)?dynamics(_analysis)?_result$|^dynamics$",
        "analysis",
    ),
    (r"^scale_|streaming_retune", "effects"),
)

# Key patterns tried before AREAS whenever the declaring file's basename names no domain.
KEY_PATTERNS: tuple[tuple[str, str], ...] = (
    (r"^engine_mixer_", "engine"),
    (r"worklet_scope", "engine"),
    (r"^mastering_", "mastering"),
    (r"pitch_correct", "effects"),
    (r"^note_(move|stretch)(_|$)", "effects"),
    (r"^(harmonic|percussive)(_|$)", "effects"),
    (r"^normalize(_|$)", "mastering"),
    (r"^(detect_clipping|dynamic_range|peak_db|true_peak_db|rms_db)(_|$)", "mastering"),
    (r"^(crest_factor_db|dc_offset|silence_ratio)(_|$)", "mastering"),
    (r"^(zero_crossing_rate|rms_energy)(_|$)", "analysis"),
)

# Override classes whose members are operations of other domains: each member is
# classified by its own name (key patterns, then AREAS), falling back to core.
MEMBER_KEYED: frozenset[str] = frozenset({"audio"})

# Files whose declarations span domains: classify by symbol key instead.
AGGREGATE_FILES: frozenset[str] = frozenset(
    {
        "bindings/python/src/libsonare/__init__.pyi",
        "bindings/python/src/libsonare/_types_analysis.py",
        "bindings/python/src/libsonare/__init__.py",
        "bindings/python/src/libsonare/analyzer.pyi",
        "bindings/python/src/libsonare/analyzer.py",
        "bindings/python/src/libsonare/_conversions.py",
        "bindings/python/src/libsonare/_types_enums.py",
        "bindings/python/src/libsonare/_ffi_types_core.py",
        "bindings/wasm/src/index.ts",
        "bindings/wasm/src/public_types_spectral.ts",
        "bindings/wasm/src/analysis.ts",
        "bindings/wasm/src/public_types.ts",
        "bindings/wasm/src/public_types_music.ts",
        "bindings/wasm/src/public_types_realtime.ts",
        "bindings/wasm/src/worklet/messages.ts",
        "bindings/wasm/src/validation.ts",
        "bindings/node/src/index.ts",
        "bindings/node/src/types_analysis.ts",
        "bindings/node/src/types_notes.ts",
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
    "name-pattern",
    "declaring-file",
    "key-pattern",
    "aggregate-key",
    "default",
    "member-key",
    "member-default",
    "unified",
    "ledger",
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
    if name.startswith(("sonare_", "SONARE_")):
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


def _first_match(patterns: tuple[tuple[str, str], ...], key: str) -> str | None:
    for pattern, domain in patterns:
        if re.search(pattern, key):
            return domain
    return None


def name_pattern(key: str) -> str | None:
    return _first_match(NAME_PATTERNS, key)


def key_pattern(key: str) -> str | None:
    return _first_match(KEY_PATTERNS, key)


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
    key = symbol_key(name)
    domain = name_pattern(key)
    if domain:
        return domain, "name-pattern"
    agg = AGGREGATE_FILES if aggregates is None else aggregates
    if file and file not in agg:
        domain = area_of(PurePosixPath(file).name.replace("-", "_"))
        if domain:
            return domain, "declaring-file"
    domain = key_pattern(key)
    if domain:
        return domain, "key-pattern"
    if file in agg or not file:
        domain = area_of(key)
        if domain:
            return domain, "aggregate-key"
        # An aggregate whose basename names a domain keeps it for keys no pattern claims.
        domain = area_of(PurePosixPath(file).name.replace("-", "_")) if file else None
        if domain:
            return domain, "declaring-file"
    return "core", "default"


def member_keyed(owner: str) -> bool:
    """True when members of this top-level owner classify by their own name."""
    return compact(owner) in MEMBER_KEYED


def classify_member(member: str) -> tuple[str, str]:
    """Return (domain, path) for a member of a MEMBER_KEYED owner."""
    key = symbol_key(member)
    domain = name_pattern(key) or key_pattern(key) or area_of(key)
    return (domain, "member-key") if domain else ("core", "member-default")


# Top-level kinds the name vote covers, and its tie-break order (highest first).
UNIFY_KINDS = ("function", "class", "type", "export")
UNIFY_PRIORITY = ("c", "wasm", "node", "python")


def top_level(name: str) -> str:
    """The top-level name a ledger spelling refers to: Type.field, fn(param), Type='token'."""
    return re.split(r"[.(=]", name, maxsplit=1)[0]


def _voters(records: list[dict]) -> dict[str, dict[str, list[dict]]]:
    groups: dict[str, dict[str, list[dict]]] = {}
    for r in records:
        if r["owner"] or r["kind"] not in UNIFY_KINDS or r["surface"] not in UNIFY_PRIORITY:
            continue
        groups.setdefault(symbol_key(r["name"]), {}).setdefault(r["surface"], []).append(r)
    return groups


def vote(records: list[dict]) -> dict[str, dict]:
    """Resolve each top-level name (as a symbol key) to one domain from per-surface domains.

    An override on any surface pins the name; otherwise the domain held by the most surfaces
    wins, ties broken by UNIFY_PRIORITY. A surface holding the name in two domains votes with
    its first record (by file, line, kind). Returns {key: {domain, surfaces, tie}}.
    """
    rank = {s: i for i, s in enumerate(UNIFY_PRIORITY)}
    out = {}
    for key, per in _voters(records).items():
        firsts = {
            s: min(per[s], key=lambda r: (r["file"], r["line"], r["kind"]))
            for s in UNIFY_PRIORITY
            if s in per
        }
        pinned = [
            r for s in UNIFY_PRIORITY for r in per.get(s, []) if r.get("domain_path") == "override"
        ]
        tie = False
        if pinned:
            winner = pinned[0]["domain"]
        else:
            tally: dict[str, list[int]] = {}
            for s, r in firsts.items():
                t = tally.setdefault(r["domain"], [0, len(rank)])
                t[0] += 1
                t[1] = min(t[1], rank[s])
            winner = min(tally, key=lambda d: (-tally[d][0], tally[d][1]))
            tie = sum(1 for t in tally.values() if t[0] == tally[winner][0]) > 1
        out[key] = {
            "domain": winner,
            "surfaces": {s: r["domain"] for s, r in firsts.items()},
            "tie": tie,
            "pinned": bool(pinned),
        }
    # A result, request or options type belongs where its function does.
    for key, v in out.items():
        function = re.sub(r"_(result|request|options)$", "", key)
        if function != key and function in out and not v["pinned"]:
            v["domain"] = out[function]["domain"]
    # A releaser belongs where the struct it releases does.
    for key, v in out.items():
        released = out.get(key[len("free_") :]) if key.startswith("free_") else None
        if released is not None and not v["pinned"]:
            v["domain"] = released["domain"]
    for v in out.values():
        del v["pinned"]
    return out


def regenerate(records: list[dict]) -> str:
    """The generated domains.json text: one line per name, its domain and the surfaces seen."""
    lines = [
        json.dumps(k)
        + ": "
        + json.dumps({"domain": v["domain"], "surfaces": sorted(v["surfaces"])})
        for k, v in sorted(vote(records).items())
    ]
    return "{\n" + ",\n".join(lines) + "\n}\n"


def assign(records: list[dict], frozen: dict[str, dict], renames: dict[str, str]) -> list[str]:
    """Give top-level records the frozen domain of their name, else the ledger's; return the rest.

    ``renames`` maps a new name's key to the domain of its old entry. A name in neither keeps
    its per-surface classification; the sorted keys of those names are returned.
    """
    unassigned = set()
    for key, per in _voters(records).items():
        if key in frozen:
            domain, path = frozen[key]["domain"], "unified"
        elif key in renames:
            domain, path = renames[key], "ledger"
        else:
            unassigned.add(key)
            continue
        for recs in per.values():
            for r in recs:
                if r["domain"] != domain:
                    r["domain"], r["domain_path"] = domain, path
    return sorted(unassigned)


def inherit(records: list[dict]) -> None:
    """Set member domains from their owner, or by name for a MEMBER_KEYED owner's direct members."""
    owners = {(r["surface"], r["name"]): r["domain"] for r in records if not r["owner"]}
    for r in records:
        if (
            r["owner"]
            and "." not in r["owner"]
            and "(" not in r["name"]
            and member_keyed(r["owner"])
        ):
            r["domain"], r["domain_path"] = classify_member(r["member"])
            owners[(r["surface"], r["name"])] = r["domain"]
    for r in records:
        if not r["owner"] or r.get("domain_path", "").startswith("member-"):
            continue
        owner = r["owner"]
        d = None
        while d is None:
            d = owners.get((r["surface"], owner))
            if d is None and "." in owner:
                owner = owner.rsplit(".", 1)[0]
            elif d is None:
                owner = owner.split("(")[0]
                d = owners.get((r["surface"], owner), "core")
        r["domain"], r["domain_path"] = d, "inherited"
