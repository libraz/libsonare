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

import json
import re
import shutil
import subprocess
import tempfile
from html.parser import HTMLParser
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
WEB_DIR = APP_DIR / "web"
JS_DIR = WEB_DIR / "js"
HTML = (WEB_DIR / "index.html").read_text()
JS_FILES = sorted(p for p in JS_DIR.glob("*.js"))
JS = "\n".join(p.read_text() for p in JS_FILES)
#: Every sheet, for the same reason every module is read: a rule moved to the
#: sheet for its region is still the page's rule.
CSS_FILES = sorted(p for p in (WEB_DIR / "css").glob("*.css"))
CSS = "\n".join(p.read_text() for p in CSS_FILES)

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
    text = (JS_DIR / "i18n.js").read_text()
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
    text = (JS_DIR / "i18n.js").read_text()
    body = text.split("const TREE = {", 1)[1].split("\n};", 1)[0]
    nodes = body.split("  nodes: {", 1)[1]
    found: dict[str, str] = {}
    spans = list(re.finditer(r"^    (\w+): \{", nodes, re.MULTILINE))
    for i, m in enumerate(spans):
        end = spans[i + 1].start() if i + 1 < len(spans) else len(nodes)
        found[m.group(1)] = nodes[m.start() : end]
    return found


def test_the_triage_tree_has_no_dead_ends() -> None:
    """A fork pointing at a node that is not there is a question that answers
    into nothing, and the page shows a blank panel rather than an error."""
    text = (JS_DIR / "i18n.js").read_text()
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
    labelled = set(
        re.findall(
            r"^  '?([\w/-]+)'?: 'grade\.", (JS_DIR / "feedback.js").read_text(), re.MULTILINE
        )
    )
    assert verdicts <= labelled, f"no label for: {sorted(verdicts - labelled)}"


#: Where the take list's words are written, on the other side of the harness.
PHRASES = APP_DIR.parent / "voicematch" / "phrases.py"
EXCERPTS = PHRASES.parent / "excerpts"


def _take_strings() -> set[str]:
    """Every label, group and note a rendered take list can carry.

    Read out of the source rather than out of a rendered manifest, because the
    manifests live under an untracked scratch root: a clone with nothing
    rendered would run this against an empty set, which is the way a check
    quietly stops checking. The `Take(…)` calls are parsed, not imported —
    `phrases.py` pulls in the metrics package and numpy with it, and this file
    reads the page rather than running it.
    """
    import ast

    out: set[str] = set()
    for node in ast.walk(ast.parse(PHRASES.read_text())):
        if not (isinstance(node, ast.Call) and getattr(node.func, "id", "") == "Take"):
            continue
        # id, label, group, sub — everything but the id, and only where it is a
        # literal. The musical take names neither its label nor its note here;
        # both come from the excerpt, which is read below.
        for arg in node.args[1:4]:
            if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
                out.add(arg.value)
    for path in sorted(EXCERPTS.glob("*.json")):
        data = json.loads(path.read_text())
        out |= {data["label"], data["note"]}
    return {s for s in out if s}


def test_every_take_string_the_harness_can_render_is_translated() -> None:
    """A take's label and note are baked into the manifest in English, so the
    Japanese for them lives on the page. A phrase added without one renders an
    English line in a Japanese list, which nothing else would report."""
    text = (JS_DIR / "take-text.js").read_text()
    keys = set(re.findall(r"^  (?:'([^']*)'|\"([^\"]*)\"):", text, re.MULTILINE))
    known = {a or b for a, b in keys}
    missing = _take_strings() - known
    assert not missing, f"no Japanese for: {sorted(missing)}"


def test_every_module_the_page_imports_is_present() -> None:
    wanted = set(re.findall(r"from '\./([\w.-]+)'", JS))
    wanted |= set(re.findall(r'<script[^>]+src="([\w.-]+)"', HTML))
    here = {p.name for p in JS_FILES}
    assert not wanted - here, f"imported and not present: {sorted(wanted - here)}"


def _select_take_source() -> str:
    text = (JS_DIR / "listen.js").read_text()
    return text.split("export async function selectTake", 1)[1].split("\n}\n", 1)[0]


