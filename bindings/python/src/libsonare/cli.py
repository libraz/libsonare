# ruff: noqa: F405
"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import sys

from ._cli_advanced import *  # noqa: F403
from ._cli_analysis import *  # noqa: F403
from ._cli_common import (
    _SONARE_CODE_TO_EXIT as _SONARE_CODE_TO_EXIT,
)
from ._cli_common import (
    EXIT_DECODE_FAILED as EXIT_DECODE_FAILED,
)
from ._cli_common import (
    EXIT_ERROR as EXIT_ERROR,
)
from ._cli_common import (
    EXIT_FILE_NOT_FOUND as EXIT_FILE_NOT_FOUND,
)
from ._cli_common import (
    EXIT_INVALID_FORMAT as EXIT_INVALID_FORMAT,
)
from ._cli_common import (
    EXIT_INVALID_PARAMETER as EXIT_INVALID_PARAMETER,
)
from ._cli_common import (
    EXIT_INVALID_STATE as EXIT_INVALID_STATE,
)
from ._cli_common import (
    EXIT_NOT_SUPPORTED as EXIT_NOT_SUPPORTED,
)
from ._cli_common import (
    EXIT_OUT_OF_MEMORY as EXIT_OUT_OF_MEMORY,
)
from ._cli_common import (
    EXIT_SUCCESS as EXIT_SUCCESS,
)
from ._cli_common import (
    EXIT_USAGE as EXIT_USAGE,
)
from ._cli_common import (
    MODE_NAMES as MODE_NAMES,
)
from ._cli_common import (
    PITCH_NAMES as PITCH_NAMES,
)
from ._cli_common import (
    _apply_voice_macro_override as _apply_voice_macro_override,
)
from ._cli_common import (
    _apply_voice_sets as _apply_voice_sets,
)
from ._cli_common import (
    _array_stats as _array_stats,
)
from ._cli_common import (
    _atomic_write_bytes as _atomic_write_bytes,
)
from ._cli_common import (
    _emit_effect_result as _emit_effect_result,
)
from ._cli_common import (
    _exit_code_for as _exit_code_for,
)
from ._cli_common import (
    _float_sequence as _float_sequence,
)
from ._cli_common import (
    _format_time as _format_time,
)
from ._cli_common import (
    _legacy_exit_codes as _legacy_exit_codes,
)
from ._cli_common import (
    _load_audio as _load_audio,
)
from ._cli_common import (
    _load_json_object as _load_json_object,
)
from ._cli_common import (
    _load_voice_preset_pack as _load_voice_preset_pack,
)
from ._cli_common import (
    _parse_json_config as _parse_json_config,
)
from ._cli_common import (
    _parse_json_list as _parse_json_list,
)
from ._cli_common import (
    _parse_key_profile as _parse_key_profile,
)
from ._cli_common import (
    _parse_kv_params as _parse_kv_params,
)
from ._cli_common import (
    _parse_mode as _parse_mode,
)
from ._cli_common import (
    _parse_modes as _parse_modes,
)
from ._cli_common import (
    _parse_pitch_class as _parse_pitch_class,
)
from ._cli_common import (
    _parse_voice_set_value as _parse_voice_set_value,
)
from ._cli_common import (
    _pcm16 as _pcm16,
)
from ._cli_common import (
    _read_bounded as _read_bounded,
)
from ._cli_common import (
    _resample as _resample,
)
from ._cli_common import (
    _resample_linear as _resample_linear,
)
from ._cli_common import (
    _set_nested_value as _set_nested_value,
)
from ._cli_common import (
    _write_project_bounce_wav as _write_project_bounce_wav,
)
from ._cli_common import (
    _write_wav as _write_wav,
)
from ._cli_common import (
    _write_wav_stereo as _write_wav_stereo,
)
from ._cli_effects import *  # noqa: F403
from ._cli_mastering import *  # noqa: F403
from ._cli_project import *  # noqa: F403
from ._cli_project import (
    _load_project as _load_project,
)
from ._cli_project import (
    _project_bounce as _project_bounce,
)
from ._cli_project import (
    _write_project_json as _write_project_json,
)
from ._facade import rebind_facade_exports as _rebind_facade_exports


