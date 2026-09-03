#!/usr/bin/env python3
"""The static gates say the same thing in every file that carries them.

CI does not invoke the Makefile for linting. Both workflows inline their own
`python3 -m ruff` and their own `git ls-files | clang-format` pipeline, so each
scope exists in three or four places at once and a widening has to be made in
all of them. Changing only the Makefile leaves the gate that actually runs on a
push exactly as narrow as it was, and reports green while doing it: ruff linted
`bindings/python` alone while Python sat in ten trees, and the clang-format glob
once omitted `.mm` the same way.

The version pins are the same shape of claim. Each workflow installs a pinned
tool with a comment saying the pin is "the resolved version in
requirements-dev.lock" -- true when written and enforced by nothing, so a lock
bump silently leaves CI checking with a different tool than `make lint` uses.

Stdlib only, no build. Run directly:

    python3 tools/conformance/check_lint_scope.py
"""

from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MAKEFILE = REPO_ROOT / "Makefile"
WORKFLOWS = (
    REPO_ROOT / ".github/workflows/ci.yml",
    REPO_ROOT / ".github/workflows/develop-ci.yml",
)
LOCK = REPO_ROOT / "bindings/python/requirements-dev.lock"

#: `ruff check <target>`, however the interpreter in front of it is spelled and
#: whatever flags sit between. Only the target is scope: `make format` runs the
#: same path with `--fix` and that is not a narrower gate.
RUFF_CALL = re.compile(r"ruff\s+check\s+(?:--[\w-]+\s+)*(?P<target>[^\s|;&]+)")

#: The pathspec list a `git ls-files ... -- <specs> |` pipeline feeds clang-format.
LS_FILES_SPECS = re.compile(r"git ls-files[^|]*?--\s+(?P<specs>(?:'[^']*'\s*)+)")

#: A pinned install, as `pip install ... name==version`.
PIN = re.compile(r"(?P<name>[A-Za-z0-9._-]+)==(?P<version>[A-Za-z0-9._-]+)")


def normalize(name: str) -> str:
    """PEP 503 name comparison, so `Foo_Bar` and `foo-bar` are one package."""
    return re.sub(r"[-_.]+", "-", name).lower()


def ruff_targets(text: str) -> list[str]:
    """The distinct paths `ruff check` is pointed at in @p text."""
    return sorted({m.group("target") for m in RUFF_CALL.finditer(text)})


def clang_format_specs(text: str) -> list[tuple[str, ...]]:
    """Each `git ls-files` pathspec list that reaches clang-format, in order.

    The Makefile's `format` also passes `--cached --others --exclude-standard`
    so it reformats untracked files too. That is a deliberate difference in
    *which files exist*, not in which are in scope, so only the pathspec is
    compared.
    """
    out = []
    matches = list(LS_FILES_SPECS.finditer(text))
    for i, m in enumerate(matches):
        # Attribute each pipeline to its own consumer: the window runs to the next
        # `git ls-files`, so a nearby unrelated one cannot lend its clang-format.
        # A fixed character lookahead does exactly that, and did.
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        if "clang-format" not in text[m.end():end]:
            continue
        out.append(tuple(re.findall(r"'([^']*)'", m.group("specs"))))
    return out


def locked_versions(text: str) -> dict[str, str]:
    """The lock's pinned versions, keyed by normalized package name."""
    versions = {}
    for line in text.splitlines():
        line = line.split("#")[0].strip()
        m = re.fullmatch(r"(?P<name>[A-Za-z0-9._-]+)==(?P<version>[^\s;]+)", line)
        if m:
            versions[normalize(m.group("name"))] = m.group("version")
    return versions


def pinned_installs(text: str) -> list[tuple[str, str]]:
    """Every `name==version` a pip install line in @p text asks for."""
    out = []
    for line in text.splitlines():
        if "pip install" not in line:
            continue
        out.extend((m.group("name"), m.group("version")) for m in PIN.finditer(line))
    return out


def check_agreement(label: str, found: dict[str, list], failures: list[str]) -> None:
    """Fail unless every file that carries @p label carries the same value."""
    carriers = {name: value for name, value in found.items() if value}
    if not carriers:
        failures.append(
            f"{label}: no file carries it at all. Either the invocation moved and this "
            "check is now blind, or the gate was deleted.")
        return
    distinct = {tuple(value) for value in carriers.values()}
    if len(distinct) == 1:
        return
    failures.append(
        f"{label} disagrees across the files that carry it. CI does not call the "
        f"Makefile, so the narrowest of these is the gate that actually runs:\n"
        + "\n".join(f"    {name}: {value}" for name, value in sorted(carriers.items())))


def main() -> int:
    sources = {p.relative_to(REPO_ROOT).as_posix(): p.read_text()
               for p in (MAKEFILE, *WORKFLOWS)}
    failures: list[str] = []

    check_agreement("the ruff scope",
                    {name: ruff_targets(text) for name, text in sources.items()},
                    failures)
    check_agreement("the clang-format pathspec",
                    {name: sorted(set(clang_format_specs(text)))
                     for name, text in sources.items()},
                    failures)

    locked = locked_versions(LOCK.read_text())
    lock_name = LOCK.relative_to(REPO_ROOT).as_posix()
    checked = 0
    for name, text in sources.items():
        if name == "Makefile":
            continue  # the Makefile installs through rye, which reads the lock itself
        for package, version in pinned_installs(text):
            want = locked.get(normalize(package))
            if want is None:
                # A CI-only tool the binding does not depend on -- librosa, for the
                # reference-generation job. The lock cannot speak for it, so there is
                # nothing here to disagree with.
                continue
            if want != version:
                failures.append(
                    f"{name} pins {package}=={version} but {lock_name} resolves "
                    f"{package}=={want}. The workflow comment claims the pin follows "
                    f"the lock; a bump moved one and not the other, so CI and "
                    f"`make lint` no longer run the same tool.")
            checked += 1
    if not checked:
        failures.append(
            "no workflow pins any tool version, so this check verified nothing. The "
            "install lines moved and the parsing above is stale.")

    if failures:
        print("\n".join(f"[FAIL] {f}" for f in failures))
        return 1
    ruff = ruff_targets(sources["Makefile"])
    specs = sorted(set(clang_format_specs(sources["Makefile"])))
    print(f"lint scope agrees across {len(sources)} file(s): ruff {ruff[0]}, "
          f"clang-format {' '.join(specs[0])}")
    print(f"{checked} pinned tool version(s) match {lock_name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
