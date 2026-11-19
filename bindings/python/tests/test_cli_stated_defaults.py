"""An option the CLI leaves to the library states the library's own default in ``--help``."""

from __future__ import annotations

import importlib
import inspect
import re
from typing import Any

import pytest

from libsonare import cli
from libsonare._cli_inventory import _leaf_command_parsers

_STATED = re.compile(r"\(default: ([^)]*)\)")

# (command, flag, facade, parameter, index into a tuple default or None).
_ROWS: tuple[tuple[str, str, str, str, int | None], ...] = (
    (
        "acoustic",
        "--n-octave-bands",
        "_analysis_reports:analyze_impulse_response",
        "n_octave_bands",
        None,
    ),
    (
        "acoustic",
        "--min-decay-db",
        "_analysis_reports:analyze_impulse_response",
        "min_decay_db",
        None,
    ),
    (
        "acoustic",
        "--noise-floor-margin-db",
        "_analysis_reports:detect_acoustic",
        "noise_floor_margin_db",
        None,
    ),
    ("estimate-room", "--aspect-lw", "_acoustic:estimate_room", "aspect_hint_lw", None),
    ("estimate-room", "--aspect-lh", "_acoustic:estimate_room", "aspect_hint_lh", None),
    ("synthesize-rir", "--length", "_acoustic:synthesize_rir", "length_m", None),
    ("synthesize-rir", "--width", "_acoustic:synthesize_rir", "width_m", None),
    ("synthesize-rir", "--height", "_acoustic:synthesize_rir", "height_m", None),
    ("synthesize-rir", "--absorption", "_acoustic:synthesize_rir", "absorption", None),
    ("synthesize-rir", "--ism-order", "_acoustic:synthesize_rir", "ism_order", None),
    ("room-morph", "--absorption", "_acoustic:room_morph", "absorption", None),
    ("room-morph", "--ism-order", "_acoustic:room_morph", "ism_order", None),
    ("room-morph", "--wet", "_acoustic:room_morph", "wet", None),
    ("room-morph", "--suppression", "_acoustic:room_morph", "source_tail_suppression", None),
    ("rhythm", "--start-bpm", "_analysis_reports:analyze_rhythm", "start_bpm", None),
    ("dynamics", "--window-sec", "_analysis_reports:analyze_dynamics", "window_sec", None),
    ("tempogram", "--win-length", "_conversions:tempogram", "win_length", None),
    ("plp", "--win-length", "_conversions:plp", "win_length", None),
    ("key", "--high-pass-hz", "_analysis_detection:detect_key", "chroma_high_pass_hz", None),
    ("chords", "--min-duration-sec", "_analysis_music:detect_chords", "min_duration_sec", None),
    ("chords", "--smoothing-window", "_analysis_music:detect_chords", "smoothing_window_sec", None),
    ("chords", "--threshold", "_analysis_music:detect_chords", "threshold", None),
    ("chords", "--hmm-beam-width", "_analysis_music:detect_chords", "hmm_beam_width", None),
    ("chords", "--chroma-method", "_analysis_music:detect_chords", "chroma_method", None),
    ("chords", "--key-mode", "_analysis_music:detect_chords", "key_mode", None),
    ("sections", "--min-duration-sec", "_analysis_music:analyze_sections", "min_section_sec", None),
    ("analyze", "--chroma-high-pass-hz", "_analysis_reports:analyze", "chroma_high_pass_hz", None),
    ("analyze", "--meter-denominator", "_analysis_reports:analyze", "meter_denominator", None),
    ("pitch", "--threshold", "_features_pitch:pitch_pyin", "threshold", None),
    ("pitch", "--f0-min-hz", "_features_pitch:pitch_pyin", "f0_min_hz", None),
    ("pitch", "--f0-max-hz", "_features_pitch:pitch_pyin", "f0_max_hz", None),
    ("hpss", "--kernel-harmonic", "_effects_separation:hpss", "kernel_harmonic", None),
    ("hpss", "--kernel-percussive", "_effects_separation:hpss", "kernel_percussive", None),
    (
        "pitch-correct-timevarying",
        "--hop-length",
        "_effects_timepitch:pitch_correct_timevarying",
        "hop_length",
        None,
    ),
    (
        "pitch-correct-timevarying",
        "--scale-root",
        "_effects_timepitch:pitch_correct_timevarying",
        "scale_root",
        None,
    ),
    (
        "tune-to-midi",
        "--unmatched-policy",
        "_effects_note_ops:assign_note_targets",
        "unmatched_policy",
        None,
    ),
    (
        "declip",
        "--lpc-order",
        "_effects_repair_impulsive:mastering_repair_declip",
        "lpc_order",
        None,
    ),
    (
        "declip",
        "--iterations",
        "_effects_repair_impulsive:mastering_repair_declip",
        "iterations",
        None,
    ),
    (
        "declip",
        "--lpc-blend",
        "_effects_repair_impulsive:mastering_repair_declip",
        "lpc_blend",
        None,
    ),
    (
        "project.bounce",
        "--instrument-latency-samples",
        "_project_render:_ProjectRenderMixin.bounce",
        "instrument_latency_samples",
        None,
    ),
    (
        "midi-render",
        "--instrument-latency-samples",
        "_project_render:_ProjectRenderMixin.bounce",
        "instrument_latency_samples",
        None,
    ),
    ("transcribe", "--group", "_project_midi:_ProjectMidiMixin.transcribe_to_clip", "group", None),
    ("trim-silence", "--threshold-db", "_effects_level:trim", "threshold_db", None),
    ("trim-silence", "--n-fft", "_effects_level:trim", "frame_length", None),
    ("trim-silence", "--hop-length", "_effects_level:trim", "hop_length", None),
    ("split-silence", "--top-db", "_conversions:split_silence_common_with_report", "top_db", None),
    (
        "split-silence",
        "--n-fft",
        "_conversions:split_silence_common_with_report",
        "frame_length",
        None,
    ),
    (
        "split-silence",
        "--hop-length",
        "_conversions:split_silence_common_with_report",
        "hop_length",
        None,
    ),
    (
        "transcribe",
        "--channel",
        "_project_midi:_ProjectMidiMixin.transcribe_to_clip",
        "channel",
        None,
    ),
    ("rhythm", "--n-fft", "_analysis_reports:analyze_rhythm", "n_fft", None),
    ("rhythm", "--hop-length", "_analysis_reports:analyze_rhythm", "hop_length", None),
    ("chords", "--n-fft", "_analysis_music:detect_chords", "n_fft", None),
    ("chords", "--hop-length", "_analysis_music:detect_chords", "hop_length", None),
    ("sections", "--n-fft", "_analysis_music:analyze_sections", "n_fft", None),
    ("sections", "--hop-length", "_analysis_music:analyze_sections", "hop_length", None),
    ("chroma", "--n-fft", "_features_core:chroma", "n_fft", None),
    ("chroma", "--hop-length", "_features_core:chroma", "hop_length", None),
    ("spectral", "--n-fft", "_features_core:spectral_centroid", "n_fft", None),
    ("spectral", "--hop-length", "_features_core:spectral_centroid", "hop_length", None),
    ("timbre", "--n-fft", "_analysis_reports:analyze_timbre", "n_fft", None),
    ("timbre", "--hop-length", "_analysis_reports:analyze_timbre", "hop_length", None),
    ("timbre", "--n-mels", "_analysis_reports:analyze_timbre", "n_mels", None),
    ("onset-envelope", "--n-fft", "_conversions:onset_strength", "n_fft", None),
    ("onset-envelope", "--hop-length", "_conversions:onset_strength", "hop_length", None),
    ("onset-envelope", "--n-mels", "_conversions:onset_strength", "n_mels", None),
    ("tempogram", "--n-fft", "_conversions:onset_strength", "n_fft", None),
    ("tempogram", "--hop-length", "_conversions:onset_strength", "hop_length", None),
    ("tempogram", "--n-mels", "_conversions:onset_strength", "n_mels", None),
    ("tempogram", "--hop-length", "_conversions:tempogram", "hop_length", None),
    ("plp", "--n-fft", "_conversions:onset_strength", "n_fft", None),
    ("plp", "--hop-length", "_conversions:onset_strength", "hop_length", None),
    ("plp", "--n-mels", "_conversions:onset_strength", "n_mels", None),
    ("plp", "--hop-length", "_conversions:plp", "hop_length", None),
    ("mel", "--n-fft", "_features_core:mel_spectrogram", "n_fft", None),
    ("mel", "--hop-length", "_features_core:mel_spectrogram", "hop_length", None),
    ("mel", "--n-mels", "_features_core:mel_spectrogram", "n_mels", None),
    ("mel", "--fmin-hz", "_features_core:mel_spectrogram", "fmin_hz", None),
    ("key", "--n-fft", "_analysis_detection:detect_key", "n_fft", None),
    ("key", "--hop-length", "_analysis_detection:detect_key", "hop_length", None),
    ("dynamics", "--hop-length", "_analysis_reports:analyze_dynamics", "hop_length", None),
    ("nnls-chroma", "--hop-length", "_conversions:nnls_chroma", "hop_length", None),
    ("pitch", "--hop-length", "_features_pitch:pitch_pyin", "hop_length", None),
    ("hpss", "--n-fft", "_effects_separation:hpss", "n_fft", None),
    ("pitch-shift", "--n-fft", "_effects_timepitch:pitch_shift", "n_fft", None),
    ("time-stretch", "--n-fft", "_effects_timepitch:time_stretch", "n_fft", None),
)

