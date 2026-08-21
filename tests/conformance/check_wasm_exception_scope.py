#!/usr/bin/env python3
"""Keep the WASM exception-scope list in step with the units that catch.

emscripten defaults to ``DISABLE_EXCEPTION_CATCHING=1``, which elides landing
pads at compile time.  ``-fexceptions`` on the link line only enables the
runtime support; it cannot restore a ``catch`` that was never emitted.  A
translation unit linked into the WASM module without ``-fexceptions`` therefore
has every one of its ``catch`` arms silently deleted, and a C-ABI unit loses
``SONARE_C_TRY`` / ``SONARE_C_CATCH`` entirely: the throw escapes past the
error-code translation and reaches JS raw.  A ``noexcept`` function whose
``catch (...)`` was deleted calls ``std::terminate`` and aborts the module.

The failure is invisible.  It is not a compile error, not a link error, and not
a test failure unless a test happens to assert the exact error code of a path
that throws deep inside the C ABI.  The flag is applied per source file
(``SONARE_WASM_EXCEPTION_SOURCES`` in ``src/CMakeLists.txt``) because a
whole-target ``-fexceptions`` costs several times as much binary for no
additional behaviour -- and a hand-maintained file list drifts, which is what
this check exists to catch.

Three things decide what is inspected, and each is derived from the build tree
rather than hardcoded, because a hardcoded answer is what drifted before:

* **Which units are in the module.**  The module's own ``link.txt`` is found by
  its ``-o *.js`` output, and every object it names is followed -- directly and
  through each static archive, whose members come from the ``link.txt`` that
  builds that archive.  Sibling static libraries are linked into the module and
  their ``catch`` arms are elided exactly like the module target's own, so
  scoping this check to a single target hides most of the surface.
* **Which units catch.**  A unit catches if its own text does, or if any
  repo-owned header in its dependency closure does -- an inline function in a
  header is compiled into the including TU and loses its landing pads with it.
  The closure is read from the compiler's own ``.o.d`` files rather than from a
  re-implemented include scanner.  A unit whose ``.d`` is missing is reported as
  unanalysable, never as clean.
* **What counts as a catch.**  Comments and string literals are blanked first
  (``mixing/surround_panner.cpp`` describes a catch in prose and has none).
  ``#define`` bodies are blanked too, since a ``catch`` inside a macro exists
  only where the macro expands; instead the macros whose bodies catch are
  collected -- transitively -- and spelling one of their names counts as a
  catch.  That is how ``SONARE_C_TRY`` / ``SONARE_C_CATCH`` are recognised
  without naming them here.

Known limitation: only repo-owned headers (``src/``, ``include/``) are scanned.
A ``catch`` inside a libc++ or third-party inline function is elided just the
same, but flagging it would flag every unit in the build while naming no source
file anyone can act on.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = ROOT / "bindings" / "wasm" / "build-wasm"

# Repo-owned trees whose headers are scanned as part of a unit's closure, and
# whose macro definitions form the catching-macro table.
_OWNED_TREES = (ROOT / "src", ROOT / "include")
_OWNED_SUFFIXES = (".h", ".hpp", ".hh", ".hxx", ".inc", ".ipp", ".cpp", ".cc", ".cxx")
_HEADER_SUFFIXES = (".h", ".hpp", ".hh", ".hxx", ".inc", ".ipp")

_CATCH = re.compile(r"\bcatch\s*\(")
_DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)")

# Comments and literals, longest-context-first at each position: a `"` inside a
# comment is reached only after the comment alternative has already consumed it,
# and vice versa.  Not a C++ lexer -- it only has to stop `catch (` written in
# prose or in a message string from matching.
_LEXICAL = re.compile(
    r"""//[^\n]*
      | /\*.*?\*/
      | R"([^()\\ ]*)\(.*?\)\1"
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)


