"""The library's own voice list, as the index every audition page is built from.

The harness grew the other way round. A page was addressed by capture — the
reference plugin's definition named the instrument, the GM program and the
phrase set — which meant a voice could be auditioned only where somebody owned
a plugin and had captured it. Four captures exist, so four of the bank's voices
had a page and the rest had none, including every voice nobody has a reference
for and every voice whose reference is not worth buying.

So the index is the bank: every GM program, every variation bank the library
voices apart, and each drum kit. A capture is an attachment to an
entry rather than the entry itself — where one exists its phrase set and its
reference timbres are used, and where none does the page renders the model
alone and plays rather than compares. `serve.py` already treats a set holding
one version of each take as a set, so nothing downstream needs a special case.

Two facts have two sources and this module keeps them apart. What the library
voices — which banks a program has, which patch answers it — comes from the
library itself through `catalogue.dump_catalogue`, and needs a
`-DBUILD_TUNING=ON` build; what a program IS — its GM name, its tone class, its
compass — comes from the tables here and needs no build at all. An index
without a tuning build is therefore complete and merely says less: it lists all
128 programs at bank 0 and names no patch.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

from capture import load_config
from gm_names import gm_name
from phrases import is_drum_set, take_set_for
from toneclass import ToneClass, tone_class

HERE = Path(__file__).resolve().parent
CAPTURE_DIR = HERE / "capture"

#: The GM family a program belongs to, as the heading a picker groups by. The
#: numbers are in the label because a set is chosen by program at least as often
#: as by name.
FAMILY_NAMES: dict[int, str] = {
    0: "0-7 Piano",
    8: "8-15 Chromatic percussion",
    16: "16-23 Organ",
    24: "24-31 Guitar",
    32: "32-39 Bass",
    40: "40-47 Strings",
    48: "48-55 Ensemble",
    56: "56-63 Brass",
    64: "64-71 Reed",
    72: "72-79 Pipe",
    80: "80-87 Synth lead",
    88: "88-95 Synth pad",
    96: "96-103 Synth effects",
    104: "104-111 Ethnic",
    112: "112-119 Percussive",
    120: "120-127 Sound effects",
}

DRUM_GROUP = "Drum kit"

#: GM kit numbers, as the program that selects them on the drum channel. A kit
#: is one entry of the index rather than 47: the phrase set is written for the
#: relations between its pieces — the mute group, the tom series, a groove —
#: and none of those is a property of a single note.
KIT_NAMES: dict[int, str] = {
    0: "Standard kit", 8: "Room kit", 16: "Power kit", 24: "Electronic kit",
    25: "TR-808 kit", 32: "Jazz kit", 40: "Brush kit", 48: "Orchestra kit",
    56: "Sound-effects kit",
}


def _slugify(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


@dataclass(frozen=True)
class Capture:
    """A capture definition, read for what an audition page needs from it.

    The definition is the only place the reference instrument is named, and it
    already carries the program it answers, the phrase set it wants and its
    timbres. Read here rather than re-derived so a page and a fit cannot end up
    describing different instruments.
    """

    path: Path
    id: str
    label: str
    program: int
    bank: int
    take_set: str
    timbres: tuple[dict, ...]
    dry: bool
    title: str
    #: The definition as read, overlay folded in. `capture.source_for` wants the
    #: whole thing — the plugin triple, the params, the preset of a timbre —
    #: and mirroring those fields here would be a second copy that can disagree.
    raw: dict

    @property
    def drums(self) -> bool:
        """Whether this capture's notes select instruments rather than pitches."""
        return is_drum_set(self.take_set)


def load_capture(path: Path) -> Capture | None:
    """One capture definition, or None if it names no phrase set.

    A capture with no `takes` cannot be auditioned; it is still a perfectly good
    fitting target, so that is a skip rather than an error.
    """
    cfg = load_config(path)
    take_set = cfg.get("takes") or ""
    if not take_set:
        return None
    return Capture(
        path=path,
        id=cfg["id"],
        label=cfg.get("label", cfg["id"]),
        program=int(cfg.get("program", 0)),
        bank=int(cfg.get("bank", 0)),
        take_set=take_set,
        timbres=tuple(cfg.get("timbres") or ()),
        dry=bool(cfg.get("dry", True)),
        title=cfg.get("audition_title", f"libsonare vs {cfg.get('label', 'reference')}"),
        raw=cfg,
    )


def captures() -> list[Capture]:
    """Every capture definition in `capture/`, with its local overlay folded in.

    The overlay names a commercial product and is untracked, so a clone without
    one still gets the program, the phrase set and the timbre ids — everything
    the index needs to say a reference exists. What it will not get is the
    plugin, which only matters when a take has to be rendered rather than read
    from the archive.
    """
    found = [load_capture(p) for p in sorted(CAPTURE_DIR.glob("*.json"))
             if not p.name.endswith(".local.json")]
    return [c for c in found if c is not None]


def capture_for(program: int, bank: int = 0, *, kit: bool = False,
                pool: list[Capture] | None = None) -> Capture | None:
    """The capture covering a voice, if one does.

    Program 0 is both the grand piano and the standard kit, so the kit flag is
    part of the key rather than something to resolve afterwards. A melodic
    capture matches its own bank exactly — a variation is a separate patch with
    separate knobs, and a reference registered for one of them says nothing
    about another.
    """
    for cap in (captures() if pool is None else pool):
        if cap.drums != kit:
            continue
        if cap.program == program and (kit or cap.bank == bank):
            return cap
    return None


