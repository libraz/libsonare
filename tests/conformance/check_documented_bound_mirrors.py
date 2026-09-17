"""Hold every numeric bound a facade doc states equal to the core constant that enforces it.

A consumer cannot read a core constant, so a limit reaches them only as a
number written into a C header comment, a TypeScript interface doc, a ``.pyi``
or a Python docstring. Those numbers are copies, and a copy nothing compares
drifts silently in both directions: a widened bound reads as a restriction that
no longer exists, a narrowed one promises a value the library refuses.

Each entry names the constant and the *wording* of the claim, never the number
-- the number is a capture group, so a doc whose value moves still matches and
is reported as a mismatch rather than vanishing from the scan. Sites are
discovered rather than listed, so a new doc repeating a registered claim is
covered the day it is written.

Two vacuity guards, because a scan that matches nothing and exits 0 reads as
coverage: a constant this check cannot locate is a failure, and every claim
carries the number of sites it reached when it was written, so wording that
stops matching fails instead of quietly measuring less.

Failures are grouped by constant, so moving a constant reports one line naming
every document left stale rather than one line per document.
"""

from __future__ import annotations

import argparse
import re
import sys
import typing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The documentation this check reads. Nothing outside these trees is a facade
# doc, and a number in ordinary code is not a claim.
DOC_TREES = (
    ("include/sonare", ("*.h",)),
    ("bindings/node/src", ("*.ts",)),
    ("bindings/wasm/src", ("*.ts",)),
    ("bindings/python/src/libsonare", ("*.py", "*.pyi")),
)


class Constant(typing.NamedTuple):
    """A core constant, qualified by the class that declares it when it has one."""

    path: str
    name: str
    scope: str | None = None

    @property
    def display(self) -> str:
        return f"{self.scope}::{self.name}" if self.scope else self.name


class Claim(typing.NamedTuple):
    """One documented bound: the wording that states it and what each number is.

    ``groups`` maps a capture group to the constant it must equal and an offset
    applied to that constant first -- a doc that states the largest legal value
    of a ceiling carries ceiling - 1, which is a mirror of the same constant.
    """

    key: str
    pattern: str
    groups: dict[str, tuple[str, int]]
    floor: int


CONSTANTS: dict[str, Constant] = {
    "spectral_edit_max_n_fft": Constant("src/effects/spectral_edit.h", "kSpectralEditMaxNFft"),
    "max_stft_n_fft": Constant("src/core/spectrum.h", "kMaxStftNFft"),
    "min_sample_rate": Constant("src/core/audio.h", "kMinAudioSampleRate"),
    "max_sample_rate": Constant("src/core/audio.h", "kMaxAudioSampleRate"),
    "max_public_tempo_bpm": Constant("src/transport/tempo_map.h", "kMaxPublicTempoBpm"),
    "min_assistant_tempo_bpm": Constant("src/mixing/assistant/suggester.h", "kMinAssistantTempoBpm"),
    "max_assistant_tempo_bpm": Constant("src/mixing/assistant/suggester.h", "kMaxAssistantTempoBpm"),
    "min_tuning_ref_hz": Constant("src/streaming/stream_config.h", "kMinTuningRefHz"),
    "max_tuning_ref_hz": Constant("src/streaming/stream_config.h", "kMaxTuningRefHz"),
    "max_command_capacity": Constant(
        "src/engine/realtime_engine.h", "kMaxCommandCapacity", scope="RealtimeEngine"
    ),
    "max_telemetry_capacity": Constant(
        "src/engine/realtime_engine.h", "kMaxTelemetryCapacity", scope="RealtimeEngine"
    ),
    "max_bank_sample_points": Constant("src/c_api/sonare_c_sample_bank.cpp", "kMaxBankSamplePoints"),
    "max_keymap_sets": Constant("src/c_api/sonare_c_sample_bank.cpp", "kMaxKeymapSets"),
    "declip_max_lpc_gap": Constant("src/mastering/repair/declip.h", "kDeclipMaxLpcGapSamples"),
    "max_griffin_lim_iterations": Constant("src/util/resource_limits.h", "kMaxGriffinLimIterations"),
    "max_synth_voices": Constant("src/midi/builtin_synth.h", "kMaxSynthVoices"),
    "max_meter_numerators": Constant("src/analysis/meter_analyzer.h", "kMaxMeterCandidateNumerators"),
    "max_meter_numerator": Constant("src/analysis/meter_analyzer.h", "kMaxMeterCandidateNumerator"),
    "max_meter_denominator": Constant("src/analysis/meter_analyzer.h", "kMaxMeterDenominator"),
    "max_grain_size": Constant("src/editing/voice_changer/streaming_retune.cpp", "kMaxGrainSize"),
    "max_retune_semitones": Constant("src/editing/voice_changer/streaming_retune.cpp", "kMaxSemitones"),
    "max_metronome_click_samples": Constant("src/engine/metronome.h", "kMaxMetronomeClickSamples"),
    "max_salience_harmonics": Constant("src/editing/polyphony/f0_salience.h", "kMaxSalienceHarmonics"),
    "max_note_mask_harmonics": Constant("src/editing/polyphony/note_mask.h", "kMaxNoteMaskHarmonics"),
    "max_polyphony_voices": Constant("src/editing/polyphony/multi_f0.h", "kMaxPolyphonyVoices"),
}

