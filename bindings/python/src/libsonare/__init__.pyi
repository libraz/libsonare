from __future__ import annotations

from ._project import BuiltinSynthConfig as BuiltinSynthConfig
from ._project import ExternalInstrument as ExternalInstrument
from ._project import MidiCcBinding as MidiCcBinding
from ._project import MidiRouteResult as MidiRouteResult
from ._project import NotePairValidation as NotePairValidation
from ._project import Project as Project
from ._project import ProjectDeserializeResult as ProjectDeserializeResult
from ._project import Sf2InstrumentConfig as Sf2InstrumentConfig
from ._project import Sf2ProgramStatus as Sf2ProgramStatus
from ._project import SynthModRouting as SynthModRouting
from ._project import SynthPatch as SynthPatch
from ._project import project_abi_version as project_abi_version
from ._project import synth_enum_tables as synth_enum_tables
from ._project import synth_preset_names as synth_preset_names
from ._project import synth_preset_patch as synth_preset_patch
from ._runtime import SonareError as SonareError
from .analyzer import (
    Mixer as Mixer,
)
from .analyzer import (
    MixerStereoResult as MixerStereoResult,
)
from .analyzer import (
    RealtimeVoiceChanger as RealtimeVoiceChanger,
)
from .analyzer import (
    RealtimeVoiceChangerConfig as RealtimeVoiceChangerConfig,
)
from .analyzer import (
    SpectralRegionOp as SpectralRegionOp,
)
from .analyzer import (
    StreamingEqualizer as StreamingEqualizer,
)
from .analyzer import (
    StreamingMasteringChain as StreamingMasteringChain,
)
from .analyzer import (
    abi_version as abi_version,
)
from .analyzer import (
    amplitude_to_db as amplitude_to_db,
)
from .analyzer import (
    analyze as analyze,
)
from .analyzer import (
    analyze_bpm as analyze_bpm,
)
from .analyzer import (
    analyze_dynamics as analyze_dynamics,
)
from .analyzer import (
    analyze_impulse_response as analyze_impulse_response,
)
from .analyzer import (
    analyze_melody as analyze_melody,
)
from .analyzer import (
    analyze_rhythm as analyze_rhythm,
)
from .analyzer import (
    analyze_sections as analyze_sections,
)
from .analyzer import (
    analyze_timbre as analyze_timbre,
)
from .analyzer import (
    analyze_with_progress as analyze_with_progress,
)
from .analyzer import (
    bass_chroma as bass_chroma,
)
from .analyzer import (
    chord_functional_analysis as chord_functional_analysis,
)
from .analyzer import (
    chroma as chroma,
)
from .analyzer import (
    chroma_cens as chroma_cens,
)
from .analyzer import (
    cqt as cqt,
)
from .analyzer import (
    cyclic_tempogram as cyclic_tempogram,
)
from .analyzer import (
    db_to_amplitude as db_to_amplitude,
)
from .analyzer import (
    db_to_power as db_to_power,
)
from .analyzer import (
    decompose as decompose,
)
from .analyzer import (
    decompose_with_init as decompose_with_init,
)
from .analyzer import (
    deemphasis as deemphasis,
)
from .analyzer import (
    detect_acoustic as detect_acoustic,
)
from .analyzer import (
    detect_beats as detect_beats,
)
from .analyzer import (
    detect_bpm as detect_bpm,
)
from .analyzer import (
    detect_chords as detect_chords,
)
from .analyzer import (
    detect_downbeats as detect_downbeats,
)
from .analyzer import (
    detect_key as detect_key,
)
from .analyzer import (
    detect_key_candidates as detect_key_candidates,
)
from .analyzer import (
    detect_onsets as detect_onsets,
)
from .analyzer import (
    ebur128_loudness_range as ebur128_loudness_range,
)
from .analyzer import (
    engine_abi_version as engine_abi_version,
)
from .analyzer import (
    estimate_room as estimate_room,
)
from .analyzer import (
    estimate_tuning as estimate_tuning,
)
from .analyzer import (
    fix_frames as fix_frames,
)
from .analyzer import (
    fix_length as fix_length,
)
from .analyzer import (
    frame_signal as frame_signal,
)
from .analyzer import (
    frames_to_samples as frames_to_samples,
)
from .analyzer import (
    frames_to_time as frames_to_time,
)
from .analyzer import (
    harmonic as harmonic,
)
from .analyzer import (
    has_ffmpeg_support as has_ffmpeg_support,
)
from .analyzer import (
    hpss as hpss,
)
from .analyzer import (
    hpss_with_residual as hpss_with_residual,
)
from .analyzer import (
    hybrid_cqt as hybrid_cqt,
)
from .analyzer import (
    hz_to_mel as hz_to_mel,
)
from .analyzer import (
    hz_to_midi as hz_to_midi,
)
from .analyzer import (
    hz_to_note as hz_to_note,
)
from .analyzer import (
    lufs_interleaved as lufs_interleaved,
)
from .analyzer import (
    master_audio as master_audio,
)
from .analyzer import (
    master_audio_stereo as master_audio_stereo,
)
from .analyzer import (
    mastering as mastering,
)
from .analyzer import (
    mastering_assistant_suggest as mastering_assistant_suggest,
)
from .analyzer import (
    mastering_audio_profile as mastering_audio_profile,
)
from .analyzer import (
    mastering_chain as mastering_chain,
)
from .analyzer import (
    mastering_chain_stereo as mastering_chain_stereo,
)
from .analyzer import (
    mastering_insert_names as mastering_insert_names,
)
from .analyzer import (
    mastering_insert_param_info as mastering_insert_param_info,
)
from .analyzer import (
    mastering_insert_param_names as mastering_insert_param_names,
)
from .analyzer import (
    mastering_pair_analysis_names as mastering_pair_analysis_names,
)
from .analyzer import (
    mastering_pair_analyze as mastering_pair_analyze,
)
from .analyzer import (
    mastering_pair_process as mastering_pair_process,
)
from .analyzer import (
    mastering_pair_processor_names as mastering_pair_processor_names,
)
from .analyzer import (
    mastering_preset_names as mastering_preset_names,
)
from .analyzer import (
    mastering_process as mastering_process,
)
from .analyzer import (
    mastering_process_stereo as mastering_process_stereo,
)
from .analyzer import (
    mastering_processor_catalog as mastering_processor_catalog,
)
from .analyzer import (
    mastering_processor_names as mastering_processor_names,
)
from .analyzer import (
    mastering_repair_declick as mastering_repair_declick,
)
from .analyzer import (
    mastering_repair_declip as mastering_repair_declip,
)
from .analyzer import (
    mastering_repair_decrackle as mastering_repair_decrackle,
)
from .analyzer import (
    mastering_repair_dehum as mastering_repair_dehum,
)
from .analyzer import (
    mastering_repair_denoise_classical as mastering_repair_denoise_classical,
)
from .analyzer import (
    mastering_repair_dereverb_classical as mastering_repair_dereverb_classical,
)
from .analyzer import (
    mastering_repair_trim_silence as mastering_repair_trim_silence,
)
from .analyzer import (
    mastering_stereo_analysis_names as mastering_stereo_analysis_names,
)
from .analyzer import (
    mastering_stereo_analyze as mastering_stereo_analyze,
)
from .analyzer import (
    mastering_streaming_preview as mastering_streaming_preview,
)
from .analyzer import (
    mel_spectrogram as mel_spectrogram,
)
from .analyzer import (
    mel_to_hz as mel_to_hz,
)
from .analyzer import (
    metering_phase_scope as metering_phase_scope,
)
from .analyzer import (
    metering_phase_scope_decimated as metering_phase_scope_decimated,
)
from .analyzer import (
    metering_spectrum as metering_spectrum,
)
from .analyzer import (
    metering_spectrum_frame as metering_spectrum_frame,
)
from .analyzer import (
    metering_stereo_correlation as metering_stereo_correlation,
)
from .analyzer import (
    metering_stereo_width as metering_stereo_width,
)
from .analyzer import (
    metering_vectorscope as metering_vectorscope,
)
from .analyzer import (
    metering_vectorscope_decimated as metering_vectorscope_decimated,
)
from .analyzer import (
    mfcc as mfcc,
)
from .analyzer import (
    midi_to_hz as midi_to_hz,
)
from .analyzer import (
    mix_stereo as mix_stereo,
)
from .analyzer import (
    mixing_scene_preset_json as mixing_scene_preset_json,
)
from .analyzer import (
    mixing_scene_preset_names as mixing_scene_preset_names,
)
from .analyzer import (
    nn_filter as nn_filter,
)
from .analyzer import (
    normalize as normalize,
)
from .analyzer import (
    note_stretch as note_stretch,
)
from .analyzer import (
    note_to_hz as note_to_hz,
)
from .analyzer import (
    onset_strength_multi as onset_strength_multi,
)
from .analyzer import (
    pad_center as pad_center,
)
from .analyzer import (
    pcen as pcen,
)
from .analyzer import (
    peak_pick as peak_pick,
)
from .analyzer import (
    percussive as percussive,
)
from .analyzer import (
    phase_vocoder as phase_vocoder,
)
from .analyzer import (
    pitch_correct_to_midi as pitch_correct_to_midi,
)
from .analyzer import (
    pitch_correct_to_midi_timevarying as pitch_correct_to_midi_timevarying,
)
from .analyzer import (
    pitch_pyin as pitch_pyin,
)
from .analyzer import (
    pitch_shift as pitch_shift,
)
from .analyzer import (
    pitch_tuning as pitch_tuning,
)
from .analyzer import (
    pitch_yin as pitch_yin,
)
from .analyzer import (
    plp as plp,
)
from .analyzer import (
    poly_features as poly_features,
)
from .analyzer import (
    power_to_db as power_to_db,
)
from .analyzer import (
    preemphasis as preemphasis,
)
from .analyzer import (
    pseudo_cqt as pseudo_cqt,
)
from .analyzer import (
    realtime_voice_changer_preset_config as realtime_voice_changer_preset_config,
)
from .analyzer import (
    realtime_voice_changer_preset_json as realtime_voice_changer_preset_json,
)
from .analyzer import (
    realtime_voice_changer_preset_names as realtime_voice_changer_preset_names,
)
from .analyzer import (
    realtime_voice_changer_preset_pod as realtime_voice_changer_preset_pod,
)
from .analyzer import (
    remix as remix,
)
from .analyzer import (
    resample as resample,
)
from .analyzer import (
    rms_energy as rms_energy,
)
from .analyzer import (
    room_morph as room_morph,
)
from .analyzer import (
    samples_to_frames as samples_to_frames,
)
from .analyzer import (
    spectral_bandwidth as spectral_bandwidth,
)
from .analyzer import (
    spectral_centroid as spectral_centroid,
)
from .analyzer import (
    spectral_contrast as spectral_contrast,
)
from .analyzer import (
    spectral_edit as spectral_edit,
)
from .analyzer import (
    spectral_flatness as spectral_flatness,
)
from .analyzer import (
    spectral_rolloff as spectral_rolloff,
)
from .analyzer import (
    split_silence as split_silence,
)
from .analyzer import (
    stft as stft,
)
from .analyzer import (
    stft_db as stft_db,
)
from .analyzer import (
    synthesize_rir as synthesize_rir,
)
from .analyzer import (
    tempogram as tempogram,
)
from .analyzer import (
    time_stretch as time_stretch,
)
from .analyzer import (
    time_to_frames as time_to_frames,
)
from .analyzer import (
    tonnetz as tonnetz,
)
from .analyzer import (
    trim as trim,
)
from .analyzer import (
    trim_silence as trim_silence,
)
from .analyzer import (
    validate_realtime_voice_changer_preset_json as validate_realtime_voice_changer_preset_json,
)
from .analyzer import (
    vector_normalize as vector_normalize,
)
from .analyzer import (
    version as version,
)
from .analyzer import (
    voice_change as voice_change,
)
from .analyzer import (
    voice_change_realtime as voice_change_realtime,
)
from .analyzer import (
    voice_changer_abi_version as voice_changer_abi_version,
)
from .analyzer import (
    voice_character_preset_id as voice_character_preset_id,
)
from .analyzer import (
    vqt as vqt,
)
from .analyzer import (
    waveform_peak_pyramid as waveform_peak_pyramid,
)
from .analyzer import (
    waveform_peaks as waveform_peaks,
)
from .analyzer import (
    zero_crossing_rate as zero_crossing_rate,
)
from .analyzer import (
    zero_crossings as zero_crossings,
)
from .audio import Audio as Audio
from .engine import ClipPageProvider as ClipPageProvider
from .engine import FileClipPageProvider as FileClipPageProvider
from .engine import RealtimeEngine as RealtimeEngine
from .streaming import StreamAnalyzer as StreamAnalyzer
from .types import (
    AcousticResult as AcousticResult,
)
from .types import (
    AnalysisDynamics as AnalysisDynamics,
)
from .types import (
    AnalysisMelody as AnalysisMelody,
)
from .types import (
    AnalysisResult as AnalysisResult,
)
from .types import (
    AnalysisRhythm as AnalysisRhythm,
)
from .types import (
    AnalysisTimbre as AnalysisTimbre,
)
from .types import (
    AutomationCurve as AutomationCurve,
)
from .types import (
    AutomationPoint as AutomationPoint,
)
from .types import (
    BpmAnalysisResult as BpmAnalysisResult,
)
from .types import (
    BpmCandidate as BpmCandidate,
)
from .types import (
    ChannelLayout as ChannelLayout,
)
from .types import (
    Chord as Chord,
)
from .types import (
    ChordAnalysisResult as ChordAnalysisResult,
)
from .types import (
    ChromaResult as ChromaResult,
)
from .types import (
    CqtResult as CqtResult,
)
from .types import (
    DynamicsResult as DynamicsResult,
)
from .types import (
    EngineBounceOptions as EngineBounceOptions,
)
from .types import (
    EngineBounceResult as EngineBounceResult,
)
from .types import (
    EngineCaptureStatus as EngineCaptureStatus,
)
from .types import (
    EngineClip as EngineClip,
)
from .types import (
    EngineFreezeOptions as EngineFreezeOptions,
)
from .types import (
    EngineFreezeResult as EngineFreezeResult,
)
from .types import (
    EngineGraphConnection as EngineGraphConnection,
)
from .types import (
    EngineGraphMix as EngineGraphMix,
)
from .types import (
    EngineGraphNode as EngineGraphNode,
)
from .types import (
    EngineGraphNodeType as EngineGraphNodeType,
)
from .types import (
    EngineGraphParameterBinding as EngineGraphParameterBinding,
)
from .types import (
    EngineGraphSpec as EngineGraphSpec,
)
from .types import (
    EngineMarker as EngineMarker,
)
from .types import (
    EngineMetronomeConfig as EngineMetronomeConfig,
)
from .types import (
    EngineMidiClipSchedule as EngineMidiClipSchedule,
)
from .types import (
    EngineMidiEvent as EngineMidiEvent,
)
from .types import (
    EngineTelemetry as EngineTelemetry,
)
from .types import (
    EngineTelemetryError as EngineTelemetryError,
)
from .types import (
    EngineTelemetryType as EngineTelemetryType,
)
from .types import (
    EqSpectrumSnapshot as EqSpectrumSnapshot,
)
from .types import (
    GoniometerPoint as GoniometerPoint,
)
from .types import (
    HpssResult as HpssResult,
)
from .types import (
    Key as Key,
)
from .types import (
    KeyCandidate as KeyCandidate,
)
from .types import (
    KeyProfile as KeyProfile,
)
from .types import (
    MarkerKind as MarkerKind,
)
from .types import (
    MasteringChainResult as MasteringChainResult,
)
from .types import (
    MasteringChainStereoResult as MasteringChainStereoResult,
)
from .types import (
    MasteringResult as MasteringResult,
)
from .types import (
    MasteringStereoResult as MasteringStereoResult,
)
from .types import (
    MelodyPoint as MelodyPoint,
)
from .types import (
    MelodyResult as MelodyResult,
)
from .types import (
    MelSpectrogramResult as MelSpectrogramResult,
)
from .types import (
    MeterTap as MeterTap,
)
from .types import (
    MeterTelemetryRecord as MeterTelemetryRecord,
)
from .types import (
    MeterTelemetryRecordWide as MeterTelemetryRecordWide,
)
from .types import (
    MfccResult as MfccResult,
)
from .types import (
    MixMeterSnapshot as MixMeterSnapshot,
)
from .types import (
    MixResult as MixResult,
)
from .types import (
    Mode as Mode,
)
from .types import (
    ParameterInfo as ParameterInfo,
)
from .types import (
    PhaseScopeReport as PhaseScopeReport,
)
from .types import (
    PitchClass as PitchClass,
)
from .types import (
    PitchResult as PitchResult,
)
from .types import (
    ProjectMarker as ProjectMarker,
)
from .types import (
    QuantizeConfig as QuantizeConfig,
)
from .types import (
    RhythmResult as RhythmResult,
)
from .types import (
    RirResult as RirResult,
)
from .types import (
    RoomEstimate as RoomEstimate,
)
from .types import (
    Section as Section,
)
from .types import (
    SectionResult as SectionResult,
)
from .types import (
    SectionType as SectionType,
)
from .types import (
    SendTiming as SendTiming,
)
from .types import (
    SpectrumReport as SpectrumReport,
)
from .types import (
    StftResult as StftResult,
)
from .types import (
    StreamBarChord as StreamBarChord,
)
from .types import (
    StreamChordChange as StreamChordChange,
)
from .types import (
    StreamConfig as StreamConfig,
)
from .types import (
    StreamFrames as StreamFrames,
)
from .types import (
    StreamFramesI16 as StreamFramesI16,
)
from .types import (
    StreamFramesU8 as StreamFramesU8,
)
from .types import (
    StreamPatternScore as StreamPatternScore,
)
from .types import (
    StreamStats as StreamStats,
)
from .types import (
    TimbreFrame as TimbreFrame,
)
from .types import (
    TimbreResult as TimbreResult,
)
from .types import (
    TimeSignature as TimeSignature,
)
from .types import (
    TransportState as TransportState,
)
from .types import (
    VectorscopeReport as VectorscopeReport,
)
from .types import (
    WaveformPeaksReport as WaveformPeaksReport,
)

__version__: str
