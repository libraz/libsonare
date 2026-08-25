"""Spectrogram-shape comparison: the picture as a number a search can minimise.

`loss.py` next door reduces each render to about a dozen scalar summaries per
note. An average over a region cannot see a partial that is absent, a decay
whose curvature is wrong rather than whose rate is wrong, or a transient with
the wrong spectrum, and an instrument is mostly those three. This package
compares the two log-frequency spectrograms cell by cell instead, which is what
a person does with the two pictures side by side, without the person.

It is the same corpus, the same renderer and the same tuning-override mechanism
as the rest of the harness -- only the comparison differs. Nothing here is
piano-specific: the note grid, the velocities and the gate come from the capture
manifest, and the inharmonicity of each note is fitted from the reference rather
than assumed, so a harpsichord or an organ rank is measured on its own terms.

The modules, in dependency order:

    spectro   log-frequency spectrogram, two resolutions
    render    reference and model signals, cached, one subprocess per override
    bed       the reference's own recorded noise floor, measured and removed
    partials  inharmonic partial tracking and its bed-clearance guard
    terms     the five comparisons: spectrum, onset, residue, invariance, release
    loss      the terms combined into one number, gain-aligned
    probes    diagnostics that answer a question rather than drive a search
    density   how many things are ringing, and how diffuse the field is
    purity    how much of a render is the played string and how much is not
    takes     phrase material -- chords, pedal, repeats -- against a reference
    reach     which residuals any parameter can move, and which none can
    search    coordinate descent, single-move ablation, and pruning

The last four exist because the loss is blind to what they measure, and each of
them was written after the ear reported something the score could not. The loss
counts how MUCH energy sits off the string's partials; `purity` and `density`
between them say what KIND it is, and a bank of fixed resonances and a diffuse
body register identically on the first and oppositely on the second. `takes`
covers the material the note corpus does not contain at all -- everything an
instrument does when more than one string is moving.

One rule cuts across all of them and has produced a wrong answer in every module
that lacked it: a recorded reference stops decaying and sits on its session
floor, and a floor is noise, and noise reads as the dense, diffuse, richly
non-harmonic thing a model is being asked to match. Every comparison against a
reference gates on that floor -- `bed`, `density.band_snr_db`,
`purity.floor_share`, `takes.band_error`.
"""

from .bed import Bed  # noqa: F401
from .loss import ShapeLoss, Terms  # noqa: F401
from .render import Signals  # noqa: F401
from .spectro import Spectro  # noqa: F401
