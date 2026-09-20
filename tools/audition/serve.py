"""Serve directories of audio renders as an A/B listening page.

    python tools/audition/serve.py [<audition-dir> ...]

Opens a browser on a page that plays every version of a take from one
transport, so switching between them is instant. Two renders compared by
stopping one and starting the other are compared across a gap of a second or
two, which is long enough for the ear to lose what it was holding; here they
run together and the switch costs nothing.

A switch seeks back to the start by default, so the attack is the first thing
heard. Turning that off leaves the versions sample-aligned instead — the switch
becomes a plain gain change on a sound that never stopped, which is what a
sustain or a decay has to be compared on.

More than one directory can be served at once, and the page gets a control to
move between them. Each is a separate instrument or a separate experiment: a
piano set and a harpsichord set have different takes and different references,
so they are separate sets rather than one long list, and one server serves both.

WHICH SIDE OF THE COMPARISON IS SOUNDING IS THE PAGE'S FIRST JOB. Each source
role owns a colour — the library's renders in one, the reference in the other —
and it holds on the switch, on the banner above the transport, on both pictures
and on every note the page has taken. One key swaps between the two sides.

WHAT WAS HEARD IS SAID ON THE PAGE. Two answers at a time, in a listener's own
words rather than in the names of the parameters underneath, and "not sure" is
always a third. Each note lands as a line of JSON under the scratch root next to
what was sounding when it was written. Nothing has to be exported, found and
pasted somewhere, which is the trip most of what gets heard never made.

WHAT IS SOUNDING IS ADDRESSABLE. Every set, take and version has an address —
`#<set>/<take>/<version>` — which the page rewrites as it is navigated, and the
per-set form is printed below on startup. A listening report that names the
wrong render is worse than none, and once several sets of one instrument are up
at once, each holding a reference, an unmodified build and a few candidate
settings, nobody can be sure from memory which one they heard.

ONE SERVER, FOR AS LONG AS THE WORK LASTS. The set list is rebuilt on every
request for it, so a set rendered after the server started shows up on a
refresh; and starting this again while it is running does not start a second
one, it opens a browser on the first. Between them there is never a reason to
pick a different port, which is what stops a tuning session ending with a row of
servers each showing a subset of the renders.

WHAT IS SERVED, AND WHAT IS NOT. Given no directory, the sets are discovered
under the scratch root the rest of the harness uses — `.cache/voicematch/` in
the checkout, or wherever `SONARE_VOICEMATCH_ROOT` points. None of it is
committed, which matters for anyone who has only cloned the repository: there
is nothing there until they render something, and the reference side cannot be
rendered at all without the commercial plugin it captures. So a set holding a
single version of each take is expected rather than broken, and the page turns
the comparison controls off for it and plays instead. Nothing here requires a
reference to exist.

The directory needs a `manifest.json` naming the takes and their versions. If
there is none, one is inferred from the layout — each subdirectory is a take
and each WAV inside it is a version of it — so a directory somebody assembled
by hand still opens.

Nothing here is specific to any instrument or to this repository: point it at
any directory of renders. Only the standard library is used, so it runs from
any interpreter without an environment.
"""

from __future__ import annotations

import argparse
import datetime
import http.server
import json
import os
import re
import socketserver
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from pathlib import Path
from typing import ClassVar

APP_DIR = Path(__file__).resolve().parent
REPO_ROOT = APP_DIR.parents[1]
AUDIO_SUFFIXES = (".wav", ".flac", ".mp3", ".ogg", ".m4a", ".aac")
#: The scratch root the whole harness renders into, and the one `capture.py`
#: reads. Untracked on purpose — the reference side of a comparison is captured
#: from a commercial plugin and cannot be redistributed — so a fresh clone finds
#: nothing here, which is a supported state rather than a failure.
#: Resolved, because `discover` resolves what it finds and `set_id` compares a
#: set's parent against this: one symlink anywhere above the checkout and the
#: two spellings stop matching, which would rename every set silently.
SCRATCH_ROOT = Path(
    os.environ.get("SONARE_VOICEMATCH_ROOT") or REPO_ROOT / ".cache" / "voicematch"
).expanduser().resolve()
#: Searched under the scratch root, in order. The second form is for a root that
#: holds one set directly rather than a directory of them.
FALLBACK_GLOBS = ("audition/*", "audition", "*/audition")

