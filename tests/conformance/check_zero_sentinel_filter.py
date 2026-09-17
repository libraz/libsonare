"""Keep the "0 selects the library default" sentinel from being spelled ``> 0``.

An optional scalar whose documented ``0`` means "keep the library default" has
exactly one correct test: ``== 0``.  Spelled ``> 0`` the comparison stops being
a sentinel test and becomes a filter, and everything it filters out -- a
negative, a NaN -- is promoted to the default instead of reaching the validator
that would have refused it.  The caller asked for something invalid and got a
successful call carrying a value they did not choose, which is the one outcome a
boundary is there to prevent.  ``src/util/zero_is_default.h`` exists to remove
the idiom and names it in its own docblock.

THE SHAPE, NOT THE HELPER
-------------------------
A search anchored on ``ZeroIsDefault`` returns the sites that are already
correct, and a search anchored on a field name returns one field.  Only the raw
comparison returns the population, so that is what is scanned:

* **The statement form.**  ``if (x > 0) cfg.field = x;`` -- an ``if`` whose
  then-branch assigns the tested expression itself to something else, braced or
  not.  The braced spelling is not a variant worth skipping: six sites in these
  trees are written that way, so a pattern that stopped at the bare statement
  would have reported none of them.
* **The conditional form.**  ``x > 0 ? x : fallback`` -- the same substitution
  written as an expression.

WHAT MAKES IT A SENTINEL FILTER RATHER THAN A POSITIVITY TEST
-------------------------------------------------------------
The discriminator is that **the tested expression is itself the value assigned**.
An ordinary ``> 0`` guards other work with the comparison -- it loops, it
early-returns, it sizes a buffer -- and never re-assigns the thing it compared.
That is the whole difference, and it is what keeps the report off the several
hundred positivity tests these trees also contain.

Two refinements the shape needs to stay precise:

* A conditional whose fallback is the literal ``0`` is a clamp, not a default
  selection: the "default" is the sentinel itself, so nothing the caller chose
  is being replaced.  ``n > 0 ? n : 0`` is not reported.
* An assignment back to the tested expression would be a no-op, so the target
  has to differ from the value.

KNOWN BLIND SPOTS
-----------------
The scan reads the spelling, so it cannot see a filter written with ``>=``, one
whose value is repeated in a different spelling on the two sides, or one whose
then-branch does other work before the assignment.  None of those are reachable
by tightening this pattern -- they need the value flowing through a parse -- so
the floor below is what keeps a scan that has quietly stopped matching from
certifying itself.  Two empty sets agree perfectly.  The ``>=`` spelling was
measured on these trees and is currently empty, which is why closing it was not
worth the precision it would have cost.

THE RECORDS ARE A RATCHET
-------------------------
``zero_sentinel_records.json`` carries the sites that have not been converted,
each with why it is still there.  It is not a blessing: it is the line the
population may not cross.  A site that is neither converted nor recorded is a
new one, and that is the case this guard exists for -- the helper's adoption has
been growing alongside the raw shape rather than instead of it.  A record that
matches nothing is itself an error, because a stale one keeps asserting a
reviewed decision about a field name and the next field to take it inherits the
blessing unexamined.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RECORDS = Path(__file__).resolve().parent / "zero_sentinel_records.json"

# The trees that read a caller's scalar and hand it to a core config. The core
# itself is not scanned: a `> 0` there is the core's own contract, not a
# boundary pre-empting one.
TREES = (
    Path("src") / "c_api",
    Path("src") / "wasm" / "bindings",
    Path("bindings") / "node" / "src" / "addon",
)

SUFFIXES = (".cpp", ".h", ".cc", ".hpp")

# A C++ value expression: an identifier, optionally walked through members and a
# single string-keyed lookup. Deliberately not a general expression -- a
# sentinel is read from one named thing, and widening this is how a scan starts
# matching arithmetic that only looks like the shape.
_VALUE = r"[A-Za-z_][A-Za-z0-9_]*(?:(?:\.|->)[A-Za-z0-9_]+)*(?:\[\"[A-Za-z0-9_]+\"\])?"

# `> 0`, in every float and integer spelling the trees use.
_ZERO = r"0(?:\.0*[fF]?)?"

_STATEMENT = re.compile(
    r"if \((?P<value>" + _VALUE + r") > " + _ZERO + r"\)\s*(?:\{\s*)?"
    r"(?P<target>" + _VALUE + r") = (?P=value);"
)

_CONDITIONAL = re.compile(
    r"(?P<value>" + _VALUE + r") > " + _ZERO + r"\s*\?\s*(?P=value)\s*:\s*(?P<fallback>[^;,)]+)"
)


def _blank(match: re.Match[str]) -> str:
    """Replace a matched span with spaces, keeping its newlines and its length."""
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


_LEXICAL = re.compile(
    r"""//[^\n]*
      | /\*.*?\*/
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)


def strip_lexical(text: str) -> str:
    """Comments and string literals blanked, offsets and line numbers preserved.

    A comment that quotes the idiom while explaining why it is gone would
    otherwise be reported as the idiom, and the fixed sites do exactly that.
    """
    return _LEXICAL.sub(_blank, text)


