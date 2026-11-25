"""Built-in synth, SoundFont and external-instrument configuration for projects."""

from __future__ import annotations

import ctypes
import json
from collections.abc import Mapping
from dataclasses import dataclass, replace
from typing import Any, Generic, Literal, Protocol, TypeAlias, TypeVar, cast, get_args

import numpy as np

from ._errors import _entry, _malformed_output, _refusals_under, _refuse, _refuse_type
from ._ffi_types_mastering_project import (
    SONARE_SYNTH_FIELD2_AMP_DELAY_MS,
    SONARE_SYNTH_FIELD2_AMP_HOLD_MS,
    SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ,
    SONARE_SYNTH_FIELD2_FILTER_DELAY_MS,
    SONARE_SYNTH_FIELD2_FILTER_HOLD_MS,
    SONARE_SYNTH_FIELD2_ONE_SHOT,
    SONARE_SYNTH_FIELD_AMP_ATTACK_MS,
    SONARE_SYNTH_FIELD_AMP_DECAY_MS,
    SONARE_SYNTH_FIELD_AMP_RELEASE_MS,
    SONARE_SYNTH_FIELD_AMP_SUSTAIN,
    SONARE_SYNTH_FIELD_BIT_DEPTH,
    SONARE_SYNTH_FIELD_BODY_MIX,
    SONARE_SYNTH_FIELD_BUS_DRIVE,
    SONARE_SYNTH_FIELD_CUTOFF_HZ,
    SONARE_SYNTH_FIELD_DETUNE_CENTS,
    SONARE_SYNTH_FIELD_DRIFT_CENTS,
    SONARE_SYNTH_FIELD_DRIVE,
    SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS,
    SONARE_SYNTH_FIELD_FILTER_ATTACK_MS,
    SONARE_SYNTH_FIELD_FILTER_DECAY_MS,
    SONARE_SYNTH_FIELD_FILTER_RELEASE_MS,
    SONARE_SYNTH_FIELD_FILTER_SUSTAIN,
    SONARE_SYNTH_FIELD_GAIN,
    SONARE_SYNTH_FIELD_GLIDE_MS,
    SONARE_SYNTH_FIELD_HP_CUTOFF_HZ,
    SONARE_SYNTH_FIELD_KEY_TRACK,
    SONARE_SYNTH_FIELD_LFO2_RATE_HZ,
    SONARE_SYNTH_FIELD_LFO_RATE_HZ,
    SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS,
    SONARE_SYNTH_FIELD_MOD_ROUTINGS,
    SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS,
    SONARE_SYNTH_FIELD_POLYPHONY,
    SONARE_SYNTH_FIELD_RESONANCE_Q,
    SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ,
    SONARE_SYNTH_FIELD_STEREO_SPREAD,
    SONARE_SYNTH_FIELD_UNISON,
    SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS,
    SONARE_SYNTH_PATCH_MOD_ROUTINGS,
    SONARE_SYNTH_PRESET_NAME_MAX,
)
from ._narrowing import _narrow_double, _narrow_int
from ._project_synth import (
    _BUILTIN_SYNTH_WAVEFORMS,
    _SAMPLE_KEY_TRACKS,
    _SAMPLE_LOOP_MODES,
    _SYNTH_BODY_TYPES,
    _SYNTH_ENGINE_MODES,
    _SYNTH_FILTER_MODELS,
    _SYNTH_FILTER_OUTPUTS,
    _SYNTH_LFO_SHAPES,
    _SYNTH_MOD_DESTINATIONS,
    _SYNTH_MOD_SOURCES,
    _SYNTH_OSC_WAVEFORMS,
    _SYNTH_RETRIGGERS,
    _sample_key_track_value,
    _sample_loop_value,
    _strip_va_prefix,
    _synth_enum_name,
    _synth_enum_value,
)
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    SampleBank as SampleBank,
)
from ._project_synth import (
    SampleKeyTrack as SampleKeyTrack,
)
from ._project_synth import (
    SampleLoopMode as SampleLoopMode,
)
from ._project_synth import (
    SynthBodyType as SynthBodyType,
)
from ._project_synth import (
    SynthEngineMode as SynthEngineMode,
)
from ._project_synth import (
    SynthFilterModel as SynthFilterModel,
)
from ._project_synth import (
    SynthFilterOutput as SynthFilterOutput,
)
from ._project_synth import (
    SynthLfoShape as SynthLfoShape,
)
from ._project_synth import (
    SynthModDestination as SynthModDestination,
)
from ._project_synth import (
    SynthModSource as SynthModSource,
)
from ._project_synth import (
    SynthOscWaveform as SynthOscWaveform,
)
from ._project_synth import (
    SynthRetrigger as SynthRetrigger,
)
from ._project_synth import (
    SynthWaveform as SynthWaveform,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    ErrorCode,
    SonareBuiltinSynthConfig,
    SonareError,
    SonareInstrumentCallbacks,
    SonareInstrumentOnEventCallback,
    SonareInstrumentPrepareCallback,
    SonareInstrumentRenderCallback,
    SonareProjectInstrument,
    SonareProjectInstrumentBinding,
    SonareSf2InstrumentConfig,
    SonareSynthEngineParam,
    SonareSynthModRouting,
    SonareSynthPatch,
    _call_string,
    _check,
    _get_lib,
    _joined_names,
    _to_c_int,
    _utf8_arg,
)
from ._types_capabilities import (
    SynthParamInfo,
    SynthPatchEnumParamInfo,
    SynthPatchParamInfo,
    _from_wire,
)

GsEfxRealization: TypeAlias = Literal["modern", "classic"]
"""Which GS insertion-effect realisation an instrument uses."""


@dataclass(frozen=True)
class BuiltinSynthConfig:
    """Patch for the built-in polyphonic oscillator synth (see
    :meth:`Project.bounce`).

    Every numeric field is ``None`` for the library value, so a
    default-constructed config is the default sine patch; override only what
    you need. A given value is read literally and refused outside its range
    (``gain`` in (0, 4], ``attack_ms``, ``decay_ms`` and ``release_ms`` in
    (0, 20000], ``sustain`` in (0, 1], ``polyphony`` in [1, 64]) with
    :class:`SonareValueError`. ``waveform`` is ``"sine"``, ``"saw"`` (also spelled ``"sawtooth"``),
    ``"square"`` or ``"triangle"``. It is the one field with no "nearest
    sensible value": another string raises :class:`SonareValueError` and a
    number raises :class:`TypeError` rather than falling back to sine.

    ``attack_ms`` is a linear ramp that reaches full level at the set time;
    ``decay_ms`` and ``release_ms`` are linear too, the set time being a full 0
    to 1 sweep (they take proportionally less from a lower level).
    """

    waveform: SynthWaveform = "sine"
    """Oscillator waveform, a ``SynthWaveform`` token."""
    gain: float | None = None
    """Master output gain (linear), in (0, 4]; None selects the library value (0.2)."""
    attack_ms: float | None = None
    """ADSR attack in ms, in (0, 20000]; None selects the library value (5)."""
    decay_ms: float | None = None
    """ADSR decay in ms, in (0, 20000]; None selects the library value (60)."""
    sustain: float | None = None
    """ADSR sustain level, in (0, 1]; None selects the library value (0.7)."""
    release_ms: float | None = None
    """ADSR release in ms, in (0, 20000]; None selects the library value (120)."""
    polyphony: int | None = None
    """Maximum simultaneous voices, in [1, 64]; None selects the library value (16)."""

    def _to_c(self) -> SonareBuiltinSynthConfig:
        # An omitted field keeps the value sonare_builtin_synth_config_init wrote.
        return SonareBuiltinSynthConfig._given(
            waveform=_synth_waveform_value(self.waveform, "waveform"),
            gain=self.gain,
            attack_ms=self.attack_ms,
            decay_ms=self.decay_ms,
            sustain=self.sustain,
            release_ms=self.release_ms,
            polyphony=self.polyphony,
        )


SourceBackend: TypeAlias = Literal["synth", "sf2"]
"""Backend a resolved MIDI program renders through."""
# Indexed by the C SonareSourceBackend ordinal.
_SOURCE_BACKENDS: tuple[str, ...] = get_args(SourceBackend)