# Bounds deliberately left without a constant, and why. A family named here is
# absent from CONSTANTS on purpose, so the "registered but no claim reads it"
# guard never sees it; this table is what keeps that absence a decision.
UNMIRRORED: dict[str, str] = {
    "true-peak oversample factor": (
        "`is_supported_polyphase_oversample_factor` (src/rt/true_peak_fir.cpp) admits the set "
        "{1, 2, 4, 8, 16}, each a separately designed FIR rather than a point in a range, so a "
        "constant naming the largest would put a mirrorable number on one endpoint of a "
        "composite condition and invite a reader to believe the interval between the members is "
        "legal. The five documents state the set, not an endpoint, and are correct as written. "
        "bindings/wasm/src/metering.ts re-states the condition in TypeScript ((n & (n - 1)) === 0) "
        "instead of calling the predicate; that copy is real exposure a number-to-constant check "
        "cannot compare."
    ),
}

# The interval brackets a doc writes around a pair, with the markup (backticks,
# reStructuredText double backticks) each surface wraps them in.
_OPEN = r"[^\w\d]{0,4}\[\s*"
_MID = r"\s*,\s*"
_CLOSE = r"\s*\]"

CLAIMS: tuple[Claim, ...] = (
    Claim(
        key="spectral edit n_fft ceiling",
        pattern=r"power of two in" + _OPEN + r"2" + _MID + r"(?P<max>\d+)" + _CLOSE,
        groups={"max": ("spectral_edit_max_n_fft", 0)},
        floor=4,
    ),
    Claim(
        key="HPSS median kernel largest legal size",
        pattern=r"positive odd integer at most\s+(?P<largest>\d+)",
        groups={"largest": ("max_stft_n_fft", -1)},
        floor=10,
    ),
    Claim(
        key="HPSS median kernel ceiling",
        pattern=r"[Tt]he ceiling is\s+(?P<ceiling>\d+)",
        groups={"ceiling": ("max_stft_n_fft", 0)},
        floor=9,
    ),
    Claim(
        key="HPSS median kernel largest restated",
        pattern=r"so\s+(?P<largest>\d+)\s+is the largest legal",
        groups={"largest": ("max_stft_n_fft", -1)},
        floor=9,
    ),
    Claim(
        key="audio sample rate range",
        pattern=r"[Ss]ample.?rate[\s\S]{0,110}?"
        + _OPEN
        + r"(?P<lo>\d{4,})"
        + _MID
        + r"(?P<hi>\d{4,})"
        + _CLOSE,
        groups={"lo": ("min_sample_rate", 0), "hi": ("max_sample_rate", 0)},
        floor=9,
    ),
    Claim(
        key="public tempo ceiling",
        pattern=r"\(\s*0\s*,\s*(?P<max>\d{3,})\s*\]`{0,2}\s*(?:BPM|;|,)",
        groups={"max": ("max_public_tempo_bpm", 0)},
        floor=7,
    ),
    Claim(
        key="mixing assistant tempo range",
        pattern=r"outside\s+(?P<lo>\d+)\s*(?:\.\.|[-–—])\s*(?P<hi>\d+)\s*BPM",
        groups={"lo": ("min_assistant_tempo_bpm", 0), "hi": ("max_assistant_tempo_bpm", 0)},
        floor=4,
    ),
    Claim(
        key="A4 tuning reference range",
        # The trailing guard is the unit: a bare `lo..hi` is this claim, and so
        # is one voiced in Hz, but a range carrying any other unit states a
        # different bound. Without it the assistant's `20..400 BPM` read as a
        # tuning range and reported the tempo constants as stale Hz.
        pattern=r"(?:within|outside|range)\s+(?P<lo>\d+)\.\.(?P<hi>\d+)(?!\d|\s+(?!Hz\b)\w)",
        groups={"lo": ("min_tuning_ref_hz", 0), "hi": ("max_tuning_ref_hz", 0)},
        floor=10,
    ),
    Claim(
        key="realtime command ring capacity ceiling",
        pattern=r"[Cc]ommand.?[Cc]apacity`{0,2} must not\s+exceed\s+(?P<max>\d+)",
        groups={"max": ("max_command_capacity", 0)},
        floor=3,
    ),
    Claim(
        key="realtime telemetry ring capacity ceiling",
        pattern=r"exceed\s+(?P<max>\d+);\s+a larger value",
        groups={"max": ("max_telemetry_capacity", 0)},
        floor=3,
    ),
    Claim(
        key="sample bank total sample points ceiling",
        pattern=r"exceed\s+(?P<max>[\d,]+)\s+sample points",
        groups={"max": ("max_bank_sample_points", 0)},
        floor=3,
    ),
    Claim(
        key="sample bank keymap set ceiling",
        pattern=r"`?set_?[Ii]ndex`?[^.]{0,40}?(?:at or above|below)\s+(?P<max>\d+)",
        groups={"max": ("max_keymap_sets", 0)},
        floor=5,
    ),
    Claim(
        key="declip LPC reconstructed run ceiling",
        pattern=r"clipped runs of at most\s+(?P<max>\d+)\s+consecutive samples",
        groups={"max": ("declip_max_lpc_gap", 0)},
        floor=4,
    ),
    Claim(
        key="Griffin-Lim iteration ceiling",
        pattern=r"[Ii]terations must be in" + _OPEN + r"1" + _MID + r"(?P<max>\d+)" + _CLOSE,
        groups={"max": ("max_griffin_lim_iterations", 0)},
        floor=1,
    ),
    Claim(
        key="simultaneous voice ceiling",
        pattern=r"[Vv]oices[^.]{0,40}?" + _OPEN + r"1" + _MID + r"(?P<max>\d+)" + _CLOSE,
        groups={"max": ("max_synth_voices", 0)},
        floor=10,
    ),
    Claim(
        key="meter candidate numerator count ceiling",
        pattern=r"[Aa]t most\s+(?P<max>\d+)\s+entries",
        groups={"max": ("max_meter_numerators", 0)},
        floor=4,
    ),
    Claim(
        key="meter candidate numerator range",
        pattern=r"(?:each (?:must be )?in|numerators?[^.]{0,40}?)"
        + _OPEN
        + r"2"
        + _MID
        + r"(?P<max>\d+)"
        + _CLOSE,
        groups={"max": ("max_meter_numerator", 0)},
        floor=8,
    ),
    Claim(
        key="meter beat unit range",
        pattern=r"[Bb]eat unit[^.]{0,60}?" + _OPEN + r"1" + _MID + r"(?P<max>\d+)" + _CLOSE,
        groups={"max": ("max_meter_denominator", 0)},
        floor=6,
    ),
    Claim(
        key="retune grain size ceiling",
        pattern=r"above the\s+(?P<max>\d+)\s+ceiling",
        groups={"max": ("max_grain_size", 0)},
        floor=1,
    ),
    Claim(
        key="retune semitone clamp",
        pattern=r"\+/-\s?(?P<max>\d+)",
        groups={"max": ("max_retune_semitones", 0)},
        floor=3,
    ),
    Claim(
        key="explicit metronome click length ceiling",
        pattern=r"click lengths are limited to\s+(?P<max>\d+)\s+samples",
        groups={"max": ("max_metronome_click_samples", 0)},
        floor=1,
    ),
    # The two polyphony harmonic ceilings hold the same number on separate
    # fields of separate stages, so each anchors on what its partials are for --
    # matching on 128 would let either claim answer for both.
    Claim(
        key="polyphony salience harmonic ceiling",
        pattern=r"[Pp]artials summed per (?:F0 )?candidate.{0,60}?at most\s+(?P<max>\d+)",
        groups={"max": ("max_salience_harmonics", 0)},
        floor=4,
    ),
    Claim(
        key="polyphony note mask harmonic ceiling",
        pattern=r"[Pp]artials claimed per note.{0,60}?at most\s+(?P<max>\d+)",
        groups={"max": ("max_note_mask_harmonics", 0)},
        floor=4,
    ),
    Claim(
        key="polyphony voice ceiling",
        pattern=r"[Vv]oices (?:a|one) frame (?:is allowed|may hold).{0,40}?at most\s+(?P<max>\d+)",
        groups={"max": ("max_polyphony_voices", 0)},
        floor=3,
    ),
    # The C header states the same ceiling as a bare sentinel-and-limit pair,
    # sharing no wording with the three facades, so it reads the constant
    # through a claim of its own rather than widening theirs to reach it.
    Claim(
        key="polyphony voice ceiling in the C header",
        pattern=r"0 => 4, at most\s+(?P<max>\d+)",
        groups={"max": ("max_polyphony_voices", 0)},
        floor=1,
    ),
)


