"""Identities for audition renders and the generation directories they are written to.

Four identities, each a `boundary.canonical_digest`: `request_id` (what was
asked for, the actual SMF bytes included), `source_id` (the resolved reference
configuration and state, with its rig and room classes), `build_id` (the loaded
library and its bank registry digest; see `render_model.render_evidence`) and
`asset_id` (the bytes of the written WAV). A set's generation is derived from
its asset ids and comparisons, never from a timestamp.

Local paths and product names enter a `source_id` only inside a digest.

A generation is written whole into a staging directory and renamed into place
under its own digest, so an asset an older manifest or index points at is never
overwritten; the manifest or index naming it is published after it, atomically.

The reference archive has one reader, `archived_records`, for every tool that
reads it. A v2 entry carries its request, source and asset identities; a v1
entry carries none and is returned flagged historical, for measurement only.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import tempfile
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from boundary import Reference, canonical_digest
from wavio import read_wav

#: Bumped when the reference renderers change what one request produces.
REFERENCE_METHOD_VERSION = 1
#: Length of the digest prefix a generation directory is named by.
GENERATION_NAME_LENGTH = 16

#: The reference archive's v2 index; `index.json` beside it is v1.
ARCHIVE_INDEX = "index-v2.json"
ARCHIVE_INDEX_V1 = "index.json"
#: Under the archive, one directory per (capture, take, generation).
ARCHIVE_V2_DIR = "v2"
#: How a stored take's level was set: one `shared_gain` over its fresh timbres, 24-bit PCM.
ARCHIVE_GAIN_VERSION = 1
#: Which archive a stored reference came from.
ARCHIVE_VERIFIED = "v2"
ARCHIVE_HISTORICAL = "v1"
#: A reference version's evidence status: its source was re-resolved here, or not.
REFERENCE_VERIFIED = "verified"
REFERENCE_UNVERIFIED = "unverified"
SOURCE_UNRESOLVED = "source identity could not be re-resolved here"

_FILE_DIGESTS: dict[tuple[str, int, int], str] = {}


def file_digest(path: Path) -> str:
    """SHA-256 of a file's bytes, cached per (path, size, mtime) for this process."""
    resolved = Path(path).resolve()
    stat = resolved.stat()
    key = (str(resolved), stat.st_size, stat.st_mtime_ns)
    if key not in _FILE_DIGESTS:
        h = hashlib.sha256()
        with open(resolved, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        _FILE_DIGESTS[key] = h.hexdigest()
    return _FILE_DIGESTS[key]


def reference_request(method: str, *, seconds: float, sample_rate: int, **stimulus) -> dict:
    """The request one reference render answers: method, stimulus, rate and length."""
    return {
        "method": method,
        "method_version": REFERENCE_METHOD_VERSION,
        "seconds": float(seconds),
        "sample_rate": int(sample_rate),
        "frames": round(float(seconds) * int(sample_rate)),
        **stimulus,
    }


def reference_source_id(kind: str, state: Mapping, raw_capture: Mapping) -> str:
    """A reference source's identity: its resolved state digest and its boundary classes."""
    ref = Reference.from_capture(raw_capture)
    return canonical_digest(
        {
            "kind": kind,
            "state": canonical_digest(state),
            "rig": ref.rig,
            "room": ref.room,
            "rig_evidence": ref.rig_evidence,
        }
    )


def au_source_id(source, raw_capture: Mapping) -> str:
    """The `source_id` of an AudioUnit reference, from `AuSource.identity()`.

    The identity digests the preset and the saved state by content, so an
    edited state or an amp switched in `params` is a different source. Raises
    what resolving the preset raises when it is not on this machine.
    """
    # `identity()` reads an absent state file as an empty digest, which is a
    # different source rather than an unknown one.
    if source.state and not Path(source.state).expanduser().is_file():
        raise FileNotFoundError(f"saved state {source.state} is not on this machine")
    state = {
        "source": source.identity(),
        "key_offset": source.key_offset,
        "key_map": source.key_map,
        "keyswitch": source.keyswitch,
        "keyswitch_lead_ms": source.keyswitch_lead_ms,
    }
    return reference_source_id("au", state, raw_capture)


def module_source_id(
    raw_capture: Mapping, timbre: Mapping, font: Path, preset: str, rows: Mapping[int, Mapping]
) -> str:
    """The `source_id` of a module reference: the font by content, the preset, the level rows."""
    state = {
        "font_sha256": file_digest(font),
        "preset": preset,
        "timbre": dict(timbre),
        "gate_ms": raw_capture.get("gate_ms"),
        "rows": {
            str(note): [int(row["velocity"]), float(row["peak_dbfs"])]
            for note, row in sorted(rows.items())
        },
    }
    return reference_source_id("module", state, raw_capture)


def set_generation(items: list[dict], comparisons: list[dict]) -> str:
    """A page's generation: every asset it plays, per take and version, and its comparisons."""
    assets = {
        item["id"]: {key: ev["asset_id"] for key, ev in (item.get("evidence") or {}).items()}
        for item in items
    }
    return canonical_digest({"assets": assets, "comparisons": comparisons})


def staging_dir(parent: Path) -> Path:
    """A fresh directory beside where a generation will be published."""
    parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=".staging-", dir=parent))


