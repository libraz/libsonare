"""Generate the GS classic realization's model data from the soundings archive.

Each insertion-effect type's classic model is the archive's own leading candidate
(the stage record's ``p0``) -- a node graph drawn at 32 kHz -- converted into the
pools ``src/midi/synth/gs_classic/model_format.h`` defines. Two configurations are
generated from one pass: the default one, with ``tools/gs/classic-overlays/*.json``
applied, and the raw one without them, which the conformance test holds against
reference digests the archive's own renderer draws.

Byte maps and control curves are evaluated by the archive's code
(``soundings.reproduce._from_map`` and the renderer's value reader), never by a
second definition written here; the schedule (components, loop order) is the
renderer's too. The archive's modules need its virtual environment, so this script
starts under any python and re-executes itself under ``<archive>/.venv/bin/python``.

Stops (exit 1) on: a ``source: document`` value that none of the four document
forms reproduces; a node kind or reference type outside the vocabulary the C++
engine draws; an ``x-`` kind on the archive side; a section whose ``gain_db`` or
``sections`` a control drives, which the renderer refuses; an overlay entry that rewrites an
existing node without ``replaces``; and, except under ``--raw``, a printed slot no
node reads (denominator 770, ``02 0C`` being the archive's ``03 00``).

Usage::

    classic_models.py [--archive DIR] [--raw | --scope SPEC] [--check] [--overlays DIR]
    classic_models.py --check-overlays [--overlays DIR]

``--archive`` defaults to ``$GS_EFX_ARCHIVE``. ``--scope`` takes ``MSB[:LSB-LSB]``
items separated by commas (hex); an empty value puts no type in scope.
``--check-overlays`` needs no archive: it compares the overlay set with the digest recorded in
the committed default ``.inc`` and exits 1 when they differ.
"""

from __future__ import annotations

import argparse
import ctypes
import difflib
import gzip
import hashlib
import importlib.util
import json
import math
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent
UNIT = "roland-sc8850-01"
EFFECT_BLOCK = "40 03"
FIRST_PARAMETER = 0x03
SLOTS = 20

OUT_DEFAULT = REPO / "src/midi/synth/gs_classic_models.inc"
OUT_RAW = REPO / "tests/midi/gs_classic_models_raw.inc"
OUT_TSV = REPO / "tests/midi/gs_classic_reference.tsv"
OUT_EXPANSION = REPO / "tests/midi/gs_classic_map_expansion.inc"
OUT_DOC = REPO / "tools/gs/docs/classic.md"
DEFAULT_OVERLAYS = TOOLS / "classic-overlays"
TABLES = TOOLS / "efx-tables.json"

DOC_BEGIN = "<!-- BEGIN GENERATED: tools/gs/classic_models.py -->"
DOC_END = "<!-- END GENERATED: tools/gs/classic_models.py -->"

REEXEC_MARK = "GS_CLASSIC_MODELS_REEXEC"
RENDER_WORKERS = 2

# The kinds the C++ engine draws; `x-noise` is written only by an overlay.
VOCABULARY = (
    "gain",
    "mix",
    "pan",
    "delay",
    "lfo",
    "section",
    "shaper",
    "envelope",
    "gain_computer",
    "vca",
    "hold",
    "quantize",
    "pitch",
)
OVERLAY_KINDS = ("x-noise",)
KIND_INDEX = {k: i for i, k in enumerate((*VOCABULARY, "x-noise"))}
KIND_NAME = {
    "gain": "kGain",
    "mix": "kMix",
    "pan": "kPan",
    "delay": "kDelay",
    "lfo": "kLfo",
    "section": "kSection",
    "shaper": "kShaper",
    "envelope": "kEnvelope",
    "gain_computer": "kGainComputer",
    "vca": "kVca",
    "hold": "kHold",
    "quantize": "kQuantize",
    "pitch": "kPitch",
    "x-noise": "kXNoise",
}
# The one reference a node may make: a `pan` names a law model of kind `pan`.
REFERENCE_KINDS = {"pan": ("law", "pan")}

# Fields each kind is converted from. Any other field must be commentary or provenance,
# which the renderer does not read either (below).
FIELDS = {
    "gain": {"input", "gain", "unit"},
    "mix": {"inputs", "weights"},
    "pan": {"input", "position", "law"},
    "delay": {"input", "time_ms", "interpolation", "modulated_by"},
    "lfo": {"shape", "points", "rate_hz", "phase_offset"},
    "section": {
        "input",
        "stage",
        "side",
        "order",
        "form",
        "corner_hz",
        "centre_hz",
        "gain_db",
        "q",
        "sections",
        "mix",
        "reached_by",
    },
    "shaper": {"input", "curve", "points", "drive", "oversample"},
    "envelope": {
        "input",
        "detector",
        "topology",
        "domain",
        "attack_ms",
        "release_ms",
        "floor_db",
    },
    "gain_computer": {"input", "threshold_db", "ratio", "knee_db"},
    "vca": {"input", "control"},
    "hold": {"input", "rate_hz"},
    "quantize": {"input", "bits"},
    "pitch": {"input", "ratio", "window_ms", "crossfade", "interpolation"},
    "x-noise": {"noise", "level", "parameter", "seed"},
}
COMMENTARY = re.compile(r"^(why.*|is|.*_is|.*_from)$")
# Section values that decide which sections a stage is built from; the renderer's
# `_scalar` refuses a control on them, so the engine never draws one.
SECTION_FIXED = ("gain_db", "sections")

INTERPOLATION = {"none": 0, "linear": 1}
CROSSFADE = {"hann": 0, "linear": 1, "s_curve": 2}
LFO_SHAPE = {"sine": 0, "triangle": 1, "square": 2, "saw": 3, "points": 4}
SHAPER_CURVE = {"tanh": 0, "hard": 1, "cubic": 2, "points": 3}
STAGE = {"shelf": 0, "peaking": 1, "pole": 2, "allpass-chain": 3}
SECTION_FORM = {"bilinear": 0, "one-multiply": 1}
SIDE = {"low": 0, "high": 1}
NOISE = {"white": 0, "pink": 1, "radio": 2, "disc": 3, "hum": 4}
STORED_AT = {"full-boost": 0, "full-cut": 1}
OTHER_SIDE = {"the-same-section-inverted": 0, "a-second-section-stored-at-the-other-end": 1}
MAP_KIND = {"stepped-table": 0, "window": 1, "states": 2, "points": 3, "table": 4}
MAP_KIND_NAME = ("kSteppedTable", "kWindow", "kStates", "kPoints", "kTable")
TOPOLOGY_NAME = ("kNotApplicable", "kSideBySide", "kInSeries")
TOPOLOGY_WORD = ("-", "side-by-side", "in-series")

ENVELOPE_RMS, ENVELOPE_DECOUPLED, ENVELOPE_LOG = 1, 2, 4
WILDCARD = 255
NONE16 = 0xFFFF
ABSENT8 = 0xFF
LUT_SIZE = 128
CURVE_SIZE = 256
MAX_INPUTS = 16
PARALLEL_MSB = 0x11

# D5: a document value passes only in one of these four forms.
DOCUMENT_POINTS_MAX = 3
DOCUMENT_STATES_MAX = 5
FORMULA_TOLERANCE = 1e-6

# Reference stimulus and digest (the conformance test restates both).
FS = 32000
STIMULUS_SEEDS = (0x12345678, 0x9E3779B9)
STIMULUS_SAW_HZ = 220
STIMULUS_SCALE = 0.25
DIGEST_WINDOW = 8000
BAND_EXPONENTS = range(-13, 12)  # centres 1000 * 2^(n/3): 49.6 Hz .. 12.7 kHz
DIGEST_FLOOR_DB = -300.0


def stop(message: str) -> None:
    sys.exit(f"classic_models: {message}")


# ---------------------------------------------------------------- the archive's python


def _archive_arg(argv: list[str]) -> Path | None:
    for i, arg in enumerate(argv):
        if arg == "--archive" and i + 1 < len(argv):
            return Path(argv[i + 1])
        if arg.startswith("--archive="):
            return Path(arg.split("=", 1)[1])
    env = os.environ.get("GS_EFX_ARCHIVE")
    return Path(env) if env else None


def reexec_under_archive_venv() -> None:
    """Run this script again under the archive's venv unless that is where it runs."""
    if os.environ.get(REEXEC_MARK) == "1":
        return
    archive = _archive_arg(sys.argv[1:])
    if archive is None:
        stop("no archive: pass --archive or set GS_EFX_ARCHIVE to a soundings checkout")
    python = archive / ".venv" / "bin" / "python"
    if not python.is_file():
        stop(
            f"{python} does not exist; this generator imports soundings.reproduce and "
            "soundings.render.graph, which need the archive's own virtual environment"
        )
    os.environ[REEXEC_MARK] = "1"
    os.execv(str(python), [str(python), str(Path(__file__).resolve()), *sys.argv[1:]])


def load_sibling(name: str, file: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# ---------------------------------------------------------------- revision


def archive_revision(root: Path, inputs: list[Path]) -> dict:
    """What identifies the inputs read, in the form ``derive_efx_tables.py`` records."""
    root = root.resolve()
    inside: list[Path] = []
    for path in inputs:
        resolved = Path(path).resolve()
        try:
            resolved.relative_to(root)
        except ValueError:
            # A dependency outside the archive is not archive input.
            continue
        inside.append(resolved)
    inputs = sorted(set(inside))
    try:
        rev = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        digest = hashlib.sha256()
        for path in sorted(inputs):
            digest.update(path.relative_to(root).as_posix().encode())
            digest.update(path.read_bytes())
        return {
            "archive_revision": f"sha256:{digest.hexdigest()}",
            "archive_revision_source": "a hash over the files read; the archive is not a git tree",
            "archive_inputs_dirty": False,
        }
    status = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "status",
            "--porcelain",
            "--",
            *[p.relative_to(root).as_posix() for p in inputs],
        ],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    return {
        "archive_revision": rev,
        "archive_revision_source": "git rev-parse HEAD",
        "archive_inputs_dirty": bool(status),
    }


