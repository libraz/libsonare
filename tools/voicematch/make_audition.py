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

A voice can reach more than one capture, and only the first — the one
`policy.json` aims it at — is the reference. Every other capture that can
still supply takes is offered too, under its own `comparison` role rather than
folded into the reference: the standard kit's page plays the module recording
it is fitted to AND the sampled library kit it used to be judged against
instead, side by side rather than one substituted for the other.
`--no-comparisons` turns this off.

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
part that cannot be reproduced from this repository. A module capture's
reference is a fast dry fluidsynth render instead, so it is remade fresh every
run and never needs the archive at all. Kept per page instead, it
was both the bulk of the disk and the reason nobody dared delete a page — and a
directory of pages nobody dares delete stops being a place to look.

    --reference-from DIR     take them from here (default: the archive; empty
                             string to always render)
    --archive-references DIR keep this run's reference renders for the next page

The archive stores each take under a gain computed from its reference renders
ALONE, so it does not move when a page's candidates get louder, and divides that
gain back out on the way in. Only a take whose every reference came from the
plugin in one run is written, so nothing in it has been through 16 bits twice.
An archived render is adopted only when its request (the reference MIDI as
written, rate, length, render method) and its source (the resolved plugin or
font state, key map, key switch, rig and room classes) match what this run
would render exactly; a caption is in neither. The v1 `index.json` records
neither, so it is never adopted and is left for reading by hand.

A page is written as one generation: every WAV goes into a directory named by
the set's digest, and `manifest.json` is replaced only after it is complete, so
an asset an older manifest (and the feedback against it) points at is never
overwritten. Each version of each take carries its evidence — request, source,
build and asset identities and the library's render-path record — see
`render_evidence.py`.

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
import shutil
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
import policy
from _repo import REPO_ROOT
from au_oracle import AuRenderError, render_oracle_au, with_keyswitches
from bank import Capture, Voice, load_capture, parse_selection, voices, write_index
from boundary import (
    SCOPE_INSTRUMENT,
    SCOPE_PRODUCT,
    SENDS_DRY,
    SENDS_POWER_ON,
    Reference,
    RenderRequest,
    assess,
    canonical_digest,
)
from calibration import Variant
from capture import (
    CORPUS_ROOT,
    RIG_NONE,
    RIG_UNCLASSIFIED,
    ROOM_NONE,
    ROOM_PRESENT,
    ROOM_UNCLASSIFIED,
    resolve_font,
    rig_capable,
    source_for,
)
from capture import load_config as capture_load_config
from metrics import _db
from phrases import Take, build_takes
from render_evidence import (
    ARCHIVE_GAIN_VERSION,
    ARCHIVE_INDEX,
    ARCHIVE_V2_DIR,
    REFERENCE_UNVERIFIED,
    REFERENCE_VERIFIED,
    SOURCE_UNRESOLVED,
    archived_records,
    au_source_id,
    file_digest,
    module_source_id,
    publish_generation,
    read_archive_index,
    reference_request,
    set_generation,
    staging_dir,
    write_json_atomic,
)
from render_model import RenderedAudio, render_model_rendered
from render_oracle import render_oracle_fluidsynth
from sf2 import SoundFont
from smf import Note, write_smf
from wavio import write_wav

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
#: Under a page, one directory per set generation.
GENERATIONS_DIR = "generations"


#: Renders one SMF in a fresh interpreter. The tuning override table is read
#: when the library loads, so two settings of the same constant cannot be
#: rendered by one process -- the second would silently get the first's values.
_VARIANT_WORKER = r"""
import json
import sys
import numpy as np
sys.path.insert(0, "tools"); sys.path.insert(0, "tools/voicematch")
from render_model import render_model_rendered
smf, out, evidence_out = sys.argv[1], sys.argv[2], sys.argv[3]
seconds, sr, rig = float(sys.argv[4]), int(sys.argv[5]), sys.argv[6] != "0"
preset, request_id = sys.argv[7], sys.argv[8] or None
with open(smf, "rb") as fh:
    rendered = render_model_rendered(
        fh.read(), seconds, sr, rig=rig, preset=preset, request_id=request_id
    )
np.save(out, np.asarray(rendered.audio, dtype=np.float32))
with open(evidence_out, "w", encoding="utf-8") as fh:
    json.dump(rendered.evidence, fh)
"""


def render_variant(
    smf: bytes,
    seconds: float,
    sr: int,
    overrides: str,
    lib_path: str = "",
    rig: bool = True,
    preset: str = "",
) -> np.ndarray:
    """`render_variant_rendered` for a caller that wants the audio alone."""
    return render_variant_rendered(smf, seconds, sr, overrides, lib_path, rig, preset).audio


def render_variant_rendered(
    smf: bytes,
    seconds: float,
    sr: int,
    overrides: str,
    lib_path: str = "",
    rig: bool = True,
    preset: str = "",
    request_id: str | None = None,
) -> RenderedAudio:
    """One take under one override set, in its own interpreter, with its evidence."""
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
        evidence_path = Path(tmp) / "evidence.json"
        proc = subprocess.run(
            [
                sys.executable,
                "-c",
                _VARIANT_WORKER,
                str(smf_path),
                str(out_path),
                str(evidence_path),
                str(seconds),
                str(sr),
                "1" if rig else "0",
                preset,
                request_id or "",
            ],
            capture_output=True,
            check=False,
            text=True,
            env=env,
            cwd=str(REPO_ROOT),
        )
        if proc.returncode:
            raise RuntimeError(proc.stderr[-4000:])
        return RenderedAudio(np.load(out_path), json.loads(evidence_path.read_text()))


def digest(audio: np.ndarray) -> str:
    """A render's identity, comparable across the two ways one is produced.

    Renders keep their native channel layout, so the arrays are hashed after a
    downmix: the check that wants them compared is asking whether a variant
    changed anything at all, and a mono and a stereo layout of one signal must
    not read as a difference.
    """
    mono = audio.mean(axis=1) if audio.ndim > 1 else audio
    return hashlib.sha256(np.ascontiguousarray(mono, dtype=np.float32).tobytes()).hexdigest()


def shared_gain(renders: dict[str, np.ndarray], headroom_db: float = -1.0) -> float:
    """One gain for every version of a take, so their level difference survives.

    Normalising each version on its own would erase exactly the thing a
    register-balance or velocity-curve problem shows up as.
    """
    peak = max((float(np.abs(a).max()) for a in renders.values() if a.size), default=0.0)
    if peak <= 0.0:
        return 1.0
    return float(10.0 ** (headroom_db / 20.0) / peak)


def _frames_channels(audio: np.ndarray) -> tuple[int, int]:
    return int(audio.shape[0]), int(audio.shape[1]) if audio.ndim > 1 else 1


