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
if any does. A voice no capture covers renders the model alone and the page
plays rather than compares, which `serve.py` already handles as an ordinary
set — today that is mostly the GS variations, whose slots only the machine
defines. A reference is an attachment to an entry, never the reason the entry
exists.

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
import re
import subprocess
import sys
import tempfile
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import calibration
from _repo import REPO_ROOT
from au_oracle import AuRenderError, render_oracle_au, with_keyswitches
from bank import Capture, Voice, load_capture, parse_selection, voices, write_index
from calibration import Variant
from capture import CORPUS_ROOT, source_for
from phrases import Take, build_takes
from render_model import render_model
from smf import write_smf
from wavio import read_wav, write_wav

SR = 48000
DEFAULT_OUT = CORPUS_ROOT / "audition"
#: Where a measurement run goes. `serve.py` globs `audition/` under the scratch
#: root and nothing else, so a probe written here is out of the listening tree
#: by construction rather than by being named something discouraging — which is
#: what the component-isolation set was, and it sat on the picker beside four
#: real pages of the same voice.
DEFAULT_PROBE_OUT = CORPUS_ROOT / "probe"
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
rig = sys.argv[5] != "0"
with open(smf, "rb") as fh:
    a = np.asarray(render_model(fh.read(), seconds, sr, rig=rig), dtype=np.float32)