class Site(typing.NamedTuple):
    """One place a claim was found: where it is and the numbers it states."""

    claim: str
    path: str
    line: int
    values: dict[str, int]

    @property
    def display(self) -> str:
        return f"{self.path}:{self.line}"


# A run of consecutive line comments is one block: a claim wraps across `///`
# lines constantly, and a per-line match cannot see a wrapped sentence at all.
_CPP_DOC = re.compile(
    r"/\*\*.*?\*/|/\*.*?\*/|(?:[ \t]*///[^\n]*\n)*[ \t]*///[^\n]*|(?:[ \t]*//[^\n]*\n)*[ \t]*//[^\n]*",
    re.DOTALL,
)
_PY_DOC = re.compile(r"\"\"\".*?\"\"\"|'''.*?'''|(?:[ \t]*\#[^\n]*\n)*[ \t]*\#[^\n]*", re.DOTALL)
_LEADER = re.compile(r"^\s*(?:///|//|/\*\*|/\*|\*/|\*|#)?\s*")


def _flatten(block: str, first_line: int) -> tuple[str, list[int]]:
    """A doc block as one line, with the source line each character came from.

    A claim wraps across comment lines far more often than not, so matching the
    raw block would need the leader and the wrap in every pattern.
    """
    text: list[str] = []
    origin: list[int] = []
    for offset, raw in enumerate(block.splitlines()):
        stripped = _LEADER.sub("", raw).rstrip().removesuffix("*/").rstrip()
        for _ in range(len(stripped) + 1):
            origin.append(first_line + offset)
        text.append(stripped + " ")
    return "".join(text), origin