def package_sources(root: Path, package_dir: Path) -> set[Path]:
    """Every tracked ``*.py`` of the renderer package, when it lives inside the archive.

    The revision must identify the renderer implementation as well as the JSON it
    reads. A package installed under ``.venv`` or from a sibling checkout is not
    archive input, so it yields nothing.
    """
    root = root.resolve()
    package_dir = package_dir.resolve()
    try:
        relative = package_dir.relative_to(root)
    except ValueError:
        return set()
    if ".venv" in relative.parts:
        return set()
    try:
        listed = subprocess.run(
            ["git", "-C", str(root), "ls-files", "--", relative.as_posix()],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.splitlines()
        paths = [root / name for name in listed if name.endswith(".py")]
    except (OSError, subprocess.CalledProcessError):
        paths = sorted(package_dir.rglob("*.py"))
    return {path.resolve() for path in paths if path.is_file()}


# ---------------------------------------------------------------- helpers


def type_number(key: str) -> int:
    msb, lsb = key.split()
    return int(msb, 16) << 8 | int(lsb, 16)


def type_key(number: int) -> str:
    return f"{number >> 8:02X} {number & 0xFF:02X}"


def slot_of(address: str, where: str) -> int:
    parts = address.split()
    if len(parts) != 3 or " ".join(parts[:2]) != EFFECT_BLOCK:
        stop(f"{where} reads {address!r}, which is not an EFX parameter address")
    slot = int(parts[2], 16) - FIRST_PARAMETER
    if not 0 <= slot < SLOTS:
        stop(f"{where} reads {address!r}, outside the {SLOTS} parameter slots")
    return slot


def address_of(slot: int) -> str:
    return f"{EFFECT_BLOCK} {FIRST_PARAMETER + slot:02X}"


def is_value(item) -> bool:
    return isinstance(item, dict) and (
        "value" in item
        or ("byte" in item and ("map" in item or "law" in item))
        or "control" in item
    )


def node_values(node: dict):
    """Every (field, value) of a node, weights and modulation depth included."""
    for key, item in node.items():
        if key == "weights" and isinstance(item, dict):
            for ref, weight in item.items():
                yield f"weights.{ref}", weight
        elif key == "modulated_by" and isinstance(item, dict):
            yield "modulated_by.depth_ms", item.get("depth_ms")
        elif is_value(item):
            yield key, item


def f32(value: float) -> float:
    return ctypes.c_float(value).value


def float_lit(value: float) -> str:
    value = f32(value)
    if math.isnan(value) or math.isinf(value):
        stop(f"a float value {value} cannot be written as a literal")
    text = f"{value:.9g}"
    if "." not in text and "e" not in text:
        text += ".0"
    return text + "f"


def double_lit(value: float) -> str:
    if math.isnan(value) or math.isinf(value):
        stop(f"a double value {value} cannot be written as a literal")
    return repr(float(value))


# ---------------------------------------------------------------- D5


RATIO_UNITS = (("semitone ratio", 12.0), ("cent ratio", 1200.0))


def _ratio_fit(keys: list[int], ys: list[float], per_octave: float) -> tuple[float, float] | None:
    """(offset, step) with y = 2^((offset + step k) / per_octave) and a whole step, or None."""
    logs = [per_octave * math.log2(y) for y in ys]
    step = round((logs[-1] - logs[0]) / (keys[-1] - keys[0]))
    if step == 0:
        return None
    offsets = [v - step * k for k, v in zip(keys, logs, strict=True)]
    if max(offsets) - min(offsets) > FORMULA_TOLERANCE * per_octave:
        return None
    offset = sum(offsets) / len(offsets)
    if abs(offset - round(offset)) <= FORMULA_TOLERANCE * per_octave:
        offset = float(round(offset))
    return offset, float(step)


def _formula(keys: list[int], ys: list[float]) -> tuple[str, list[float]] | None:
    """The formula a many-state document column follows, with the values it gives."""
    if all(y == ys[0] for y in ys):
        return "constant", [ys[0]] * len(keys)
    if all(y > 0 for y in ys):
        for name, per_octave in RATIO_UNITS:
            fit = _ratio_fit(keys, ys, per_octave)
            if fit is not None:
                offset, step = fit
                return name, [2.0 ** ((offset + step * k) / per_octave) for k in keys]
    slope = (ys[-1] - ys[0]) / (keys[-1] - keys[0])
    derived = [ys[0] + slope * (k - keys[0]) for k in keys]
    scale = max(abs(y) for y in ys)
    if all(abs(d - y) <= FORMULA_TOLERANCE * scale for d, y in zip(derived, ys, strict=True)):
        return "linear step", derived
    return None


def check_document(value: dict, where: str, tally: dict) -> dict:
    """D5: pass a document value in one of the four forms; a formula is re-derived."""
    if "value" in value:
        tally["constant"] = tally.get("constant", 0) + 1
        return value
    if "control" in value:
        stop(f"{where}: a control curve is sourced from a document, which D5 does not pass")
    spec = value["map"]
    kind = spec["kind"]
    if kind == "points" and len(spec["points"]) <= DOCUMENT_POINTS_MAX:
        tally["few points"] = tally.get("few points", 0) + 1
        return value
    if kind == "states":
        named = {k: v for k, v in spec["values"].items() if k != "*"}
        if len(named) <= DOCUMENT_STATES_MAX:
            tally["enumeration"] = tally.get("enumeration", 0) + 1
            return value
        keys = sorted(int(k) for k in named)
        ys = [float(named[str(k)]) for k in keys]
        found = _formula(keys, ys)
        if found is not None:
            name, formula_values = found
            tally[name] = tally.get(name, 0) + 1
            derived = dict(zip((str(k) for k in keys), formula_values, strict=True))
            if "*" in spec["values"]:
                derived["*"] = spec["values"]["*"]
            return {**value, "map": {**spec, "values": derived}}
    stop(
        f"{where}: a `source: document` {kind} map is none of few points (<= "
        f"{DOCUMENT_POINTS_MAX}), an enumeration (<= {DOCUMENT_STATES_MAX} states), a "
        "semitone/cent-ratio or linear-step formula, or a constant column"
    )
    return value


# ---------------------------------------------------------------- vocabulary


def check_vocabulary(model: dict, where: str, *, overlay: bool) -> None:
    for node in model["nodes"]:
        kind = node.get("kind")
        label = f"{where} node {node.get('id')!r}"
        if isinstance(kind, str) and kind.startswith("x-") and not overlay:
            stop(f"{label} is a {kind!r}: an `x-` kind is overlay-only and the archive wrote one")
        if kind not in VOCABULARY and not (overlay and kind in OVERLAY_KINDS):
            stop(
                f"{label} is a {kind!r}, outside the kinds the C++ engine draws "
                f"({', '.join(VOCABULARY)})"
            )
        for key in node:
            if key in ("id", "kind") or key in FIELDS[kind] or COMMENTARY.match(key):
                continue
            stop(f"{label} carries `{key}`, a field the classic data format has no place for")
        if kind == "section":
            for key in SECTION_FIXED:
                if isinstance(node.get(key), dict) and "control" in node[key]:
                    stop(
                        f"{label} drives `{key}` by a control, which the renderer refuses: "
                        "it decides the sections the stage is built from"
                    )
        if kind == "shaper":
            oversample = node.get("oversample")
            if isinstance(oversample, bool) or not isinstance(oversample, int) or oversample != 1:
                stop(
                    f"{label} oversamples by {oversample!r}, but the classic engine supports "
                    "only the integer factor 1"
                )
        for key in ("model", "record"):
            if key in node:
                stop(
                    f"{label} refers by `{key}`, a reference type the classic engine does not read"
                )


def check_references(model: dict, models_dir: Path, where: str) -> list[str]:
    laws = []
    for node in model["nodes"]:
        if node["kind"] not in REFERENCE_KINDS:
            continue
        key, kind = REFERENCE_KINDS[node["kind"]]
        name = node[key]
        other = json.loads((models_dir / name).read_text())
        if other["model"].get("kind") != kind:
            stop(f"{where} node {node['id']!r} names {name}, which is not a `{kind}` model")
        for side in ("left", "right"):
            for position, _ in other["sides"][side]:
                if not 0 <= int(position) < LUT_SIZE:
                    stop(f"{name} places a point at {position}, off the 0..127 axis")
        laws.append(name)
    return sorted(set(laws))


# ---------------------------------------------------------------- overlays


OVERLAY_ENTRY_KEYS = ("node", "basis", "rationale", "replaced_when")
OVERLAY_COVER_KEYS = ("covers", "rationale", "replaced_when")
BASES = ("invented", "carried")
COVERS_ID = re.compile(r"^[0-9a-f]{12}$")


def load_overlays(directory: Path) -> tuple[dict[int, list[dict]], dict[int, list[dict]]]:
    """Each type's overlay entries, and the file-level `covers` that change no node."""
    by_type: dict[int, list[dict]] = {}
    covers: dict[int, list[dict]] = {}
    if not directory.is_dir():
        return by_type, covers
    for path in sorted(directory.glob("*.json")):
        data = json.loads(path.read_text())
        number = type_number(data["type"])
        for i, cover in enumerate(data.get("covers", [])):
            where = f"{path.name} covers[{i}]"
            for key in OVERLAY_COVER_KEYS:
                if key not in cover:
                    stop(f"{where} has no `{key}`")
            if cover["replaced_when"] != {"covers": cover["covers"]}:
                stop(f"{where} is replaced when its own item changes, and names otherwise")
            cover["_where"] = where
            covers.setdefault(number, []).append(cover)
        entries = data.get("entries", [])
        for i, entry in enumerate(entries):
            where = f"{path.name}[{i}]"
            for key in OVERLAY_ENTRY_KEYS:
                if key not in entry:
                    stop(f"{where} has no `{key}`")
            if entry["basis"] not in BASES:
                stop(f"{where} has basis {entry['basis']!r}, not one of {BASES}")
            if entry["basis"] == "carried" and "from" not in entry:
                stop(f"{where} is carried and names no `from`")
            for field, value in node_values(entry["node"]):
                if "law" in value and "map" in value:
                    stop(f"{where} {field} names a law and writes a map of its own")
            entry["_where"] = where
        by_type.setdefault(number, []).extend(entries)
    return by_type, covers


def check_covers(
    archive, types: list[dict], overlays: dict[int, list[dict]], covers: dict[int, list[dict]]
) -> None:
    """Every `covers`, on an entry or on a file, names an item no candidate predicts.

    The id is the first twelve hex digits of the sha1 of the item's text in the type's
    ``inferences/candidates/whole-<MMLL>.json`` ``what_no_candidate_here_predicts``.
    """
    archive_key = {t["number"]: t["archive_key"] for t in types}
    for number in sorted(set(overlays) | set(covers)):
        claimed = [(e["_where"], e["covers"]) for e in overlays.get(number, []) if "covers" in e]
        filed = [(c["_where"], c["covers"]) for c in covers.get(number, [])]
        if not claimed and not filed:
            continue
        key = archive_key[number].replace(" ", "")
        record = archive.json(Path("inferences") / "candidates" / f"whole-{key}.json")
        items = {
            hashlib.sha1(text.encode("utf-8")).hexdigest()[:12]
            for text in record.get("what_no_candidate_here_predicts", [])
        }
        seen: set[str] = set()
        for where, item in filed:
            if item in seen:
                stop(f"{where} covers {item} a second time")
            seen.add(item)
        for where, item in claimed + filed:
            if not isinstance(item, str) or not COVERS_ID.match(item):
                stop(f"{where} covers {item!r}, which is not twelve hex digits of a sha1")
            if item not in items:
                stop(f"{where} covers {item}, which no item of whole-{key}.json hashes to")


def _designed_value(law: dict, byte: int, byte_lo: int, byte_hi: int) -> float:
    """``gs_efx_designed_value`` over one byte: the modern evaluator's reading of a law."""
    form, n_states = law["form"], law.get("n_states", 0)
    lo, hi = f32(law["lo"]), f32(law["hi"])
    if form == "enum":
        return float(min(byte, n_states - 1))
    if n_states > 1 and form in ("linear", "log"):
        return _designed_value({**law, "n_states": 0}, byte, 0, n_states - 1)
    inside = min(max(byte, byte_lo), byte_hi)
    if inside in (byte_lo, byte_hi) and form not in ("bipolar", "db"):
        return lo if inside == byte_lo else hi
    along = (inside - byte_lo) / (byte_hi - byte_lo)
    if form == "linear":
        return f32(lo + (hi - lo) * along)
    if form == "log":
        return f32(lo * (hi / lo) ** along)
    if form == "db":
        return f32(10.0 ** ((lo + (hi - lo) * along) / 20.0))
    if form == "bipolar":
        return f32((2.0 * along - 1.0) * hi)
    stop(f"a designed law has form {form!r}, which no evaluator reads")
    return 0.0


def _law_table(coverage, laws: dict, tables: dict, law_id: str, values: str, where: str) -> list:
    """A law named by id as the value of each byte 0..127.

    A ``d.`` id is a designed law of ``efx-designed-laws.json`` over the slot's printed
    byte range; any other id is a measured ``class.table`` of ``efx-tables.json`` whose
    entries place a frequency over a run of bytes.
    """
    if law_id.startswith("d."):
        law = coverage.designed_law(laws, law_id)
        if law is None:
            stop(f"{where} names {law_id}, which is not in the designed-law file")
        byte_lo, byte_hi = coverage.byte_domain(values)
        return [_designed_value(law, b, byte_lo, byte_hi) for b in range(LUT_SIZE)]
    gs_class, _, table = law_id.partition(".")
    body = tables["classes"].get(gs_class, {}).get("tables", {}).get(table)
    if body is None or body.get("kind") != "entries":
        stop(f"{where} names {law_id}, which is neither a designed law nor a measured table")
    out: list[float | None] = [None] * LUT_SIZE
    for entry in body["entries"]:
        if "hz" not in entry:
            stop(f"{where} names {law_id}, whose entries place no frequency")
        first, last = entry["settings"]
        for b in range(first, last + 1):
            out[b] = float(entry["hz"])
    if any(v is None for v in out):
        stop(f"{where} names {law_id}, whose entries leave a byte unplaced")
    return out


def _fitted_constant(model: dict, path: str) -> float | None:
    """The constant p0 holds at ``node.field`` (``weights.<ref>`` included), or None."""
    node_id, _, field = path.partition(".")
    nodes = {n["id"]: n for n in model["nodes"]}
    if node_id not in nodes:
        return None
    value = dict(node_values(nodes[node_id])).get(field)
    if value is None or "value" not in value:
        return None
    return float(value["value"])


def _anchored(lut: list[float], fitted: float, at: int, field: str, where: str) -> list[float]:
    """D33: shift a law so the power-on byte reads p0's fitted constant.

    A decibel field moves by the difference; any other quantity (a time, a frequency, a
    linear multiplier) scales by the ratio.
    """
    if field.split(".")[-1].endswith("_db"):
        shift = fitted - lut[at]
        out = [v + shift for v in lut]
    else:
        if lut[at] == 0.0:
            stop(f"{where}: the law is 0 at the power-on byte, so no ratio anchors it")
        scale = fitted / lut[at]
        out = [v * scale for v in lut]
    out[at] = fitted
    return out


def resolve_overlay_laws(
    coverage,
    overlays: dict[int, list[dict]],
    printed: dict[str, dict[int, str]],
    power_on: dict[int, list[int]],
    models: dict[int, dict],
) -> None:
    """Turn every ``{"byte", "law"}`` value of an overlay into a 128-entry table map.

    Where the law stands in for a constant p0 fitted -- the same field of the node the
    entry replaces, or the ``node.field`` an ``anchor`` names -- it is anchored to that
    constant at the power-on byte (D33).
    """
    laws = coverage.load_laws(coverage.DEFAULT_LAWS)
    tables = json.loads(TABLES.read_text())
    for number, entries in overlays.items():
        slots = printed.get(type_key(number), {})
        for entry in entries:
            node = entry["node"]
            for field, value in list(node_values(node)):
                if "law" not in value:
                    continue
                where = f"{entry['_where']} {node['id']}.{field}"
                slot = slot_of(value["byte"], where)
                if slot not in slots:
                    stop(f"{where} reads {value['byte']}, which {type_key(number)} does not print")
                lut = _law_table(coverage, laws, tables, value["law"], slots[slot], where)
                if "anchor" in value:
                    fitted = _fitted_constant(models[number], value["anchor"])
                    if fitted is None:
                        stop(f"{where} anchors to {value['anchor']}, which p0 holds no constant at")
                elif "replaces" in entry:
                    fitted = _fitted_constant(models[number], f"{entry['replaces']}.{field}")
                else:
                    fitted = None
                if fitted is not None:
                    lut = _anchored(lut, fitted, power_on[number][slot], field, where)
                spec = {"kind": "table", "entries": lut, "out_of_range": LUT_SIZE - 1}
                replace_value(node, field, {**value, "map": spec})


def apply_overlay(model: dict, entries: list[dict]) -> dict:
    """Add nodes and replace named ones; a node rewritten without `replaces` stops."""
    nodes = [dict(n) for n in model["nodes"]]
    ids = {n["id"]: i for i, n in enumerate(nodes)}
    for entry in entries:
        node = entry["node"]
        replaces = entry.get("replaces")
        where = entry["_where"]
        if node["id"] in ids:
            if replaces != node["id"]:
                stop(f"{where} writes node {node['id']!r}, which the model has, without `replaces`")
            nodes[ids[node["id"]]] = node
        elif replaces is not None:
            stop(f"{where} replaces {replaces!r}, which is not a node of this model")
        else:
            ids[node["id"]] = len(nodes)
            nodes.append(node)
    return {**model, "nodes": nodes}


# ---------------------------------------------------------------- conversion


class Pools:
    """One configuration's pools, with identical specs, curves and point runs merged."""

    def __init__(self) -> None:
        self.types: list[dict] = []
        self.nodes: list[tuple] = []
        self.components: list[tuple] = []
        self.inputs: list[int] = []
        self.values: list[tuple] = []
        self.sections: list[tuple] = []
        self.reached_by: list[tuple] = []
        self.point_lists: list[tuple] = []
        self.points: list[tuple] = []
        self.pan_laws: list[tuple] = []
        self.curves: list[tuple] = []
        self.specs: list[tuple] = []
        self.spec_maps: list[dict] = []
        self.map_keys: list[int] = []
        self.map_values: list[float] = []
        self._spec_index: dict = {}
        self._curve_index: dict = {}
        self._points_index: dict = {}
        self._law_index: dict = {}
        self._reached_index: dict = {}

    def spec(self, fields: tuple, keys: list[int], values: list[float], source: dict) -> int:
        key = (fields, tuple(keys), tuple(values))
        if key not in self._spec_index:
            kind, log, n, per_entry, out_of_range, lo, hi, power_on = fields
            self._spec_index[key] = len(self.specs)
            self.specs.append(
                (
                    kind,
                    log,
                    len(self.map_keys),
                    len(self.map_values),
                    n,
                    per_entry,
                    out_of_range,
                    lo,
                    hi,
                    power_on,
                )
            )
            self.spec_maps.append(source)
            self.map_keys.extend(keys)
            self.map_values.extend(values)
        return self._spec_index[key]

    def curve(self, values: list[float], lo: float, hi: float) -> int:
        key = (tuple(values), lo, hi)
        if key not in self._curve_index:
            self._curve_index[key] = len(self.curves)
            self.curves.append(key)
        return self._curve_index[key]

    def point_run(self, pairs: list[tuple[float, float]]) -> int:
        key = tuple(pairs)
        if key not in self._points_index:
            self._points_index[key] = len(self.point_lists)
            self.point_lists.append((len(self.points), len(pairs)))
            self.points.extend(pairs)
        return self._points_index[key]

    def law(self, name: str, table: tuple) -> int:
        if name not in self._law_index:
            self._law_index[name] = len(self.pan_laws)
            self.pan_laws.append(table)
        return self._law_index[name]

    def reached(self, entry: tuple) -> int:
        if entry not in self._reached_index:
            self._reached_index[entry] = len(self.reached_by)
            self.reached_by.append(entry)
        return self._reached_index[entry]


class Converter:
    """Turns one loaded graph into pool entries."""

    def __init__(self, ctx, pools: Pools, number: int, model: dict, laws: dict[str, dict]):
        self.ctx = ctx
        self.pools = pools
        self.number = number
        self.model = model
        self.laws = laws
        self.where = type_key(number)

    # -- maps --

    def _domain(self, slot: int) -> tuple[int, int, int]:
        lo, hi = self.ctx.accept[(self.number, slot)]
        return lo, hi, self.ctx.power_on[self.number][slot]

    def byte_spec(self, value: dict, *, db: bool) -> tuple[int, int]:
        slot = slot_of(value["byte"], self.where)
        spec = value["map"]
        kind = spec["kind"]
        if kind not in MAP_KIND:
            stop(f"{self.where} maps {value['byte']} by {kind!r}, not one of the five rules")
        fold = (lambda v: 10.0 ** (v / 20.0)) if db else (lambda v: v)
        if db and kind in ("window", "points"):
            # A dB curve is not linear in the multiplier; fold every byte into a table.
            entries = [fold(self.ctx.from_map(spec, b)) for b in range(LUT_SIZE)]
            spec, kind = {"kind": "table", "entries": entries, "out_of_range": 0}, "table"
        lo = hi = power_on = 0
        keys: list[int] = []
        if kind == "stepped-table":
            values = [fold(float(v)) for v in spec["entries"]]
            fields = (0, 0, len(values), int(spec["per_entry"]), 0)
            spec = {**spec, "entries": values}
        elif kind == "window":
            keys = [int(spec["low"]), int(spec["high"])]
            values = [float(spec["at_low"]), float(spec["at_high"])]
            fields = (1, 0, 2, 0, 0)
        elif kind == "states":
            named = sorted((int(k), fold(float(v))) for k, v in spec["values"].items() if k != "*")
            keys = [k for k, _ in named]
            values = [v for _, v in named]
            if "*" in spec["values"]:
                keys.append(WILDCARD)
                values.append(fold(float(spec["values"]["*"])))
            else:
                lo, hi, power_on = self._domain(slot)
            spec = {
                "kind": "states",
                "values": {
                    ("*" if k == WILDCARD else str(k)): v for k, v in zip(keys, values, strict=True)
                },
            }
            fields = (2, 0, len(keys), 0, 0)
        elif kind == "points":
            pairs = sorted((int(b), float(v)) for b, v in spec["points"])
            keys = [b for b, _ in pairs]
            values = [v for _, v in pairs]
            fields = (3, 1 if spec.get("log", False) else 0, len(keys), 0, 0)
        else:
            values = [fold(float(v)) for v in spec["entries"]]
            fields = (4, 0, len(values), 0, int(spec["out_of_range"]))
            spec = {**spec, "entries": values}
        for k in keys:
            if not 0 <= k <= WILDCARD or (k == WILDCARD and kind != "states"):
                stop(f"{self.where} maps {value['byte']} with a key {k} off the byte")
        kind_code, log, n, per_entry, out_of_range = fields
        index = self.pools.spec(
            (kind_code, log, n, per_entry, out_of_range, lo, hi, power_on), keys, values, spec
        )
        return slot, index

    def curve(self, value: dict, *, db: bool) -> int:
        spec = value["map"]
        xs = [float(x) for x, _ in spec["points"]]
        lo, hi = f32(min(xs)), f32(max(xs))
        grid = [lo + (hi - lo) * j / (CURVE_SIZE - 1) for j in range(CURVE_SIZE)]
        ys = self.ctx.control_curve(value, grid)
        if db:
            ys = [10.0 ** (y / 20.0) for y in ys]
        return self.pools.curve([f32(y) for y in ys], lo, hi)

    # -- values --

    def value(self, item: dict, *, db: bool = False) -> tuple:
        if "value" in item:
            v = float(item["value"])
            return (0, 0, NONE16, NONE16, 10.0 ** (v / 20.0) if db else v)
        if "byte" in item:
            slot, table = self.byte_spec(item, db=db)
            return (1, slot, table, NONE16, 0.0)
        return (2, 0, self.curve(item, db=db), self.signal(item["control"]), 0.0)

    def raw_control(self, ref: str) -> tuple:
        return (2, 0, NONE16, self.signal(ref), 0.0)

    def const(self, v: float) -> tuple:
        return (0, 0, NONE16, NONE16, float(v))

    # -- signals and schedule --

    def signal(self, ref: str) -> int:
        if ref not in self.signals:
            stop(f"{self.where} reads {ref!r}, which names no signal the classic engine carries")
        return self.signals[ref]

    def schedule(self) -> list[tuple[list[str], bool]]:
        graph = self.ctx.graph
        ids = [n["id"] for n in self.model["nodes"]]
        every, immediate = edges(graph, self.model)
        order = []
        for component in graph._components(ids, every):
            looped = graph._looped(component, every)
            order.append(
                (graph._in_order(component, immediate) if looped else [component[0]], looped)
            )
        return order

    def convert(self) -> dict:
        nodes = {n["id"]: n for n in self.model["nodes"]}
        order = self.schedule()
        self.signals = {name: i for i, name in enumerate(self.model["inputs"])}
        if list(self.model["inputs"]) != ["in_l", "in_r"]:
            stop(f"{self.where} takes {self.model['inputs']}, not in_l and in_r")
        next_signal = 2
        for ids, _ in order:
            for node_id in ids:
                if nodes[node_id]["kind"] == "pan":
                    self.signals[f"{node_id}.left"] = next_signal
                    self.signals[f"{node_id}.right"] = next_signal + 1
                    next_signal += 2
                else:
                    self.signals[node_id] = next_signal
                    next_signal += 1

        p = self.pools
        node_begin, comp_begin = len(p.nodes), len(p.components)
        longest = 1.0
        for ids, looped in order:
            first = len(p.nodes)
            for node_id in ids:
                longest = max(longest, self.node(nodes[node_id]))
            p.components.append((first, len(p.nodes), 1 if looped else 0))
        outputs = self.model["outputs"]
        return {
            "node_begin": node_begin,
            "node_end": len(p.nodes),
            "comp_begin": comp_begin,
            "comp_end": len(p.components),
            "out_l": self.signal(outputs["out_l"]),
            "out_r": self.signal(outputs["out_r"]),
            "max_delay_samples": math.ceil(longest),
        }

    # -- bounds for the delay history --

    def _largest(self, item: dict) -> float:
        if "value" in item:
            return abs(float(item["value"]))
        if "byte" in item:
            return max(abs(v) for v in self.ctx.expand(item["map"], *self._fake_domain(item)))
        return max(
            abs(v)
            for v in self.ctx.control_curve(item, [float(x) for x, _ in item["map"]["points"]])
        )

    def _fake_domain(self, item: dict) -> tuple[int, int, int]:
        return self._domain(slot_of(item["byte"], self.where))

    def _swing(self, ref: str) -> float:
        nodes = {n["id"]: n for n in self.model["nodes"]}
        node = nodes.get(ref.split(".", 1)[0])
        if node is None:
            stop(f"{self.where} modulates a delay by {ref!r}, whose swing is not bounded")
        if node["kind"] == "lfo":
            if node["shape"] == "points":
                return max(abs(float(y)) for _, y in node["points"])
            return 1.0
        if node["kind"] == "hold":
            return self._swing(node["input"])
        stop(f"{self.where} modulates a delay by a {node['kind']}, whose swing is not bounded")
        return 0.0

    # -- nodes --

    def node(self, node: dict) -> float:
        """Append one node; returns the longest read-back it can take, in samples."""
        p = self.pools
        kind = node["kind"]
        inputs: list[int] = []
        values: list[tuple] = []
        flags = 0
        aux = NONE16
        reach = 0.0
        if "input" in node:
            inputs = [self.signal(node["input"])]
        if kind == "gain":
            if node["unit"] not in ("ratio", "db"):
                stop(f"{self.where} {node['id']} has a gain in {node['unit']!r}")
            values = [self.value(node["gain"], db=node["unit"] == "db")]
        elif kind == "mix":
            inputs = [self.signal(ref) for ref in node["inputs"]]
            if len(inputs) > MAX_INPUTS:
                stop(f"{self.where} {node['id']} mixes {len(inputs)} inputs, over {MAX_INPUTS}")
            values = [self.value(node["weights"][ref]) for ref in node["inputs"]]
        elif kind == "pan":
            values = [self.value(node["position"])]
            law = self.laws[node["law"]]
            table = tuple(
                tuple(f32(v) for v in self.ctx.pan_side(law, side)) for side in ("left", "right")
            )
            aux = p.law(node["law"], table)
        elif kind == "delay":
            flags = self._enum(INTERPOLATION, node["interpolation"], node, "interpolation")
            values = [self.value(node["time_ms"])]
            reach = self._largest(node["time_ms"])
            modulation = node.get("modulated_by")
            if modulation is not None:
                values += [
                    self.value(modulation["depth_ms"]),
                    self.raw_control(modulation["control"]),
                ]
                reach += self._largest(modulation["depth_ms"]) * self._swing(modulation["control"])
            reach = reach * FS / 1000.0 + flags
        elif kind == "lfo":
            flags = self._enum(LFO_SHAPE, node["shape"], node, "shape")
            values = [self.value(node["rate_hz"]), self.value(node["phase_offset"])]
            if node["shape"] == "points":
                aux = p.point_run(sorted((float(x), float(y)) for x, y in node["points"]))
        elif kind == "section":
            aux, values = self.section(node)
        elif kind == "shaper":
            curve = self._enum(SHAPER_CURVE, node["curve"], node, "curve")
            oversample = node["oversample"]
            if isinstance(oversample, bool) or not isinstance(oversample, int) or oversample != 1:
                stop(
                    f"{self.where} {node['id']} oversamples by {oversample!r}, but the classic "
                    "engine supports only the integer factor 1"
                )
            flags = curve | oversample << 4
            values = [self.value(node["drive"])]
            if node["curve"] == "points":
                aux = p.point_run(sorted((float(x), float(y)) for x, y in node["points"]))
        elif kind == "envelope":
            flags = (
                (ENVELOPE_RMS if node["detector"] == "rms" else 0)
                | (ENVELOPE_DECOUPLED if node["topology"] == "decoupled" else 0)
                | (ENVELOPE_LOG if node["domain"] == "log" else 0)
            )
            values = [self.value(node["attack_ms"]), self.value(node["release_ms"])]
            if node["domain"] == "log":
                values.append(self.value(node["floor_db"]))
        elif kind == "gain_computer":
            values = [self.value(node[k]) for k in ("threshold_db", "ratio", "knee_db")]
        elif kind == "vca":
            values = [self.raw_control(node["control"])]
        elif kind == "hold":
            values = [self.value(node["rate_hz"])]
        elif kind == "quantize":
            aux = int(node["bits"])
        elif kind == "pitch":
            interpolation = self._enum(INTERPOLATION, node["interpolation"], node, "interpolation")
            flags = interpolation | self._enum(CROSSFADE, node["crossfade"], node, "crossfade") << 2
            values = [self.value(node["ratio"]), self.value(node["window_ms"])]
            swing = self._largest_deviation(node["ratio"])
            reach = interpolation + swing * self._largest(node["window_ms"]) * FS / 1000.0
        elif kind == "x-noise":
            flags = self._enum(NOISE, node["noise"], node, "noise")
            values = [self.value(node["level"]), self.value(node.get("parameter", {"value": 0.0}))]
            aux = int(node.get("seed", 0))
        input_begin = len(p.inputs)
        p.inputs.extend(inputs)
        value_begin = len(p.values)
        p.values.extend(values)
        p.nodes.append((kind, flags, aux, input_begin, len(inputs), len(values), value_begin))
        return reach

    def _largest_deviation(self, item: dict) -> float:
        if "value" in item:
            return abs(1.0 - float(item["value"]))
        return max(abs(1.0 - v) for v in self.ctx.expand(item["map"], *self._fake_domain(item)))

    def _enum(self, table: dict, name, node: dict, field: str) -> int:
        if name not in table:
            stop(
                f"{self.where} {node['id']} has {field} {name!r}, which the classic format cannot hold"
            )
        return table[name]

    def section(self, node: dict) -> tuple[int, list[tuple]]:
        stage = node["stage"]
        if stage not in STAGE:
            stop(
                f"{self.where} {node['id']} is a {stage!r} stage, which the classic format cannot hold"
            )
        values: list[tuple] = []

        def put(item) -> int:
            if item is None:
                return ABSENT8
            values.append(self.value(item) if isinstance(item, dict) else self.const(item))
            return len(values) - 1

        corner = put(node.get("centre_hz") if stage == "peaking" else node.get("corner_hz"))
        gain = put(node.get("gain_db"))
        q = put(node.get("q"))
        count = put(node.get("sections"))
        mix = put(node.get("mix"))
        side = self._enum(SIDE, node.get("side", "low"), node, "side")
        form = self._enum(SECTION_FORM, node.get("form", "bilinear"), node, "form")
        order = int(node.get("order", 1)) if stage == "shelf" else 0
        reached = NONE16
        if "reached_by" in node:
            r = node["reached_by"]
            reached = self.pools.reached(
                (
                    f32(float(r["full_db"])),
                    self._enum(STORED_AT, r["stored_at"], node, "reached_by.stored_at"),
                    self._enum(OTHER_SIDE, r["the_other_side"], node, "reached_by.the_other_side"),
                )
            )
        self.pools.sections.append(
            (STAGE[stage], form, order, side, corner, gain, q, count, mix, reached)
        )
        return len(self.pools.sections) - 1, values


# ---------------------------------------------------------------- the archive side


class Archive:
    """The archive's code and data, read once."""

    def __init__(self, root: Path) -> None:
        self.root = root.resolve()
        self.read: set[Path] = set()
        import soundings
        from soundings import reproduce
        from soundings.render import graph

        self.reproduce = reproduce
        self.graph = graph
        self.coverage = load_sibling("gs_coverage", "coverage.py")
        self.read |= package_sources(self.root, Path(soundings.__file__).parent)

    def json(self, rel) -> dict:
        path = self.root / rel
        self.read.add(path)
        return json.loads(path.read_text())

    def from_map(self, spec: dict, byte: int) -> float:
        return self.reproduce._from_map(spec, byte)

    def control_curve(self, value: dict, xs: list[float]) -> list[float]:
        """The renderer's own reading of a control through its map, at each x."""
        import numpy as np

        drawing = self.graph._Drawing({"sample_rate_hz": FS, "referenced": {}}, len(xs), {}, 0.0)
        drawing._signals[value["control"]] = np.asarray(xs, dtype=float)
        return [float(v) for v in np.broadcast_to(drawing.value(value, 0, len(xs)), (len(xs),))]

    def pan_side(self, law: dict, side: str) -> list[float]:
        import numpy as np

        points = sorted((int(v), float(x)) for v, x in law["sides"][side])
        grid = np.arange(LUT_SIZE, dtype=float)
        return [float(v) for v in np.interp(grid, [p[0] for p in points], [p[1] for p in points])]

    def expand(self, spec: dict, accept_lo: int, accept_hi: int, power_on: int) -> list[float]:
        """A map over 0..127: `_from_map`, and D22 where a states map names nothing."""
        out = []
        for b in range(LUT_SIZE):
            try:
                out.append(self.from_map(spec, b))
            except self.reproduce.NoStateNamed:
                out.append(self._d22(spec, b, accept_lo, accept_hi, power_on))
        return out

    def _d22(self, spec: dict, b: int, lo: int, hi: int, power_on: int) -> float:
        named = sorted(int(k) for k in spec["values"] if k != "*")

        def nearest(at: int) -> float:
            best = min(named, key=lambda k: (abs(k - at), k))
            return float(spec["values"][str(best)])

        if lo <= b <= hi:
            return nearest(b)
        if str(power_on) in spec["values"]:
            return float(spec["values"][str(power_on)])
        return nearest(power_on)


class Context:
    """Everything a conversion reads: archive code, protocol domains, power-on bytes."""

    def __init__(self, archive: Archive) -> None:
        self.archive = archive
        self.graph = archive.graph
        tables = json.loads(TABLES.read_text())
        aliases = archive.coverage.TYPE_ALIASES

        def canonical(key: str) -> int:
            return type_number(aliases.get(key, key))

        # An unmeasured power-on byte is 0, as the protocol layer's table holds it.
        self.power_on = {
            canonical(k): [0 if b is None else int(b) for b in v]
            for k, v in tables["defaults"]["by_type"].items()
        }
        states = {
            (canonical(e["type"]), int(e["parameter"])): int(e["states"])
            for e in tables["state_lists"]["slots"]
        }
        self.accept = {}
        for number in self.power_on:
            for slot in range(SLOTS):
                n = states.get((number, slot))
                self.accept[(number, slot)] = (0, n - 1) if n else (0, LUT_SIZE - 1)

    def from_map(self, spec: dict, byte: int) -> float:
        return self.archive.from_map(spec, byte)

    def control_curve(self, value: dict, xs: list[float]) -> list[float]:
        return self.archive.control_curve(value, xs)

    def pan_side(self, law: dict, side: str) -> list[float]:
        return self.archive.pan_side(law, side)

    def expand(self, spec, lo, hi, power_on) -> list[float]:
        return self.archive.expand(spec, lo, hi, power_on)


# ---------------------------------------------------------------- reading the p0 models


def read_models(archive: Archive) -> tuple[list[dict], dict[str, int]]:
    """Every type's p0, vocabulary- and D5-checked and loaded by the renderer."""
    stages = sorted((archive.root / "inferences" / UNIT / "stages").glob("*.json"))
    models_dir = archive.root / "inferences" / "models"
    aliases = archive.coverage.TYPE_ALIASES
    found = []
    d5: dict[str, int] = {}
    for stage_path in stages:
        stage = archive.json(stage_path.relative_to(archive.root))
        p0 = stage.get("p0")
        if not p0:
            continue
        key = stage["type"]
        model_path = archive.root / p0["model"]
        model = archive.json(p0["model"])
        where = f"{key} ({model_path.name})"
        check_vocabulary(model, where, overlay=False)
        laws = check_references(model, models_dir, where)
        for name in laws:
            archive.read.add(models_dir / name)
        for node in model["nodes"]:
            for field, value in list(node_values(node)):
                if isinstance(value, dict) and value.get("source") == "document":
                    checked = check_document(value, f"{where} {node['id']}.{field}", d5)
                    if checked is not value:
                        replace_value(node, field, checked)
        loaded = archive.graph.load_graph(model, models_dir=models_dir, root=archive.root)
        digest = hashlib.sha256(model_path.read_bytes())
        for name in laws:
            digest.update((models_dir / name).read_bytes())
        gross = (stage.get("p1") or {}).get("gates", {}).get("gross", {}).get("residual_over_span")
        if gross is None:
            stop(f"{stage_path.name} records no stage-8 gross residual")
        found.append(
            {
                "number": type_number(aliases.get(key, key)),
                "archive_key": key,
                "model": model,
                "loaded": loaded,
                "laws": {name: loaded["referenced"][name] for name in laws},
                "sha256": digest.hexdigest(),
                "gross": float(gross),
                "candidate": model["model"].get("candidate", model_path.stem),
                "file": model_path.name,
            }
        )
    documents = archive.root / "documents"
    archive.read |= {p for p in documents.rglob("*.json")} if documents.is_dir() else set()
    meta = archive.root / "data" / "units" / UNIT / "meta.json"
    archive.read.add(meta)
    return sorted(found, key=lambda t: t["number"]), d5


def replace_value(node: dict, field: str, value: dict) -> None:
    if field.startswith("weights."):
        node["weights"][field.split(".", 1)[1]] = value
    elif field == "modulated_by.depth_ms":
        node["modulated_by"]["depth_ms"] = value
    else:
        node[field] = value


# ---------------------------------------------------------------- per-type facts


def edges(graph, model: dict):
    """The renderer's wiring; an overlay-only kind reads nothing and leads nothing, as an lfo."""
    scheduled = [n if n["kind"] in graph.NODES else {**n, "kind": "lfo"} for n in model["nodes"]]
    return graph._edges({**model, "nodes": scheduled})


def halves(model: dict) -> tuple[set[str], set[str]]:
    ids = [n["id"] for n in model["nodes"]]
    return {i for i in ids if i.startswith("a_")}, {i for i in ids if i.startswith("b_")}


def topology(ctx: Context, number: int, model: dict) -> int:
    """Parallel-2: 1 when the halves meet only in a sum, 2 when one feeds the other."""
    if number >> 8 != PARALLEL_MSB:
        return 0
    a, b = halves(model)
    if not a or not b:
        stop(f"{type_key(number)} is Parallel-2 and its graph has no a_/b_ halves to judge")
    every, _ = edges(ctx.graph, model)

    def reached(start: set[str]) -> set[str]:
        seen, todo = set(), list(start)
        while todo:
            v = todo.pop()
            for w in every[v]:
                if w not in seen:
                    seen.add(w)
                    todo.append(w)
        return seen

    if reached(b) & a or reached(a) & b:
        return 2
    others = {n["id"] for n in model["nodes"]} - a - b
    if any(every[v] & a for v in others) and any(every[v] & b for v in others):
        return 1
    stop(f"{type_key(number)}: the halves neither feed one another nor meet in a sum")
    return 0


def pan_arrangement(model: dict) -> str:
    a, b = halves(model)
    words = []
    for name, half in (("a", a), ("b", b)):
        pans = [n for n in model["nodes"] if n["id"] in half and n["kind"] == "pan"]
        if len(pans) == 1:
            words.append(f"{name}: places the mono sum")
        elif len(pans) == 2:
            words.append(f"{name}: moves each channel")
        else:
            words.append(f"{name}: {len(pans)} pan nodes")
    return "; ".join(words)


def bound_slots(ctx: Context, model: dict) -> set[int]:
    read = ctx.graph._read_bytes(model["nodes"])
    for node in model["nodes"]:
        if node["kind"] == "pan":
            if node["law"] not in ctx.laws_by_name:
                stop(f"a pan node names {node['law']}, which no p0 model reads")
            read |= ctx.graph._read_bytes(ctx.laws_by_name[node["law"]])
    return {slot_of(a, "the graph") for a in read}


# ---------------------------------------------------------------- building a set


def printed_ranges(coverage, slots: dict[int, str]) -> tuple[list[int], list[int]]:
    """Each slot's first and last printed byte; an unprinted slot reads 0 and 0."""
    lo, hi = [0] * SLOTS, [0] * SLOTS
    for slot, values in slots.items():
        lo[slot], hi[slot] = coverage.byte_domain(values)
    return lo, hi


def build_set(
    ctx: Context,
    types: list[dict],
    overlays: dict[int, list[dict]] | None,
    printed: dict[str, dict[int, str]],
) -> tuple[Pools, dict]:
    pools = Pools()
    facts = {}
    for t in types:
        model = t["model"]
        if overlays and t["number"] in overlays:
            model = apply_overlay(model, overlays[t["number"]])
            check_vocabulary(model, f"{type_key(t['number'])} with overlays", overlay=True)
        converter = Converter(ctx, pools, t["number"], model, t["laws"])
        ranges = converter.convert()
        topo = topology(ctx, t["number"], model)
        printed_lo, printed_hi = printed_ranges(
            ctx.archive.coverage, printed.get(type_key(t["number"]), {})
        )
        pools.types.append(
            {
                **ranges,
                "type": t["number"],
                "topology": topo,
                "sha256": t["sha256"],
                "gross": t["gross"],
                "printed_lo": printed_lo,
                "printed_hi": printed_hi,
            }
        )
        facts[t["number"]] = {
            "bound": bound_slots(ctx, model),
            "topology": topo,
            "pan": pan_arrangement(model) if t["number"] >> 8 == PARALLEL_MSB else "",
        }
    return pools, facts


def unbound(ctx: Context, printed: dict[str, dict[int, str]], facts: dict) -> dict[int, list[int]]:
    out = {}
    for key, slots in printed.items():
        number = type_number(key)
        if number not in facts:
            stop(f"{key} prints parameters and the archive has no p0 model for it")
        missing = sorted(set(slots) - facts[number]["bound"])
        if missing:
            out[number] = missing
    return out


def parse_scope(text: str) -> list[tuple[int, int, int]]:
    items = []
    for part in filter(None, (p.strip() for p in text.split(","))):
        m = re.fullmatch(r"([0-9A-Fa-f]{2})(?::([0-9A-Fa-f]{2})-([0-9A-Fa-f]{2}))?", part)
        if m is None:
            stop(f"--scope item {part!r} is not MSB or MSB:LSB-LSB in hex")
        msb = int(m.group(1), 16)
        lo = int(m.group(2), 16) if m.group(2) else 0x00
        hi = int(m.group(3), 16) if m.group(3) else 0xFF
        items.append((msb, lo, hi))
    return items


def in_scope(number: int, scope: list[tuple[int, int, int]]) -> bool:
    return any(number >> 8 == msb and lo <= (number & 0xFF) <= hi for msb, lo, hi in scope)


# ---------------------------------------------------------------- emitting a set


def overlay_digest(directory: Path) -> str:
    """SHA-256 over the overlay files, name-sorted, each as its name and bytes."""
    digest = hashlib.sha256()
    for path in sorted(directory.glob("*.json")):
        digest.update(path.name.encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


def check_overlays(directory: Path) -> int:
    """Archive-free: compare the overlay set with the digest recorded in the committed .inc."""
    recorded = None
    if OUT_DEFAULT.is_file():
        for line in OUT_DEFAULT.read_text().splitlines()[:10]:
            m = re.match(r"// overlay_digest: ([0-9a-f]{64})$", line)
            if m:
                recorded = m.group(1)
    current = overlay_digest(directory)
    if recorded == current:
        print(f"classic overlays match {OUT_DEFAULT.relative_to(REPO)}")
        return 0
    print(
        f"classic overlays changed since {OUT_DEFAULT.relative_to(REPO)} was generated "
        "(recorded "
        f"{recorded or 'none'}, now {current}): run `make gs-classic-models`",
        file=sys.stderr,
    )
    return 1


def emit_set(pools: Pools, revision: dict, what: str, overlays: str | None = None) -> str:
    out = [
        "// Generated by tools/gs/classic_models.py from the soundings archive; do not edit.",
        f"// GS classic realization models, {what}. See tools/gs/docs/classic.md.",
        f"// archive_revision: {revision['archive_revision']}",
        f"// archive_revision_source: {revision['archive_revision_source']}",
        f"// archive_inputs_dirty: {'true' if revision['archive_inputs_dirty'] else 'false'}",
        *([f"// overlay_digest: {overlays}"] if overlays else []),
        (
            f"// types {len(pools.types)}, nodes {len(pools.nodes)}, map specs {len(pools.specs)}, "
            f"curves {len(pools.curves)}, point runs {len(pools.point_lists)}"
        ),
        "",
        "namespace {",
        "",
    ]

    def array(ctype: str, suffix: str, rows: list[str]) -> None:
        if not rows:
            return
        out.append(f"const {ctype} GS_CLASSIC_SET_NAME({suffix})[] = {{")
        out.extend(f"    {row}," for row in rows)
        out.append("};")
        out.append("")

    def packed(ctype: str, suffix: str, items: list[str], per_line: int) -> None:
        if not items:
            return
        out.append(f"const {ctype} GS_CLASSIC_SET_NAME({suffix})[] = {{")
        for i in range(0, len(items), per_line):
            out.append("    " + ", ".join(items[i : i + per_line]) + ",")
        out.append("};")
        out.append("")

    array(
        "GsClassicType",
        "_types",
        [
            f"{{0x{t['type']:04X}, {t['node_begin']}, {t['node_end']}, {t['comp_begin']}, "
            f"{t['comp_end']}, {t['out_l']}, {t['out_r']}, {t['max_delay_samples']}u, "
            f'GsClassicTopology::{TOPOLOGY_NAME[t["topology"]]}, "{t["sha256"]}", '
            f"{float_lit(t['gross'])}, {{{', '.join(map(str, t['printed_lo']))}}}, "
            f"{{{', '.join(map(str, t['printed_hi']))}}}}}"
            for t in pools.types
        ],
    )
    array(
        "GsClassicNode",
        "_nodes",
        [
            f"{{GsClassicNodeKind::{KIND_NAME[k]}, {flags}, {aux}, {ib}, {ni}, {nv}, {vb}}}"
            for k, flags, aux, ib, ni, nv, vb in pools.nodes
        ],
    )
    packed(
        "GsClassicComponent",
        "_components",
        [f"{{{a}, {b}, {c}}}" for a, b, c in pools.components],
        6,
    )
    packed("GsClassicInput", "_inputs", [f"{{{s}}}" for s in pools.inputs], 12)
    value_kind = ("kConst", "kByte", "kControl")
    array(
        "GsClassicValue",
        "_values",
        [
            f"{{GsClassicValueKind::{value_kind[k]}, {slot}, {table}, {ref}, {double_lit(c)}}}"
            for k, slot, table, ref, c in pools.values
        ],
    )
    array(
        "GsClassicSection",
        "_sections",
        [
            f"{{static_cast<GsClassicStage>({s}), static_cast<GsClassicSectionForm>({f}), {o}, "
            f"static_cast<GsClassicSide>({side}), {c}, {g}, {q}, {n}, {m}, {r}}}"
            for s, f, o, side, c, g, q, n, m, r in pools.sections
        ],
    )
    array(
        "GsClassicReachedBy",
        "_reached_by",
        [f"{{{float_lit(db)}, {at}, {other}}}" for db, at, other in pools.reached_by],
    )
    packed("GsClassicPoints", "_point_lists", [f"{{{b}, {n}}}" for b, n in pools.point_lists], 8)
    packed(
        "GsClassicPoint",
        "_points",
        [f"{{{double_lit(x)}, {double_lit(y)}}}" for x, y in pools.points],
        3,
    )
    if pools.pan_laws:
        out.append("const GsClassicPanLaw GS_CLASSIC_SET_NAME(_pan_laws)[] = {")
        for left, right in pools.pan_laws:
            out.append("    {")
            for side in (left, right):
                vals = [float_lit(v) for v in side]
                out.append("        {")
                for i in range(0, len(vals), 8):
                    out.append("            " + ", ".join(vals[i : i + 8]) + ",")
                out.append("        },")
            out.append("    },")
        out.append("};")
        out.append("")
    if pools.curves:
        out.append("const GsClassicCurve GS_CLASSIC_SET_NAME(_curves)[] = {")
        for values, lo, hi in pools.curves:
            vals = [float_lit(v) for v in values]
            out.append("    {{")
            for i in range(0, len(vals), 8):
                out.append("         " + ", ".join(vals[i : i + 8]) + ",")
            out.append(f"     }}, {float_lit(lo)}, {float_lit(hi)}}},")
        out.append("};")
        out.append("")
    array(
        "GsClassicMapSpec",
        "_map_specs",
        [
            f"{{GsClassicMapKind::{MAP_KIND_NAME[k]}, {log}, {kb}, {vb}, {n}, {pe}, {oor}, {lo}, "
            f"{hi}, {po}}}"
            for k, log, kb, vb, n, pe, oor, lo, hi, po in pools.specs
        ],
    )
    packed("uint8_t", "_map_keys", [str(k) for k in pools.map_keys], 16)
    packed("double", "_map_values", [double_lit(v) for v in pools.map_values], 4)
    out.append("}  // namespace")
    out.append("")

    def ref(suffix: str, present) -> str:
        return f"GS_CLASSIC_SET_NAME({suffix})" if present else "nullptr"

    fields = [
        ref("_types", pools.types),
        f"{len(pools.types)}",
        ref("_nodes", pools.nodes),
        ref("_components", pools.components),
        ref("_inputs", pools.inputs),
        ref("_values", pools.values),
        ref("_sections", pools.sections),
        ref("_reached_by", pools.reached_by),
        ref("_point_lists", pools.point_lists),
        ref("_points", pools.points),
        ref("_pan_laws", pools.pan_laws),
        ref("_curves", pools.curves),
        ref("_map_specs", pools.specs),
        f"{len(pools.specs)}",
        ref("_map_keys", pools.map_keys),
        ref("_map_values", pools.map_values),
        "nullptr",
    ]
    out.append("GsClassicModelSet GS_CLASSIC_SET_PREFIX() {")
    out.append("  return {")
    out.extend(f"      {f}," if i < len(fields) - 1 else f"      {f}" for i, f in enumerate(fields))
    out.append("  };")
    out.append("}")
    return "\n".join(out) + "\n"


def check_pool_limits(pools: Pools, what: str) -> None:
    limits = {
        "nodes": len(pools.nodes),
        "inputs": len(pools.inputs),
        "values": len(pools.values),
        "map keys": len(pools.map_keys),
        "map values": len(pools.map_values),
        "points": len(pools.points),
        "components": len(pools.components),
    }
    for name, size in limits.items():
        if size >= NONE16:
            stop(f"the {what} {name} pool holds {size} entries, past its 16-bit index")


# ---------------------------------------------------------------- map expansion (test only)


def emit_expansion(ctx: Context, sets: dict[str, Pools], revision: dict) -> str:
    rows: dict[tuple, int] = {}
    table: list[list[float]] = []
    index: dict[str, list[int]] = {}
    for name, pools in sets.items():
        index[name] = []
        for spec, source in zip(pools.specs, pools.spec_maps, strict=True):
            lo, hi, power_on = spec[7], spec[8], spec[9]
            expanded = [f32(v) for v in ctx.expand(source, lo, hi, power_on)]
            key = tuple(expanded)
            if key not in rows:
                rows[key] = len(table)
                table.append(expanded)
            index[name].append(rows[key])
    out = [
        "// Generated by tools/gs/classic_models.py from the soundings archive; do not edit.",
        "// Every map spec of both configurations expanded over bytes 0..127 by soundings'",
        "// _from_map (and, where a states map names nothing, the nearest-state rule), rounded",
        "// to float. Test-only: the C++ expander must reproduce every entry.",
        f"// archive_revision: {revision['archive_revision']}",
        f"// archive_revision_source: {revision['archive_revision_source']}",
        f"// archive_inputs_dirty: {'true' if revision['archive_inputs_dirty'] else 'false'}",
        "",
        "namespace {",
        "",
        "constexpr float kGsClassicMapExpansion[][128] = {",
    ]
    for row in table:
        vals = [float_lit(v) for v in row]
        out.append("    {")
        for i in range(0, len(vals), 8):
            out.append("        " + ", ".join(vals[i : i + 8]) + ",")
        out.append("    },")
    out.append("};")
    out.append("")
    for name, rows_of in index.items():
        cname = "Default" if name == "default" else "Raw"
        out.append(f"/// Row of kGsClassicMapExpansion for each map spec of the {name} set.")
        out.append(f"constexpr uint16_t kGsClassicMapExpansion{cname}[] = {{")
        for i in range(0, len(rows_of), 16):
            out.append("    " + ", ".join(str(r) for r in rows_of[i : i + 16]) + ",")
        out.append("};")
        out.append("")
    out.append("}  // namespace")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- sizes


class _Spec(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_uint8),
        ("log", ctypes.c_uint8),
        ("key_begin", ctypes.c_uint16),
        ("value_begin", ctypes.c_uint16),
        ("n", ctypes.c_uint16),
        ("per_entry", ctypes.c_uint16),
        ("out_of_range", ctypes.c_uint16),
        ("lo", ctypes.c_uint8),
        ("hi", ctypes.c_uint8),
        ("power_on", ctypes.c_uint8),
    ]


class _Curve(ctypes.Structure):
    _fields_ = [("v", ctypes.c_float * CURVE_SIZE), ("lo", ctypes.c_float), ("hi", ctypes.c_float)]


class _Node(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("aux", ctypes.c_uint16),
        ("input_begin", ctypes.c_uint16),
        ("n_inputs", ctypes.c_uint8),
        ("value_count", ctypes.c_uint8),
        ("value_begin", ctypes.c_uint16),
    ]


class _Value(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_uint8),
        ("slot", ctypes.c_uint8),
        ("table", ctypes.c_uint16),
        ("control_ref", ctypes.c_uint16),
        ("constant", ctypes.c_double),
    ]


