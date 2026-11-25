"""Pytest plugin: every refusal subject the Python suite provokes names a parameter of its entry.

A C-ABI refusal records the refusing entry and its subjects in the C spelling. The
leading segment of each subject (``options`` in ``options.total_frames``,
``ranges`` in ``ranges[2].start``) must be a parameter the entry's header
declaration carries, or no surface can respell it to what its caller wrote. A
path inside a JSON document parameter roots at the parameter's stem
(``config.loudness.targetLufs`` under ``config_json``). A loader refusing the
content of a ``document`` it was handed (InvalidFormat) names the path inside
that document, rooted at the document's own top-level keys, so any root is
accepted for those refusals.

The plugin wraps the binding's single refusal translation point, records each
(entry, subject) the suite reaches, and fails the session on a subject whose root
is not a parameter of any declaration answering to that entry key. It reads only
what a test happens to provoke, so its reach is printed beside the verdict.

Set SONARE_EMITTED_PAIRS=1 with the pytest plugin to compare rendered subjects
against the spelling declarations. The same check reads the Node and WASM
Vitest captures via `python3 tests/conformance/refusal_subject_plugin.py SURFACE CAPTURE`.
"""

from __future__ import annotations

import json
import os
import re
import sys
from collections import defaultdict
from collections.abc import Mapping
from dataclasses import fields, is_dataclass
from functools import cache, wraps
from inspect import signature
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SUFFIXES = ("_with_progress", "_with_options", "_checked", "_ex2", "_ex", "_v2")
_ROOT_SEGMENT = re.compile(r"[A-Za-z_]\w*")

_INVALID_FORMAT = 2
_INVALID_PARAMETER = 4
_DOCUMENT_PARAMETERS = frozenset({"document", "json"})

_seen: dict[tuple[str, str], int] = defaultdict(int)
_content: set[tuple[str, str]] = set()
_emitted: list[dict] = []


def entry_key(name: str) -> str:
    """The key the C ABI records for a refusing entry (``c_entry_key`` in sonare_c_internal.cpp)."""
    key = name.removeprefix("sonare_")
    for suffix in _SUFFIXES:
        if len(key) > len(suffix) and key.endswith(suffix):
            return key[: -len(suffix)]
    return key


def header_parameters(root: Path = ROOT) -> dict[str, set[str]]:
    """Entry key -> the parameter names of every declaration answering to it."""
    sys.path.insert(0, str(root / "tools/parity"))
    from extractors import c_api
    from extractors._tokens import strip_c_comments

    texts = [
        strip_c_comments(p.read_text(encoding="utf-8"))
        for p in c_api._collect_api_headers(root)
        if p.exists()
    ]
    return_types = set(c_api._RETURN_TYPES)
    for text in texts:
        return_types |= c_api.declared_return_types(text)
    decl = c_api._decl_re(return_types)
    params: dict[str, set[str]] = defaultdict(set)
    for text in texts:
        for m in decl.finditer(text):
            names = {c_api._parse_param(p).name for p in c_api._split_args(m.group("args"))}
            params[entry_key(m.group("name"))] |= {n for n in names if n} | document_roots(names)
    return params


def document_roots(names: set[str]) -> set[str]:
    """The root a JSON document parameter's paths take: ``config_json`` -> ``config``."""
    return {n.removesuffix("_json") for n in names if n.endswith("_json") and n != "_json"}


def takes_document(names: set[str]) -> bool:
    """Whether an entry reads a JSON document: ``document``, ``json`` or an input ``*_json``."""
    return any(
        n in _DOCUMENT_PARAMETERS or (n.endswith("_json") and n != "out_json") for n in names
    )


def misnamed(
    seen: dict[tuple[str, str], int],
    params: dict[str, set[str]],
    content: set[tuple[str, str]] | frozenset[tuple[str, str]] = frozenset(),
) -> list[tuple[str, str]]:
    """The recorded (entry, subject) pairs whose subject root is no parameter of the entry.

    ``content`` holds the pairs refused as InvalidFormat; one whose entry takes a JSON
    document (``takes_document``) names a path inside that document and is not checked.
    """
    found = []
    for entry, subject in sorted(seen):
        if (entry, subject) in content and takes_document(params.get(entry, set())):
            continue
        head = _ROOT_SEGMENT.match(subject)
        if head is None or head.group(0) not in params.get(entry, set()):
            found.append((entry, subject))
    return found


