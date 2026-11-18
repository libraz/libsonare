from __future__ import annotations

from ._cancellation import CancelCallback as CancelCallback
from ._cancellation import ProgressCallback as ProgressCallback
from ._effects_polyphony import PolyphonicAnalysis as PolyphonicAnalysis
from ._playback import HrtfSet as HrtfSet
from ._playback import PlaybackLoudnessMeter as PlaybackLoudnessMeter
from ._playback import PlaybackRenderer as PlaybackRenderer
from ._playback import PlaybackRendererConfig as PlaybackRendererConfig
from ._playback import render_playback as render_playback
from ._project import AlignTakeToReferenceResult as AlignTakeToReferenceResult
from ._project import Articulation as Articulation
from ._project import AssistSidecar as AssistSidecar
from ._project import BuiltinSynthConfig as BuiltinSynthConfig
from ._project import ControllerAxis as ControllerAxis
from ._project import ControllerInput as ControllerInput
from ._project import ExternalInstrument as ExternalInstrument
from ._project import ExternalSeparatedStemImportResult as ExternalSeparatedStemImportResult
from ._project import MidiCcBinding as MidiCcBinding
from ._project import MidiRouteResult as MidiRouteResult
from ._project import MpeDimension as MpeDimension
from ._project import NotePairValidation as NotePairValidation
from ._project import NoteTracking as NoteTracking
from ._project import PartRigEntry as PartRigEntry
from ._project import PartRigMode as PartRigMode
from ._project import Project as Project
from ._project import ProjectAutomationTargetKind as ProjectAutomationTargetKind
from ._project import ProjectChordQuality as ProjectChordQuality
from ._project import ProjectDeserializeResult as ProjectDeserializeResult
from ._project import ProjectFadeCurve as ProjectFadeCurve
from ._project import ProjectKeyMode as ProjectKeyMode
from ._project import ProjectLoopMode as ProjectLoopMode
from ._project import ProjectLoopRecordingResult as ProjectLoopRecordingResult
from ._project import ProjectMidiCcBindingKind as ProjectMidiCcBindingKind
from ._project import ProjectMidiClipResult as ProjectMidiClipResult
from ._project import ProjectMidiEvent as ProjectMidiEvent
from ._project import ProjectOverlapPolicy as ProjectOverlapPolicy
from ._project import ProjectSourceKind as ProjectSourceKind
from ._project import ProjectStemLayout as ProjectStemLayout
from ._project import ProjectTimeline as ProjectTimeline
from ._project import ProjectTrackKind as ProjectTrackKind
from ._project import SampleBank as SampleBank
from ._project import SampleDescLoopMode as SampleDescLoopMode
from ._project import SampleKeyTrack as SampleKeyTrack
from ._project import SampleLoopMode as SampleLoopMode
from ._project import Sf2InstrumentConfig as Sf2InstrumentConfig
from ._project import Sf2ProgramStatus as Sf2ProgramStatus
from ._project import SynthBodyType as SynthBodyType
from ._project import SynthEngineMode as SynthEngineMode
from ._project import SynthFilterModel as SynthFilterModel
from ._project import SynthFilterOutput as SynthFilterOutput
from ._project import SynthLfoShape as SynthLfoShape
from ._project import SynthModDestination as SynthModDestination
from ._project import SynthModRouting as SynthModRouting
from ._project import SynthModSource as SynthModSource
from ._project import SynthOscWaveform as SynthOscWaveform
from ._project import SynthPatch as SynthPatch
from ._project import SynthRetrigger as SynthRetrigger
from ._project import SynthWaveform as SynthWaveform
from ._project import TakeAlignment as TakeAlignment
from ._project import align_take_to_reference as align_take_to_reference
from ._project import chord_symbol_from_analysis as chord_symbol_from_analysis
from ._project import controller_profile_names as controller_profile_names
from ._project import key_mode_from_analysis as key_mode_from_analysis
from ._project import project_abi_version as project_abi_version
from ._project import synth_engine_param_info as synth_engine_param_info
from ._project import synth_enum_tables as synth_enum_tables
from ._project import synth_gs_drum_kit_is_voiced_apart as synth_gs_drum_kit_is_voiced_apart
from ._project import synth_gs_drum_kit_name as synth_gs_drum_kit_name
from ._project import synth_gs_variation_is_voiced_apart as synth_gs_variation_is_voiced_apart
from ._project import synth_patch_param_info as synth_patch_param_info
from ._project import synth_preset_names as synth_preset_names
from ._project import synth_preset_patch as synth_preset_patch
from ._project_edit import PART_RIG_ALL_PARTS as PART_RIG_ALL_PARTS
from ._project_edit import PART_RIG_MODES as PART_RIG_MODES
from ._project_instruments import GsEfxRealization as GsEfxRealization
from ._project_model import ProjectCompileResult as ProjectCompileResult
from ._project_model import ProjectCompileTimelineResult as ProjectCompileTimelineResult
from ._project_model import ProjectDiagnostic as ProjectDiagnostic
from ._project_model import ProjectDiagnosticCode as ProjectDiagnosticCode
from ._project_model import ProjectDiagnosticSeverity as ProjectDiagnosticSeverity
from ._runtime import ErrorCode as ErrorCode
from ._runtime import ErrorCodeName as ErrorCodeName
from ._runtime import SonareError as SonareError
from ._runtime import SonareValueError as SonareValueError
from .analyzer import AssignNoteTargetsResult as AssignNoteTargetsResult
from .analyzer import DecomposeResult as DecomposeResult
from .analyzer import MasteringAssistantParams as MasteringAssistantParams
from .analyzer import MasteringParams as MasteringParams
from .analyzer import MasteringParamValue as MasteringParamValue
from .analyzer import MasteringPreset as MasteringPreset
from .analyzer import MasteringSoloParams as MasteringSoloParams
from .analyzer import (
    Mixer as Mixer,
)
from .analyzer import (
    MixerStereoResult as MixerStereoResult,
)
from .analyzer import (
    NormalizeMode as NormalizeMode,
)
from .analyzer import (
    NoteEdit as NoteEdit,
)
from .analyzer import (
    NoteObject as NoteObject,
)
from .analyzer import (
    NoteTarget as NoteTarget,
)
from .analyzer import (
    PercussiveEvent as PercussiveEvent,
)
from .analyzer import (
    PercussiveEventEdit as PercussiveEventEdit,
)
from .analyzer import (
    PitchDecompositionResult as PitchDecompositionResult,
)
from .analyzer import PlanarChannels as PlanarChannels
from .analyzer import (
    RealtimeVoiceChanger as RealtimeVoiceChanger,
)
from .analyzer import (
    RealtimeVoiceChangerConfig as RealtimeVoiceChangerConfig,
)
from .analyzer import (
    RealtimeVoiceChangerPreset as RealtimeVoiceChangerPreset,
)
from .analyzer import (
    SilenceCommonReport as SilenceCommonReport,
)
from .analyzer import SoloProcessor as SoloProcessor
from .analyzer import (
    SpectralRegionOp as SpectralRegionOp,
)
from .analyzer import (
    SplitSilenceCommonWithReportResult as SplitSilenceCommonWithReportResult,
)
from .analyzer import (
    StreamingEqualizer as StreamingEqualizer,
)
from .analyzer import (
    StreamingMasteringChain as StreamingMasteringChain,
)
from .analyzer import (
    StreamingRetune as StreamingRetune,
)
from .analyzer import StripRef as StripRef
from .analyzer import (
    TranscribeResult as TranscribeResult,
)
from .analyzer import (
    TrimResult as TrimResult,
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
    assign_note_targets as assign_note_targets,
)
from .analyzer import (
    auto_tune as auto_tune,
)
from .analyzer import (
    bass_chroma as bass_chroma,
)
from .analyzer import (
    capabilities as capabilities,
)
from .analyzer import (
    capability_catalog as capability_catalog,
)
from .analyzer import chirp as chirp
from .analyzer import (
    chord_functional_analysis as chord_functional_analysis,
)
from .analyzer import (
    chord_functions as chord_functions,
)
from .analyzer import (
    chroma as chroma,
)
from .analyzer import (
    chroma_cens as chroma_cens,
)
from .analyzer import (
    chroma_cqt as chroma_cqt,
)
from .analyzer import clicks as clicks
from .analyzer import (
    cqt as cqt,
)
from .analyzer import cqt_to_audio as cqt_to_audio
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
    decompose_note_pitch as decompose_note_pitch,
)
from .analyzer import (
    decompose_stems as decompose_stems,
)
from .analyzer import (
    decompose_stems_linked as decompose_stems_linked,
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
    detect_boundaries as detect_boundaries,
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
    ebur128_loudness_range_lu as ebur128_loudness_range_lu,
)
from .analyzer import (
    engine_abi_version as engine_abi_version,
)
from .analyzer import (
    estimate_meter as estimate_meter,
)
from .analyzer import (
    estimate_room as estimate_room,
)
from .analyzer import (
    estimate_tuning as estimate_tuning,
)
from .analyzer import (
    extract_notes as extract_notes,
)
from .analyzer import (
    extract_percussive_events as extract_percussive_events,
)
from .analyzer import (
    fix_frames as fix_frames,
)
from .analyzer import (
    fix_length as fix_length,
)
from .analyzer import fourier_tempogram as fourier_tempogram
from .analyzer import (
    frame_signal as frame_signal,
)
from .analyzer import (
    frames_to_samples as frames_to_samples,
)
from .analyzer import (
    frames_to_time as frames_to_time,
)
from .analyzer import griffin_lim as griffin_lim
from .analyzer import (
    harmonic as harmonic,
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
from .analyzer import lufs as lufs
from .analyzer import (
    lufs_interleaved as lufs_interleaved,
)
from .analyzer import (
    lufs_series_interleaved as lufs_series_interleaved,
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
    mastering_ab_match_loudness as mastering_ab_match_loudness,
)
from .analyzer import (
    mastering_ab_match_loudness_stereo as mastering_ab_match_loudness_stereo,
)
from .analyzer import (
    mastering_amp_preset_catalog as mastering_amp_preset_catalog,
)
from .analyzer import (
    mastering_analyze_pair as mastering_analyze_pair,
)
from .analyzer import (
    mastering_analyze_stereo as mastering_analyze_stereo,
)
from .analyzer import (
    mastering_apply_pair_processor as mastering_apply_pair_processor,
)
from .analyzer import (
    mastering_apply_pair_processor_stereo as mastering_apply_pair_processor_stereo,
)
from .analyzer import (
    mastering_apply_processor as mastering_apply_processor,
)
from .analyzer import (
    mastering_apply_processor_stereo as mastering_apply_processor_stereo,
)
from .analyzer import (
    mastering_assistant_suggest as mastering_assistant_suggest,
)
from .analyzer import (
    mastering_assistant_suggest_chain as mastering_assistant_suggest_chain,
)
from .analyzer import (
    mastering_assistant_suggest_chain_stereo as mastering_assistant_suggest_chain_stereo,
)
from .analyzer import (
    mastering_assistant_suggest_stereo as mastering_assistant_suggest_stereo,
)
from .analyzer import (
    mastering_audio_profile as mastering_audio_profile,
)
from .analyzer import (
    mastering_audio_profile_stereo as mastering_audio_profile_stereo,
)
from .analyzer import (
    mastering_chain_config_from_json as mastering_chain_config_from_json,
)
from .analyzer import mastering_dynamics_compressor as mastering_dynamics_compressor
from .analyzer import mastering_dynamics_gate as mastering_dynamics_gate
from .analyzer import mastering_dynamics_transient_shaper as mastering_dynamics_transient_shaper
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
    mastering_insert_timing as mastering_insert_timing,
)
from .analyzer import (
    mastering_pair_analysis_names as mastering_pair_analysis_names,
)
from .analyzer import (
    mastering_pair_processor_names as mastering_pair_processor_names,
)
from .analyzer import (
    mastering_platform_names as mastering_platform_names,
)
from .analyzer import (
    mastering_preset_names as mastering_preset_names,
)
from .analyzer import (
    mastering_preset_params as mastering_preset_params,
)
from .analyzer import (
    mastering_processor_catalog as mastering_processor_catalog,
)
from .analyzer import (
    mastering_processor_names as mastering_processor_names,
)
from .analyzer import (
    mastering_repair_analyze as mastering_repair_analyze,
)
from .analyzer import (
    mastering_repair_apply as mastering_repair_apply,
)
from .analyzer import (
    mastering_repair_declick as mastering_repair_declick,
)
from .analyzer import (
    mastering_repair_declick_stereo as mastering_repair_declick_stereo,
)
from .analyzer import (
    mastering_repair_declip as mastering_repair_declip,
)
from .analyzer import (
    mastering_repair_declip_stereo as mastering_repair_declip_stereo,
)
from .analyzer import (
    mastering_repair_decrackle as mastering_repair_decrackle,
)
from .analyzer import (
    mastering_repair_decrackle_stereo as mastering_repair_decrackle_stereo,
)
from .analyzer import (
    mastering_repair_dehum as mastering_repair_dehum,
)
from .analyzer import (
    mastering_repair_dehum_stereo as mastering_repair_dehum_stereo,
)
from .analyzer import (
    mastering_repair_denoise_classical as mastering_repair_denoise_classical,
)
from .analyzer import (
    mastering_repair_denoise_classical_linked as mastering_repair_denoise_classical_linked,
)
from .analyzer import (
    mastering_repair_denoise_classical_stereo as mastering_repair_denoise_classical_stereo,
)
from .analyzer import (
    mastering_repair_dereverb_classical as mastering_repair_dereverb_classical,
)
from .analyzer import (
    mastering_repair_dereverb_classical_linked as mastering_repair_dereverb_classical_linked,
)
from .analyzer import (
    mastering_repair_dereverb_classical_stereo as mastering_repair_dereverb_classical_stereo,
)
from .analyzer import (
    mastering_repair_dereverb_config_for_room as mastering_repair_dereverb_config_for_room,
)
from .analyzer import (
    mastering_repair_detect_clicks as mastering_repair_detect_clicks,
)
from .analyzer import (
    mastering_repair_detect_clipping as mastering_repair_detect_clipping,
)
from .analyzer import (
    mastering_repair_detect_crackle as mastering_repair_detect_crackle,
)
from .analyzer import (
    mastering_repair_detect_hum as mastering_repair_detect_hum,
)
from .analyzer import (
    mastering_repair_detect_noise_floor as mastering_repair_detect_noise_floor,
)
from .analyzer import (
    mastering_repair_detect_reverb as mastering_repair_detect_reverb,
)
from .analyzer import (
    mastering_repair_detect_trim_range as mastering_repair_detect_trim_range,
)
from .analyzer import (
    mastering_repair_detect_trim_range_stereo as mastering_repair_detect_trim_range_stereo,
)
from .analyzer import (
    mastering_repair_noise_band_bins as mastering_repair_noise_band_bins,
)
from .analyzer import (
    mastering_repair_trim_silence as mastering_repair_trim_silence,
)
from .analyzer import (
    mastering_repair_trim_silence_stereo as mastering_repair_trim_silence_stereo,
)
from .analyzer import (
    mastering_stereo_analysis_names as mastering_stereo_analysis_names,
)
from .analyzer import (
    mastering_streaming_preview as mastering_streaming_preview,
)
from .analyzer import (
    mastering_streaming_preview_stereo as mastering_streaming_preview_stereo,
)
from .analyzer import mel_delta as mel_delta
from .analyzer import (
    mel_spectrogram as mel_spectrogram,
)
from .analyzer import mel_to_audio as mel_to_audio
from .analyzer import (
    mel_to_hz as mel_to_hz,
)
from .analyzer import mel_to_stft as mel_to_stft
from .analyzer import merge_notes as merge_notes
from .analyzer import metering_crest_factor_db as metering_crest_factor_db
from .analyzer import metering_crest_factor_db_stereo as metering_crest_factor_db_stereo
from .analyzer import metering_dc_offset as metering_dc_offset
from .analyzer import metering_detect_clipping as metering_detect_clipping
from .analyzer import metering_dynamic_range as metering_dynamic_range
from .analyzer import metering_peak_db as metering_peak_db
from .analyzer import (
    metering_phase_scope as metering_phase_scope,
)
from .analyzer import (
    metering_phase_scope_decimated as metering_phase_scope_decimated,
)
from .analyzer import metering_rms_db as metering_rms_db
from .analyzer import metering_silence_ratio as metering_silence_ratio
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
from .analyzer import metering_true_peak_dbtp as metering_true_peak_dbtp
from .analyzer import (
    metering_vectorscope as metering_vectorscope,
)
from .analyzer import (
    metering_vectorscope_decimated as metering_vectorscope_decimated,
)
from .analyzer import (
    mfcc as mfcc,
)
from .analyzer import mfcc_to_audio as mfcc_to_audio
from .analyzer import mfcc_to_mel as mfcc_to_mel
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
from .analyzer import momentary_lufs as momentary_lufs
from .analyzer import (
    nn_filter as nn_filter,
)
from .analyzer import nnls_chroma as nnls_chroma
from .analyzer import (
    normalize as normalize,
)
from .analyzer import (
    normalize_stereo as normalize_stereo,
)
from .analyzer import (
    note_move as note_move,
)
from .analyzer import (
    note_segments as note_segments,
)
from .analyzer import (
    note_stretch as note_stretch,
)
from .analyzer import (
    note_targets_from_smf as note_targets_from_smf,
)
from .analyzer import (
    note_to_hz as note_to_hz,
)
from .analyzer import (
    onset_backtrack as onset_backtrack,
)
from .analyzer import onset_strength as onset_strength
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
from .analyzer import piptrack as piptrack
from .analyzer import (
    pitch_correct_timevarying as pitch_correct_timevarying,
)
from .analyzer import (
    pitch_correct_to_midi as pitch_correct_to_midi,
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
    reassigned_spectrogram as reassigned_spectrogram,
)
from .analyzer import (
    reference_hz_to_tuning as reference_hz_to_tuning,
)
from .analyzer import (
    remix as remix,
)
from .analyzer import (
    remix_aligned_intervals as remix_aligned_intervals,
)
from .analyzer import (
    render_notes as render_notes,
)
from .analyzer import (
    render_percussive_events as render_percussive_events,
)
from .analyzer import (
    resample as resample,
)
from .analyzer import (
    rms_energy as rms_energy,
)
from .analyzer import (
    room_geometry_from_estimate as room_geometry_from_estimate,
)
from .analyzer import (
    room_morph as room_morph,
)
from .analyzer import (
    room_morph_stereo as room_morph_stereo,
)
from .analyzer import (
    samples_to_frames as samples_to_frames,
)
from .analyzer import scale_correction_semitones as scale_correction_semitones
from .analyzer import scale_mask_for_mode as scale_mask_for_mode
from .analyzer import scale_pitch_class_enabled as scale_pitch_class_enabled
from .analyzer import scale_quantize_midi as scale_quantize_midi
from .analyzer import (
    segment_agglomerative as segment_agglomerative,
)
from .analyzer import (
    segment_cross_similarity as segment_cross_similarity,
)
from .analyzer import (
    segment_lag_to_recurrence as segment_lag_to_recurrence,
)
from .analyzer import (
    segment_path_enhance as segment_path_enhance,
)
from .analyzer import (
    segment_recurrence_matrix as segment_recurrence_matrix,
)
from .analyzer import (
    segment_recurrence_to_lag as segment_recurrence_to_lag,
)
from .analyzer import (
    segment_subsegment as segment_subsegment,
)
from .analyzer import short_term_lufs as short_term_lufs
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
    spectral_flux as spectral_flux,
)
from .analyzer import (
    spectral_rolloff as spectral_rolloff,
)
from .analyzer import (
    split_note as split_note,
)
from .analyzer import (
    split_silence as split_silence,
)
from .analyzer import (
    split_silence_common as split_silence_common,
)
from .analyzer import (
    split_silence_common_with_report as split_silence_common_with_report,
)
from .analyzer import (
    stft as stft,
)
from .analyzer import (
    stft_db as stft_db,
)
from .analyzer import (
    streaming_loudness_gain as streaming_loudness_gain,
)
from .analyzer import (
    streaming_loudness_gain_stereo as streaming_loudness_gain_stereo,
)
from .analyzer import (
    synthesize_rir as synthesize_rir,
)
from .analyzer import (
    tempogram as tempogram,
)
from .analyzer import tempogram_ratio as tempogram_ratio
from .analyzer import (
    time_stretch as time_stretch,
)
from .analyzer import (
    time_to_frames as time_to_frames,
)
from .analyzer import tone as tone
from .analyzer import (
    tonnetz as tonnetz,
)
from .analyzer import (
    transcribe as transcribe,
)
from .analyzer import (
    trim as trim,
)
from .analyzer import (
    trim_silence as trim_silence,
)
from .analyzer import tuning_to_reference_hz as tuning_to_reference_hz
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
from .analyzer import vqt_to_audio as vqt_to_audio
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
from .audio import DecodedChannels as DecodedChannels
from .audio import decode_channels as decode_channels
from .audio import downmix as downmix
from .engine import MAX_TRACK_LANES as MAX_TRACK_LANES
from .engine import ClipPageProvider as ClipPageProvider
from .engine import FileClipPageProvider as FileClipPageProvider
from .engine import RealtimeEngine as RealtimeEngine
from .mixing_assistant import MixAssistantTrack as MixAssistantTrack
from .mixing_assistant import (
    mixing_assistant_source_class_from_name as mixing_assistant_source_class_from_name,
)
from .mixing_assistant import (
    mixing_assistant_source_class_names as mixing_assistant_source_class_names,
)
from .mixing_assistant import mixing_assistant_suggest as mixing_assistant_suggest
from .streaming import StreamAnalyzer as StreamAnalyzer
from .types import CHORD_QUALITIES as CHORD_QUALITIES
from .types import (
    AcousticModeName as AcousticModeName,
)
from .types import (
    AcousticResult as AcousticResult,
)
from .types import AnalysisBpmCandidate as AnalysisBpmCandidate
from .types import AnalysisChord as AnalysisChord
from .types import (
    AnalysisDynamics as AnalysisDynamics,
)
from .types import (
    AnalysisResult as AnalysisResult,
)
from .types import (
    AnalysisRhythm as AnalysisRhythm,
)
from .types import AudioDefects as AudioDefects
from .types import (
    AutomationCurve as AutomationCurve,
)
from .types import (
    AutomationPoint as AutomationPoint,
)
from .types import (
    AutoTuneResult as AutoTuneResult,
)
from .types import Beat as Beat
from .types import BeatObservations as BeatObservations
from .types import Boundary as Boundary
from .types import BoundaryResult as BoundaryResult
from .types import (
    BpmAnalysisResult as BpmAnalysisResult,
)
from .types import (
    BpmCandidate as BpmCandidate,
)
from .types import (
    BpmCandidateRelation as BpmCandidateRelation,
)
from .types import Capabilities as Capabilities
from .types import CapabilitiesAbi as CapabilitiesAbi
from .types import CapabilitiesDecode as CapabilitiesDecode
from .types import CapabilitiesFeatures as CapabilitiesFeatures
from .types import CapabilityCatalog as CapabilityCatalog
from .types import CapabilityCatalogChain as CapabilityCatalogChain
from .types import CapabilityCatalogChainList as CapabilityCatalogChainList
from .types import CapabilityCatalogChainStage as CapabilityCatalogChainStage
from .types import CapabilityCatalogPresets as CapabilityCatalogPresets
from .types import (
    CaptureInputMonitor as CaptureInputMonitor,
)
from .types import (
    CapturePunch as CapturePunch,
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
    ChordChromaMethod as ChordChromaMethod,
)
from .types import (
    ChordFunctionName as ChordFunctionName,
)
from .types import (
    ChromaResult as ChromaResult,
)
from .types import ClickDetection as ClickDetection
from .types import ClipDetection as ClipDetection
from .types import ClipPageRequest as ClipPageRequest
from .types import ClippingRegion as ClippingRegion
from .types import ClippingReport as ClippingReport
from .types import (
    CompressorDetector as CompressorDetector,
)
from .types import (
    CqtResult as CqtResult,
)
from .types import CrackleDetection as CrackleDetection
from .types import DeclickReport as DeclickReport
from .types import DeclickResult as DeclickResult
from .types import DeclickStage as DeclickStage
from .types import DeclickStereoResult as DeclickStereoResult
from .types import DeclipReport as DeclipReport
from .types import DeclipResult as DeclipResult
from .types import DeclipStage as DeclipStage
from .types import DeclipStereoResult as DeclipStereoResult
from .types import (
    DecrackleMode as DecrackleMode,
)
from .types import DecrackleReport as DecrackleReport
from .types import DecrackleResult as DecrackleResult
from .types import DecrackleStage as DecrackleStage
from .types import DecrackleStereoResult as DecrackleStereoResult
from .types import (
    DehumMode as DehumMode,
)
from .types import (
    DehumReport as DehumReport,
)
from .types import (
    DehumResult as DehumResult,
)
from .types import DehumStage as DehumStage
from .types import (
    DehumStereoResult as DehumStereoResult,
)
from .types import (
    DenoiseClassicalMode as DenoiseClassicalMode,
)
from .types import (
    DenoiseClassicalNoiseEstimator as DenoiseClassicalNoiseEstimator,
)
from .types import (
    DenoiseLinkedResult as DenoiseLinkedResult,
)
from .types import (
    DenoiseReport as DenoiseReport,
)
from .types import (
    DenoiseResult as DenoiseResult,
)
from .types import DenoiseStage as DenoiseStage
from .types import (
    DenoiseStereoResult as DenoiseStereoResult,
)
from .types import DereverbClassicalConfig as DereverbClassicalConfig
from .types import (
    DereverbLinkedResult as DereverbLinkedResult,
)
from .types import (
    DereverbReport as DereverbReport,
)
from .types import (
    DereverbResult as DereverbResult,
)
from .types import DereverbStage as DereverbStage
from .types import (
    DereverbStereoResult as DereverbStereoResult,
)
from .types import DynamicRangeReport as DynamicRangeReport
from .types import DynamicsProcessorResult as DynamicsProcessorResult
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
    EngineCaptureSource as EngineCaptureSource,
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
from .types import EngineProcessWithMonitorResult as EngineProcessWithMonitorResult
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
    EngineTrackMonitorMode as EngineTrackMonitorMode,
)
from .types import (
    EqBandInput as EqBandInput,
)
from .types import (
    EqPhaseMode as EqPhaseMode,
)
from .types import (
    EqSpectrumSnapshot as EqSpectrumSnapshot,
)
from .types import (
    EqStereoPlacement as EqStereoPlacement,
)
from .types import (
    ExternalMidiEvent as ExternalMidiEvent,
)
from .types import (
    FunctionalChord as FunctionalChord,
)
from .types import (
    GenreHint as GenreHint,
)
from .types import (
    GoniometerPoint as GoniometerPoint,
)
from .types import (
    HpssResult as HpssResult,
)
from .types import (
    HumDetection as HumDetection,
)
from .types import (
    Key as Key,
)
from .types import (
    KeyCandidate as KeyCandidate,
)
from .types import (
    KeyDetection as KeyDetection,
)
from .types import (
    KeyMode as KeyMode,
)
from .types import (
    KeyProfile as KeyProfile,
)
from .types import LoudnessMatchResult as LoudnessMatchResult
from .types import LoudnessMatchStereoResult as LoudnessMatchStereoResult
from .types import LufsResult as LufsResult
from .types import LufsSeriesResult as LufsSeriesResult
from .types import (
    MarkerKind as MarkerKind,
)
from .types import (
    MasteringAmpPresetCatalogEntry as MasteringAmpPresetCatalogEntry,
)
from .types import (
    MasteringAssistantProfile as MasteringAssistantProfile,
)
from .types import (
    MasteringAssistantResult as MasteringAssistantResult,
)
from .types import (
    MasteringAudioProfile as MasteringAudioProfile,
)
from .types import (
    MasteringAudioProfileDynamics as MasteringAudioProfileDynamics,
)
from .types import (
    MasteringAudioProfileLoudness as MasteringAudioProfileLoudness,
)
from .types import (
    MasteringAudioProfileSpectral as MasteringAudioProfileSpectral,
)
from .types import (
    MasteringChainConfig as MasteringChainConfig,
)
from .types import (
    MasteringChainResult as MasteringChainResult,
)
from .types import (
    MasteringChainStereoResult as MasteringChainStereoResult,
)
from .types import MasteringChannelPolicy as MasteringChannelPolicy
from .types import MasteringInsertParamChoice as MasteringInsertParamChoice
from .types import MasteringInsertParamDependency as MasteringInsertParamDependency
from .types import MasteringInsertParamInfo as MasteringInsertParamInfo
from .types import MasteringInsertParamRelation as MasteringInsertParamRelation
from .types import MasteringInsertParamScale as MasteringInsertParamScale
from .types import MasteringInsertParamUnit as MasteringInsertParamUnit
from .types import MasteringInsertSlot as MasteringInsertSlot
from .types import MasteringInsertTiming as MasteringInsertTiming
from .types import (
    MasteringLoudnessSummary as MasteringLoudnessSummary,
)
from .types import MasteringPresetCatalogEntry as MasteringPresetCatalogEntry
from .types import MasteringPresetKind as MasteringPresetKind
from .types import MasteringProcessorCatalogEntry as MasteringProcessorCatalogEntry
from .types import MasteringProcessorCategory as MasteringProcessorCategory
from .types import MasteringProcessorKind as MasteringProcessorKind
from .types import (
    MasteringRepairAnalysis as MasteringRepairAnalysis,
)
from .types import MasteringReport as MasteringReport
from .types import (
    MasteringResult as MasteringResult,
)
from .types import (
    MasteringStereoResult as MasteringStereoResult,
)
from .types import (
    MasteringStreamingPreviewPlatform as MasteringStreamingPreviewPlatform,
)
from .types import (
    MasteringStreamingPreviewResult as MasteringStreamingPreviewResult,
)
from .types import (
    MatchEqCurveResult as MatchEqCurveResult,
)
from .types import (
    MatchEstimateReferenceDelaySamplesResult as MatchEstimateReferenceDelaySamplesResult,
)
from .types import (
    MatchReferenceLoudnessResult as MatchReferenceLoudnessResult,
)
from .types import (
    MatchTonalBalanceBand as MatchTonalBalanceBand,
)
from .types import (
    MatchTonalBalanceResult as MatchTonalBalanceResult,
)
from .types import (
    MaterialPresetName as MaterialPresetName,
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
    MeterEstimate as MeterEstimate,
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
    MidiFxConfig as MidiFxConfig,
)
from .types import MixAnalysisBand as MixAnalysisBand
from .types import MixAssistantMixProfile as MixAssistantMixProfile
from .types import MixAssistantResult as MixAssistantResult
from .types import MixAssistantTrackProfile as MixAssistantTrackProfile
from .types import MixBandDominance as MixBandDominance
from .types import (
    MixBusRole as MixBusRole,
)
from .types import MixCrowdedBand as MixCrowdedBand
from .types import (
    MixMeterSnapshot as MixMeterSnapshot,
)
from .types import MixMonoRisk as MixMonoRisk
from .types import (
    MixResult as MixResult,
)
from .types import (
    MixSceneBusInput as MixSceneBusInput,
)
from .types import (
    MixSceneStripInput as MixSceneStripInput,
)
from .types import MixSourceClass as MixSourceClass
from .types import MixTrackAlignment as MixTrackAlignment
from .types import (
    NoiseDetection as NoiseDetection,
)
from .types import (
    NormalizeStereoResult as NormalizeStereoResult,
)
from .types import (
    NormType as NormType,
)
from .types import (
    NoteSegment as NoteSegment,
)
from .types import PanLaw as PanLaw
from .types import PanMode as PanMode
from .types import (
    ParameterInfo as ParameterInfo,
)
from .types import (
    PhaseScopeReport as PhaseScopeReport,
)
from .types import (
    PiptrackResult as PiptrackResult,
)
from .types import (
    PitchClass as PitchClass,
)
from .types import (
    PitchResult as PitchResult,
)
from .types import (
    PitchTargetMode as PitchTargetMode,
)
from .types import (
    ProjectClip as ProjectClip,
)
from .types import (
    ProjectMarker as ProjectMarker,
)
from .types import (
    ProjectSource as ProjectSource,
)
from .types import (
    ProjectTrack as ProjectTrack,
)
from .types import (
    ProjectWarpMode as ProjectWarpMode,
)
from .types import (
    ReassignedSpectrogramResult as ReassignedSpectrogramResult,
)
from .types import (
    RepairApplyResult as RepairApplyResult,
)
from .types import RepairStage as RepairStage
from .types import RepairStageName as RepairStageName
from .types import (
    RepairStageReports as RepairStageReports,
)
from .types import (
    ReverbDetection as ReverbDetection,
)
from .types import (
    RhythmResult as RhythmResult,
)
from .types import (
    RirDiagnostic as RirDiagnostic,
)
from .types import (
    RirResult as RirResult,
)
from .types import (
    RoomEstimate as RoomEstimate,
)
from .types import (
    RoomGeometry as RoomGeometry,
)
from .types import (
    RoomMorphResult as RoomMorphResult,
)
from .types import (
    RoomMorphStereoResult as RoomMorphStereoResult,
)
from .types import (
    ScopeTelemetryRecord as ScopeTelemetryRecord,
)
from .types import (
    Section as Section,
)
from .types import (
    SectionType as SectionType,
)
from .types import (
    SendTiming as SendTiming,
)
from .types import (
    SidechainCheck as SidechainCheck,
)
from .types import (
    SidechainRefusal as SidechainRefusal,
)
from .types import (
    SidechainSource as SidechainSource,
)
from .types import (
    SidechainSourceKind as SidechainSourceKind,
)
from .types import (
    SpectralEditMode as SpectralEditMode,
)
from .types import (
    SpectrumReport as SpectrumReport,
)
from .types import (
    StageGainReduction as StageGainReduction,
)
from .types import (
    StereoBlock as StereoBlock,
)
from .types import (
    StereoMonoCompatBand as StereoMonoCompatBand,
)
from .types import (
    StereoMonoCompatCheckLogBandsResult as StereoMonoCompatCheckLogBandsResult,
)
from .types import (
    StereoMonoCompatCheckResult as StereoMonoCompatCheckResult,
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
    StreamFrames as StreamFrames,
)
from .types import (
    StreamFramesI16 as StreamFramesI16,
)
from .types import (
    StreamFramesU8 as StreamFramesU8,
)
from .types import (
    StreamingLoudnessGain as StreamingLoudnessGain,
)
from .types import (
    StreamingPlatform as StreamingPlatform,
)
from .types import (
    StreamPatternScore as StreamPatternScore,
)
from .types import (
    StreamProgressiveEstimate as StreamProgressiveEstimate,
)
from .types import (
    StreamQuantizeConfig as StreamQuantizeConfig,
)
from .types import (
    StreamStats as StreamStats,
)
from .types import SynthParamCondition as SynthParamCondition
from .types import SynthParamInfo as SynthParamInfo
from .types import (
    TempogramMode as TempogramMode,
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
    TrimRange as TrimRange,
)
from .types import (
    TrimReport as TrimReport,
)
from .types import (
    TrimSilenceMode as TrimSilenceMode,
)
from .types import (
    TrimSilenceResult as TrimSilenceResult,
)
from .types import (
    TrimSilenceStereoResult as TrimSilenceStereoResult,
)
from .types import (
    VectorscopeReport as VectorscopeReport,
)
from .types import (
    VoicePresetId as VoicePresetId,
)
from .types import (
    WaveformPeaksReport as WaveformPeaksReport,
)
from .types import (
    WindowType as WindowType,
)
from .vocal_edit import (
    VocalAnalysis as VocalAnalysis,
)
from .vocal_edit import (
    VocalAnalysisResult as VocalAnalysisResult,
)
from .vocal_edit import (
    VocalCapabilities as VocalCapabilities,
)
from .vocal_edit import (
    VocalEditDraft as VocalEditDraft,
)
from .vocal_edit import (
    VocalEditError as VocalEditError,
)
from .vocal_edit import (
    VocalEditResult as VocalEditResult,
)
from .vocal_edit import (
    VocalEditSession as VocalEditSession,
)
from .vocal_edit import (
    VocalFormantMode as VocalFormantMode,
)
from .vocal_edit import VocalHistory as VocalHistory
from .vocal_edit import (
    VocalIdChange as VocalIdChange,
)
from .vocal_edit import (
    VocalMergeNotes as VocalMergeNotes,
)
from .vocal_edit import (
    VocalMergePolicy as VocalMergePolicy,
)
from .vocal_edit import (
    VocalNote as VocalNote,
)
from .vocal_edit import (
    VocalNoteEdit as VocalNoteEdit,
)
from .vocal_edit import VocalNotesResult as VocalNotesResult
from .vocal_edit import (
    VocalOperation as VocalOperation,
)
from .vocal_edit import (
    VocalPitchPoint as VocalPitchPoint,
)
from .vocal_edit import (
    VocalPitchResult as VocalPitchResult,
)
from .vocal_edit import (
    VocalRange as VocalRange,
)
from .vocal_edit import (
    VocalReason as VocalReason,
)
from .vocal_edit import (
    VocalRemoveTransition as VocalRemoveTransition,
)
from .vocal_edit import (
    VocalRenderJob as VocalRenderJob,
)
from .vocal_edit import (
    VocalRenderResult as VocalRenderResult,
)
from .vocal_edit import (
    VocalRenderSnapshot as VocalRenderSnapshot,
)
from .vocal_edit import (
    VocalResetNotes as VocalResetNotes,
)
from .vocal_edit import (
    VocalSetNoteEdit as VocalSetNoteEdit,
)
from .vocal_edit import (
    VocalSetNoteSourceSpan as VocalSetNoteSourceSpan,
)
from .vocal_edit import (
    VocalSetTransition as VocalSetTransition,
)
from .vocal_edit import (
    VocalSplitNote as VocalSplitNote,
)
from .vocal_edit import (
    VocalStateToken as VocalStateToken,
)
from .vocal_edit import (
    VocalTargetMode as VocalTargetMode,
)
from .vocal_edit import (
    VocalTransition as VocalTransition,
)
from .vocal_edit import (
    vocal_edit_abi_version as vocal_edit_abi_version,
)
from .vocal_project import (
    ProjectVocalEditApplyResult as ProjectVocalEditApplyResult,
)
from .vocal_project import (
    ProjectVocalEditDependency as ProjectVocalEditDependency,
)
from .vocal_project import (
    ProjectVocalOriginalSource as ProjectVocalOriginalSource,
)
from .vocal_project import (
    ProjectVocalRehydrateItem as ProjectVocalRehydrateItem,
)
from .vocal_project import (
    ProjectVocalRehydrateStatus as ProjectVocalRehydrateStatus,
)

__version__: str
"""Installed package version, or the native library version in a source checkout."""