def publish_generation(staging: Path, parent: Path, generation: str) -> Path:
    """Move a finished staging directory to `parent/<generation prefix>`.

    A directory already there holds the same generation, so it is kept and the
    staging copy discarded rather than written over it.
    """
    target = parent / generation[:GENERATION_NAME_LENGTH]
    if target.exists():
        shutil.rmtree(staging)
    else:
        staging.rename(target)
    return target


def write_json_atomic(path: Path, obj: object) -> None:
    """Write JSON beside `path` and rename it into place, so no reader sees half a file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(obj, indent=2) + "\n")
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


@dataclass(frozen=True)
class ArchivedReference:
    """One stored reference render and where it came from; `load` reads and checks it."""

    timbre: str
    path: Path
    gain: float
    archive: str
    generation: str | None = None
    request_id: str | None = None
    source_id: str | None = None
    asset_id: str | None = None
    sample_rate: int | None = None
    frames: int | None = None
    channels: int | None = None

    @property
    def historical(self) -> bool:
        return self.archive == ARCHIVE_HISTORICAL

    def describe(self) -> str:
        if self.historical:
            return "archive v1, historical: no request or source identity"
        return f"archive v2, generation {self.generation[:GENERATION_NAME_LENGTH]}"

    def load(self) -> tuple[np.ndarray, int] | None:
        """The audio at the level it was rendered, or None when the file no longer matches.

        A v2 file must still have the digest, rate and shape its index recorded;
        a v1 file has nothing recorded to check against.
        """
        if not self.path.is_file():
            return None
        if not self.historical and file_digest(self.path) != self.asset_id:
            return None
        audio, sr = read_wav(self.path)
        if not self.historical and (sr, *audio.shape) != (
            self.sample_rate,
            self.frames,
            self.channels,
        ):
            return None
        return np.asarray(audio, dtype=np.float64) / self.gain, sr


def read_archive_index(archive: Path) -> dict:
    """The archive's v2 index, empty when there is none."""
    path = archive / ARCHIVE_INDEX
    if not path.exists():
        return {"schema_version": 2, "takes": {}}
    return json.loads(path.read_text())


def _read_v1_index(archive: Path) -> dict:
    path = archive / ARCHIVE_INDEX_V1
    return json.loads(path.read_text()) if path.exists() else {}


def archived_records(archive: Path, capture_id: str, take_id: str) -> list[ArchivedReference]:
    """Every stored reference of one take: v2 newest first, then v1 flagged historical.

    Only v2 entries whose gain was set the current way are returned. Nothing is
    read from disk beyond the indexes until `load`.
    """
    out = []
    entries = read_archive_index(archive).get("takes", {}).get(capture_id, {}).get(take_id, [])
    for entry in reversed(entries):
        if entry.get("gain_version") != ARCHIVE_GAIN_VERSION:
            continue
        for timbre, record in entry.get("timbres", {}).items():
            out.append(
                ArchivedReference(
                    timbre=timbre,
                    path=archive / record["path"],
                    gain=float(entry["gain"]),
                    archive=ARCHIVE_VERIFIED,
                    generation=entry["generation"],
                    request_id=record["request_id"],
                    source_id=record["source_id"],
                    asset_id=record["asset_id"],
                    sample_rate=int(record["request"]["sample_rate"]),
                    frames=int(record["frames"]),
                    channels=int(record["channels"]),
                )
            )
    meta = _read_v1_index(archive).get(capture_id, {}).get(take_id)
    gain = 10.0 ** (float(meta["gain_db"]) / 20.0) if meta else 0.0
    if gain > 0.0:
        for path in sorted((archive / capture_id / take_id).glob("*.wav")):
            out.append(ArchivedReference(path.stem, path, gain, ARCHIVE_HISTORICAL))
    return out


def archived_take_ids(archive: Path, capture_id: str) -> dict[str, str]:
    """Which takes the archive holds for a capture, each with the archive it is in (v2 wins)."""
    held = {take: ARCHIVE_HISTORICAL for take in _read_v1_index(archive).get(capture_id, {})}
    v2 = read_archive_index(archive).get("takes", {}).get(capture_id, {})
    held.update(
        {
            take: ARCHIVE_VERIFIED
            for take, entries in v2.items()
            if any(e.get("gain_version") == ARCHIVE_GAIN_VERSION for e in entries)
        }
    )
    return held