class _Section(ctypes.Structure):
    _fields_ = [
        (n, ctypes.c_uint8) for n in ("st", "fo", "or", "si", "co", "ga", "q", "cn", "mx")
    ] + [("reached_by", ctypes.c_uint16)]


class _Reached(ctypes.Structure):
    _fields_ = [("full_db", ctypes.c_float), ("at", ctypes.c_uint16), ("other", ctypes.c_uint16)]


class _Type(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint16) for n in ("t", "nb", "ne", "cb", "ce", "ol", "or")] + [
        ("max_delay", ctypes.c_uint32),
        ("topology", ctypes.c_uint8),
        ("sha", ctypes.c_char * 65),
        ("gross", ctypes.c_float),
        ("printed_lo", ctypes.c_uint8 * SLOTS),
        ("printed_hi", ctypes.c_uint8 * SLOTS),
    ]


class _Component(ctypes.Structure):
    _fields_ = [("b", ctypes.c_uint16), ("e", ctypes.c_uint16), ("looped", ctypes.c_uint8)]


class _PanLaw(ctypes.Structure):
    _fields_ = [("l", ctypes.c_float * LUT_SIZE), ("r", ctypes.c_float * LUT_SIZE)]


def _blob(structs: list) -> bytes:
    return b"".join(bytes(s) for s in structs)


