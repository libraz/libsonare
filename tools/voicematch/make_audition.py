#!/usr/bin/env python3
"""Render a voice of libsonare's own bank, and any reference it happens to have.

    rye run --pyproject bindings/python/pyproject.toml \
        python tools/voicematch/make_audition.py --program 40

Writes `<take>/model.wav` and one WAV per reference timbre into a directory
outside the repository's tracked tree, plus the `manifest.json` that
`tools/audition/serve.py` reads. Then:

    python tools/audition/serve.py

THE INDEX IS THE BANK, NOT THE CAPTURE LIST. What is auditioned is named as a
GM program, a variation bank or a kit, and `bank.py` resolves it: the phrase
set from the voice's tone class, the reference from whichever capture covers it
if any does. Four captures exist and the bank has 128 programs, so most voices
have no reference at all — those render the model alone and the page plays
rather than compares, which `serve.py` already handles as an ordinary set. A
reference is an attachment to an entry, never the reason the entry exists.

`--config` still names a capture directly, for the case where the capture is
the subject: it fixes the program, the phrase set and the timbres in one, which
is what a calibration page wants.

A phrase set belongs to an instrument rather than to the tool — a harpsichord
has no pedal to lift and a piano has no stops to draw — so each is written for
what its own instrument is hard to get right, and the generic ones are written
per tone class for what a struck, plucked, blown or struck-bar voice has in
common. They live in `phrases.py`.

The takes are chosen for what they can catch by ear that a metric will not
report on its own. A harmonic ladder can be matched note by note while the
instrument still sounds wrong the moment two notes overlap, or the moment a
note is struck again before it has stopped, or the moment the pedal comes up —
because those are couplings between strings rather than properties of one, and
the per-note analysis in `metrics.py` never sees them.

All versions of a take are written at one shared gain, so their level
difference survives into the comparison; the listening page has its own
loudness match for when that difference is in the way.

`--variant` adds candidate settings of the voice as further versions of every
take, each rendered under its own `SONARE_TUNING_OVERRIDES`. That is the form a
listening question usually arrives in — "is this constant better at 0 or at 4"
is not a question the metrics can settle, and the answer has to be heard against
the same phrase and the same reference. It needs a library built with
`-DBUILD_TUNING=ON`; without one the override layer is compiled out and every
variant renders identically, which the tool checks for and reports rather than
producing a page of indistinguishable versions.

A flag applies to every voice in the run, though, so a batch across the bank
cannot carry per-voice candidates — and the settings a listening session decided
something about are gone with the shell history. `--calibrations` reads them from
`calibrations.json` instead, where each voice keeps its own named settings; those
come first on the page and `--variant` adds to them. Off by default: each is one
more render of every take, and a page opened to hear one voice should not pay for
it. See `calibration.py`.

The reference side comes from an archive by default, and only falls through to
the plugin for a take the archive does not hold. That is what makes a page cheap
enough to throw away: the model renders take seconds and can always be made
again from the library plus the overrides the manifest records, while a
reference render is a real-time pass through a commercial plugin and is the one
part that cannot be reproduced from this repository. Kept per page instead, it
was both the bulk of the disk and the reason nobody dared delete a page — and a
directory of pages nobody dares delete stops being a place to look.

    --reference-from DIR     take them from here (default: the archive; empty
                             string to always render)
    --archive-references DIR keep this run's reference renders for the next page

The archive stores each take under a gain computed from its reference renders
ALONE, so it does not move when a page's candidates get louder, and divides that
gain back out on the way in. Only a take whose every reference came from the
plugin in one run is written, so nothing in it has been through 16 bits twice.

`--title` is worth setting on any page built to settle a question. The default
names the voice, which is right until there are two pages of the same voice on
the picker — at which point they read identically and the only way to find the
live one is to open both.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT  # noqa: E402
from au_oracle import AuRenderError, render_oracle_au  # noqa: E402
import calibration  # noqa: E402
from bank import Voice, load_capture, parse_selection, voices, write_index  # noqa: E402
from calibration import Variant  # noqa: E402
from capture import CORPUS_ROOT, source_for  # noqa: E402
from phrases import Take, build_takes  # noqa: E402
from render_model import render_model  # noqa: E402
from smf import write_smf  # noqa: E402
from wavio import read_wav, write_wav  # noqa: E402

SR = 48000
DEFAULT_OUT = CORPUS_ROOT / "audition"
# Reference renders, kept once and outside the audition root so the listening
# server does not offer the archive itself as a set to listen to. A reference
# render costs a real-time pass through a commercial plugin on this machine and
# is the one part of a page that cannot be reproduced from the repository, so
# every page copying its own was both the bulk of the disk and the reason none
# of them could be deleted.
DEFAULT_REFERENCE_ARCHIVE = CORPUS_ROOT / "audition-references"


#: Renders one SMF in a fresh interpreter. The tuning override table is read
#: when the library loads, so two settings of the same constant cannot be
#: rendered by one process -- the second would silently get the first's values.
_VARIANT_WORKER = r'''
import sys
import numpy as np
sys.path.insert(0, "tools"); sys.path.insert(0, "tools/voicematch")
from render_model import render_model
smf, out, seconds, sr = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4])
with open(smf, "rb") as fh:
    a = np.asarray(render_model(fh.read(), seconds, sr), dtype=np.float32)
np.save(out, a.mean(axis=1) if a.ndim > 1 else a)
'''


def render_variant(smf: bytes, seconds: float, sr: int, overrides: str,
                   lib_path: str = "") -> np.ndarray:
    """One take under one override set, in its own interpreter."""
    env = dict(os.environ)
    if lib_path:
        env["SONARE_LIB_PATH"] = lib_path
    if overrides:
        env["SONARE_TUNING_OVERRIDES"] = overrides
    else:
        env.pop("SONARE_TUNING_OVERRIDES", None)
    with tempfile.TemporaryDirectory() as tmp:
        smf_path = Path(tmp) / "take.mid"
        smf_path.write_bytes(smf)
        out_path = Path(tmp) / "render.npy"
        proc = subprocess.run(
            [sys.executable, "-c", _VARIANT_WORKER, str(smf_path), str(out_path),
             str(seconds), str(sr)],
            capture_output=True, text=True, env=env, cwd=str(REPO_ROOT))
        if proc.returncode:
            raise RuntimeError(proc.stderr[-4000:])
        return np.load(out_path)


def digest(audio: np.ndarray) -> str:
    """A render's identity, comparable across the two ways one is produced.

    The baseline comes back stereo from `render_model` and a variant comes back
    mono from the subprocess worker, so the arrays are hashed after a downmix or
    the two could never be equal — and the check that wants them compared is
    exactly the one asking whether a variant changed anything at all.
    """
    mono = audio.mean(axis=1) if audio.ndim > 1 else audio
    return hashlib.sha256(
        np.ascontiguousarray(mono, dtype=np.float32).tobytes()).hexdigest()


def shared_gain(renders: dict[str, np.ndarray], headroom_db: float = -1.0) -> float:
    """One gain for every version of a take, so their level difference survives.

    Normalising each version on its own would erase exactly the thing a
    register-balance or velocity-curve problem shows up as.
    """
    peak = max((float(np.abs(a).max()) for a in renders.values() if a.size), default=0.0)
    if peak <= 0.0:
        return 1.0
    return float(10.0 ** (headroom_db / 20.0) / peak)


def archived_references(archive: Path, capture_id: str, take_id: str,
                        timbres: list[dict]) -> dict[str, np.ndarray]:
    """Reference renders for one take, back at the level the plugin produced.

    The archive stores them under a gain of its own so 16 bits are spent on the
    signal rather than on whatever headroom a particular page needed, and that
    gain is divided out here. One gain per take rather than one per file, so the
    level difference BETWEEN timbres -- which is a real property of the three
    instruments and one of the things a page is read for -- survives the trip.
    """
    index = archive / "index.json"
    if not index.exists():
        return {}
    meta = json.loads(index.read_text()).get(capture_id, {}).get(take_id)
    if not meta:
        return {}
    gain = 10.0 ** (float(meta["gain_db"]) / 20.0)
    if gain <= 0.0:
        return {}
    out: dict[str, np.ndarray] = {}
    for timbre in timbres:
        path = archive / capture_id / take_id / f"{timbre['id']}.wav"
        if not path.exists():
            continue
        audio, sr = read_wav(path)
        # A rate mismatch is a different capture, not a resampling job: the
        # analysis windows and the take's own timing are written for one rate.
        if sr != SR:
            print(f"  {timbre['id']}: archived at {sr} Hz, not {SR} — rendering instead",
                  file=sys.stderr)
            continue
        out[timbre["id"]] = np.asarray(audio, dtype=np.float64) / gain
    return out


def archive_references(archive: Path, capture_id: str, take_id: str,
                       renders: dict[str, np.ndarray]) -> None:
    """Keep this take's reference renders so no later page needs the plugin."""
    if not renders:
        return
    gain = shared_gain(renders)
    directory = archive / capture_id / take_id
    directory.mkdir(parents=True, exist_ok=True)
    for name, audio in renders.items():
        write_wav(directory / f"{name}.wav", np.clip(audio * gain, -1.0, 1.0), SR)
    index = archive / "index.json"
    data = json.loads(index.read_text()) if index.exists() else {}
    data.setdefault(capture_id, {})[take_id] = {
        "gain_db": round(float(20 * np.log10(max(gain, 1e-9))), 4),
        "timbres": sorted(renders),
    }
    index.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def build_sources(voice: Voice, timbres: list[dict],
                  variants: list[Variant]) -> dict:
    """The page's version switch, split into a model row and a reference row.

    Seven versions of a take is an ordinary number once a couple of candidate
    settings are in play, and as one undifferentiated strip of buttons it takes
    reading every label to find which side of the comparison a version is on --
    which is the one thing the page should never make anybody work out.
    """
    detail = f"the library as it stands, no overrides — {voice.label}"
    if voice.patch:
        detail += f", patch {voice.patch}"
    sources = {"model": {
        "label": "libsonare NativeSynth (GM fallback)",
        "role": "model",
        "detail": detail,
    }}
    for variant in variants:
        sources[variant.name] = {
            "label": f"libsonare NativeSynth (GM fallback), {variant.name}",
            "role": "model",
            # The note first, because the override string says what moved and
            # never says what it was trying to fix, and a page is read weeks
            # after the question that built it.
            "detail": variant.detail,
        }
    reference_of = voice.capture.label.split(",")[0] if voice.capture else ""
    for t in timbres:
        sources[t["id"]] = {
            "label": t["label"],
            "role": "reference",
            "detail": reference_of,
        }
    return sources


