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
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_PATH = HERE / "calibrations.json"

#: The source key the unmodified voice takes on every page.
BASELINE = "model"

#: JSON keys that document the file rather than describing a voice. The capture
#: definitions use the same convention, which is what lets the explanation sit
#: next to the thing it explains instead of in a document nobody opens.
DOC_PREFIX = "_"


@dataclass(frozen=True)
class Variant:
    """One named setting of a voice, as a version of every take on the page."""

    name: str
    overrides: str
    note: str = ""

    @property
    def detail(self) -> str:
        """What the page shows under the version button."""
        if self.note and self.overrides:
            return f"{self.note} — {self.overrides}"
        return self.note or self.overrides or "no overrides"


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


def parse_cli(specs: list[str]) -> list[Variant]:
    """`name=overrides` pairs from the command line, in the order given.

    The overrides are passed through untouched, since the library is the only
    thing that can say whether a key exists.
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
            variants.append(Variant(
                name=name,
                overrides=str(item.get("overrides", "")).strip(),
                note=str(item.get("note", "")).strip(),
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

    A name declared in both is refused rather than resolved. The page labels its
    version buttons with the name and nothing else, so two settings sharing one
    is the single failure a listening page must not have — whichever won, the
    note written about it would name the other just as well.
    """
    recorded = table.get(slug, [])
    clash = {v.name for v in recorded} & {v.name for v in extra}
    if clash:
        raise ValueError(
            f"{slug}: {', '.join(sorted(clash))} is both recorded and passed as "
            f"--variant; rename one, or drop the flag to use the recorded setting")
    return [*recorded, *extra]
