"""Fail when a header an ABI counter governs changed layout since the last release tag.

Each counter in ``COUNTERS`` names the headers whose declarations it versions. For
every counter whose value still equals the tag's, the declarations those headers
held at the tag must all survive unchanged in the tree: a struct, union, typedef,
function signature or ``#define`` constant that was modified or removed means a
caller compiled against the tag now disagrees with the library, so the bump is owed.
Additions never count, nor do comments, formatting, inline function bodies or the
counters themselves. An enum may grow at its end; reordering, renumbering or
removing an enumerator counts. A counter absent from the tag is new and passes.

Usage: python3 tools/abi/check_abi_bumps.py [--tag <ref>]
(default tag: ``git describe --tags --abbrev=0 main``)
"""

from __future__ import annotations

import fnmatch
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT


@dataclass(frozen=True)
class Counter:
    name: str
    source: str
    pattern: str
    headers: tuple[str, ...]


# Subsystem counters claim their headers first; the feature counter governs every public
# header no other counter claims. Mirrors the counter list in check_abi_versions.py.
COUNTERS: tuple[Counter, ...] = (
    Counter(
        "project",
        "include/sonare/sonare_c_project.h",
        r"#define\s+SONARE_PROJECT_ABI_VERSION\s+(\w+)",
        ("include/sonare/sonare_c_project.h", "include/sonare/sonare_c_project_*.h"),
    ),
    Counter(
        "voice_changer",
        "include/sonare/sonare_c_voice_changer.h",
        r"#define\s+SONARE_VOICE_CHANGER_ABI_VERSION\s+(\w+)",
        ("include/sonare/sonare_c_voice_changer.h",),
    ),
    Counter(
        "acoustic",
        "include/sonare/sonare_c_acoustic.h",
        r"#define\s+SONARE_ACOUSTIC_ABI_VERSION\s+(\w+)",
        ("include/sonare/sonare_c_acoustic.h",),
    ),
    Counter(
        "vocal_edit",
        "include/sonare/sonare_c_vocal_edit.h",
        r"#define\s+SONARE_VOCAL_EDIT_ABI_VERSION\s+(\w+)",
        ("include/sonare/sonare_c_vocal_edit.h",),
    ),
    Counter(
        "vocal_project",
        "include/sonare/sonare_c_vocal_project.h",
        r"#define\s+SONARE_VOCAL_PROJECT_ABI_VERSION\s+(\w+)",
        ("include/sonare/sonare_c_vocal_project.h",),
    ),
    Counter(
        "engine",
        "src/rt/command.h",
        r"kEngineAbiVersion\s*=\s*(\w+)",
        (
            "src/rt/command.h",
            "include/sonare/sonare_c_engine.h",
            "include/sonare/sonare_c_types_engine.h",
        ),
    ),
    Counter(
        "feature",
        "include/sonare/sonare_c_types.h",
        r"#define\s+SONARE_FEATURE_ABI_VERSION\s+(\w+)",
        ("include/sonare/*.h",),
    ),
)

_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\w+|::|->|\S')
_DEFINE = re.compile(r"#\s*define\s+(\w+)(\(?)\s*(.*)")
_ABI_NAME = re.compile(r"(ABI_VERSION|AbiVersion)$")


def _tokens(text: str) -> list[str]:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    text = text.replace("\\\n", " ")
    out: list[str] = []
    for line in text.split("\n"):
        stripped = line.strip()
        if not stripped.startswith("#"):
            out += _TOKEN.findall(line)
            continue
        define = _DEFINE.match(stripped)
        if define and not define.group(2) and define.group(3):
            name, value = define.group(1), define.group(3)
            if not _ABI_NAME.search(name):
                out += ["#define", name, *_TOKEN.findall(value), ";"]
    return out


def _statements(tokens: list[str]) -> list[list[str]]:
    """Top-level declarations, looking through namespace and extern "C" blocks."""
    out: list[list[str]] = []
    cur: list[str] = []
    depth = transparent = 0
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if depth == 0 and not cur:
            if t == "namespace":
                while tokens[i] != "{":
                    i += 1
                transparent += 1
                i += 1
                continue
            if t == "extern" and tokens[i + 1 : i + 3] == ['"C"', "{"]:
                transparent += 1
                i += 3
                continue
            if t == "}" and transparent:
                transparent -= 1
                i += 1
                continue
        cur.append(t)
        if t == "{":
            depth += 1
        elif t == "}":
            depth -= 1
            head = cur[: cur.index("{")]
            if depth == 0 and "(" in head and "=" not in head:
                out.append([*head, "{", "}"])
                cur = []
        elif t == ";" and depth == 0:
            out.append(cur)
            cur = []
        i += 1
    return out


def _is_counter(statement: list[str]) -> bool:
    if "=" not in statement:
        return False
    return bool(_ABI_NAME.search(statement[statement.index("=") - 1]))