def reference_note(voice: Voice, timbres: list[dict]) -> str:
    """The sentence under the title saying what the reference side is worth.

    Read off the timbres actually rendered rather than off the capture, so a
    `--model-only` page of a captured voice does not describe a reference that
    is not on it.
    """
    if voice.capture is None or not timbres:
        return ("Nothing is being compared here: this page holds the model alone, "
                "either because no reference has been captured for this voice or "
                "because none was asked for.")
    if voice.capture.dry:
        return ("The reference is captured dry — every effect section of the plugin is "
                "switched off — so what is being compared is the instrument and not a room.")
    return ("The reference is NOT captured dry: this one carries effects of its own "
            "that cannot be switched off per slot, so part of what is heard on the "
            "reference side is its room.")


def render_take(take: Take, voice: Voice, timbres: list[dict], out: Path, args,
                variants: list[Variant], archive: Path | None) -> dict:
    """Every version of one take, written out, as the manifest item describing it."""
    total = take.duration()
    channel = 9 if voice.kit else take.channel
    smf = write_smf(take.notes, program=voice.program, bank=voice.bank,
                    end_pad=take.tail_s, cc_events=take.cc_events, channel=channel)
    print(f"== {take.id} ({total:.1f}s) ==", file=sys.stderr)

    renders: dict[str, np.ndarray] = {}
    # `--lib` has to reach the unmodified voice as well as the variants.
    # Rendering it in-process instead would take whichever library the loader
    # prefers, so a page meant to compare four settings of one constant would be
    # comparing two builds -- and the difference between two build trees is
    # invisible on a listening page and reads as tuning.
    renders["model"] = (render_variant(smf, total, SR, "", args.lib) if args.lib
                        else render_model(smf, total, SR))
    print("  model", file=sys.stderr)

    # The BASELINE is in the set, not just the variants. What has to be caught
    # is a library with the override layer compiled out, where nothing an
    # override says reaches the render — and a voice with one recorded setting
    # is the common case, which a variants-only comparison cannot see at all.
    #
    # A variant that sets nothing is left out of it rather than counted: it is a
    # second copy of the baseline on purpose, which is what a blind comparison
    # needs a control for, and it is identical to the baseline in a working
    # build as much as in a broken one.
    tuned = [v for v in variants if v.overrides]
    digests: set[str] = {digest(renders["model"])} if tuned else set()
    for variant in variants:
        audio = render_variant(smf, total, SR, variant.overrides, args.lib)
        renders[variant.name] = audio
        if variant.overrides:
            digests.add(digest(audio))
        print(f"  {variant.name}", file=sys.stderr)

    cfg = voice.capture
    held = (archived_references(archive, cfg.id, take.id, timbres)
            if archive is not None and cfg is not None else {})
    fresh: dict[str, np.ndarray] = {}
    for timbre in timbres:
        if timbre["id"] in held:
            renders[timbre["id"]] = held[timbre["id"]]
            print(f"  {timbre['id']} (archived)", file=sys.stderr)
            continue
        # Built through the same helper the capture path uses, so a timbre
        # selected by preset reaches the plugin here too.
        source = source_for(cfg.raw, timbre, tail=f"{take.tail_s:.0f}s", sample_rate=SR)
        # A slot of a multitimbral rack is NOT selected by the source here,
        # though: aubounce ignores `--channel` whenever it is given a MIDI file,
        # because the file supplies its own channels. So the slot that answers is
        # whichever one sits on the channel the SMF was written on, and every
        # timbre of a rack renders from that same slot unless the file is
        # rewritten per timbre. It is silent -- each render has the right length,
        # the right level and an organ in it, and the two registrations come back
        # byte-identical.
        #
        # The model keeps the take's own channel, which is what makes a note
        # number a drum rather than a pitch; a reference gets its timbre's,
        # one-based in the capture definition and zero-based in the file.
        ref_channel = int(timbre.get("channel", channel + 1)) - 1
        timbre_smf = smf if ref_channel == channel else write_smf(
            take.notes, program=voice.program, bank=voice.bank, end_pad=take.tail_s,
            cc_events=take.cc_events, channel=ref_channel,
        )
        try:
            fresh[timbre["id"]] = render_oracle_au(timbre_smf, total, SR, source=source)
            renders[timbre["id"]] = fresh[timbre["id"]]
            print(f"  {timbre['id']}", file=sys.stderr)
        except (AuRenderError, FileNotFoundError) as exc:
            print(f"  {timbre['id']}: SKIPPED — {exc}", file=sys.stderr)
    # Only a take whose every reference came from the plugin THIS run is
    # written, so the archive never holds a render that has been through 16-bit
    # twice. A partial take is left alone rather than topped up.
    if args.archive_references and cfg is not None and timbres and len(fresh) == len(timbres):
        archive_references(Path(args.archive_references).expanduser().resolve(),
                           cfg.id, take.id, fresh)

    gain = shared_gain(renders)
    tracks = {}
    for key, audio in renders.items():
        rel = Path(take.id) / f"{key}.wav"
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        write_wav(out / rel, np.clip(audio * gain, -1.0, 1.0), SR)
        tracks[key] = str(rel)

    return {
        "id": take.id,
        "label": take.label,
        "sub": take.sub,
        "group": take.group,
        "tracks": tracks,
        "_digests": digests,
        "meta": {
            "seconds": round(total, 2),
            "shared_gain_db": round(float(20 * np.log10(max(gain, 1e-9))), 2),
            "program": voice.program,
            "bank": voice.bank,
            "channel": channel,
            "pedal": bool(take.cc_events),
            # The schedule, so a take can say where its own measurement windows
            # are. Anything reading these files otherwise has to place a window
            # by eye against a phrase it cannot see, and a window placed by eye
            # lands in the wrong place: the pedal take's resonance lives between
            # the last note-off and the pedal lifting, and a window a little
            # further on measures the dampers landing instead -- the opposite
            # mechanism, at the other end of the same take.
            "notes": [{"note": n.note, "velocity": n.velocity,
                       "start": round(n.start, 4), "duration": round(n.dur, 4)}
                      for n in take.notes],
            "cc": [[round(t, 4), int(cc), int(v)] for t, cc, v in take.cc_events],
        },
    }