@dataclass(frozen=True)
class Sf2InstrumentConfig:
    """Patch for the SoundFont (SF2) player (see
    :meth:`Project.bounce`).

    ``gain`` and ``polyphony`` are ``None`` for the library value; a given
    value is read literally and refused outside its range (``gain`` in [0, 4],
    ``polyphony`` in [1, 64]) with :class:`SonareValueError`.
    ``prefer_model_for_modeled_families`` defaults to false, retaining the
    established SoundFont-first behavior.

    ``clear_bank_rig`` removes the stage the bank binds after a voice — the
    amplifier and cabinet an electric guitar is never heard without. It defaults
    to false, so a MIDI file that selects a distorted guitar and asks for
    nothing else still comes out amplified. Set it to render the instrument
    alone, which is what a calibration fit measures against a direct reference.

    ``gs_efx_realization`` selects how the GS insertion effects are realised:
    ``"modern"`` (the default) or ``"classic"``. Any other value raises
    :class:`SonareValueError`.
    """

    gain: float | None = None
    """Master output gain (linear), in [0, 4]; None selects the library value (0.5)."""
    polyphony: int | None = None
    """Maximum simultaneous voices, in [1, 64]; None selects the library value (48)."""
    prefer_model_for_modeled_families: bool = False
    """True selects the dedicated melodic models; drums stay SF2-first."""
    clear_bank_rig: bool = False
    """True renders the instrument without the rig the bank binds after its voice."""
    gs_efx_realization: GsEfxRealization = "modern"
    """How GS insertion effects are realised, a ``GsEfxRealization`` token."""

    def _to_c(self) -> SonareSf2InstrumentConfig:
        return SonareSf2InstrumentConfig._given(
            gain=self.gain,
            polyphony=self.polyphony,
            prefer_model_for_modeled_families=int(self.prefer_model_for_modeled_families),
            clear_bank_rig=int(self.clear_bank_rig),
            gs_efx_realization=_gs_efx_realization_value(self.gs_efx_realization),
        )


_GS_EFX_REALIZATIONS = {"modern": 0, "classic": 1}


def _gs_efx_realization_value(name: str) -> int:
    if not isinstance(name, str) or name not in _GS_EFX_REALIZATIONS:
        raise _refuse(f"gs_efx_realization must be one of {', '.join(_GS_EFX_REALIZATIONS)}")
    return _GS_EFX_REALIZATIONS[name]


@dataclass(frozen=True)
class Sf2ProgramStatus:
    """One :meth:`Project.sound_font_manifest` entry: a (channel, bank, program)
    combination the arrangement plays, with the backend it resolves to
    (a :data:`SourceBackend` token) and the
    resolved SF2 preset name (GS fallback included; empty for the synth
    fallback)."""

    channel: int
    """MIDI channel, 0-15."""
    bank: int
    """Effective SF2 bank; 128 for a drum channel."""
    program: int
    """Program number, 0-127."""
    backend: SourceBackend
    """Backend the program resolves to, a :data:`SourceBackend` token."""
    preset_name: str
    """Resolved SF2 preset name; empty for the synth fallback."""


def _synth_waveform_value(waveform: SynthWaveform, key: str) -> int:
    """Resolve a built-in synth waveform token to its C ordinal."""
    return _synth_enum_value(waveform, _BUILTIN_SYNTH_WAVEFORMS, key)


@dataclass(frozen=True)
class SynthModRouting:
    """One NativeSynth mod-matrix routing (see :class:`SynthPatch`).

    ``source`` / ``destination`` accept the ordinal or a name (sources:
    ``"ampEnv"`` / ``"filterEnv"`` / ``"lfo1"`` / ``"lfo2"`` / ``"velocity"``
    / ``"keyTrack"`` / ``"modWheel"`` / ``"random"`` / ``"breath"`` /
    ``"aftertouch"`` / ``"expressionCc"`` / ``"pitchBend"``; destinations:
    ``"pitchCents"`` / ``"cutoffCents"`` / ``"ampGain"`` / ``"panUnits"`` /
    ``"resonanceQ"`` / ``"vibratoDepthCents"`` / ``"filterEnvDepth"`` /
    ``"lfo1RateScale"`` / ``"excitationForce"`` / ``"excitationPosition"`` /
    ``"excitationBrightness"`` / ``"spectrumMorph"``). ``"none"`` is refused
    for either field: a routing driven by or aimed at nothing is a caller
    mistake, not an empty slot.

    ``depth`` is destination units at full source deflection, summed onto
    whatever the patch or a CC already set and then handled per destination:

    - ``"panUnits"``: SF2 pan units, clamped to ``[-500, 500]`` -- ``1`` moves
      the image by about 0.2% of a side.
    - ``"ampGain"`` / ``"filterEnvDepth"``: multiplicative. Each routing's
      amount composes onto a running multiplier clamped to ``[0, 4]``.
      ``"filterEnvDepth"`` has nothing to scale while the patch's
      ``env_to_cutoff_cents`` is ``0``.
    - ``"resonanceQ"``: added to the patch's own Q, then floored at a fixed
      ``0.5`` (not at that patch's own Q).
    - ``"lfo1RateScale"``: multiplicative, clamped to ``[1/16, 16]``. It
      retunes LFO1 rather than sounding on its own, so it is audible only once
      something else already reads LFO1's output (vibrato depth, the filter's
      LFO amount, tremolo).
    - The three ``excitation-*`` destinations and ``"spectrumMorph"``: an
      offset in the engine's own normalized ``[0, 1]`` units -- the same scale
      the live-control CCs drive -- clamped to ``[-1, 1]``. Reach is per engine
      and not uniform across the three: ``"excitationForce"`` reaches pipe
      organ, bowed string, reed, brass, flute and free reed;
      ``"excitationPosition"`` reaches bowed string only;
      ``"excitationBrightness"`` reaches pipe organ, reed, brass, flute, free
      reed and vocal. An engine outside a destination's list ignores it -- its
      exciter is finished before the second sample renders, or it has none.
      ``"spectrumMorph"`` travels between the two spectral tables an additive
      patch carries; a patch needs an explicit ``drawbars_b`` distinct from
      ``drawbars`` to have a second table to travel to, and no catalog preset
      ships one, so the destination is currently unreachable from a preset name
      alone.

    Percussion reaches none of the above: a drum channel's voice runs the
    per-note kit patch instead of this one, so the whole mod matrix is
    discarded before the voice starts.
    """

    source: SynthModSource
    """Modulation source, a ``SynthModSource`` name or ordinal; ``"none"`` is refused."""
    destination: SynthModDestination
    depth: float
    """Destination units at full source deflection; its per-destination handling is above."""