def _blank(match: re.Match[str]) -> str:
    """Replace a matched span with spaces, keeping its newlines."""
    text = match.group(0)
    return "".join(ch if ch == "\n" else " " for ch in text)


def strip_comments_and_literals(text: str) -> str:
    """Blank out comments and string/char literals, preserving line structure."""
    return _LEXICAL.sub(_blank, text)


def split_define_bodies(text: str) -> tuple[str, dict[str, str]]:
    """Return (text with ``#define`` directives blanked, {macro name: body})."""
    bodies: dict[str, str] = {}
    kept: list[str] = []
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        match = _DEFINE.match(lines[i])
        if match is None:
            kept.append(lines[i])
            i += 1
            continue
        body = [lines[i]]
        kept.append("")
        while body[-1].rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            body.append(lines[i])
            kept.append("")
        bodies[match.group(1)] = "\n".join(body)
        i += 1
    return "\n".join(kept), bodies


class FileScan:
    """Catch-relevant facts about one repo-owned source or header file."""

    def __init__(self, text: str) -> None:
        code, self.macro_bodies = split_define_bodies(strip_comments_and_literals(text))
        self.text = code
        self.direct_catch = bool(_CATCH.search(code))

    @classmethod
    def from_path(cls, path: Path) -> FileScan:
        return cls(path.read_text(encoding="utf-8", errors="replace"))


def catching_macros(scans: list[FileScan]) -> set[str]:
    """Names of macros that expand to a catch, following macro-in-macro use."""
    bodies: dict[str, str] = {}
    for scan in scans:
        bodies.update(scan.macro_bodies)
    catching = {name for name, body in bodies.items() if _CATCH.search(body)}
    pending = True
    while pending:
        pending = False
        spelled = re.compile(r"\b(" + "|".join(sorted(catching)) + r")\b")
        for name, body in bodies.items():
            if name not in catching and spelled.search(body):
                catching.add(name)
                pending = True
    return catching


class Scanner:
    """Lazily scans repo-owned files and holds the catching-macro table.

    The macro table is built from every repo-owned file rather than per unit:
    whether a macro expands to a catch is a property of the macro, not of the
    unit that uses it, and building it once keeps the per-unit work linear.
    """

    def __init__(self) -> None:
        self._cache: dict[Path, FileScan | None] = {}
        scans = []
        for tree in _OWNED_TREES:
            for path in sorted(tree.rglob("*")):
                if path.suffix in _OWNED_SUFFIXES and path.is_file():
                    scan = self.scan(path)
                    if scan is not None:
                        scans.append(scan)
        macros = catching_macros(scans)
        self.macro_use = (
            re.compile(r"\b(" + "|".join(sorted(macros)) + r")\b") if macros else None
        )

    def scan(self, path: Path) -> FileScan | None:
        if path not in self._cache:
            try:
                self._cache[path] = FileScan.from_path(path)
            except OSError:
                self._cache[path] = None
        return self._cache[path]

    def catches(self, scan: FileScan) -> bool:
        if scan.direct_catch:
            return True
        return self.macro_use is not None and bool(self.macro_use.search(scan.text))


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


def _link_lines(build_dir: Path) -> list[tuple[Path, list[str]]]:
    return [(_binary_dir(p), _tokens(p)) for p in sorted(build_dir.rglob("link.txt"))]


def _output_of(tokens: list[str]) -> str | None:
    """The ``-o`` argument of a link line, or None for an archiver line."""
    for token, following in zip(tokens, tokens[1:]):
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


def owned_headers(dep_file: Path) -> list[Path]:
    """Repo-owned headers in a unit's dependency closure, from its ``.o.d``."""
    text = dep_file.read_text(encoding="utf-8", errors="replace")
    text = text.split(":", 1)[-1].replace("\\\n", " ").replace("\\ ", "\0")
    paths = (Path(t.replace("\0", " ")) for t in text.split() if t != "\\")
    return [
        path
        for path in paths
        if path.suffix in _HEADER_SUFFIXES
        and any(str(path).startswith(f"{tree}/") for tree in _OWNED_TREES)
    ]


