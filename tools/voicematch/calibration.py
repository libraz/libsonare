"""Named calibration settings, recorded per voice.

`--variant name=overrides` puts a candidate setting of a voice on the listening
page beside the unmodified build, which is how a question the metrics cannot
settle gets answered. It is a command-line argument, so it lives as long as the
shell history does, and it applies to every voice in the run — which means a
batch across the bank cannot carry per-voice candidates at all, and the settings
that a listening session actually decided something about are gone by the next
one.

So they are recorded here instead: `calibrations.json` maps a voice to an
ordered list of named settings, each an override string and a line saying what
it is for. Tracked, because an override string is knob names and numbers — no
part of it names a commercial product, unlike the capture overlays — so a
calibration question a page was built to settle can be reopened from a clone.

The file is not consulted unless a run asks for it (`--calibrations`). Every
recorded setting is an extra render of every take and the whole layer needs a
`-DBUILD_TUNING=ON` library, so paying for it on a page somebody opened to hear
one voice would be the wrong default.

Keyed by the voice's slug — the same string the page shows, the URL carries and
the directory is called — so a note about a render names the same voice the file
does. A key that matches no voice in the bank is a typo that would otherwise be
silent, and `unknown_voices` is what a test holds it to.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_PATH = HERE / "calibrations.json"

#: The source key the unmodified voice takes on every page.
BASELINE = "model"

#: The languages a recorded setting is written in. English is the repository's
#: and the fallback, so a missing translation is an English line in a Japanese
#: page rather than a blank one.
LANGS = ("en", "ja")

#: JSON keys that document the file rather than describing a voice. The capture
#: definitions use the same convention, which is what lets the explanation sit
#: next to the thing it explains instead of in a document nobody opens.
DOC_PREFIX = "_"


@dataclass(frozen=True)
class Variant:
    """One named setting of a voice, as a version of every take on the page."""

    name: str
    overrides: str
    #: The long rationale: what was measured, what it ruled out, what it costs.
    #: The record rather than the page's first line — it is read once, by
    #: whoever reopens the question.
    note: str = ""
    #: What the setting is, in a few words, per language. This is the button:
    #: a name has to be an identifier and an identifier is not a sentence, so
    #: `foundations-only` says what moved and never says what it is FOR, and a
    #: row of ten of them is a lineup nobody can read the intent of.
    title: dict = field(default_factory=dict)
    #: One line for whoever is listening, per language: what to listen for, and
    #: what hearing it would mean. Not the override string in words.
    desc: dict = field(default_factory=dict)

    def text(self, lang: str = "en") -> dict[str, str]:
        """Title and line in one language, falling back to English."""
        return {
            "title": self.title.get(lang) or self.title.get("en") or self.name,
            "desc": self.desc.get(lang) or self.desc.get("en") or "",
        }

    @property
    def detail(self) -> str:
        """What the page shows when this version is the one sounding.

        The override string is deliberately not in it. A question put in the
        parameter's own vocabulary gets the parameter's own answer back — a
        listener shown `piano.brightness=0.30` reports on brightness, which is
        the one thing the knob cannot be asked about. It stays here in the
        registry, where the person changing it reads it.
        """
        return self.note or self.desc.get("en", "") or ("" if self.overrides else "no overrides")


#: One `a.b.c=1.5` assignment, which is what an override string is a list of.
_ASSIGNMENT = re.compile(r"[\w.]+=[-\w.+]+")


def _all_assignments(text: str) -> bool:
    parts = [p.strip() for p in text.split(",") if p.strip()]
    return bool(parts) and all(_ASSIGNMENT.fullmatch(p) for p in parts)


def strip_overrides(detail: str) -> str:
    """A version's line with any override string taken off it.

    Pages rendered before the string came off the page carry `note — a.b=1,c=2`
    in the field the banner reads, and there are a hundred and eighty-odd of
    them. Matched against the exact shape the renderer wrote — a list of
    assignments — rather than by looking for an equals sign, so a note that
    happens to end in prose is left alone.

    A setting with no note at all had the override string as its WHOLE line,
    which is the case a rule written around the separator misses: five of them
    were still naming a knob on the page after the first pass. Nothing is a
    better answer than the parameter, and it is also the true one — nobody
    registered anything about that setting.
    """
    if _all_assignments(detail):
        return ""
    head, sep, tail = detail.rpartition(" — ")
    return head if sep and _all_assignments(tail) else detail


def source_text(variant: Variant, *, direct: bool = False) -> dict:
    """The both-language title and line a version button carries.

    One function for the two places that need it — `make_audition.py` writing a
    new manifest and `tools/audition/serve.py` answering for one rendered before
    the fields existed — so a page rendered months ago carries the same words as
    one rendered today, and neither is a copy of the other's rule.
    """
    title = dict(variant.title)
    desc = dict(variant.desc)
    if direct:
        # In front rather than behind. A voice with a rig renders each candidate
        # twice and the pair sits side by side, so the one word that separates
        # them has to be in the part of the button a narrow column keeps.
        title = {k: (f"リグなし・{v}" if k == "ja" else f"No rig — {v}")
                 for k, v in title.items()}
    return {"title": title, "desc": desc}


def check_name(name: str) -> str:
    """A variant name, or the reason it cannot be one.

    The name is the source key the page shows, the file stem on disk and the
    last segment of a render's address, so it is restricted to what is safe in
    all three.
    """
    if not name:
        return "a variant needs a name"
    if not all(c.isalnum() or c in "._-" for c in name):
        return f"name may only hold letters, digits, . _ - : {name!r}"
    if name == BASELINE:
        return f"name {BASELINE!r} is taken by the unmodified voice"
    return ""


def _lines(raw: object, where: str, field_name: str) -> dict[str, str]:
    """One line per language, or the reason it is not one.

    Refused at load rather than checked by a test, because the cost of a gap is
    paid by whoever opens the page: a button with no title falls back to its own
    key, which is exactly the state this field exists to end, and nothing about
    the render says a translation was meant to be there. A run that reads the
    file is the moment the author is still holding the setting.
    """
    lines = raw if isinstance(raw, dict) else {}
    out = {k: str(v).strip() for k, v in lines.items() if k in LANGS and str(v).strip()}
    missing = [lang for lang in LANGS if not out.get(lang)]
    if missing:
        raise ValueError(
            f"{where}: {field_name} is missing {', '.join(missing)} — it wants a "
            f"line per language, {{{', '.join(repr(k) for k in LANGS)}}}")
    return out


def parse_cli(specs: list[str]) -> list[Variant]:
    """`name=overrides` pairs from the command line, in the order given.

    The overrides are passed through untouched, since the library is the only
    thing that can say whether a key exists.

    No title and no line: a shell argument cannot carry two languages, and the
    person who typed it is the person listening. Its button falls back to the
    name, which is the state a *recorded* setting is refused for — a setting
    worth keeping is worth a sentence, and one typed to hear something once is
    gone with the shell history.
    """
    out: list[Variant] = []
    for spec in specs:
        name, sep, overrides = spec.partition("=")
        name = name.strip()
        if not sep:
            raise ValueError(f"--variant wants name=overrides, got {spec!r}")
        bad = check_name(name)
        if bad:
            raise ValueError(f"--variant {bad}")
        out.append(Variant(name, overrides.strip()))
    return out


def load(path: Path | None = None) -> dict[str, list[Variant]]:
    """The recorded settings, by voice slug. An absent file is an empty one."""
    path = DEFAULT_PATH if path is None else path
    if not path.exists():
        return {}
    raw = json.loads(path.read_text())
    table: dict[str, list[Variant]] = {}
    for slug, entry in raw.items():
        if slug.startswith(DOC_PREFIX):
            continue
        variants: list[Variant] = []
        for item in entry.get("variants", []):
            name = str(item.get("name", "")).strip()
            bad = check_name(name)
            if bad:
                raise ValueError(f"{path.name}: {slug}: {bad}")
            where = f"{path.name}: {slug}: {name}"
            variants.append(Variant(
                name=name,
                overrides=str(item.get("overrides", "")).strip(),
                note=str(item.get("note", "")).strip(),
                title=_lines(item.get("title"), where, "title"),
                desc=_lines(item.get("desc"), where, "desc"),
            ))
        names = [v.name for v in variants]
        if len(names) != len(set(names)):
            raise ValueError(f"{path.name}: {slug} names a variant twice")
        if variants:
            table[slug] = variants
    return table


def unknown_voices(table: dict[str, list[Variant]], slugs: set[str]) -> list[str]:
    """Keys that match no voice in the bank.

    A typo here costs nothing at the time and everything later: the run finds no
    settings for the voice, renders the baseline alone, and reports a page that
    looks exactly like a voice nobody has recorded a candidate for.
    """
    return sorted(slug for slug in table if slug not in slugs)


def for_voice(slug: str, table: dict[str, list[Variant]],
              extra: list[Variant]) -> list[Variant]:
    """The recorded settings for one voice, then the run's own, in that order.

    A name declared in both is refused rather than resolved. The name is the
    address a render is reached at and the string a listening note carries, so
    two settings sharing one is the single failure a listening page must not
    have — whichever won, the note written about it would name the other just
    as well.
    """
    recorded = table.get(slug, [])
    clash = {v.name for v in recorded} & {v.name for v in extra}
    if clash:
        raise ValueError(
            f"{slug}: {', '.join(sorted(clash))} is both recorded and passed as "
            f"--variant; rename one, or drop the flag to use the recorded setting")
    return [*recorded, *extra]