def archived_references(
    archive: Path, capture_id: str, take_id: str, wanted: dict[str, tuple[str, str | None]]
) -> dict[str, tuple[np.ndarray, str]]:
    """Reference renders for one take, back at the level the plugin produced, with their source_id.

    `wanted` maps a timbre id to the (request_id, source_id) this run would
    render it from. A v2 render is adopted only when both match and its WAV
    still has the digest and shape the index recorded. A source_id of None is a
    source this machine cannot resolve: the request alone is matched then, and
    only while every stored candidate names the same source. A v1 entry records
    neither identity and is never adopted.
    """
    records = archived_records(archive, capture_id, take_id)
    out: dict[str, tuple[np.ndarray, str]] = {}
    for tid, (request_id, source_id) in wanted.items():
        stored = [r for r in records if r.timbre == tid and not r.historical]
        matching = [
            r for r in stored if r.request_id == request_id and source_id in (None, r.source_id)
        ]
        if source_id is None and len({r.source_id for r in matching}) > 1:
            print(
                f"  {tid}: the archive holds this request under several sources and this "
                f"one cannot be resolved here — not adopted",
                file=sys.stderr,
            )
            continue
        for record in matching:
            loaded = record.load()
            if loaded is None:
                print(f"  {tid}: archived WAV missing or changed since indexed", file=sys.stderr)
                continue
            out[tid] = (loaded[0], record.source_id)
            break
        if stored and not matching:
            print(
                f"  {tid}: archived under a different request or source — rendering instead",
                file=sys.stderr,
            )
    if len(out) < len(wanted) and any(r.historical for r in records):
        print(
            f"  {capture_id}/{take_id}: the v1 archive holds this take without a request or "
            f"source identity, so it is not adopted (read it by hand if it is wanted)",
            file=sys.stderr,
        )
    return out


def archive_references(
    archive: Path,
    capture_id: str,
    take_id: str,
    renders: dict[str, np.ndarray],
    identities: dict[str, dict],
) -> None:
    """Keep this take's reference renders, as one new generation, so no later page needs the plugin.

    `identities` gives each timbre's `request`, `request_id` and `source_id`.
    The WAVs are complete in their generation directory before the index names it.
    """
    if not renders:
        return
    gain = shared_gain(renders)
    parent = archive / ARCHIVE_V2_DIR / capture_id / take_id
    staging = staging_dir(parent)
    records = {}
    for name, audio in renders.items():
        wav = staging / f"{name}.wav"
        write_wav(wav, np.clip(audio * gain, -1.0, 1.0), SR, bits=24)
        frames, channels = _frames_channels(audio)
        request = identities[name]["request"]
        records[name] = {
            "request_id": identities[name]["request_id"],
            "request": {
                k: request[k] for k in ("method", "method_version", "sample_rate", "frames")
            },
            "source_id": identities[name]["source_id"],
            "asset_id": file_digest(wav),
            "frames": frames,
            "channels": channels,
        }
    generation = canonical_digest(
        {"timbres": records, "gain": gain, "gain_version": ARCHIVE_GAIN_VERSION}
    )
    target = publish_generation(staging, parent, generation)
    for name, record in records.items():
        record["path"] = str((target / f"{name}.wav").relative_to(archive))
    index = read_archive_index(archive)
    entries = index.setdefault("takes", {}).setdefault(capture_id, {}).setdefault(take_id, [])
    if not any(e.get("generation") == generation for e in entries):
        entries.append(
            {
                "generation": generation,
                "gain": gain,
                "gain_db": round(float(20 * np.log10(max(gain, 1e-9))), 4),
                "gain_version": ARCHIVE_GAIN_VERSION,
                "timbres": records,
            }
        )
    write_json_atomic(archive / ARCHIVE_INDEX, index)


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


def comparison_captures(voice: Voice) -> list[Capture]:
    """Every capture beyond the policy reference that names a genuinely different source.

    `captures_for` can return several captures sharing the reference's own
    `source_class` -- the standard kit's three module grids are one set of
    recordings split by decay window for gating, not a second thing to hear --
    so only the first capture of each OTHER class is offered as a comparison.
    """
    if not voice.captures:
        return []
    seen = {voice.capture.source_class}
    out = []
    for cap in voice.captures[1:]:
        if cap.source_class in seen:
            continue
        seen.add(cap.source_class)
        out.append(cap)
    return out


#: What a comparison capture is, per source class, without naming a product;
#: its `detail` carries this since nothing else on the page says it.
_COMPARISON_KIND = {
    "module": "the module's own recording",
    "library": "a modern recording of the same instrument",
    "dedicated": "a dedicated instrument's recording",
}


def comparison_detail(cap: Capture) -> str:
    kind = _COMPARISON_KIND.get(cap.source_class, "another recording of the same instrument")
    return f"{kind} — {cap.label.split(',')[0]}"


def capture_room(voice: Voice) -> str:
    """The room class of the voice's reference capture, `unclassified` when it names none."""
    if voice.capture is None:
        return ROOM_UNCLASSIFIED
    return str(voice.capture.raw.get("room", ROOM_UNCLASSIFIED))


def capture_rig(voice: Voice) -> str | None:
    """The rig class of the voice's reference capture, None when there is no capture."""
    if voice.capture is None:
        return None
    return str(voice.capture.raw.get("rig", RIG_UNCLASSIFIED))


def model_sends_wet(model_sends: str, voice: Voice, timbres: list[dict]) -> bool:
    """Whether the model renders with the GS reverb/chorus sends at power-on.

    `auto` follows what the reference recording contains (`room`), never the
    host's `dry` instruction: only a reference with a room gets the model's own
    ambience, and an unclassified one is compared without it.
    """
    if model_sends != "auto":
        return model_sends == "gs"
    return voice.capture is not None and bool(timbres) and capture_room(voice) == ROOM_PRESENT


def _rig_explanation(rig: str | None) -> str:
    """What the reference's rig class says about which model path it is comparable with."""
    if rig == "none":
        return (
            "the reference is a direct-input recording with no amplifier in it, "
            "so this direct render is what it is compared with; the rigged `model` "
            "is a separate, secondary block"
        )
    if rig == "baked":
        return (
            "the reference has the rig recorded into it, so `model` (what ships, "
            "with the bank's rig) is what it is comparable with and this render "
            "is what the rig is being asked to work on"
        )
    return (
        "the reference's rig is unclassified, so it is not known whether `model` "
        "or this render is the comparable one"
    )