@dataclass(frozen=True)
class SynthPatch:
    """Versioned NativeSynth patch (see
    :meth:`Project.bounce` and
    :meth:`RealtimeEngine.set_synth_instrument`).

    The patch starts from a BASE — the named ``preset`` (see
    :func:`synth_preset_names`; a ``"va:"`` routing prefix is accepted) or,
    when ``preset`` is ``None``, the default subtractive patch. Every numeric
    field defaults to ``None``, meaning "keep the base value"; any value given
    overrides it, including an explicit ``0``. A value that is non-finite or
    outside the range :func:`synth_patch_param_info` publishes for its field is
    refused with a :class:`SonareValueError` naming the field, and so are more than 8
    mod routings. Enum fields accept the C ordinal or a name with ``"default"``
    = keep. A
    ``mod_routings`` tuple REPLACES the base mod matrix, and an empty tuple
    clears it, while ``None`` keeps it.

    Every engine mode except ``"sample"`` is audible from ``engine_mode`` alone:
    the engine section is the named preset's when that preset uses the selected
    engine, else the engine's base preset's, so the bare ``"fm"``, ``"modal"``,
    ``"percussion"`` and ``"karplusStrong"`` modes voice their base preset
    (``"percussion"`` is the GM drum kit) and follow that preset's bank version.
    ``"sample"`` without a bank stays silent. The fields are the wrapper
    sections; which engines honour each one:

    - subtractive only: ``waveform``, ``unison``, ``detune_cents``.
    - sample only: the ``sample_*`` fields.
    - every engine: ``pitch_offset_cents``, the LFOs, ``drift_cents`` /
      ``drift_rate_hz``, ``glide_ms``, the filter and its envelope, the
      amplitude envelope (an undamped harpsichord tail outlasts it),
      ``one_shot``, ``body`` / ``body_mix``, ``drive``, ``hp_cutoff_hz``,
      ``sample_hold_hz`` / ``bit_depth``, ``stereo_spread``.
    - every engine and a percussion channel: ``gain``, ``polyphony``,
      ``bus_drive``. On a percussion channel (a percussion patch voicing a GM
      kit, as the ``"drumKit"`` preset does) each note runs its per-note kit
      patch and every other field is discarded.

    ``drift_rate_hz`` is the per-voice drift rate in Hz, ``[0.01, 20]``;
    ``amp_delay_ms`` / ``filter_delay_ms`` wait before each envelope's attack
    and ``amp_hold_ms`` / ``filter_hold_ms`` hold its peak, each ``[0, 5000]``
    ms. ``one_shot=True`` ignores note-off, so the voice ends with its
    envelope's decay or its engine's own end, as a drum hit does; it must be a
    ``bool``.

    ``engine_params`` sets fields of the selected engine's own section by public
    key (``"bowForce"``, ``"ops1.level"``; the keys and their ranges, units and
    defaults are :func:`synth_engine_param_info`). A key outside the selected
    engine's section, a non-finite value, a fractional value for an integer
    field or a value outside a bounded field's range is refused with a
    :class:`SonareValueError` naming the key; nothing is clamped. A patch read back
    from a preset carries ``None`` here.

    ``env_to_cutoff_cents`` is the filter envelope's cutoff depth, in cents at
    full envelope; ``0`` leaves the envelope's cutoff contribution off. The
    cutoff is multiplied by ``2 ** (envelope * env_to_cutoff_cents / 1200)``, so
    the response moves by exactly that interval wherever the corner sits well
    below Nyquist; near the top the ``"svf"`` model moves slightly less and the
    ladders slightly more. ``cutoff_hz`` is the -3 dB corner only for ``"svf"``
    at ``resonance_q`` 0.707: with resonance the SVF corner sits above it, and
    the ladder and Sallen-Key models put their resonant peak on it with the
    -3 dB point one to three octaves lower, so measure a depth as a ratio of
    corners rather than against ``cutoff_hz``.
    ``vel_to_cutoff_cents`` is velocity's cutoff depth, in cents: the rendered
    term is ``vel_to_cutoff_cents * (velocity / 127 - 1)``, so velocity 127 is
    the anchor where it is exactly zero -- a positive value darkens softer
    notes, a negative value brightens them, and both effects grow the further
    the velocity sits below 127.

    The ``sample_*`` fields are read only when the resolved engine is
    ``"sample"``, which is what lets ``sample_set`` keep its natural zero: a
    patch voicing another engine never consults them, so set 0 stays
    addressable. ``sample_loop`` overrides what the bank recorded
    (``"default"`` / ``"none"`` / ``"continuous"`` / ``"keyDown"``) and
    ``sample_key_track`` whether the sample follows the played key
    (``"default"`` / ``"on"`` / ``"off"``); both accept the C ordinal too.

    ``lfo_shape`` (``"default"`` / ``"triangle"`` / ``"sine"`` / ``"random"``,
    or the C ordinal) is the patch's LFO waveform (vibrato and tremolo LFOs),
    wherever either is read: vibrato depth, the GS LFO cutoff and amplitude
    depths, and the ``lfo1`` and ``lfo2`` mod sources. ``"triangle"`` is every catalog
    preset's shape; ``"sine"`` carries no harmonics, so a vibrato's or a
    tremolo's sidebands sit where a player's do. ``"random"`` glides to a new
    seeded value in [-1, 1] each cycle, so it wanders with no line at the rate;
    it is seeded per voice, so a bounce repeats it exactly.

    ``retrigger`` picks what seeds each voice's start state (``"default"`` /
    ``"free"`` / ``"note"``, or the C ordinal). ``"note"`` derives oscillator
    start phases, unison jitter, drift and every engine's noise stream from
    the note number, so the same note played again after its tail has ended
    renders the same samples; ``"free"`` (the built-in patches' choice) varies
    them per note. State outside the voice is reset by neither: controllers,
    the bus DC blocker, a piano's shared soundboard, a plucked string's
    sympathetic halo, an organ's wind chest and effect tails. The
    ``"drumKit"`` preset's per-note pieces keep their own mode.

    ``sample_bank`` carries the PCM those fields address. It is a BINDING
    option, not a patch field -- it travels on the C binding struct rather than
    inside the patch, so every bound destination has its own bank and two
    destinations may carry different ones. Node and WASM spell it
    ``sampleBank`` on the same object for the same reason. A sample patch bound
    without a bank renders silence rather than failing, the same way one naming
    a keymap set the bank lacks does.

    ``use_gm_programs`` is a BINDING option too (Node and WASM spell it
    ``useGmPrograms``): it makes this destination resolve each MIDI channel from
    its GM bank/program messages. ``None`` and ``False`` both leave it off.

    ``amp_attack_ms`` / ``filter_attack_ms`` reach full level at the set time.
    ``*_decay_ms`` and ``*_release_ms`` are three time constants: about -26 dB
    of the remaining distance at the set time.
    """

    preset: str | None = None
    """Catalog preset to start from (:func:`synth_preset_names`); ``None`` is the init patch."""
    engine_mode: SynthEngineMode = "default"
    """Engine that voices the patch, a ``SynthEngineMode`` token; ``"default"`` keeps the base."""
    waveform: SynthOscWaveform = "default"
    unison: int | None = None
    """Detuned-stack width in [1, 7]; subtractive engine only."""
    detune_cents: float | None = None
    """Unison spread in cents; subtractive engine only."""
    drift_cents: float | None = None
    """Per-voice slow pitch drift depth in cents."""
    drive: float | None = None
    """Pre-filter drive in [0, 1]."""
    filter_model: SynthFilterModel = "default"
    """Filter model, a ``SynthFilterModel`` token; ``"default"`` keeps the base."""
    filter_output: SynthFilterOutput = "default"
    """Filter output tap, a ``SynthFilterOutput`` token; ``"default"`` keeps the base."""
    cutoff_hz: float | None = None
    """Filter cutoff in Hz."""
    hp_cutoff_hz: float | None = None
    """Corner in Hz of the series 12 dB/oct highpass after the main filter; 0 switches it off."""
    sample_hold_hz: float | None = None
    """Rate in Hz the voice output is held at; 0 switches the stage off."""
    bit_depth: float | None = None
    """Word length in bits the held value is quantized to, [1, 24]; 0 switches it off."""
    pitch_offset_cents: float | None = None
    """Constant transposition of the voice's pitch in cents, [-4800, 4800]."""
    resonance_q: float | None = None
    """Main filter resonance Q."""
    key_track: float | None = None
    """Cutoff keyboard tracking in [0, 1]."""
    env_to_cutoff_cents: float | None = None
    """Filter envelope's cutoff depth in cents at full envelope; 0 leaves it off."""
    vel_to_cutoff_cents: float | None = None
    """Velocity's cutoff depth in cents; velocity 127 is the anchor where it is zero."""
    amp_attack_ms: float | None = None
    """Amplitude envelope attack in ms; full level is reached at the set time."""
    amp_decay_ms: float | None = None
    """Amplitude envelope decay in ms, three time constants."""
    amp_sustain: float | None = None
    """Amplitude envelope sustain level in [0, 1]."""
    amp_release_ms: float | None = None
    """Amplitude envelope release in ms, three time constants."""
    filter_attack_ms: float | None = None
    """Filter envelope attack in ms; full level is reached at the set time."""
    filter_decay_ms: float | None = None
    """Filter envelope decay in ms, three time constants."""
    filter_sustain: float | None = None
    """Filter envelope sustain level in [0, 1]."""
    filter_release_ms: float | None = None
    """Filter envelope release in ms, three time constants."""
    lfo_rate_hz: float | None = None
    """Vibrato LFO rate in Hz."""
    lfo_to_pitch_cents: float | None = None
    """Hardwired vibrato depth in cents."""
    lfo_shape: SynthLfoShape = "default"
    """LFO waveform, a ``SynthLfoShape`` token; ``"default"`` keeps the base."""
    lfo2_rate_hz: float | None = None
    """Matrix-routed LFO2 rate in Hz."""
    glide_ms: float | None = None
    """Portamento time in ms."""
    body: SynthBodyType = "default"
    """Body resonance type, a ``SynthBodyType`` token; ``"default"`` keeps the base."""
    body_mix: float | None = None
    """Body resonance mix in [0, 1]."""
    stereo_spread: float | None = None
    """Seeded per-voice pan scatter in [0, 1]."""
    retrigger: SynthRetrigger = "default"
    """What seeds each voice's start state, a ``SynthRetrigger`` token."""
    mod_routings: tuple[SynthModRouting, ...] | None = None
    """Mod matrix, at most 8 routings; replaces the base matrix, empty clears it."""
    gain: float | None = None
    """Master output gain (linear)."""
    polyphony: int | None = None
    """Maximum simultaneous voices, [1, 64]."""
    bus_drive: float | None = None
    """Gain-neutral bus saturation in [0, 1]."""
    sample_set: int | None = None
    """Keymap set in the bound sample bank; negative selects none. Sample engine only."""
    sample_level: float | None = None
    """Linear gain on the sample. Sample engine only."""
    sample_loop: SampleLoopMode = "default"
    """Loop mode override, a ``SampleLoopMode`` token; ``"default"`` keeps the bank's."""
    sample_start_offset: float | None = None
    """Attack skip as a fraction of the sampled region, [0, 1]. Sample engine only."""
    sample_key_track: SampleKeyTrack = "default"
    """Whether the sample follows the played key, a ``SampleKeyTrack`` token."""
    # A BINDING option rather than a NativeSynth patch field: it travels on
    # SonareSynthInstrumentBinding, not SonareSynthPatch, so _to_c() does not
    # carry it and _from_c() never fills it. It lives here because the C ABI
    # gives every binding its own bank, which a per-call argument cannot.
    sample_bank: SampleBank | None = None
    """PCM bank the ``sample_*`` fields address; a binding option, not a patch field."""
    # The same kind of BINDING option, spelled `useGmPrograms` by Node and
    # WASM: it travels on SonareSynthInstrumentBinding, so _to_c() does not
    # carry it and _from_c() never fills it.
    use_gm_programs: bool | None = None
    """Resolve each MIDI channel from GM bank/program messages; a binding option."""
    engine_params: Mapping[str, float] | None = None
    """Fields of the selected engine's own section by public key; write-only."""
    drift_rate_hz: float | None = None
    """Per-voice drift rate in Hz, [0.01, 20]."""
    amp_delay_ms: float | None = None
    """Amplitude envelope delay before the attack in ms, [0, 5000]."""
    amp_hold_ms: float | None = None
    """Amplitude envelope hold at the peak in ms, [0, 5000]."""
    filter_delay_ms: float | None = None
    """Filter envelope delay before the attack in ms, [0, 5000]."""
    filter_hold_ms: float | None = None
    """Filter envelope hold at the peak in ms, [0, 5000]."""
    one_shot: bool | None = None
    """True ignores note-off, so the voice ends with its envelope's decay or the engine's end."""

    def _to_c(self) -> SonareSynthPatch:
        preset = b""
        if self.preset is not None:
            if not isinstance(self.preset, str):
                raise _refuse_type("synth patch preset must be a string or None")
            preset = _utf8_arg(_strip_va_prefix(self.preset), "preset")
            if not preset:
                raise _refuse("synth patch preset must not be empty; omit it for the init patch")
            if len(preset) >= SONARE_SYNTH_PRESET_NAME_MAX:
                raise _refuse(
                    f"synth patch preset must be at most {SONARE_SYNTH_PRESET_NAME_MAX - 1} bytes"
                )
        c = SonareSynthPatch()

        # A field left at None keeps the base; anything supplied — including a
        # zero — is marked present so the core overrides with it.
        def _set_float(name: str, bit: int, value: float | None) -> None:
            if value is None:
                return
            # Unconverted, so the struct's _narrow_float refuses overflow and bool by name.
            setattr(c, name, value)
            c.present_fields |= bit

        def _set_int(name: str, bit: int, value: int | None) -> None:
            if value is None:
                return
            # Assigned unconverted so the struct's own narrowing sees the
            # caller's value; int() would truncate a fraction past it.
            setattr(c, name, value)
            c.present_fields |= bit

        def _set_float2(name: str, bit: int, value: float | None) -> None:
            if value is None:
                return
            setattr(c, name, value)
            c.present_fields2 |= bit

        c.preset = preset
        c.engine_mode = _synth_enum_value(self.engine_mode, _SYNTH_ENGINE_MODES, "engine_mode")
        c.waveform = _synth_enum_value(self.waveform, _SYNTH_OSC_WAVEFORMS, "waveform")
        _set_int("unison", SONARE_SYNTH_FIELD_UNISON, self.unison)
        _set_float("detune_cents", SONARE_SYNTH_FIELD_DETUNE_CENTS, self.detune_cents)
        _set_float("drift_cents", SONARE_SYNTH_FIELD_DRIFT_CENTS, self.drift_cents)
        _set_float("drive", SONARE_SYNTH_FIELD_DRIVE, self.drive)
        c.filter_model = _synth_enum_value(self.filter_model, _SYNTH_FILTER_MODELS, "filter_model")
        c.filter_output = _synth_enum_value(
            self.filter_output, _SYNTH_FILTER_OUTPUTS, "filter_output"
        )
        _set_float("cutoff_hz", SONARE_SYNTH_FIELD_CUTOFF_HZ, self.cutoff_hz)
        _set_float("hp_cutoff_hz", SONARE_SYNTH_FIELD_HP_CUTOFF_HZ, self.hp_cutoff_hz)
        _set_float("sample_hold_hz", SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ, self.sample_hold_hz)
        _set_float("bit_depth", SONARE_SYNTH_FIELD_BIT_DEPTH, self.bit_depth)
        _set_float(
            "pitch_offset_cents", SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS, self.pitch_offset_cents
        )
        _set_float("resonance_q", SONARE_SYNTH_FIELD_RESONANCE_Q, self.resonance_q)
        _set_float("key_track", SONARE_SYNTH_FIELD_KEY_TRACK, self.key_track)
        _set_float(
            "env_to_cutoff_cents",
            SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS,
            self.env_to_cutoff_cents,
        )
        _set_float(
            "vel_to_cutoff_cents",
            SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS,
            self.vel_to_cutoff_cents,
        )
        _set_float("amp_attack_ms", SONARE_SYNTH_FIELD_AMP_ATTACK_MS, self.amp_attack_ms)
        _set_float("amp_decay_ms", SONARE_SYNTH_FIELD_AMP_DECAY_MS, self.amp_decay_ms)
        _set_float("amp_sustain", SONARE_SYNTH_FIELD_AMP_SUSTAIN, self.amp_sustain)
        _set_float("amp_release_ms", SONARE_SYNTH_FIELD_AMP_RELEASE_MS, self.amp_release_ms)
        _set_float("filter_attack_ms", SONARE_SYNTH_FIELD_FILTER_ATTACK_MS, self.filter_attack_ms)
        _set_float("filter_decay_ms", SONARE_SYNTH_FIELD_FILTER_DECAY_MS, self.filter_decay_ms)
        _set_float("filter_sustain", SONARE_SYNTH_FIELD_FILTER_SUSTAIN, self.filter_sustain)
        _set_float(
            "filter_release_ms", SONARE_SYNTH_FIELD_FILTER_RELEASE_MS, self.filter_release_ms
        )
        _set_float("lfo_rate_hz", SONARE_SYNTH_FIELD_LFO_RATE_HZ, self.lfo_rate_hz)
        _set_float(
            "lfo_to_pitch_cents", SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS, self.lfo_to_pitch_cents
        )
        _set_float("lfo2_rate_hz", SONARE_SYNTH_FIELD_LFO2_RATE_HZ, self.lfo2_rate_hz)
        _set_float("glide_ms", SONARE_SYNTH_FIELD_GLIDE_MS, self.glide_ms)
        c.body = _synth_enum_value(self.body, _SYNTH_BODY_TYPES, "body")
        _set_float("body_mix", SONARE_SYNTH_FIELD_BODY_MIX, self.body_mix)
        _set_float("stereo_spread", SONARE_SYNTH_FIELD_STEREO_SPREAD, self.stereo_spread)
        c.retrigger = _synth_enum_value(self.retrigger, _SYNTH_RETRIGGERS, "retrigger")
        c.lfo_shape = _synth_enum_value(self.lfo_shape, _SYNTH_LFO_SHAPES, "lfo_shape")
        routings = list(self.mod_routings or ())
        if self.mod_routings is not None:
            c.present_fields |= SONARE_SYNTH_FIELD_MOD_ROUTINGS
        if len(routings) > SONARE_SYNTH_PATCH_MOD_ROUTINGS:
            raise _refuse(
                "synth patch mod_routings must contain at most "
                f"{SONARE_SYNTH_PATCH_MOD_ROUTINGS} entries"
            )
        c.num_mod_routings = len(routings)
        for i, routing in enumerate(routings):
            c.mod_routings[i] = SonareSynthModRouting(
                source=_synth_enum_value(
                    routing.source, _SYNTH_MOD_SOURCES, f"mod_routings[{i}].source"
                ),
                destination=_synth_enum_value(
                    routing.destination, _SYNTH_MOD_DESTINATIONS, f"mod_routings[{i}].destination"
                ),
                depth=_narrow_double(routing.depth, f"mod_routings[{i}].depth"),
            )
        _set_float("gain", SONARE_SYNTH_FIELD_GAIN, self.gain)
        _set_int("polyphony", SONARE_SYNTH_FIELD_POLYPHONY, self.polyphony)
        _set_float("bus_drive", SONARE_SYNTH_FIELD_BUS_DRIVE, self.bus_drive)
        _set_float2("drift_rate_hz", SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ, self.drift_rate_hz)
        _set_float2("amp_delay_ms", SONARE_SYNTH_FIELD2_AMP_DELAY_MS, self.amp_delay_ms)
        _set_float2("amp_hold_ms", SONARE_SYNTH_FIELD2_AMP_HOLD_MS, self.amp_hold_ms)
        _set_float2("filter_delay_ms", SONARE_SYNTH_FIELD2_FILTER_DELAY_MS, self.filter_delay_ms)
        _set_float2("filter_hold_ms", SONARE_SYNTH_FIELD2_FILTER_HOLD_MS, self.filter_hold_ms)
        if self.one_shot is not None:
            if not isinstance(self.one_shot, bool):
                raise _refuse_type("synth patch one_shot must be a bool")
            c.one_shot = 1 if self.one_shot else 0
            c.present_fields2 |= SONARE_SYNTH_FIELD2_ONE_SHOT
        # Sample engine: no presence bits, because only a sample patch reads the
        # block at all. That is what keeps set 0 addressable without one.
        c.sample_set = 0 if self.sample_set is None else self.sample_set
        c.sample_level = 0.0 if self.sample_level is None else float(self.sample_level)
        c.sample_loop = _sample_loop_value(self.sample_loop, "sample_loop")
        c.sample_start_offset = (
            0.0 if self.sample_start_offset is None else float(self.sample_start_offset)
        )
        c.sample_key_track = _sample_key_track_value(self.sample_key_track, "sample_key_track")
        if self.engine_params:
            # The struct borrows the key bytes and the array, and copying the
            # struct into a binding array copies bytes only, so the caller keeps
            # the returned object alive until the C call returns.
            keys: list[bytes] = []
            for key in self.engine_params:
                if not isinstance(key, str):
                    raise _refuse_type("engine_params keys must be strings")
                keys.append(_utf8_arg(key, "engine_params key"))
            params = (SonareSynthEngineParam * len(keys))()
            for i, (raw_key, (param_name, value)) in enumerate(
                zip(keys, self.engine_params.items(), strict=True)
            ):
                params[i].key = raw_key
                params[i].value = _narrow_double(value, f"engine_params.{param_name}")
            c.engine_params = ctypes.cast(params, ctypes.POINTER(SonareSynthEngineParam))
            c.engine_param_count = len(keys)
            c._keepalive = (keys, params)
        return c

    @classmethod
    def _from_c(cls, c: SonareSynthPatch) -> SynthPatch:
        routings = tuple(
            SynthModRouting(
                source=cast(SynthModSource, _synth_enum_name(int(r.source), _SYNTH_MOD_SOURCES)),
                destination=cast(
                    SynthModDestination,
                    _synth_enum_name(int(r.destination), _SYNTH_MOD_DESTINATIONS),
                ),
                depth=float(r.depth),
            )
            for r in c.mod_routings[: int(c.num_mod_routings)]
        )
        return cls(
            preset=c.preset.decode("utf-8", errors="replace") or None,
            engine_mode=cast(
                SynthEngineMode, _synth_enum_name(int(c.engine_mode), _SYNTH_ENGINE_MODES)
            ),
            waveform=cast(
                SynthOscWaveform, _synth_enum_name(int(c.waveform), _SYNTH_OSC_WAVEFORMS)
            ),
            unison=int(c.unison),
            detune_cents=float(c.detune_cents),
            drift_cents=float(c.drift_cents),
            drive=float(c.drive),
            filter_model=cast(
                SynthFilterModel, _synth_enum_name(int(c.filter_model), _SYNTH_FILTER_MODELS)
            ),
            filter_output=cast(
                SynthFilterOutput, _synth_enum_name(int(c.filter_output), _SYNTH_FILTER_OUTPUTS)
            ),
            cutoff_hz=float(c.cutoff_hz),
            hp_cutoff_hz=float(c.hp_cutoff_hz),
            sample_hold_hz=float(c.sample_hold_hz),
            bit_depth=float(c.bit_depth),
            pitch_offset_cents=float(c.pitch_offset_cents),
            resonance_q=float(c.resonance_q),
            key_track=float(c.key_track),
            env_to_cutoff_cents=float(c.env_to_cutoff_cents),
            vel_to_cutoff_cents=float(c.vel_to_cutoff_cents),
            amp_attack_ms=float(c.amp_attack_ms),
            amp_decay_ms=float(c.amp_decay_ms),
            amp_sustain=float(c.amp_sustain),
            amp_release_ms=float(c.amp_release_ms),
            filter_attack_ms=float(c.filter_attack_ms),
            filter_decay_ms=float(c.filter_decay_ms),
            filter_sustain=float(c.filter_sustain),
            filter_release_ms=float(c.filter_release_ms),
            lfo_rate_hz=float(c.lfo_rate_hz),
            lfo_to_pitch_cents=float(c.lfo_to_pitch_cents),
            lfo2_rate_hz=float(c.lfo2_rate_hz),
            glide_ms=float(c.glide_ms),
            body=cast(SynthBodyType, _synth_enum_name(int(c.body), _SYNTH_BODY_TYPES)),
            body_mix=float(c.body_mix),
            stereo_spread=float(c.stereo_spread),
            retrigger=cast(SynthRetrigger, _synth_enum_name(int(c.retrigger), _SYNTH_RETRIGGERS)),
            lfo_shape=cast(SynthLfoShape, _synth_enum_name(int(c.lfo_shape), _SYNTH_LFO_SHAPES)),
            mod_routings=routings,
            gain=float(c.gain),
            polyphony=int(c.polyphony),
            bus_drive=float(c.bus_drive),
            sample_set=int(c.sample_set),
            sample_level=float(c.sample_level),
            sample_loop=cast(
                SampleLoopMode, _synth_enum_name(int(c.sample_loop), _SAMPLE_LOOP_MODES)
            ),
            sample_start_offset=float(c.sample_start_offset),
            sample_key_track=cast(
                SampleKeyTrack, _synth_enum_name(int(c.sample_key_track), _SAMPLE_KEY_TRACKS)
            ),
            drift_rate_hz=float(c.drift_rate_hz),
            amp_delay_ms=float(c.amp_delay_ms),
            amp_hold_ms=float(c.amp_hold_ms),
            filter_delay_ms=float(c.filter_delay_ms),
            filter_hold_ms=float(c.filter_hold_ms),
            one_shot=bool(c.one_shot),
        )


