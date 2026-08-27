"""The page's three files have to agree, and nothing else checks that.

`serve.py` never parses the page it serves, so a renamed element or a class that
lost its rule is a silently dead control rather than a failed request: the page
loads, the button is there, and it does nothing. These are the two agreements
that can be checked without a browser.
"""

from __future__ import annotations

import re
from html.parser import HTMLParser
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
HTML = (APP_DIR / "index.html").read_text()
JS = (APP_DIR / "app.js").read_text()
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
    assert not used - ids, f"app.js reads elements index.html does not define: {used - ids}"


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