# Axes of the tuple-valued ``source`` / ``listener`` defaults, one flag per axis.
_AXES = tuple(
    (command, f"--{role}-{axis}", f"_acoustic:{facade}", role, index)
    for command, facade in (("synthesize-rir", "synthesize_rir"), ("room-morph", "room_morph"))
    for role in ("source", "listener")
    for index, axis in enumerate("xyz")
)

# Options that state a default in ``--help`` while the facade parameter is ``None``, either
# because ``None`` is how the library is told to choose, or because the stated value is not
# a single number. The value noted is what the library then uses.
_NOT_A_SIGNATURE_DEFAULT: dict[tuple[str, str], str] = {
    ("estimate-room", "--reference-absorption"): "None selects the C default, 0.15",
    ("synthesize-rir", "--seed"): "None selects the core seed, 1",
    ("room-morph", "--seed"): "None selects the core seed, 1",
    ("room-morph", "--receiver-spacing"): "None selects the core spacing, 0.5 m",
    ("synthesize-rir", "--max-sec"): "None is the natural length",
    ("room-morph", "--max-sec"): "None is the natural length",
    ("rhythm", "--bpm-min"): "None selects the module constant, 60.0",
    ("rhythm", "--bpm-max"): "None selects the module constant, 200.0",
    ("key", "--modes"): "None selects major and minor",
    ("key", "--profile"): "None lets the genre hint choose",
    ("key", "--genre-hint"): "None is auto",
    ("chords", "--key-root"): "PitchClass.C, an enum rather than the stated name",
    ("decompose-stems", "--n-components"): "None selects the C default, 4",
    ("decompose-stems", "--n-iter"): "None selects the C default, 100",
    ("decompose-stems", "--beta"): "None selects the C default, 2.0",
    ("decompose-stems", "--init"): "None selects the C default, random",
    ("decompose-stems", "--mask-power"): "None selects the C default, 1.0",
    ("decompose-stems", "--n-fft"): "None selects the C default, 2048",
    ("decompose-stems", "--hop-length"): "None derives a quarter of the FFT size in the core",
    ("hpss", "--hop-length"): "None derives a quarter of the FFT size in the core",
    ("pitch-shift", "--hop-length"): "None derives a quarter of the FFT size in the core",
    ("time-stretch", "--hop-length"): "None derives a quarter of the FFT size in the core",
    ("project.align-takes", "--hop-length"): "None selects the C default, 512",
    ("project.align-takes", "--bins-per-octave"): "None selects the C default, 12",
    ("pitch-correct-timevarying", "--scale-mode-mask"): "None selects the C-major mask, 2741",
    ("pitch-correct-timevarying", "--reference-midi"): "None selects A4, 69.0",
    ("pitch-correct-timevarying", "--retune-amount"): "None selects the C default, 1.0",
    ("pitch-correct-timevarying", "--max-correction-semitones"): "None selects 12.0",
    ("pitch-correct-timevarying", "--retune-speed-ms"): "None selects 50.0",
    ("pitch-correct-timevarying", "--vibrato-threshold-cents"): "None selects 20.0",
    ("project.new", "--sample-rate"): "the project's own rate; the setter takes a required value",
    ("sections", "--threshold"): "None selects the core section threshold",
    ("analyze", "--meter-candidates"): "None selects the core set, 3,4,6",
    ("mel", "--fmax-hz"): "None is the Nyquist frequency",
    ("normalize", "--target-db"): "None selects the mode's level, 0.0 (peak) or -20.0 (rms)",
    ("voice-change", "--preset"): "None selects the non-preset path",
    ("declip", "--clip-threshold-db"): "a dBFS spelling of the library's 0.98 of full scale",
    ("project.bounce", "--frames"): "None derives the length from the project",
    ("project.bounce", "--channels"): "None is the scene master's width",
    ("midi-render", "--frames"): "None derives the length from the project",
    ("midi-render", "--channels"): "None is the scene master's width",
    ("midi-render", "--synth"): "None selects GM program routing",
    ("transcribe", "--max-polyphony"): "None selects the core default, 10",
    ("transcribe", "--min-frame-peak-ratio"): "None selects the core default, 0.20",
    ("transcribe", "--min-ridge-peak-ratio"): "None selects the core default, 0.10",
    ("transcribe", "--reattack-ratio"): "None selects the core default, 2.0",
    ("transcribe", "--segmentation-threshold-cents"): "None selects the core default, 50",
    ("playback", "--target"): "a field of the render config, not a signature default",
    ("playback", "--input-layout"): "a field of the render config, not a signature default",
    ("playback", "--night"): "a field of the render config, not a signature default",
    ("playback", "--dialogue-db"): "a field of the render config, not a signature default",
    ("playback", "--program-lufs"): "a field of the render config, not a signature default",
    ("playback", "--target-lufs"): "a field of the render config, not a signature default",
    ("transcribe", "--monophonic-model"): "None selects segments",
    ("transcribe", "--tempo-bpm"): "detected from the audio",
    ("transcribe", "--min-note-ms"): "depends on the model",
    ("transcribe", "--velocity-floor-db"): "None selects -48",
    ("transcribe", "--reference-hz"): "None selects 440",
}


