#!/usr/bin/env python3
"""Report where the soundings archive has moved past what the GS EFX tree recorded.

A designed row, an enables row and a classic overlay entry each record the condition under
which a measurement would retire them (``replaced_when``); a classic model records the hash of
the archive model it was generated from and its stage-8 gross residual. This tool reads the
archive as it is now and prints one line per recorded key that no longer holds, as
``<kind>: <where>: <what changed>``, sorted by kind. Stdlib only; needs no archive venv.

Exit status: 0 nothing drifted, 3 at least one finding, 2 the archive or a needed file is
missing.

Usage:
    soundings_drift.py [--archive ROOT] [--bindings DIR] [--overlays DIR] [--tables FILE]
                       [--inc FILE]

``table_reaches`` reads only the committed ``--tables`` file.
``--archive`` defaults to ``$GS_EFX_ARCHIVE``.
"""

import argparse
import ctypes
import hashlib
import json
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
UNIT = "roland-sc8850-01"
TYPE_ALIASES = {"03 00": "02 0C"}
MEASURED_SOURCES = ("law", "measured")
# One row of the generated type table: number, ..., model_sha256, gross_residual.
# A record path names a pair as `<dir>/MM-LL-AA-...` (type, address low byte) or, for a record
# taken across types, `<dir>/40-03-AA-...` (address only, the claim's `about.types` gives types).
RECORD_TYPED = re.compile(r"/([0-9A-F]{2})-([0-9A-F]{2})-([0-9A-F]{2})(?:-|\.json)")
RECORD_ADDRESS = re.compile(r"/40-03-([0-9A-F]{2})-")
TYPE_ROW = re.compile(
    r'\{0x([0-9A-Fa-f]{4}),[^"{}]*"([0-9a-f]{64})",\s*([-+0-9.eE]+)f?,'
)


class Stop(Exception):
    """A file the comparison needs is not there."""


def canonical(type_key: str) -> str:
    return TYPE_ALIASES.get(type_key, type_key)


def f32(value: float) -> float:
    return ctypes.c_float(value).value


def strings(node):
    """Every string in a JSON value."""
    if isinstance(node, str):
        yield node
    elif isinstance(node, dict):
        for value in node.values():
            yield from strings(value)
    elif isinstance(node, list):
        for value in node:
            yield from strings(value)


def record_pairs(record: str, types: set[str]) -> set[tuple[str, str]]:
    """The (type, address) pairs a measurement record's path names."""
    if not record.startswith("data/units/"):
        return set()
    pairs = set()
    for msb, lsb, low in RECORD_TYPED.findall("/" + record):
        pairs.add((canonical(f"{msb} {lsb}"), f"40 03 {low}"))
    for low in RECORD_ADDRESS.findall("/" + record):
        pairs |= {(t, f"40 03 {low}") for t in types}
    return pairs