#: The bank view's source, generated by `tools/voicematch/status.py` and
#: committed. Read per request like the set index, so a refresh after
#: `make voice-status-refresh` shows the new stages.
BANK_PATH = REPO_ROOT / "tools" / "voice-status.json"

#: Which slot of the map a page is of, and what its reference was allowed to be.
#: Both tracked, both read per request and never written into a render: a page
#: rendered in June is still the page of a slot whose policy moved in September,
#: and a manifest carrying a copy would say the old answer until somebody spent
#: an hour of plugin time re-rendering audio that did not change. Same reason
#: the tier is resolved when `status.py` prints rather than baked into
#: `voice-status.json`.
POLICY_PATH = REPO_ROOT / "tools" / "voicematch" / "policy.json"
CAPTURE_DIR = REPO_ROOT / "tools" / "voicematch" / "capture"

#: The one source class a `machine` timbre axis can be answered by: a slot
#: naming a sound the machine invented has nothing standing behind it for a
#: recording to be made of. The rule belongs to
#: `tests/conformance/check_bank_policy.py`, which counts the slots that break
#: it; it is spelled again here because this server imports nothing outside the
#: standard library and the tools tree needs numpy.
MACHINE_SOURCE = "module"

#: Where the page writes what a listener said, one file of JSON lines per voice.
#: Under the scratch root rather than in the tree: a listening note is taken
#: against renders that are themselves untracked, and a note whose subject no
#: longer exists is worse than no note. Append-only, so two tabs open on two
#: voices cannot lose each other's lines.
FEEDBACK_ROOT = SCRATCH_ROOT / "feedback"

#: A listener can hold down a key on a textarea; nothing here needs more room
#: than a paragraph, and an unbounded read from a local socket is still a way to
#: fill a disk by accident.
MAX_FEEDBACK_BYTES = 64 * 1024


def feedback_path(set_id: str) -> Path | None:
    """The log file for a set, or None if the name could not be one.

    Sanitised rather than looked up in `Sets`, because the page is usable
    against any directory of renders and a set served by something else is still
    entitled to a log. `..` and an empty name are the two spellings that would
    escape the directory, and both come back as None.
    """
    name = re.sub(r"[^a-zA-Z0-9._-]+", "-", set_id or "").strip("-.")
    if not name:
        return None
    return FEEDBACK_ROOT / f"{name}.jsonl"


def read_feedback(path: Path) -> list[dict]:
    """Every entry in a log, skipping any line that is not one.

    A truncated final line is what a crash mid-append leaves, and dropping it is
    the whole recovery: the file is a log rather than a document.
    """
    try:
        text = path.read_text()
    except OSError:
        return []
    out: list[dict] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except ValueError:
            continue
    return out


#: Verdicts worst first. The order is what a one-glance marker has to show: a
#: voice that barely sounds outranks anything about its colour, so a list of
#: 180 entries marks each with the worst thing said about it rather than the
#: last. Kept in step with the tree's verdict labels in `i18n.js`.
GRADE_ORDER = ("broken", "wrong-instrument", "off", "acceptable", "ok")


def feedback_index() -> dict[str, dict]:
    """Per set: how much has been said about it, and the worst of it.

    The picker and the bank both list every voice, and "has anybody listened to
    this one" is the fact neither could answer — the log is one file per voice
    and reading 180 of them one request at a time is not a thing a list does.
    Scanned per request like the set index, so a note taken in one tab shows up
    in another's list on a refresh.
    """
    out: dict[str, dict] = {}
    for path in sorted(FEEDBACK_ROOT.glob("*.jsonl")):
        entries = read_feedback(path)
        if not entries:
            continue
        grades = [str(e.get("grade") or "") for e in entries]
        worst = next((g for g in GRADE_ORDER if g in grades), "")
        out[path.stem] = {
            "n": len(entries),
            "worst": worst,
            # A preference ranks candidates and carries no verdict, so it is
            # counted apart: a voice with three preferences and no grade has
            # been listened to hard and judged not at all.
            "prefer": sum(1 for e in entries if e.get("tag") == "prefer"),
            "last": max((str(e.get("at") or "") for e in entries), default=""),
        }
    return out