np.save(out, a.mean(axis=1) if a.ndim > 1 else a)
'''


def render_variant(smf: bytes, seconds: float, sr: int, overrides: str,
                   lib_path: str = "", rig: bool = True) -> np.ndarray:
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
             str(seconds), str(sr), "1" if rig else "0"],
            capture_output=True, check=False, text=True, env=env, cwd=str(REPO_ROOT))
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
        write_wav(directory / f"{name}.wav", np.clip(audio * gain, -1.0, 1.0), SR, bits=24)
    index = archive / "index.json"
    data = json.loads(index.read_text()) if index.exists() else {}
    data.setdefault(capture_id, {})[take_id] = {
        "gain_db": round(float(20 * np.log10(max(gain, 1e-9))), 4),
        "timbres": sorted(renders),
    }
    index.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


#: Prefix every knob that belongs to the rig rather than to the instrument.
RIG_KNOB_PREFIX = "gm_fallback_map.kRig"


def moves_the_instrument(overrides: str) -> bool:
    """Whether a variant changes anything the direct signal would show.

    A variant that only turns the amplifier cannot change the direct render at
    all, so pairing one with a DI costs a render per take to produce a second
    copy of `model-di`. The amp sweeps are ten variants wide, which is sixty
    such renders on one voice.
    """
    keys = [k.split("=", 1)[0].strip() for k in overrides.split(",") if "=" in k]
    return any(not k.startswith(RIG_KNOB_PREFIX) for k in keys)


def build_sources(voice: Voice, timbres: list[dict],
                  variants: list[Variant], di: bool = False) -> dict:
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
    if di:
        sources["model-di"] = {
            "label": "libsonare NativeSynth (GM fallback), direct",
            "role": "model",
            # The signal path is an axis rather than a choice: the same setting
            # heard down two paths is not two candidates, and a switch that
            # interleaves them asks one question where there are two.
            "path": "direct",
            "detail": "the same voice with the bank's rig cleared, which is where "
                      "the instrument itself stops. `model` is what ships and what "
                      "the reference is comparable with, since a module's samples "
                      "of this program have an amplifier recorded into them; this "
                      "is what the rig is being asked to work on.",
        }
    for variant in variants:
        sources[variant.name] = {
            "label": f"libsonare NativeSynth (GM fallback), {variant.name}",
            "role": "model",
            # The note first, because the override string says what moved and
            # never says what it was trying to fix, and a page is read weeks
            # after the question that built it.
            "detail": variant.detail,
            **calibration.source_text(variant),
        }
        if di and moves_the_instrument(variant.overrides):
            sources[f"{variant.name}-di"] = {
                "label": f"libsonare NativeSynth (GM fallback), {variant.name}, direct",
                "role": "model",
                "path": "direct",
                "detail": "the same candidate with the bank's rig cleared. A setting "
                          "that moves the instrument is judged where the instrument "
                          "ends, since an amplifier in front of it both hides a change "
                          "and invents one: it compresses, so it narrows whatever the "
                          "candidate did to the decay. — " + variant.detail,
                **calibration.source_text(variant, direct=True),
            }
    reference_of = voice.capture.label.split(",")[0] if voice.capture else ""
    for t in timbres:
        sources[t["id"]] = {
            "label": t["label"],
            "role": "reference",
            "detail": reference_of,
        }
    return sources


def reference_note(voice: Voice, timbres: list[dict], model_sends: str = "auto") -> str:
    """The sentence under the title saying what the reference side is worth.

    Read off the timbres actually rendered rather than off the capture, so a
    `--model-only` page of a captured voice does not describe a reference that
    is not on it.
    """
    forced = ""
    if model_sends == "gs":
        forced = ("The model side renders with CC91/93/94 at their GS power-on values "
                  "whatever the reference does, because this page was built to be heard "
                  "through libsonare's own ambience. ")
    elif model_sends == "dry":
        forced = "The model side renders with CC91/93/94 zeroed, by request. "
    if voice.capture is None or not timbres:
        return forced + (
            "Nothing is being compared here: this page holds the model alone, "
            "either because no reference has been captured for this voice or "
            "because none was asked for.")
    if voice.capture.dry:
        return forced + (
            "The reference is captured dry — every effect section of the plugin is "
            "switched off — so what is being compared is the instrument and not a room.")
    return ("The reference is NOT captured dry: this one carries effects of its own "
            "that cannot be switched off per slot, so part of what is heard on the "
            "reference side is its room. The model side therefore renders the way it "
            "ships — CC91/93/94 left at their GS power-on values, weighted per program "
            "by `gm_fallback_sends` — rather than at the zero a dry-versus-dry metric "
            "needs. That is libsonare's own ambience and the only ambience a listener "
            "gets from it, so what is being compared is the product against the "
            "recording, room included on both sides.")


def render_take(take: Take, voice: Voice, timbres: list[dict], out: Path, args,
                variants: list[Variant], archive: Path | None,
                di_state: dict | None = None) -> dict:
    """Every version of one take, written out, as the manifest item describing it."""
    di_state = {} if di_state is None else di_state
    total = take.duration()
    channel = 9 if voice.kit else take.channel
    # A page is heard, not measured, so the model renders the way it SHIPS.
    # `write_smf` defaults CC91/93/94 to 0 because a dry-versus-dry metric needs
    # that; leaving them alone here keeps the GS power-on values a plain GM file
    # arrives with, weighted per program by `gm_fallback_sends` — which is where
    # libsonare's own ambience lives, and the only ambience a listener will ever
    # get from it. Zeroing them put a bone-dry model beside a reference carrying
    # its building, and the low end, where a registration question is decided,
    # was then decided by the building. Only for a reference that HAS a room: a
    # dry capture is compared dry, and its page stays what it always was.
    #
    # `--model-sends` overrides that inference, because the capture decides what
    # a COMPARISON needs and the question on the page is not always a comparison.
    # A change to the shared GS tank reaches every program, and the voice that
    # has to be checked for collateral is whichever one the listener knows best
    # — usually a dry-captured one, whose page would otherwise render at CC91 0
    # and hold the one setting the question is about perfectly inert.
    wet = (args.model_sends == "gs" or (
        args.model_sends == "auto"
        and voice.capture is not None and not voice.capture.dry and bool(timbres)))
    smf = write_smf(take.notes, program=voice.program, bank=voice.bank,
                    end_pad=take.tail_s, cc_events=take.cc_events, channel=channel,
                    sends=(None, None, None) if wet else (0, 0, 0))
    print(f"== {take.id} ({total:.1f}s){' [GS sends at power-on]' if wet else ''} ==",
          file=sys.stderr)

    renders: dict[str, np.ndarray] = {}
    # `--lib` has to reach the unmodified voice as well as the variants.
    # Rendering it in-process instead would take whichever library the loader
    # prefers, so a page meant to compare four settings of one constant would be
    # comparing two builds -- and the difference between two build trees is
    # invisible on a listening page and reads as tuning.
    renders["model"] = (render_variant(smf, total, SR, "", args.lib) if args.lib
                        else render_model(smf, total, SR))
    print("  model", file=sys.stderr)

    # The bank binds an amplifier after some voices and `model` is the product
    # sound, so without this the page cannot play the instrument on its own --
    # the surface that renders the direct signal exists and reached no listener.
    # Which programs are bound is asked by rendering rather than mirrored from
    # the table: an identical render means nothing was bound, and a rig added to
    # a voice later shows up here without this file being told about it. Asked
    # once per voice rather than once per take, since a voice with no rig would
    # otherwise pay for a duplicate render of every take it has -- six programs
    # in the bank are bound and the rest would render twice for nothing.
    if di_state.get("bound") is not False:
        di = (render_variant(smf, total, SR, "", args.lib, rig=False) if args.lib
              else render_model(smf, total, SR, rig=False))
        di_state["bound"] = digest(di) != digest(renders["model"])
        if di_state["bound"]:
            renders["model-di"] = di
            print("  model-di", file=sys.stderr)

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
        # A candidate for a rigged voice gets its direct render too, by the same
        # rule `model-di` is asked by: the amplifier compresses, so it narrows
        # whatever the candidate did to the decay and a listener judging the
        # instrument through it is judging the wrong end of the chain.
        if di_state.get("bound") and moves_the_instrument(variant.overrides):
            renders[f"{variant.name}-di"] = render_variant(
                smf, total, SR, variant.overrides, args.lib, rig=False)
            print(f"  {variant.name}-di", file=sys.stderr)

    cfg = voice.capture
    held = (archived_references(archive, cfg.id, take.id, timbres)
            if archive is not None and cfg is not None else {})
    fresh: dict[str, np.ndarray] = {}
    for timbre in timbres:
        if timbre["id"] in held:
            renders[timbre["id"]] = held[timbre["id"]]
            print(f"  {timbre['id']} (archived)", file=sys.stderr)
            continue
        if "plugin" not in cfg.raw:
            # A capture whose reference came from a SoundFont rather than from a
            # hosted plugin, which this path has no renderer for. Named and
            # skipped rather than raised: the model side of this page is what a
            # listener is here for, and one voice with no renderable reference
            # took the whole run down with it — including every voice after it.
            print(f"  {timbre['id']}: {cfg.id} names no plugin, so its reference cannot be "
                  f"rendered here — the archive under --reference-from is the only route, "
                  f"and it does not hold this take", file=sys.stderr)
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
        ref_channel = int(timbre.get("slot_channel",
                                     timbre.get("channel", channel + 1))) - 1
        # A take is written in sounding pitch, so an instrument mapped away from
        # it needs its own score even when the channel already matches.
        ref_notes = ([replace(n, note=source.key(n.note)) for n in take.notes]
                     if source.key_offset else take.notes)
        # A timbre selected from the keyboard needs its own score for the same
        # reason a rack slot does, and for the same failure: the switch would
        # simply be absent and every switched timbre would render as the
        # unswitched instrument, at the right length and level, byte-identical to
        # its sibling. One switch per onset, since a switch is consumed by the
        # note it arms rather than latching for the phrase.
        ref_notes = with_keyswitches(source, ref_notes)
        # The phrase moves back by the lead, so anything else on its timeline
        # moves with it. On a take under the sustain pedal, leaving CC64 where it
        # was would lift the dampers a third of a second early and read as the
        # variant.
        lead_s = source.keyswitch_lead_ms / 1000.0
        ref_cc = tuple((at + lead_s, cc, v) for at, cc, v in take.cc_events)
        timbre_smf = (smf if (ref_channel == channel and not source.key_offset
                              and not source.keyswitch)
                      else write_smf(
                          ref_notes, program=voice.program, bank=voice.bank,
                          end_pad=take.tail_s, cc_events=ref_cc,
                          channel=ref_channel,
                      ))
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
        write_wav(out / rel, np.clip(audio * gain, -1.0, 1.0), SR, bits=24)
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


#: A drum note's override key: `d038.percussion.wire_buzz` names note 38. The
#: only prefix in the override key space that carries an instrument a take can
#: be asked to strike; a patch or engine key names no note at all.
_DRUM_KEY = re.compile(r"\bd(\d{3})\.")


def played_notes(items: list[dict]) -> set[int]:
    """Every note number this page's takes actually strike."""
    return {int(n["note"]) for item in items
            for n in (item.get("meta") or {}).get("notes") or []}


def unheard_drum_notes(tuned: list[Variant], items: list[dict]) -> list[int]:
    """Drum notes these settings move that no take on the page strikes.

    Empty when the settings name no drum note, and empty as soon as ONE of them
    is played: a page that can hear part of a setting is a page worth listening
    to, and the reader is told which takes went unchanged separately.
    """
    named = {int(m.group(1)) for v in tuned for m in _DRUM_KEY.finditer(v.overrides)}
    return sorted(named - played_notes(items)) if named and not (
        named & played_notes(items)) else []


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
    # A listening page carries the musical take; a measurement run does not, and
    # `--no-music` is for the case where the ten seconds of polyphony are just
    # render time — a sweep across the bank narrowed with `--only`, say.
    music = None if args.no_music else (
        voice.capture.raw.get("music", "") if voice.capture else "")
    selected = [t for t in build_takes(voice.take_set, voice.program, music=music)
                if not args.only_takes or t.id in args.only_takes]
    if not selected:
        print(f"{voice.slug}: no takes selected", file=sys.stderr)
        return 0

    archive = (Path(args.reference_from).expanduser().resolve()
               if args.reference_from else None)
    print(f"\n### {voice.label}  ->  {out}", file=sys.stderr)

    items = []
    variant_digests: dict[str, set[str]] = {}
    di_state: dict = {}
    for take in selected:
        item = render_take(take, voice, timbres, out, args, variants, archive, di_state)
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
        # Travels with the data rather than with the directory, so a probe
        # copied or pointed at explicitly is still not served.
        "probe": bool(args.probe),
        "voice": voice.describe(),
        "notes": ((args.note + " ") if args.note else "") + (
            "Every version of a take is written at one shared gain, so the level "
            "difference between them is real. "
            + reference_note(voice, timbres, args.model_sends)),
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "sources": build_sources(
            voice, timbres, variants,
            di=any("model-di" in (i.get("tracks") or {}) for i in items)),
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
            unplayed = unheard_drum_notes(tuned, items)
            if unplayed:
                # Named separately because it sends the reader somewhere else
                # entirely, and it is the cause a build flag looks exactly like:
                # the settings reach the library and move nothing because no
                # take strikes the instrument they are about. A candidate on a
                # note no phrase plays cannot be listened to at all, which is
                # the one thing recording it was supposed to make possible.
                print(f"WARNING: no setting changed the render on any take "
                      f"({', '.join(v.name for v in tuned)}), because no take on "
                      f"this page\n         strikes the drum notes they move: "
                      f"{', '.join(str(n) for n in unplayed)}. The takes play "
                      f"{', '.join(str(n) for n in sorted(played_notes(items)))}."
                      f"\n         This is a gap in the take set, not in the "
                      f"settings.", file=sys.stderr)
            else:
                print(f"WARNING: no setting changed the render on any take "
                      f"({', '.join(v.name for v in tuned)}).\n         The library has "
                      f"no tuning override layer -- rebuild it with -DBUILD_TUNING=ON, "
                      f"or point\n         --lib at one that has; or the keys reach "
                      f"nothing this voice consults.", file=sys.stderr)
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


def can_supply_a_reference(capture: Capture, archive: Path | None) -> bool:
    """Whether a reference take can actually be produced from this capture.

    A capture imported from a file names no plugin, so nothing can be rendered
    from it and the archive is its only route.
    """
    if capture.raw.get("plugin"):
        return True
    return bool(archive) and (archive / capture.id).is_dir()


def playable_first(voice: Voice, archive: Path | None) -> Voice:
    """Move a capture that can supply a reference to the front of the voice.

    `Voice.capture` is defined as the one whose reference a PAGE PLAYS, so a
    capture that can play none is the wrong representative for a page however
    well it answers the policy's layer. The kit is the live case: the policy
    aims a kit at the machine, the module captures therefore lead, and none of
    them names a plugin — so the most-calibrated voice in the bank rendered
    model-only while its library reference sat in the archive.

    Order is otherwise preserved, so the layer preference still decides among
    captures that can each supply one. This reorders the audition run's own view
    and not `capture_for`, which answers a different question for the gates.
    """
    playable = [c for c in voice.captures if can_supply_a_reference(c, archive)]
    if not playable or playable[0] is voice.capture:
        return voice
    rest = [c for c in voice.captures if c not in playable]
    print(f"{voice.slug}: {voice.capture.id} can render no reference; "
          f"the page plays {playable[0].id}", file=sys.stderr)
    return replace(voice, captures=tuple(playable + rest))


def resolve_voices(args) -> list[Voice]:
    """What this run was asked to audition, as bank entries."""
    if args.config:
        capture = load_capture(Path(args.config).expanduser().resolve())
        if capture is None:
            raise Unselectable(f"{args.config} names no phrase set (`takes`)")
        program = args.program if args.program is not None else capture.program
        return [Voice(program=program, bank=capture.bank,
                      kit=capture.drums, captures=(capture,))]

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
    archive = (Path(args.reference_from).expanduser().resolve()
               if args.reference_from else None)
    return [playable_first(v, archive) for v in
            voices(sorted(set(programs)), banks=banks, kits=kits, catalogue=catalogue)]


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

    ap.add_argument("--out", default="",
                    help=f"one subdirectory per voice is written under here "
                         f"(default: {DEFAULT_OUT.name}/ under the scratch root, "
                         f"or {DEFAULT_PROBE_OUT.name}/ with --probe)")
    ap.add_argument("--probe", action="store_true",
                    help="a measurement run, not a listening page: writes under the "
                         "probe root, which the listening server does not discover, "
                         "and marks the manifest so it stays unserved wherever it is")
    ap.add_argument("--timbres", default="",
                    help="comma-separated timbre ids to render (default: all the "
                         "voice's capture has)")
    ap.add_argument("--model-only", action="store_true",
                    help="skip the reference renders even where one exists")
    ap.add_argument("--no-music", action="store_true",
                    help="leave off the musical take — ten seconds of real Bach, "
                         "which nothing measures and every page otherwise carries")
    ap.add_argument("--only", default="", help="comma-separated take ids")
    ap.add_argument("--model-sends", choices=("auto", "gs", "dry"), default="auto",
                    help="what the model side does with CC91/93/94: 'auto' leaves them "
                         "at the GS power-on values where the reference carries a room "
                         "and zeroes them where it does not, 'gs' always leaves them "
                         "(the way the library ships, which is what a question about "
                         "the shared GS tank has to be heard through), 'dry' always "
                         "zeroes them")
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
    default_root = DEFAULT_PROBE_OUT if args.probe else DEFAULT_OUT
    root = Path(args.out or default_root).expanduser().resolve()

    # One subdirectory per voice, always. A capture-driven run used to write
    # straight into --out instead, which put its manifest at the path every
    # other single-voice run also writes to: the second instrument silently took
    # the first one's page, leaving that page's takes on disk with nothing left
    # to name or group them. `write_index` merges by slug for exactly this
    # reason, and the flat path was the one route that skipped it.
    total = 0
    for voice in selected:
        total += render_set(voice, root / voice.slug, args, table, extra)

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