def _python_paths(value, path, depth=0):
    """Snapshot paths in ordinary arguments without traversing audio buffers."""
    if value is None:
        return
    yield path
    if depth == 4:
        return
    if type(value) is dict:
        for name, child in value.items():
            if isinstance(name, str):
                yield from _python_paths(child, f"{path}.{name}", depth + 1)
    elif type(value) in (list, tuple):
        yield f"{path}.length"
        for index, child in enumerate(value[:64]):
            yield from _python_paths(child, f"{path}[{index}]", depth + 1)
    elif type(value).__module__.startswith("libsonare.") and is_dataclass(value):
        for field in fields(value):
            if not field.name.startswith("_"):
                yield from _python_paths(
                    getattr(value, field.name), f"{path}.{field.name}", depth + 1
                )


def _python_argument_paths(frame, entry_code):
    # The decorator still holds the actual arguments, before default filling and conversion.
    given = {}
    while frame is not None:
        if frame.f_code is entry_code:
            local = frame.f_locals
            try:
                given = signature(local["fn"]).bind(*local["args"], **local["kwargs"]).arguments
            except (TypeError, ValueError):
                pass
        frame = frame.f_back
    return sorted(
        {
            path
            for name, value in given.items()
            if name not in {"self", "cls"}
            for path in _python_paths(value, name)
        }
    )


def _converted_subjects(subjects, root, respelled):
    """Read the selected alias separately from the rendered message."""
    if not isinstance(respelled, Mapping):
        return None
    prefix = f"{root}." if root is not None else ""
    return [
        given.name if (given := respelled.get(subject.removeprefix(prefix))) is not None else None
        for subject in subjects
    ]


def pytest_configure(config) -> None:
    from libsonare import _errors, _runtime

    entry_code = _errors._entry("")(lambda: None).__code__
    original = _runtime._render_as_given
    enabled = os.environ.get("SONARE_EMITTED_PAIRS") == "1"

    class RenderCapture:
        def __init__(self, lib):
            self.lib = lib
            self.subjects = ()

        def __getattr__(self, name):
            return getattr(self.lib, name)

        def sonare_last_refusal_render(self, subjects, *args):
            status = self.lib.sonare_last_refusal_render(subjects, *args)
            if not status:
                self.subjects = tuple(value.decode("utf-8") for value in subjects)
            return status

    @wraps(original)
    def recording(lib, *args, **kwargs):
        count = lib.sonare_last_refusal_subject_count()
        if count == 0:
            return original(lib, *args, **kwargs)
        entry = (lib.sonare_last_refusal_entry() or b"").decode("utf-8")
        code = lib.sonare_last_error_code()
        content = code == _INVALID_FORMAT
        subjects = []
        for index in range(count):
            raw = lib.sonare_last_refusal_subject(_runtime._to_c_size_t(index, "index"))
            pair = (entry, (raw or b"").decode("utf-8"))
            _seen[pair] += 1
            subjects.append(pair[1])
            if content:
                _content.add(pair)
        if not enabled:
            return original(lib, *args, **kwargs)
        capture = RenderCapture(lib)
        result = original(capture, *args, **kwargs)
        if capture.subjects:
            root = args[0] if args else kwargs.get("root")
            respelled = args[1] if len(args) > 1 else kwargs.get("respelled")
            aliases = _converted_subjects(subjects, root, respelled)
            witness = (
                {"argumentPaths": _python_argument_paths(sys._getframe(1), entry_code)}
                if aliases and any(name is not None for name in aliases)
                else {}
            )
            _emitted.append(
                {
                    "entry": entry,
                    **witness,
                    "refusal": {
                        **({"convertedSubjects": aliases} if witness else {}),
                        "cEntry": entry,
                        "code": _INVALID_PARAMETER if result and result[1] else code,
                        "subjects": capture.subjects,
                        "typedDocument": kwargs.get("typed_document", False),
                        "flattened": root,
                        "raw": {
                            "code": code,
                            "subjects": subjects,
                            "callerSpelled": False,
                            "fromC": True,
                        },
                    },
                }
            )
        return result

    _runtime._render_as_given = recording


def pytest_sessionfinish(session, exitstatus) -> None:
    params = header_parameters()
    bad = misnamed(_seen, params, _content)
    entries = {entry for entry, _ in _seen}
    reporter = session.config.pluginmanager.get_plugin("terminalreporter")
    lines = [
        (
            f"refusal subjects: {len(_seen)} (entry, subject) pairs over {len(entries)} entries; "
            f"{len(bad)} name no parameter of their entry"
        )
    ]
    lines += [f"  {entry}: {subject}" for entry, subject in bad]
    if reporter is not None:
        reporter.write_line("")
    for line in lines:
        if reporter is not None:
            reporter.write_line(line)
        else:
            print(line)
    rejected = os.environ.get("SONARE_EMITTED_PAIRS") == "1" and check_emitted(
        "python", _emitted, reporter.write_line if reporter else print
    )
    if (bad or rejected) and session.exitstatus == 0:
        session.exitstatus = 1