def _synth_patch_arg(instrument: SynthPatch | str | None) -> SynthPatch:
    if instrument is None:
        return SynthPatch()
    if isinstance(instrument, str):
        return SynthPatch(preset=instrument)
    return instrument


@_entry("synth_preset_names")
def synth_preset_names() -> list[str]:
    """NativeSynth preset catalog names (``"sine"``, ``"sawLead"``,
    ``"ePiano"``, ``"drumKit"``, ...). Use these to discover valid
    :class:`SynthPatch` preset names instead of hardcoding magic strings."""
    lib = _get_lib()
    return _joined_names(lib.sonare_synth_preset_names(), "synth preset names")


def _synth_param_info(raw: str) -> list[SynthParamInfo]:
    return [_from_wire(SynthParamInfo, entry) for entry in json.loads(raw)]


@_entry("synth_engine_param_info")
def synth_engine_param_info(engine_mode: SynthEngineMode) -> list[SynthParamInfo]:
    """Describe every :attr:`SynthPatch.engine_params` key of ``engine_mode``.

    One descriptor per field of that engine's own section, in the layout of
    :func:`mastering_insert_param_info` plus an ``integer`` flag (present and
    true for a field taking whole numbers). ``min`` / ``max`` are present only
    for a field with a closed range, which a value outside is refused rather than
    clamped; ``default`` is the engine's base preset value. The ``"default"``
    and ``"sample"`` modes have no engine section and give an empty list. An
    unknown mode raises.
    """
    lib = _get_lib()
    mode = _synth_enum_value(engine_mode, _SYNTH_ENGINE_MODES, "engine_mode")
    return _synth_param_info(
        _call_string(lib.sonare_synth_engine_param_info, _to_c_int(mode, "engine_mode"))
    )


