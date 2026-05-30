"""Name normalization utilities.

Every surface names the same logical function differently:

* C:      ``sonare_mastering_repair_declick``  (snake, ``sonare_`` prefix)
* Python: ``mastering_repair_declick``         (snake, already the key)
* Node:   ``masteringRepairDeclick``           (camelCase)
* WASM:   ``masteringRepairDeclick``            (camelCase)
* CLI:    ``mastering-repair-declick`` / ``declick`` (kebab / short)

The CANONICAL KEY is the C name with the ``sonare_`` prefix stripped, in
snake_case. These helpers convert any surface name to that key and convert
parameter names to a common snake_case form for positional diffing.
"""

from __future__ import annotations

import re

_CAMEL_BOUNDARY = re.compile(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])")
_NUM_BOUNDARY = re.compile(r"(?<=[A-Za-z])(?=[0-9])")


def camel_to_snake(name: str) -> str:
    """``masteringRepairDeclick`` / ``nFft`` -> ``mastering_repair_declick`` / ``n_fft``."""
    s = _CAMEL_BOUNDARY.sub("_", name)
    s = _NUM_BOUNDARY.sub("_", s)
    return s.lower()


def kebab_to_snake(name: str) -> str:
    """``mastering-repair-declick`` -> ``mastering_repair_declick``."""
    return name.replace("-", "_").lower()


def snake_to_camel(name: str) -> str:
    """``mastering_repair_declick`` -> ``masteringRepairDeclick``."""
    parts = name.split("_")
    return parts[0] + "".join(p.title() for p in parts[1:])


def strip_sonare_prefix(c_name: str) -> str:
    """``sonare_detect_bpm`` -> ``detect_bpm``."""
    if c_name.startswith("sonare_"):
        return c_name[len("sonare_") :]
    return c_name


def canonical_key(name: str, surface: str) -> str:
    """Map a surface symbol name to the canonical snake_case key."""
    if surface == "c":
        return strip_sonare_prefix(name)
    if surface == "python":
        return name.lower()
    if surface in ("node", "wasm"):
        return camel_to_snake(name)
    if surface == "cli":
        return kebab_to_snake(name)
    return name.lower()


def normalize_param_name(name: str) -> str:
    """Normalize a parameter name from any surface to snake_case for diffing."""
    if "-" in name:
        return kebab_to_snake(name)
    if any(c.isupper() for c in name):
        return camel_to_snake(name)
    return name.lower()


# Enum members whose integer value is 0 across the C ABI and every facade. A
# facade may spell a zero-valued default either as the bare ``0`` (Node/WASM) or
# as the named enum member (Python ``PitchClass.C`` / ``Mode.MAJOR`` / ...). They
# denote the same wire value, so we fold the named member to its integer.
_ENUM_MEMBER_INT = {
    "PitchClass.C": "0",
    "Mode.MAJOR": "0",
    "Mode.MINOR": "1",
    "ChromaMethod.STFT": "0",
    "ChromaMethod.NNLS": "1",
}


def normalize_default(value: str | None) -> str | None:
    """Normalize a default literal so cross-surface comparison is meaningful.

    Folds Python/JS/C++ boolean spellings together, drops trailing ``.0`` on
    floats, strips quotes around string defaults, and maps zero-valued enum
    members (``PitchClass.C``) to their integer wire value.
    """
    if value is None:
        return None
    v = value.strip()
    if v in _ENUM_MEMBER_INT:
        return _ENUM_MEMBER_INT[v]
    low = v.lower()
    if low in ("true", "false"):
        return low
    if low in ("none", "null", "undefined", "nullptr"):
        return "none"
    # String literal: 'stft' / "stft" -> stft
    if len(v) >= 2 and v[0] in "\"'" and v[-1] == v[0]:
        return v[1:-1]
    # Numeric: normalize float spelling. 22050.0 -> 22050, 0.40 -> 0.4
    num = v.rstrip("fFlL")
    try:
        f = float(num)
        if f == int(f):
            return str(int(f))
        return repr(f)
    except ValueError:
        pass
    return v
