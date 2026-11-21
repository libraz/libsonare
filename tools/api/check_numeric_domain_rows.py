#!/usr/bin/env python3
"""Fail unless every named NumericDomain constant under src/ appears in a row partition.

A constant only a binding reads is allowed through _UNROWED, with the reason beside it.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROWS = sorted((ROOT / "src" / "c_api").glob("numeric_domain_rows*.h"))
EXCLUDED = ROOT / "src" / "util" / "parameter_checks.h"

# constant name -> why no C entry refuses with it.
_UNROWED: dict[str, str] = {
    "kDecomposeStemsHopLengthDomain": "the C entries check the hop against kStftHopDomain and map the auto hop to 0 first",
    "kEngineBandIndexFloorDomain": "read only in a build without mixing; the same subjects read kEngineBandIndexDomain otherwise",
    "kEngineInsertIndexDomain": "the insert-count relation refuses first; inserts are capped below the domain's bound",
    "kEngineMidiCc14NumberDomain": "binding.cc_number's row is kEngineMidiControllerDomain; this bound is its below_32_when_14_bit relation",
    "kF0TrackHopLengthDomain": "every C caller refuses or derives the hop under its own subject before the F0 track is checked",
    "kMelFrequencyCountDomain": "mel_frequencies has no C entry",
    "kMultibandDynamicBandCountDomain": "the band-list builders stop at DynamicEq::kMaxBands, so no C entry reaches the bound",
    "kSilenceIndexableLengthDomain": "refuses only a buffer longer than INT_MAX samples",
    "kSilenceSignalCountDomain": "split_silence_common_ex refuses a signal_count below 1 with its own at-least check first",
    "kStftWinLengthDomain": "polyphonic_analyze, the one C entry taking win_length, checks kColaWinLengthDomain first",
    "kVoiceChangerBlockSizeDomain": "voice_change_realtime passes a constant block size",
    "kWaveletSampleRateDomain": "wavelet_lengths has no C entry",
}

DECLARATION = re.compile(r"inline\s+constexpr\s+(?:[\w:]+::)?NumericDomain\s+(k\w+Domain)\b")


def main() -> int:
    rows_text = "\n".join(path.read_text() for path in ROWS)
    declared: dict[str, Path] = {}
    for path in sorted((ROOT / "src").rglob("*.h")):
        if path == EXCLUDED:
            continue
        for name in DECLARATION.findall(path.read_text()):
            declared[name] = path
    print(f"numeric-domain rows: scanned {len(declared)} named domain constants")
    if not declared:
        print("check_numeric_domain_rows: no constants found; the scan is broken", file=sys.stderr)
        return 1
    missing = [
        f"{name} ({path.relative_to(ROOT)})"
        for name, path in sorted(declared.items())
        if name not in _UNROWED and not re.search(rf"\b{name}\b", rows_text)
    ]
    for name in _UNROWED:
        if name not in declared:
            missing.append(f"{name} is listed in _UNROWED but no longer declared")
    if missing:
        print("named domains with no row in src/c_api/numeric_domain_rows*.h:", file=sys.stderr)
        for line in missing:
            print(f"  {line}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
