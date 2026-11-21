"""Pytest plugin: every refusal subject the Python suite provokes names a parameter of its entry.

A C-ABI refusal records the refusing entry and its subjects in the C spelling. The
leading segment of each subject (``options`` in ``options.total_frames``,
``ranges`` in ``ranges[2].start``) must be a parameter the entry's header
declaration carries, or no surface can respell it to what its caller wrote. A
path inside a JSON document parameter roots at the parameter's stem
(``config.loudness.targetLufs`` under ``config_json``).

The plugin wraps the binding's single refusal translation point, records each
(entry, subject) the suite reaches, and fails the session on a subject whose root
is not a parameter of any declaration answering to that entry key. It reads only
what a test happens to provoke, so its reach is printed beside the verdict.

Usage: rye run pytest -p refusal_subject_plugin (with tests/conformance on PYTHONPATH)
"""

from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SUFFIXES = ("_with_progress", "_with_options", "_checked", "_ex2", "_ex", "_v2")
_ROOT_SEGMENT = re.compile(r"[A-Za-z_]\w*")

_seen: dict[tuple[str, str], int] = defaultdict(int)


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


def misnamed(
    seen: dict[tuple[str, str], int], params: dict[str, set[str]]
) -> list[tuple[str, str]]:
    """The recorded (entry, subject) pairs whose subject root is no parameter of the entry."""
    found = []
    for entry, subject in sorted(seen):
        head = _ROOT_SEGMENT.match(subject)
        if head is None or head.group(0) not in params.get(entry, set()):
            found.append((entry, subject))
    return found


def pytest_configure(config) -> None:
    from libsonare import _runtime

    original = _runtime._render_as_given

    def recording(lib, root, respelled):
        entry = (lib.sonare_last_refusal_entry() or b"").decode("utf-8")
        for index in range(lib.sonare_last_refusal_subject_count()):
            raw = lib.sonare_last_refusal_subject(_runtime._to_c_size_t(index, "index"))
            _seen[(entry, (raw or b"").decode("utf-8"))] += 1
        return original(lib, root, respelled)

    _runtime._render_as_given = recording


def pytest_sessionfinish(session, exitstatus) -> None:
    params = header_parameters()
    bad = misnamed(_seen, params)
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
    if bad and session.exitstatus == 0:
        session.exitstatus = 1
