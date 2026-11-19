"""Hold the JS facades to entry-named refusals: a ratchet on the old shapes.

A Node or WASM facade refusal is raised bare through the helpers in
``internal/entry.ts`` (``refuse`` / ``refuseType`` / ``invalidState`` /
``codedError``) and named by the entry the consumer called (``@entryClass``,
``withEntry`` or the native wrapper). Two spellings predate that and are counted
here per surface: a raw ``new RangeError(`` / ``new TypeError(`` /
``new SonareError(`` outside ``entry.ts``, and a helper parameter that carries
the caller's name as text (``fnName`` and its siblings). A third kind,
``value-echo``, counts string literals stating ``, got <value>``, ``(got <value>)``,
``out of [supported ]range`` or ``a finite number`` instead of the predicate the value
failed, and ``unknown-key`` counts
the spellings of an undeclared-key refusal older than ``unknown key <path> (expected
...)``, and ``array-scan`` counts the retired ``non-finite value`` word of an array-scan
refusal, which reads ``<key> contains a non-finite sample at index <i>``. All three are also
counted over the native readers (``bindings/node/src/addon`` and ``src/wasm``) as the
``native`` surface, so the TS and native halves are two lines of one scan; ``array-scan``
is counted over the rest of ``src`` as the ``core`` surface too.

The counts in ``facade_refusal_ratchet.json`` are a line, not a target: a count
above it fails, and a count below it fails too until the file is rewritten with
``--write``, so a migration lowers the line in the same commit and it cannot
drift back up. Comments are stripped before matching; a scan reading no file
fails, so a moved tree cannot pass as clean.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = Path(__file__).resolve().parent / "facade_refusal_ratchet.json"
# surface -> (source roots, file suffixes, counted kinds)
TS_KINDS = ("raw-error", "fn-name-param", "value-echo", "unknown-key", "array-scan")
SURFACES: dict[str, tuple[tuple[str, ...], tuple[str, ...], tuple[str, ...]]] = {
    "node": (("bindings/node/src",), (".ts",), TS_KINDS),
    "wasm": (("bindings/wasm/src",), (".ts",), TS_KINDS),
    "native": (
        ("bindings/node/src/addon", "src/wasm"),
        (".cpp", ".h"),
        ("value-echo", "unknown-key", "array-scan"),
    ),
    "core": (("src",), (".cpp", ".h"), ("array-scan",)),
}
# Trees a surface's roots reach that another surface already counts.
SKIP_UNDER = {"core": ("wasm/",)}
# The helpers' own home, and a generated table.
EXEMPT = {"internal/entry.ts", "internal/native_entries.ts"}
FN_PARAM_NAMES = ("fnName", "fn", "fnLabel", "entryName", "caller")

RAW_ERROR = re.compile(r"\bnew\s+(?:RangeError|TypeError|SonareError)\s*\(")
FN_PARAM = re.compile(
    r"[(,]\s*(?:" + "|".join(FN_PARAM_NAMES) + r")\s*\??\s*:\s*(?:string|RequestFn)\b"
)
KINDS = {"raw-error": RAW_ERROR, "fn-name-param": FN_PARAM}
# A refusal states the predicate, never the value it refused or a bare "out of range";
# "a finite number" restates the type where the value predicate is "must be finite".
VALUE_ECHO = re.compile(r"[,(]\s*got\b|out of (?:supported )?range|a finite number", re.IGNORECASE)
# An undeclared key reads "unknown key <path> (expected ...)"; these are the spellings before it.
UNKNOWN_KEY = re.compile(r"unknown (?:request key|option|config key)\b", re.IGNORECASE)
# An array scan reads "<key> contains a non-finite sample at index <i>"; "value" is the retired word.
ARRAY_SCAN = re.compile(r"non-finite value", re.IGNORECASE)
LITERAL_KINDS = {"value-echo": VALUE_ECHO, "unknown-key": UNKNOWN_KEY, "array-scan": ARRAY_SCAN}
HINTS = {
    "raw-error": "raise the refusal through refuse/refuseType/invalidState/codedError",
    "fn-name-param": "name it with the entry, not a passed-in name",
    "value-echo": 'state the predicate ("<key> must be ...") without echoing the value',
    "unknown-key": "refuse it with refuseUnknownKey: unknown key <path> (expected a, b, c)",
    "array-scan": "say <key> contains a non-finite sample at index <i>",
}


def strip_comments(text: str) -> str:
    """``text`` with comments and quoted-string contents blanked.

    Template literals are kept whole, since their ``${}`` holes are code.
    """
    out: list[str] = []
    i, n = 0, len(text)
    quote = ""
    while i < n:
        c = text[i]
        if quote:
            keep = quote == "`" or c == quote
            if c == "\\" and i + 1 < n:
                if quote == "`":
                    out.append(text[i : i + 2])
                i += 2
                continue
            if keep:
                out.append(c)
            if c == quote:
                quote = ""
            i += 1
        elif c in "'\"`":
            quote = c
            out.append(c)
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            stop = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, stop))
            i = stop
        else:
            out.append(c)
            i += 1
    return "".join(out)


def string_literals(text: str, cpp: bool = False) -> list[str]:
    """Contents of the string literals in ``text``, comments skipped.

    TS reads ``'``, ``"`` and template literals; C++ reads ``"`` literals and
    steps over ``'`` character literals.
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
        elif c == '"' or (not cpp and c in "'`") or (cpp and c == "'"):
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            if not (cpp and c == "'"):
                out.append(text[i + 1 : j])
            i = j + 1
        else:
            i += 1
    return out


