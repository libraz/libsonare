"""Check that every public C-ABI header compiles as a translation unit's only include.

A header that names a sibling's type without including that sibling still builds
everywhere in this tree, because the ``sonare_c.h`` umbrella pulls the sibling in
first: the dependency is satisfied by include order rather than declared by the
header that has it.  Nothing in the tree can notice.  The consumer that does is
the one outside it -- a binding, a downstream C project, an IDE parsing one file
-- and by then the header is installed.

The check compiles rather than reads.  A textual scan would have to model which
names a declaration needs and where each one is defined, which is the same work
the compiler already does and gets wrong differently; a probe TU whose only
include is the header under test asks the question directly.

Two language modes, because a header's ``#ifdef __cplusplus`` section -- the
layout ``static_assert``s the POD bindings depend on -- is skipped entirely by a
C probe.  A header can be self-contained in C and not in C++.

**An unused include is not a finding.**  The scan reports a header that fails to
compile, never a header that includes more than it needs, so it cannot degrade
into an include-line detector: removing a redundant include leaves the report
unchanged, and only removing a load-bearing one moves it.

``--floor`` pins how many headers the scan compiles at all.  A glob that stopped
matching would otherwise report a clean tree over an empty set.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_HEADER_DIR = ROOT / "include" / "sonare"

# The C ABI is the surface a C consumer compiles, and the C++ mode is what the
# in-tree sources and the layout static_asserts see.  A header owes both.
LANGUAGES = (
    ("c", "cc", "-std=c11", "probe.c", "int main(void) { return 0; }"),
    ("c++", "c++", "-std=c++17", "probe.cpp", "int main() { return 0; }"),
)


@dataclass
class Finding:
    """One header that does not compile on its own, in one language."""

    header: str
    language: str
    detail: str

    def as_line(self) -> str:
        return f"[{self.language}] {self.header}  {self.detail}"


@dataclass
class Report:
    findings: list[Finding]
    headers: int
    compiles: int


def _first_error(stderr: str) -> str:
    """The compiler's first error line, trimmed to something reportable."""
    for line in stderr.splitlines():
        if ": error:" in line or ": fatal error:" in line:
            return " ".join(line.split())[:160]
    return " ".join(stderr.split())[:160] or "compilation failed without a diagnostic"


class CompilerMissing(RuntimeError):
    """The probe compiler is absent, so every header would report as broken."""


def compile_alone(header_dir: Path, name: str, language: tuple) -> str | None:
    """Compile a TU whose only include is ``name``; the failure detail, or None."""
    _, compiler, std, probe_name, body = language
    include_root = header_dir.parent
    with tempfile.TemporaryDirectory() as tmp:
        probe = Path(tmp) / probe_name
        probe.write_text(
            f"#include <{header_dir.name}/{name}>\n{body}\n",
            encoding="utf-8",
        )
        try:
            result = subprocess.run(
                [compiler, std, "-I", str(include_root), "-fsyntax-only", str(probe)],
                capture_output=True,
                check=False,
                text=True,
            )
        except OSError as error:
            # An absent compiler would otherwise fail every header at once and
            # read as a tree-wide defect.  A scan that cannot run says so.
            raise CompilerMissing(f"{compiler}: {error}") from error
    return None if result.returncode == 0 else _first_error(result.stderr)


def audit(header_dir: Path, languages: tuple = LANGUAGES) -> Report:
    """Compile every header under ``header_dir`` on its own, in every language."""
    findings: list[Finding] = []
    headers = sorted(header_dir.glob("*.h"))
    compiles = 0
    for path in headers:
        for language in languages:
            compiles += 1
            detail = compile_alone(header_dir, path.name, language)
            if detail is not None:
                findings.append(Finding(path.name, language[0], detail))
    return Report(findings, len(headers), compiles)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header-dir", type=Path, default=DEFAULT_HEADER_DIR)
    parser.add_argument(
        "--floor",
        type=int,
        default=0,
        help="minimum headers the scan must compile; guards against a glob that "
        "stopped matching reporting a clean tree",
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    try:
        report = audit(args.header_dir)
    except CompilerMissing as error:
        print(f"\nthe probe compiler is unavailable ({error}), so this scan "
              "certifies nothing", file=sys.stderr)
        return 2

    if args.json:
        print(
            json.dumps(
                {
                    "headers": report.headers,
                    "compiles": report.compiles,
                    "findings": [f.__dict__ for f in report.findings],
                },
                indent=2,
            )
        )
    else:
        print(f"headers compiled alone: {report.headers}")
        print(f"  translation units compiled: {report.compiles}")
        print(f"  headers that need a sibling included first: {len(report.findings)}")
        if report.findings:
            print(
                "\nThese headers do not compile as a translation unit's only include:",
                *(f"  {finding.as_line()}" for finding in report.findings),
                sep="\n",
                file=sys.stderr,
            )

    if report.headers < args.floor:
        print(
            f"\ncompiled {report.headers} headers, below the floor of {args.floor}: "
            "the header glob stopped matching, so a clean report here would "
            "certify nothing",
            file=sys.stderr,
        )
        return 2
    return 1 if report.findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