def render_set(voice: Voice, out: Path, args, table: dict[str, list[Variant]],
               extra: list[Variant]) -> int:
    """One voice's whole page: every take, every version, and the manifest.

    The settings recorded for this voice come first and the run's own `--variant`
    flags after, which is what makes a batch across the bank carry per-voice
    candidates — one command line cannot.
    """
    variants = calibration.for_voice(voice.slug, table, extra)
    timbres = [] if args.model_only else [
        t for t in (voice.capture.timbres if voice.capture else ())
        if not args.wanted_timbres or t["id"] in args.wanted_timbres
    ]
    selected = [t for t in build_takes(voice.take_set, voice.program)
                if not args.only_takes or t.id in args.only_takes]
    if not selected:
        print(f"{voice.slug}: no takes selected", file=sys.stderr)
        return 0

    archive = (Path(args.reference_from).expanduser().resolve()
               if args.reference_from else None)
    print(f"\n### {voice.label}  ->  {out}", file=sys.stderr)

    items = []
    variant_digests: dict[str, set[str]] = {}
    for take in selected:
        item = render_take(take, voice, timbres, out, args, variants, archive)
        variant_digests[take.id] = item.pop("_digests")
        items.append(item)

    manifest = {
        # The voice names itself, which is the right default and the wrong
        # answer once two pages of the same voice are on the picker at once:
        # they then read identically and the only way to tell the live question
        # from last week's is to open both. --title is what a page built to
        # settle one question should carry.
        "title": args.title or voice.title,
        "group": voice.group,
        "voice": voice.describe(),
        "notes": ((args.note + " ") if args.note else "") + (
            "Every version of a take is written at one shared gain, so the level "
            "difference between them is real. " + reference_note(voice, timbres)),
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "sources": build_sources(voice, timbres, variants),
        "items": items,
    }
    out.mkdir(parents=True, exist_ok=True)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    # A page whose settings all render the same looks exactly like a page whose
    # settings are subtly different, and the difference is a build flag nobody
    # sees. Say it here rather than let it be listened to. The comparison
    # includes the baseline, so a single recorded setting that reaches nothing
    # is caught as readily as a dozen.
    tuned = [v for v in variants if v.overrides]
    if tuned:
        identical = [tid for tid, d in variant_digests.items() if len(d) == 1]
        if len(identical) == len(variant_digests):
            print(f"WARNING: no setting changed the render on any take "
                  f"({', '.join(v.name for v in tuned)}).\n         The library has no "
                  f"tuning override layer -- rebuild it with -DBUILD_TUNING=ON, or point"
                  f"\n         --lib at one that has; or the keys reach nothing this "
                  f"voice consults.", file=sys.stderr)
        elif identical:
            print(f"note: {len(identical)} take(s) render identically across the "
                  f"settings: {', '.join(sorted(identical))}", file=sys.stderr)
    return len(items)


