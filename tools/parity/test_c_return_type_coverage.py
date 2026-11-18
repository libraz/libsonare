"""Regression test for the reach of the C-API extractor's return-type list.

``extractors/c_api.py`` matches a declaration by its return type, against a
closed tuple. A public C function whose return type is missing from that tuple
is not extracted, and an unextracted symbol produces no finding of any kind --
it reads exactly like a symbol every surface exposes. Twelve return types the
public headers spell were absent from the tuple at once, which hid ten symbols
from every comparison the checker makes.

So the tuple is held against the headers rather than against itself: every
return type a public declaration actually uses has to be in it. A control asserts
the scan finds declarations at all, since a header walk that matched nothing
would pass the first half vacuously.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_c_return_type_coverage.py
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from extractors.c_api import _RETURN_TYPES, extract

_ROOT = _HERE.parent.parent
_INCLUDE_DIR = _ROOT / "include" / "sonare"

# The release the API ledger compares against, and declarations in it whose
# return types the tip no longer spells. Read with the tip's closed list alone,
# they were missing from the baseline and every ledger row for them failed.
_BASELINE_TAG = "v1.10.0"
_BASELINE_ONLY_RETURNS = {
    "sonare_eq_create": "SonareEq*",
    "sonare_last_diagnostic_severity": "SonareDiagnosticSeverity",
    "sonare_sample_bank_create": "SonareSampleBank*",
    "sonare_streaming_mastering_chain_create": "SonareStreamingMasteringChain*",
    "sonare_streaming_mastering_chain_create_ex": "SonareStreamingMasteringChain*",
    "sonare_streaming_retune_create": "SonareStreamingRetune*",
}

# A declaration at column 0: the return type, then a sonare_ name and its
# argument list. Deliberately looser than the extractor's own pattern, which is
# what the extractor is being measured against. The pointer star is matched
# explicitly rather than folded into the identifier character class -- a class
# that cannot hold `*` silently skips every pointer-returning declaration, which
# is four of the sixteen return types in these headers.
_DECL_RE = re.compile(
    r"^(?P<ret>(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\*?)\s+sonare_[A-Za-z0-9_]+\s*\(",
    re.MULTILINE,
)

# Keywords that can open a line and be followed by a sonare_ name without being a
# return type (a forward declaration, a macro body).
_NON_RETURN_PREFIXES = frozenset({"typedef", "return", "struct", "enum", "union"})


def _declared_return_types() -> dict[str, list[str]]:
    """Maps each return type the headers spell to the files that spell it."""
    found: dict[str, list[str]] = {}
    for header in sorted(_INCLUDE_DIR.glob("*.h")):
        for match in _DECL_RE.finditer(header.read_text(encoding="utf-8")):
            ret = " ".join(match.group("ret").split()).replace(" *", "*")
            if ret.split()[0] in _NON_RETURN_PREFIXES:
                continue
            found.setdefault(ret, []).append(header.name)
    return found


def test_every_declared_return_type_is_extracted() -> None:
    declared = _declared_return_types()
    missing = {ret: files for ret, files in declared.items() if ret not in _RETURN_TYPES}
    assert not missing, (
        "public C declarations use return types the parity extractor does not "
        f"match, so their symbols are never compared: {missing}"
    )


def test_the_header_scan_reaches_declarations() -> None:
    # The control: without this, an _INCLUDE_DIR that moved or a pattern that
    # stopped matching would make the assertion above pass by finding nothing.
    declared = _declared_return_types()
    # One of each shape the pattern has to handle: a plain identifier, a
    # qualified pointer, and an opaque-handle pointer. A pattern that drops any
    # of the three passes the assertion above by not looking.
    for shape in ("SonareError", "const char*", "SonareMixer*"):
        assert shape in declared, f"header scan found no {shape} declaration"
    assert len(declared) >= 16, f"header scan found only {len(declared)} return types"


def test_a_return_type_only_the_read_tree_spells_is_extracted() -> None:
    with tempfile.TemporaryDirectory(prefix="sonare-c-ret-") as d:
        header = Path(d) / "include" / "sonare" / "sonare_c.h"
        header.parent.mkdir(parents=True)
        header.write_text(
            "typedef struct SonareGone SonareGone;\n"
            "SonareGone *sonare_gone_create(int sample_rate);\n"
            "SonareGoneKind sonare_gone_kind(const SonareGone* gone);\n",
            encoding="utf-8",
        )
        returns = {f.raw_name: f.returns for f in extract(Path(d)).functions}
    assert returns == {
        "sonare_gone_create": "SonareGone*",
        "sonare_gone_kind": "SonareGoneKind",
    }, returns


def test_the_baseline_release_extracts_its_own_return_types() -> bool:
    """Returns False (skipped) when the tag is not in this clone."""
    probe = subprocess.run(
        ["git", "-C", str(_ROOT), "rev-parse", "-q", "--verify", f"refs/tags/{_BASELINE_TAG}"],
        capture_output=True,
        check=False,
    )
    if probe.returncode != 0:
        return False
    with tempfile.TemporaryDirectory(prefix="sonare-c-ret-tag-") as d:
        archive = subprocess.run(
            ["git", "-C", str(_ROOT), "archive", _BASELINE_TAG, "include"],
            capture_output=True,
            check=True,
        )
        subprocess.run(["tar", "xf", "-", "-C", d], input=archive.stdout, check=True)
        returns = {f.raw_name: f.returns for f in extract(Path(d)).functions}
    found = {name: returns.get(name) for name in _BASELINE_ONLY_RETURNS}
    assert found == _BASELINE_ONLY_RETURNS, found
    return True


if __name__ == "__main__":
    test_every_declared_return_type_is_extracted()
    test_the_header_scan_reaches_declarations()
    test_a_return_type_only_the_read_tree_spells_is_extracted()
    if not test_the_baseline_release_extracts_its_own_return_types():
        print(f"c return-type coverage: {_BASELINE_TAG} not in this clone, baseline case skipped")
    print(f"c return-type coverage: OK ({len(_declared_return_types())} return types declared)")