def append_feedback(path: Path, entry: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(entry, ensure_ascii=False) + "\n")


def drop_last_feedback(path: Path) -> None:
    """Undo, which is a rewrite of the file without its last valid entry.

    Offered because a note is sent while the sound is still going and the wrong
    version is one keystroke away; without it the only fix is editing a file by
    hand, which nobody does mid-session.
    """
    entries = read_feedback(path)
    if not entries:
        return
    body = "".join(json.dumps(e, ensure_ascii=False) + "\n" for e in entries[:-1])
    path.write_text(body, encoding="utf-8")


def read_bank() -> dict:
    """Where every voice in the bank stands, or an empty bank if none is generated.

    A clone that has never run the generator, and any use of this page against a
    directory of renders outside this repository, both land here. The page drops
    the bank view rather than showing an error: it is a second view of a tool
    whose first view works without it.
    """
    try:
        return json.loads(BANK_PATH.read_text())
    except (OSError, ValueError):
        return {"voices": []}


def drum_names() -> dict[str, str]:
    """The GM drum map, note number to instrument name.

    A kit is one part holding forty-odd instruments, so a kit page's "which
    slot" has a second level the melodic pages do not: the note IS the
    instrument. Imported from the table the rest of the harness names drums
    from rather than mirrored here -- a second copy of a map is a copy that
    disagrees — and empty where this is serving a directory outside the
    repository, which costs the names and nothing else.
    """
    tools = REPO_ROOT / "tools" / "voicematch"
    if str(tools) not in sys.path:
        sys.path.append(str(tools))
    try:
        from gm_names import GM_DRUM_NAMES
    except ImportError:
        return {}
    return {str(note): name for note, name in GM_DRUM_NAMES.items()}


def _calibration():
    """The module that owns the registry, or None where it cannot be read.

    Imported rather than parsed, so the refusal of an unlabelled setting is
    enforced in one place. None where this is serving a directory outside the
    repository, which costs the words and nothing else — a page whose buttons
    fall back to their own keys is the state before any of this existed.
    """
    tools = REPO_ROOT / "tools" / "voicematch"
    if str(tools) not in sys.path:
        sys.path.append(str(tools))
    try:
        import calibration
    except ImportError:
        return None
    return calibration


def recorded_variants(slug: str) -> dict:
    """The named settings recorded for one voice, by name."""
    calibration = _calibration()
    if calibration is None:
        return {}
    try:
        table = calibration.load()
    except (ValueError, OSError):
        return {}
    return {v.name: v for v in table.get(slug, [])}


def label_sources(manifest: dict, slug: str) -> bool:
    """Give every version the words and the axis its button needs.

    Three things, all resolved per request for the same reason the slot is: they
    are tracked facts that move on their own, and the hundred and eighty-odd
    pages already rendered were written before any of them existed. Re-rendering
    one to read its own button is hours of audio for a sentence.

    - `title` and `desc`, in both languages, for a version the registry names.
    - `path`, for a render taken with the rig cleared. The signal path is an
      axis and not a choice, so the switch has to be able to put it in its own
      block instead of interleaving it with the candidates.
    - `detail` with any override string taken off it. A listener shown the knob
      answers about the knob, and every page rendered so far carries it.

    A manifest that already carries the words keeps them — a render says what
    the setting was when it was made — and anything the registry does not name
    (the unmodified build, a reference, a `--variant` typed at a shell) keeps
    whatever it had.
    """
    sources = manifest.get("sources")
    calibration = _calibration()
    if not isinstance(sources, dict) or calibration is None:
        return False
    variants = recorded_variants(slug)
    changed = False
    for key, src in sources.items():
        if not isinstance(src, dict):
            continue
        direct = key.endswith("-di")
        if direct and not src.get("path"):
            src["path"] = "direct"
            changed = True
        detail = src.get("detail")
        if isinstance(detail, str) and detail:
            plain = calibration.strip_overrides(detail)
            if plain != detail:
                src["detail"] = plain
                changed = True
        found = variants.get(key[:-3] if direct else key)
        if not found or src.get("title"):
            continue
        src.update(calibration.source_text(found, direct=direct))
        changed = True
    return changed