class Archive:
    """The parts of the archive the drift keys read, each loaded once."""

    def __init__(self, root: Path):
        self.root = root
        self.inferences = root / "inferences"
        if not (self.inferences / UNIT).is_dir():
            raise Stop(f"{root} is not a soundings archive: no inferences/{UNIT}/")
        self._json: dict[Path, dict] = {}
        self.claims = self._load_claims()
        self.claim_pairs = dict(self.claims)

    def json(self, path: Path) -> dict:
        if path not in self._json:
            if not path.is_file():
                raise Stop(f"{path} does not exist")
            self._json[path] = json.loads(path.read_text())
        return self._json[path]

    def _load_claims(self) -> list[tuple[str, set[tuple[str, str]]]]:
        """Each claim with the (type, address) pairs its record paths name."""
        claims = []
        for path in sorted((self.inferences / UNIT).glob("*.json")):
            data = self.json(path)
            inference = data.get("inference", {})
            about = inference.get("about", {})
            types = {canonical(t) for t in about.get("types", [])}
            pairs: set[tuple[str, str]] = set()
            for record in strings(data.get("rests_on", {})):
                pairs |= record_pairs(record, types)
            # Only a standing claim that measures a quantity at the address counts.
            if inference.get("state") != "standing" or not about.get("quantities"):
                pairs = set()
            addresses = set(about.get("addresses", []))
            claims.append((path.name, {p for p in pairs if p[1] in addresses}))
        return claims

    def claims_naming(self, type_key: str, address: str) -> list[str]:
        return [name for name, pairs in self.claims if (type_key, address) in pairs]

    def stage(self, type_key: str) -> tuple[Path, dict] | None:
        """The stage record of a type, filed under its own or its alias's number."""
        names = {type_key.replace(" ", "-")}
        names |= {a.replace(" ", "-") for a, c in TYPE_ALIASES.items() if c == type_key}
        for name in sorted(names):
            path = self.inferences / UNIT / "stages" / f"{name}.json"
            if path.is_file():
                return path, self.json(path)
        return None

    def p0_model_path(self, type_key: str) -> Path | None:
        found = self.stage(type_key)
        if found is None or not found[1].get("p0"):
            return None
        return self.root / found[1]["p0"]["model"]

    def bound_measured(self, type_key: str, address: str) -> bool:
        """True when p0 reads the address as a fitted law or measurement of this very pair.

        The pair is this one's only when the binding rests on a record naming it, directly or
        through a claim; a fitted law resting on another type's records is a carried one.
        """
        path = self.p0_model_path(type_key)
        if path is None:
            return False

        def names_pair(rests_on) -> bool:
            for text in strings(rests_on):
                if text.startswith("inferences/"):
                    if (type_key, address) in self.claim_pairs.get(Path(text).name, set()):
                        return True
                elif (type_key, address) in record_pairs(text, {type_key}):
                    return True
            return False

        def walk(node) -> bool:
            if isinstance(node, dict):
                if (
                    node.get("byte") == address
                    and node.get("source") in MEASURED_SOURCES
                    and node.get("fitted_on")
                    and names_pair(node.get("rests_on", []))
                ):
                    return True
                return any(walk(v) for v in node.values())
            if isinstance(node, list):
                return any(walk(v) for v in node)
            return False

        return walk(self.json(path))

    def stage_passed(self, type_key: str) -> bool:
        found = self.stage(type_key)
        return found is not None and (found[1].get("p1") or {}).get("verdict") == "passed"

    def gross(self, type_key: str) -> float | None:
        found = self.stage(type_key)
        value = ((found[1].get("p1") or {}).get("gates") or {}).get("gross", {}) if found else {}
        return value.get("residual_over_span")

    def model_sha256(self, type_key: str) -> str | None:
        """The model file's hash extended over the pan law models it names."""
        path = self.p0_model_path(type_key)
        if path is None:
            return None
        if not path.is_file():
            raise Stop(f"{path} does not exist")
        digest = hashlib.sha256(path.read_bytes())
        laws = sorted(
            {n["law"] for n in self.json(path)["nodes"] if n.get("kind") == "pan" and "law" in n}
        )
        for name in laws:
            law = self.inferences / "models" / name
            if not law.is_file():
                raise Stop(f"{law} does not exist")
            digest.update(law.read_bytes())
        return digest.hexdigest()

    def uncovered_items(self, type_key: str) -> set[str]:
        """Hashes of the type's "no candidate predicts" items, as the overlays cite them."""
        names = {type_key.replace(" ", "")}
        names |= {a.replace(" ", "") for a, c in TYPE_ALIASES.items() if c == type_key}
        for name in sorted(names):
            path = self.inferences / "candidates" / f"whole-{name}.json"
            if path.is_file():
                items = self.json(path).get("what_no_candidate_here_predicts", [])
                return {hashlib.sha1(t.encode("utf-8")).hexdigest()[:12] for t in items}
        raise Stop(f"no candidates/whole-{sorted(names)[0]}.json in {self.inferences}")


def check_key(archive: Archive, reaches: set, key: str, value, where: str, findings: list) -> None:
    """Compare one `replaced_when` key with the archive's present state."""
    if key == "table_reaches":
        if (canonical(value[0]), value[1]) in reaches:
            findings.append((key, where, f"efx-tables.json maps {value[0]} {value[1]}"))
    elif key in ("claim_names", "model_binding"):
        type_key, address = canonical(value[0]), value[1]
        if key == "claim_names":
            names = archive.claims_naming(type_key, address)
            if names:
                findings.append((key, where, f"{names[0]} now records {type_key} {address}"))
        elif archive.bound_measured(type_key, address):
            findings.append((key, where, f"p0 binds {type_key} {address} as a fitted law"))
    elif key == "stage_passed":
        if archive.stage_passed(canonical(value)):
            findings.append((key, where, f"{value} p1 verdict is now passed"))
    elif key == "covers":
        pass  # Needs the owning type; handled by the caller.
    else:
        raise Stop(f"{where}: unknown replaced_when key {key!r}")