def build_sources(
    voice: Voice,
    timbres: list[dict],
    variants: list[Variant],
    comparisons: list[tuple[Capture, list[dict]]],
    di: bool = False,
) -> dict:
    """The page's version switch, split into a model row, a reference row and,
    where `comparisons` names one, a row of captures beside the reference
    rather than instead of it.

    Seven versions of a take is an ordinary number once a couple of candidate
    settings are in play, and as one undifferentiated strip of buttons it takes
    reading every label to find which side of the comparison a version is on --
    which is the one thing the page should never make anybody work out.
    """
    detail = f"the library as it stands, no overrides — {voice.label}"
    if voice.patch:
        detail += f", patch {voice.patch}"
    sources = {
        "model": {
            "label": "libsonare NativeSynth (GM fallback)",
            "role": "model",
            "scope": SCOPE_PRODUCT,
            "detail": detail,
        }
    }
    if di:
        sources["model-di"] = {
            "label": "libsonare NativeSynth (GM fallback), direct",
            "role": "model",
            # The boundary is an axis rather than a choice: the same setting
            # heard at two boundaries is not two candidates, and a switch that
            # interleaves them asks one question where there are two.
            "scope": SCOPE_INSTRUMENT,
            "detail": "the same voice with the bank's rig cleared, which is where "
            "the instrument itself stops; " + _rig_explanation(capture_rig(voice)) + ".",
        }
    for variant in variants:
        sources[variant.name] = {
            "label": f"libsonare NativeSynth (GM fallback), {variant.name}",
            "role": "model",
            "scope": SCOPE_PRODUCT,
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
                "scope": SCOPE_INSTRUMENT,
                "detail": "the same candidate with the bank's rig cleared. A setting "
                "that moves the instrument is judged where the instrument "
                "ends, since an amplifier in front of it both hides a change "
                "and invents one: it compresses, so it narrows whatever the "
                "candidate did to the decay. — " + variant.detail,
                **calibration.source_text(variant, direct=True),
            }
    if di and capture_rig(voice) == "none":
        # A DI reference is compared with the direct render; the rigged
        # versions move to a secondary block behind it.
        has_twin = {k[: -len("-di")] for k in sources if k.endswith("-di")}
        for key, src in sources.items():
            if key.endswith("-di"):
                src["block"] = "primary"
            elif key == "model" or key in has_twin:
                src["block"] = "secondary"
        sources = dict(sorted(sources.items(), key=lambda kv: kv[1].get("block") != "primary"))
    reference_of = voice.capture.label.split(",")[0] if voice.capture else ""
    for t in timbres:
        sources[t["id"]] = {
            "label": t["label"],
            "role": "reference",
            "detail": reference_of,
        }
    for cap, cap_timbres in comparisons:
        detail = comparison_detail(cap)
        for t in cap_timbres:
            sources[t["id"]] = {
                "label": t["label"],
                "role": "comparison",
                "detail": detail,
            }
    return sources


def instrument_request(
    product: RenderRequest, smf: bytes, sends: tuple = SENDS_DRY
) -> RenderRequest:
    """The instrument comparison's request: the product's with the rig cleared and the sends off.

    `--model-sends` reaches the product side only. GS sends on the instrument
    side would put the module's ambience into a comparison with a dry
    direct-input recording, so asking for them is refused rather than honoured.
    """
    if tuple(sends) != SENDS_DRY:
        raise ValueError(
            f"the instrument comparison renders with CC91/93/94 at zero, not {tuple(sends)!r}"
        )
    return replace(product, smf=smf, rig=False, sends=SENDS_DRY)


#: The comparisons a page offers, in the order its selector shows them.
COMPARISONS = (
    (
        policy.INSTRUMENT_DI,
        SCOPE_INSTRUMENT,
        (
            "the voice's direct output, bank rig cleared and GS sends at zero, against a "
            "direct-input recording of the instrument"
        ),
    ),
    (
        policy.GM_GS_PRODUCT,
        SCOPE_PRODUCT,
        (
            "the default playback, bank rig and GS effects included, against the slot's "
            "GM/GS reference"
        ),
    ),
)


def scoped_references(voice: Voice, product_rig: bool | None) -> dict[str, str | None]:
    """Which capture id answers each comparison of this voice, per `policy.json`."""
    return policy.references_by_scope(
        policy.load(),
        voice.program,
        bank=voice.bank,
        kit=voice.kit,
        capture=voice.capture.id if voice.capture else None,
        capture_direct=capture_rig(voice) == RIG_NONE,
        product_rig=product_rig,
    )


def build_comparisons(
    voice: Voice,
    sources: dict,
    items: list[dict],
    requests: dict[str, RenderRequest],
    product_rig: bool | None,
) -> list[dict]:
    """The page's comparisons, each judged by `boundary.assess` for display only.

    A comparison whose scope has no capture on this page is `unavailable` and
    lists no oracle: another scope's reference never stands in for it. The
    instrument comparison is offered only for a family that can carry a rig;
    elsewhere no rig exists to clear, and it would repeat the product one.
    """
    played = {key for item in items for key in item.get("tracks") or {}}
    by_id = {cap.id: cap for cap in voice.captures}
    model = {k: s for k, s in sources.items() if s.get("role") == "model"}
    product_keys = [k for k, s in model.items() if s.get("scope") == SCOPE_PRODUCT]
    instrument_keys = [k for k, s in model.items() if s.get("scope") == SCOPE_INSTRUMENT]
    # With no rig bound and the product rendered dry, the rig-cleared render is
    # the product render (the probe compared their digests), so it is not made twice.
    if not instrument_keys and product_rig is False and requests[SCOPE_PRODUCT].sends == SENDS_DRY:
        instrument_keys = product_keys
    scoped = scoped_references(voice, product_rig)
    out = []
    for cid, scope, purpose in COMPARISONS:
        cap_id = scoped[cid]
        if scope == SCOPE_INSTRUMENT and not rig_capable(voice.program):
            continue
        cap = by_id.get(cap_id) if cap_id else None
        oracle = [t["id"] for t in cap.timbres if t["id"] in played] if cap else []
        reference = Reference.from_capture(cap.raw) if oracle else None
        verdict = assess(reference, scope, requests[scope], product_rig=product_rig)
        reasons = list(verdict.reasons)
        if cap_id and not oracle:
            reasons.insert(0, f"{cap_id} is this comparison's reference and is not on this page")
        out.append(
            {
                "id": cid,
                "scope": scope,
                "purpose": purpose,
                "model_sources": instrument_keys if scope == SCOPE_INSTRUMENT else product_keys,
                "oracle_sources": oracle,
                "status": verdict.status,
                "may_sign_off": verdict.may_sign_off,
                "reasons": reasons,
            }
        )
    return out