def test_a_take_load_commits_only_under_its_own_selection() -> None:
    """Source-shape check, not a race test: it pins that the epoch exists, is
    bumped on selection, and guards the commit and the failure path. That the
    slower of two loads is actually discarded needs a browser."""
    body = _select_take_source()
    assert "let selectEpoch = 0;" in (JS_DIR / "listen.js").read_text()
    assert "const epoch = ++selectEpoch;" in body
    commit = body.index("state.take = take;")
    assert "if (epoch !== selectEpoch) return;" in body[:commit]
    catch = body.split("} catch (err) {", 1)[1].split("return;", 1)[0]
    assert "epoch !== selectEpoch" in catch, "a stale failure must not write the caption"
    # The previous buffer is dropped before the await, not after it.
    assert body.index("state.take = null;") < body.index("await loadTake(")
    assert body.index("setSending(false)") < body.index("await loadTake(")
    assert body.index("setSending(true)") > commit


def test_a_set_load_is_also_guarded_by_the_epoch() -> None:
    text = (JS_DIR / "listen.js").read_text()
    body = text.split("export async function loadSet", 1)[1].split("\n}\n", 1)[0]
    assert body.count("epoch !== selectEpoch") >= 2


def test_sending_is_gated_for_the_button_and_the_shortcut() -> None:
    text = (JS_DIR / "listen.js").read_text()
    assert "$('fbSend').disabled = !on" in text
    assert re.search(r"addEventListener\('keydown'.*?,\s*true\)", text, re.DOTALL)
    # feedback.js is what the guard sits in front of: the shortcut calls send()
    # directly, so a disabled button alone would not have been enough.
    assert "ev.key === 'Enter'" in (JS_DIR / "feedback.js").read_text()


def test_blind_is_refused_for_a_direct_input_reference() -> None:
    text = (JS_DIR / "blind.js").read_text()
    assert "voice.rig === 'none'" in text
    assert "applyBlindGate" in (JS_DIR / "listen.js").read_text()
    # The B shortcut fires `change` on the box without checking `disabled`.
    assert re.search(
        r"\$\('blind'\)\.addEventListener\('change'.*?stopImmediatePropagation", text, re.DOTALL
    )
    assert "t('blind.refDi')" in text


def test_blocks_are_keyed_by_role_and_scope() -> None:
    state = (JS_DIR / "state.js").read_text()
    assert "export const scopeOf = (key) => sourceOf(key).scope || '';" in state
    assert "export const blockOf = (key) => `${roleOf(key)}|${scopeOf(key)}`;" in state
    versions = (JS_DIR / "versions.js").read_text()
    assert "pick.dataset.block = `${role}|${scope}`;" in versions
    assert "scopeOf(key) === 'instrument' ? 1 : 0" in versions, "instrument after product"
    # The signal-path field is gone from every reader.
    assert "pathOf" not in JS and ".path === 'direct'" not in JS


def test_blind_follows_the_selected_comparison() -> None:
    text = (JS_DIR / "blind.js").read_text()
    gate = text.split("export const blindBlocked", 1)[1].split("};", 1)[0]
    assert "if (c) return c.status !== 'matched';" in gate
    # The legacy rule for a manifest written before comparisons.
    assert gate.index("c.status") < gate.index("voice.rig === 'none'")
    draw = text.split("export function reshuffleBlind", 1)[1].split("\n}\n", 1)[0]
    assert "comparisonKeys(c)" in draw
    assert "scopeOf(key) !== 'instrument'" in draw, "a legacy draw takes the product scope only"
    assert "document.addEventListener('audition:comparison'" in text
    assert "new CustomEvent('audition:comparison')" in (JS_DIR / "versions.js").read_text()


