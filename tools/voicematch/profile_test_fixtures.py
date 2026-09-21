"""The committed record a profile test reads, and the rows it builds.

A profile is measured once and compared against for months, so the failures
worth a test are the ones that produce a plausible number from a wrong premise:
a program nobody chose, a noise measurement whose window is narrower than an FFT
bin, a dynamic range read off a single velocity. That is what the `test_profile_*`
modules are for, and this is what they construct their inputs from.

`shipped_captures` globs rather than listing, so a capture added without
being added to a list is still covered; `assert_names_no_product` takes its
directories as arguments so a test can point it at a file that DOES name a
product and show that it fails.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))



SR = 48000


CAPTURE_DIR = Path(__file__).resolve().parent / "capture"


REFERENCE_DIR = Path(__file__).resolve().parent / "reference"


def shipped_captures(root: Path | None = None) -> list[str]:
    """Every committed capture definition, by id.

    Globbed rather than listed. A hand-kept list of instrument names is the
    mirror table these tests exist to make unnecessary: the failure worth
    catching is a capture added without being added to the list, which a list
    cannot catch by construction. The `.local.json` overlays are the untracked
    identity half and are excluded by suffix.
    """
    here = root or CAPTURE_DIR
    return sorted(p.stem for p in here.glob("*.json") if not p.name.endswith(".local.json"))


def assert_names_no_product(name: str, *, capture_dir: Path, reference_dir: Path) -> None:
    """Neither half of one instrument's committed record identifies a product.

    Keys rather than the words: the prose in these files explains that the
    plugin triple and the presets live in the untracked overlay, and it should.
    What must not appear is a value - `profile.py measure` copies the capture
    block into the reference, so a field added on one side reaches the other.

    Taking its directories as arguments is what lets the test below point it at
    a file that does name a product, and so show that it fails.
    """
    docs = [(json.loads((capture_dir / f"{name}.json").read_text()), "capture")]
    reference = reference_dir / f"{name}.json"
    if reference.exists():
        docs.append((json.loads(reference.read_text()).get("capture", {}), "reference"))
    for doc, where in docs:
        assert "plugin" not in doc, f"{name} {where} names its plugin"
        for timbre in doc.get("timbres", []):
            assert "preset" not in timbre, f"{name} {where} names a preset"


def _hit_row(**over):
    row = {"bands_db": [0.0] * 25, "band_decay_db_s": [-10.0] * 8, "attack_ms": 2.0,
           "crest_db": 12.0, "centroid_hz": 1000.0, "decay_ms": 200.0,
           "decay_capped": False, "flatness_db": -20.0, "stereo_width": 0.4,
           "peak_dbfs": -6.0}
    row.update(over)
    return row
