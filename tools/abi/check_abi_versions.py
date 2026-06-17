#!/usr/bin/env python3
"""Verify the ABI-version mirrors agree across the C core and every binding.

The C ABI carries several *independent* version counters, not one:

    SONARE_FEATURE_ABI_VERSION         (include/sonare/sonare_c_types.h)
    SONARE_PROJECT_ABI_VERSION         (include/sonare/sonare_c_project.h)
    SONARE_VOICE_CHANGER_ABI_VERSION   (include/sonare/sonare_c_effects.h)
    SONARE_ACOUSTIC_ABI_VERSION        (include/sonare/sonare_c_acoustic.h)
    kEngineAbiVersion                  (src/rt/command.h)

``SONARE_ABI_VERSION`` (include/sonare/sonare_c.h) packs feature/project/voice-changer/
acoustic into one ``uint32_t`` (bytes 0..3). Each binding hard-codes mirror
constants that MUST equal the C source of truth; a stale literal means a binding
silently accepts an incompatible native library. Bindings that *derive* their
constant from the C macro (e.g. the Node addon's
``kExpectedProjectAbiVersion = SONARE_PROJECT_ABI_VERSION``) cannot drift and are
not checked here -- only the hand-written literals are.

This mechanises the "ABI lives in N mirrors that must match" discipline. It is
stdlib-only, read-only, and exits non-zero on any mismatch. Per-subsystem
counters live only in C today; bindings mirror the *aggregate* and the project /
engine counters. Add a new family to ``MIRRORS`` deliberately when a binding
grows a new literal -- an unmodelled mirror is simply unchecked, never a silent
pass for the ones listed.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def _read(rel: str) -> str:
    return (REPO_ROOT / rel).read_text()


def _find_int(text: str, pattern: str, source: str) -> int:
    """Extract a single integer literal (decimal or 0x hex) captured by ``pattern``."""
    match = re.search(pattern, text)
    if match is None:
        raise SystemExit(f"could not locate ABI constant in {source}: /{pattern}/")
    raw = match.group(1).rstrip("uU")
    return int(raw, 0)


def c_source_of_truth() -> dict[str, int]:
    """The authoritative per-subsystem versions and the packed aggregate."""
    feature = _find_int(
        _read("include/sonare/sonare_c_types.h"),
        r"#define\s+SONARE_FEATURE_ABI_VERSION\s+(\w+)",
        "sonare_c_types.h",
    )
    project = _find_int(
        _read("include/sonare/sonare_c_project.h"),
        r"#define\s+SONARE_PROJECT_ABI_VERSION\s+(\w+)",
        "sonare_c_project.h",
    )
    voice_changer = _find_int(
        _read("include/sonare/sonare_c_effects.h"),
        r"#define\s+SONARE_VOICE_CHANGER_ABI_VERSION\s+(\w+)",
        "sonare_c_effects.h",
    )
    acoustic = _find_int(
        _read("include/sonare/sonare_c_acoustic.h"),
        r"#define\s+SONARE_ACOUSTIC_ABI_VERSION\s+(\w+)",
        "sonare_c_acoustic.h",
    )
    engine = _find_int(
        _read("src/rt/command.h"),
        r"kEngineAbiVersion\s*=\s*(\w+)",
        "rt/command.h",
    )
    # Mirrors include/sonare/sonare_c.h SONARE_ABI_VERSION packing.
    aggregate = (
        (feature & 0xFF)
        | ((project & 0xFF) << 8)
        | ((voice_changer & 0xFF) << 16)
        | ((acoustic & 0xFF) << 24)
    )
    return {
        "feature": feature,
        "project": project,
        "voice_changer": voice_changer,
        "acoustic": acoustic,
        "engine": engine,
        "aggregate": aggregate,
    }


# Hand-written binding literals that must equal a C source-of-truth counter.
# (file, regex capturing the literal, source-of-truth key)
MIRRORS: tuple[tuple[str, str, str], ...] = (
    # Aggregate packed version.
    ("bindings/python/src/libsonare/_ffi.py", r"EXPECTED_ABI_VERSION\s*=\s*(\w+)", "aggregate"),
    # Project ABI.
    (
        "bindings/python/src/libsonare/_project.py",
        r"EXPECTED_PROJECT_ABI_VERSION\s*=\s*(\w+)",
        "project",
    ),
    ("bindings/node/src/types.ts", r"EXPECTED_PROJECT_ABI_VERSION\s*=\s*(\w+)", "project"),
    ("bindings/wasm/src/project.ts", r"EXPECTED_PROJECT_ABI_VERSION\s*=\s*(\w+)", "project"),
    # Engine ABI.
    (
        "bindings/python/src/libsonare/engine.py",
        r"EXPECTED_ENGINE_ABI_VERSION\s*=\s*(\w+)",
        "engine",
    ),
    ("bindings/node/src/addon/engine/common.h", r"kExpectedEngineAbiVersion\s*=\s*(\w+)", "engine"),
    ("bindings/wasm/src/realtime_engine.ts", r"EXPECTED_ENGINE_ABI_VERSION\s*=\s*(\w+)", "engine"),
)


def main() -> int:
    truth = c_source_of_truth()
    mismatches: list[str] = []
    checked = 0
    for rel, pattern, key in MIRRORS:
        actual = _find_int(_read(rel), pattern, rel)
        expected = truth[key]
        checked += 1
        if actual != expected:
            mismatches.append(
                f"  {rel}: {key} mirror is {actual} (0x{actual:X}), "
                f"C source of truth is {expected} (0x{expected:X})"
            )

    if mismatches:
        sys.stderr.write("ABI-version mirror mismatch:\n")
        sys.stderr.write("\n".join(mismatches) + "\n")
        return 1

    print(
        f"ABI-version mirrors consistent ({checked} mirrors checked): "
        f"feature={truth['feature']} project={truth['project']} "
        f"voice_changer={truth['voice_changer']} acoustic={truth['acoustic']} "
        f"engine={truth['engine']} aggregate=0x{truth['aggregate']:08X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