def _read_json(path: Path) -> dict:
    """A JSON object, or an empty one wherever the file is missing or broken.

    Every caller below is adding context to a page that works without it, so a
    policy file somebody is midway through editing costs the context rather than
    the page.
    """
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return loaded if isinstance(loaded, dict) else {}


def capture_facts(capture_id: str) -> dict:
    """Where a reference came from, from the definition and its local overlay.

    Two files and the split is deliberate: the tracked definition classifies the
    source — module, dedicated instrument, or sample library — and the untracked
    `<id>.local.json` names the product. The class is the fact a listener needs
    and the identity is the one that may not be committed, so a clone without the
    overlay still gets told what kind of thing it is hearing.
    """
    cfg = _read_json(CAPTURE_DIR / f"{capture_id}.json")
    if not cfg:
        return {}
    local = _read_json(CAPTURE_DIR / f"{capture_id}.local.json")
    return {
        "id": capture_id,
        "label": cfg.get("label") or capture_id,
        "source_class": cfg.get("source_class") or "",
        "product": local.get("label") or "",
        "dry": bool(cfg.get("dry", True)),
        "room": cfg.get("room") or "",
    }


def wanted_layer(policy: dict, program: int, kit: bool, bank: int = 0) -> dict:
    """Which kind of reference this slot is aimed at, and why.

    Two branches are selected by a flag rather than by a program number, because
    both share the program space with something else and the number cannot tell
    them apart. A kit takes the kit branch whatever its number, or a kit selected
    by a program some branch also names would be answered as that melodic voice.
    A GS variation takes the variation branch whenever its bank is non-zero,
    because a variation carries its capital's program: dispatching it by number
    resolves it to `default`, which wants an instrument, and the caller's
    off-target test then cannot fire on any variation at all. Otherwise a branch
    naming this program wins, and `default` takes everything left.
    """
    branches = policy.get("reference_layer")
    if not isinstance(branches, dict):
        return {}
    named = {k: v for k, v in branches.items()
             if isinstance(v, dict) and not k.startswith("_")}
    chosen = ""
    if kit and "kits" in named:
        chosen = "kits"
    elif bank and "variations" in named:
        chosen = "variations"
    for name, branch in named.items():
        if not chosen and program in (branch.get("programs") or []):
            chosen = name
    if not chosen:
        chosen = "default"
    branch = named.get(chosen)
    if not branch:
        return {}
    return {
        "branch": chosen,
        "timbre": branch.get("timbre") or "",
        "behaviour": branch.get("behaviour") or "",
        "reason": branch.get("reason") or "",
    }


def provenance(voice: dict, ident: str) -> dict:
    """Which slot of the map this page is of, and where its reference came from.

    The two are one question. A page names a program and plays a reference, and
    nothing on it used to say whether that reference is the kind of thing the
    slot is supposed to be aimed at — so forty slots whose target is the module
    are auditioned against a sample library's idea of the same sound, which
    sounds like a finished voice and is a gap. `state` is that comparison:

    `aimed`         the reference answers the layer the policy asks for
    `off-target`    it cannot — the slot wants the module and this is not it
    `unclassified`  the capture says nothing about what answered it
    `declined`      the policy holds this address to have no usable reference
    `uncaptured`    nothing covers this slot yet
    """
    if not isinstance(voice, dict):
        return {}
    policy = _read_json(POLICY_PATH)
    program = voice.get("program")
    if not isinstance(program, int):
        return {}
    kit = bool(voice.get("kit"))
    bank = voice.get("bank")
    want = wanted_layer(policy, program, kit, bank if isinstance(bank, int) else 0)
    declined = (policy.get("no_reference") or {}).get(ident)
    capture = capture_facts(voice.get("capture") or "") if voice.get("capture") else {}

    if capture:
        cls = capture["source_class"]
        if not cls:
            state = "unclassified"
        elif want.get("timbre") == "machine" and cls != MACHINE_SOURCE:
            state = "off-target"
        else:
            state = "aimed"
    elif isinstance(declined, dict):
        state = "declined"
    else:
        state = "uncaptured"

    out = {"state": state, "want": want}
    if kit:
        # A kit's unit of work is the drum note, not the kit: `bank-versions`
        # versions each of `d000`-`d127` separately and a verdict on "the kit"
        # cannot be attributed to any of them.
        out["drum_names"] = drum_names()
    if capture:
        out["capture"] = capture
    if isinstance(declined, dict):
        # The address's own name on the machine, which is the one place the GS
        # map is written down as data rather than as a comment beside a patch.
        out["declined"] = {
            "names": declined.get("names") or "",
            "carries": declined.get("carries") or "",
            "reason": declined.get("reason") or "",
        }
    return out