def shipping_sizes(pools: Pools) -> dict[str, tuple[int, int]]:
    specs = _blob([_Spec(*s) for s in pools.specs])
    specs += bytes(pools.map_keys) + _blob([ctypes.c_double(v) for v in pools.map_values])
    curves = _blob(
        [_Curve((ctypes.c_float * CURVE_SIZE)(*v), lo, hi) for v, lo, hi in pools.curves]
    )
    nodes = _blob([_Node(KIND_INDEX[k], *rest) for k, *rest in pools.nodes])
    nodes += _blob([_Value(*v) for v in pools.values])
    nodes += _blob([ctypes.c_uint16(s) for s in pools.inputs])
    nodes += _blob([_Section(*s) for s in pools.sections])
    nodes += _blob([_Reached(*r) for r in pools.reached_by])
    nodes += _blob([_Component(*c) for c in pools.components])
    nodes += _blob([ctypes.c_uint16(b) for pair in pools.point_lists for b in pair])
    nodes += _blob([ctypes.c_double(v) for pair in pools.points for v in pair])
    nodes += _blob(
        [
            _PanLaw((ctypes.c_float * LUT_SIZE)(*l), (ctypes.c_float * LUT_SIZE)(*r))
            for l, r in pools.pan_laws
        ]
    )
    nodes += _blob(
        [
            _Type(
                t["type"],
                t["node_begin"],
                t["node_end"],
                t["comp_begin"],
                t["comp_end"],
                t["out_l"],
                t["out_r"],
                t["max_delay_samples"],
                t["topology"],
                t["sha256"].encode(),
                t["gross"],
                (ctypes.c_uint8 * SLOTS)(*t["printed_lo"]),
                (ctypes.c_uint8 * SLOTS)(*t["printed_hi"]),
            )
            for t in pools.types
        ]
    )
    blobs = {"map specs": specs, "control curves": curves, "node tables": nodes}
    blobs["total"] = specs + curves + nodes
    return {name: (len(b), len(gzip.compress(b, 9, mtime=0))) for name, b in blobs.items()}