def reference_note(voice: Voice, timbres: list[dict], model_sends: str = "auto") -> str:
    """The sentence under the title saying what the reference side is worth.

    Read off the timbres actually rendered rather than off the capture, so a
    `--model-only` page of a captured voice does not describe a reference that
    is not on it.
    """
    if voice.capture is None or not timbres:
        return (
            "Nothing is being compared here: this page holds the model alone, "
            "either because no reference has been captured for this voice or "
            "because none was asked for."
        )
    room = capture_room(voice)
    rig = capture_rig(voice)
    wet = model_sends_wet(model_sends, voice, timbres)
    if room == ROOM_PRESENT:
        reference = (
            "The reference carries a room of its own, so part of what is heard on "
            "the reference side is that room. "
        )
    elif room == ROOM_NONE:
        reference = (
            "The reference carries no room, so what is being compared is the "
            "instrument and not a space. "
        )
    else:
        reference = (
            "The reference's room is unclassified: whether the recording contains "
            "a room has not been answered, so none is assumed. "
        )
    by_request = " (set by --model-sends)" if model_sends != "auto" else ""
    if wet:
        model_side = (
            "The model side renders with CC91/93/94 at their GS power-on values"
            f"{by_request}, weighted per program by `gm_fallback_sends` — "
            "libsonare's own ambience."
        )
    else:
        model_side = (
            f"The model side renders with CC91/93/94 zeroed{by_request}, so it carries no room."
        )
    if rig == "none":
        path = (
            " The reference is a direct-input recording (no rig), so the primary "
            "comparison is the direct model render and the rigged one is secondary."
        )
    elif rig == "baked":
        path = " The reference has its rig recorded in, so it is compared with the rigged model."
    else:
        path = " The reference's rig is unclassified, so the model path it is comparable with is not known."
    return reference + model_side + path


def reference_plan(
    cfg: Capture,
    timbre: dict,
    take: Take,
    channel: int,
    total: float,
    smf: bytes,
    program: int,
    bank: int,
):
    """How one timbre's reference is rendered: (request, source_id, render, unresolved).

    Resolved before the archive is asked, since a stored render is adopted only
    for the request and source this run would render now. A plugin source whose
    preset or state is not on this machine has no source_id, and `unresolved`
    says why. Raises `ReferenceUnavailable` for a capture naming no plugin, and
    whatever resolving a module font raises.
    """
    ref_channel = int(timbre.get("slot_channel", timbre.get("channel", channel + 1))) - 1
    if cfg.source_class == "module":
        # Addressed by the overlay's preset name: a family font renumbers the GS map.
        font, preset = resolve_font(cfg.raw, timbre)
        played = {n.note for n in take.notes}
        rows = {
            n: row
            for n, row in _module_reference_rows(font, preset, timbre["id"]).items()
            if n in played
        }
        request = reference_request(
            "module",
            seconds=total,
            sample_rate=SR,
            notes=list(take.notes),
            cc=list(take.cc_events),
            channel=ref_channel,
            tail_s=float(take.tail_s),
        )
        return (
            request,
            module_source_id(cfg.raw, timbre, font, preset, rows),
            lambda: render_module_reference(
                cfg.raw, timbre, take.notes, take.cc_events, ref_channel, take.tail_s, total, SR
            ),
            None,
        )
    if "plugin" not in cfg.raw:
        # No other layer may stand in for what a capture cannot supply.
        raise ReferenceUnavailable(
            f"{cfg.id}/{timbre['id']} names no plugin and is not a module capture, "
            f"so no reference can be rendered here, and --reference-from holds none "
            f"of this take either."
        )
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
    # A take is written in sounding pitch, so an instrument mapped away from
    # it needs its own score even when the channel already matches.
    ref_notes = (
        [replace(n, note=source.key(n.note)) for n in take.notes]
        if source.key_offset
        else take.notes
    )
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
    timbre_smf = (
        smf
        if (ref_channel == channel and not source.key_offset and not source.keyswitch)
        else write_smf(
            ref_notes,
            program=program,
            bank=bank,
            end_pad=take.tail_s,
            cc_events=ref_cc,
            channel=ref_channel,
        )
    )
    source_id, unresolved = None, None
    try:
        source_id = au_source_id(source, cfg.raw)
    except (FileNotFoundError, ValueError) as exc:
        unresolved = str(exc)
    request = reference_request("au", seconds=total, sample_rate=SR, smf=timbre_smf)
    return (
        request,
        source_id,
        lambda: render_oracle_au(timbre_smf, total, SR, source=source),
        unresolved,
    )


def _module_unavailable(cfg: Capture, tid: str, exc: Exception) -> ReferenceUnavailable:
    return ReferenceUnavailable(
        f"{cfg.id}/{tid}'s module reference did not render: {exc}. "
        f"--reference-from holds none of this take either."
    )


def render_reference_timbres(
    cfg: Capture,
    timbres: list[dict],
    take: Take,
    channel: int,
    total: float,
    smf: bytes,
    program: int,
    bank: int,
    args,
    archive: Path | None,
) -> tuple[dict[str, np.ndarray], dict[str, np.ndarray], dict[str, dict]]:
    """Every timbre of one capture, rendered or read from the archive.

    Shared by the page's reference and by each capture `comparison_captures`
    adds beside it, so a comparison is rendered the same way the reference
    is rather than down a second, looser path. Raises `ReferenceUnavailable`
    for the failure that makes the WHOLE capture unusable here (a module
    render error, or a capture naming no plugin); a single timbre the plugin
    itself cannot reach is printed and left out instead, since the rest of the
    capture still answers.

    Returns (renders, fresh, identities): `fresh` is what this run rendered,
    and `identities` gives each timbre's `request`, `request_id`, `source_id`,
    `origin` (`archive` or `rendered`) and `status`: `unverified` for a stored
    render adopted while its source could not be resolved here.
    """
    plans = {}
    for timbre in timbres:
        tid = timbre["id"]
        try:
            plans[tid] = reference_plan(cfg, timbre, take, channel, total, smf, program, bank)
        except (ValueError, RuntimeError, FileNotFoundError) as exc:
            if cfg.source_class != "module":
                raise
            raise _module_unavailable(cfg, tid, exc) from exc
    identities = {
        tid: {
            "request": request,
            "request_id": canonical_digest(request),
            "source_id": source_id,
            "status": REFERENCE_VERIFIED,
            "reason": None,
        }
        for tid, (request, source_id, _, _) in plans.items()
    }
    wanted = {tid: (i["request_id"], i["source_id"]) for tid, i in identities.items()}
    held = archived_references(archive, cfg.id, take.id, wanted) if archive is not None else {}
    renders: dict[str, np.ndarray] = {}
    fresh: dict[str, np.ndarray] = {}
    for tid, (_, _, render, unresolved) in plans.items():
        if tid in held:
            renders[tid], stored_source = held[tid]
            identities[tid]["origin"] = "archive"
            if unresolved:
                identities[tid].update(
                    source_id=stored_source, status=REFERENCE_UNVERIFIED, reason=SOURCE_UNRESOLVED
                )
                print(f"  {tid} (archived, unverified: {unresolved})", file=sys.stderr)
            else:
                print(f"  {tid} (archived)", file=sys.stderr)
            continue
        if unresolved:
            print(f"  {tid}: SKIPPED — source identity unavailable: {unresolved}", file=sys.stderr)
            del identities[tid]
            continue
        try:
            audio = render()
        except (AuRenderError, FileNotFoundError) as exc:
            if cfg.source_class == "module":
                raise _module_unavailable(cfg, tid, exc) from exc
            print(f"  {tid}: SKIPPED — {exc}", file=sys.stderr)
            del identities[tid]
            continue
        except (ValueError, RuntimeError) as exc:
            if cfg.source_class != "module":
                raise
            raise _module_unavailable(cfg, tid, exc) from exc
        fresh[tid] = renders[tid] = audio
        identities[tid]["origin"] = "rendered"
        print(f"  {tid}", file=sys.stderr)
    return renders, fresh, identities


