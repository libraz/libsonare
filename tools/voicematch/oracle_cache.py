"""A numeric cache of a corpus oracle, so a fit can run where the capture is not.

`autofit.oracle_reference` reduces a captured corpus to per-note rows, a room,
a band edge and a channel correlation. This module stores exactly that, plus the
scrubbed `Corpus` descriptor the fit path reads, and nothing else: no audio, no
spectrogram, no STFT magnitude, no path and nothing a manifest says about its
source. Both `write` and `read` pass the content through one whitelist in which
every key at every level is named; an unknown key stops the run with its name,
so the whitelist changes only by a deliberate edit here.

A cache is refused unless its identity (what the run asked of the oracle), its
code digest (the code that measured it) and, where the corpus is present, its
corpus digest (the WAVs and manifest it was measured from) all match.
"""

from __future__ import annotations

import ast
import gzip
import hashlib
import json
import os
import re
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import objective

HERE = Path(__file__).resolve().parent

SCHEMA_VERSION = 1

#: Longest flat numeric list a cache may hold; a partial ladder is tens, a spectrum is thousands.
MAX_ARRAY = 128

#: Longest table of fixed-width rows (oracle rows, slot lengths); each row is itself bounded.
MAX_TABLE = 4096

ROLES = ("fit", "holdout")

#: The modules the oracle's rows, room and band edge are computed through.
CODE_ROOTS = (
    "loss",
    "corpus",
    "room",
    "autofit_resolve",
    "loop_record",
    "metrics_signal",
    "oracle_cache",
)


class CacheError(ValueError):
    """A cache that may not stand in for the oracle, or content that may not leave."""


# --------------------------------------------------------------------------- #
# The whitelist
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class Obj:
    """A mapping whose keys are exactly the named ones (all required, or any subset)."""

    fields: dict
    required: bool = True


@dataclass(frozen=True)
class Table:
    """A list of fixed-width rows or of named objects, bounded by `MAX_TABLE`."""

    row: object


@dataclass(frozen=True)
class Enum:
    values: tuple


@dataclass(frozen=True)
class EnumList:
    values: tuple


@dataclass(frozen=True)
class Maybe:
    """Either None or the wrapped spec."""

    spec: object


#: Every key of an oracle row: the note and hit measurement records, plus what
#: `probe_rows` and `level_of` add. A test compares this against the records.
ROW_KEYS = [
    "arrival_ms",
    "attack_ms",
    "attack_fine_ms",
    "attack_floored",
    "attack_hf_db",
    "attack_lf_db",
    "attack_peaks",
    "band_decay_db_s",
    "bands_db",
    "beat_db",
    "beat_rate_hz",
    "body_below_f0_db",
    "centroid_hz",
    "crest_db",
    "decay_capped",
    "decay_ms",
    "evolution_db",
    "evolution_measured",
    "evolution_valid",
    "f0_cents_err",
    "f0_hz",
    "f0_width_cents",
    "flatness_db",
    "fm_fast_cents",
    "fm_fast_rate_hz",
    "fm_slow_cents",
    "fm_slow_rate_hz",
    "harmonics_db",
    "held_crest_db",
    "held_rms_dbfs",
    "inharmonicity_b",
    "inharmonicity_partials",
    "ladder_partials",
    "level_db",
    "loop_unweighted",
    "modal_db",
    "modal_density",
    "modal_density_valid",
    "modal_hz",
    "modal_ratio",
    "note",
    "odd_even_db",
    "onset_ms",
    "onset_overshoot_db",
    "onset_settle_ms",
    "peak_band_hz",
    "peak_dbfs",
    "pitch_drop_ms",
    "pitch_drop_ratio",
    "prompt_late_db",
    "prompt_late_valid",
    "release_capped",
    "release_ms",
    "skeleton",
    "stereo_width",
    "sustain_rms_db",
    "sustain_slope_db_s",
    "tilt_rise_db",
    "tilt_strike_db",
    "tnr_db",
    "tnr_mask",
    "tone_f0_hz",
    "tone_lowest_hz",
    "trem_db",
    "trem_rate_hz",
    "velocity",
    "vib_cents",
    "vib_rate_hz",
    "window_flatness_db",
    "window_flatness_measured",
    "window_flatness_valid",
]

#: The terms a loop record can take weight off a row (`loop_record.LOOP_TERM_DIMENSION`).
LOOP_TERMS = ("cents", "tnr", "slope", "tail", "mod")

ROOM_KEYS = ("rt60_s", "hf_ratio", "tail_db", "predelay_ms", "tail_window_s", "note_window_s")

_ID = re.compile(r"^[A-Za-z0-9_.\-]{1,64}$")
_ID_OR_EMPTY = re.compile(r"^[A-Za-z0-9_.\-]{0,64}$")
_CSV = re.compile(r"^[0-9, ]{0,512}$")
_HEX = re.compile(r"^[0-9a-f]{16,64}$")