# ---------------------------------------------------------------- reference digests


def stimulus():
    import numpy as np

    n = FS
    channels = []
    for seed in STIMULUS_SEEDS:
        s = seed
        noise = np.empty(n)
        for i in range(n):
            s ^= (s << 13) & 0xFFFFFFFF
            s ^= s >> 17
            s ^= (s << 5) & 0xFFFFFFFF
            word = (s >> 16) & 0xFFFF
            noise[i] = (word - 0x10000 if word >= 0x8000 else word) / 32768.0
        channels.append(noise)
    i = np.arange(n, dtype=np.int64)
    saw = (((i * STIMULUS_SAW_HZ * 65536) // FS) % 65536 - 32768) / 32768.0
    return [np.concatenate([c, saw]) * STIMULUS_SCALE for c in channels]


def band_edges() -> list[tuple[float, float]]:
    return [
        (1000.0 * 2.0 ** ((n - 0.5) / 3.0), 1000.0 * 2.0 ** ((n + 0.5) / 3.0))
        for n in BAND_EXPONENTS
    ]


def digest(x) -> list[list[float]]:
    """Per 0.25 s window, the mean-square power in each third-octave band, in dB."""
    import numpy as np

    freqs = np.arange(DIGEST_WINDOW // 2 + 1) * FS / DIGEST_WINDOW
    weight = np.full(freqs.size, 2.0)
    weight[0] = weight[-1] = 1.0
    out = []
    for w in range(len(x) // DIGEST_WINDOW):
        seg = x[w * DIGEST_WINDOW : (w + 1) * DIGEST_WINDOW]
        power = np.abs(np.fft.rfft(seg)) ** 2 * weight / DIGEST_WINDOW**2
        row = []
        for lo, hi in band_edges():
            total = float(power[(freqs >= lo) & (freqs < hi)].sum())
            row.append(10.0 * math.log10(total) if total > 1e-30 else DIGEST_FLOOR_DB)
        out.append(row)
    return out


_WORKER: dict = {}


def _worker_init(models: dict) -> None:
    _WORKER["models"] = models
    _WORKER["stimulus"] = stimulus()
    from soundings.render import graph

    _WORKER["graph"] = graph


def _render(task: tuple) -> tuple:
    number, label, bytes_now = task
    graph = _WORKER["graph"]
    left, right = _WORKER["stimulus"]
    now = {address_of(s): b for s, b in enumerate(bytes_now)}
    try:
        drawn = graph.run(
            _WORKER["models"][number], {"in_l": left, "in_r": right}, now, lfo_phase=0.0
        )
    except graph.Unrenderable as refused:
        return number, label, None, str(refused).replace("\t", " ").replace("\n", " ")
    return number, label, (digest(drawn["out_l"]), digest(drawn["out_r"])), None


def render_reference(
    ctx: Context, types: list[dict], printed, facts, revision: dict, log
) -> tuple[str, dict]:
    tasks, rows_order, same = [], [], {}
    for t in types:
        number = t["number"]
        on = ctx.power_on[number]
        rows_order.append((number, "power-on", "-", "-"))
        tasks.append((number, ("power-on", "-", "-"), tuple(on)))
        for slot in sorted(printed[type_key(number)]):
            lo, hi = ctx.accept[(number, slot)]
            for byte in (lo, hi):
                label = ("slot", str(slot), str(byte))
                rows_order.append((number, *label))
                if slot not in facts[number]["bound"]:
                    same[(number, label)] = "unbound"
                elif on[slot] == byte:
                    same[(number, label)] = "same-as-power-on"
                else:
                    now = list(on)
                    now[slot] = byte
                    tasks.append((number, label, tuple(now)))
    models = {t["number"]: t["loaded"] for t in types}
    results = {}
    with ProcessPoolExecutor(
        max_workers=RENDER_WORKERS, initializer=_worker_init, initargs=(models,)
    ) as pool:
        for done, (number, label, got, refused) in enumerate(
            pool.map(_render, tasks, chunksize=4), 1
        ):
            results[(number, label)] = (got, refused)
            if done % 50 == 0 or done == len(tasks):
                print(f"rendered {done}/{len(tasks)}", file=log, flush=True)

    edges = band_edges()
    lines = [
        (
            "# archive_revision={archive_revision}\tarchive_revision_source="
            "{archive_revision_source}\tarchive_inputs_dirty={dirty}"
        ).format(dirty="true" if revision["archive_inputs_dirty"] else "false", **revision),
        (
            "# Reference digests of the raw classic models (no overlays), drawn by soundings' "
            "render.graph.run at 32 kHz with lfo_phase 0. Generated by tools/gs/classic_models.py."
        ),
        (
            "# Stimulus: per channel, xorshift32 (L seed 0x12345678, R seed 0x9E3779B9; "
            "s^=s<<13, s^=s>>17, s^=s<<5, then use) top 16 bits as int16 / 32768 for 32000 "
            "samples, then ((i*220*65536/32000) mod 65536 - 32768) / 32768 for i in 0..31999 "
            "(integer division); all times 0.25."
        ),
        (
            "# Digest: 8 non-overlapping 8000-sample windows from sample 0; rectangular rfft; "
            "power |X|^2 * w / 8000^2 (w = 2, 1 at DC and Nyquist) summed over bins f=k*4 Hz "
            "with lo <= f < hi; 10*log10, -300 where the sum is under 1e-30."
        ),
        "# Bands: 25 third-octaves, centre 1000*2^(n/3) for n=-13..11, edges centre*2^(+-1/6): "
        + " ".join(f"{lo:.2f}-{hi:.2f}" for lo, hi in edges),
        (
            "# States: power-on (the protocol layer's power-on bytes), then each printed slot at "
            "its lowest and highest accepted byte. status: rendered; unrenderable (the renderer "
            "refuses the setting); same-as-power-on (the byte is the power-on byte); unbound "
            "(no raw-model node reads the slot, so the output is the power-on one)."
        ),
        "type\tslot\tbyte\tstatus\tchannel\twindow\t"
        + "\t".join(f"b{i:02d}" for i in range(len(edges))),
    ]
    unrenderable: dict[int, int] = {}
    for number, state, slot, byte in rows_order:
        key = (number, (state, slot, byte))
        head = f"{type_key(number)}\t{slot}\t{byte}"
        if key in same:
            lines.append(f"{head}\t{same[key]}\t-\t-")
            continue
        got, refused = results[key]
        if got is None:
            unrenderable[number] = unrenderable.get(number, 0) + 1
            lines.append(f"{head}\tunrenderable\t-\t-\t{refused}")
            continue
        for channel, windows in zip("LR", got, strict=True):
            for w, row in enumerate(windows):
                lines.append(
                    f"{head}\trendered\t{channel}\t{w}\t" + "\t".join(f"{v:.2f}" for v in row)
                )
    return "\n".join(lines) + "\n", unrenderable


def unrenderable_from_tsv(path: Path) -> dict[int, int]:
    counts: dict[int, int] = {}
    if not path.is_file():
        return counts
    for line in path.read_text().splitlines():
        if line.startswith(("#", "type\t")):
            continue
        cols = line.split("\t")
        if len(cols) > 3 and cols[3] == "unrenderable":
            number = type_number(cols[0])
            counts[number] = counts.get(number, 0) + 1
    return counts


# ---------------------------------------------------------------- classic.md


def emit_doc_section(
    types: list[dict],
    pools: Pools,
    facts: dict,
    printed,
    missing: dict,
    unrenderable: dict,
    revision: dict,
    d5: dict,
    raw_specs: int,
) -> str:
    out = [DOC_BEGIN, "", "### Archive revision", ""]
    out.append(f"- `archive_revision`: `{revision['archive_revision']}`")
    out.append(f"- `archive_revision_source`: {revision['archive_revision_source']}")
    out.append(
        f"- `archive_inputs_dirty`: {'true' if revision['archive_inputs_dirty'] else 'false'}"
    )
    out += ["", "### Realization differences", ""]
    out.append(
        "Per type: the archive's leading candidate, the Parallel-2 arrangement read off its "
        "graph (0 not Parallel-2, 1 side-by-side, 2 in-series) and how each half's pan acts, "
        "printed slots, printed slots no node reads, states the renderer refuses in the "
        "reference, and the candidate's stage-8 gross residual."
    )
    out += [
        "",
        "| type | candidate | topology | pan | printed | unbound | unrenderable | gross |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for t in types:
        n = t["number"]
        f = facts[n]
        topo = f"{f['topology']} {TOPOLOGY_WORD[f['topology']]}" if f["topology"] else "0"
        out.append(
            f"| `{type_key(n)}` | {t['candidate']} | {topo} | {f['pan'] or '-'} | "
            f"{len(printed[type_key(n)])} | {len(missing.get(n, []))} | {unrenderable.get(n, 0)} | "
            f"{t['gross']:.3f} |"
        )
    total_missing = sum(len(v) for v in missing.values())
    out += ["", f"Unbound printed slots: {total_missing} of 770 in {len(missing)} types."]
    out += ["", "### Document values", ""]
    out.append(
        "`source: document` values passed, by form: "
        + ", ".join(f"{name} {count}" for name, count in sorted(d5.items()))
        + ". Formula and constant columns are re-derived from the formula, not copied."
    )
    sizes = shipping_sizes(pools)
    out += ["", "### Shipping size", ""]
    out.append(
        f"The default configuration ships {len(pools.specs)} de-duplicated map specs "
        f"({raw_specs} in the raw configuration), {len(pools.curves)} control curves, "
        f"{len(pools.nodes)} nodes and {len(pools.point_lists)} point runs. Packed as the "
        "structs of `model_format.h`, gzip -9:"
    )
    out += ["", "| pool | raw bytes | gzip bytes |", "|---|---|---|"]
    for name, (raw, packed) in sizes.items():
        out.append(f"| {name} | {raw} | {packed} |")
    out.append("")
    out.append(
        f"Expanding the specs at registry construction puts {len(pools.specs) * LUT_SIZE * 4} "
        "bytes of LUTs on the heap per configuration."
    )
    out += ["", DOC_END]
    return "\n".join(out)


def splice_doc(current: str, section: str) -> str:
    if DOC_BEGIN not in current or DOC_END not in current:
        stop(f"{OUT_DOC} has no generated-section markers")
    head, rest = current.split(DOC_BEGIN, 1)
    _, tail = rest.split(DOC_END, 1)
    return head + section + tail


# ---------------------------------------------------------------- main


def committed_revision(paths: list[Path]) -> str | None:
    for path in paths:
        if path.is_file():
            for line in path.read_text().splitlines()[:8]:
                m = re.search(r"archive_revision[:=] ?([^\t\s]+)", line)
                if m:
                    return m.group(1)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--archive", help="soundings checkout (default $GS_EFX_ARCHIVE)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--raw", action="store_true", help="overlay-less set and reference TSV only")
    mode.add_argument("--scope", help="overlay set; unbound check only inside MSB[:LSB-LSB],...")
    ap.add_argument("--check", action="store_true", help="regenerate to scratch and diff")
    ap.add_argument(
        "--check-overlays",
        action="store_true",
        help="no archive: compare the overlay set with the digest in the committed .inc",
    )
    ap.add_argument("--overlays", default=str(DEFAULT_OVERLAYS), help="overlay directory")
    args = ap.parse_args()

    root = Path(args.archive or os.environ["GS_EFX_ARCHIVE"]).resolve()
    archive = Archive(root)
    ctx = Context(archive)
    types, d5 = read_models(archive)
    ctx.laws_by_name = {name: law for t in types for name, law in t["laws"].items()}
    _, printed = archive.coverage.load_printed(root)
    archive.read |= set((root / "data" / "units").glob("*/efx-params/*.json"))
    total = sum(len(v) for v in printed.values())
    if total != archive.coverage.EXPECTED_PRINTED:
        stop(
            f"the archive prints {total} (type, slot) parameters, not {archive.coverage.EXPECTED_PRINTED}"
        )

    overlays, file_covers = load_overlays(Path(args.overlays))
    check_covers(archive, types, overlays, file_covers)
    resolve_overlay_laws(
        archive.coverage, overlays, printed, ctx.power_on, {t["number"]: t["model"] for t in types}
    )
    raw_pools, raw_facts = build_set(ctx, types, None, printed)
    default_pools, default_facts = build_set(ctx, types, overlays, printed)
    check_pool_limits(raw_pools, "raw")
    check_pool_limits(default_pools, "default")

    missing = unbound(ctx, printed, default_facts)
    if args.raw:
        pass
    elif args.scope is not None:
        scope = parse_scope(args.scope)
        inside = {n: s for n, s in missing.items() if in_scope(n, scope)}
        outside: dict[str, int] = {}
        for n, s in missing.items():
            if n not in inside:
                outside[f"{n >> 8:02X}"] = outside.get(f"{n >> 8:02X}", 0) + len(s)
        for msb, count in sorted(outside.items()):
            print(f"out of scope: MSB {msb}: {count} unbound printed slots")
        if inside:
            listed = "; ".join(f"{type_key(n)} slots {s}" for n, s in sorted(inside.items()))
            stop(f"printed slots no node reads, inside --scope: {listed}")
    elif missing:
        listed = "; ".join(f"{type_key(n)} slots {s}" for n, s in sorted(missing.items()))
        stop(
            f"{sum(len(s) for s in missing.values())} printed slots in {len(missing)} types are "
            f"read by no node (write overlays, or run --raw / --scope): {listed}"
        )

    revision = archive_revision(root, sorted(archive.read))
    if revision["archive_inputs_dirty"]:
        print(
            "warning: the archive's working copy is dirty over the files read, so "
            f"archive_revision {revision['archive_revision']} does not identify them",
            file=sys.stderr,
        )

    outputs: dict[Path, str] = {}
    render = args.scope is None
    if render or args.raw:
        outputs[OUT_RAW] = emit_set(raw_pools, revision, "without overlays (test-only)")
    if not args.raw:
        outputs[OUT_DEFAULT] = emit_set(
            default_pools, revision, "with overlays", overlay_digest(Path(args.overlays))
        )
    outputs[OUT_EXPANSION] = emit_expansion(
        ctx, {"default": default_pools, "raw": raw_pools}, revision
    )
    if render:
        tsv, unrenderable = render_reference(ctx, types, printed, raw_facts, revision, sys.stdout)
        outputs[OUT_TSV] = tsv
    else:
        unrenderable = unrenderable_from_tsv(OUT_TSV)
    section = emit_doc_section(
        types,
        default_pools,
        default_facts,
        printed,
        missing,
        unrenderable,
        revision,
        d5,
        len(raw_pools.specs),
    )
    outputs[OUT_DOC] = splice_doc(OUT_DOC.read_text(), section)

    print(
        f"types {len(types)}; map specs default {len(default_pools.specs)}, raw {len(raw_pools.specs)}"
    )
    for n in sorted(default_facts):
        if n >> 8 == PARALLEL_MSB:
            print(f"topology {type_key(n)} {default_facts[n]['topology']}")
    print(f"unbound printed slots {sum(len(s) for s in missing.values())}")

    if not args.check:
        for path, text in outputs.items():
            path.write_text(text)
            print(f"wrote {path.relative_to(REPO)}")
        return 0

    committed = committed_revision([OUT_RAW, OUT_DEFAULT, OUT_TSV])
    if committed is not None and committed != revision["archive_revision"]:
        print(
            f"archive_revision differs: committed {committed}, regenerated from the archive "
            f"{revision['archive_revision']}"
        )
    differs = False
    with tempfile.TemporaryDirectory() as scratch:
        for path, text in outputs.items():
            fresh = Path(scratch) / path.name
            fresh.write_text(text)
            old = path.read_text().splitlines(keepends=True) if path.is_file() else []
            diff = list(
                difflib.unified_diff(
                    old, text.splitlines(keepends=True), str(path.relative_to(REPO)), str(fresh)
                )
            )
            if diff:
                differs = True
                sys.stdout.writelines(diff)
    return 1 if differs else 0


if __name__ == "__main__":
    if "--check-overlays" in sys.argv[1:]:
        argv = sys.argv[1:]
        given = [a.split("=", 1)[1] for a in argv if a.startswith("--overlays=")]
        if "--overlays" in argv and argv.index("--overlays") + 1 < len(argv):
            given.append(argv[argv.index("--overlays") + 1])
        sys.exit(check_overlays(Path(given[-1]) if given else DEFAULT_OVERLAYS))
    reexec_under_archive_venv()
    # tools/gs/coverage.py would shadow the `coverage` package numba imports.
    sys.path[:] = [p for p in sys.path if Path(p or ".").resolve() != TOOLS]
    sys.exit(main())
