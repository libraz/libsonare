"""The reference's own recorded noise floor, measured and then removed.

A sampled instrument carries its session's floor under every note. The sampler
gates it -- it begins at note-on, scales with the velocity layer, and fades with
the release -- so it survives every test for being part of the instrument except
one: its spectrum is the same for the lowest note and the highest. One source,
scaled by a gain.

It is also most of the plane. On the corpus this was written against it rises
some 37 dB above its own 14 kHz level down at 30 Hz, which is exactly where a
treble note's entire low band lives. Left in, a cell-by-cell comparison mostly
measures it, and the only answer a physical model has is to hiss.

So it is subtracted from the reference in the power domain, cells it dominates
are dropped rather than compared, and the model is charged only for exceeding
it there. One-sided, because below the floor the reference's true level is
unknown and any value at all is consistent with the capture.

The estimator has to prove itself before it is used. `measure` returns the
across-note agreement it found, and a shape whose notes disagree is not one
source and is refused -- which is what should happen for a reference that has no
recorded floor, a synthesised one or a close capture of a real instrument.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np

#: Band used to read each column's bed gain. High enough that few instruments
#: put a partial there, wide enough to average.
DEFAULT_ANCHOR = (13000.0, 15900.0)
#: Seconds at the start of a render where the sampler's gate has not opened.
ATTACK_S = 0.05
#: dB a cell must stand over the bed before it is treated as instrument.
MARGIN_DB = 3.0
#: Largest across-note spread, in dB, that still reads as a single source.
AGREEMENT_LIMIT_DB = 3.0


@dataclass
class Bed:
    """A frozen bed shape per scale, relative to the anchor band's level."""

    shapes: dict[int, np.ndarray]
    anchor_rows: dict[int, np.ndarray]
    agreement_db: float
    scales: tuple

    @classmethod
    def measure(cls, spectro, ref_signals: dict, anchor=DEFAULT_ANCHOR,
                window=(0.7, 1.7), agree_band=(4000.0, 14000.0)) -> "Bed":
        """Freeze the shape from a set of reference renders.

        Each note is put on its own bed gain, read off the anchor band, and the
        shape is the per-row minimum across notes: at any given frequency some
        note in the set has no partial there, and that note's level is the bed.
        A note that does have a partial on a row can only push that row up, so
        the minimum is the only statistic that survives a corpus of octaves.

        Read while the note is sounding rather than late in it, because the
        sampler gates the floor with the note -- it opens at note-on and fades
        with the release, so measured after the decay it is measured on its way
        out. The window sits after the strike and well before the release.

        Agreement is what licenses the whole idea, and it is measured rather
        than assumed: in a band above where these notes put audible partials,
        how far the second-quietest note stands over the quietest. One source
        under every note puts most of them on top of each other there.
        """
        shapes, anchor_rows, spreads = {}, {}, []
        for s in range(len(spectro.scales)):
            hz = spectro.rows_hz(s)
            rows = np.where((hz >= anchor[0]) & (hz < anchor[1]))[0]
            anchor_rows[s] = rows
            per_note = []
            for sig in ref_signals.values():
                R = spectro(sig)[s]
                c = spectro.columns(s, R.shape[1], *window)
                if not c.any():
                    continue
                col = R[:, c].mean(axis=1)
                per_note.append(col - float(col[rows].mean()))
            if len(per_note) < 3:
                raise ValueError("a bed needs at least three notes to agree")
            stack = np.stack(per_note)
            shapes[s] = stack.min(axis=0)
            band = (hz >= agree_band[0]) & (hz < agree_band[1])
            spreads.append(float(np.median(
                np.percentile(stack[:, band], 25, axis=0) - stack[:, band].min(axis=0))))
        return cls(shapes=shapes, anchor_rows=anchor_rows,
                   agreement_db=float(np.max(spreads)), scales=spectro.scales)

    @property
    def usable(self) -> bool:
        return self.agreement_db <= AGREEMENT_LIMIT_DB

    def grid(self, spectro, R: np.ndarray, scale: int) -> np.ndarray | None:
        """Per-cell bed level for one reference spectrogram, or None if refused.

        The gain is read per time column as a low percentile of the anchor band,
        so a partial that happens to land in the anchor lifts one row and not
        the whole column's estimate.
        """
        if not self.usable:
            return None
        g = np.percentile(R[self.anchor_rows[scale], :], 25, axis=0)
        bed = self.shapes[scale][:, None] + g[None, :]
        bed[:, spectro.times(scale, R.shape[1]) < ATTACK_S] = -400.0
        return bed

    def clean(self, spectro, R: np.ndarray, scale: int) -> tuple[np.ndarray, np.ndarray]:
        """(reference with the floor taken off, the floor itself), both in dB.

        Power-domain subtraction: what is left is the instrument, and where
        nothing is left the capture never held an instrument level to compare
        against.
        """
        bed = self.grid(spectro, R, scale)
        if bed is None:
            return R, np.full_like(R, -400.0)
        resid = 10.0 ** (R / 10.0) - 10.0 ** (bed / 10.0)
        return 10 * np.log10(np.maximum(resid, 1e-30)), bed

    def save(self, path: Path | str) -> None:
        np.savez(path, agreement=self.agreement_db,
                 **{f"shape{s}": v for s, v in self.shapes.items()},
                 **{f"anchor{s}": v for s, v in self.anchor_rows.items()})

    @classmethod
    def load(cls, path: Path | str, scales) -> "Bed":
        z = np.load(path)
        n = len(scales)
        return cls(shapes={s: z[f"shape{s}"] for s in range(n)},
                   anchor_rows={s: z[f"anchor{s}"] for s in range(n)},
                   agreement_db=float(z["agreement"]), scales=scales)
