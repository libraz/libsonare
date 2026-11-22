"""The static gates say the same thing in every file that carries them.

CI does not invoke the Makefile for linting. Both workflows inline their own
`python3 -m ruff` and their own `git ls-files | clang-format` pipeline, so each
scope exists in three or four places at once and a widening has to be made in
all of them. Changing only the Makefile leaves the gate that actually runs on a
push exactly as narrow as it was, and reports green while doing it: ruff linted
`bindings/python` alone while Python sat in ten trees, and the clang-format glob
once omitted `.mm` the same way.

Tool versions get the same treatment by having one source. Every tool a
workflow runs comes from a dependency group in `bindings/python/uv.lock`, so a
lock bump moves CI and `make lint` together. What this checks is that no
workflow reaches around the lock: a bare `pip install <name>` or an unlocked
`uv sync` would bring back a hand-copied version that nothing keeps in step.

Stdlib only, no build. Run directly:

    python3 tools/conformance/check_lint_scope.py
"""

from __future__ import annotations

import re
import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT

MAKEFILE = REPO_ROOT / "Makefile"
WORKFLOWS = (
    REPO_ROOT / ".github/workflows/ci.yml",
    REPO_ROOT / ".github/workflows/develop-ci.yml",
)
#: Every workflow, not just the two that lint: a release workflow installing a
#: tool by name escapes the lock just as well.
ALL_WORKFLOWS = tuple(sorted((REPO_ROOT / ".github/workflows").glob("*.yml")))

#: `ruff check <target>`, however the interpreter in front of it is spelled and
#: whatever flags sit between. Only the target is scope: `make format` runs the
#: same path with `--fix` and that is not a narrower gate. The target has to sit
#: on the same line, because the auto-fix side takes its files from an `xargs`
#: pipeline and so ends the line at the flags: letting the match cross the
#: newline read the next recipe line as the scope, and letting a flag be the
#: target reads `--fix` as one.
RUFF_CALL = re.compile(r"ruff[ \t]+check(?:[ \t]+--[\w-]+)*[ \t]+(?P<target>(?!-)[^\s|;&]+)")

#: The pathspec list a `git ls-files ... -- <specs> |` pipeline feeds clang-format.
LS_FILES_SPECS = re.compile(r"git ls-files[^|]*?--\s+(?P<specs>(?:'[^']*'\s*)+)")

#: Options of `pip install` / `uv pip install` that take a value, and what each
#: value may be. `-r` must name a file `uv export` wrote from the lock; `-e` only
#: the project itself; `--python` and `--resolution` are not packages at all.
VALUE_OPTIONS = {
    "--python": None,
    "--resolution": None,
    "-r": None,
    "--requirement": None,
    "-e": ".",
    "--editable": ".",
}


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
        if "clang-format" not in text[m.end() : end]:
            continue
        out.append(tuple(re.findall(r"'([^']*)'", m.group("specs"))))
    return out


def shell_lines(text: str) -> list[str]:
    """@p text with backslash continuations joined, one command line per entry."""
    return re.sub(r"\\\n\s*", " ", text).splitlines()


def install_problems(line: str) -> list[str] | None:
    """What lets @p line install around the lock, or None if it installs nothing.

    An install may name a local wheel, the project in editable form, or a
    requirements file; any other positional argument is a package name, and a
    package name carries a version the lock does not own. `uv sync` must be
    `--locked`, and `uv run` `--locked` or `--no-sync` after one, so a stale lock
    fails rather than being re-resolved.
    """
    command = re.split(r"\|\||&&|[|;]", line)
    problems: list[str] = []
    seen = False
    for segment in command:
        try:
            words = shlex.split(segment, comments=True)
        except ValueError:
            words = segment.split()
        for i, word in enumerate(words):
            if word == "sync" and i > 0 and words[i - 1] == "uv":
                seen = True
                if "--locked" not in words[i + 1 :]:
                    problems.append("`uv sync` without --locked re-resolves instead of failing")
            if word == "run" and i > 0 and words[i - 1] == "uv":
                seen = True
                if not {"--locked", "--no-sync"} & set(words[i + 1 :]):
                    problems.append("`uv run` without --locked or --no-sync may re-resolve")
            if word == "install" and i > 0 and words[i - 1] == "pip":
                seen = True
                problems.extend(_install_argument_problems(words[i + 1 :]))
    return problems if seen else None


def _install_argument_problems(args: list[str]) -> list[str]:
    problems: list[str] = []
    k = 0
    while k < len(args):
        arg = args[k]
        name, eq, inline = arg.partition("=")
        if name in VALUE_OPTIONS:
            value = inline if eq else (args[k + 1] if k + 1 < len(args) else "")
            k += 1 if eq else 2
            want = VALUE_OPTIONS[name]
            if want is not None and value != want:
                problems.append(f"`{name} {value}` installs something other than {want!r}")
            continue
        k += 1
        if arg.startswith("-"):
            continue
        if not arg.endswith(".whl"):
            problems.append(f"`{arg}` is installed by name, outside the lock")
    return problems


def check_agreement(label: str, found: dict[str, list], failures: list[str]) -> None:
    """Fail unless every file that carries @p label carries the same value."""
    carriers = {name: value for name, value in found.items() if value}
    if not carriers:
        failures.append(
            f"{label}: no file carries it at all. Either the invocation moved and this "
            "check is now blind, or the gate was deleted."
        )
        return
    distinct = {tuple(value) for value in carriers.values()}
    if len(distinct) == 1:
        return
    failures.append(
        f"{label} disagrees across the files that carry it. CI does not call the "
        f"Makefile, so the narrowest of these is the gate that actually runs:\n"
        + "\n".join(f"    {name}: {value}" for name, value in sorted(carriers.items()))
    )


def main() -> int:
    sources = {p.relative_to(REPO_ROOT).as_posix(): p.read_text() for p in (MAKEFILE, *WORKFLOWS)}
    failures: list[str] = []

    check_agreement(
        "the ruff scope", {name: ruff_targets(text) for name, text in sources.items()}, failures
    )
    check_agreement(
        "the clang-format pathspec",
        {name: sorted(set(clang_format_specs(text))) for name, text in sources.items()},
        failures,
    )

    checked = 0
    for path in ALL_WORKFLOWS:
        name = path.relative_to(REPO_ROOT).as_posix()
        for line in shell_lines(path.read_text()):
            problems = install_problems(line)
            if problems is None:
                continue
            checked += 1
            failures.extend(f"{name}: {p}:\n    {line.strip()}" for p in problems)
    if not checked:
        failures.append(
            "no workflow installs anything, so the install check verified nothing. "
            "The install lines moved and the parsing above is stale."
        )

    if failures:
        print("\n".join(f"[FAIL] {f}" for f in failures))
        return 1
    ruff = ruff_targets(sources["Makefile"])
    specs = sorted(set(clang_format_specs(sources["Makefile"])))
    print(
        f"lint scope agrees across {len(sources)} file(s): ruff {ruff[0]}, "
        f"clang-format {' '.join(specs[0])}"
    )
    print(f"{checked} workflow install line(s) take their versions from the lock")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