@_entry("synth_patch_param_info")
def synth_patch_param_info() -> list[SynthPatchParamInfo]:
    """Describe every numeric, boolean, and enum :class:`SynthPatch` wrapper field.

    Each numeric or boolean row follows :func:`synth_engine_param_info`.
    An enum row has ``type`` ``"enum"``, ``values`` in ordinal order, and ``default`` set to
    the ordinal-0 token. The preset name and mod matrix are not described.
    """
    lib = _get_lib()
    return _synth_patch_param_info(_call_string(lib.sonare_synth_patch_param_info))


@_entry("synth_gs_drum_kit_name")
def synth_gs_drum_kit_name(program: int) -> str | None:
    """GS rhythm-set name a rhythm part's ``program`` selects (``"Standard"``,
    ``"Room"``, ``"Jazz"``, ...), or ``None`` when the module's own tone map
    defines no set there.

    The answer is the module's own map, which is the newest one and reaches
    every set this build voices; a file selecting an older map reaches fewer.
    """
    lib = _get_lib()
    raw = lib.sonare_synth_gs_drum_kit_name(_to_c_int(program, "program"))
    return raw.decode("utf-8") if raw else None


@_entry("synth_gs_drum_kit_is_voiced_apart")
def synth_gs_drum_kit_is_voiced_apart(program: int) -> bool | None:
    """Whether the GS rhythm set at ``program`` is voiced apart from Standard:
    ``True`` when at least one drum note differs, ``False`` when the set renders
    exactly as Standard, ``None`` when no set sits at ``program``.

    Derived by applying the set to every note's resolved patch and comparing, so
    the answer follows the voicing rather than a list kept in step with it. Four
    sets GS fills with one-shots share the Standard voicing deliberately, so a
    host offering every set without this query offers four choices that change
    nothing.
    """
    lib = _get_lib()
    r = lib.sonare_synth_gs_drum_kit_is_voiced_apart(_to_c_int(program, "program"))
    return None if r < 0 else bool(r)


