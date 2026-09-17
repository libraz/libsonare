"""Polyphonic note editing: the analysis as a handle.

The monophonic door (:func:`libsonare.extract_notes` / :func:`libsonare.render_notes`)
passes everything by value because a note was only ever measured against the caller's
own audio and F0 track. A polyphonic analysis holds a complex spectrogram and, per
note, the complex weight of every bin it claimed, so it stays on the C side and the
caller holds a handle: :class:`PolyphonicAnalysis`. What crosses is what a host acts
on -- the notes, each note's pending edit, the per-frame voice count, and per note a
pitch, a level and a salience curve -- and re-rendering an edit therefore costs no
second analysis.
"""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import TYPE_CHECKING, Self

import numpy as np

from ._effects_note_model import (
    _NOTE_STRUCT_VERSION,
    _note_set_index,
)
from ._runtime import _unsupported_effect_symbol

if TYPE_CHECKING:
    # analyzer.pyi declares the two note dataclasses itself rather than
    # re-exporting the implementation's, and those declarations are what a
    # consumer reaches as libsonare.NoteEdit / libsonare.NoteObject. Annotating
    # against them is what lets a caller pass back the class it was handed; the
    # two names are the same objects at runtime.
    from .analyzer import NoteEdit, NoteObject
else:
    from ._effects_note_model import NoteEdit, NoteObject
from ._ffi import (
    SonareNoteEdit,
    SonareNoteObject,
    SonareNoteRenderConfig,
    SonarePolyphonicConfig,
)
from ._runtime import (
    SonareValueError,
    _check,
    _from_c_float_array,
    _from_c_int_array,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _validate_c_int_field,
    _validate_samples,
)

# The polyphonic config is at layout version 1; 0 selects the same layout.
_POLYPHONIC_STRUCT_VERSION = 1


def _count_field(name: str, value: int | None) -> int:
    """0 is the C sentinel for "take the default" on every count field.

    Narrowed rather than coerced for that reason: int(0.5) is the sentinel, so a
    fractional count would run at the default and report success.
    """
    return 0 if value is None else _validate_c_int_field("PolyphonicAnalysis", value, name)


def _value_field(value: float | None) -> float:
    """Same sentinel for the float fields, which 0 never means literally."""
    return 0.0 if value is None else float(value)