def declaration_facts(surface: str):
    """Read the same declared subjects, overrides and unbound reasons as the generator."""
    sys.path.insert(0, str(ROOT / "tools/api"))
    import tomllib

    import gen_field_spelling as spelling
    from facts import field_bindings

    bindings = field_bindings(ROOT, (surface,))
    renames = tomllib.loads(spelling.RENAMES.read_text(encoding="utf-8"))
    register = tomllib.loads(spelling.UNBOUND.read_text(encoding="utf-8"))
    c_params = spelling.load_c_params(ROOT)
    requests = spelling.load_request_fields((surface,)) if surface != "python" else {}
    derived, _, _ = spelling.derive(bindings, requests, renames, c_params)
    bound = {
        (b.c_key, b.c_subject): b.surface_spelling
        for b in bindings
        if b.c_subject and b.surface_spelling and b.kind == "param"
    }
    for row in derived[surface]:
        name = row.spelling
        if row.length:
            name += " length" if surface == "python" else ".length"
        if row.divisor:
            name += " / " + row.divisor
        if not row.document:
            bound[row.c_key, row.c_subject] = name
    roots = {row.c_key: (row.c_subject, row.spelling) for row in derived[surface] if row.document}
    declared = {
        (b.c_key, b.c_subject)
        for b in bindings
        if b.kind == "param"
        and b.c_subject
        and f"{b.c_key}.{b.c_subject}" not in register["params"]
    } | bound.keys()
    declared.update((key, name.rstrip(".")) for key, (_, name) in roots.items())
    visible = {
        (b.c_key, b.surface_spelling.partition(".")[0])
        for b in bindings
        if b.surface_spelling and b.kind == "param"
    }
    for row in requests.get(surface, ()):
        for key in spelling.entry_keys(row["entry"]):
            visible.update((key, name) for name in [row["param"], *row["fields"]])
    records = defaultdict(set)
    for row in requests.get(surface, ()):
        if (index := row.get("index")) is not None:
            records[row["entry"]].add((index, row["param"]))
    return header_parameters(), declared, bound, roots, visible, records


def argument_paths(value, prefix=""):
    if isinstance(value, list):
        for index, child in enumerate(value):
            yield from argument_paths(child, f"{prefix}[{index}]")
    if not isinstance(value, dict):
        return
    for name, child in value.items():
        path = f"{prefix}.{name}" if prefix else name
        yield path
        yield from argument_paths(child, path)