def infer_manifest(root: Path) -> dict:
    """Build a manifest from a directory that has none.

    A subdirectory holding two or more audio files is a take whose versions are
    those files. A directory of loose files is one take per file, which gives a
    page that plays rather than compares — still useful, and honest about
    having nothing to compare against.
    """
    items = []
    for sub in sorted(p for p in root.iterdir() if p.is_dir()):
        tracks = {p.stem: str(p.relative_to(root)) for p in sorted(sub.iterdir())
                  if p.suffix.lower() in AUDIO_SUFFIXES}
        if tracks:
            items.append({"id": sub.name, "label": sub.name, "tracks": tracks})
    loose = [p for p in sorted(root.iterdir())
             if p.is_file() and p.suffix.lower() in AUDIO_SUFFIXES]
    for p in loose:
        items.append({"id": p.stem, "label": p.stem, "tracks": {p.stem: p.name}})
    return {
        "title": root.name,
        "notes": "Inferred from the directory layout; no manifest.json was found.",
        "items": items,
    }


def read_manifest(root: Path) -> dict:
    explicit = root / "manifest.json"
    if explicit.exists():
        return json.loads(explicit.read_text())
    return infer_manifest(root)


#: Leaf names that say nothing about which set a directory holds.
GENERIC_NAMES = ("audition", "renders", "out")


def set_id(root: Path) -> str:
    """A short URL-safe name for a set, and the one its links carry.

    Taken from the parent directory when the leaf says nothing, so that two sets
    both written to a directory called `audition` are still told apart by the
    directory holding them — except directly under the scratch root, where the
    parent is the scratch root and names the harness rather than the set.
    """
    name = root.name
    if name in GENERIC_NAMES and root.parent not in (root, SCRATCH_ROOT):
        name = root.parent.name
    return re.sub(r"[^a-zA-Z0-9._-]+", "-", name).strip("-") or "set"


def take_dirs(root: Path) -> set[Path]:
    """The directories a set's own manifest names as holding its takes."""
    mf = root / "manifest.json"
    if not mf.exists():
        return set()
    try:
        manifest = json.loads(mf.read_text())
    except (OSError, ValueError):
        return set()
    out: set[Path] = set()
    for item in manifest.get("items", []):
        for rel in (item.get("tracks") or {}).values():
            parent = (root / rel).parent.resolve()
            if parent != root:
                out.add(parent)
    return out


def is_set(root: Path) -> bool:
    """Whether this directory holds renders of its own.

    A manifest settles it. Without one the layout does: a directory whose
    subdirectories hold audio, or which holds loose audio itself, is a set, and
    one holding neither is a container.
    """
    if (root / "manifest.json").exists():
        return True
    try:
        return bool(infer_manifest(root).get("items"))
    except OSError:
        return False


def expand(root: Path) -> list[Path]:
    """A named directory, or the sets inside it when it is a directory of sets.

    One level, and only when the directory is not a set itself. A run that
    renders several voices writes one set per voice under a root, and naming
    that root is the obvious way to ask for all of them — without this it is
    reported as empty, which is true of the root and false of what is in it.
    """
    if not root.is_dir() or is_set(root):
        return [root]
    inside = [p for p in sorted(root.iterdir()) if p.is_dir() and is_set(p)]
    return inside or [root]