class PolyphonicAnalysis:
    """One polyphonic analysis, editable note by note and renderable back to audio.

    Built by :meth:`analyze` (or the constructor, which is the same call), which
    runs one pass: one STFT, the multi-F0 extraction over it, a claim set per
    tracked ridge, the apportionment of the bins two notes stand on, and the
    measured fields of each note. Every note starts with the identity
    :class:`libsonare.NoteEdit`, so :meth:`render` on a fresh analysis reproduces
    the analysis's own round trip -- not the source bit for bit, the STFT round
    trip's error being neither added to nor removed by the edit chain.

    The analysis owns native memory and is released deterministically: use it as
    a context manager, or call :meth:`close` when done. ``__del__`` releases it
    too, but only as a backstop -- it runs at whatever point the interpreter
    collects the object, which on a long analysis is a lot of held memory.

    An analysis finding no notes is not an error. Silence, or material the
    register of the framing cannot resolve, tracks no ridge; rendering that is
    the residual alone, which is the whole round trip.

    Example:
        >>> with libsonare.PolyphonicAnalysis.analyze(chord, sr) as analysis:
        ...     notes = analysis.notes()
        ...     edit = notes[1].edit
        ...     edit.gain_db = -6.0
        ...     analysis.set_note_edit(1, edit)
        ...     quieter = analysis.render()
    """

    def __init__(
        self,
        samples: Sequence[float] | list[float] | np.ndarray,
        sample_rate: int,
        *,
        n_fft: int | None = None,
        hop_length: int | None = None,
        win_length: int | None = None,
        cent_ref_hz: float | None = None,
        cents_per_bin: float | None = None,
        cent_max_hz: float | None = None,
        tonality_off: bool = False,
        salience_harmonics: int | None = None,
        f0_min_hz: float | None = None,
        f0_max_hz: float | None = None,
        salience_alpha_hz: float | None = None,
        salience_beta_hz: float | None = None,
        salience_inharmonicity: float | None = None,
        max_polyphony: int | None = None,
        min_frame_peak_ratio: float | None = None,
        min_separation_cents: float | None = None,
        subtraction_factor: float | None = None,
        max_jump_cents: float | None = None,
        min_ridge_peak_ratio: float | None = None,
        min_ridge_duration_ms: float | None = None,
        mask_harmonics: int | None = None,
        claim_lobes: float | None = None,
        inharmonicity: float | None = None,
        estimate_inharmonicity: bool = False,
        inharmonicity_min_partials: int | None = None,
        inharmonicity_max_residual_bins: float | None = None,
        inharmonicity_max_stretch: float | None = None,
        window_frames: int | None = None,
        min_partial_separation: float | None = None,
        max_fit_residual: float | None = None,
        max_weight_modulus: float | None = None,
        max_refine_hz: float | None = None,
        f0_tolerance_cents: float | None = None,
        segmentation_threshold_cents: float | None = None,
        min_note_ms: float | None = None,
        reference_hz: float | None = None,
    ) -> None:
        """Analyse ``samples`` into editable notes.

        Every keyword is optional, and ``None`` keeps the library default stated
        for it. The window function and the centred framing are not settable:
        every span, claim and mask offset in this chain is derived against one
        framing, so a second way to state it would be a second thing to keep in
        agreement.

        Args:
            samples: Source audio (any sequence convertible to float32). Must be
                non-empty and finite.
            sample_rate: Sample rate in Hz.
            n_fft: STFT size for the whole chain -- the extraction, the claims,
                the apportionment and the render (default 4096, the size this
                chain is tuned at).
            hop_length: STFT hop (default 512).
            win_length: Window length (default ``n_fft``).
            cent_ref_hz: Bottom of the cent axis the salience is folded onto
                (default 55).
            cents_per_bin: Resolution of that axis (default 100/3). Finer than
                1 cent is rejected.
            cent_max_hz: Top of that axis (default 8000).
            tonality_off: ``True`` stops weighting bins by tonality, which is on.
            salience_harmonics: Partials summed per F0 candidate (default 20, at
                most 128).
            f0_min_hz: Lowest F0 a candidate may take (default 55).
            f0_max_hz: Highest (default 1760).
            salience_alpha_hz: Harmonic weighting offset (default 27).
            salience_beta_hz: Harmonic weighting scale (default 320).
            salience_inharmonicity: Stretch assumed while scoring a candidate.
                0 is both the default and a legal value, and needs no sentinel:
                it is passed through as given.
            max_polyphony: Voices a frame is allowed (default 4, at most 64).
            min_frame_peak_ratio: Share of the frame's peak below which the
                estimation stops iterating (default 0.20). 0 is a legal value
                here as well as the sentinel for the default, so **pass a
                negative number to select 0**; any other negative is rejected.
            min_separation_cents: Closest two candidates in one frame may sit
                (default 50). Negative selects 0, as above.
            subtraction_factor: Share of a found voice removed before the next
                iteration (default 1).
            max_jump_cents: A larger jump between frames breaks the ridge
                (default 50).
            min_ridge_peak_ratio: A fade below this share of the ridge's own peak
                breaks it (default 0.10). Negative selects 0, as above.
            min_ridge_duration_ms: Shorter ridges are dropped (default 140).
                Negative selects 0, as above.
            mask_harmonics: Partials claimed per note (default 20, at most 128).
                A claim is predicted from the note's F0 and harmonic number and
                never read from the spectrum, so a note claims bins its own
                partials never reached.
            claim_lobes: Claim half-width in Hann main lobes (default 1).
            inharmonicity: Stretch of the partial series, ``B`` in
                ``f_h = h*f0*sqrt(1 + B*h^2)``. 0 is both the default and a legal
                value, passed through as given. Leaving it at 0 for stretched
                material costs more than a widened claim would: at a piano's
                ``1e-4`` the highest partial of a twenty-harmonic claim sits
                outside the claim entirely, and a partial outside every claim is
                residual, which is carried unedited and so keeps sounding at the
                old pitch after its note is moved.
            estimate_inharmonicity: ``True`` fits a stretch per note from the
                spectrum instead of spending ``inharmonicity`` on every one of
                them. Off by default because of what it reaches rather than what
                it costs: at this framing the fit takes an isolated note in the
                middle register and refuses a chord. A refused note keeps the
                declared stretch, so the fit only ever replaces a guess with a
                measurement --
                :meth:`note_inharmonicity` reports which notes it reached.
            inharmonicity_min_partials: Usable partials a fit needs (default 3).
            inharmonicity_max_residual_bins: Largest per-partial misfit a fit may
                keep (default 0.5).
            inharmonicity_max_stretch: A fit above this is refused (default
                0.03125).
            window_frames: Frames per apportionment fit (default 8, between 4
                and 64).
            min_partial_separation: Radians per frame two partials must differ by
                (default 0.01).
            max_fit_residual: Relative misfit ceiling above which the fit refuses
                a shared bin rather than guessing; a refused bin keeps the equal
                share (default 0.02).
            max_weight_modulus: Ceiling on one fitted weight's modulus (default
                8). Where two partials nearly cancel a weight exceeds one and the
                residual carries several times the input there; while every edit
                is identity that is inaudible, since the notes and the residual
                still sum to the input. Lowering it trades separation for a
                quieter residual.
            max_refine_hz: Highest partial usable to refine an F0; 0 derives one,
                and is passed through as given.
            f0_tolerance_cents: Worst F0 error tolerated (default 50).
            segmentation_threshold_cents: Pitch jump that cuts a new note out of
                a ridge (default 50).
            min_note_ms: Shortest note kept (default 30).
            reference_hz: Reference for each note's ``median_cents`` (default
                A4 = 440).

        Raises:
            SonareValueError: If ``samples`` is empty, non-finite, or not a
                one-dimensional numeric buffer.
            SonareError: If the C call rejects the request, including
                ``ErrorCode.NOT_SUPPORTED`` when the library was built without
                the pitch editor.
        """
        # Set first so a failed analysis leaves a valid attribute for
        # __del__ / close() instead of raising AttributeError.
        self._handle: ctypes.c_void_p | None = None
        buf = _validate_samples("PolyphonicAnalysis", samples)

        lib = _get_lib()
        if not hasattr(lib, "sonare_polyphonic_analyze"):
            raise _unsupported_effect_symbol("sonare_polyphonic_analyze")

        config = SonarePolyphonicConfig(
            struct_version=_POLYPHONIC_STRUCT_VERSION,
            n_fft=_count_field("n_fft", n_fft),
            hop_length=_count_field("hop_length", hop_length),
            win_length=_count_field("win_length", win_length),
            cent_ref_hz=_value_field(cent_ref_hz),
            cents_per_bin=_value_field(cents_per_bin),
            cent_max_hz=_value_field(cent_max_hz),
            tonality_off=1 if tonality_off else 0,
            salience_harmonics=_count_field("salience_harmonics", salience_harmonics),
            f0_min_hz=_value_field(f0_min_hz),
            f0_max_hz=_value_field(f0_max_hz),
            salience_alpha_hz=_value_field(salience_alpha_hz),
            salience_beta_hz=_value_field(salience_beta_hz),
            salience_inharmonicity=_value_field(salience_inharmonicity),
            max_polyphony=_count_field("max_polyphony", max_polyphony),
            min_frame_peak_ratio=_value_field(min_frame_peak_ratio),
            min_separation_cents=_value_field(min_separation_cents),
            subtraction_factor=_value_field(subtraction_factor),
            max_jump_cents=_value_field(max_jump_cents),
            min_ridge_peak_ratio=_value_field(min_ridge_peak_ratio),
            min_ridge_duration_ms=_value_field(min_ridge_duration_ms),
            mask_harmonics=_count_field("mask_harmonics", mask_harmonics),
            claim_lobes=_value_field(claim_lobes),
            inharmonicity=_value_field(inharmonicity),
            estimate_inharmonicity=1 if estimate_inharmonicity else 0,
            inharmonicity_min_partials=_count_field(
                "inharmonicity_min_partials", inharmonicity_min_partials
            ),
            inharmonicity_max_residual_bins=_value_field(inharmonicity_max_residual_bins),
            inharmonicity_max_stretch=_value_field(inharmonicity_max_stretch),
            window_frames=_count_field("window_frames", window_frames),
            min_partial_separation=_value_field(min_partial_separation),
            max_fit_residual=_value_field(max_fit_residual),
            max_weight_modulus=_value_field(max_weight_modulus),
            max_refine_hz=_value_field(max_refine_hz),
            f0_tolerance_cents=_value_field(f0_tolerance_cents),
            segmentation_threshold_cents=_value_field(segmentation_threshold_cents),
            min_note_ms=_value_field(min_note_ms),
            reference_hz=_value_field(reference_hz),
        )

        c_array, length = _to_c_float_array(buf)
        handle = ctypes.c_void_p()
        _check(
            lib.sonare_polyphonic_analyze(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.byref(config),
                ctypes.byref(handle),
            )
        )
        self._handle = handle

    @classmethod
    @_guard_buffer("samples")
    def analyze(
        cls,
        samples: Sequence[float] | list[float] | np.ndarray,
        sample_rate: int,
        **config: object,
    ) -> Self:
        """Analyse ``samples`` into editable notes and return the analysis.

        The constructor under another name, for a call that reads as the verb it
        is. ``config`` takes the constructor's keywords unchanged, documented
        there rather than restated here.

        Returns:
            A :class:`PolyphonicAnalysis`. Release it with :meth:`close` or by
            using it as a context manager.
        """
        return cls(samples, sample_rate, **config)  # type: ignore[arg-type]

    # -- lifecycle ----------------------------------------------------------

    def close(self) -> None:
        """Release the native analysis. Idempotent, so a double close is a no-op."""
        if self._handle is not None:
            _get_lib().sonare_polyphonic_analysis_destroy(self._handle)
            self._handle = None

    # Cross-binding aliases: Node uses destroy(), WASM uses delete().
    def destroy(self) -> None:
        """Alias of :meth:`close` for cross-binding (Node ``destroy``) parity."""
        self.close()

    def delete(self) -> None:
        """Alias of :meth:`close` for cross-binding (WASM ``delete``) parity."""
        self.close()

    def __enter__(self) -> PolyphonicAnalysis:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise RuntimeError("PolyphonicAnalysis is closed")
        return self._handle

    # -- what the analysis found -------------------------------------------

    def note_count(self) -> int:
        """Number of notes, which is also the number of claim sets."""
        lib = _get_lib()
        out = ctypes.c_size_t()
        _check(lib.sonare_polyphonic_note_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def frame_count(self) -> int:
        """Number of STFT frames the analysis ran over."""
        lib = _get_lib()
        out = ctypes.c_int32()
        _check(lib.sonare_polyphonic_frame_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def notes(self) -> list[NoteObject]:
        """Copy the notes out, in the order their claim sets are held in.

        Each note carries its sample span, its frame span, its median pitch, its
        steadiness, its amplitude curve and its pending edit. Two fields of the
        by-value door have no meaning through a handle and are not reported
        here: ``amplitude_offset`` and ``NoteEdit``'s envelope offset are always
        0 on this door, the curves having their own accessors.

        ``amplitude`` is filled from :meth:`note_amplitude` and the edit's
        ``amplitude_envelope`` from :meth:`note_envelope`, so a note comes back
        whole and a host can round-trip one through :meth:`set_note_edit` without
        losing the envelope it set. Each costs one call per note, which is a copy
        rather than any recomputation.

        Returns:
            List of :class:`libsonare.NoteObject`; empty when the material
            tracked no ridge.
        """
        lib = _get_lib()
        handle = self._require_handle()
        count = self.note_count()
        if count == 0:
            return []
        rows = (SonareNoteObject * count)()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_polyphonic_notes(
                handle, rows, _to_c_size_t(count, "count"), ctypes.byref(written)
            )
        )
        return [self._note_from_c(rows[i], i) for i in range(int(written.value))]

    def polyphony(self) -> np.ndarray:
        """Per-frame voice count, before tracking dropped anything.

        What the estimation saw rather than what survived: a frame reported as
        three voices with two notes spanning it is the difference between the two
        stages, which is the figure a host deciding what to edit wants.

        Returns:
            ``int32`` ndarray with one count per frame from frame 0.
        """
        lib = _get_lib()
        handle = self._require_handle()
        capacity = self.frame_count()
        out = (ctypes.c_int32 * capacity)()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_polyphonic_polyphony(
                handle, out, _to_c_size_t(capacity, "capacity"), ctypes.byref(written)
            )
        )
        return _from_c_int_array(out, int(written.value))

    def note_inharmonicity(self) -> np.ndarray:
        """The stretch fitted for each note, where the fit was asked for.

        One entry per note, in :meth:`notes`' order: non-negative where the
        stretch was fitted, and exactly ``-1`` where it was refused, which means
        that note's claims were placed at ``inharmonicity`` instead. **0 is a
        fitted result and means the harmonic series**, so it is not the refusal.

        The refusal is reported rather than folded away because the value a
        refused note ends up using is the declared one, and the declared one
        defaults to 0 -- which is also what a genuine fit returns for an
        unstretched note. Handed only the effective stretch, a host could not
        tell a fit that reached its material from one that did not, and the fit
        refuses a chord at the default framing.

        Returns:
            ``float32`` ndarray with one entry per note, and empty when
            ``estimate_inharmonicity`` was not set. An analysis that asked for
            the fit reports one entry per note whatever happened to each.
        """
        lib = _get_lib()
        handle = self._require_handle()
        capacity = self.note_count()
        out = (ctypes.c_float * capacity)()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_polyphonic_note_inharmonicity(
                handle, out, _to_c_size_t(capacity, "capacity"), ctypes.byref(written)
            )
        )
        return _from_c_float_array(out, int(written.value))

    def note_f0(self, note: int) -> np.ndarray:
        """One note's F0 in Hz, per frame over its own span.

        ``frame_end - frame_start`` entries, so the value at index ``i`` belongs
        to frame ``frame_start + i``. This is the curve the monophonic door makes
        the caller pass back in; here the analysis already holds it, so a curve
        edit needs nothing from the caller.

        Args:
            note: Index below :meth:`note_count`.

        Returns:
            ``float32`` ndarray over the note's frame span.

        Raises:
            SonareValueError: If ``note`` is not a non-negative integer.
            SonareError: If ``note`` is at or past :meth:`note_count`.
        """
        return self._note_curve("sonare_polyphonic_note_f0", "note_f0", note)

    def note_amplitude(self, note: int) -> np.ndarray:
        """One note's linear RMS, per frame over its own span.

        Indexed exactly as :meth:`note_f0`.
        """
        return self._note_curve("sonare_polyphonic_note_amplitude", "note_amplitude", note)

    def note_salience(self, note: int) -> np.ndarray:
        """One note's salience, per frame over its own span.

        Indexed exactly as :meth:`note_f0`, and the one curve here that is not
        the note's own: it is the tracked ridge's, so a frame of the note the
        ridge does not reach reads 0. Salience is what the estimation scored the
        candidate at, so it says how well the material supported this note rather
        than how loud the note is -- :meth:`note_amplitude` is the loud.
        """
        return self._note_curve("sonare_polyphonic_note_salience", "note_salience", note)

    def note_envelope(self, note: int) -> np.ndarray:
        """One note's amplitude envelope points, as last set.

        Indexed from 0 rather than over the note's frame span, and the only one of
        the four accessors here that is not a measurement: these are the points a
        caller handed :meth:`set_note_edit`. An envelope is a set of gain points
        stretched over whatever length the note renders at, so its length is the
        note's own point count and has nothing to do with
        ``frame_end - frame_start``.

        Args:
            note: Index below :meth:`note_count`.

        Returns:
            ``float32`` ndarray of gain points; empty where the note carries no
            envelope.

        Raises:
            SonareValueError: If ``note`` is not a non-negative integer.
            SonareError: If ``note`` is at or past :meth:`note_count`.
        """
        lib = _get_lib()
        handle = self._require_handle()
        index = _note_set_index("note_envelope", "note", note)
        return self._envelope_points(lib, handle, index, self._envelope_count(lib, handle, index))

    # -- editing ------------------------------------------------------------

    def set_note_edit(self, note: int, edit: NoteEdit | None = None) -> None:
        """Replace one note's pending edit.

        The only thing a host writes. Everything else on a note is a
        measurement, and the order is the pairing with the claim sets, so neither
        is settable. The replacement is whole: a field left at its default on
        ``edit`` is set to that default, and an empty
        ``edit.amplitude_envelope`` clears whatever envelope the note carried.

        ``edit.amplitude_envelope`` is the note's envelope here -- per-frame
        linear gain points over the note's span, on top of ``gain_db``, which the
        analysis copies, so the caller's array need not outlive the call. It is
        stretched over whatever length the note renders at, so it survives a time
        stretch and need not match the note's frame count; one entry is a
        constant gain. Every value must be finite and non-negative.

        Args:
            note: Index below :meth:`note_count`.
            edit: The new edit, or ``None`` for the identity edit.

        Raises:
            SonareValueError: If ``note`` is not a non-negative integer or the
                envelope is not one-dimensional.
            SonareError: If ``note`` is at or past :meth:`note_count`.
        """
        lib = _get_lib()
        handle = self._require_handle()
        index = _note_set_index("set_note_edit", "note", note)
        if edit is None:
            _check(
                lib.sonare_polyphonic_set_note_edit(
                    handle, _to_c_size_t(index, "index"), None, None, ctypes.c_size_t(0)
                )
            )
            return

        curve = np.asarray(edit.amplitude_envelope, dtype=np.float32)
        if curve.ndim != 1:
            raise SonareValueError("set_note_edit: edit.amplitude_envelope must be one-dimensional")
        envelope, envelope_count = _to_c_float_array(curve, arg_name="edit.amplitude_envelope")
        # envelope_offset / envelope_count are inert on this door: the points are
        # the argument below and the analysis keeps its own copy.
        c_edit = SonareNoteEdit(
            # Narrowed rather than coerced: int(0.5) is 0, which is this field's
            # identity, so a sub-sample shift would render unmoved and report
            # success.
            time_offset_samples=_validate_c_int_field(
                "set_note_edit", edit.time_offset_samples, "edit.time_offset_samples"
            ),
            pitch_shift_semitones=float(edit.pitch_shift_semitones),
            gain_db=float(edit.gain_db),
            time_stretch_ratio=float(edit.time_stretch_ratio),
            formant_shift_semitones=float(edit.formant_shift_semitones),
            vibrato_depth_change=float(edit.vibrato_depth_change),
            drift_change=float(edit.drift_change),
            muted=1 if edit.muted else 0,
        )
        _check(
            lib.sonare_polyphonic_set_note_edit(
                handle,
                _to_c_size_t(index, "index"),
                ctypes.byref(c_edit),
                envelope,
                _to_c_size_t(envelope_count, "envelope_count"),
            )
        )

    def render(
        self,
        *,
        fade_ms: float | None = None,
        vibrato_cutoff_hz: float | None = None,
    ) -> np.ndarray:
        """Render the analysis back to audio with whatever edits its notes carry.

        Each note's claimed share is inverted, edited, and added to the residual
        -- the part of the input no note claimed. The render is additive per note
        with no cross-note term, so an unedited note's contribution is identical
        between two renders. That is worth stating because it is also the limit:
        a host cannot tell from two renders whether a claim set divided the
        energy correctly.

        Args:
            fade_ms: Equal-power cross-fade at each edited note's edges; ``None``
                keeps the library default (5 ms). A hard cut is deliberately not
                selectable, because the seam it leaves behind is a click.
            vibrato_cutoff_hz: Boundary between the drift and the vibrato that
                ``NoteEdit.vibrato_depth_change`` and ``NoteEdit.drift_change``
                act on; ``None`` keeps the default (3 Hz). Pass whatever a curve
                edit was drawn at -- a host that draws the vibrato at one cutoff
                and edits it at another edits a curve it never showed anyone.

        Returns:
            ``float32`` ndarray with the source's length.

        Raises:
            SonareError: If the C call rejects the request (e.g. a negative
                ``fade_ms``, or an edit the render refuses).
        """
        lib = _get_lib()
        handle = self._require_handle()
        config = SonareNoteRenderConfig(
            _NOTE_STRUCT_VERSION,
            0.0 if fade_ms is None else fade_ms,
            0.0 if vibrato_cutoff_hz is None else vibrato_cutoff_hz,
        )
        with _out_float_array(lib) as (out, out_length):
            _check(
                lib.sonare_polyphonic_render(
                    handle, ctypes.byref(config), ctypes.byref(out), ctypes.byref(out_length)
                )
            )
            return _from_c_float_array(out, out_length.value)

    # -- internals ----------------------------------------------------------

    def _note_curve(self, symbol: str, fn_name: str, note: int) -> np.ndarray:
        """Read one per-frame curve, sizing the buffer so the caller never clamps.

        A note's span is cut out of the analysis's own frames, so the frame count
        bounds each of the three per-frame curves and one query sizes any of them.
        The envelope is not one of them -- it is a point set, not a per-frame
        signal -- so it has its own sizer below.
        """
        lib = _get_lib()
        handle = self._require_handle()
        index = _note_set_index(fn_name, "note", note)
        capacity = self.frame_count()
        out = (ctypes.c_float * capacity)()
        written = ctypes.c_size_t()
        _check(
            getattr(lib, symbol)(
                handle,
                _to_c_size_t(index, "index"),
                out,
                _to_c_size_t(capacity, "capacity"),
                ctypes.byref(written),
            )
        )
        return _from_c_float_array(out, int(written.value))

    def _envelope_count(self, lib: ctypes.CDLL, handle: ctypes.c_void_p, index: int) -> int:
        """How many envelope points note ``index`` holds, which sizes the read.

        Off the note itself rather than off the framing: an envelope need not match
        the note's frame count, so the frame count is not an upper bound on it. The
        notes are read up to this one because the C ABI copies them from 0; an index
        past the end is clamped rather than refused here, and 0 then lets the
        accessor's own call report it with the code its three siblings use.
        """
        count = index + 1
        rows = (SonareNoteObject * count)()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_polyphonic_notes(
                handle, rows, _to_c_size_t(count, "count"), ctypes.byref(written)
            )
        )
        if int(written.value) <= index:
            return 0
        return int(rows[index].edit.envelope_count)

    def _envelope_points(
        self, lib: ctypes.CDLL, handle: ctypes.c_void_p, index: int, capacity: int
    ) -> np.ndarray:
        """Read ``capacity`` envelope points off note ``index``.

        Called even at capacity 0, because that is still the call that validates
        the note index.
        """
        out = (ctypes.c_float * capacity)()
        written = ctypes.c_size_t()
        _check(
            lib.sonare_polyphonic_note_envelope(
                handle,
                _to_c_size_t(index, "index"),
                out,
                _to_c_size_t(capacity, "capacity"),
                ctypes.byref(written),
            )
        )
        return _from_c_float_array(out, int(written.value))

    def _note_from_c(self, row: SonareNoteObject, index: int) -> NoteObject:
        """Build one :class:`libsonare.NoteObject` from a C row.

        The row's ``envelope_count`` sizes the envelope read directly, so filling
        the edit's points costs one call per note and no second count query.
        """
        return NoteObject(
            onset_sample=int(row.onset_sample),
            offset_sample=int(row.offset_sample),
            frame_start=int(row.frame_start),
            frame_end=int(row.frame_end),
            median_hz=float(row.median_hz),
            median_cents=float(row.median_cents),
            f0_stability=float(row.f0_stability),
            amplitude=self.note_amplitude(index),
            edit=NoteEdit(
                time_offset_samples=int(row.edit.time_offset_samples),
                pitch_shift_semitones=float(row.edit.pitch_shift_semitones),
                gain_db=float(row.edit.gain_db),
                time_stretch_ratio=float(row.edit.time_stretch_ratio),
                muted=bool(row.edit.muted),
                formant_shift_semitones=float(row.edit.formant_shift_semitones),
                vibrato_depth_change=float(row.edit.vibrato_depth_change),
                drift_change=float(row.edit.drift_change),
                amplitude_envelope=self._envelope_points(
                    _get_lib(),
                    self._require_handle(),
                    index,
                    int(row.edit.envelope_count),
                ),
            ),
        )