def test_the_comparison_selector_and_what_attaches_to_it() -> None:
    versions = (JS_DIR / "versions.js").read_text()
    assert "buildComparisonRow(box);" in versions
    select = versions.split("function selectComparison", 1)[1].split("\n}\n", 1)[0]
    assert "state.picks = JSON.parse(localStorage.getItem(picksKey())" in select
    assert "state.lastOracle = null;" in select
    state = (JS_DIR / "state.js").read_text()
    assert "${c ? `:${c.id}` : ''}" in state, "picks are held per comparison"
    player = (JS_DIR / "player.js").read_text()
    assert "comparison_id: comparison ? comparison.id : null," in player
    # Keys built at run time from the manifest's ids and statuses.
    table = _string_table()
    wanted = {"compare.label", "compare.against", "compare.noReference", "blind.unmatched"}
    for cid in ("instrument_di", "gm_gs_product"):
        wanted |= {f"compare.{cid}", f"compare.{cid}.long"}
    for status in ("matched", "context_only", "unverified", "unavailable"):
        wanted.add(f"compare.status.{status}")
    assert wanted <= table["en"], sorted(wanted - table["en"])
    # The reference is named by the manifest's own label, never by the page.
    assert "(cur.oracle_sources || []).map(sourceLabel)" in versions


#: Just enough of a browser for the page's modules to load under node: what
#: they touch at import time, and the storage and address the tests set.
_NODE_PRELUDE = """
const store = new Map(Object.entries(JSON.parse(process.env.STORE || '{}')));
globalThis.localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => { store.set(k, String(v)); },
  key: (i) => [...store.keys()][i] ?? null,
  get length() { return store.size; },
};
Object.defineProperty(globalThis, 'navigator',
  { value: { languages: ['en'], language: 'en' }, configurable: true });
globalThis.document = {
  addEventListener() {}, getElementById() { return null; },
  documentElement: { lang: 'en' }, querySelectorAll() { return []; },
};
globalThis.window = globalThis;
globalThis.location = { search: '', hash: process.env.HASH || '', origin: '', pathname: '/' };
const dir = process.env.JS_DIR;
"""


def _node(script: str, **env: str) -> list[str] | None:
    """Run `script` as an ES module after the prelude; None where node is absent."""
    node = shutil.which("node")
    if node is None:
        return None
    with tempfile.NamedTemporaryFile("w", suffix=".mjs", delete=False) as fh:
        fh.write(_NODE_PRELUDE + script)
    try:
        out = subprocess.run(
            [node, fh.name],
            capture_output=True,
            text=True,
            check=True,
            env={"PATH": "/usr/bin:/bin", "JS_DIR": str(JS_DIR), **env},
        )
    finally:
        Path(fh.name).unlink()
    return out.stdout.splitlines()


def _render_path_source() -> str:
    text = (JS_DIR / "path.js").read_text()
    return text.split("export function renderPath", 1)[1]


def test_the_path_line_draws_the_recorded_chain() -> None:
    """Executed under node where it is installed: the chain, the bypassed
    stage, the pre-rig branch and the interval the playhead falls in."""
    got = _node(
        """
const path = await import(`${dir}/path.js`);
const units = [{ unit: 0, type: '0x0110', realization: 'classic',
  stages: ['od', 'amp'], enabled: [true, false] }];
const part = { part: 0, stages: ['amp', 'cab'], unit: 0, send_tap: 'pre_rig' };
console.log(JSON.stringify(path.partChain(part, units)));
console.log(JSON.stringify(path.partChain({ ...part, stages: [], send_tap: 'post_unit' }, units)));
console.log(JSON.stringify(path.partChain({ ...part, unit: null, send_tap: 'none' }, units)));
const ev = [{ frame: 0 }, { frame: 48000 }];
console.log([0, 47999, 48000, 96000].map((f) => path.topologyIndexAt(ev, f)).join(','));
console.log(path.topologyIndexAt([{ frame: 100 }], 0));
"""
    )
    if got is not None:
        pre, post, none, at, before = got
        assert json.loads(pre) == {
            "chain": "voice → amp → cab → GS 0x0110 unit (classic) [od, amp (bypassed)]",
            "branch": "voice ↳ sends taken here, before the rig",
        }, pre
        assert json.loads(post)["chain"].endswith("[od, amp (bypassed)] → sends"), post
        assert json.loads(post)["branch"] is None
        assert json.loads(none)["chain"] == "voice → amp → cab  ·  no sends", none
        assert at == "0,0,1,1", at
        assert before == "0", "before the first topology, the first is shown"
    body = _render_path_source()
    assert "ev.status !== 'recorded'" in body and "t('path.notRecorded'" in body
    assert "ev.complete === false" in body and "t('path.incomplete'" in body
    assert "events.length > 1" in body and "t('path.changes'" in body
    assert "state.take.rates[key]" in body, "frames are counted at the file's own rate"
    assert "renderPath();" in (JS_DIR / "app.js").read_text()
    assert "rates[k] = wavRate(bytes);" in (JS_DIR / "player.js").read_text()
    table = _string_table()
    for key in (
        "path.realization.modern",
        "path.realization.classic",
        "path.sends.pre_rig",
        "path.sends.post_unit",
        "path.sends.none",
    ):
        assert key in table["en"] and key in table["ja"], key


