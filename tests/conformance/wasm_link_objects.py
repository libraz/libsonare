"""Resolve the objects a WASM module's link line pulls in, static archives included.

The answer comes from the ``link.txt`` files CMake wrote into the build tree, with
response files spliced in and each archive replaced by the objects its archiver
line put into it, so it is a property of the build that produced the module
rather than of the source lists that were meant to produce it.
"""

from __future__ import annotations

import itertools
import shlex
from pathlib import Path


def _tokens(path: Path) -> list[str]:
    return shlex.split(path.read_text(encoding="utf-8"), comments=False, posix=True)


def _binary_dir(link_txt: Path) -> Path:
    """The directory a ``link.txt`` command runs in.

    The file lives in ``<binary dir>/CMakeFiles/<target>.dir/`` but its paths are
    relative to ``<binary dir>``, so climb back out of the CMake bookkeeping.
    """
    directory = link_txt.parent
    while directory.name.endswith(".dir") or directory.name == "CMakeFiles":
        directory = directory.parent
    return directory


def _expand_response_files(binary_dir: Path, tokens: list[str]) -> list[str]:
    """Splices each ``@file`` response file (relative to @p binary_dir) into the line.

    Newer Emscripten releases hand CMake's object and library lists to the linker
    this way, so a link line read without expansion names no objects at all.
    """
    expanded: list[str] = []
    for token in tokens:
        response = binary_dir / token[1:] if token.startswith("@") else None
        if response is not None and response.is_file():
            expanded.extend(_tokens(response))
        else:
            expanded.append(token)
    return expanded


def _link_lines(build_dir: Path) -> list[tuple[Path, list[str]]]:
    lines = []
    for path in sorted(build_dir.rglob("link.txt")):
        binary_dir = _binary_dir(path)
        lines.append((binary_dir, _expand_response_files(binary_dir, _tokens(path))))
    return lines


def _output_of(tokens: list[str]) -> str | None:
    """The ``-o`` argument of a link line, or None for an archiver line."""
    for token, following in itertools.pairwise(tokens):
        if token == "-o":
            return following
    return None


def _archive_members(
    link_lines: list[tuple[Path, list[str]]],
) -> dict[Path, list[Path]]:
    """Map each produced static archive to the objects archived into it.

    An archiver line is told from a link line by having no ``-o``: a link line
    lists archives as *inputs*, so keying on the first ``.a`` token alone would
    file the module's own objects under the first library it links against.
    """
    members: dict[Path, list[Path]] = {}
    for link_dir, tokens in link_lines:
        if _output_of(tokens) is not None:
            continue
        archives = [t for t in tokens if t.endswith(".a")]
        objects = [t for t in tokens if t.endswith(".o")]
        if not archives or not objects:
            continue
        # An `ar`-style line names the archive it produces before its members.
        members.setdefault((link_dir / archives[0]).resolve(), []).extend(
            (link_dir / obj).resolve() for obj in objects
        )
    return members


def module_objects(build_dir: Path) -> list[Path]:
    """Every object linked into the WASM module, static archives resolved."""
    link_lines = _link_lines(build_dir)
    module = [
        (link_dir, tokens)
        for link_dir, tokens in link_lines
        if (_output_of(tokens) or "").endswith(".js")
    ]
    if len(module) != 1:
        raise SystemExit(
            f"expected exactly one WASM module link line under {build_dir}, found "
            f"{len(module)}; the build tree does not look like a WASM module build"
        )
    link_dir, tokens = module[0]
    members = _archive_members(link_lines)
    objects: list[Path] = []
    seen: set[Path] = set()
    for token in tokens:
        if token.endswith(".o"):
            linked = [(link_dir / token).resolve()]
        elif token.endswith(".a"):
            linked = members.get((link_dir / token).resolve(), [])
        else:
            continue
        for obj in linked:
            if obj not in seen:
                seen.add(obj)
                objects.append(obj)
    return objects