def reference_evidence(identities: dict[str, dict]) -> dict[str, dict]:
    """A reference version's evidence: its identities and whether its source was re-resolved."""
    return {
        tid: {
            "request_id": i["request_id"],
            "source_id": i["source_id"],
            "build_id": None,
            "path": None,
            "complete": None,
            "status": i["status"],
            "reason": i["reason"],
            "origin": i["origin"],
        }
        for tid, i in identities.items()
    }


def render_take(
    take: Take,
    voice: Voice,
    timbres: list[dict],
    out: Path,
    args,
    variants: list[Variant],
    archive: Path | None,
    comparisons: list[tuple[Capture, list[dict]]] = (),
    di_state: dict | None = None,
) -> dict:
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
    wet = model_sends_wet(args.model_sends, voice, timbres)
    smf = write_smf(
        take.notes,
        program=voice.program,
        bank=voice.bank,
        end_pad=take.tail_s,
        cc_events=take.cc_events,
        channel=channel,
        sends=SENDS_POWER_ON if wet else SENDS_DRY,
    )
    product = RenderRequest(
        program=voice.program,
        seconds=total,
        smf=smf,
        bank=voice.bank,
        channel=channel,
        preset=voice.preset,
        sends=SENDS_POWER_ON if wet else SENDS_DRY,
        sample_rate=SR,
    )
    # The instrument side is always dry; a wet product needs its own score for it.
    instrument = instrument_request(
        product,
        write_smf(
            take.notes,
            program=voice.program,
            bank=voice.bank,
            end_pad=take.tail_s,
            cc_events=take.cc_events,
            channel=channel,
            sends=SENDS_DRY,
        )
        if wet
        else smf,
    )
    print(
        f"== {take.id} ({total:.1f}s){' [GS sends at power-on]' if wet else ''} ==", file=sys.stderr
    )

    renders: dict[str, np.ndarray] = {}
    evidence: dict[str, dict] = {}

    def keep(key: str, rendered: RenderedAudio) -> None:
        renders[key] = rendered.audio
        evidence[key] = {**rendered.evidence, "source_id": None}

    # `--lib` has to reach the unmodified voice as well as the variants.
    # Rendering it in-process instead would take whichever library the loader
    # prefers, so a page meant to compare four settings of one constant would be
    # comparing two builds -- and the difference between two build trees is
    # invisible on a listening page and reads as tuning.
    keep(
        "model",
        render_variant_rendered(
            smf, total, SR, "", args.lib, preset=voice.preset, request_id=product.fingerprint()
        )
        if args.lib
        else render_model_rendered(
            smf, total, SR, preset=voice.preset, request_id=product.fingerprint()
        ),
    )
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
    # A preset is a bare patch and the rig is bank data, so there is no second
    # side of the boundary to offer and the probe would render every take twice
    # to prove it.
    if di_state.get("bound") is not False and not voice.preset:
        probe = replace(product, rig=False).fingerprint()
        di = (
            render_variant_rendered(smf, total, SR, "", args.lib, rig=False, request_id=probe)
            if args.lib
            else render_model_rendered(smf, total, SR, rig=False, request_id=probe)
        )
        di_state["bound"] = digest(di.audio) != digest(renders["model"])
        if di_state["bound"] and not wet:
            keep("model-di", di)
    # The instrument comparison renders dry whatever `--model-sends` gave the
    # product, so a wet page renders its rig-cleared side again from the dry score.
    # An unbound voice needs that only where a direct-input reference awaits it.
    direct = bool(di_state.get("bound")) or (
        di_state.get("bound") is False and wet and bool(di_state.get("instrument_oracle"))
    )
    if direct and "model-di" not in renders:
        keep(
            "model-di",
            render_variant_rendered(
                instrument.smf,
                total,
                SR,
                "",
                args.lib,
                rig=False,
                request_id=instrument.fingerprint(),
            )
            if args.lib
            else render_model_rendered(
                instrument.smf, total, SR, rig=False, request_id=instrument.fingerprint()
            ),
        )
    if direct:
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
        keep(
            variant.name,
            render_variant_rendered(
                smf,
                total,
                SR,
                variant.overrides,
                args.lib,
                preset=voice.preset,
                request_id=replace(product, overrides=variant.overrides).fingerprint(),
            ),
        )
        if variant.overrides:
            digests.add(digest(renders[variant.name]))
        print(f"  {variant.name}", file=sys.stderr)
        # A candidate for a rigged voice gets its direct render too, by the same
        # rule `model-di` is asked by: the amplifier compresses, so it narrows
        # whatever the candidate did to the decay and a listener judging the
        # instrument through it is judging the wrong end of the chain.
        if direct and moves_the_instrument(variant.overrides):
            keep(
                f"{variant.name}-di",
                render_variant_rendered(
                    instrument.smf,
                    total,
                    SR,
                    variant.overrides,
                    args.lib,
                    rig=False,
                    preset=voice.preset,
                    request_id=replace(instrument, overrides=variant.overrides).fingerprint(),
                ),
            )
            print(f"  {variant.name}-di", file=sys.stderr)

    cfg = voice.capture
    if cfg is not None and timbres:
        try:
            got, fresh, identities = render_reference_timbres(
                cfg, timbres, take, channel, total, smf, voice.program, voice.bank, args, archive
            )
        except ReferenceUnavailable as exc:
            raise ReferenceUnavailable(
                f"{voice.slug}: {exc} This is the capture `policy.json` aims this voice "
                f"at; playing a different capture's reference instead is the "
                f"substitution this page must not make."
            ) from exc
        renders.update(got)
        evidence.update(reference_evidence(identities))
        # Only a take whose every reference came from the plugin THIS run is
        # written, so the archive never holds a render that has been through
        # 16-bit twice. A partial take is left alone rather than topped up.
        if args.archive_references and len(fresh) == len(timbres):
            archive_references(
                Path(args.archive_references).expanduser().resolve(),
                cfg.id,
                take.id,
                fresh,
                identities,
            )

    # Every OTHER capture `comparison_captures` found beside the reference,
    # rendered the same way and shown under its own role rather than in place
    # of it (`.claude/rules/synth-bank.md`). A capture that cannot render this
    # take is a gap in that capture, not in the page: it is skipped rather than
    # raised, since the reference above already answers the question this take
    # exists for.
    for comp, comp_timbres in comparisons:
        if not comp_timbres:
            continue
        try:
            got, comp_fresh, comp_identities = render_reference_timbres(
                comp,
                comp_timbres,
                take,
                channel,
                total,
                smf,
                voice.program,
                voice.bank,
                args,
                archive,
            )
        except ReferenceUnavailable as exc:
            print(f"  {comp.id}: comparison SKIPPED — {exc}", file=sys.stderr)
            continue
        renders.update(got)
        evidence.update(reference_evidence(comp_identities))
        if args.archive_references and comp_fresh and len(comp_fresh) == len(comp_timbres):
            archive_references(
                Path(args.archive_references).expanduser().resolve(),
                comp.id,
                take.id,
                comp_fresh,
                comp_identities,
            )

    gain = shared_gain(renders)
    tracks = {}
    for key, audio in renders.items():
        rel = Path(take.id) / f"{key}.wav"
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        write_wav(out / rel, np.clip(audio * gain, -1.0, 1.0), SR, bits=24)
        tracks[key] = str(rel)
        evidence[key]["asset_id"] = file_digest(out / rel)

    return {
        "id": take.id,
        "label": take.label,
        "sub": take.sub,
        "group": take.group,
        "tracks": tracks,
        "evidence": {key: evidence[key] for key in tracks},
        "_digests": digests,
        "_requests": {SCOPE_PRODUCT: product, SCOPE_INSTRUMENT: instrument},
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
            "notes": [
                {
                    "note": n.note,
                    "velocity": n.velocity,
                    "start": round(n.start, 4),
                    "duration": round(n.dur, 4),
                }
                for n in take.notes
            ],
            "cc": [[round(t, 4), int(cc), int(v)] for t, cc, v in take.cc_events],
        },
    }