def test_the_path_line_names_stages_and_says_when_no_send_leaves() -> None:
    """A processor reads by its display name, a disabled part stage as bypassed,
    a score with every send at zero says so instead of drawing a send tap, and
    only the part the score plays is drawn."""
    got = _node(
        """
const path = await import(`${dir}/path.js`);
const part = { part: 0, stages: ['saturation.overdrive', 'saturation.ampSim'],
  enabled: [false, true], unit: null, send_tap: 'pre_rig' };
console.log(JSON.stringify(path.partChain(part, [], [0, 0, 0])));
console.log(JSON.stringify(path.partChain(part, [], [null, null, null])));
"""
    )
    if got is not None:
        zero, power_on = got
        assert json.loads(zero) == {
            "chain": "voice → overdrive (bypassed) → amp  ·  sends at zero (no reverb or chorus)",
            "branch": None,
        }, zero
        assert json.loads(power_on)["branch"] == "voice ↳ sends taken here, before the rig"
    body = _render_path_source()
    assert "p.part === ev.channel" in body, "only the scored part is drawn"
    table = _string_table()
    for key in ("path.sends.zero", "path.stage.saturation.ampSim"):
        assert key in table["en"] and key in table["ja"], key


def test_blind_hides_the_path_but_names_the_comparison() -> None:
    body = _render_path_source()
    blind = body.split("if (state.blind) {", 1)[1].split("\n  }\n", 1)[0]
    assert "return;" in blind
    assert "compare.status." in blind and "t('path.hidden')" in blind
    # Nothing that names a stage, a source or the raw record before the blind return.
    head = body.split("if (state.blind) {", 1)[0] + blind
    for named in ("partChain(", "rawText(", "box.title = rawText", "path.reference"):
        assert named not in head, named


def test_picks_are_keyed_by_generation_and_comparison() -> None:
    gen = "a" * 64
    store = {
        f"audition:picks:p027:gm_gs_product@{gen[:16]}": json.dumps({"t1": {"key": "m"}}),
        f"audition:picks:p027:gm_gs_product@{'b' * 16}": json.dumps({"t1": {"key": "m"}}),
        "audition:picks:p027:gm_gs_product": json.dumps({"t2": {"unseparated": True}}),
        "audition:picks:p027": json.dumps({"t3": {"key": "m"}}),
        "audition:picks:p027:instrument_di": json.dumps({"t4": {"key": "m"}}),
        "audition:picks:p0270:gm_gs_product": json.dumps({"t5": {"key": "m"}}),
    }
    got = _node(
        f"""
const s = await import(`${{dir}}/state.js`);
s.state.setId = 'p027';
s.state.manifest = {{ set_generation: '{gen}',
  comparisons: [{{ id: 'gm_gs_product', scope: 'product', status: 'matched' }}] }};
console.log(s.picksKey());
console.log(JSON.stringify(s.earlierPicksKeys().sort()));
s.state.manifest = {{ items: [] }};
console.log(s.picksKey());
""",
        STORE=json.dumps(store),
    )
    if got is not None:
        current, earlier, legacy = got
        assert current == f"audition:picks:p027:gm_gs_product@{gen[:16]}", current
        assert json.loads(earlier) == sorted(
            [
                f"audition:picks:p027:gm_gs_product@{'b' * 16}",
                "audition:picks:p027:gm_gs_product",
                "audition:picks:p027",
            ]
        ), earlier
        assert legacy == "audition:picks:p027", "a page with neither keeps its old key"
    blind = (JS_DIR / "blind.js").read_text()
    tally = blind.split("function blindTally", 1)[1].split("\n}\n", 1)[0]
    assert "state.picks" in tally and "earlier" not in tally, "earlier picks stay out of the tally"
    assert "t('blind.earlier'" in blind