def _resolve(spec: str) -> Any:
    module_name, _, qualified = spec.partition(":")
    target: Any = importlib.import_module(f"libsonare.{module_name}")
    for part in qualified.split("."):
        target = getattr(target, part)
    return target


def _command_parsers() -> dict[str, Any]:
    return dict(_leaf_command_parsers(cli._build_parser()))


def _option(parser: Any, flag: str) -> Any:
    return next(action for action in parser._actions if flag in action.option_strings)


def _stated(action: Any) -> str | None:
    match = _STATED.search(action.help or "")
    return None if match is None else match.group(1)


def _same(stated: str, default: object) -> bool:
    if isinstance(default, (int, float)) and not isinstance(default, bool):
        try:
            return float(stated) == float(default)
        except ValueError:
            return False
    return stated == str(default)


@pytest.mark.parametrize(
    ("command", "flag", "facade", "parameter", "index"),
    _ROWS + _AXES,
    ids=lambda value: str(value),
)
def test_help_states_the_facade_default(
    command: str, flag: str, facade: str, parameter: str, index: int | None
) -> None:
    action = _option(_command_parsers()[command], flag)
    assert action.default is None, f"{command} {flag} restates a default instead of leaving it out"
    stated = _stated(action)
    assert stated is not None, f"{command} {flag} help does not state its default"
    default = inspect.signature(_resolve(facade)).parameters[parameter].default
    assert default is not inspect.Parameter.empty
    if index is not None:
        default = default[index]
    assert _same(stated, default), f"{command} {flag}: help says {stated}, facade holds {default!r}"