def discover(paths: list[str]) -> list[Path]:
    """The directories to serve: the ones named, else whatever the scratch root holds."""
    if paths:
        named = [Path(p).expanduser().resolve() for p in paths]
        return [q for q in dict.fromkeys(r for p in named for r in expand(p))
                if not is_probe(q)]
    found: list[Path] = []
    for pattern in FALLBACK_GLOBS:
        found += [p.resolve() for p in sorted(SCRATCH_ROOT.glob(pattern)) if p.is_dir()]
    unique = list(dict.fromkeys(found))
    # A directory a set's manifest names as one of its own takes is part of that
    # set, not a set beside it. The default output directory is the parent of
    # every named one, so `audition/*` matches a set's take directories as
    # readily as it matches the sets: a kit set of six takes came out as six
    # play-only "sets" of two versions each -- and the kit set itself was gone,
    # dropped by the leaf rule below for being their parent.
    claimed: set[Path] = set()
    for p in unique:
        claimed |= take_dirs(p)
    unique = [p for p in unique if p not in claimed]
    # A directory with a manifest is a set whatever it contains. One without is
    # a set only if it is a leaf: two of these globs can match a directory and
    # its parent, and a parent holding no renders of its own would otherwise put
    # a name in the picker that plays nothing.
    return [p for p in unique
            if not is_probe(p)
            and ((p / "manifest.json").exists()
                 or not any(p in other.parents for other in unique))]


def is_probe(path: Path) -> bool:
    """Whether this set is a measurement run rather than something to listen to.

    A component-isolation run renders a voice with parts of it switched off. It
    is a legitimate thing to produce and a nonsense thing to put on a picker
    beside the real pages of the same voice, where it reads as another candidate
    version. The flag is in the manifest rather than in the path so a probe
    named explicitly on the command line is still skipped.
    """
    manifest = path / "manifest.json"
    if not manifest.exists():
        return False
    try:
        return bool(json.loads(manifest.read_text()).get("probe"))
    except (OSError, ValueError):
        return False


class Sets:
    """The served sets, by id, and the index the page reads to list them.

    Reloaded on every request for the index rather than once at startup, so one
    server outlives the renders it is showing. A tuning session produces a set
    every few minutes and the alternative is a server -- and a port, and a stale
    browser tab -- per set, which is how eight of them came to be running at
    once. Discovery is a handful of `glob` calls against a directory that holds
    tens of entries, so doing it per index request costs nothing worth naming.
    """

    #: What `main` was asked to serve, so a reload can repeat the same search.
    paths: ClassVar[list[str]] = []
    by_id: ClassVar[dict[str, Path]] = {}
    index: ClassVar[list[dict]] = []

    @classmethod
    def reload(cls) -> None:
        cls.load(discover(cls.paths))

    @classmethod
    def load(cls, roots: list[Path]) -> None:
        cls.by_id = {}
        cls.index = []
        for root in roots:
            if not root.is_dir():
                print(f"skipping (not a directory): {root}", file=sys.stderr)
                continue
            ident = set_id(root)
            # Two directories with the same leaf name would otherwise shadow
            # each other, and the second would silently never be reachable.
            base = ident
            n = 2
            while ident in cls.by_id:
                ident = f"{base}-{n}"
                n += 1
            manifest = read_manifest(root)
            items = manifest.get("items", [])
            if not items:
                # An empty directory is not a set. Listing it would put a name in
                # the picker that plays nothing.
                print(f"skipping (no renders in it): {root}", file=sys.stderr)
                continue
            # A set whose takes each hold one version has nothing to compare, so
            # the page drops the comparison controls rather than showing a
            # switcher with one entry.
            compare = any(len(it.get("tracks", {})) > 1 for it in items)
            cls.by_id[ident] = root
            cls.index.append({
                "id": ident,
                "title": manifest.get("title") or root.name,
                "takes": len(items),
                "compare": compare,
                # A heading the picker files this set under. Optional and
                # generic — the manifest says what it is, this only carries it
                # through — but a picker of a hundred and thirty sets is a wall
                # of names without one.
                "group": manifest.get("group") or "",
                "path": str(root),
            })