def doc_blocks(root: Path) -> list[tuple[str, str, list[int]]]:
    """(path, flattened doc text, per-character source line) for every doc comment."""
    blocks: list[tuple[str, str, list[int]]] = []
    for relative, globs in DOC_TREES:
        base = root / relative
        if not base.is_dir():
            continue
        for path in sorted({p for glob in globs for p in base.rglob(glob)}):
            text = path.read_text(encoding="utf-8")
            doc = _PY_DOC if path.suffix in {".py", ".pyi"} else _CPP_DOC
            for match in doc.finditer(text):
                flat, origin = _flatten(match.group(0), text.count("\n", 0, match.start()) + 1)
                blocks.append((str(path.relative_to(root)), flat, origin))
    return blocks


# Braces, class heads and numeric constexpr declarations, read left to right so
# a declaration can be attributed to the class body it sits in.
_EVENT = re.compile(
    r"(?P<open>\{)"
    r"|(?P<close>\})"
    r"|(?P<semi>;)"
    r"|\b(?:class|struct)\s+(?P<scope>\w+)"
    r"|\b(?:static\s+)?constexpr\s+[\w:]+\s+(?P<name>\w+)\s*=\s*(?P<value>[^;{]*)"
)

_CPP_LEXICAL = re.compile(
    r"""//[^\n]*
      | /\*.*?\*/
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)


def _blank(match: re.Match[str]) -> str:
    """Replace a matched span with spaces, keeping its newlines and its length."""
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


def _number(text: str) -> int | None:
    """An integral constant value, whether it was written as an int or a float."""
    match = re.fullmatch(r"\s*(\d+)(?:\.(\d*))?[fFuUlL]*\s*", text)
    if match is None or (match.group(2) or "").strip("0"):
        return None
    return int(match.group(1))


def find(root: Path, constant: Constant) -> int | None:
    """The value @p constant declares, or None -- never a near miss."""
    path = root / constant.path
    if not path.is_file():
        return None
    stripped = _CPP_LEXICAL.sub(_blank, path.read_text(encoding="utf-8"))
    found: list[int] = []
    stack: list[tuple[str, int]] = []
    depth = 0
    pending: str | None = None
    for event in _EVENT.finditer(stripped):
        if event.group("open") is not None:
            depth += 1
            if pending is not None:
                stack.append((pending, depth))
                pending = None
        elif event.group("close") is not None:
            if stack and stack[-1][1] == depth:
                stack.pop()
            depth -= 1
        elif event.group("semi") is not None:
            pending = None
        elif event.group("scope") is not None:
            pending = event.group("scope")
        else:
            pending = None
            value = _number(event.group("value"))
            # A member is declared at its class body's own depth; anything
            # deeper is a local inside a member function.
            scope = stack[-1][0] if stack and stack[-1][1] == depth else None
            if value is not None and event.group("name") == constant.name and scope == constant.scope:
                found.append(value)
    return found[0] if len(found) == 1 else None


def collect(
    root: Path,
    blocks: list[tuple[str, str, list[int]]],
    claims: tuple[Claim, ...] = CLAIMS,
) -> dict[str, list[Site]]:
    """Every site of every claim, keyed by claim."""
    sites: dict[str, list[Site]] = {claim.key: [] for claim in claims}
    for claim in claims:
        pattern = re.compile(claim.pattern)
        for path, flat, origin in blocks:
            for match in pattern.finditer(flat):
                values = {
                    group: int(match.group(group).replace(",", "")) for group in claim.groups
                }
                sites[claim.key].append(
                    Site(claim.key, path, origin[match.start()], values)
                )
    return sites


def evaluate(root: Path = ROOT, claims: tuple[Claim, ...] = CLAIMS) -> list[str]:
    """Every disagreement, as one line each. Empty means the documents hold."""
    failures: list[str] = []

    read = {key for claim in CLAIMS for key, _ in claim.groups.values()}
    for key in sorted(set(CONSTANTS) - read):
        failures.append(
            f"constant '{key}' ({CONSTANTS[key].display}) is registered but no claim reads it, "
            "so nothing it backs is compared. Give it a claim or drop it."
        )

    used = {key for claim in claims for key, _ in claim.groups.values()}
    values: dict[str, int | None] = {key: find(root, CONSTANTS[key]) for key in used}
    for key, value in values.items():
        if value is None:
            constant = CONSTANTS[key]
            failures.append(
                f"{constant.path}: no single `{constant.display}`. Every document stating this "
                "bound is checked against it, so a rename, a move or a second declaration of the "
                "same name leaves them unmeasured -- re-point this check at the declaration."
            )

    sites = collect(root, doc_blocks(root), claims)

    for claim in claims:
        found = sites[claim.key]
        if len(found) < claim.floor:
            failures.append(
                f"claim '{claim.key}': matched {len(found)} documents, below the {claim.floor} it "
                "reached when it was written. A claim whose wording stops matching measures less "
                "than it reports -- re-word the pattern, or lower the floor with the deletion that "
                "justifies it."
            )

    # Grouped by constant so a moved constant reports its stale documents once.
    stale: dict[str, list[str]] = {}
    for claim in claims:
        for site in sites[claim.key]:
            for group, (key, offset) in claim.groups.items():
                core = values[key]
                if core is None:
                    continue
                expected = core + offset
                if site.values[group] != expected:
                    stale.setdefault(key, []).append(
                        f"{site.display} says {site.values[group]} ('{claim.key}')"
                    )

    for key, documents in stale.items():
        constant = CONSTANTS[key]
        failures.append(
            f"{constant.path}: {constant.display} is {values[key]}, but "
            f"{len(documents)} document(s) state another number: {'; '.join(sorted(documents))}. "
            "A consumer cannot read the constant, so the documented number is the bound they "
            "act on -- move the documents with it, or move it back."
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description="Documented numeric bounds vs the core constants.")
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--list", action="store_true", help="print every site each claim reached")
    args = parser.parse_args()

    sites = collect(args.root, doc_blocks(args.root))
    for claim in CLAIMS:
        found = sites[claim.key]
        print(f"claim: {claim.key} -> {len(found)} document(s) (floor {claim.floor})")
        if args.list:
            for site in found:
                print(f"    {site.display}  {site.values}")

    failures = evaluate(args.root)
    for line in failures:
        print(line, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