@_entry("synth_gs_variation_is_voiced_apart")
def synth_gs_variation_is_voiced_apart(bank: int, program: int) -> bool | None:
    """Whether melodic Bank Select ``bank`` on ``program`` is voiced apart from
    the capital tone: ``True`` when the bank has a patch of its own, ``False``
    when it resolves to the capital, ``None`` when either argument is outside
    ``[0, 127]`` -- both are seven-bit MIDI values, so ``128`` is out of range
    rather than the drum bank here.

    Resolving an unvoiced variation to its capital is what GS specifies, so a
    ``False`` is correct behaviour rather than a gap — but only this query
    separates it from a bank that is voiced, which otherwise takes rendering
    both and comparing. Accepts the GS Bank Select MSB and the GM2 LSB alike,
    since both address the same variation.
    """
    lib = _get_lib()
    r = lib.sonare_synth_gs_variation_is_voiced_apart(
        _to_c_int(bank, "bank"), _to_c_int(program, "program")
    )
    return None if r < 0 else bool(r)


@_entry("controller_profile_names")
def controller_profile_names() -> list[str]:
    """Controller-profile preset names (``"gm"``, ``"breath"``,
    ``"breathAftertouch"``, ``"mpe"``, ...). Use these to discover the names
    :meth:`RealtimeEngine.set_controller_profile` accepts instead of hardcoding
    magic strings."""
    lib = _get_lib()
    return _joined_names(lib.sonare_controller_profile_names(), "controller profile names")


@_entry("synth_preset_patch")
def synth_preset_patch(name: str) -> SynthPatch:
    """Fetch the named catalog preset as a :class:`SynthPatch` (the preset name
    plus the wrapper-section values), so hosts can inspect a preset and tweak
    fields before binding it. A ``"va:"`` routing prefix is accepted. Raises
    :class:`SonareValueError` for unknown names."""
    lib = _get_lib()
    out = SonareSynthPatch()
    _check(
        lib.sonare_synth_preset_patch(_utf8_arg(_strip_va_prefix(name), "name"), ctypes.byref(out))
    )
    return SynthPatch._from_c(out)


class ExternalInstrument(Protocol):
    """A host-supplied instrument driven during :meth:`Project.bounce`.

    The object models a MIDI instrument the bounce engine hosts in place of the
    built-in synth. Only :meth:`render` is required; ``prepare``, ``on_event``
    and a ``latency_samples`` attribute are optional (duck-typed). Every method
    is called synchronously on the thread that invokes the bounce, so no
    cross-thread state sharing is involved.

    Optional members::

        def prepare(self, sample_rate: float, max_block_size: int) -> None: ...
                def on_event(self, destination_id: int,
                             ump_words: tuple[int, ...], render_frame: int) -> None: ...
                latency_samples: int  # reported plugin-delay (PDC); defaults to 0
                tail_samples: int  # release/effect tail for auto-length bounce; defaults to 0
    """

    @_entry("ExternalInstrument.render")
    def render(self, channels: np.ndarray, num_frames: int) -> None:
        """Add ``num_frames`` of audio into the planar ``(channels, num_frames)``
        float32 array (already zero-filled). Sum into it; do not overwrite
        unrelated frames."""
        ...


def _make_instrument_callbacks(
    instrument: ExternalInstrument,
    errors: list[BaseException],
    keepalive: list[object],
) -> SonareInstrumentCallbacks:
    """Build the C callback table for one external instrument.

    The returned closures capture ``instrument`` directly (so ``user_data`` is
    unused) and record the first exception raised by any user callback into
    ``errors`` instead of propagating through the C frames (ctypes would only
    print it). The ``ctypes`` callback objects are appended to ``keepalive``;
    the caller MUST keep that list referenced for the whole bounce call, since
    copying the struct into the bindings array does not pin the closures.
    """
    cbs = SonareInstrumentCallbacks()
    cbs.user_data = None
    # Assigned unconverted so the struct's own narrowing sees the instrument's
    # value; int() would truncate a fractional latency past it.
    latency = getattr(instrument, "latency_samples", 0)
    cbs.latency_samples = latency if latency else 0
    tail = getattr(instrument, "tail_samples", 0)
    cbs.tail_samples = tail if tail else 0

    prepare = getattr(instrument, "prepare", None)
    if callable(prepare):

        def _prepare(_user, sample_rate, max_block_size, _fn=prepare):  # type: ignore[no-untyped-def]
            if errors:
                return
            try:
                _fn(float(sample_rate), int(max_block_size))
            except BaseException as exc:  # noqa: BLE001
                if not errors:
                    errors.append(exc)

        cb_prepare = SonareInstrumentPrepareCallback(_prepare)
        keepalive.append(cb_prepare)
        cbs.prepare = cb_prepare

    on_event = getattr(instrument, "on_event", None)
    if callable(on_event):

        def _on_event(_user, dest, words, count, frame, _fn=on_event):  # type: ignore[no-untyped-def]
            if errors:
                return
            try:
                ump = tuple(int(words[i]) for i in range(int(count)))
                _fn(int(dest), ump, int(frame))
            except BaseException as exc:  # noqa: BLE001
                if not errors:
                    errors.append(exc)

        cb_event = SonareInstrumentOnEventCallback(_on_event)
        keepalive.append(cb_event)
        cbs.on_event = cb_event

    render = instrument.render

    def _render(_user, channels, num_channels, num_frames, _fn=render):  # type: ignore[no-untyped-def]
        if errors:
            return
        try:
            nch = int(num_channels)
            nfr = int(num_frames)
            if nch <= 0 or nfr <= 0:
                return
            # The engine zero-fills the C scratch before this call; the
            # instrument sums into `block`, and we add `block` back into each
            # planar channel buffer (a view that shares the C memory).
            block = np.zeros((nch, nfr), dtype=np.float32)
            _fn(block, nfr)
            for ch in range(nch):
                dst = np.ctypeslib.as_array(channels[ch], shape=(nfr,))
                dst += block[ch]
        except BaseException as exc:  # noqa: BLE001
            if not errors:
                errors.append(exc)

    cb_render = SonareInstrumentRenderCallback(_render)
    keepalive.append(cb_render)
    cbs.render = cb_render
    return cbs


