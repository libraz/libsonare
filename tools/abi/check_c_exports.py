"""Check that a built shared library exports exactly the C ABI the headers declare.

Every `sonare_*` function declared in the public C headers must be a defined,
exported text symbol of the library, and the library must export no `sonare_*`
function the headers do not declare. The header set does not depend on the
build configuration, so a feature-off build is held to the same list: a gated
entry answers NOT_SUPPORTED from a stub rather than disappearing from the ABI.

Usage: python3 tools/abi/check_c_exports.py <libsonare.so|.dylib|lib-dir> [...]
Exit status 1 on any missing or undeclared export, or when either side is empty.
"""

from __future__ import annotations

import argparse
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def declared_entries(root: Path = ROOT) -> set[str]:
    """Every function name the public C headers declare."""
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
    return {m.group("name") for text in texts for m in decl.finditer(text)}


def exported_entries(nm_output: str, *, mach_o: bool) -> set[str]:
    """The defined, exported `sonare_*` text symbols in `nm` output."""
    names = set()
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) != 3 or fields[1] != "T":
            continue
        name = fields[2]
        if mach_o:
            name = name.removeprefix("_")
        if name.startswith("sonare_"):
            names.add(name)
    return names


def resolve_library(path: Path) -> Path | None:
    """The library itself, or the unversioned libsonare inside a lib/ directory."""
    if not path.is_dir():
        return path if path.exists() else None
    for name in ("libsonare.dylib", "libsonare.so"):
        if (path / name).exists():
            return path / name
    return None


def nm_command(library: Path) -> list[str]:
    if platform.system() == "Darwin":
        return ["nm", "-gU", str(library)]
    return ["nm", "-D", "--defined-only", str(library)]


def compare(declared: set[str], exported: set[str]) -> list[str]:
    """Problem lines; empty when the two sets agree and neither is empty."""
    problems = []
    if not declared:
        problems.append("the headers declare no sonare_* function; the extractor read nothing")
    if not exported:
        problems.append("the library exports no sonare_* function; nm read nothing")
    problems += [f"declared but not exported: {name}" for name in sorted(declared - exported)]
    problems += [f"exported but not declared: {name}" for name in sorted(exported - declared)]
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("libraries", nargs="+", type=Path)
    args = parser.parse_args()
    declared = declared_entries()
    status = 0
    for given in args.libraries:
        library = resolve_library(given)
        if library is None:
            print(f"C exports: {given}: no libsonare.dylib or libsonare.so there")
            status = 1
            continue
        run = subprocess.run(nm_command(library), capture_output=True, text=True, check=False)
        if run.returncode != 0:
            print(f"C exports: {library}: nm failed: {run.stderr.strip()}")
            status = 1
            continue
        exported = exported_entries(run.stdout, mach_o=platform.system() == "Darwin")
        problems = compare(declared, exported)
        print(
            f"C exports: {library}: declared={len(declared)} exported={len(exported)} "
            f"problems={len(problems)}"
        )
        for line in problems:
            print(f"  {line}")
        if problems:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