@dataclass(frozen=True)
class Voice:
    """One entry of the bank: something the library can be asked to sound."""

    program: int
    bank: int = 0
    #: Whether `program` selects a drum kit on channel 10 rather than a melodic
    #: instrument on channel 1. The two share a number space and nothing in the
    #: number tells them apart.
    kit: bool = False
    #: The patch the library reports as voicing this entry, when a catalogue
    #: from a tuning build was available. Empty otherwise, which is a missing
    #: reading rather than a missing patch.
    patch: str = ""
    capture: Capture | None = None

    @property
    def name(self) -> str:
        """What the instrument is called."""
        if self.kit:
            return KIT_NAMES.get(self.program, f"kit {self.program}")
        return gm_name(self.program)

    @property
    def tone(self) -> ToneClass:
        # Every piece of a kit is measured by finding its partials rather than
        # by predicting them, which is what `MODAL` means here.
        return ToneClass.MODAL if self.kit else tone_class(self.program)

    @property
    def take_set(self) -> str:
        """The phrase set this voice is auditioned on.

        A capture's own set wins, and not only because it was chosen for the
        instrument: the reference archive is keyed by take id, so a voice given
        a different set from the one its references were rendered against would
        find none of them and silently drop to model-only.
        """
        if self.capture is not None:
            return self.capture.take_set
        return "drums" if self.kit else take_set_for(self.program)

    @property
    def slug(self) -> str:
        """The directory name, and so the id every link to this voice carries."""
        if self.kit:
            return f"kit{self.program:03d}-{_slugify(self.name)}"
        stem = f"p{self.program:03d}"
        if self.bank:
            stem += f"b{self.bank:03d}"
        return f"{stem}-{_slugify(self.name)}"

    @property
    def group(self) -> str:
        """The heading a set picker files this voice under."""
        if self.kit:
            return DRUM_GROUP
        return FAMILY_NAMES.get((self.program // 8) * 8, "other")

    @property
    def label(self) -> str:
        """A one-line name for the voice, bank and all."""
        if self.kit:
            return f"kit {self.program} — {self.name}"
        if self.bank:
            return f"program {self.program} bank {self.bank} — {self.name}"
        return f"program {self.program} — {self.name}"

    @property
    def title(self) -> str:
        """The page title: what is being compared, or what is being played."""
        if self.capture is not None:
            return f"{self.label} vs {self.capture.label.split(',')[0]}"
        return f"{self.label} — libsonare NativeSynth"

    def describe(self) -> dict:
        """The `voice` block of a manifest: what is sounding, in one object.

        A page whose only identification is a directory name says which program
        was asked for and not which of the library's voices answered — and those
        differ whenever a fallback table is edited, which is exactly when a page
        is being listened to.
        """
        out = {
            "program": self.program,
            "bank": self.bank,
            "name": self.name,
            "tone_class": self.tone.value,
            "take_set": self.take_set,
            "group": self.group,
        }
        if self.kit:
            out["kit"] = True
        if self.patch:
            out["patch"] = self.patch
        if self.capture is not None:
            out["capture"] = self.capture.id
        return out


def parse_selection(spec: str, *, limit: int = 128) -> list[int]:
    """`0-7,40,73` or `all` to a sorted list of numbers, deduplicated."""
    spec = spec.strip()
    if not spec:
        return []
    if spec == "all":
        return list(range(limit))
    out: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part.lstrip("-"):
            lo, _, hi = part.partition("-")
            out.update(range(int(lo), int(hi) + 1))
        else:
            out.add(int(part))
    bad = sorted(n for n in out if not 0 <= n < limit)
    if bad:
        raise ValueError(f"out of range 0..{limit - 1}: {', '.join(map(str, bad))}")
    return sorted(out)


def voices(programs: list[int] | None = None, *, banks: list[int] | None = None,
           kits: list[int] | None = None, catalogue=None) -> list[Voice]:
    """The bank entries a run was asked for, in program order with kits last.

    `banks` names variation banks explicitly. Given none, a catalogue expands
    each program to every bank the library voices apart and a run without one
    stays at bank 0 — the capital tone, which is what a MIDI file selects unless
    it says otherwise.
    """
    pool = captures()
    out: list[Voice] = []
    for program in (range(128) if programs is None else programs):
        if banks is not None:
            program_banks = banks
        elif catalogue is not None:
            program_banks = catalogue.banks_for(program) or [0]
        else:
            program_banks = [0]
        for bank in program_banks:
            patch = (catalogue.patch_for(program, bank) or "") if catalogue else ""
            out.append(Voice(
                program=program, bank=bank, patch=patch,
                capture=capture_for(program, bank, pool=pool),
            ))
    for kit in (kits or []):
        out.append(Voice(
            program=kit, kit=True,
            capture=capture_for(kit, kit=True, pool=pool),
        ))
    return out


def index_path(root: Path) -> Path:
    return root / "bank.json"


def write_index(root: Path, entries: list[Voice]) -> Path:
    """A machine-readable list of the voices this directory holds pages for.

    `serve.py` discovers sets from the filesystem and needs nothing from this;
    it exists so a run can be diffed against the previous one — which voice
    gained a reference, which changed patch — without listening to any of them.

    Merged by slug rather than overwritten, because a directory is filled a few
    programs at a time and an index describing only the last command would
    contradict the directory it sits in.
    """
    root.mkdir(parents=True, exist_ok=True)
    path = index_path(root)
    held: dict[str, dict] = {}
    if path.exists():
        try:
            held = {e["slug"]: e for e in json.loads(path.read_text())}
        except (OSError, ValueError, KeyError, TypeError):
            held = {}
    for voice in entries:
        held[voice.slug] = dict(voice.describe(), slug=voice.slug, title=voice.title)
    payload = sorted(held.values(), key=lambda e: e["slug"])
    path.write_text(json.dumps(payload, indent=2) + "\n")
    return path