ProjectInstrumentKind: TypeAlias = Literal["synth", "builtin", "sf2"]
"""Kind of a persistent MIDI destination instrument."""

_PROJECT_INSTRUMENT_KINDS = {"synth": 0, "builtin": 1, "sf2": 2}
_PROJECT_INSTRUMENT_KIND_NAMES = tuple(_PROJECT_INSTRUMENT_KINDS)

_InstrumentConfig = TypeVar("_InstrumentConfig")


@dataclass(frozen=True, slots=True, kw_only=True)
class ProjectInstrumentBinding(Generic[_InstrumentConfig]):
    """An instrument supplied for one project bounce."""

    instrument: _InstrumentConfig
    """The instrument or configuration to render through."""
    destination_id: int | None = None
    """MIDI destination served by the instrument; ``None`` is destination 0, a real id."""


@dataclass(frozen=True, slots=True, kw_only=True)
class ProjectSf2InstrumentBinding(ProjectInstrumentBinding[Sf2InstrumentConfig]):
    """A SoundFont instrument supplied for one project bounce."""

    sound_font_uri: str | None = None
    """Loaded SoundFont URI, or ``None`` to use the default."""


@dataclass(frozen=True)
class ProjectInstrument:
    """Instrument and optional bank/program bound to one project destination."""

    kind: ProjectInstrumentKind
    """Instrument family: NativeSynth, the built-in synth, or SoundFont."""
    patch: SynthPatch | None = None
    """NativeSynth patch when ``kind`` is ``"synth"``."""
    config: BuiltinSynthConfig | Sf2InstrumentConfig | None = None
    """Configuration for ``"builtin"`` or ``"sf2"``."""
    sound_font_uri: str | None = None
    """SoundFont URI for an SF2 binding, or ``None`` for the default font."""
    use_gm_programs: bool | None = None
    """Whether NativeSynth follows incoming GM bank/program messages."""
    bank_msb: int | None = None
    """MIDI bank MSB, or ``None`` to follow each clip."""
    bank_lsb: int | None = None
    """MIDI bank LSB, or ``None`` to follow each clip."""
    program: int | None = None
    """MIDI program, or ``None`` to follow each clip."""

    def _to_c(self) -> SonareProjectInstrument:
        kind = _synth_enum_value(self.kind, _PROJECT_INSTRUMENT_KINDS, "instrument.kind")
        if self.patch is not None and kind != _PROJECT_INSTRUMENT_KINDS["synth"]:
            raise _refuse_type("instrument.patch is only valid for kind 'synth'")
        if self.config is not None and kind == _PROJECT_INSTRUMENT_KINDS["synth"]:
            raise _refuse_type("instrument.config is not valid for kind 'synth'")
        if self.sound_font_uri is not None and kind != _PROJECT_INSTRUMENT_KINDS["sf2"]:
            raise _refuse_type("instrument.sound_font_uri is only valid for kind 'sf2'")
        if self.use_gm_programs is not None and kind != _PROJECT_INSTRUMENT_KINDS["synth"]:
            raise _refuse_type("instrument.use_gm_programs is only valid for kind 'synth'")

        c = SonareProjectInstrument()
        c.kind = kind
        if self.bank_msb is not None:
            c.bank_msb = _narrow_int(self.bank_msb, "instrument.bank_msb", 0, 127)
        if self.bank_lsb is not None:
            c.bank_lsb = _narrow_int(self.bank_lsb, "instrument.bank_lsb", 0, 127)
        if self.program is not None:
            c.program = _narrow_int(self.program, "instrument.program", 0, 127)
        keepalive: list[object] = []

        if kind == _PROJECT_INSTRUMENT_KINDS["synth"]:
            if not isinstance(self.patch, SynthPatch):
                raise _refuse_type("instrument.patch must be a SynthPatch for kind 'synth'")
            if self.patch.sample_bank is not None:
                error = SonareError(
                    ErrorCode.NOT_SUPPORTED,
                    "instrument.patch.sample_bank cannot be persisted",
                )
                error.reason = "notPersistable"
                raise error
            use_gm_programs = self.use_gm_programs
            if use_gm_programs is not None and not isinstance(use_gm_programs, bool):
                raise _refuse_type("instrument.use_gm_programs must be a bool")
            if self.patch.use_gm_programs is not None:
                if not isinstance(self.patch.use_gm_programs, bool):
                    raise _refuse_type("instrument.patch.use_gm_programs must be a bool")
                if use_gm_programs is not None and use_gm_programs != self.patch.use_gm_programs:
                    raise _refuse(
                        "instrument.use_gm_programs conflicts with instrument.patch.use_gm_programs"
                    )
                use_gm_programs = self.patch.use_gm_programs
            with _refusals_under("instrument.patch"):
                patch = self.patch._to_c()
            c.synth = ctypes.pointer(patch)
            keepalive.append(patch)
            if hasattr(patch, "_keepalive"):
                keepalive.append(patch._keepalive)
            if use_gm_programs is not None:
                c.use_gm_programs = int(use_gm_programs)
        elif kind == _PROJECT_INSTRUMENT_KINDS["builtin"]:
            if not isinstance(self.config, BuiltinSynthConfig):
                raise _refuse_type(
                    "instrument.config must be a BuiltinSynthConfig for kind 'builtin'"
                )
            with _refusals_under("instrument.config"):
                builtin_config = self.config._to_c()
            c.builtin = ctypes.pointer(builtin_config)
            keepalive.append(builtin_config)
        else:
            if not isinstance(self.config, Sf2InstrumentConfig):
                raise _refuse_type(
                    "instrument.config must be an Sf2InstrumentConfig for kind 'sf2'"
                )
            for field in ("prefer_model_for_modeled_families", "clear_bank_rig"):
                if not isinstance(getattr(self.config, field), bool):
                    raise _refuse_type(f"instrument.config.{field} must be a bool")
            with _refusals_under("instrument.config"):
                sf2_config = self.config._to_c()
            c.sf2 = ctypes.pointer(sf2_config)
            keepalive.append(sf2_config)
            if self.sound_font_uri is not None:
                c.sound_font_uri = _utf8_arg(self.sound_font_uri, "instrument.sound_font_uri")

        c._keepalive = tuple(keepalive)
        return c