def test_every_stated_default_is_checked_or_excused() -> None:
    covered = {(command, flag) for command, flag, *_ in _ROWS + _AXES}
    seen: set[tuple[str, str]] = set()
    for command, parser in _command_parsers().items():
        for action in parser._actions:
            if not action.option_strings or action.default is not None or _stated(action) is None:
                continue
            key = (command, action.option_strings[-1])
            seen.add(key)
            assert key in covered or key in _NOT_A_SIGNATURE_DEFAULT, (
                f"{key} states a default in --help and is neither compared with the facade nor "
                "excused in _NOT_A_SIGNATURE_DEFAULT"
            )
    stale = set(_NOT_A_SIGNATURE_DEFAULT) - seen
    assert not stale, f"excused options that no longer state a default: {sorted(stale)}"


def _key_call(monkeypatch: pytest.MonkeyPatch, *extra: str) -> dict[str, Any]:
    from types import SimpleNamespace

    import libsonare
    from libsonare import _cli_analysis

    seen: dict[str, Any] = {}

    def fake_detect_key(samples: object, **kwargs: Any) -> SimpleNamespace:
        seen.update(kwargs)
        return SimpleNamespace(root=SimpleNamespace(value=0), mode="major", confidence=0.5)

    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))
    monkeypatch.setattr(libsonare, "detect_key", fake_detect_key)
    args = cli._build_parser().parse_args(["key", "input.wav", *extra, "--json"])
    assert _cli_analysis.cmd_key(args) == 0
    return seen


def test_key_options_left_out_are_not_passed(monkeypatch: pytest.MonkeyPatch) -> None:
    seen = _key_call(monkeypatch)
    for name in ("modes", "profile", "genre_hint", "chroma_high_pass_hz"):
        assert name not in seen


def test_an_empty_modes_value_is_forwarded_as_given(monkeypatch: pytest.MonkeyPatch) -> None:
    # The library refuses an empty mode set; the CLI hands it over rather than deciding.
    assert _key_call(monkeypatch, "--modes", "")["modes"] == []


def test_an_empty_synth_name_is_not_the_bare_flag() -> None:
    from libsonare._cli_project import _synth_preset

    assert _synth_preset(None, bare_flag=False) is None
    assert _synth_preset("", bare_flag=True) is None
    assert _synth_preset("", bare_flag=False) is not None
