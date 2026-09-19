"""The page's markup, stylesheet and modules have to agree, and nothing else
checks that.

`serve.py` never parses the page it serves, so a renamed element or a class that
lost its rule is a silently dead control rather than a failed request: the page
loads, the button is there, and it does nothing. These are the agreements that
can be checked without a browser.

EVERY module is read, not `app.js` alone. The page is a handful of ES modules
and most of what builds an element is in the other ones, so a check anchored on
the entry point would pass over almost all of the markup it is meant to guard —
which is the same failure as not running it.
"""

from __future__ import annotations

import re
from html.parser import HTMLParser
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
HTML = (APP_DIR / "index.html").read_text()
JS_FILES = sorted(p for p in APP_DIR.glob("*.js"))
JS = "\n".join(p.read_text() for p in JS_FILES)
CSS = (APP_DIR / "style.css").read_text()

#: Elements the page writes as void tags, which have no closing tag to match.
VOID = {"meta", "link", "br", "input", "img", "hr", "source", "col"}


def js_class_names() -> set[str]:
    """Every class name the script can put on an element.

    Both spellings are collected — the `el(tag, 'a b')` helper and a template
    literal assigned to `className` — and the interpolations inside a literal
    are dropped, since what a `${…}` produces is one of the plain names found
    elsewhere.
    """
    names: set[str] = set()
    for chunk in re.findall(r"el\('[a-z0-9]+',\s*'([^']*)'", JS):
        names |= set(chunk.split())
    for chunk in re.findall(r"className\s*=\s*[`']([^`']*)[`']", JS):
        names |= {c for c in chunk.split() if "$" not in c and "{" not in c}
    for chunk in re.findall(r"el\('[a-z0-9]+',\s*`([^`]*)`", JS):
        names |= {c for c in re.sub(r"\$\{[^}]*\}", " ", chunk).split()}
    names |= set(re.findall(r"classList\.(?:add|remove|toggle)\('([\w-]+)'", JS))
    return {n for n in names if n}


def html_class_names() -> set[str]:
    names: set[str] = set()
    for chunk in re.findall(r'class="([^"]+)"', HTML):
        names |= set(chunk.split())
    return names


def test_every_element_the_script_reaches_for_exists() -> None:
    ids = set(re.findall(r'\bid="([^"]+)"', HTML))
    used = set(re.findall(r"\$\('([^']+)'\)", JS))
    assert not used - ids, f"read by a module, not in index.html: {sorted(used - ids)}"


def test_every_class_the_page_uses_has_a_rule() -> None:
    defined = set(re.findall(r"\.([A-Za-z][\w-]*)", CSS))
    used = js_class_names() | html_class_names()
    assert not used - defined, f"styled by nothing: {sorted(used - defined)}"


def test_the_markup_is_balanced() -> None:
    class Check(HTMLParser):
        def __init__(self) -> None:
            super().__init__()
            self.stack: list[str] = []
            self.bad: list[str] = []

        def handle_starttag(self, tag: str, attrs: object) -> None:
            if tag not in VOID:
                self.stack.append(tag)

        def handle_endtag(self, tag: str) -> None:
            if not self.stack or self.stack[-1] != tag:
                self.bad.append(tag)
            else:
                self.stack.pop()

    check = Check()
    check.feed(HTML)
    assert not check.bad, f"closed out of order: {check.bad}"
    assert not check.stack, f"never closed: {check.stack}"


def _string_table() -> dict[str, set[str]]:
    """The keys each language declares, read out of `i18n.js`.

    A regex rather than a parser because the table is a literal and is required
    to stay one: it is the file a translation is written into, and anything
    clever enough to need parsing would be too clever to hand to a translator.
    """
    text = (APP_DIR / "i18n.js").read_text()
    body = text.split("const STRINGS = {", 1)[1]
    out: dict[str, set[str]] = {}
    for lang in ("en", "ja"):
        chunk = body.split(f"\n  {lang}: {{", 1)[1].split("\n  },", 1)[0]
        out[lang] = set(re.findall(r"^\s*'([\w.]+)':", chunk, re.MULTILINE))
    return out