#: A drum note's override key: `d038.percussion.wire_buzz` names note 38. The
#: only prefix in the override key space that carries an instrument a take can
#: be asked to strike; a patch or engine key names no note at all.
_DRUM_KEY = re.compile(r"\bd(\d{3})\.")


def played_notes(items: list[dict]) -> set[int]:
    """Every note number this page's takes actually strike."""
    return {int(n["note"]) for item in items for n in (item.get("meta") or {}).get("notes") or []}


def unheard_drum_notes(tuned: list[Variant], items: list[dict]) -> list[int]:
    """Drum notes these settings move that no take on the page strikes.

    Empty when the settings name no drum note, and empty as soon as ONE of them
    is played: a page that can hear part of a setting is a page worth listening
    to, and the reader is told which takes went unchanged separately.
    """
    named = {int(m.group(1)) for v in tuned for m in _DRUM_KEY.finditer(v.overrides)}
    return (
        sorted(named - played_notes(items)) if named and not (named & played_notes(items)) else []
    )


def render_set(
    voice: Voice, out: Path, args, table: dict[str, list[Variant]], extra: list[Variant]
) -> int:
    """One voice's whole page: every take, every version, and the manifest.

    The settings recorded for this voice come first and the run's own `--variant`
    flags after, which is what makes a batch across the bank carry per-voice
    candidates — one command line cannot.
    """
    variants = calibration.for_voice(voice.slug, table, extra)
    timbres = (
        []
        if args.model_only
        else [
            t
            for t in (voice.capture.timbres if voice.capture else ())
            if not args.wanted_timbres or t["id"] in args.wanted_timbres
        ]
    )
    # Every OTHER capture this voice reaches, filtered to the same timbre
    # selection as the reference so `--timbres` narrows both alike.
    comparisons = [
        (cap, [t for t in cap.timbres if not args.wanted_timbres or t["id"] in args.wanted_timbres])
        for cap in ([] if args.model_only or args.no_comparisons else comparison_captures(voice))
    ]
    comparisons = [(cap, ts) for cap, ts in comparisons if ts]
    # A listening page carries the musical take; a measurement run does not, and
    # `--no-music` is for the case where the ten seconds of polyphony are just
    # render time — a sweep across the bank narrowed with `--only`, say.
    music = None if args.no_music else (voice.capture.raw.get("music", "") if voice.capture else "")
    selected = [
        t
        for t in build_takes(voice.take_set, voice.program, music=music)
        if not args.only_takes or t.id in args.only_takes
    ]
    if not selected:
        print(f"{voice.slug}: no takes selected", file=sys.stderr)
        return 0

    archive = Path(args.reference_from).expanduser().resolve() if args.reference_from else None
    print(f"\n### {voice.label}  ->  {out}", file=sys.stderr)

    items = []
    requests: list[dict[str, RenderRequest]] = []
    variant_digests: dict[str, set[str]] = {}
    # The instrument reference does not depend on whether the product binds a rig.
    di_state: dict = {
        "instrument_oracle": bool(timbres)
        and scoped_references(voice, None)[policy.INSTRUMENT_DI] is not None
    }
    generations = out / GENERATIONS_DIR
    staging = staging_dir(generations)
    try:
        for take in selected:
            item = render_take(
                take, voice, timbres, staging, args, variants, archive, comparisons, di_state
            )
            variant_digests[take.id] = item.pop("_digests")
            requests.append(item.pop("_requests"))
            items.append(item)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    sources = build_sources(
        voice,
        timbres,
        variants,
        comparisons,
        di=any("model-di" in (i.get("tracks") or {}) for i in items),
    )

    # Display copy only: a reader that needs a verdict re-derives it with
    # `boundary.assess`. Every take shares the boundary, so the first speaks for all.
    comparisons_out = build_comparisons(voice, sources, items, requests[0], di_state.get("bound"))
    generation = set_generation(items, comparisons_out)
    # Every asset is in place before the manifest that names it is replaced.
    prefix = publish_generation(staging, generations, generation).relative_to(out)
    for item in items:
        item["tracks"] = {key: str(prefix / rel) for key, rel in item["tracks"].items()}

    manifest = {
        "schema_version": 2,
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
        "voice": {**voice.describe(), "rig": capture_rig(voice)},
        "notes": ((args.note + " ") if args.note else "")
        + (
            "Every version of a take is written at one shared gain, so the level "
            "difference between them is real. " + reference_note(voice, timbres, args.model_sends)
        ),
        # Display only; the identity of what was rendered is `set_generation`.
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "set_generation": generation,
        "sources": sources,
        "comparisons": comparisons_out,
        "items": items,
    }
    write_json_atomic(out / "manifest.json", manifest)

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
                print(
                    f"WARNING: no setting changed the render on any take "
                    f"({', '.join(v.name for v in tuned)}), because no take on "
                    f"this page\n         strikes the drum notes they move: "
                    f"{', '.join(str(n) for n in unplayed)}. The takes play "
                    f"{', '.join(str(n) for n in sorted(played_notes(items)))}."
                    f"\n         This is a gap in the take set, not in the "
                    f"settings.",
                    file=sys.stderr,
                )
            else:
                print(
                    f"WARNING: no setting changed the render on any take "
                    f"({', '.join(v.name for v in tuned)}).\n         The library has "
                    f"no tuning override layer -- rebuild it with -DBUILD_TUNING=ON, "
                    f"or point\n         --lib at one that has; or the keys reach "
                    f"nothing this voice consults.",
                    file=sys.stderr,
                )
        elif identical:
            print(
                f"note: {len(identical)} take(s) render identically across the "
                f"settings: {', '.join(sorted(identical))}",
                file=sys.stderr,
            )
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
        print(
            f"note: no knob catalogue ({str(exc).splitlines()[0]}); "
            f"listing bank 0 only, with no patch names",
            file=sys.stderr,
        )
        return None


