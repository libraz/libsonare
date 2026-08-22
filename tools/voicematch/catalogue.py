"""What the library reports about its own knob space.

A `-DBUILD_TUNING=ON` build writes every knob it consulted during a render to
the path in `SONARE_TUNING_DUMP`: the engine calibration constants, every
patch's fields, the program-to-patch map, and the interval `clamp_synth_patch`
accepts for each field. Reading that back is how the fitter learns what exists.
A parse of the sources could not see a clamp bound at all, and would drift from
them besides — the catalogue is produced by the same code the render uses.

`scan_tunables` goes the other way and exists only for the write-back: it finds
the `SONARE_TUNABLE` declaration whose literal a fitted value has to replace.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from _repo import REPO_ROOT

HERE = Path(__file__).resolve().parent

TUNABLE_DEF = re.compile(
    r"SONARE_TUNABLE\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(-?[0-9.]+(?:[eE][-+]?[0-9]+)?)f?\s*\)"
)


@dataclass
class TunableDef:
    """Where a `SONARE_TUNABLE` is declared and what its compiled-in default is."""

    file: Path
    value: float
    span_start: int  # offsets of the literal within the file text
    span_end: int


def scan_tunables() -> dict[str, TunableDef]:
    """Map every `SONARE_TUNABLE` to its declaration, keyed `<file stem>.<name>`.

    The scope prefix is what the library itself derives from `__FILE__`, and it
    is what makes the keys unique: `kBreathBase` is declared by four different
    voices, each in its own anonymous namespace, while the override table has
    one flat key space.

    This scan exists for the write-back, not for validation — it is how a
    fitted value finds the literal it has to replace in the source. The knob
    catalogue (`--dump-knobs`) is the authority on what exists, because it also
    covers the per-program patch fields, which are not `SONARE_TUNABLE`
    declarations at all.
    """
    found: dict[str, TunableDef] = {}
    for path in sorted((REPO_ROOT / "src").rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp"):
            continue
        for m in TUNABLE_DEF.finditer(path.read_text()):
            key = f"{path.stem}.{m.group(1)}"
            if key in found:
                raise ValueError(
                    f"{key} is declared twice in {path.relative_to(REPO_ROOT)}; "
                    f"the override table is keyed by scope and name, so the two "
                    f"would move together"
                )
            found[key] = TunableDef(path, float(m.group(2)), m.start(2), m.end(2))
    return found


def resolve_knob_name(name: str, tunables: dict[str, TunableDef]) -> str:
    """Accept either a fully scoped key or an unambiguous bare constant name."""
    if name in tunables or "." in name:
        return name
    matches = [k for k in tunables if k.rsplit(".", 1)[1] == name]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        return name
    raise ValueError(
        f"knob name {name!r} is declared by {len(matches)} files "
        f"({', '.join(sorted(matches))}); use the scoped form"
    )


@dataclass
class Catalogue:
    """What the library reports about its own knob space.

    `defaults` maps every key it consulted to the value it would have used;
    `programs` maps a GM program to the patch that voices it; `bounds` maps a
    patch field *path* (no patch prefix — a bound belongs to the field) to the
    interval `clamp_synth_patch` accepts.
    """

    defaults: dict[str, float]
    programs: dict[int, str]
    bounds: dict[str, tuple[float, float]]

    def bound_for(self, key: str) -> tuple[float, float] | None:
        """The admissible range of the field a patch key addresses, if bounded."""
        _, _, path = key.partition(".")
        return self.bounds.get(path)


def dump_catalogue(
    program: int, pattern: str, lib_path: str | None, *, sr: int, notes: str = ""
) -> Catalogue:
    """Render once with `SONARE_TUNING_DUMP` and read back the whole knob space.

    The library reports what it actually consulted, so this covers the engine
    calibration constants *and* every program's patch fields, with the exact
    defaults and the exact clamp bounds, and it cannot drift from the source the
    way a parse would.
    """
    with tempfile.TemporaryDirectory() as tmp:
        dump = Path(tmp) / "knobs.tsv"
        env = dict(os.environ)
        env["SONARE_TUNING_DUMP"] = str(dump)
        env.pop("SONARE_TUNING_OVERRIDES", None)
        if lib_path:
            env["SONARE_LIB_PATH"] = lib_path
        child = (
            "import sys; sys.path.insert(0, %r)\n"
            "from patterns import build_pattern, pattern_length\n"
            "from smf import write_smf\n"
            "import render_model\n"
            "kw = {'notes': %r} if %r else {}\n"
            "pat = build_pattern(%r, %d, **kw)\n"
            "render_model.render_model(write_smf("
            "pat.notes, program=%d, channel=pat.channel, end_pad=pat.tail), "
            "pattern_length(pat), %d)\n"
        ) % (
            str(HERE),
            tuple(int(n) for n in notes.split(",")) if notes else (),
            notes, pattern, program, program, sr,
        )
        proc = subprocess.run([sys.executable, "-c", child], env=env,
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"knob dump render failed:\n{proc.stderr[-2000:]}")
        if not dump.exists():
            raise RuntimeError(
                "the render produced no knob dump — the library was built without "
                "BUILD_TUNING, so SONARE_TUNING_DUMP was ignored"
            )
        defaults: dict[str, float] = {}
        programs: dict[int, str] = {}
        bounds: dict[str, tuple[float, float]] = {}
        for line in dump.read_text().splitlines():
            parts = line.split("\t")
            if parts[0] == "#program" and len(parts) == 3:
                programs[int(parts[1])] = parts[2]
            elif parts[0] == "#bound" and len(parts) == 4:
                bounds[parts[1]] = (float(parts[2]), float(parts[3]))
            elif len(parts) == 2:
                defaults[parts[0]] = float(parts[1])
    return Catalogue(defaults, programs, bounds)


# A drum note's patch is keyed by note number rather than by a name, because a
# drum note is not a GM program: the program selects the kit and the note
# selects the instrument, so the program map has no entry for it. The formatter
# and the matcher are kept together — they are the two directions of one
# spelling, and a fit writes back through both of them.
DRUM_PATCH_KEY = re.compile(r"^d(\d{3})$")


def drum_patch_key(note: int) -> str:
    """The catalogue prefix of a drum note's patch (`d038` for note 38)."""
    return f"d{note:03d}"