def _display(path: Path) -> str:
    for base in (ROOT / "src", ROOT):
        try:
            return str(path.relative_to(base))
        except ValueError:
            continue
    return str(path)


def _target_of(obj: Path) -> str:
    match = re.search(r"CMakeFiles/([^/]+)\.dir/", str(obj))
    return match.group(1) if match else "?"


def audit(build_dir: Path) -> tuple[list[str], list[str], list[str], int]:
    """Return (covered, uncovered, unanalysable, linked-object count).

    ``covered`` and ``uncovered`` name catching units; ``unanalysable`` names
    units whose text or header closure could not be read, which are neither.
    """
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise SystemExit(
            f"missing {database}\n"
            "Build the WASM module first: (cd bindings/wasm && yarn build:wasm)"
        )
    by_output = {
        (build_dir / entry["output"]).resolve(): entry
        for entry in json.loads(database.read_text())
        if "output" in entry
    }
    objects = module_objects(build_dir)
    scanner = Scanner()

    covered: list[str] = []
    uncovered: list[str] = []
    unanalysable: list[str] = []
    for obj in objects:
        entry = by_output.get(obj)
        if entry is None:
            unanalysable.append(f"{_target_of(obj)}: {obj.name} (no compile command)")
            continue
        source = Path(entry["file"])
        dep_file = obj.with_suffix(f"{obj.suffix}.d")
        source_scan = scanner.scan(source)
        if source_scan is None:
            unanalysable.append(f"{_target_of(obj)}: {_display(source)} (unreadable)")
            continue
        if not dep_file.is_file():
            unanalysable.append(
                f"{_target_of(obj)}: {_display(source)} (no dependency file)"
            )
            continue
        sites: list[str] = []
        if scanner.catches(source_scan):
            sites.append("itself")
        for header in owned_headers(dep_file):
            header_scan = scanner.scan(header)
            if header_scan is not None and scanner.catches(header_scan):
                sites.append(_display(header))
        if not sites:
            continue
        name = f"{_target_of(obj)}: {_display(source)}"
        via = [site for site in sites if site != "itself"]
        if "itself" not in sites:
            name += f"  [via {', '.join(via)}]"
        (covered if "-fexceptions" in entry["command"] else uncovered).append(name)
    return sorted(covered), sorted(uncovered), sorted(unanalysable), len(objects)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    args = parser.parse_args()

    covered, uncovered, unanalysable, linked = audit(args.build_dir)
    if linked == 0:
        raise SystemExit(
            f"no objects resolved from the module link line in {args.build_dir}; "
            "the build database does not look like a WASM module build"
        )
    # A feature-reduced configuration can legitimately compile no catching unit
    # at all, so an empty set is a pass, not a broken database.
    print(f"linked translation units: {linked}")
    print(f"  of which catch: {len(covered) + len(uncovered)}")
    print(f"  compiled with -fexceptions: {len(covered)}")
    if unanalysable:
        print(
            "\nThese linked units could not be analysed, so whether their catch arms",
            "survived is unverified:",
            *(f"  {name}" for name in unanalysable),
            sep="\n",
            file=sys.stderr,
        )
    if uncovered:
        print(
            "\nThese units catch but were compiled without -fexceptions, so every",
            "catch arm in them was deleted:",
            *(f"  {name}" for name in uncovered),
            "\nUnits of the `sonare` target belong in SONARE_WASM_EXCEPTION_SOURCES",
            "in src/CMakeLists.txt; a unit of a sibling static library needs the flag",
            "on that library's own sources.",
            sep="\n",
            file=sys.stderr,
        )
    return 1 if uncovered or unanalysable else 0


if __name__ == "__main__":
    raise SystemExit(main())