def test_blind_answers_carry_what_they_were_given_under() -> None:
    blind = (JS_DIR / "blind.js").read_text()
    answered = blind.split("function answered", 1)[1].split("\n}\n", 1)[0]
    for field in ("take:", "candidates:", "comparison_id:", "set_generation:", "answered_at:"):
        assert field in answered, field
    record = blind.split("export async function recordBlindResult", 1)[1].split("\n}\n", 1)[0]
    for field in ("picked:", "abstained:", "p.set_generation", "p.answered_at", "p.candidates"):
        assert field in record, field
    feedback = (JS_DIR / "feedback.js").read_text()
    sent = feedback.split("export async function recordBlind", 1)[1].split("\n}\n", 1)[0]
    assert "...conditions()" not in sent, "a run carries no conditions from the moment it is sent"
    assert "blind_answers: blindAnswers" in sent


def test_a_note_is_schema_2_and_a_409_keeps_the_text() -> None:
    feedback = (JS_DIR / "feedback.js").read_text()
    send = feedback.split("async function send()", 1)[1].split("\n}\n", 1)[0]
    assert "schema_version: 2" in send and "evaluation: judged" in send
    assert "evidence: attached ? evidenceClaim(judged.judged_source) : null" in send
    post = feedback.split("async function post(", 1)[1].split("\n}\n", 1)[0]
    stale = post.split("if (res.status === 409) {", 1)[1].split("\n    }\n", 1)[0]
    assert "after" not in stale, "a refused note must not clear the composer"
    assert "$('fbReload').hidden = false" in stale
    assert "new CustomEvent('audition:reload-set')" in feedback
    assert "document.addEventListener('audition:reload-set'" in (JS_DIR / "listen.js").read_text()


def test_the_address_carries_the_comparison() -> None:
    got = _node(
        """
const a = await import(`${dir}/address.js`);
console.log(JSON.stringify(a.readRoute()));
""",
        HASH="#p027/riff/model?c=instrument_di",
    )
    if got is not None:
        route = json.loads(got[0])
        assert (route["set"], route["take"], route["ver"], route["cmp"]) == (
            "p027",
            "riff",
            "model",
            "instrument_di",
        ), route
    address = (JS_DIR / "address.js").read_text()
    assert "`?c=${encodeURIComponent(c.id)}`" in address
    listen = (JS_DIR / "listen.js").read_text()
    route = listen.split("export async function applyRoute", 1)[1].split("\n}\n", 1)[0]
    assert "selectComparison(r.cmp)" in route
    load = listen.split("export async function loadSet", 1)[1].split("\n}\n", 1)[0]
    assert load.index("state.comparisonId = wanted.cmp") < load.index("picksKey()")


def test_the_epoch_also_guards_set_switch_feedback_and_pictures() -> None:
    """Source shape only; that a late reply is actually dropped needs a browser."""
    listen = (JS_DIR / "listen.js").read_text()
    load = listen.split("export async function loadSet", 1)[1].split("\n}\n", 1)[0]
    first_await = load.index("await ")
    assert load.index("state.setEpoch += 1;") < first_await
    assert load.index("state.take = null;") < first_await
    take = _select_take_source()
    assert take.index("take.epoch = epoch;") < take.index("state.take = take;")
    feedback = (JS_DIR / "feedback.js").read_text()
    post = feedback.split("async function post(", 1)[1].split("\n}\n", 1)[0]
    assert "const setEpoch = state.setEpoch;" in post
    commit = post.index("fb.entries = got.entries")
    assert "if (setEpoch !== state.setEpoch) return true;" in post[:commit]
    load_fb = feedback.split("export async function loadFeedback", 1)[1].split("\n}\n", 1)[0]
    assert load_fb.index("if (setEpoch !== state.setEpoch) return;") < load_fb.index(
        "fb.entries = got.entries"
    )
    scope = (JS_DIR / "scope.js").read_text()
    assert scope.count("const sig = `${state.take.epoch}|") == 2
    assert "${state.take.id}|" not in scope


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
            print(
                f"FAIL {t.__name__}: {type(e).__name__}: {e}"
                if not isinstance(e, AssertionError)
                else f"FAIL {t.__name__}: {e}"
            )
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