@dataclass(frozen=True, kw_only=True)
class ProjectDestinationInstrument(ProjectInstrument):
    """A stored instrument and the MIDI destination it serves."""

    destination_id: int
    """MIDI destination id of the stored binding."""

    @classmethod
    def _from_c(cls, binding: SonareProjectInstrumentBinding) -> ProjectDestinationInstrument:
        c = binding.instrument
        destination_id = int(binding.destination_id)
        kind_value = int(c.kind)
        if not 0 <= kind_value < len(_PROJECT_INSTRUMENT_KIND_NAMES):
            raise _malformed_output(f"unknown project instrument kind ordinal: {kind_value}")
        kind = cast(ProjectInstrumentKind, _PROJECT_INSTRUMENT_KIND_NAMES[kind_value])
        bank_msb = _optional_midi_value(int(c.bank_msb), "bank_msb")
        bank_lsb = _optional_midi_value(int(c.bank_lsb), "bank_lsb")
        program = _optional_midi_value(int(c.program), "program")
        if kind == "synth":
            if not c.synth:
                raise _malformed_output("project instrument synth config is missing")
            patch = _project_synth_patch_from_c(c.synth.contents)
            return cls(
                destination_id=destination_id,
                kind=kind,
                patch=patch,
                use_gm_programs=_project_bool_from_c(c.use_gm_programs, "use_gm_programs"),
                bank_msb=bank_msb,
                bank_lsb=bank_lsb,
                program=program,
            )
        if kind == "builtin":
            if not c.builtin:
                raise _malformed_output("project instrument built-in config is missing")
            config = c.builtin.contents
            waveform = _synth_enum_name(int(config.waveform), _BUILTIN_SYNTH_WAVEFORMS)
            return cls(
                destination_id=destination_id,
                kind=kind,
                config=BuiltinSynthConfig(
                    waveform=cast(SynthWaveform, waveform),
                    gain=float(config.gain),
                    attack_ms=float(config.attack_ms),
                    decay_ms=float(config.decay_ms),
                    sustain=float(config.sustain),
                    release_ms=float(config.release_ms),
                    polyphony=int(config.polyphony),
                ),
                bank_msb=bank_msb,
                bank_lsb=bank_lsb,
                program=program,
            )
        if not c.sf2:
            raise _malformed_output("project instrument SoundFont config is missing")
        config = c.sf2.contents
        if config.prefer_model_for_modeled_families < 0 or config.clear_bank_rig < 0:
            raise _malformed_output("project instrument SoundFont flags must be nonnegative")
        gs_efx_realization = _enum_token_from_value(
            int(config.gs_efx_realization), _GS_EFX_REALIZATIONS, "GS effect realization"
        )
        uri = _project_string_from_c(c.sound_font_uri, "sound_font_uri")
        return cls(
            destination_id=destination_id,
            kind=kind,
            config=Sf2InstrumentConfig(
                gain=float(config.gain),
                polyphony=int(config.polyphony),
                prefer_model_for_modeled_families=bool(config.prefer_model_for_modeled_families),
                clear_bank_rig=bool(config.clear_bank_rig),
                gs_efx_realization=cast(GsEfxRealization, gs_efx_realization),
            ),
            sound_font_uri=uri,
            bank_msb=bank_msb,
            bank_lsb=bank_lsb,
            program=program,
        )


def _optional_midi_value(value: int, name: str) -> int | None:
    if value == -1:
        return None
    if not 0 <= value <= 127:
        raise _malformed_output(f"destination instrument has an invalid {name} override")
    return value


def _project_bool_from_c(value: int, name: str) -> bool:
    if value not in (0, 1):
        raise _malformed_output(f"project instrument {name} is not a bool: {value}")
    return bool(value)


def _project_string_from_c(value: bytes | None, name: str) -> str | None:
    if value is None:
        return None
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError:
        raise _malformed_output(f"project instrument {name} is not UTF-8") from None


def _enum_token_from_value(value: int, names: Mapping[str, int], noun: str) -> str:
    try:
        return next(name for name, ordinal in names.items() if ordinal == value)
    except StopIteration:
        raise _malformed_output(f"unknown {noun} ordinal: {value}") from None


_PROJECT_PATCH_PRESENCE = {
    "unison": (SONARE_SYNTH_FIELD_UNISON, False),
    "detune_cents": (SONARE_SYNTH_FIELD_DETUNE_CENTS, False),
    "drift_cents": (SONARE_SYNTH_FIELD_DRIFT_CENTS, False),
    "drive": (SONARE_SYNTH_FIELD_DRIVE, False),
    "cutoff_hz": (SONARE_SYNTH_FIELD_CUTOFF_HZ, False),
    "resonance_q": (SONARE_SYNTH_FIELD_RESONANCE_Q, False),
    "key_track": (SONARE_SYNTH_FIELD_KEY_TRACK, False),
    "env_to_cutoff_cents": (SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS, False),
    "vel_to_cutoff_cents": (SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS, False),
    "amp_attack_ms": (SONARE_SYNTH_FIELD_AMP_ATTACK_MS, False),
    "amp_decay_ms": (SONARE_SYNTH_FIELD_AMP_DECAY_MS, False),
    "amp_sustain": (SONARE_SYNTH_FIELD_AMP_SUSTAIN, False),
    "amp_release_ms": (SONARE_SYNTH_FIELD_AMP_RELEASE_MS, False),
    "filter_attack_ms": (SONARE_SYNTH_FIELD_FILTER_ATTACK_MS, False),
    "filter_decay_ms": (SONARE_SYNTH_FIELD_FILTER_DECAY_MS, False),
    "filter_sustain": (SONARE_SYNTH_FIELD_FILTER_SUSTAIN, False),
    "filter_release_ms": (SONARE_SYNTH_FIELD_FILTER_RELEASE_MS, False),
    "lfo_rate_hz": (SONARE_SYNTH_FIELD_LFO_RATE_HZ, False),
    "lfo_to_pitch_cents": (SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS, False),
    "lfo2_rate_hz": (SONARE_SYNTH_FIELD_LFO2_RATE_HZ, False),
    "glide_ms": (SONARE_SYNTH_FIELD_GLIDE_MS, False),
    "body_mix": (SONARE_SYNTH_FIELD_BODY_MIX, False),
    "stereo_spread": (SONARE_SYNTH_FIELD_STEREO_SPREAD, False),
    "gain": (SONARE_SYNTH_FIELD_GAIN, False),
    "polyphony": (SONARE_SYNTH_FIELD_POLYPHONY, False),
    "bus_drive": (SONARE_SYNTH_FIELD_BUS_DRIVE, False),
    "sample_set": (0, False),
    "sample_level": (0, False),
    "sample_start_offset": (0, False),
    "hp_cutoff_hz": (SONARE_SYNTH_FIELD_HP_CUTOFF_HZ, False),
    "sample_hold_hz": (SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ, False),
    "bit_depth": (SONARE_SYNTH_FIELD_BIT_DEPTH, False),
    "pitch_offset_cents": (SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS, False),
    "drift_rate_hz": (SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ, True),
    "amp_delay_ms": (SONARE_SYNTH_FIELD2_AMP_DELAY_MS, True),
    "amp_hold_ms": (SONARE_SYNTH_FIELD2_AMP_HOLD_MS, True),
    "filter_delay_ms": (SONARE_SYNTH_FIELD2_FILTER_DELAY_MS, True),
    "filter_hold_ms": (SONARE_SYNTH_FIELD2_FILTER_HOLD_MS, True),
    "one_shot": (SONARE_SYNTH_FIELD2_ONE_SHOT, True),
}


def _project_synth_patch_from_c(c: SonareSynthPatch) -> SynthPatch:
    if not 0 <= int(c.num_mod_routings) <= SONARE_SYNTH_PATCH_MOD_ROUTINGS:
        raise _malformed_output(
            f"project instrument mod routing count is out of range: {c.num_mod_routings}"
        )
    if c.engine_param_count and not c.engine_params:
        raise _malformed_output("project instrument engine parameters are missing")

    patch = SynthPatch._from_c(c)
    updates: dict[str, object] = {}
    for name, (bit, second_word) in _PROJECT_PATCH_PRESENCE.items():
        value = getattr(patch, name)
        if bit == 0:
            if value == 0:
                updates[name] = None
            continue
        word = int(c.present_fields2 if second_word else c.present_fields)
        if word & bit == 0:
            updates[name] = None

    if (
        int(c.num_mod_routings) == 0
        and int(c.present_fields) & SONARE_SYNTH_FIELD_MOD_ROUTINGS == 0
    ):
        updates["mod_routings"] = None
    if c.engine_param_count:
        params: dict[str, float] = {}
        for index in range(int(c.engine_param_count)):
            raw_key = c.engine_params[index].key
            if not raw_key:
                raise _malformed_output("project instrument engine parameter key is empty")
            key = _project_string_from_c(raw_key, "engine parameter key")
            if not key:
                raise _malformed_output("project instrument engine parameter key is empty")
            params[key] = float(c.engine_params[index].value)
        updates["engine_params"] = params
    else:
        updates["engine_params"] = None
    return replace(patch, **cast(Any, updates))


def _synth_patch_param_info(raw: str) -> list[SynthPatchParamInfo]:
    rows = json.loads(raw)
    result: list[SynthPatchParamInfo] = []
    for row in rows:
        if row.get("type") == "enum":
            result.append(_from_wire(SynthPatchEnumParamInfo, row))
        else:
            result.append(_from_wire(SynthParamInfo, row))
    return result