def preset_names(lib: str = "") -> tuple[str, ...]:
    """Every public preset the library reports, asked rather than mirrored.

    The catalogue is built in C++ and has no tracked manifest anywhere, so a
    list written here would be a copy that drifts the moment an entry is added
    — and the drift would read as "that preset does not exist" rather than as a
    stale table.
    """
    from render_model import ensure_lib_path

    if lib:
        os.environ["SONARE_LIB_PATH"] = lib
    else:
        ensure_lib_path()
    import libsonare

    return tuple(libsonare.synth_preset_names())


class Unselectable(Exception):
    """What to audition could not be worked out. The message is for the user.

    Raised rather than exited on, so the reason reaches `main` and leaves by
    the one path a caller can test: stderr and a usage exit code. A capture
    with no phrase set has to stop here in particular — the alternative is
    rendering it on whichever set was nearest, which plays, looks like a
    successful comparison, and is of the wrong music.
    """


class ReferenceUnavailable(Exception):
    """`Voice.capture` cannot supply the reference this page needs, and nothing else may.

    The page's reference is the capture `policy.json` aims this voice at — that
    is what `Voice.capture` means — so a capture that cannot render one is a
    defect to report, never a reason to show a different layer's capture under
    the same label. A listener comparing the model against the wrong instrument
    is a worse failure than a page that says so and stops.
    """


#: (font path, preset name, timbre id) -> {note: reference row}, merged across
#: every module capture naming that exact (font, preset, timbre).
_MODULE_REFERENCE_ROW_CACHE: dict[tuple[str, str, str], dict[int, dict]] = {}


def _module_reference_rows(
    font_path: Path,
    preset_name: str,
    timbre_id: str,
    *,
    capture_dir: Path | None = None,
    reference_dir: Path | None = None,
) -> dict[int, dict]:
    """Every measured row for this exact (font, preset, timbre), across all module captures.

    A module's recordings are split across several grids (the standard kit is read
    at 17 and again at 44 keys), and a take plays whatever notes its phrase needs.
    """
    key = (str(font_path), preset_name, timbre_id)
    if key in _MODULE_REFERENCE_ROW_CACHE:
        return _MODULE_REFERENCE_ROW_CACHE[key]
    here = Path(__file__).resolve().parent
    capture_dir = capture_dir or here / "capture"
    reference_dir = reference_dir or here / "reference"
    rows: dict[int, dict] = {}
    for path in sorted(capture_dir.glob("*.json")):
        if path.name.endswith(".local.json"):
            continue
        try:
            other_cfg = capture_load_config(path)
        except (ValueError, KeyError, OSError, json.JSONDecodeError):
            continue
        if other_cfg.get("source_class") != "module":
            continue
        for other_timbre in other_cfg.get("timbres", []):
            if other_timbre.get("id") != timbre_id:
                continue
            try:
                other_font, other_preset = resolve_font(other_cfg, other_timbre)
            except ValueError:
                continue
            if other_font != font_path or other_preset != preset_name:
                continue
            ref_path = reference_dir / f"{other_cfg['id']}.json"
            if not ref_path.exists():
                continue
            for row in json.loads(ref_path.read_text())["rows"]:
                if row["timbre"] == timbre_id:
                    rows.setdefault(row["note"], row)
    _MODULE_REFERENCE_ROW_CACHE[key] = rows
    return rows


def module_reference_gain(
    cfg: dict,
    timbre: dict,
    font_path: Path,
    preset: object,
    notes: list[Note],
    channel: int,
    tail_s: float,
    sr: int,
) -> float:
    """The gain that puts a module render at the level `import_sf2.py` extracted it at.

    fluidsynth's velocity curve and output stage put a note 10-30 dB under the raw
    sample, by an offset that differs 3-9 dB between notes. So each distinct note of
    the take is rendered alone at its own reference velocity, compared with that
    row's `peak_dbfs`, and the median offset is applied to the whole render.
    """
    rows = _module_reference_rows(font_path, preset.name, timbre["id"])
    distinct = sorted({n.note for n in notes if n.note in rows})
    if not distinct:
        raise ValueError(
            f"{cfg['id']}/{timbre['id']}: none of this take's notes has a measured "
            f"row anywhere for {font_path.name} {preset.name!r} to calibrate the "
            f"render's level against"
        )
    gate_s = float(cfg["gate_ms"]) / 1000.0
    deltas = []
    for note in distinct:
        row = rows[note]
        probe = write_smf(
            [Note(note=note, velocity=int(row["velocity"]), start=0.0, dur=gate_s)],
            program=preset.program,
            bank=0 if preset.bank >= 128 else preset.bank,
            end_pad=tail_s,
            channel=channel,
        )
        rendered = render_oracle_fluidsynth(probe, gate_s + tail_s, sr, soundfont=font_path)
        mono = rendered.mean(axis=1) if rendered.ndim > 1 else rendered
        deltas.append(float(row["peak_dbfs"]) - float(_db(np.abs(mono).max())))
    return float(10.0 ** (float(np.median(deltas)) / 20.0))


def render_module_reference(
    cfg: dict,
    timbre: dict,
    notes: list[Note],
    cc_events: tuple[tuple[float, int, int], ...],
    channel: int,
    tail_s: float,
    total_seconds: float,
    sr: int,
) -> np.ndarray:
    """A take's reference, played from a module's own font at the tone it recorded.

    Addressed by the (bank, program) the font itself reports for the named
    preset, never by the GM number the model answers to — a module family file
    flattens the GS map into its own sequential numbering, so a preset's
    position has no relation to the program it stands beside on the page (see
    `.claude/rules/synth-bank.md`).
    """
    font_path, preset_name = resolve_font(cfg, timbre)
    with SoundFont(font_path) as font:
        preset = font.find_by_name(preset_name)
    # SF2 bank 128 is reached by the percussion channel alone (measured), so it is never sent.
    if preset.bank >= 128 and channel != 9:
        raise ValueError(
            f"{font_path.name}: {preset_name!r} sits at SF2 bank {preset.bank}, which "
            f"this font's own layout reserves for percussion (channel 10) — but this "
            f"timbre is addressed on channel {channel + 1}, where nothing reaches it"
        )
    smf = write_smf(
        notes,
        program=preset.program,
        bank=0 if preset.bank >= 128 else preset.bank,
        end_pad=tail_s,
        cc_events=cc_events,
        channel=channel,
    )
    audio = render_oracle_fluidsynth(smf, total_seconds, sr, soundfont=font_path)
    gain = module_reference_gain(cfg, timbre, font_path, preset, notes, channel, tail_s, sr)
    return audio * gain