def test_the_two_languages_declare_the_same_keys() -> None:
    table = _string_table()
    missing = table["en"] - table["ja"]
    extra = table["ja"] - table["en"]
    assert not missing, f"not translated into Japanese: {sorted(missing)}"
    assert not extra, f"in Japanese and not in English: {sorted(extra)}"


def test_every_string_the_page_asks_for_is_declared() -> None:
    """A key with no entry renders as itself, which is a visible defect and a
    silent one to every check that does not look at the page."""
    declared = _string_table()["en"]
    # Literal calls only. A key built from a variable — the six stage names, the
    # two role names — is resolved at run time and cannot be read here; each of
    # those families is covered by the pair check above instead.
    used = set(re.findall(r"\bt\('([\w.]+)'", JS))
    used |= set(re.findall(r'data-i18n(?:-\w+)?="([\w.]+)"', HTML))
    assert not used - declared, f"asked for and never declared: {sorted(used - declared)}"


def _tree_nodes() -> dict[str, str]:
    """Each triage node's id and the source of its body."""
    text = (APP_DIR / "i18n.js").read_text()
    body = text.split("const TREE = {", 1)[1].split("\n};", 1)[0]
    nodes = body.split("  nodes: {", 1)[1]
    found: dict[str, str] = {}
    spans = list(re.finditer(r"^    (\w+): \{", nodes, re.MULTILINE))
    for i, m in enumerate(spans):
        end = spans[i + 1].start() if i + 1 < len(spans) else len(nodes)
        found[m.group(1)] = nodes[m.start():end]
    return found


def test_the_triage_tree_has_no_dead_ends() -> None:
    """A fork pointing at a node that is not there is a question that answers
    into nothing, and the page shows a blank panel rather than an error."""
    text = (APP_DIR / "i18n.js").read_text()
    start = re.search(r"start: '(\w+)'", text).group(1)
    nodes = _tree_nodes()
    assert start in nodes, start
    assert len(nodes) >= 6, sorted(nodes)

    reached = {start}
    frontier = [start]
    while frontier:
        here = frontier.pop()
        for to in re.findall(r"to: '(\w+)'", nodes[here]):
            assert to in nodes, f"{here} points at {to}, which is not a node"
            if to not in reached:
                reached.add(to)
                frontier.append(to)
    assert reached == set(nodes), f"unreachable: {sorted(set(nodes) - reached)}"

    for name, src in nodes.items():
        # Two substantive answers and "not sure". Three substantive answers is
        # the shape this replaced: it asks the listener to weigh options rather
        # than to answer, and it is what made a question about an attack
        # transient feel answerable when it was not.
        assert len(re.findall(r"\{ tag: '", src)) == 2, f"{name} is not a pair"
        assert re.search(r"unsure: '", src), f"{name} has no 'not sure'"


def test_every_verdict_the_tree_can_reach_has_a_label() -> None:
    """The grade is what a triage reads first, and an unlabelled one renders as
    its own tag — which is the only string on the page that is not a sentence."""
    nodes = _tree_nodes()
    verdicts = set(re.findall(r"tag: '([\w/-]+)'", nodes["off"]))
    verdicts |= set(re.findall(r"tag: '([\w/-]+)'", nodes["grade"]))
    verdicts |= {"broken"}
    verdicts |= set(re.findall(r"unsure: '([\w/-]+)'", nodes["off"]))
    labelled = set(re.findall(r"^  '?([\w/-]+)'?: 'grade\.",
                              (APP_DIR / "feedback.js").read_text(), re.MULTILINE))
    assert verdicts <= labelled, f"no label for: {sorted(verdicts - labelled)}"


def test_every_module_the_page_imports_is_present() -> None:
    wanted = set(re.findall(r"from '\./([\w.-]+)'", JS))
    wanted |= set(re.findall(r'<script[^>]+src="([\w.-]+)"', HTML))
    here = {p.name for p in JS_FILES}
    assert not wanted - here, f"imported and not present: {sorted(wanted - here)}"


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        # Any exception, not just a failed assertion: a test that reaches for a
        # key a file stopped carrying raises, and catching only AssertionError
        # ends the whole run with no line saying which test it was.
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"FAIL {t.__name__}: {type(e).__name__}: {e}"
                  if not isinstance(e, AssertionError) else f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