def emitted_pairs(surface, rows, facts):
    params, declared, bound, documents, visible, records = facts
    sys.path.insert(0, str(ROOT / "tools/api"))
    from gen_field_spelling import entry_key as canonical_entry
    from gen_field_spelling import entry_keys, mechanical

    @cache
    def direct_entry(entry):
        canonical = canonical_entry(entry)
        if canonical in params:
            return canonical
        candidates = entry_keys(entry) & params.keys()
        return next(iter(candidates)) if len(candidates) == 1 else None

    def position_stem(subject):
        # Unit alternatives retain their position name when a facade folds them.
        name = canonical_entry(subject)
        stem, _, unit = name.rpartition("_")
        return stem if unit in {"sample", "samples", "sec", "ppq"} else None

    checked, failures = set(), set()
    for row in rows:
        final = row.get("refusal") or {}
        entry = row["entry"]
        raw = final.get("raw")
        if row.get("requireNative") and not (raw and final.get("cEntry")):
            failures.add(f"{entry}: required native refusal record is missing")
        if not raw or not raw["subjects"]:
            continue
        key = final.get("cEntry") or direct_entry(entry)
        if key is None:
            continue
        args = row.get("args", [])
        supplied = set(row.get("argumentPaths", ())) | {
            path for arg in args for path in argument_paths(arg)
        }
        rooted_paths = {
            path
            for index, name in records.get(entry, ())
            if index < len(args)
            for path in argument_paths(args[index], name)
        }
        positional = surface != "python" and not any(isinstance(arg, dict) for arg in args)
        names = params.get(key, set())
        prefix, root = documents.get(key, ("", ""))
        subjects = final["subjects"]
        converted = final.get("convertedSubjects", [None] * len(subjects))
        if len(subjects) != len(raw["subjects"]) or len(subjects) != len(converted):
            failures.add(f"{entry} ({key}): subject count changed")
            continue
        for original, shown, conversion in zip(raw["subjects"], subjects, converted, strict=True):
            typed = final.get("typedDocument") or bool(args and isinstance(args[0], dict))
            rooted = root and (prefix or typed and raw["code"] == _INVALID_FORMAT)
            if rooted and (original.startswith(prefix) or original == prefix[:-1]):
                tail = original.removeprefix(prefix) if original.startswith(prefix) else ""
                expected = root + tail if tail else root.rstrip(".")
                checked.add((key, root.rstrip(".")))
                if shown != expected or not prefix and final["code"] == _INVALID_FORMAT:
                    failures.add(
                        f"{entry} ({key}): {original!r} -> {shown!r}; expected {expected!r}"
                    )
                continue
            if raw["code"] == _INVALID_FORMAT:
                continue
            head, _, tail = original.partition(".")
            match = _ROOT_SEGMENT.match(head)
            raw_root = match.group() if match else ""
            flattened = head in {final.get("flattened"), raw.get("flattened")}
            direct = not (raw.get("fromC") or row.get("requireNative"))
            # A direct bridge can already hold a public field or a literal key in its bag.
            # Use the original witness and declaration, never the rendered subject, to identify it.
            if (
                direct
                and not flattened
                and original in supplied
                and (raw["callerSpelled"] or raw_root in names or final.get("form") == "unknownKey")
            ):
                matches = {
                    name
                    for name in names
                    if bound.get((key, name), mechanical(name, surface)) == raw_root
                }
                if len(matches) == 1:
                    checked.add((key, next(iter(matches))))
                    if shown != original:
                        failures.add(
                            f"{entry} ({key}): {original!r} -> {shown!r}; expected {original!r}"
                        )
                    continue
            flatten = flattened or (
                surface != "python" and head in {"config", "options"} and not raw["callerSpelled"]
            )
            leaf = tail if tail and flatten else original
            match = _ROOT_SEGMENT.match(leaf)
            leaf_root = match.group() if match else ""
            label = (key, leaf_root)
            expected = bound.get((key, leaf), bound.get(label))
            nested = supplied.intersection({original, shown})
            public_root = _ROOT_SEGMENT.match(shown)
            visible_pair = public_root and (key, public_root.group()) in visible
            lost_root = original in row.get("strings", []) or any(
                path.endswith(f".{original}") for path in supplied
            )
            # A direct bridge can lower a record into separate member parameters.
            # Require the converter's exact member alias and its supplied public root.
            member = (
                direct
                and raw["callerSpelled"]
                and conversion is not None
                and conversion in supplied
                and conversion.endswith(f".{original}")
                and (key, conversion.partition(".")[0]) in visible
            )
            if raw_root not in names and (not direct or lost_root and not member):
                failures.add(f"{entry} ({key}): {original!r} names no C argument")
                continue
            # Explicit conversions are declared at the call site. Check that the target exists
            # on the supplied request or is a declared positional name before using the trace.
            if conversion is not None and (
                conversion in supplied
                or conversion in rooted_paths
                or positional
                and (key, conversion) in visible
            ):
                # A string can stand for a preset, while an object supplies its own field.
                if leaf.startswith(f"{conversion}.") and leaf in supplied:
                    conversion = leaf
                stem = position_stem(original)
                if stem is not None and position_stem(conversion) not in {None, stem}:
                    alternatives = {path for path in supplied if position_stem(path) == stem}
                    if len(alternatives) == 1:
                        conversion = next(iter(alternatives))
                checked.add(label)
                if shown != conversion:
                    failures.add(
                        f"{entry} ({key}): {original!r} -> {shown!r}; expected {conversion!r}"
                    )
                continue
            if raw_root not in names and not (nested or visible_pair):
                continue
            if expected is None and label not in declared and not (nested or visible_pair):
                continue
            if (
                expected
                and shown.startswith(expected + "[")
                and original.count("[") != shown.count("[")
            ):
                # A shape-changing conversion's indices need the dimensions of the input.
                continue
            checked.add(label)
            if expected is not None:
                if leaf != leaf_root and (key, leaf) not in bound:
                    tail = leaf.removeprefix(leaf_root)
                    expected += (
                        tail
                        if raw["callerSpelled"]
                        else ".".join(mechanical(segment, surface) for segment in tail.split("."))
                    )
            else:
                expected = (
                    leaf
                    if raw["callerSpelled"]
                    else ".".join(mechanical(segment, surface) for segment in leaf.split("."))
                )
            if shown != expected:
                failures.add(f"{entry} ({key}): {original!r} -> {shown!r}; expected {expected!r}")
    return checked, declared - checked, failures


def check_emitted(surface, rows, report=print):
    checked, not_compared, failures = emitted_pairs(surface, rows, declaration_facts(surface))
    report(f"emitted pairs ({surface}): {len(checked)} checked; {len(not_compared)} not compared")
    for entry, subject in sorted(not_compared):
        report(f"  not compared: {entry}.{subject}")
    for failure in sorted(failures):
        report(f"  {failure}")
    if not checked:
        report("  no verdicts: the capture fed no comparable refusal")
    return bool(failures or not checked)


if __name__ == "__main__":
    surface, path = sys.argv[1:]
    with open(path, encoding="utf-8") as capture:
        sys.exit(check_emitted(surface, (json.loads(line) for line in capture if line.strip())))