def resolve_voices(args) -> list[Voice]:
    """What this run was asked to audition, as bank entries."""
    if args.preset:
        if args.program is None:
            raise Unselectable(
                f"--preset {args.preset} needs --program N as well: a catalogue entry "
                "carries no GM number, so nothing else says which phrase set and tone "
                "class to sound it on. Name the program the entry is voiced beside"
            )
        known = preset_names(args.lib)
        if args.preset not in known:
            near = [n for n in known if args.preset in n][:6]
            raise Unselectable(
                f"the library reports no preset named {args.preset!r}"
                + (
                    f" — did you mean {', '.join(near)}?"
                    if near
                    else f" ({len(known)} exist; run with --preset '' to see none of them)"
                )
            )
        return [Voice(program=args.program, preset=args.preset)]

    if args.config:
        capture = load_capture(Path(args.config).expanduser().resolve())
        if capture is None:
            raise Unselectable(f"{args.config} names no phrase set (`takes`)")
        program = args.program if args.program is not None else capture.program
        return [Voice(program=program, bank=capture.bank, kit=capture.drums, captures=(capture,))]

    programs = parse_selection(args.programs) if args.programs else []
    if args.program is not None:
        programs.append(args.program)
    kits = parse_selection(args.kits) if args.kits else []
    if not programs and not kits:
        raise Unselectable(
            "name what to audition: --program N, --programs 0-7,40, --kits 0, "
            "--programs all, or --config <capture>"
        )

    banks = parse_selection(args.banks) if args.banks else None
    catalogue = load_catalogue(args.lib) if banks is None and programs else None
    return voices(sorted(set(programs)), banks=banks, kits=kits, catalogue=catalogue)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    pick = ap.add_argument_group("what to audition")
    pick.add_argument("--program", type=int, default=None, help="a GM program number")
    pick.add_argument(
        "--programs", default="", help="several: `0-7,40,73`, or `all` for the whole bank"
    )
    pick.add_argument(
        "--banks",
        default="",
        help="GS variation banks to render each program at (default: "
        "every bank the library voices apart, or 0 without a "
        "tuning build)",
    )
    pick.add_argument(
        "--kits",
        default="",
        help="drum kit numbers, rendered on channel 10 (`0` is the GM standard kit)",
    )
    pick.add_argument(
        "--preset",
        default="",
        help="a public preset catalogue entry by name (`muted-trumpet`). "
        "The catalogue is a second address space over the same "
        "engines and carries no GM number, so --program is required "
        "alongside it and says only which phrase set to sound it on. "
        "No capture answers a catalogue entry, so the set is "
        "model-only by construction",
    )
    pick.add_argument(
        "--config",
        default="",
        help="a capture definition, when the capture is the subject: it "
        "fixes the program, the phrase set and the reference timbres",
    )

    ap.add_argument(
        "--out",
        default="",
        help=f"one subdirectory per voice is written under here "
        f"(default: {DEFAULT_OUT.name}/ under the scratch root, "
        f"or {DEFAULT_PROBE_OUT.name}/ with --probe)",
    )
    ap.add_argument(
        "--probe",
        action="store_true",
        help="a measurement run, not a listening page: writes under the "
        "probe root, which the listening server does not discover, "
        "and marks the manifest so it stays unserved wherever it is",
    )
    ap.add_argument(
        "--timbres",
        default="",
        help="comma-separated timbre ids to render (default: all the voice's capture has)",
    )
    ap.add_argument(
        "--model-only", action="store_true", help="skip the reference renders even where one exists"
    )
    ap.add_argument(
        "--no-comparisons",
        action="store_true",
        help="skip every capture beyond the policy reference, even where "
        "`comparison_captures` finds one",
    )
    ap.add_argument(
        "--no-music",
        action="store_true",
        help="leave off the musical take — ten seconds of real Bach, "
        "which nothing measures and every page otherwise carries",
    )
    ap.add_argument("--only", default="", help="comma-separated take ids")
    ap.add_argument(
        "--model-sends",
        choices=("auto", "gs", "dry"),
        default="auto",
        help="what the model side does with CC91/93/94: 'auto' leaves them "
        "at the GS power-on values where the reference carries a room "
        "and zeroes them where it does not, 'gs' always leaves them "
        "(the way the library ships, which is what a question about "
        "the shared GS tank has to be heard through), 'dry' always "
        "zeroes them",
    )
    ap.add_argument(
        "--variant",
        action="append",
        default=[],
        metavar="NAME=OVERRIDES",
        help="an extra version of every take, rendered under this "
        "SONARE_TUNING_OVERRIDES string; repeatable, and applied to "
        "every voice in the run",
    )
    ap.add_argument(
        "--calibrations",
        nargs="?",
        const=str(calibration.DEFAULT_PATH),
        default="",
        metavar="FILE",
        help="also render each voice's recorded calibration settings "
        f"(default file: {calibration.DEFAULT_PATH.name}). Off unless "
        "asked for: every setting is another render of every take, and "
        "the override layer needs a -DBUILD_TUNING=ON library",
    )
    ap.add_argument(
        "--lib",
        default="",
        help="library the variants load (a -DBUILD_TUNING=ON build); sets SONARE_LIB_PATH for them",
    )
    ap.add_argument(
        "--title",
        default="",
        help="what this page is for, shown in the set picker (default: the "
        "voice's own name, which is the right answer until two pages "
        "of one voice are up at once)",
    )
    ap.add_argument(
        "--note", default="", help="a sentence at the top of the page saying what to listen for"
    )
    ap.add_argument(
        "--reference-from",
        default=str(DEFAULT_REFERENCE_ARCHIVE),
        dest="reference_from",
        metavar="DIR",
        help="take reference renders from this archive instead of the plugin, "
        "falling back to the plugin for any it does not hold. Empty string "
        "to always render",
    )
    ap.add_argument(
        "--archive-references",
        default="",
        dest="archive_references",
        metavar="DIR",
        help="write every reference render this run produced into DIR, so the "
        "next page can be built without the plugin",
    )
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
            print(
                f"note: {len(unknown)} recorded voice(s) not in this run: {', '.join(unknown)}",
                file=sys.stderr,
            )
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
    try:
        for voice in selected:
            total += render_set(voice, root / voice.slug, args, table, extra)
    except ReferenceUnavailable as exc:
        print(exc, file=sys.stderr)
        return 2

    write_index(root, selected)
    print(f"\n{len(selected)} voice(s), {total} takes -> {root}", file=sys.stderr)
    if root.is_relative_to(CORPUS_ROOT):
        print(
            "listen:  python tools/audition/serve.py"
            "   (under the scratch root, so it is found with no argument)",
            file=sys.stderr,
        )
    else:
        print(f"listen:  python tools/audition/serve.py {root}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