def load_catalogue(lib: str):
    """What the library says it voices, or None if this build cannot say.

    Only a `-DBUILD_TUNING=ON` build answers, and the index does not depend on
    the answer: without one every program is listed at bank 0 and no patch is
    named. That is a reading the run did not get rather than a voice it does not
    have, so it is reported and not raised.
    """
    from catalogue import dump_catalogue

    try:
        return dump_catalogue(0, "sustain", lib or None, sr=SR)
    except RuntimeError as exc:
        print(f"note: no knob catalogue ({str(exc).splitlines()[0]}); "
              f"listing bank 0 only, with no patch names", file=sys.stderr)
        return None


class Unselectable(Exception):
    """What to audition could not be worked out. The message is for the user.

    Raised rather than exited on, so the reason reaches `main` and leaves by
    the one path a caller can test: stderr and a usage exit code. A capture
    with no phrase set has to stop here in particular — the alternative is
    rendering it on whichever set was nearest, which plays, looks like a
    successful comparison, and is of the wrong music.
    """


def resolve_voices(args) -> list[Voice]:
    """What this run was asked to audition, as bank entries."""
    if args.config:
        capture = load_capture(Path(args.config).expanduser().resolve())
        if capture is None:
            raise Unselectable(f"{args.config} names no phrase set (`takes`)")
        program = args.program if args.program is not None else capture.program
        return [Voice(program=program, bank=capture.bank,
                      kit=capture.drums, capture=capture)]

    programs = parse_selection(args.programs) if args.programs else []
    if args.program is not None:
        programs.append(args.program)
    kits = parse_selection(args.kits) if args.kits else []
    if not programs and not kits:
        raise Unselectable(
            "name what to audition: --program N, --programs 0-7,40, --kits 0, "
            "--programs all, or --config <capture>")

    banks = parse_selection(args.banks) if args.banks else None
    catalogue = load_catalogue(args.lib) if banks is None and programs else None
    return voices(sorted(set(programs)), banks=banks, kits=kits, catalogue=catalogue)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    pick = ap.add_argument_group("what to audition")
    pick.add_argument("--program", type=int, default=None,
                      help="a GM program number")
    pick.add_argument("--programs", default="",
                      help="several: `0-7,40,73`, or `all` for the whole bank")
    pick.add_argument("--banks", default="",
                      help="GS variation banks to render each program at (default: "
                           "every bank the library voices apart, or 0 without a "
                           "tuning build)")
    pick.add_argument("--kits", default="",
                      help="drum kit numbers, rendered on channel 10 (`0` is the "
                           "GM standard kit)")
    pick.add_argument("--config", default="",
                      help="a capture definition, when the capture is the subject: it "
                           "fixes the program, the phrase set and the reference timbres")

    ap.add_argument("--out", default=str(DEFAULT_OUT),
                    help="one subdirectory per voice is written under here")
    ap.add_argument("--timbres", default="",
                    help="comma-separated timbre ids to render (default: all the "
                         "voice's capture has)")
    ap.add_argument("--model-only", action="store_true",
                    help="skip the reference renders even where one exists")
    ap.add_argument("--only", default="", help="comma-separated take ids")
    ap.add_argument("--variant", action="append", default=[], metavar="NAME=OVERRIDES",
                    help="an extra version of every take, rendered under this "
                         "SONARE_TUNING_OVERRIDES string; repeatable, and applied to "
                         "every voice in the run")
    ap.add_argument("--calibrations", nargs="?", const=str(calibration.DEFAULT_PATH),
                    default="", metavar="FILE",
                    help="also render each voice's recorded calibration settings "
                         f"(default file: {calibration.DEFAULT_PATH.name}). Off unless "
                         "asked for: every setting is another render of every take, and "
                         "the override layer needs a -DBUILD_TUNING=ON library")
    ap.add_argument("--lib", default="",
                    help="library the variants load (a -DBUILD_TUNING=ON build); "
                         "sets SONARE_LIB_PATH for them")
    ap.add_argument("--title", default="",
                    help="what this page is for, shown in the set picker (default: the "
                         "voice's own name, which is the right answer until two pages "
                         "of one voice are up at once)")
    ap.add_argument("--note", default="",
                    help="a sentence at the top of the page saying what to listen for")
    ap.add_argument("--reference-from", default=str(DEFAULT_REFERENCE_ARCHIVE),
                    dest="reference_from", metavar="DIR",
                    help="take reference renders from this archive instead of the plugin, "
                         "falling back to the plugin for any it does not hold. Empty string "
                         "to always render")
    ap.add_argument("--archive-references", default="", dest="archive_references",
                    metavar="DIR",
                    help="write every reference render this run produced into DIR, so the "
                         "next page can be built without the plugin")
    args = ap.parse_args()

    args.wanted_timbres = {t.strip() for t in args.timbres.split(",") if t.strip()}
    args.only_takes = {t.strip() for t in args.only.split(",") if t.strip()}

    try:
        extra = calibration.parse_cli(args.variant)
        table = calibration.load(Path(args.calibrations)) if args.calibrations else {}
        selected = resolve_voices(args)
        # Checked against the voices this run resolved rather than against the
        # whole bank, because that is the answer being asked for: a key that
        # names no voice HERE is either a typo or a voice not in the run, and
        # both are worth a line before several hundred renders start.
        unknown = calibration.unknown_voices(table, {v.slug for v in selected})
        if unknown:
            print(f"note: {len(unknown)} recorded voice(s) not in this run: "
                  f"{', '.join(unknown)}", file=sys.stderr)
        for voice in selected:
            calibration.for_voice(voice.slug, table, extra)
    except (Unselectable, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2
    root = Path(args.out).expanduser().resolve()
    # A capture-driven run writes where it was pointed, which is what the
    # existing per-instrument invocations expect. A bank-driven run writes one
    # subdirectory per voice, because it is routinely more than one voice and
    # they cannot share a directory.
    flat = bool(args.config) and len(selected) == 1

    total = 0
    for voice in selected:
        out = root if flat else root / voice.slug
        total += render_set(voice, out, args, table, extra)

    if not flat:
        write_index(root, selected)
    print(f"\n{len(selected)} voice(s), {total} takes -> {root}", file=sys.stderr)
    if root.is_relative_to(CORPUS_ROOT):
        print("listen:  python tools/audition/serve.py"
              "   (under the scratch root, so it is found with no argument)",
              file=sys.stderr)
    else:
        print(f"listen:  python tools/audition/serve.py {root}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