_ROW = Obj(
    {
        **dict.fromkeys(ROW_KEYS, "datum"),
        "tnr_mask": Obj(
            {
                "fm": "datum",
                "am": "datum",
                "modal": "bool",
                "stationary_lines_hz": "datum",
                "body_lines_hz": "datum",
            },
            required=False,
        ),
        "skeleton": Obj(
            {k: "datum" for k in ("init_db", "early_db_s", "late_db_s", "tail_db_s")},
            required=False,
        ),
        "loop_unweighted": EnumList(LOOP_TERMS),
    },
    required=False,
)

_DESCRIPTOR = Obj(
    {
        "capture_id": "id",
        "timbre": "id",
        "label": "text",
        "sample_rate": "int",
        "gate_s": "num",
        "preroll_s": "num",
        "slot_s": "num",
        "notes": "datum",
        "velocities": "datum",
        "render_keys": Table("datum"),
        "dry": "bool",
        "room": Enum(("unclassified", "none", "present")),
        "rig": Enum(("unclassified", "none", "baked")),
        "rig_evidence": Enum(("verified", "declared", "inferred", "unknown")),
        "channel": "int",
        "groups": Table(Obj({"name": "id", "notes": "datum"})),
        "slots": Table("datum"),
        "gates": Table("datum"),
        "note_map": Table("datum"),
    }
)

SCHEMA = Obj(
    {
        "schema": "int",
        "identity": Obj(
            {
                "capture": "id",
                "timbre": "id_or_empty",
                "pattern": Enum(("corpus",)),
                "notes": "csv",
                "velocities": "csv",
                "drum_note": "int_or_none",
                "drum_gate_ms": "num",
                "mono_mode": Enum(("mean", "left", "loudest")),
                "room": Enum(("auto", "none")),
                "role": Enum(ROLES),
            }
        ),
        "digests": Obj(
            {
                "corpus": "hex",
                "code": "hex",
                "reference": "hex_or_absent",
                "objective": "hex",
            }
        ),
        "corpus": _DESCRIPTOR,
        "oracle": Obj(
            {
                "rows": Table(_ROW),
                "room": Maybe(Obj({k: "num" for k in ROOM_KEYS})),
                "band_edge": "num_or_none",
                "channel_correlation": "num_or_none",
            }
        ),
    }
)