def main() -> None:
    """CLI entry point."""
    # Common arguments shared by all subcommands
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--json", action="store_true", help="Output JSON")
    common.add_argument("-o", "--output", type=str, default="", help="Output file path")

    fft_options = argparse.ArgumentParser(add_help=False, parents=[common])
    fft_options.add_argument("--n-fft", type=int, default=2048, help="FFT size (default: 2048)")
    fft_options.add_argument(
        "--hop-length", type=int, default=512, help="Hop length (default: 512)"
    )
    mel_options = argparse.ArgumentParser(add_help=False, parents=[fft_options])
    mel_options.add_argument(
        "--n-mels", type=int, default=128, help="Number of mel bands (default: 128)"
    )

    parser = argparse.ArgumentParser(
        prog="sonare",
        description="libsonare - Fast audio analysis (Python CLI)",
    )
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("version", parents=[common], help="Show version")
    sub.add_parser("info", parents=[common], help="Show audio file information")
    sub.add_parser("bpm", parents=[common], help="Detect BPM")
    key_p = sub.add_parser("key", parents=[common], help="Detect musical key")
    # Key detection keeps a 4096-sample analysis default (better low-frequency
    # resolution) rather than the shared 2048. A None sentinel distinguishes
    # "left at the default" from an explicit value, matching the native CLI and
    # the detect_key() library default. These FFT options are declared here
    # instead of inherited from fft_options: parents= shares the actual argument
    # objects, so a set_defaults override would mutate the shared --n-fft used by
    # the other analysis commands.
    key_p.add_argument(
        "--n-fft", type=int, default=None, help="FFT size (default: 4096 for key analysis)"
    )
    key_p.add_argument("--hop-length", type=int, default=512, help="Hop length (default: 512)")
    key_p.add_argument(
        "--candidates",
        type=int,
        default=0,
        metavar="N",
        help="Also show the top N key candidates",
    )
    key_p.add_argument("--use-hpss", action="store_true", help="Use harmonic audio for key chroma")
    key_p.add_argument(
        "--loudness-weighted", action="store_true", help="Weight key chroma frames by RMS"
    )
    key_p.add_argument(
        "--high-pass-hz", type=float, default=0.0, help="High-pass cutoff before key analysis"
    )
    key_p.add_argument(
        "--modes",
        type=str,
        default="",
        help="Candidate modes: major-minor, all, or comma-separated mode names",
    )
    key_p.add_argument(
        "--profile",
        type=str,
        default="",
        help="Key profile: ks, temperley, shaath, edmt, edma, edmm, or bellman",
    )
    key_p.add_argument(
        "--genre-hint",
        type=str,
        default="",
        help="Genre hint for key profile selection, e.g. auto, edm, pop, classical, jazz",
    )
    sub.add_parser("beats", parents=[common], help="Detect beat times")
    sub.add_parser("downbeats", parents=[common], help="Detect downbeat times")
    sub.add_parser("onsets", parents=[common], help="Detect onset times")
    chords_p = sub.add_parser("chords", parents=[fft_options], help="Detect chord progression")
    chords_p.add_argument(
        "--min-duration", type=float, default=0.3, help="Minimum chord duration in seconds"
    )
    chords_p.add_argument(
        "--smoothing-window", type=float, default=2.0, help="Chroma smoothing window in seconds"
    )
    chords_p.add_argument(
        "--threshold", type=float, default=0.5, help="Chord detection confidence threshold"
    )
    chords_p.add_argument(
        "--triads-only", action="store_true", help="Restrict output to triad qualities"
    )
    chords_p.add_argument(
        "--nnls", action="store_true", help="Use NNLS chroma instead of the STFT chroma"
    )
    chords_p.add_argument(
        "--no-beat-sync", action="store_true", help="Disable beat-synchronous chroma pooling"
    )
    chords_p.add_argument(
        "--use-hmm", action="store_true", help="Decode the chord sequence with an HMM"
    )
    chords_p.add_argument("--hmm-beam-width", type=int, default=24, help="HMM decoder beam width")
    chords_p.add_argument(
        "--key-context", action="store_true", help="Bias detection with a key context"
    )
    chords_p.add_argument(
        "--key-root", default="C", help="Key-context root pitch class (e.g. C, F#)"
    )
    chords_p.add_argument(
        "--key-mode", default="major", help="Key-context mode (e.g. major, minor)"
    )
    chords_p.add_argument(
        "--detect-inversions", action="store_true", help="Report chord inversions (bass note)"
    )
    sub.add_parser("analyze", parents=[common], help="Full music analysis")
    mel_p = sub.add_parser("mel", parents=[mel_options], help="Compute mel spectrogram")
    mel_p.add_argument("--fmin", type=float, default=0.0, help="Lowest mel band frequency in Hz")
    mel_p.add_argument(
        "--fmax", type=float, default=0.0, help="Highest mel band frequency in Hz (0 = Nyquist)"
    )
    mel_p.add_argument(
        "--htk", action="store_true", help="Use the HTK mel formula instead of Slaney"
    )
    sub.add_parser("chroma", parents=[fft_options], help="Compute chromagram")
    sub.add_parser("spectral", parents=[fft_options], help="Compute spectral features")
    pitch_p = sub.add_parser("pitch", parents=[common], help="Track pitch")
    pitch_p.add_argument("--algorithm", choices=["yin", "pyin"], default="pyin")
    sub.add_parser("hpss", parents=[common], help="Harmonic-percussive separation")

    # Editing commands
    pitch_correct_p = sub.add_parser(
        "pitch-correct", parents=[common], help="Pitch-correct from a current to a target MIDI note"
    )
    pitch_correct_p.add_argument(
        "--current-midi", type=float, default=69.0, help="Current pitch as a MIDI note number"
    )
    pitch_correct_p.add_argument(
        "--target-midi", type=float, default=69.0, help="Target pitch as a MIDI note number"
    )
    pitch_tv_p = sub.add_parser(
        "pitch-correct-timevarying",
        parents=[common],
        help="Track pYIN contour and correct toward a note or scale",
    )
    pitch_tv_p.add_argument("--mode", choices=["midi", "scale"], default="midi")
    pitch_tv_p.add_argument("--target-midi", type=float, default=69.0)
    pitch_tv_p.add_argument("--hop-length", type=int, default=512)
    pitch_tv_p.add_argument("--scale-root", type=int, default=0)
    pitch_tv_p.add_argument("--scale-mode-mask", type=lambda value: int(value, 0), default=0xAB5)
    pitch_tv_p.add_argument("--reference-midi", type=float, default=69.0)
    note_move_p = sub.add_parser("note-move", parents=[common], help="Move one note region")
    note_move_p.add_argument("--onset", type=int, default=0)
    note_move_p.add_argument("--offset", type=int, default=None)
    note_move_p.add_argument("--target-onset", type=int, required=True)
    scale_quantize_p = sub.add_parser(
        "scale-quantize", parents=[common], help="Quantize one MIDI value to a scale"
    )
    scale_quantize_p.add_argument("midi", type=float)
    scale_quantize_p.add_argument("--root", type=int, default=0)
    scale_quantize_p.add_argument("--mode-mask", type=lambda value: int(value, 0), default=0xAB5)
    scale_quantize_p.add_argument("--reference-midi", type=float, default=69.0)
    note_stretch_p = sub.add_parser(
        "note-stretch", parents=[common], help="Time-stretch a single note region"
    )
    note_stretch_p.add_argument(
        "--onset", type=int, default=0, help="Start sample index of the note region"
    )
    note_stretch_p.add_argument(
        "--offset", type=int, default=0, help="End sample index of the note region"
    )
    note_stretch_p.add_argument(
        "--ratio", type=float, default=1.0, help="Stretch factor for the region (>1 lengthens)"
    )
    # Effect commands that map directly to the Python effects API. The C++ CLI
    # still exposes some low-level converters and section/melody analyses that
    # are not mirrored here; this set covers the common offline edits.
    pitch_shift_p = sub.add_parser(
        "pitch-shift", parents=[common], help="Shift pitch by a number of semitones"
    )
    pitch_shift_p.add_argument(
        "--semitones", type=float, default=0.0, help="Semitones to shift (positive = up)"
    )
    time_stretch_p = sub.add_parser(
        "time-stretch", parents=[common], help="Time-stretch without changing pitch"
    )
    time_stretch_p.add_argument(
        "--rate", type=float, default=1.0, help="Stretch factor (>1 speeds up, <1 slows down)"
    )
    normalize_p = sub.add_parser(
        "normalize", parents=[common], help="Peak-normalize audio to a target dB level"
    )
    normalize_p.add_argument(
        "--target-db", type=float, default=0.0, help="Target peak level in dB (default: 0.0)"
    )
    trim_silence_p = sub.add_parser(
        "trim-silence", parents=[common], help="Trim leading/trailing silence"
    )
    trim_silence_p.add_argument(
        "--threshold-db", type=float, default=-60.0, help="Silence threshold in dB (default: -60)"
    )
    resample_p = sub.add_parser(
        "resample", parents=[common], help="Resample audio to a target sample rate"
    )
    resample_p.add_argument(
        "--target-rate",
        "--target-sr",
        dest="target_rate",
        type=int,
        required=True,
        help="Target sample rate in Hz",
    )
    voice_change_p = sub.add_parser(
        "voice-change", parents=[common], help="Apply a voice-change effect"
    )
    voice_change_p.add_argument(
        "--pitch-semitones", type=float, default=0.0, help="Pitch shift in semitones"
    )
    voice_change_p.add_argument(
        "--formant-factor", type=float, default=1.0, help="Formant scaling factor (1.0 = unchanged)"
    )
    voice_change_p.add_argument("--preset", default="", help="Realtime voice changer preset id")
    voice_change_p.add_argument("--preset-json", help="Realtime voice changer preset JSON file")
    voice_change_p.add_argument(
        "--preset-pack", help="Realtime voice changer preset pack JSON file"
    )
    voice_change_p.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="PATH=VALUE",
        help="Override preset JSON fields, e.g. dsp.outputGainDb=-2",
    )
    voice_presets_p = sub.add_parser("voice-presets", help="List realtime voice changer presets")
    voice_presets_p.add_argument("--json", action="store_true", help="Emit JSON")
    voice_preset_p = sub.add_parser(
        "voice-preset", help="Print a realtime voice changer preset (always JSON)"
    )
    voice_preset_p.add_argument("--preset", default="neutral-monitor", help="Preset id")
    voice_preset_p.add_argument(
        "--json", action="store_true", help="No-op; a preset is always printed as JSON"
    )
    # Not a parents=[common] subcommand: it consumes a JSON preset, not audio,
    # so the analysis flags (--n-fft/--hop-length/--n-mels) do not apply and the
    # positional argument is a preset file rather than an audio file.
    voice_preset_validate_p = sub.add_parser(
        "voice-preset-validate", help="Validate and normalize voice preset JSON"
    )
    voice_preset_validate_p.add_argument("file", help="Voice preset JSON file")
    voice_preset_validate_p.add_argument("--json", action="store_true", help="Output JSON")
    voice_preset_validate_p.add_argument(
        "--preset", default="", help="Preset id when validating a pack"
    )
    voice_preset_validate_p.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="PATH=VALUE",
        help="Override preset JSON fields before validation",
    )

    # Analysis commands
    acoustic_p = sub.add_parser("acoustic", parents=[common], help="Estimate acoustic parameters")
    acoustic_p.add_argument("--ir", action="store_true", help="Treat input as an impulse response")

    def _add_room_geometry(p: argparse.ArgumentParser) -> None:
        p.add_argument("--length", type=float, default=7.0, help="Room length (m)")
        p.add_argument("--width", type=float, default=5.0, help="Room width (m)")
        p.add_argument("--height", type=float, default=3.0, help="Room height (m)")
        p.add_argument("--absorption", type=float, default=0.2, help="Uniform wall absorption")
        p.add_argument("--source-x", type=float, default=1.0)
        p.add_argument("--source-y", type=float, default=1.0)
        p.add_argument("--source-z", type=float, default=1.2)
        p.add_argument("--listener-x", type=float, default=5.0)
        p.add_argument("--listener-y", type=float, default=4.0)
        p.add_argument("--listener-z", type=float, default=1.7)
        p.add_argument("--ism-order", type=int, default=3, help="Image-source reflection order")
        p.add_argument("--seed", type=int, default=1, help="Deterministic late-tail seed")
        p.add_argument(
            "--max-seconds",
            type=float,
            default=0.0,
            help="Hard cap on RIR/tail length in seconds (0 = natural length)",
        )
        p.add_argument(
            "--sabine",
            action="store_true",
            help="Use the Sabine late-reverb model (default Eyring)",
        )

    estimate_room_p = sub.add_parser(
        "estimate-room", parents=[common], help="Estimate equivalent room from a recording"
    )
    estimate_room_p.add_argument("--aspect-lw", type=float, default=1.0, help="length/width prior")
    estimate_room_p.add_argument("--aspect-lh", type=float, default=1.0, help="length/height prior")
    estimate_room_p.add_argument(
        "--reference-absorption", type=float, default=0.15, help="absorption prior"
    )
    estimate_room_p.add_argument(
        "--sabine", action="store_true", help="Use the Sabine model (default Eyring)"
    )
    estimate_room_p.add_argument(
        "--n-octave-bands",
        type=int,
        default=0,
        help="Analyzer octave-band count (0 = library default)",
    )

    synth_rir_p = sub.add_parser(
        "synthesize-rir", parents=[common], help="Synthesize a room impulse response from geometry"
    )
    _add_room_geometry(synth_rir_p)
    synth_rir_p.add_argument("--sample-rate", type=int, default=48000, help="Output sample rate")

    room_morph_p = sub.add_parser(
        "room-morph", parents=[common], help="Morph reverberation toward a target room"
    )
    _add_room_geometry(room_morph_p)
    room_morph_p.add_argument("--wet", type=float, default=0.5, help="Target-room mix [0,1]")
    room_morph_p.add_argument(
        "--suppression", type=float, default=0.5, help="Source-tail suppression [0,1]"
    )

    sub.add_parser("rhythm", parents=[common], help="Analyze rhythm primitives")
    sub.add_parser("dynamics", parents=[common], help="Analyze dynamics/loudness")
    sub.add_parser("timbre", parents=[mel_options], help="Analyze timbre/spectral shape")
    lufs_p = sub.add_parser("lufs", parents=[common], help="Compute LUFS loudness")
    lufs_p.add_argument(
        "--series", action="store_true", help="Also emit momentary/short-term LUFS series"
    )
    sub.add_parser(
        "onset-envelope", parents=[mel_options], help="Compute the onset strength envelope"
    )
    sub.add_parser("nnls-chroma", parents=[common], help="Compute NNLS chroma")
    sub.add_parser("tempogram", parents=[mel_options], help="Compute autocorrelation tempogram")
    sub.add_parser("plp", parents=[mel_options], help="Compute predominant local pulse")

    # Mastering commands
    mastering_p = sub.add_parser(
        "mastering", parents=[common], help="Loudness-normalize with a true-peak ceiling"
    )
    mastering_p.add_argument("--target-lufs", type=float, default=-14.0)
    mastering_p.add_argument("--ceiling-db", type=float, default=-1.0)
    mproc_p = sub.add_parser(
        "mastering-processor", parents=[common], help="Apply a named mastering processor"
    )
    mproc_p.add_argument("--processor", required=True, help="Processor name")
    mproc_p.add_argument("--params", default="", help="Params as k=v,k=v (floats)")
    eq_p = sub.add_parser("eq", parents=[common], help="Apply the unified equalizer")
    eq_p.add_argument("--params", default="", help="Params as k=v,k=v (overrides band shortcuts)")
    eq_p.add_argument(
        "--type",
        type=int,
        default=0,
        help=(
            "Band type enum: 0 peak, 1 low shelf, 2 high shelf, 3 low pass, "
            "4 high pass, 5 band pass, 6 notch, 7 tilt"
        ),
    )
    eq_p.add_argument("--frequency-hz", type=float, default=1000.0)
    eq_p.add_argument("--gain-db", type=float, default=0.0)
    eq_p.add_argument("--q", type=float, default=1.0)
    eq_p.add_argument("--coeff-mode", type=int, default=0, help="0 RBJ, 1 Vicanek")
    eq_p.add_argument("--slope-db-oct", type=int, default=12)
    eq_p.add_argument(
        "--placement", type=int, default=0, help="0 stereo, 1 left, 2 right, 3 mid, 4 side"
    )
    eq_p.add_argument(
        "--phase-mode", type=int, default=1, help="1 zero latency, 2 natural, 3 linear"
    )
    eq_p.add_argument(
        "--resolution",
        type=int,
        default=0,
        help="0 custom/default, 1 low, 2 medium, 3 high, 4 very high, 5 maximum",
    )
    eq_p.add_argument("--auto-gain", action="store_true")
    eq_p.add_argument("--gain-scale", type=float, default=1.0)
    eq_p.add_argument("--output-gain-db", type=float, default=0.0)
    eq_p.add_argument("--output-pan", type=float, default=0.0)
    eq_p.add_argument("--proportional-q", action="store_true")
    eq_p.add_argument("--dynamic", action="store_true")
    eq_p.add_argument("--threshold-db", type=float, default=-24.0)
    eq_p.add_argument("--auto-threshold", action="store_true")
    eq_p.add_argument("--ratio", type=float, default=2.0)
    eq_p.add_argument("--range-db", type=float, default=-6.0)
    eq_p.add_argument("--attack-ms", type=float, default=5.0)
    eq_p.add_argument("--release-ms", type=float, default=50.0)
    eq_p.add_argument("--lookahead-ms", type=float, default=0.0)
    eq_p.add_argument("--sidechain-freq-hz", type=float, default=-1.0)
    eq_p.add_argument("--sidechain-q", type=float, default=1.0)
    sub.add_parser("mastering-processors", parents=[common], help="List mastering processor names")
    sub.add_parser(
        "mastering-pair-processors",
        parents=[common],
        help="List two-input mastering processor names",
    )
    sub.add_parser(
        "mastering-pair-analyses",
        parents=[common],
        help="List two-input mastering analysis names",
    )
    mpa_p = sub.add_parser(
        "mastering-pair-analyze",
        parents=[common],
        help="Run a two-input mastering analysis (always JSON output)",
    )
    mpa_p.add_argument("--reference", required=True, help="Reference audio file")
    mpa_p.add_argument("--analysis", required=True, help="Analysis name")
    mchain_p = sub.add_parser(
        "mastering-chain", parents=[common], help="Run a configurable mastering chain"
    )
    mchain_p.add_argument("--config", default="", help="Chain config as a JSON object")
    mchain_p.add_argument("--config-file", default="", help="Chain config JSON file")
    mchain_p.add_argument("--params", default="", help="Flat params as k=v,k=v (floats)")
    master_p = sub.add_parser("master", parents=[common], help="Apply a named mastering preset")
    master_p.add_argument("--preset", default="pop", help="Mastering preset name")
    master_p.add_argument("--config", default="", help="Preset overrides as a JSON object")
    master_p.add_argument("--config-file", default="", help="Preset override JSON file")
    master_p.add_argument("--params", default="", help="Flat overrides as k=v,k=v (floats)")
    mstream_p = sub.add_parser(
        "mastering-streaming",
        parents=[common],
        help="Preview streaming-platform normalization as JSON",
    )
    mstream_p.add_argument(
        "--platforms",
        default="",
        help="Platform targets as JSON array of {name,targetLufs,ceilingDb}",
    )
    mstream_p.add_argument("--platforms-file", default="", help="Platform targets JSON file")
    declip_p = sub.add_parser("declip", parents=[common], help="Repair clipped audio")
    declip_p.add_argument("--clip-threshold", type=float, default=0.98)
    declip_p.add_argument("--lpc-order", type=int, default=36)
    declip_p.add_argument("--iterations", type=int, default=2)
    declip_p.add_argument("--lpc-blend", type=float, default=0.65)
    sub.add_parser("mastering-presets", parents=[common], help="List mastering preset names")
    msuggest_p = sub.add_parser(
        "mastering-suggest", parents=[common], help="Suggest a mastering chain as JSON"
    )
    msuggest_p.add_argument("--params", default="", help="Assistant params as k=v,k=v")
    mprofile_p = sub.add_parser(
        "mastering-profile", parents=[common], help="Analyze a mastering audio profile as JSON"
    )
    mprofile_p.add_argument("--params", default="", help="Profile params as k=v,k=v")

    # Project / MIDI commands
    project_p = sub.add_parser("project", parents=[common], help="Headless project / SMF commands")
    project_sub = project_p.add_subparsers(dest="project_command", required=True)
    # The project-level parser owns the common defaults.  Child parsers must
    # accept the same flags without installing their own defaults, otherwise a
    # value before the project subcommand (for example `project --json abi`)
    # is overwritten by the child parser's false/empty default.
    project_common = argparse.ArgumentParser(add_help=False, argument_default=argparse.SUPPRESS)
    project_common.add_argument("--json", action="store_true")
    project_common.add_argument("-o", "--output", type=str)

    project_sub.add_parser("abi", parents=[project_common], help="Print the project ABI version")
    pnew = project_sub.add_parser(
        "new", parents=[project_common], help="Create an empty project JSON"
    )
    pnew.add_argument("--sample-rate", type=int, default=0, help="Project sample rate")
    for pname in ("validate", "compile"):
        pp = project_sub.add_parser(pname, parents=[project_common], help=f"Project {pname}")
        pp.add_argument("--in", dest="input", required=True, help="Input project JSON")
        if pname == "validate":
            pp.add_argument(
                "--strict",
                action="store_true",
                help="Fail (non-zero exit) when the project loads with diagnostics",
            )
    sf2_cli_note = (
        "SF2 / SoundFont and per-destination synth JSON are not wired through this CLI command; "
        "use the Project API for SoundFont-backed bounces."
    )
    pbounce = project_sub.add_parser(
        "bounce",
        parents=[project_common],
        help="Render project to WAV",
        description=sf2_cli_note,
    )
    pbounce.add_argument("--in", dest="input", required=True, help="Input project JSON")
    pbounce.add_argument("--sample-rate", type=int, default=48000, help="Render sample rate")
    pbounce.add_argument("--frames", type=int, default=0, help="Render length in frames")
    pbounce.add_argument("--block-size", type=int, default=0, help="Render block size")
    pbounce.add_argument("--channels", type=int, default=2, help="Render channel count")
    pbounce.add_argument("--instrument-latency", type=int, default=0)
    pbounce.add_argument(
        "--synth",
        nargs="?",
        const="",
        default=None,
        help=(
            "Render MIDI via NativeSynth preset (default patch when value is omitted); "
            "no --sf2 or --synth-json CLI wiring"
        ),
    )
    pexport_smf = project_sub.add_parser("export-smf", parents=[project_common], help="Export SMF")
    pexport_smf.add_argument("--in", dest="input", required=True, help="Input project JSON")
    pimport_smf = project_sub.add_parser("import-smf", parents=[project_common], help="Import SMF")
    pimport_smf.add_argument("--smf", required=True, help="Input Standard MIDI File")
    pexport_midi2 = project_sub.add_parser(
        "export-midi2", parents=[project_common], help="Export MIDI 2.0 Clip File"
    )
    pexport_midi2.add_argument("--in", dest="input", required=True, help="Input project JSON")
    pimport_midi2 = project_sub.add_parser(
        "import-midi2", parents=[project_common], help="Import MIDI 2.0 Clip File"
    )
    pimport_midi2.add_argument("--midi2", required=True, help="Input MIDI 2.0 Clip File")
    project_sub.add_parser(
        "synth-presets", parents=[project_common], help="List NativeSynth presets"
    )

    midi_render_p = sub.add_parser(
        "midi-render",
        parents=[common],
        help="Render a MIDI project through NativeSynth",
        description=sf2_cli_note,
    )
    midi_render_p.add_argument("--in", dest="input", required=True, help="Input project JSON")
    midi_render_p.add_argument("--sample-rate", type=int, default=48000, help="Render sample rate")
    midi_render_p.add_argument("--frames", type=int, default=0, help="Render length in frames")
    midi_render_p.add_argument("--block-size", type=int, default=0, help="Render block size")
    midi_render_p.add_argument("--channels", type=int, default=2, help="Render channel count")
    midi_render_p.add_argument("--instrument-latency", type=int, default=0)
    midi_render_p.add_argument(
        "--synth", default="", help="NativeSynth preset; no --sf2 or --synth-json CLI wiring"
    )

    # Mixing commands
    sub.add_parser("mixing-presets", parents=[common], help="List built-in mixer scene presets")
    mixing_preset_p = sub.add_parser(
        "mixing-preset", parents=[common], help="Print a built-in mixer scene preset"
    )
    mixing_preset_p.add_argument(
        "--preset", default="vocalReverbSend", help="Built-in scene preset name"
    )

    mix_p = sub.add_parser(
        "mix",
        parents=[common],
        help="Load a mixer scene (JSON file or preset) and optionally render inputs",
        description=(
            "Input WAVs are loaded as mono (stereo files are downmixed) and each "
            "mono input is duplicated across both output channels. Inputs at a "
            "different sample rate are resampled to --sample-rate before mixing."
        ),
    )
    mix_group = mix_p.add_mutually_exclusive_group(required=True)
    mix_group.add_argument("--scene", default="", help="Path to a scene JSON file")
    mix_group.add_argument("--preset", default="", help="Built-in scene preset name")
    mix_p.add_argument(
        "--input",
        action="append",
        default=[],
        metavar="WAV",
        help=(
            "Per-strip input WAV (repeat once per strip); loaded as mono and "
            "resampled to --sample-rate; requires --output to render"
        ),
    )
    mix_p.add_argument(
        "--sample-rate", type=int, default=48000, help="Mixer sample rate (default: 48000)"
    )
    mix_p.add_argument(
        "--block-size", type=int, default=512, help="Mixer max block size (default: 512)"
    )

    # Add file argument to all subcommands that need it
    for name in [
        "info",
        "bpm",
        "key",
        "beats",
        "downbeats",
        "onsets",
        "chords",
        "analyze",
        "mel",
        "chroma",
        "spectral",
        "pitch",
        "hpss",
        "pitch-correct",
        "pitch-correct-timevarying",
        "note-move",
        "note-stretch",
        "pitch-shift",
        "time-stretch",
        "normalize",
        "trim-silence",
        "resample",
        "voice-change",
        "acoustic",
        "estimate-room",
        "room-morph",
        "rhythm",
        "dynamics",
        "timbre",
        "lufs",
        "onset-envelope",
        "nnls-chroma",
        "tempogram",
        "plp",
        "mastering",
        "eq",
        "mastering-processor",
        "mastering-pair-analyze",
        "mastering-chain",
        "master",
        "mastering-streaming",
        "declip",
        "mastering-suggest",
        "mastering-profile",
    ]:
        sub.choices[name].add_argument("file", help="Audio file path")

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1 if _legacy_exit_codes() else EXIT_USAGE)

    commands = {
        "version": cmd_version,
        "info": cmd_info,
        "bpm": cmd_bpm,
        "key": cmd_key,
        "beats": cmd_beats,
        "downbeats": cmd_downbeats,
        "onsets": cmd_onsets,
        "chords": cmd_chords,
        "analyze": cmd_analyze,
        "mel": cmd_mel,
        "chroma": cmd_chroma,
        "spectral": cmd_spectral,
        "pitch": cmd_pitch,
        "hpss": cmd_hpss,
        "pitch-correct": cmd_pitch_correct,
        "pitch-correct-timevarying": cmd_pitch_correct_timevarying,
        "note-move": cmd_note_move,
        "scale-quantize": cmd_scale_quantize,
        "note-stretch": cmd_note_stretch,
        "pitch-shift": cmd_pitch_shift,
        "time-stretch": cmd_time_stretch,
        "normalize": cmd_normalize,
        "trim-silence": cmd_trim_silence,
        "resample": cmd_resample,
        "voice-change": cmd_voice_change,
        "voice-presets": cmd_voice_presets,
        "voice-preset": cmd_voice_preset,
        "voice-preset-validate": cmd_voice_preset_validate,
        "acoustic": cmd_acoustic,
        "estimate-room": cmd_estimate_room,
        "synthesize-rir": cmd_synthesize_rir,
        "room-morph": cmd_room_morph,
        "rhythm": cmd_rhythm,
        "dynamics": cmd_dynamics,
        "timbre": cmd_timbre,
        "lufs": cmd_lufs,
        "onset-envelope": cmd_onset_envelope,
        "nnls-chroma": cmd_nnls_chroma,
        "tempogram": cmd_tempogram,
        "plp": cmd_plp,
        "mastering": cmd_mastering,
        "eq": cmd_eq,
        "mastering-processor": cmd_mastering_processor,
        "mastering-processors": cmd_mastering_processors,
        "mastering-pair-processors": cmd_mastering_pair_processors,
        "mastering-pair-analyses": cmd_mastering_pair_analyses,
        "mastering-pair-analyze": cmd_mastering_pair_analyze,
        "mastering-chain": cmd_mastering_chain,
        "master": cmd_master,
        "mastering-streaming": cmd_mastering_streaming,
        "declip": cmd_declip,
        "mastering-presets": cmd_mastering_presets,
        "mastering-suggest": cmd_mastering_suggest,
        "mastering-profile": cmd_mastering_profile,
        "project": cmd_project,
        "midi-render": cmd_midi_render,
        "mixing-presets": cmd_mixing_presets,
        "mixing-preset": cmd_mixing_preset,
        "mix": cmd_mix,
    }

    handler = commands.get(args.command)
    if not handler:
        print(f"Unknown command: {args.command}", file=sys.stderr)
        sys.exit(1 if _legacy_exit_codes() else EXIT_USAGE)

    try:
        # `common` supplies -o to every parser for a uniform CLI shape, but an
        # analysis result has no audio artifact to write.  Rejecting it here
        # prevents a successful-looking invocation from silently discarding a
        # requested destination.
        output_capable_commands = {
            "hpss",
            "pitch-correct",
            "pitch-correct-timevarying",
            "note-move",
            "note-stretch",
            "pitch-shift",
            "time-stretch",
            "normalize",
            "trim-silence",
            "resample",
            "voice-change",
            "synthesize-rir",
            "room-morph",
            "mastering",
            "eq",
            "mastering-processor",
            "mastering-chain",
            "master",
            "declip",
            "midi-render",
            "mix",
            "project",
        }
        if getattr(args, "output", "") and args.command not in output_capable_commands:
            raise ValueError(f"{args.command} does not produce an audio file; remove --output")
        sys.exit(handler(args))
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(_exit_code_for(e))


_rebind_facade_exports(globals(), "libsonare._cli_", "libsonare.cli")
del _rebind_facade_exports


if __name__ == "__main__":
    main()