def iter_rows(bindings: Path):
    if not bindings.is_dir():
        raise Stop(f"{bindings} is not a directory")
    for path in sorted(bindings.glob("*.json")):
        for row in json.loads(path.read_text()):
            form = row.get("designed") or row.get("enables")
            if form and "replaced_when" in form:
                yield f"{path.name} {row['type']} slot {row['slot']}", form["replaced_when"]


def check_rows(archive: Archive, reaches: set, bindings: Path, findings: list) -> None:
    for where, replaced_when in iter_rows(bindings):
        for key, value in replaced_when.items():
            check_key(archive, reaches, key, value, where, findings)


def check_overlays(archive: Archive, reaches: set, overlays: Path, findings: list) -> None:
    if not overlays.is_dir():
        raise Stop(f"{overlays} is not a directory")
    for path in sorted(overlays.glob("*.json")):
        data = json.loads(path.read_text())
        type_key = canonical(data["type"])
        cited: list[tuple[str, str]] = []
        groups = (("entries", data.get("entries", [])), ("covers", data.get("covers", [])))
        for kind, entries in groups:
            for i, entry in enumerate(entries):
                where = f"{path.name} {kind}[{i}]"
                for key, value in entry.get("replaced_when", {}).items():
                    if key == "covers":
                        cited.append((where, value))
                    else:
                        check_key(archive, reaches, key, value, where, findings)
                if kind == "entries" and "covers" in entry:
                    cited.append((where, entry["covers"]))
        if cited:
            live = archive.uncovered_items(type_key)
            for where, item in dict.fromkeys(cited):
                if item not in live:
                    what = f"item {item} of {type_key} is gone or changed"
                    findings.append(("covers", where, what))


def check_models(archive: Archive, inc: Path, findings: list) -> None:
    if not inc.is_file():
        raise Stop(f"{inc} does not exist")
    seen: set[str] = set()
    for number, sha, gross in TYPE_ROW.findall(inc.read_text()):
        type_key = f"{number[:2].upper()} {number[2:].upper()}"
        if type_key in seen:
            continue
        seen.add(type_key)
        now = archive.model_sha256(type_key)
        if now is None:
            findings.append(("model_sha256", type_key, "the archive has no p0 model for it"))
            continue
        if now != sha:
            findings.append(
                ("model_sha256", type_key, "model changed; regenerate with make gs-classic-models")
            )
        current = archive.gross(type_key)
        if current is None or f32(float(current)) != f32(float(gross)):
            findings.append(
                ("gross_residual", type_key, f"recorded {float(gross):.9g}, archive has {current}")
            )


def table_pairs(tables: Path) -> set[tuple[str, str]]:
    """The (type, address) pairs the committed conversion tables reach."""
    if not tables.is_file():
        raise Stop(f"{tables} does not exist")
    entries = json.loads(tables.read_text()).get("map", [])
    return {(canonical(e["type"]), e["address"]) for e in entries}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--archive", default=os.environ.get("GS_EFX_ARCHIVE"))
    ap.add_argument("--bindings", type=Path, default=REPO / "tools/gs/efx-bindings")
    ap.add_argument("--overlays", type=Path, default=REPO / "tools/gs/classic-overlays")
    ap.add_argument("--tables", type=Path, default=REPO / "tools/gs/efx-tables.json")
    ap.add_argument("--inc", type=Path, default=REPO / "src/midi/synth/gs_classic_models.inc")
    args = ap.parse_args(argv)
    if not args.archive:
        print("soundings_drift: no archive: pass --archive or set GS_EFX_ARCHIVE", file=sys.stderr)
        return 2
    findings: list[tuple[str, str, str]] = []
    try:
        archive = Archive(Path(args.archive))
        reaches = table_pairs(args.tables)
        check_rows(archive, reaches, args.bindings, findings)
        check_overlays(archive, reaches, args.overlays, findings)
        check_models(archive, args.inc, findings)
    except Stop as stop:
        print(f"soundings_drift: {stop}", file=sys.stderr)
        return 2
    for kind, where, what in sorted(set(findings)):
        print(f"{kind}: {where}: {what}")
    return 3 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
