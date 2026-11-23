"""Keep every Python narrowing label a name the caller wrote.

A ``_to_c_*`` / ``_narrow_*`` reader refuses an out-of-range value with the label it
was handed, verbatim: the respelling ``_runtime`` applies covers C refusals only. A
label that is not a parameter of the function making the call (or of a function
enclosing it) therefore names something the caller never wrote -- a C dimension
(``n_frames``), a local loop name (``clip.start_ppq`` for ``clips[i]``), or a
resolved handle (``strip`` for ``ref``).

Each call with a literal label is accepted when the label's root is a parameter of the
enclosing function chain, or when the reader is 64-bit wide and its value is a count
taken from an array (``len``, an array reader's returned length, or arithmetic over
those), which no Python object can push past the target. Every other call needs a
record in ``python_narrowing_label_records.json``, keyed on (file, function, label):

* ``bounded`` -- the value cannot reach the refusal; the reason names the guard.
* ``caller`` -- a private helper spells its public callers' parameter; the reason names them.
* ``open`` -- the label can surface; the reason names what the caller wrote instead.

A record that matches no call fails, so a fixed site retires its record. A label built
at run time (``surface_spelling(...)``, an f-string) is the caller's spelling by
construction and is not read.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "bindings" / "python" / "src" / "libsonare"
RECORDS = Path(__file__).with_name("python_narrowing_label_records.json")

READER = re.compile(r"^_(?:to_c_\w+|narrow_\w+)$")
WIDE_READERS = frozenset({"_to_c_size_t", "_to_c_int64", "_to_c_uint64"})
# Calls whose returned counts are lengths of what the caller passed.
COUNT_SOURCES = frozenset(
    {
        "len",
        "_assistant_params",
        "_channel_arrays",
        "_mastering_params",
        "_midi_events_to_c",
        "_note_targets_to_c",
        "_notes_to_c",
        "_percussive_events_to_c",
        "_planar_channel_arrays",
        "_square_input",
        "_stereo_channels",
        "_stereo_pair_params",
        "_streaming_platforms",
        "_to_c_float_array",
        "_to_c_float_array_owned",
        "_to_c_float_matrix",
        "_to_c_int_array",
        "_ump_words_to_c",
    }
)
STATES = frozenset({"bounded", "caller", "open"})


@dataclass(frozen=True)
class Site:
    file: str
    line: int
    function: str
    reader: str
    label: str

    @property
    def key(self) -> tuple[str, str, str]:
        return (self.file, self.function, self.label)


def _label(call: ast.Call) -> str | None:
    node = next((kw.value for kw in call.keywords if kw.arg in ("name", "arg_name")), None)
    if node is None and len(call.args) >= 2:
        node = call.args[1]
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


def _parameters(function: ast.FunctionDef | ast.AsyncFunctionDef) -> set[str]:
    a = function.args
    names = {x.arg for x in [*a.posonlyargs, *a.args, *a.kwonlyargs]}
    names.update(x.arg for x in (a.vararg, a.kwarg) if x is not None)
    return names


def _counts(function: ast.AST) -> set[str]:
    """Names bound in ``function`` to a count taken from an array, followed through arithmetic."""
    counts: set[str] = set()
    changed = True
    while changed:
        changed = False
        for node in ast.walk(function):
            if not isinstance(node, ast.Assign) or not _is_count(node.value, counts):
                continue
            for target in node.targets:
                for name in ast.walk(target):
                    if isinstance(name, ast.Name) and name.id not in counts:
                        counts.add(name.id)
                        changed = True
    return counts


def _is_count(value: ast.expr, counts: set[str]) -> bool:
    if isinstance(value, ast.Call):
        callee = value.func
        name = callee.id if isinstance(callee, ast.Name) else getattr(callee, "attr", None)
        if name == "int" and len(value.args) == 1:
            return _is_count(value.args[0], counts)
        return name in COUNT_SOURCES
    if isinstance(value, ast.Name):
        return value.id in counts
    if isinstance(value, ast.Attribute):
        return value.attr in ("size", "shape")
    if isinstance(value, ast.Subscript):
        return isinstance(value.value, ast.Attribute) and value.value.attr == "shape"
    if isinstance(value, ast.BinOp):
        if isinstance(value.op, ast.FloorDiv):
            return _is_count(value.left, counts)
        return _is_count(value.left, counts) and _is_count(value.right, counts)
    return False


def _is_fixed(value: ast.expr) -> bool:
    """A literal or a module constant: nothing the caller passed reaches the reader."""
    if isinstance(value, ast.Constant):
        return True
    if isinstance(value, ast.Name):
        return value.id.isupper() or (value.id.startswith("_") and value.id[1:].isupper())
    if isinstance(value, ast.IfExp):
        return _is_fixed(value.body) and _is_fixed(value.orelse)
    return False


def scan(package: Path) -> tuple[list[Site], int, int]:
    """(calls needing a record, calls read, calls accepted as wide array counts)."""
    sites: list[Site] = []
    read = 0
    counted = 0
    for path in sorted(package.glob("*.py")):
        tree = ast.parse(path.read_text(), filename=str(path))
        parents: dict[ast.AST, ast.AST] = {}
        for node in ast.walk(tree):
            for child in ast.iter_child_nodes(node):
                parents[child] = node
        for node in ast.walk(tree):
            if not (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Name)
                and READER.match(node.func.id)
            ):
                continue
            label = _label(node)
            if label is None:
                continue
            chain: list[ast.FunctionDef | ast.AsyncFunctionDef] = []
            up: ast.AST = node
            while up in parents:
                up = parents[up]
                if isinstance(up, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    chain.append(up)
            if not chain:
                continue
            read += 1
            root = re.split(r"[.\[ ]", label, maxsplit=1)[0]
            if any(root in _parameters(f) for f in chain):
                continue
            value = node.args[0] if node.args else None
            if value is not None and _is_fixed(value):
                continue
            if (
                node.func.id in WIDE_READERS
                and value is not None
                and _is_count(value, _counts(chain[0]))
            ):
                counted += 1
                continue
            sites.append(Site(path.name, node.lineno, chain[0].name, node.func.id, label))
    return sites, read, counted


def evaluate(sites: list[Site], read: int, counted: int, data: dict) -> list[tuple[str, list[str]]]:
    failures: list[tuple[str, list[str]]] = []
    records: dict[tuple[str, str, str], dict] = {}
    malformed: list[str] = []
    for i, rec in enumerate(data.get("records", [])):
        key = (rec.get("file"), rec.get("function"), rec.get("label"))
        if rec.get("state") not in STATES or not rec.get("reason") or None in key:
            malformed.append(f"  record {i}: {rec}")
        elif key in records:
            malformed.append(f"  record {i}: listed twice: {key}")
        else:
            records[key] = rec
    if malformed:
        failures.append(("Malformed records (file, function, label, state, reason)", malformed))
    seen = {s.key for s in sites}
    unrecorded = [
        f"  {s.file}:{s.line}  {s.function}  {s.reader}(..., {s.label!r})"
        for s in sites
        if s.key not in records
    ]
    if unrecorded:
        failures.append(
            (
                (
                    "Narrowing labels that are not a parameter of the calling function; "
                    "pass the caller's spelling or record the call"
                ),
                unrecorded,
            )
        )
    stale = [f"  {k[0]}  {k[1]}  {k[2]!r}" for k in records if k not in seen]
    if stale:
        failures.append(("Records that match no call; delete them", stale))
    floor = data.get("floor", {})
    if read < floor.get("labels_read", 0):
        failures.append(
            ("Fewer labelled reader calls than the floor", [f"  read {read}, floor {floor}"])
        )
    if counted < floor.get("wide_counts", 0):
        failures.append(
            ("Fewer wide array counts than the floor", [f"  counted {counted}, floor {floor}"])
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, default=PACKAGE)
    parser.add_argument("--records", type=Path, default=RECORDS)
    args = parser.parse_args()
    data = json.loads(args.records.read_text())
    sites, read, counted = scan(args.package)
    states = [r.get("state") for r in data.get("records", [])]
    print(
        f"narrowing labels: read {read}, wide array counts {counted}, "
        f"recorded bounded {states.count('bounded')}, caller {states.count('caller')}, "
        f"open {states.count('open')}"
    )
    failures = evaluate(sites, read, counted, data)
    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