def _display(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


class Site:
    """One sentinel filter, with everything a record is keyed on."""

    def __init__(self, path: Path, line: int, value: str, target: str, form: str) -> None:
        self.path = path
        self.line = line
        self.value = value
        self.target = target
        self.form = form

    @property
    def display(self) -> str:
        return f"{_display(self.path)}:{self.line}"

    @property
    def text(self) -> str:
        if self.form == "conditional":
            return f"{self.value} > 0 ? {self.value} : {self.target}"
        return f"if ({self.value} > 0) {self.target} = {self.value};"

    @property
    def key(self) -> tuple[str, str]:
        return (_display(self.path), self.value)


class Scan:
    """The sentinel filters in the boundary trees, and the population's size."""

    def __init__(self, root: Path = ROOT, trees: tuple[Path, ...] = TREES) -> None:
        self.root = root
        self.trees = trees
        self.sites: list[Site] = []
        self.comparisons = 0
        self.helper_calls = 0
        self._run()

    def _files(self) -> list[Path]:
        found: list[Path] = []
        for tree in self.trees:
            base = self.root / tree
            if not base.is_dir():
                continue
            found.extend(p for p in base.rglob("*") if p.is_file() and p.suffix in SUFFIXES)
        return sorted(found)

    def _run(self) -> None:
        comparison = re.compile(r"> " + _ZERO + r"[\s)?]")
        helper = re.compile(r"\bZeroIsDefault\(")
        for path in self._files():
            source = strip_lexical(path.read_text(encoding="utf-8"))
            self.comparisons += len(comparison.findall(source))
            self.helper_calls += len(helper.findall(source))
            for match in _STATEMENT.finditer(source):
                if match.group("target") == match.group("value"):
                    continue
                line = source.count("\n", 0, match.start()) + 1
                self.sites.append(
                    Site(path, line, match.group("value"), match.group("target"), "statement")
                )
            for match in _CONDITIONAL.finditer(source):
                fallback = " ".join(match.group("fallback").split())
                # A fallback of literal 0 clamps rather than selecting a default.
                if re.fullmatch(_ZERO, fallback):
                    continue
                line = source.count("\n", 0, match.start()) + 1
                self.sites.append(Site(path, line, match.group("value"), fallback, "conditional"))


class Records:
    """The recorded-unconverted data, and which of its entries matched anything."""

    def __init__(self, data: dict) -> None:
        self.entries = data.get("sites", [])
        self.used: set[str] = set()

    def covers(self, site: Site) -> bool:
        for entry in self.entries:
            if (entry["file"], entry["value"]) == site.key:
                self.used.add(f"{entry['file']}:{entry['value']}")
                return True
        return False

    def unused(self) -> list[str]:
        every = [f"{e['file']}:{e['value']}" for e in self.entries]
        return sorted(name for name in every if name not in self.used)


def _self_check(scan: Scan, floor: dict) -> list[str]:
    """The mandatory one. A scan that matches nothing agrees with every record.

    Both halves are pinned: the raw comparisons the scan reads past prove it is
    still reading the trees at all, and the helper's own call sites prove the
    converted population has not been reverted wholesale.
    """
    measured = {
        "sites": len(scan.sites),
        "comparisons": scan.comparisons,
        "helper_calls": scan.helper_calls,
    }
    return [
        f"{name}: found {measured.get(name, 0)}, floor is {minimum} -- the scan has "
        "stopped matching, and an empty population agrees with everything"
        for name, minimum in floor.items()
        if measured.get(name, 0) < minimum
    ]


def evaluate(scan: Scan, records: Records, floor: dict) -> list[tuple[str, list[str]]]:
    """Every failure class, as (heading, lines).

    Separated from ``main`` so the self-tests can trip one class at a time -- a
    class asserted through a reimplementation of this would only agree with
    itself.
    """
    failures: list[tuple[str, list[str]]] = []

    self_check = _self_check(scan, floor)
    if self_check:
        failures.append(("The scan no longer finds the population it is sized for", self_check))

    unrecorded = [site for site in scan.sites if not records.covers(site)]
    if unrecorded:
        failures.append(
            (
                ("These test the zero-default sentinel with `> 0`, so a negative or "
                "non-finite request is promoted to the default instead of reaching "
                "the validator that would refuse it. Route them through "
                "sonare::ZeroIsDefault, or test the sentinel with `== 0` where the "
                "value is an integer"),
                [f"  {site.display}  {site.text}" for site in unrecorded],
            )
        )

    stale = records.unused()
    if stale:
        failures.append(
            (
                ("These records matched nothing. A record that suppresses nothing "
                "still asserts a reviewed decision about a field, so the next one "
                "to take that name inherits the blessing unexamined -- delete the "
                "record in the change that converts its site"),
                [f"  {name}" for name in stale],
            )
        )
    return failures


def load_records(path: Path = RECORDS) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--records", type=Path, default=RECORDS)
    parser.add_argument(
        "--list", action="store_true", help="print every site found, converted or not"
    )
    args = parser.parse_args()

    data = load_records(args.records)
    records = Records(data)
    scan = Scan(args.root)

    print(f"sentinel filters found: {len(scan.sites)}")
    print(f"`> 0` comparisons read past: {scan.comparisons}")
    print(f"ZeroIsDefault call sites: {scan.helper_calls}")
    if args.list:
        for site in scan.sites:
            print(f"  {site.display}  {site.text}")

    failures = evaluate(scan, records, data["floor"])
    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