class Handler(http.server.SimpleHTTPRequestHandler):
    """Serve the app from `tools/audition/` and the audio from the render dirs."""

    app_dir = APP_DIR

    def _set_and_rest(self, rel: str) -> tuple[Path | None, str]:
        """Split `s/<id>/<rest>` into the set's root and the path inside it."""
        if not rel.startswith("s/"):
            return None, rel
        parts = rel[2:].split("/", 1)
        root = Sets.by_id.get(parts[0])
        return root, (parts[1] if len(parts) > 1 else "")

    def _resolve(self, rel: str) -> Path | None:
        """The file a request names, or None if it names nothing servable.

        A path that resolves outside its own set's directory is None rather than
        an error page's worth of detail: `..` in a URL is not a mistake anyone
        makes by accident.
        """
        if rel in ("", "index.html"):
            return self.app_dir / "index.html"
        # The page is ES modules, so its own files are a set rather than two
        # names: a leaf name with no separator in it cannot leave this
        # directory, and the file has to already be here.
        if re.fullmatch(r"[a-z0-9_-]+\.(?:js|css)", rel) and (self.app_dir / rel).is_file():
            return self.app_dir / rel
        root, rest = self._set_and_rest(rel)
        if root is None or not rest:
            return None
        target = (root / rest).resolve()
        return target if root in target.parents else None

    def translate_path(self, path: str) -> str:
        rel = path.split("?", 1)[0].split("#", 1)[0].lstrip("/")
        target = self._resolve(rel)
        # Unreachable in practice: do_GET answers before this is consulted. The
        # path keeps SimpleHTTPRequestHandler's other verbs from serving the
        # process's working directory if one is ever added.
        return str(target) if target is not None else str(self.app_dir / "index.html")

    def _json(self, payload) -> None:
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _query(self) -> dict[str, list[str]]:
        parts = self.path.split("?", 1)
        return urllib.parse.parse_qs(parts[1]) if len(parts) > 1 else {}

    def do_GET(self) -> None:
        rel = self.path.split("?", 1)[0].lstrip("/")
        if rel == "sets.json":
            # Re-scan here, so a set rendered after the server started appears
            # on a refresh instead of needing a second server.
            Sets.reload()
            self._json(Sets.index)
            return
        if rel == "bank.json":
            self._json(read_bank())
            return
        if rel == "feedback-index.json":
            self._json(feedback_index())
            return
        if rel == "feedback.json":
            path = feedback_path((self._query().get("set") or [""])[0])
            if path is None:
                self._json({"entries": [], "path": ""})
                return
            self._json({"entries": read_feedback(path), "path": str(path)})
            return
        root, rest = self._set_and_rest(rel)
        if root is not None and rest == "manifest.json":
            # Folded in here rather than at render time: what the policy asks of
            # a slot is a tracked fact that moves on its own, and a manifest
            # carrying a copy of it would have to be re-rendered to change its
            # mind. A set served from outside this repository gets no block and
            # the page drops the line.
            manifest = read_manifest(root)
            slug = rel[2:].split("/", 1)[0]
            found = provenance(manifest.get("voice") or {}, slug)
            if found:
                manifest["provenance"] = found
            label_sources(manifest, slug)
            self._json(manifest)
            return
        if self._resolve(rel) is None:
            self.send_error(404, "not found")
            return
        super().do_GET()

    def do_POST(self) -> None:
        """Take one listening note, or undo the last one.

        The page is the only client and it is served from this process, so the
        body is trusted to be JSON and nothing else is accepted: the endpoint
        exists so that what somebody heard reaches the tree while they are still
        hearing it, and every field it stores came off the page's own readouts
        rather than out of anyone's memory.
        """
        if self.path.split("?", 1)[0].lstrip("/") != "feedback":
            self.send_error(404, "not found")
            return
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_FEEDBACK_BYTES:
            self.send_error(413, "too large")
            return
        try:
            payload = json.loads(self.rfile.read(length))
        except ValueError:
            self.send_error(400, "not JSON")
            return
        if not isinstance(payload, dict):
            self.send_error(400, "not an object")
            return

        conditions = payload.get("conditions") or {}
        set_id = payload.get("set") or conditions.get("set") or ""
        path = feedback_path(set_id)
        if path is None:
            self.send_error(400, "no set")
            return

        if payload.get("op") == "undo":
            drop_last_feedback(path)
        else:
            append_feedback(path, {
                "at": datetime.datetime.now(datetime.timezone.utc)
                      .replace(microsecond=0).isoformat(),
                # The verdict is kept apart from the finer tag: "recognisably
                # the instrument and I would still change it" and "this is a
                # different instrument" are the same `onset/hard` underneath,
                # and only one of them is a defect.
                "grade": payload.get("grade") or "",
                "tag": payload.get("tag") or "",
                "answers": payload.get("answers") or [],
                "text": payload.get("text") or "",
                "lang": payload.get("lang") or "",
                "conditions": conditions,
            })
        self._json({"entries": read_feedback(path), "path": str(path)})

    def end_headers(self) -> None:
        # A render is overwritten in place by the next tuning iteration, and a
        # cached one would be audited instead of the new one.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt: str, *args) -> None:
        if "404" in (fmt % args):
            super().log_message(fmt, *args)