def _is_number(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _scalar(v) -> bool:
    return v is None or isinstance(v, bool) or _is_number(v)


def _check_datum(v, path: str) -> None:
    if _scalar(v):
        return
    if not isinstance(v, list):
        raise CacheError(
            f"{path}: {type(v).__name__} may not leave; a number, a bool, None or a list of them"
        )
    if len(v) > MAX_ARRAY:
        raise CacheError(
            f"{path}: array too large to leave (could be a spectrum): {len(v)} entries, the cap is {MAX_ARRAY}"
        )
    for i, item in enumerate(v):
        if isinstance(item, list):
            if len(item) > MAX_ARRAY:
                raise CacheError(
                    f"{path}[{i}]: array too large to leave (could be a spectrum): "
                    f"{len(item)} entries, the cap is {MAX_ARRAY}"
                )
            if not all(_scalar(x) for x in item):
                raise CacheError(f"{path}[{i}]: a list holds numbers only")
        elif not _scalar(item):
            raise CacheError(f"{path}[{i}]: {type(item).__name__} may not leave")


def _check_leaf(v, kind: str, path: str) -> None:
    ok = {
        "int": lambda: isinstance(v, int) and not isinstance(v, bool),
        "num": lambda: _is_number(v),
        "bool": lambda: isinstance(v, bool),
        "num_or_none": lambda: v is None or _is_number(v),
        "int_or_none": lambda: v is None or (isinstance(v, int) and not isinstance(v, bool)),
        "id": lambda: isinstance(v, str) and bool(_ID.match(v)),
        "id_or_empty": lambda: isinstance(v, str) and bool(_ID_OR_EMPTY.match(v)),
        "csv": lambda: isinstance(v, str) and bool(_CSV.match(v)),
        "text": lambda: isinstance(v, str) and 0 < len(v) <= 120 and v.isprintable(),
        "hex": lambda: isinstance(v, str) and bool(_HEX.match(v)),
        "hex_or_absent": lambda: isinstance(v, str) and (v == "absent" or bool(_HEX.match(v))),
    }
    if kind == "datum":
        _check_datum(v, path)
    elif kind not in ok:
        raise AssertionError(f"unknown schema leaf {kind!r}")
    elif not ok[kind]():
        raise CacheError(f"{path}: {v!r:.60} is not a valid {kind}")


def check(value, spec=SCHEMA, path: str = "cache") -> None:
    """Refuse `value` unless it is exactly what the whitelist names; the message names the key."""
    if isinstance(spec, str):
        _check_leaf(value, spec, path)
    elif isinstance(spec, Enum):
        if value not in spec.values:
            raise CacheError(f"{path}: {value!r} is not one of {', '.join(spec.values)}")
    elif isinstance(spec, EnumList):
        if not isinstance(value, list) or any(x not in spec.values for x in value):
            raise CacheError(f"{path}: {value!r} is not a list of {', '.join(spec.values)}")
    elif isinstance(spec, Maybe):
        if value is not None:
            check(value, spec.spec, path)
    elif isinstance(spec, Table):
        if not isinstance(value, list):
            raise CacheError(f"{path}: a table is a list")
        if len(value) > MAX_TABLE:
            raise CacheError(
                f"{path}: table too large to leave: {len(value)} rows, the cap is {MAX_TABLE}"
            )
        for i, item in enumerate(value):
            check(item, spec.row, f"{path}[{i}]")
    elif isinstance(spec, Obj):
        if not isinstance(value, dict):
            raise CacheError(f"{path}: expected an object, got {type(value).__name__}")
        for key in value:
            if key not in spec.fields:
                raise CacheError(
                    f"{path}: unknown key {key!r}; add it to the whitelist in oracle_cache.py "
                    f"deliberately, or keep it out of the cache"
                )
        if spec.required:
            missing = [k for k in spec.fields if k not in value]
            if missing:
                raise CacheError(f"{path}: missing {', '.join(missing)}")
        for key, item in value.items():
            check(item, spec.fields[key], f"{path}.{key}")
    else:
        raise TypeError(f"unknown schema node {spec!r}")


def plain(value):
    """`value` as plain JSON types: numpy scalars and arrays, tuples and Rooms folded down."""
    if isinstance(value, dict):
        return {str(k): plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    if isinstance(value, np.ndarray):
        return plain(value.tolist())
    if isinstance(value, np.generic):
        return value.item()
    return value


# --------------------------------------------------------------------------- #
# Digests and identity
# --------------------------------------------------------------------------- #
def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def corpus_digest(manifest: Path, wavs: list[Path], root: Path) -> str:
    """sha256 over the manifest's bytes and the (relative path, byte hash) of every WAV read."""
    entries = []
    for wav in wavs:
        try:
            rel = wav.relative_to(root).as_posix()
        except ValueError:
            rel = wav.name
        entries.append((rel, _sha(wav.read_bytes())))
    entries.append(("manifest", _sha(manifest.read_bytes())))
    h = hashlib.sha256()
    for rel, digest in sorted(entries):
        h.update(f"{rel}\0{digest}\n".encode())
    return h.hexdigest()


def reference_digest(capture_id: str, directory: Path | None = None) -> str:
    """The committed reference profile's bytes, which the rows are read through; `absent` without one."""
    path = (directory or HERE / "reference") / f"{capture_id}.json"
    return _sha(path.read_bytes()) if capture_id and path.exists() else "absent"


_CODE: dict[Path, str] = {}


def _function_code(path: Path, name: str) -> str:
    text = path.read_text()
    for node in ast.walk(ast.parse(text)):
        if isinstance(node, ast.FunctionDef) and node.name == name:
            return objective.code_text(ast.get_source_segment(text, node) or "")
    raise AssertionError(f"{name} not found in {path.name}")


def code_digest(directory: Path = HERE) -> str:
    """The measuring code: the modules `CODE_ROOTS` reach, and `oracle_reference` itself."""
    if directory not in _CODE:
        h = hashlib.sha256(objective.objective_digest(directory, CODE_ROOTS).encode())
        h.update(_function_code(directory / "autofit.py", "oracle_reference").encode())
        _CODE[directory] = h.hexdigest()
    return _CODE[directory]


def capture_key(path: str) -> str:
    """The name a cache file carries for a corpus path: its directory's name."""
    p = Path(path)
    return (p.parent if p.suffix == ".json" else p).name


def cache_file(directory: str | Path, key: str, timbre: str, role: str) -> Path:
    """`<dir>/<capture>__<timbre or default>__<role>.json.gz`: one cache per capture, timbre and role."""
    return Path(directory) / f"{key}__{timbre or 'default'}__{role}.json.gz"


def identity_of(args, key: str, timbre: str, role: str) -> dict:
    """Everything `oracle_reference` reads from the run that decides its result."""
    drum_note = getattr(args, "drum_note", None)
    return {
        "capture": key,
        "timbre": timbre,
        "pattern": "corpus",
        "notes": str(getattr(args, "notes", "") or ""),
        "velocities": str(getattr(args, "velocities", "") or ""),
        "drum_note": None if drum_note is None else int(drum_note),
        "drum_gate_ms": getattr(args, "drum_gate_ms", 0) or 0,
        "mono_mode": getattr(args, "mono_mode", "mean"),
        "room": getattr(args, "room", "auto"),
        "role": role,
    }


# --------------------------------------------------------------------------- #
# Files
# --------------------------------------------------------------------------- #
@dataclass
class Cached:
    rows: list[dict]
    room: object
    band_edge: float | None
    channel_correlation: float | None
    descriptor: dict
    digest: str
    path: Path
    digests: dict


#: What this run took from caches: `oracle_source` reports it in the fit record.
_CONSUMED: list[dict] = []


def reset_consumed() -> None:
    _CONSUMED.clear()


def consumed() -> list[dict]:
    return list(_CONSUMED)


def _canonical(body: dict) -> bytes:
    return json.dumps(body, sort_keys=True, separators=(",", ":")).encode()


def write(
    path: Path,
    *,
    identity: dict,
    digests: dict,
    descriptor: dict,
    rows: list[dict],
    room,
    band_edge,
    channel_correlation,
) -> str:
    """Validate and write one cache; returns its digest. Nothing is written if the whitelist refuses."""
    body = plain(
        {
            "schema": SCHEMA_VERSION,
            "identity": identity,
            "digests": digests,
            "corpus": descriptor,
            "oracle": {
                "rows": rows,
                "room": None if room is None else room.to_dict(),
                "band_edge": band_edge,
                "channel_correlation": channel_correlation,
            },
        }
    )
    check(body)
    data = _canonical(body)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as gz:
        gz.write(data)
    os.replace(tmp, path)
    return _sha(data)


def read(path: Path, expect: dict | None = None) -> Cached:
    """Read one cache and refuse it unless it may stand in for the oracle.

    `expect` is `{"identity": ..., "corpus_digest": ... or None}`. Without it only
    the whitelist and the schema version are checked, which is all a caller that
    wants the descriptor needs; the oracle's own caller always passes it.
    """
    from room import Room

    if not path.exists():
        raise FileNotFoundError(
            f"no oracle cache at {path}; write one on the host that holds the corpus "
            f"(run the same command with --oracle-cache {path.parent})"
        )
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rb") as fh:
        body = json.loads(fh.read())
    check(body)
    if body["schema"] != SCHEMA_VERSION:
        raise CacheError(f"{path.name}: schema {body['schema']}, this host reads {SCHEMA_VERSION}")
    digests = body["digests"]
    if expect is not None:
        wrong = sorted(
            k for k in expect["identity"] if body["identity"].get(k) != expect["identity"][k]
        )
        if wrong:
            detail = ", ".join(
                f"{k}: cache {body['identity'].get(k)!r}, run {expect['identity'][k]!r}"
                for k in wrong
            )
            raise CacheError(f"{path.name} answers a different question ({detail})")
        here = code_digest()
        if digests["code"] != here:
            raise CacheError(
                f"{path.name} is stale: the code that measured it (digest {digests['code'][:12]}) "
                f"is not this host's (digest {here[:12]}); rewrite it where the corpus is"
            )
        ref = expect.get("reference_digest")
        if ref and digests["reference"] != ref:
            raise CacheError(
                f"{path.name} is stale: it was read through a different committed reference "
                f"profile (digest {digests['reference'][:12]}) than this host's ({ref[:12]})"
            )
        if expect.get("corpus_digest") and digests["corpus"] != expect["corpus_digest"]:
            raise CacheError(
                f"{path.name} is stale: its corpus digest {digests['corpus'][:12]} differs from "
                f"the corpus on this host ({expect['corpus_digest'][:12]})"
            )
    oracle = body["oracle"]
    digest = _sha(_canonical(body))
    cached = Cached(
        rows=oracle["rows"],
        room=None if oracle["room"] is None else Room(**oracle["room"]),
        band_edge=oracle["band_edge"],
        channel_correlation=oracle["channel_correlation"],
        descriptor=body["corpus"],
        digest=digest,
        path=path,
        digests=digests,
    )
    if expect is not None:
        _CONSUMED.append(
            {
                "capture": body["identity"]["capture"],
                "timbre": body["identity"]["timbre"],
                "role": body["identity"]["role"],
                "digest": digest,
            }
        )
    return cached


def source_record() -> dict | None:
    """The fit report's `oracle` entry when any oracle came from a cache, else None."""
    used = consumed()
    if not used:
        return None
    h = hashlib.sha256()
    for entry in sorted(used, key=lambda e: (e["capture"], e["timbre"], e["role"])):
        h.update(entry["digest"].encode())
    return {"source": "cache", "cache_digest": h.hexdigest(), "caches": used}
