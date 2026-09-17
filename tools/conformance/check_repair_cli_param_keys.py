#!/usr/bin/env python3
"""Check the hand-written key set the Python `repair` CLI subcommand carries.

`bindings/python/src/libsonare/_cli_mastering.py` hardcodes
`_NON_REPAIR_CHAIN_DISABLE_OVERRIDES` -- every top-level chain module's own
``.enabled`` flag *other than* ``repair.*``, forced off so `repair --preset`
cannot master -- because no C ABI entry exposes a chain config's field list as
data. A module added to the chain without a matching entry here would silently
start mastering through `repair`.

It is derived here from the actual source of truth,
`src/mastering/api/chain_json.cpp`, and compared against the hardcoded set so
drift fails a check instead of shipping silently.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHAIN_JSON_PATH = ROOT / "src/mastering/api/chain_json.cpp"
CLI_MASTERING_PATH = ROOT / "bindings/python/src/libsonare/_cli_mastering.py"

# One `add_field(params, "dotted.key", <cpp expression>);` call, possibly
# wrapped across two lines (the printed form breaks long calls after the key).
_ADD_FIELD_RE = re.compile(r'add_field\(\s*params\s*,\s*"([\w.]+)"\s*,\s*(.*?)\)\s*;', re.DOTALL)
_PLAIN_MEMBER_RE = re.compile(r"^cfg\.repair\.(\w+)\.config\.(\w+)$")


def _extract_python_string_set(source: str, name: str) -> set[str]:
    """Pull every quoted string literal out of the ``name = { ... }`` block.

    Reads the constant as source text rather than importing the module, so
    this check needs no built dylib -- the same approach
    `check_public_contracts.py` uses for the sibling cross-language contracts.
    """
    match = re.search(
        rf"^{re.escape(name)}\b.*?\{{(.*?)\n\s*\}}\)?", source, re.DOTALL | re.MULTILINE
    )
    if match is None:
        raise ValueError(f"{name}: declaration not found in {CLI_MASTERING_PATH}")
    return set(re.findall(r'"([\w.]+)"', match.group(1)))


def main() -> int:
    chain_json_text = CHAIN_JSON_PATH.read_text(encoding="utf-8")
    cli_source = CLI_MASTERING_PATH.read_text(encoding="utf-8")
    errors: list[str] = []

    all_enabled_keys = {
        key for key, _expr in _ADD_FIELD_RE.findall(chain_json_text) if key.endswith(".enabled")
    }
    expected_non_repair_enabled = {key for key in all_enabled_keys if not key.startswith("repair.")}
    actual_non_repair_overrides = _extract_python_string_set(
        cli_source, "_NON_REPAIR_CHAIN_DISABLE_OVERRIDES"
    )
    missing_overrides = expected_non_repair_enabled - actual_non_repair_overrides
    extra_overrides = actual_non_repair_overrides - expected_non_repair_enabled
    if missing_overrides:
        errors.append(
            "_NON_REPAIR_CHAIN_DISABLE_OVERRIDES is missing chain module(s) that "
            f"build_chain_params() emits: {sorted(missing_overrides)}"
        )
    if extra_overrides:
        errors.append(
            "_NON_REPAIR_CHAIN_DISABLE_OVERRIDES names key(s) build_chain_params() "
            f"no longer emits: {sorted(extra_overrides)}"
        )

    if errors:
        print("repair CLI param key conformance failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(
        "repair CLI param key conformance: OK "
        f"({len(all_enabled_keys)} .enabled keys scanned, "
        f"{len(expected_non_repair_enabled)} non-repair)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