def already_serving(port: int) -> bool:
    """Whether an audition server is answering on this port already.

    Checked before binding rather than after failing to, because the useful
    answer to "the port is taken" is almost always "by the one you started an
    hour ago" -- and starting a second server on a second port is what leaves a
    row of them running, each showing a subset of the sets. Anything else
    holding the port is left to the bind to report as itself.
    """
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/sets.json", timeout=1.0) as r:
            json.load(r)
        return True
    except (urllib.error.URLError, OSError, ValueError):
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("directory", nargs="*",
                    help="directories of renders (default: whatever the voicematch "
                         "scratch root holds; SONARE_VOICEMATCH_ROOT moves it)")
    ap.add_argument("--port", type=int, default=8730)
    ap.add_argument("--no-open", action="store_true", help="do not launch a browser")
    args = ap.parse_args()

    # A running server rediscovers its sets on every index request, so one that
    # is already up is showing this set too and there is nothing to start.
    if already_serving(args.port):
        url = f"http://127.0.0.1:{args.port}/"
        print(f"already serving at {url} — refresh it; new sets appear on their own")
        if not args.no_open:
            webbrowser.open(url)
        return 0

    Sets.paths = list(args.directory)
    roots = discover(args.directory)
    Sets.load(roots)
    if not Sets.index:
        # Not an error: a fresh clone has no renders and cannot make the
        # reference side of one at all. Say where they would go and serve the
        # page anyway, so a set rendered now needs only a refresh.
        where = args.directory or [f"{SCRATCH_ROOT}/{g}" for g in FALLBACK_GLOBS]
        print("no renders found in: " + ", ".join(str(w) for w in where), file=sys.stderr)
        print("render one with tools/voicematch/make_audition.py (--model-only needs no plugin)",
              file=sys.stderr)
    url = f"http://127.0.0.1:{args.port}/"
    # The per-set address, not just the name: a set is chosen for someone else
    # to listen to at least as often as for oneself, and `#<set>/<take>/<version>`
    # is the only form of "listen to this one" that cannot be misread.
    for entry in Sets.index:
        kind = "compare" if entry["compare"] else "play only"
        print(f"  {entry['id']:<16} {entry['takes']:>3} takes  [{kind:^10}]  {url}#{entry['id']}")

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", args.port), Handler) as httpd:
        # Where the listening notes land, printed whether or not any have been
        # taken: a log nobody knows the path of is a log nobody reads back.
        print(f"  notes  {FEEDBACK_ROOT}/<set>.jsonl")
        print(f"  {url}   (ctrl-c to stop)")
        if not args.no_open:
            threading.Timer(0.4, lambda: webbrowser.open(url)).start()
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