def _enum(statement: list[str]) -> tuple[str, str, list[str]] | None:
    """(name, head, enumerators) of an enum declaration, else None."""
    if "{" not in statement or "enum" not in statement[: statement.index("{")]:
        return None
    open_ = statement.index("{")
    close = len(statement) - 1 - statement[::-1].index("}")
    head = statement[:open_]
    if statement[0] == "typedef":
        name = next(t for t in reversed(statement[close:]) if re.match(r"\w+$", t))
    else:
        after = head[head.index("enum") + 1 :]
        name = next((t for t in after if t not in ("class", "struct")), "")
    items = " ".join(statement[open_ + 1 : close]).split(",")
    return name, " ".join(head), [x.strip() for x in items if x.strip()]


def _opaque(statement: list[str]) -> bool:
    """``typedef struct Foo Foo;``: a handle callers only ever hold by pointer."""
    return len(statement) == 5 and statement[:2] == ["typedef", "struct"] and statement[4] == ";"


def _parsed(texts: list[str]) -> tuple[list[list[str]], set[str]]:
    statements = [
        statement
        for text in texts
        for statement in _statements(_tokens(text))
        if statement and statement[0] != "static_assert" and not _is_counter(statement)
    ]
    opaque = {name for st in statements if _opaque(st) for name in st[2:4]}
    return [st for st in statements if not _opaque(st)], opaque


def declarations(
    statements: list[list[str]], opaque: set[str]
) -> tuple[set[str], dict[str, tuple[str, list[str]]]]:
    """The exact declarations and the enums, with every opaque handle name made one name."""
    exact: set[str] = set()
    enums: dict[str, tuple[str, list[str]]] = {}
    for statement in statements:
        statement = ["<handle>" if t in opaque else t for t in statement]
        enum = _enum(statement)
        if enum is not None and enum[0]:
            enums[enum[0]] = enum[1:]
        else:
            exact.add(" ".join(statement))
    return exact, enums


def changed(tag_texts: list[str], tag_all: list[str], tree_all: list[str]) -> list[str]:
    """Declarations of ``tag_texts`` that no header of the tree still carries unchanged.

    Opaque handles are compared as one type, since a renamed handle is still a pointer,
    and a declaration that moved to another header is matched wherever it now lives.
    """
    tag_statements, _ = _parsed(tag_texts)
    tag_exact, tag_enums = declarations(tag_statements, _parsed(tag_all)[1])
    tree_exact, tree_enums = declarations(*_parsed(tree_all))
    out = sorted(tag_exact - tree_exact)
    for name, (head, items) in sorted(tag_enums.items()):
        now = tree_enums.get(name)
        if now is None or now[0] != head or now[1][: len(items)] != items:
            out.append(f"enum {name}")
    return out


def _git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=REPO_ROOT, check=True, capture_output=True, text=True
    ).stdout


def _owner(path: str) -> str | None:
    return next(
        (c.name for c in COUNTERS if any(fnmatch.fnmatch(path, g) for g in c.headers)), None
    )


def _value(counter: Counter, text: str) -> int | None:
    match = re.search(counter.pattern, text)
    return int(match.group(1).rstrip("uU"), 0) if match else None


def main(argv: list[str]) -> int:
    tag = argv[argv.index("--tag") + 1] if "--tag" in argv else None
    tag = tag or _git("describe", "--tags", "--abbrev=0", "main").strip()
    tag_paths = _git("ls-tree", "-r", "--name-only", tag, "--", "include/sonare", "src/rt").split()
    tag_paths = [p for p in tag_paths if _owner(p) is not None]
    tree_paths = [str(p.relative_to(REPO_ROOT)) for p in (REPO_ROOT / "include/sonare").glob("*.h")]
    tree_paths.append("src/rt/command.h")
    tag_blobs = {p: _git("show", f"{tag}:{p}") for p in tag_paths}
    tag_all = list(tag_blobs.values())
    tree_all = [(REPO_ROOT / p).read_text(encoding="utf-8") for p in tree_paths]
    failed = []
    for counter in COUNTERS:
        then = _value(counter, tag_blobs.get(counter.source, ""))
        now = _value(counter, (REPO_ROOT / counter.source).read_text(encoding="utf-8"))
        if then is None:
            print(f"abi bumps: {counter.name} absent from {tag}, new counter")
            continue
        if now != then:
            print(f"abi bumps: {counter.name} {then} -> {now}")
            continue
        tag_texts = [text for p, text in tag_blobs.items() if _owner(p) == counter.name]
        diff = changed(tag_texts, tag_all, tree_all)
        if not diff:
            print(f"abi bumps: {counter.name} {now} unchanged, layout unchanged since {tag}")
            continue
        failed.append(counter.name)
        print(
            f"abi bumps: {counter.name} is still {now} as in {tag} but {len(diff)} declaration(s) "
            f"it governs changed; bump it",
            file=sys.stderr,
        )
        for d in diff[:8]:
            print(f"  {d[:160]}", file=sys.stderr)
    if failed:
        print(f"abi bumps: bump owed for {', '.join(failed)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