def count_text(text: str, kinds: tuple[str, ...] = TS_KINDS, cpp: bool = False) -> Counter:
    """Occurrences of each of ``kinds`` in one source text, comments excluded."""
    found: Counter = Counter()
    if "raw-error" in kinds or "fn-name-param" in kinds:
        code = strip_comments(text)
        for kind in kinds:
            if kind in KINDS:
                found[kind] = len(KINDS[kind].findall(code))
    literal_kinds = [kind for kind in kinds if kind in LITERAL_KINDS]
    if literal_kinds:
        literals = string_literals(text, cpp)
        for kind in literal_kinds:
            found[kind] = sum(len(LITERAL_KINDS[kind].findall(s)) for s in literals)
    return found


def scan(root: Path) -> dict[str, dict]:
    """Per surface: files scanned, total per kind, and per-file counts."""
    result: dict[str, dict] = {}
    for surface, (rels, suffixes, kinds) in SURFACES.items():
        totals: Counter = Counter({kind: 0 for kind in kinds})
        per_file: dict[str, Counter] = {}
        scanned = 0
        cpp = ".cpp" in suffixes
        for rel in rels:
            base = root / rel
            for path in sorted(p for p in base.rglob("*") if p.suffix in suffixes):
                name = path.relative_to(base).as_posix()
                if path.name.endswith(".d.ts") or name in EXEMPT:
                    continue
                if name.startswith(SKIP_UNDER.get(surface, ())):
                    continue
                scanned += 1
                counts = count_text(path.read_text(encoding="utf-8"), kinds, cpp)
                totals.update(counts)
                if any(counts.values()):
                    per_file[path.relative_to(root).as_posix()] = counts
        result[surface] = {"scanned": scanned, "totals": dict(totals), "files": per_file}
    return result


def evaluate(found: dict[str, dict], baseline: dict[str, dict]) -> list[str]:
    """Every way ``found`` departs from the recorded line."""
    errors: list[str] = []
    for surface, data in found.items():
        if data["scanned"] == 0:
            errors.append(f"{surface}: scanned no source under {', '.join(SURFACES[surface][0])}")
            continue
        recorded = baseline.get(surface, {})
        for kind, count in data["totals"].items():
            line = recorded.get(kind)
            if line is None:
                errors.append(f"{surface} {kind}: no recorded line; run with --write")
            elif count > line:
                worst = sorted(
                    ((c[kind], f) for f, c in data["files"].items() if c[kind]), reverse=True
                )[:8]
                where = ", ".join(f"{f} {c}" for c, f in worst)
                errors.append(
                    f"{surface} {kind}: {count} exceeds the recorded {line}; {HINTS[kind]} ({where})"
                )
            elif count < line:
                errors.append(
                    f"{surface} {kind}: {count} is below the recorded {line}; lower the line "
                    f"with --write in the same change"
                )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write", action="store_true", help="record the current counts")
    args = parser.parse_args(argv)
    found = scan(ROOT)
    for surface, data in found.items():
        totals = " ".join(f"{k}={v}" for k, v in data["totals"].items())
        print(f"facade refusal ratchet: {surface} scanned={data['scanned']} {totals}")
    if args.write:
        if any(data["scanned"] == 0 for data in found.values()):
            print("refusing to record a scan that read no file", file=sys.stderr)
            return 1
        BASELINE.write_text(
            json.dumps({s: d["totals"] for s, d in found.items()}, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"wrote {BASELINE.relative_to(ROOT)}")
        return 0
    baseline = json.loads(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else {}
    errors = evaluate(found, baseline)
    for error in errors:
        print(f"  FAIL {error}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
