/**
 * The field names each request-object entry point declares.
 *
 * `requestObject` refuses any key outside its entry point's set, by name, so a
 * misspelt or misplaced field fails instead of being ignored. Each set is
 * checked against the request type at its call site (see `validation.ts`), so a
 * field added to or removed from a request type is a compile error until the
 * set here follows. This module has no imports on purpose: it is read by
 * `validation.ts`, which every request module already depends on.
 */

/** A set of field names, spelt as an object so `keyof` recovers the names. */
export type KeySet<K extends string> = { readonly [P in K]: true };

function keySet<const K extends string>(...keys: K[]): KeySet<K> {
  return Object.freeze(Object.fromEntries(keys.map((key) => [key, true]))) as KeySet<K>;
}

const ALIGN_TAKE_TO_REFERENCE_REQUEST_KEYS = keySet(
  'reference',
  'take',
  'sampleRate',
  'takeSampleRate',
  'hopLength',
  'binsPerOctave',
);
const RENDER_PLAYBACK_REQUEST_KEYS = keySet('samples', 'channels', 'sampleRate', 'config', 'hrtf');
const SAMPLES_REQUEST_KEYS = keySet('samples', 'sampleRate');
const DETECT_KEY_REQUEST_KEYS = keySet(
  'tuning',
  'nFft',
  'hopLength',
  'useHpss',
  'loudnessWeighted',
  'highPassHz',
  'modes',
  'profile',
  'genreHint',
  'samples',
  'sampleRate',
);
const DETECT_KEY_CANDIDATES_REQUEST_KEYS = keySet(
  'nFft',
  'hopLength',
  'useHpss',
  'loudnessWeighted',
  'highPassHz',
  'modes',
  'profile',
  'genreHint',
  'samples',
  'sampleRate',
);
const DETECT_ONSETS_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'threshold',
  'preMax',
  'postMax',
  'preAvg',
  'postAvg',
  'delta',
  'wait',
  'backtrack',
  'backtrackRange',
);
const MUSIC_ANALYZE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'bpmMin',
  'bpmMax',
  'startBpm',
  'useTriadsOnly',
  'useHpss',
  'chromaHighpassHz',
  'useBassWeighted',
  'chromaHopMultiplier',
  'useChordHmm',
  'useChordKeyContext',
  'chordHmmBeamWidth',
  'detectChordInversions',
  'adaptiveTempo',
  'tempoUpdateIntervalBeats',
  'computeTempoCurve',
  'meterCandidateNumerators',
  'meterDenominator',
  'tuning',
);
const ESTIMATE_METER_REQUEST_KEYS = keySet(
  'beatTimes',
  'beatStrengths',
  'candidateNumerators',
  'denominator',
  'downbeatWeight',
  'measureWeight',
  'subdivisionWeight',
  'compoundSubdivisionThreshold',
);
const RIR_SYNTH_OPTIONS_KEYS = keySet(
  'sampleRate',
  'preferEyring',
  'mixingTimeMs',
  'crossfadeMs',
  'lengthM',
  'widthM',
  'heightM',
  'absorption',
  'bandAbsorption',
  'bandScattering',
  'materialPreset',
  'sourceX',
  'sourceY',
  'sourceZ',
  'listenerX',
  'listenerY',
  'listenerZ',
  'ismOrder',
  'seed',
  'maxSeconds',
  'airAbsorptionEnabled',
  'airTemperatureC',
  'airHumidityPercent',
);
const ROOM_ESTIMATE_REQUEST_KEYS = keySet(
  'aspectHintLw',
  'aspectHintLh',
  'referenceAbsorption',
  'preferEyring',
  'nOctaveBands',
  'mode',
  'minDecayDb',
  'noiseFloorMarginDb',
  'samples',
  'sampleRate',
);
const ROOM_MORPH_REQUEST_KEYS = keySet(
  'wet',
  'sourceTailSuppression',
  'preferEyring',
  'mixingTimeMs',
  'crossfadeMs',
  'lengthM',
  'widthM',
  'heightM',
  'absorption',
  'bandAbsorption',
  'bandScattering',
  'materialPreset',
  'sourceX',
  'sourceY',
  'sourceZ',
  'listenerX',
  'listenerY',
  'listenerZ',
  'ismOrder',
  'seed',
  'maxSeconds',
  'airAbsorptionEnabled',
  'airTemperatureC',
  'airHumidityPercent',
  'samples',
  'sampleRate',
);
const ROOM_MORPH_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'receiverSpacingM',
  'wet',
  'sourceTailSuppression',
  'preferEyring',
  'mixingTimeMs',
  'crossfadeMs',
  'lengthM',
  'widthM',
  'heightM',
  'absorption',
  'bandAbsorption',
  'bandScattering',
  'materialPreset',
  'sourceX',
  'sourceY',
  'sourceZ',
  'listenerX',
  'listenerY',
  'listenerZ',
  'ismOrder',
  'seed',
  'maxSeconds',
  'airAbsorptionEnabled',
  'airTemperatureC',
  'airHumidityPercent',
);
const ANALYZE_WITH_PROGRESS_REQUEST_KEYS = keySet(
  'onProgress',
  'cancel',
  'options',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'bpmMin',
  'bpmMax',
  'startBpm',
  'useTriadsOnly',
  'useHpss',
  'chromaHighpassHz',
  'useBassWeighted',
  'chromaHopMultiplier',
  'useChordHmm',
  'useChordKeyContext',
  'chordHmmBeamWidth',
  'detectChordInversions',
  'adaptiveTempo',
  'tempoUpdateIntervalBeats',
  'computeTempoCurve',
  'meterCandidateNumerators',
  'meterDenominator',
  'tuning',
);
const ANALYZE_SECTIONS_REQUEST_KEYS = keySet(
  'nFft',
  'hopLength',
  'minSectionSec',
  'samples',
  'sampleRate',
);
const DETECT_BOUNDARIES_REQUEST_KEYS = keySet(
  'nFft',
  'hopLength',
  'kernelSize',
  'threshold',
  'absoluteThreshold',
  'nMfcc',
  'nChroma',
  'peakDistance',
  'referenceWindow',
  'useMfcc',
  'useChroma',
  'samples',
  'sampleRate',
);
const ANALYZE_MELODY_REQUEST_KEYS = keySet(
  'fmin',
  'fmax',
  'frameLength',
  'hopLength',
  'threshold',
  'usePyin',
  'center',
  'samples',
  'sampleRate',
);
const ANALYZE_BPM_REQUEST_KEYS = keySet(
  'bpmMin',
  'bpmMax',
  'startBpm',
  'nFft',
  'hopLength',
  'maxCandidates',
  'samples',
  'sampleRate',
);
const ANALYZE_RHYTHM_REQUEST_KEYS = keySet(
  'bpmMin',
  'bpmMax',
  'startBpm',
  'nFft',
  'hopLength',
  'samples',
  'sampleRate',
);
const ANALYZE_DYNAMICS_REQUEST_KEYS = keySet(
  'windowSec',
  'hopLength',
  'compressionThreshold',
  'samples',
  'sampleRate',
);
const ANALYZE_IMPULSE_RESPONSE_REQUEST_KEYS = keySet(
  'nOctaveBands',
  'minDecayDb',
  'samples',
  'sampleRate',
);
const DETECT_ACOUSTIC_REQUEST_KEYS = keySet(
  'nOctaveBands',
  'nThirdOctaveSubbands',
  'minDecayDb',
  'noiseFloorMarginDb',
  'samples',
  'sampleRate',
);
const ANALYZE_TIMBRE_REQUEST_KEYS = keySet(
  'nFft',
  'hopLength',
  'nMels',
  'nMfcc',
  'windowSec',
  'samples',
  'sampleRate',
);
const DETECT_CHORDS_REQUEST_KEYS = keySet(
  'minDuration',
  'smoothingWindow',
  'threshold',
  'useTriadsOnly',
  'nFft',
  'hopLength',
  'useBeatSync',
  'useHmm',
  'hmmBeamWidth',
  'useKeyContext',
  'keyRoot',
  'keyMode',
  'detectInversions',
  'chromaMethod',
  'tuning',
  'samples',
  'sampleRate',
);
const CHORD_FUNCTIONAL_ANALYSIS_REQUEST_KEYS = keySet(
  'keyRoot',
  'keyMode',
  'minDuration',
  'smoothingWindow',
  'threshold',
  'useTriadsOnly',
  'nFft',
  'hopLength',
  'useBeatSync',
  'useHmm',
  'hmmBeamWidth',
  'useKeyContext',
  'detectInversions',
  'chromaMethod',
  'tuning',
  'samples',
  'sampleRate',
);
const CHORD_FUNCTIONS_REQUEST_KEYS = keySet('chords', 'key');
const NOTE_STRETCH_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'onsetSample',
  'onsetSec',
  'offsetSample',
  'offsetSec',
  'stretchRatio',
);
const NOTE_MOVE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'onsetSample',
  'onsetSec',
  'offsetSample',
  'offsetSec',
  'targetOnsetSample',
  'targetOnsetSec',
);
const NOTE_TRACK_REQUEST_KEYS = keySet(
  'sampleRate',
  'f0Hz',
  'frameRate',
  'voiced',
  'voicedProb',
  'samples',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'voicedThreshold',
);
const RENDER_NOTES_REQUEST_KEYS = keySet(
  'sampleRate',
  'notes',
  'fadeMs',
  'f0Hz',
  'frameRate',
  'voiced',
  'vibratoCutoffHz',
  'samples',
);
const DECOMPOSE_NOTE_PITCH_REQUEST_KEYS = keySet(
  'f0Hz',
  'voiced',
  'frameRate',
  'medianHz',
  'vibratoCutoffHz',
);
const SPLIT_NOTE_REQUEST_KEYS = keySet(
  'notes',
  'index',
  'frame',
  'sampleRate',
  'f0Hz',
  'frameRate',
  'voiced',
  'voicedProb',
  'samples',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'voicedThreshold',
);
const MERGE_NOTES_REQUEST_KEYS = keySet(
  'notes',
  'first',
  'last',
  'sampleRate',
  'f0Hz',
  'frameRate',
  'voiced',
  'voicedProb',
  'samples',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'voicedThreshold',
);
const NOTE_TARGETS_FROM_SMF_REQUEST_KEYS = keySet('data', 'trackIndex');
const ASSIGN_NOTE_TARGETS_REQUEST_KEYS = keySet(
  'notes',
  'sampleRate',
  'targets',
  'unmatchedPolicy',
  'minOverlapRatio',
  'maxCorrectionSemitones',
);
const EXTRACT_PERCUSSIVE_EVENTS_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'onsetWait',
  'onsetDelta',
  'maxEventMs',
  'minPercussiveRatio',
  'nFft',
  'hopLength',
  'hpssKernelHarmonic',
  'hpssKernelPercussive',
);
const RENDER_PERCUSSIVE_EVENTS_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'events',
  'fadeMs',
  'nFft',
  'hopLength',
  'hpssKernelHarmonic',
  'hpssKernelPercussive',
);
const HPSS_REQUEST_KEYS = keySet(
  'kernelHarmonic',
  'kernelPercussive',
  'nFft',
  'hopLength',
  'hardMask',
  'samples',
  'sampleRate',
);
const EFFECT_SAMPLES_REQUEST_KEYS = keySet('samples', 'sampleRate');
const SPECTRAL_EDIT_REQUEST_KEYS = keySet(
  'sampleRate',
  'ops',
  'samples',
  'nFft',
  'hopLength',
  'window',
  'healRadiusFrames',
);
const TIME_STRETCH_REQUEST_KEYS = keySet('rate', 'nFft', 'hopLength', 'samples', 'sampleRate');
const PITCH_SHIFT_REQUEST_KEYS = keySet('semitones', 'nFft', 'hopLength', 'samples', 'sampleRate');
const PITCH_CORRECT_TO_MIDI_REQUEST_KEYS = keySet(
  'currentMidi',
  'targetMidi',
  'samples',
  'sampleRate',
);
const PITCH_CORRECT_TO_MIDI_TIMEVARYING_REQUEST_KEYS = keySet(
  'f0Hz',
  'targetMidi',
  'hopLength',
  'voiced',
  'voicedProb',
  'samples',
  'sampleRate',
);
const PITCH_CORRECT_TIMEVARYING_REQUEST_KEYS = keySet(
  'f0Hz',
  'hopLength',
  'samples',
  'sampleRate',
  'mode',
  'targetMidi',
  'scaleRoot',
  'scaleModeMask',
  'referenceMidi',
  'retuneAmount',
  'maxCorrectionSemitones',
  'retuneSpeedMs',
  'vibratoThresholdCents',
  'voiced',
  'voicedProb',
);
const AUTO_TUNE_REQUEST_KEYS = keySet(
  'key',
  'samples',
  'sampleRate',
  'strength',
  'retuneSpeedMs',
  'vibratoThresholdCents',
  'maxCorrectionSemitones',
  'referenceMidi',
);
const MASTERING_ASSISTANT_SUGGEST_REQUEST_KEYS = keySet('samples', 'sampleRate', 'params');
const MASTERING_AUDIO_PROFILE_REQUEST_KEYS = keySet('samples', 'sampleRate', 'params');
const MASTERING_STREAMING_PREVIEW_REQUEST_KEYS = keySet('samples', 'sampleRate', 'platforms');
const NORMALIZE_REQUEST_KEYS = keySet('samples', 'sampleRate', 'targetDb', 'mode');
const NORMALIZE_STEREO_REQUEST_KEYS = keySet('left', 'right', 'sampleRate', 'targetDb', 'mode');
const MASTERING_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'targetLufs',
  'ceilingDb',
  'truePeakOversample',
  'releaseMs',
  'applyGainAtInputRate',
);
const MASTERING_PROCESS_REQUEST_KEYS = keySet('processorName', 'samples', 'sampleRate', 'params');
const MASTERING_PROCESS_STEREO_REQUEST_KEYS = keySet(
  'processorName',
  'left',
  'right',
  'sampleRate',
  'params',
);
const MASTERING_CHAIN_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'config',
  'onProgress',
  'cancel',
);
const MASTERING_CHAIN_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'config',
  'onProgress',
  'cancel',
);
const STREAMING_LOUDNESS_GAIN_REQUEST_KEYS = keySet('samples', 'sampleRate', 'config');
const STREAMING_LOUDNESS_GAIN_STEREO_REQUEST_KEYS = keySet('left', 'right', 'sampleRate', 'config');
const MASTERING_PAIR_PROCESS_REQUEST_KEYS = keySet(
  'processorName',
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
  'params',
);
const MASTERING_PAIR_PROCESS_STEREO_REQUEST_KEYS = keySet(
  'processorName',
  'sourceLeft',
  'sourceRight',
  'referenceLeft',
  'referenceRight',
  'sampleRate',
  'referenceSampleRate',
  'params',
);
const MASTERING_AB_MATCH_LOUDNESS_REQUEST_KEYS = keySet(
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
);
const MASTERING_AB_MATCH_LOUDNESS_STEREO_REQUEST_KEYS = keySet(
  'sourceLeft',
  'sourceRight',
  'referenceLeft',
  'referenceRight',
  'sampleRate',
  'referenceSampleRate',
);
const MASTERING_PAIR_ANALYZE_REQUEST_KEYS = keySet(
  'analysisName',
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
  'params',
);
const MASTERING_STEREO_ANALYZE_REQUEST_KEYS = keySet(
  'analysisName',
  'left',
  'right',
  'sampleRate',
  'params',
);
const MASTERING_DYNAMICS_COMPRESSOR_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'thresholdDb',
  'ratio',
  'attackMs',
  'releaseMs',
  'kneeDb',
  'makeupGainDb',
  'autoMakeup',
  'detector',
  'sidechainHpfEnabled',
  'sidechainHpfHz',
  'pdrTimeMs',
  'pdrReleaseScale',
  'validate',
);
const MASTERING_DYNAMICS_GATE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'thresholdDb',
  'attackMs',
  'releaseMs',
  'rangeDb',
  'holdMs',
  'closeThresholdDb',
  'keyHpfHz',
  'validate',
);
const MASTERING_DYNAMICS_TRANSIENT_SHAPER_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'attackGainDb',
  'sustainGainDb',
  'fastAttackMs',
  'fastReleaseMs',
  'slowAttackMs',
  'slowReleaseMs',
  'sensitivity',
  'maxGainDb',
  'gainSmoothingMs',
  'lookaheadMs',
  'validate',
);
const MASTERING_REPAIR_DEREVERB_CLASSICAL_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'threshold',
  'attenuation',
  'nFft',
  'hopLength',
  't60Sec',
  'lateDelayMs',
  'overSubtraction',
  'spectralFloor',
  'wpeEnabled',
  'wpeIterations',
  'wpeTaps',
  'wpeStrength',
);
const MASTERING_REPAIR_DEREVERB_CLASSICAL_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'threshold',
  'attenuation',
  'nFft',
  'hopLength',
  't60Sec',
  'lateDelayMs',
  'overSubtraction',
  'spectralFloor',
  'wpeEnabled',
  'wpeIterations',
  'wpeTaps',
  'wpeStrength',
);
const MASTERING_REPAIR_DEREVERB_CLASSICAL_LINKED_REQUEST_KEYS = keySet(
  'channels',
  'sampleRate',
  'threshold',
  'attenuation',
  'nFft',
  'hopLength',
  't60Sec',
  'lateDelayMs',
  'overSubtraction',
  'spectralFloor',
  'wpeEnabled',
  'wpeIterations',
  'wpeTaps',
  'wpeStrength',
);
const MASTERING_REPAIR_DETECT_REVERB_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'threshold',
  'attenuation',
  'nFft',
  'hopLength',
  't60Sec',
  'lateDelayMs',
  'overSubtraction',
  'spectralFloor',
  'wpeEnabled',
  'wpeIterations',
  'wpeTaps',
  'wpeStrength',
);
const MASTERING_REPAIR_DECLICK_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'threshold',
  'neighborRatio',
  'maxClickSamples',
  'lpcOrder',
  'residualRatio',
);
const MASTERING_REPAIR_DECLICK_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'threshold',
  'neighborRatio',
  'maxClickSamples',
  'lpcOrder',
  'residualRatio',
);
const MASTERING_REPAIR_DECLIP_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'clipThreshold',
  'lpcOrder',
  'iterations',
  'lpcBlend',
);
const MASTERING_REPAIR_DECLIP_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'clipThreshold',
  'lpcOrder',
  'iterations',
  'lpcBlend',
);
const MASTERING_REPAIR_DECRACKLE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'threshold',
  'mode',
  'levels',
);
const MASTERING_REPAIR_DECRACKLE_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'threshold',
  'mode',
  'levels',
);
const MASTERING_REPAIR_DETECT_CLICKS_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'threshold',
  'neighborRatio',
  'maxClickSamples',
  'lpcOrder',
  'residualRatio',
);
const MASTERING_REPAIR_DETECT_CLIPPING_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'clipThreshold',
  'lpcOrder',
  'iterations',
  'lpcBlend',
);
const MASTERING_REPAIR_DETECT_CRACKLE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'threshold',
  'mode',
  'levels',
);
const MASTERING_REPAIR_DENOISE_CLASSICAL_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'mode',
  'noiseEstimator',
  'nFft',
  'hopLength',
  'ddAlpha',
  'reductionDb',
  'overSubtraction',
  'spectralFloor',
  'noiseEstimationQuantile',
  'speechPresenceGain',
  'gainSmoothing',
);
const MASTERING_REPAIR_DENOISE_CLASSICAL_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'mode',
  'noiseEstimator',
  'nFft',
  'hopLength',
  'ddAlpha',
  'reductionDb',
  'overSubtraction',
  'spectralFloor',
  'noiseEstimationQuantile',
  'speechPresenceGain',
  'gainSmoothing',
);
const MASTERING_REPAIR_DENOISE_CLASSICAL_LINKED_REQUEST_KEYS = keySet(
  'channels',
  'sampleRate',
  'mode',
  'noiseEstimator',
  'nFft',
  'hopLength',
  'ddAlpha',
  'reductionDb',
  'overSubtraction',
  'spectralFloor',
  'noiseEstimationQuantile',
  'speechPresenceGain',
  'gainSmoothing',
);
const MASTERING_REPAIR_DEHUM_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'fundamentalHz',
  'harmonics',
  'q',
  'adaptive',
  'searchRangeHz',
  'adaptation',
  'frameSize',
  'pllBandwidth',
  'mode',
);
const MASTERING_REPAIR_DEHUM_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'fundamentalHz',
  'harmonics',
  'q',
  'adaptive',
  'searchRangeHz',
  'adaptation',
  'frameSize',
  'pllBandwidth',
  'mode',
);
const MASTERING_REPAIR_DETECT_NOISE_FLOOR_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'mode',
  'noiseEstimator',
  'nFft',
  'hopLength',
  'ddAlpha',
  'reductionDb',
  'overSubtraction',
  'spectralFloor',
  'noiseEstimationQuantile',
  'speechPresenceGain',
  'gainSmoothing',
);
const MASTERING_REPAIR_NOISE_BAND_BINS_REQUEST_KEYS = keySet('nFft', 'sampleRate');
const MASTERING_REPAIR_DETECT_HUM_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'fundamentalHz',
  'harmonics',
  'q',
  'adaptive',
  'searchRangeHz',
  'adaptation',
  'frameSize',
  'pllBandwidth',
  'mode',
);
const MASTERING_REPAIR_ANALYZE_REQUEST_KEYS = keySet(
  'channels',
  'sampleRate',
  'preferStreamingSafe',
);
const MASTERING_REPAIR_APPLY_REQUEST_KEYS = keySet(
  'channels',
  'sampleRate',
  'stages',
  'onProgress',
  'cancel',
);
const MASTERING_REPAIR_TRIM_SILENCE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'threshold',
  'paddingSamples',
  'paddingSec',
  'mode',
  'gateLufs',
  'windowMs',
);
const MASTERING_REPAIR_TRIM_SILENCE_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'threshold',
  'paddingSamples',
  'paddingSec',
  'mode',
  'gateLufs',
  'windowMs',
);
const MASTERING_REPAIR_DETECT_TRIM_RANGE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'threshold',
  'paddingSamples',
  'paddingSec',
  'mode',
  'gateLufs',
  'windowMs',
);
const MASTERING_REPAIR_DETECT_TRIM_RANGE_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'threshold',
  'paddingSamples',
  'paddingSec',
  'mode',
  'gateLufs',
  'windowMs',
);
const VOICE_CHANGE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'pitchSemitones',
  'formantFactor',
  'formantMode',
  'validate',
);
const VOICE_CHANGE_REALTIME_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'preset',
  'channels',
  'validate',
);
const FRAMES_TO_TIME_REQUEST_KEYS = keySet('frames', 'sr', 'hopLength');
const TIME_TO_FRAMES_REQUEST_KEYS = keySet('time', 'sr', 'hopLength');
const FRAMES_TO_SAMPLES_REQUEST_KEYS = keySet('frames', 'hopLength', 'nFft');
const SAMPLES_TO_FRAMES_REQUEST_KEYS = keySet('samples', 'hopLength', 'nFft');
const POWER_TO_DB_REQUEST_KEYS = keySet('values', 'ref', 'amin', 'topDb');
const STFT_REQUEST_KEYS = keySet('nFft', 'hopLength', 'samples', 'sampleRate');
const MEL_SPECTROGRAM_REQUEST_KEYS = keySet(
  'nMels',
  'fmin',
  'fmax',
  'htk',
  'nFft',
  'hopLength',
  'samples',
  'sampleRate',
);
const MFCC_REQUEST_KEYS = keySet(
  'nMfcc',
  'lifter',
  'nMels',
  'fmin',
  'fmax',
  'htk',
  'nFft',
  'hopLength',
  'samples',
  'sampleRate',
);
const MEL_DELTA_REQUEST_KEYS = keySet('features', 'nFeatures', 'nFrames', 'width');
const REASSIGNED_SPECTROGRAM_REQUEST_KEYS = keySet(
  'refPower',
  'fillNan',
  'nFft',
  'hopLength',
  'samples',
  'sampleRate',
);
const SPECTRAL_CONTRAST_REQUEST_KEYS = keySet(
  'nBands',
  'fmin',
  'quantile',
  'nFft',
  'hopLength',
  'samples',
  'sampleRate',
);
const POLY_FEATURES_REQUEST_KEYS = keySet('order', 'nFft', 'hopLength', 'samples', 'sampleRate');
const ZERO_CROSSINGS_REQUEST_KEYS = keySet(
  'samples',
  'threshold',
  'refMagnitude',
  'pad',
  'zeroPos',
);
const SPECTRAL_BANDWIDTH_REQUEST_KEYS = keySet('nFft', 'hopLength', 'samples', 'sampleRate', 'p');
const SPECTRAL_ROLLOFF_REQUEST_KEYS = keySet(
  'nFft',
  'hopLength',
  'samples',
  'sampleRate',
  'rollPercent',
);
const SPECTRAL_FLUX_REQUEST_KEYS = keySet('nFft', 'hopLength', 'samples', 'sampleRate', 'lag');
const ZERO_CROSSING_RATE_REQUEST_KEYS = keySet('samples', 'sampleRate', 'frameLength', 'hopLength');
const PCEN_REQUEST_KEYS = keySet(
  'values',
  'nBins',
  'nFrames',
  'sampleRate',
  'hopLength',
  'timeConstant',
  'gain',
  'bias',
  'power',
  'eps',
);
const SEGMENT_CROSS_SIMILARITY_REQUEST_KEYS = keySet(
  'x',
  'xRows',
  'xCols',
  'y',
  'yRows',
  'yCols',
  'k',
  'metric',
  'mode',
);
const SEGMENT_RECURRENCE_MATRIX_REQUEST_KEYS = keySet(
  'data',
  'rows',
  'cols',
  'k',
  'width',
  'sym',
  'metric',
  'mode',
);
const SEGMENT_RECURRENCE_TO_LAG_REQUEST_KEYS = keySet('recurrence', 'n', 'pad');
const SEGMENT_LAG_TO_RECURRENCE_REQUEST_KEYS = keySet('lag', 'rows', 'lags');
const SEGMENT_SUBSEGMENT_REQUEST_KEYS = keySet('data', 'rows', 'cols', 'boundaries', 'nSegments');
const SEGMENT_AGGLOMERATIVE_REQUEST_KEYS = keySet('data', 'rows', 'cols', 'k', 'linkage');
const SEGMENT_PATH_ENHANCE_REQUEST_KEYS = keySet(
  'recurrence',
  'n',
  'win',
  'maxRatio',
  'minRatio',
  'nFilters',
);
const DECOMPOSE_REQUEST_KEYS = keySet(
  's',
  'nFeatures',
  'nFrames',
  'nComponents',
  'nIter',
  'beta',
  'init',
);
const DECOMPOSE_STEMS_REQUEST_KEYS = keySet(
  'nComponents',
  'nFft',
  'hopLength',
  'nIter',
  'beta',
  'init',
  'maskPower',
  'samples',
  'sampleRate',
);
const DECOMPOSE_STEMS_LINKED_REQUEST_KEYS = keySet(
  'channels',
  'sampleRate',
  'nComponents',
  'nFft',
  'hopLength',
  'nIter',
  'beta',
  'init',
  'maskPower',
);
const NN_FILTER_REQUEST_KEYS = keySet('s', 'nFeatures', 'nFrames', 'aggregate', 'k', 'width');
const REMIX_REQUEST_KEYS = keySet('intervals', 'alignZeros', 'samples', 'sampleRate');
const HPSS_WITH_RESIDUAL_REQUEST_KEYS = keySet(
  'kernelHarmonic',
  'kernelPercussive',
  'nFft',
  'hopLength',
  'hardMask',
  'samples',
  'sampleRate',
);
const TRIM_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'thresholdDb',
  'frameLength',
  'hopLength',
);
const EMPHASIS_REQUEST_KEYS = keySet('samples', 'coef', 'zi');
const TRIM_SILENCE_REQUEST_KEYS = keySet('samples', 'topDb', 'frameLength', 'hopLength');
const SPLIT_SILENCE_COMMON_REQUEST_KEYS = keySet('signals', 'topDb', 'frameLength', 'hopLength');
const FRAME_SIGNAL_REQUEST_KEYS = keySet('samples', 'frameLength', 'hopLength');
const PAD_CENTER_REQUEST_KEYS = keySet('values', 'targetSize', 'padValue');
const FIX_FRAMES_REQUEST_KEYS = keySet('frames', 'xMin', 'xMax', 'pad');
const VECTOR_NORMALIZE_REQUEST_KEYS = keySet('values', 'normType', 'threshold');
const CQT_TO_AUDIO_REQUEST_KEYS = keySet(
  'magnitude',
  'nBins',
  'nFrames',
  'sampleRate',
  'hopLength',
  'fmin',
  'binsPerOctave',
  'nIter',
);
const VQT_TO_AUDIO_REQUEST_KEYS = keySet(
  'gamma',
  'magnitude',
  'nBins',
  'nFrames',
  'sampleRate',
  'hopLength',
  'fmin',
  'binsPerOctave',
  'nIter',
);
const GRIFFIN_LIM_REQUEST_KEYS = keySet(
  'magnitude',
  'nBins',
  'nFrames',
  'sampleRate',
  'nFft',
  'hopLength',
  'nIter',
  'momentum',
);
const PHASE_VOCODER_REQUEST_KEYS = keySet('rate', 'nFft', 'hopLength', 'samples', 'sampleRate');
const MEL_TO_STFT_REQUEST_KEYS = keySet(
  'power',
  'result',
  'nMels',
  'nFrames',
  'sampleRate',
  'nFft',
  'fmin',
  'fmax',
  'htk',
);
const MEL_TO_AUDIO_REQUEST_KEYS = keySet(
  'power',
  'result',
  'nMels',
  'nFrames',
  'sampleRate',
  'nFft',
  'hopLength',
  'fmin',
  'fmax',
  'htk',
  'nIter',
);
const MFCC_TO_MEL_REQUEST_KEYS = keySet(
  'coefficients',
  'result',
  'nMfcc',
  'nFrames',
  'nMels',
  'lifter',
);
const MFCC_TO_AUDIO_REQUEST_KEYS = keySet(
  'coefficients',
  'result',
  'nMfcc',
  'nFrames',
  'nMels',
  'lifter',
  'sampleRate',
  'nFft',
  'hopLength',
  'fmin',
  'fmax',
  'htk',
  'nIter',
);
const TONE_REQUEST_KEYS = keySet('frequency', 'sampleRate', 'duration', 'phase', 'amplitude');
const CHIRP_REQUEST_KEYS = keySet('fmin', 'fmax', 'sampleRate', 'duration', 'linear');
const CLICKS_REQUEST_KEYS = keySet('times', 'sampleRate', 'length', 'frequency', 'clickDuration');
const LUFS_INTERLEAVED_REQUEST_KEYS = keySet('samples', 'sampleRate', 'channels');
const FEATURE_SAMPLES_REQUEST_KEYS = keySet('samples', 'sampleRate');
const LUFS_REQUEST_KEYS = keySet('samples', 'sampleRate', 'validate');
const CHROMA_REQUEST_KEYS = keySet(
  'hopLength',
  'nChroma',
  'binsPerOctave',
  'samples',
  'sampleRate',
);
const BASS_CHROMA_REQUEST_KEYS = keySet('hopLength', 'nChroma', 'samples', 'sampleRate');
const CQT_REQUEST_KEYS = keySet(
  'hopLength',
  'fmin',
  'nBins',
  'binsPerOctave',
  'samples',
  'sampleRate',
);
const VQT_REQUEST_KEYS = keySet(
  'gamma',
  'hopLength',
  'fmin',
  'nBins',
  'binsPerOctave',
  'samples',
  'sampleRate',
);
const PITCH_TUNING_REQUEST_KEYS = keySet('frequencies', 'resolution', 'binsPerOctave');
const ESTIMATE_TUNING_REQUEST_KEYS = keySet(
  'resolution',
  'binsPerOctave',
  'nFft',
  'hopLength',
  'samples',
  'sampleRate',
);
const PIPTRACK_REQUEST_KEYS = keySet(
  'fmin',
  'fmax',
  'threshold',
  'nFft',
  'hopLength',
  'samples',
  'sampleRate',
);
const PITCH_REQUEST_KEYS = keySet(
  'frameLength',
  'hopLength',
  'fmin',
  'fmax',
  'threshold',
  'fillNa',
  'samples',
  'sampleRate',
);
const NOTE_SEGMENTS_REQUEST_KEYS = keySet(
  'f0Hz',
  'voicedProb',
  'frameRate',
  'config',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'voicedThreshold',
);
const TONNETZ_REQUEST_KEYS = keySet('chromagram', 'nChroma', 'nFrames');
const NNLS_CHROMA_REQUEST_KEYS = keySet(
  'enableStftBlend',
  'stftBlendWeight',
  'stftBlendNFft',
  'hopLength',
  'samples',
  'sampleRate',
);
const ONSET_BACKTRACK_REQUEST_KEYS = keySet('events', 'energy');
const PEAK_PICK_REQUEST_KEYS = keySet(
  'values',
  'preMax',
  'postMax',
  'preAvg',
  'postAvg',
  'delta',
  'wait',
);
const TEMPOGRAM_REQUEST_KEYS = keySet(
  'onsetEnvelope',
  'sampleRate',
  'hopLength',
  'winLength',
  'mode',
  'center',
  'norm',
);
const CYCLIC_TEMPOGRAM_REQUEST_KEYS = keySet(
  'onsetEnvelope',
  'sampleRate',
  'hopLength',
  'winLength',
  'bpmMin',
  'nBins',
);
const PLP_REQUEST_KEYS = keySet(
  'onsetEnvelope',
  'sampleRate',
  'hopLength',
  'tempoMin',
  'tempoMax',
  'winLength',
);
const ONSET_ENVELOPE_REQUEST_KEYS = keySet('nMels', 'nFft', 'hopLength', 'samples', 'sampleRate');
const ONSET_STRENGTH_MULTI_REQUEST_KEYS = keySet(
  'nBands',
  'nMels',
  'nFft',
  'hopLength',
  'samples',
  'sampleRate',
);
const TEMPOGRAM_RATIO_REQUEST_KEYS = keySet(
  'tempogramData',
  'winLength',
  'sampleRate',
  'hopLength',
  'factors',
);
const METERING_SAMPLES_REQUEST_KEYS = keySet('samples', 'sampleRate', 'validate');
const METERING_SILENCE_RATIO_REQUEST_KEYS = keySet(
  'thresholdDb',
  'frameLength',
  'hopLength',
  'samples',
  'sampleRate',
  'validate',
);
const METERING_STEREO_REQUEST_KEYS = keySet('left', 'right', 'sampleRate', 'validate');
const METERING_TRUE_PEAK_REQUEST_KEYS = keySet(
  'oversampleFactor',
  'samples',
  'sampleRate',
  'validate',
);
const METERING_DETECT_CLIPPING_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'validate',
  'threshold',
  'minRegionSamples',
);
const METERING_DYNAMIC_RANGE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'validate',
  'windowSec',
  'hopSec',
  'lowPercentile',
  'highPercentile',
);
const METERING_SCOPE_REQUEST_KEYS = keySet('left', 'right', 'sampleRate', 'validate', 'maxPoints');
const METERING_SPECTRUM_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'validate',
  'nFft',
  'applyOctaveSmoothing',
  'octaveFraction',
  'dbRef',
  'dbAmin',
);
const METERING_SPECTRUM_FRAME_REQUEST_KEYS = keySet(
  'frameOffset',
  'samples',
  'sampleRate',
  'validate',
  'nFft',
  'applyOctaveSmoothing',
  'octaveFraction',
  'dbRef',
  'dbAmin',
);
const WAVEFORM_PEAKS_REQUEST_KEYS = keySet('samples', 'channels', 'samplesPerBucket', 'validate');
const WAVEFORM_PEAK_PYRAMID_REQUEST_KEYS = keySet(
  'samples',
  'channels',
  'samplesPerBucketLevels',
  'validate',
);
const RESAMPLE_REQUEST_KEYS = keySet('samples', 'srcSr', 'targetSr');
const MIX_STEREO_REQUEST_KEYS = keySet(
  'leftChannels',
  'rightChannels',
  'sampleRate',
  'inputTrimDb',
  'faderDb',
  'pan',
  'panMode',
  'width',
  'muted',
);
const ANALYZE_POLYPHONIC_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'winLength',
  'centRefHz',
  'centsPerBin',
  'centMaxHz',
  'tonalityOff',
  'salienceHarmonics',
  'f0MinHz',
  'f0MaxHz',
  'salienceAlphaHz',
  'salienceBetaHz',
  'salienceInharmonicity',
  'maxPolyphony',
  'minFramePeakRatio',
  'minSeparationCents',
  'subtractionFactor',
  'maxJumpCents',
  'minRidgePeakRatio',
  'minRidgeDurationMs',
  'maskHarmonics',
  'claimLobes',
  'inharmonicity',
  'estimateInharmonicity',
  'inharmonicityMinPartials',
  'inharmonicityMaxResidualBins',
  'inharmonicityMaxStretch',
  'windowFrames',
  'minPartialSeparation',
  'maxFitResidual',
  'maxWeightModulus',
  'maxRefineHz',
  'f0ToleranceCents',
  'segmentationThresholdCents',
  'minNoteMs',
  'reattackRatio',
  'referenceHz',
);
const SUGGEST_MIX_SCENE_REQUEST_KEYS = keySet('tracks', 'sampleRate', 'options');
const TRANSCRIBE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'tempoBpm',
  'polyphonic',
  'referenceHz',
  'fmin',
  'fmax',
  'minNoteMs',
  'minNoteDivision',
  'segmentationThresholdCents',
  'velocityFloorDb',
  'fixedVelocity',
  'maxPolyphony',
  'minFramePeakRatio',
  'minRidgePeakRatio',
  'reattackRatio',
  'group',
  'channel',
);
const VOCAL_RESTORE_REQUEST_KEYS = keySet('samples', 'sampleRate', 'state', 'limits');
const MASTER_AUDIO_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'preset',
  'overrides',
  'onProgress',
  'cancel',
);
const MASTER_AUDIO_ASYNC_REQUEST_KEYS = keySet('samples', 'sampleRate', 'preset', 'overrides');
const MASTER_AUDIO_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'preset',
  'overrides',
  'onProgress',
  'cancel',
);

const MASTER_AUDIO_STEREO_ASYNC_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'preset',
  'overrides',
);

export const MUSIC_ANALYZE_OPTIONS_KEYS = keySet(
  'nFft',
  'hopLength',
  'bpmMin',
  'bpmMax',
  'startBpm',
  'useTriadsOnly',
  'useHpss',
  'chromaHighpassHz',
  'useBassWeighted',
  'chromaHopMultiplier',
  'useChordHmm',
  'useChordKeyContext',
  'chordHmmBeamWidth',
  'detectChordInversions',
  'adaptiveTempo',
  'tempoUpdateIntervalBeats',
  'computeTempoCurve',
  'meterCandidateNumerators',
  'meterDenominator',
  'tuning',
);
const NOTE_SEGMENTS_CONFIG_KEYS = keySet(
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'voicedThreshold',
);
const VOCAL_SESSION_LIMITS_KEYS = keySet(
  'maxHistoryBytes',
  'maxCacheBytes',
  'maxUndoDepth',
  'maxRenderJobs',
);
const CHAIN_CONFIG_SECTIONS = keySet(
  'repair',
  'eq',
  'dynamics',
  'saturation',
  'spectral',
  'stereo',
  'maximizer',
  'loudness',
);

const MASTERING_ASSISTANT_SUGGEST_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'params',
);
const MASTERING_AUDIO_PROFILE_STEREO_REQUEST_KEYS = keySet('left', 'right', 'sampleRate', 'params');
const MASTERING_STREAMING_PREVIEW_STEREO_REQUEST_KEYS = keySet(
  'left',
  'right',
  'sampleRate',
  'platforms',
);
const ROOM_GEOMETRY_FROM_ESTIMATE_REQUEST_KEYS = keySet('estimate', 'source', 'listener');
const VOCAL_CREATE_REQUEST_KEYS = keySet(
  'samples',
  'sampleRate',
  'outputLengthSamples',
  'edgeFadeMs',
  'vibratoCutoffHz',
  'segmentationThresholdCents',
  'minNoteMs',
  'frameLengthSamples',
  'hopLengthSamples',
  'fminHz',
  'fmaxHz',
  'yinThreshold',
  'voicedThreshold',
  'centered',
  'referenceHz',
  'limits',
  'analysis',
);

const PLAYBACK_RENDERER_CONFIG_KEYS = keySet(
  'input',
  'target',
  'lfe_mix_db',
  'upmix',
  'dialogue_level_db',
  'loudness',
  'night_mode',
  'room',
  'head_tracking',
  'output_limiter',
);

export const REQUEST_KEYS = {
  alignTakeToReference: ALIGN_TAKE_TO_REFERENCE_REQUEST_KEYS,
  renderPlayback: RENDER_PLAYBACK_REQUEST_KEYS,
  detectBpm: SAMPLES_REQUEST_KEYS,
  detectKey: DETECT_KEY_REQUEST_KEYS,
  detectKeyCandidates: DETECT_KEY_CANDIDATES_REQUEST_KEYS,
  detectBeats: SAMPLES_REQUEST_KEYS,
  detectDownbeats: SAMPLES_REQUEST_KEYS,
  detectOnsets: DETECT_ONSETS_REQUEST_KEYS,
  analyze: MUSIC_ANALYZE_REQUEST_KEYS,
  estimateMeter: ESTIMATE_METER_REQUEST_KEYS,
  synthesizeRir: RIR_SYNTH_OPTIONS_KEYS,
  estimateRoom: ROOM_ESTIMATE_REQUEST_KEYS,
  roomMorph: ROOM_MORPH_REQUEST_KEYS,
  roomMorphStereo: ROOM_MORPH_STEREO_REQUEST_KEYS,
  analyzeWithProgress: ANALYZE_WITH_PROGRESS_REQUEST_KEYS,
  analyzeSections: ANALYZE_SECTIONS_REQUEST_KEYS,
  detectBoundaries: DETECT_BOUNDARIES_REQUEST_KEYS,
  analyzeMelody: ANALYZE_MELODY_REQUEST_KEYS,
  analyzeBpm: ANALYZE_BPM_REQUEST_KEYS,
  analyzeRhythm: ANALYZE_RHYTHM_REQUEST_KEYS,
  analyzeDynamics: ANALYZE_DYNAMICS_REQUEST_KEYS,
  analyzeImpulseResponse: ANALYZE_IMPULSE_RESPONSE_REQUEST_KEYS,
  detectAcoustic: DETECT_ACOUSTIC_REQUEST_KEYS,
  analyzeTimbre: ANALYZE_TIMBRE_REQUEST_KEYS,
  detectChords: DETECT_CHORDS_REQUEST_KEYS,
  chordFunctionalAnalysis: CHORD_FUNCTIONAL_ANALYSIS_REQUEST_KEYS,
  chordFunctions: CHORD_FUNCTIONS_REQUEST_KEYS,
  noteStretch: NOTE_STRETCH_REQUEST_KEYS,
  noteMove: NOTE_MOVE_REQUEST_KEYS,
  extractNotes: NOTE_TRACK_REQUEST_KEYS,
  renderNotes: RENDER_NOTES_REQUEST_KEYS,
  decomposeNotePitch: DECOMPOSE_NOTE_PITCH_REQUEST_KEYS,
  splitNote: SPLIT_NOTE_REQUEST_KEYS,
  mergeNotes: MERGE_NOTES_REQUEST_KEYS,
  noteTargetsFromSmf: NOTE_TARGETS_FROM_SMF_REQUEST_KEYS,
  assignNoteTargets: ASSIGN_NOTE_TARGETS_REQUEST_KEYS,
  extractPercussiveEvents: EXTRACT_PERCUSSIVE_EVENTS_REQUEST_KEYS,
  renderPercussiveEvents: RENDER_PERCUSSIVE_EVENTS_REQUEST_KEYS,
  hpss: HPSS_REQUEST_KEYS,
  harmonic: EFFECT_SAMPLES_REQUEST_KEYS,
  percussive: EFFECT_SAMPLES_REQUEST_KEYS,
  spectralEdit: SPECTRAL_EDIT_REQUEST_KEYS,
  timeStretch: TIME_STRETCH_REQUEST_KEYS,
  pitchShift: PITCH_SHIFT_REQUEST_KEYS,
  pitchCorrectToMidi: PITCH_CORRECT_TO_MIDI_REQUEST_KEYS,
  pitchCorrectToMidiTimevarying: PITCH_CORRECT_TO_MIDI_TIMEVARYING_REQUEST_KEYS,
  pitchCorrectTimevarying: PITCH_CORRECT_TIMEVARYING_REQUEST_KEYS,
  autoTune: AUTO_TUNE_REQUEST_KEYS,
  masteringAssistantSuggest: MASTERING_ASSISTANT_SUGGEST_REQUEST_KEYS,
  masteringAssistantSuggestChain: MASTERING_ASSISTANT_SUGGEST_REQUEST_KEYS,
  masteringAudioProfile: MASTERING_AUDIO_PROFILE_REQUEST_KEYS,
  masteringStreamingPreview: MASTERING_STREAMING_PREVIEW_REQUEST_KEYS,
  normalize: NORMALIZE_REQUEST_KEYS,
  normalizeStereo: NORMALIZE_STEREO_REQUEST_KEYS,
  mastering: MASTERING_REQUEST_KEYS,
  masteringProcess: MASTERING_PROCESS_REQUEST_KEYS,
  masteringProcessStereo: MASTERING_PROCESS_STEREO_REQUEST_KEYS,
  masteringChain: MASTERING_CHAIN_REQUEST_KEYS,
  masteringChainStereo: MASTERING_CHAIN_STEREO_REQUEST_KEYS,
  streamingLoudnessGain: STREAMING_LOUDNESS_GAIN_REQUEST_KEYS,
  streamingLoudnessGainStereo: STREAMING_LOUDNESS_GAIN_STEREO_REQUEST_KEYS,
  masteringPairProcess: MASTERING_PAIR_PROCESS_REQUEST_KEYS,
  masteringPairProcessStereo: MASTERING_PAIR_PROCESS_STEREO_REQUEST_KEYS,
  masteringAbMatchLoudness: MASTERING_AB_MATCH_LOUDNESS_REQUEST_KEYS,
  masteringAbMatchLoudnessStereo: MASTERING_AB_MATCH_LOUDNESS_STEREO_REQUEST_KEYS,
  masteringPairAnalyze: MASTERING_PAIR_ANALYZE_REQUEST_KEYS,
  masteringStereoAnalyze: MASTERING_STEREO_ANALYZE_REQUEST_KEYS,
  masteringDynamicsCompressor: MASTERING_DYNAMICS_COMPRESSOR_REQUEST_KEYS,
  masteringDynamicsGate: MASTERING_DYNAMICS_GATE_REQUEST_KEYS,
  masteringDynamicsTransientShaper: MASTERING_DYNAMICS_TRANSIENT_SHAPER_REQUEST_KEYS,
  masteringRepairDereverbClassical: MASTERING_REPAIR_DEREVERB_CLASSICAL_REQUEST_KEYS,
  masteringRepairDereverbClassicalStereo: MASTERING_REPAIR_DEREVERB_CLASSICAL_STEREO_REQUEST_KEYS,
  masteringRepairDereverbClassicalLinked: MASTERING_REPAIR_DEREVERB_CLASSICAL_LINKED_REQUEST_KEYS,
  masteringRepairDetectReverb: MASTERING_REPAIR_DETECT_REVERB_REQUEST_KEYS,
  masteringRepairDeclick: MASTERING_REPAIR_DECLICK_REQUEST_KEYS,
  masteringRepairDeclickStereo: MASTERING_REPAIR_DECLICK_STEREO_REQUEST_KEYS,
  masteringRepairDeclip: MASTERING_REPAIR_DECLIP_REQUEST_KEYS,
  masteringRepairDeclipStereo: MASTERING_REPAIR_DECLIP_STEREO_REQUEST_KEYS,
  masteringRepairDecrackle: MASTERING_REPAIR_DECRACKLE_REQUEST_KEYS,
  masteringRepairDecrackleStereo: MASTERING_REPAIR_DECRACKLE_STEREO_REQUEST_KEYS,
  masteringRepairDetectClicks: MASTERING_REPAIR_DETECT_CLICKS_REQUEST_KEYS,
  masteringRepairDetectClipping: MASTERING_REPAIR_DETECT_CLIPPING_REQUEST_KEYS,
  masteringRepairDetectCrackle: MASTERING_REPAIR_DETECT_CRACKLE_REQUEST_KEYS,
  masteringRepairDenoiseClassical: MASTERING_REPAIR_DENOISE_CLASSICAL_REQUEST_KEYS,
  masteringRepairDenoiseClassicalStereo: MASTERING_REPAIR_DENOISE_CLASSICAL_STEREO_REQUEST_KEYS,
  masteringRepairDenoiseClassicalLinked: MASTERING_REPAIR_DENOISE_CLASSICAL_LINKED_REQUEST_KEYS,
  masteringRepairDehum: MASTERING_REPAIR_DEHUM_REQUEST_KEYS,
  masteringRepairDehumStereo: MASTERING_REPAIR_DEHUM_STEREO_REQUEST_KEYS,
  masteringRepairDetectNoiseFloor: MASTERING_REPAIR_DETECT_NOISE_FLOOR_REQUEST_KEYS,
  masteringRepairNoiseBandBins: MASTERING_REPAIR_NOISE_BAND_BINS_REQUEST_KEYS,
  masteringRepairDetectHum: MASTERING_REPAIR_DETECT_HUM_REQUEST_KEYS,
  masteringRepairAnalyze: MASTERING_REPAIR_ANALYZE_REQUEST_KEYS,
  masteringRepairApply: MASTERING_REPAIR_APPLY_REQUEST_KEYS,
  masteringRepairTrimSilence: MASTERING_REPAIR_TRIM_SILENCE_REQUEST_KEYS,
  masteringRepairTrimSilenceStereo: MASTERING_REPAIR_TRIM_SILENCE_STEREO_REQUEST_KEYS,
  masteringRepairDetectTrimRange: MASTERING_REPAIR_DETECT_TRIM_RANGE_REQUEST_KEYS,
  masteringRepairDetectTrimRangeStereo: MASTERING_REPAIR_DETECT_TRIM_RANGE_STEREO_REQUEST_KEYS,
  voiceChange: VOICE_CHANGE_REQUEST_KEYS,
  voiceChangeRealtime: VOICE_CHANGE_REALTIME_REQUEST_KEYS,
  framesToTime: FRAMES_TO_TIME_REQUEST_KEYS,
  timeToFrames: TIME_TO_FRAMES_REQUEST_KEYS,
  framesToSamples: FRAMES_TO_SAMPLES_REQUEST_KEYS,
  samplesToFrames: SAMPLES_TO_FRAMES_REQUEST_KEYS,
  powerToDb: POWER_TO_DB_REQUEST_KEYS,
  amplitudeToDb: POWER_TO_DB_REQUEST_KEYS,
  stft: STFT_REQUEST_KEYS,
  stftDb: STFT_REQUEST_KEYS,
  melSpectrogram: MEL_SPECTROGRAM_REQUEST_KEYS,
  mfcc: MFCC_REQUEST_KEYS,
  melDelta: MEL_DELTA_REQUEST_KEYS,
  reassignedSpectrogram: REASSIGNED_SPECTROGRAM_REQUEST_KEYS,
  spectralCentroid: STFT_REQUEST_KEYS,
  spectralContrast: SPECTRAL_CONTRAST_REQUEST_KEYS,
  polyFeatures: POLY_FEATURES_REQUEST_KEYS,
  zeroCrossings: ZERO_CROSSINGS_REQUEST_KEYS,
  spectralBandwidth: SPECTRAL_BANDWIDTH_REQUEST_KEYS,
  spectralRolloff: SPECTRAL_ROLLOFF_REQUEST_KEYS,
  spectralFlatness: STFT_REQUEST_KEYS,
  spectralFlux: SPECTRAL_FLUX_REQUEST_KEYS,
  zeroCrossingRate: ZERO_CROSSING_RATE_REQUEST_KEYS,
  rmsEnergy: ZERO_CROSSING_RATE_REQUEST_KEYS,
  pcen: PCEN_REQUEST_KEYS,
  segmentCrossSimilarity: SEGMENT_CROSS_SIMILARITY_REQUEST_KEYS,
  segmentRecurrenceMatrix: SEGMENT_RECURRENCE_MATRIX_REQUEST_KEYS,
  segmentRecurrenceToLag: SEGMENT_RECURRENCE_TO_LAG_REQUEST_KEYS,
  segmentLagToRecurrence: SEGMENT_LAG_TO_RECURRENCE_REQUEST_KEYS,
  segmentSubsegment: SEGMENT_SUBSEGMENT_REQUEST_KEYS,
  segmentAgglomerative: SEGMENT_AGGLOMERATIVE_REQUEST_KEYS,
  segmentPathEnhance: SEGMENT_PATH_ENHANCE_REQUEST_KEYS,
  decompose: DECOMPOSE_REQUEST_KEYS,
  decomposeStems: DECOMPOSE_STEMS_REQUEST_KEYS,
  decomposeStemsLinked: DECOMPOSE_STEMS_LINKED_REQUEST_KEYS,
  nnFilter: NN_FILTER_REQUEST_KEYS,
  remix: REMIX_REQUEST_KEYS,
  remixAlignedIntervals: REMIX_REQUEST_KEYS,
  hpssWithResidual: HPSS_WITH_RESIDUAL_REQUEST_KEYS,
  trim: TRIM_REQUEST_KEYS,
  preemphasis: EMPHASIS_REQUEST_KEYS,
  deemphasis: EMPHASIS_REQUEST_KEYS,
  trimSilence: TRIM_SILENCE_REQUEST_KEYS,
  splitSilence: TRIM_SILENCE_REQUEST_KEYS,
  splitSilenceCommon: SPLIT_SILENCE_COMMON_REQUEST_KEYS,
  splitSilenceCommonWithReport: SPLIT_SILENCE_COMMON_REQUEST_KEYS,
  frameSignal: FRAME_SIGNAL_REQUEST_KEYS,
  padCenter: PAD_CENTER_REQUEST_KEYS,
  fixLength: PAD_CENTER_REQUEST_KEYS,
  fixFrames: FIX_FRAMES_REQUEST_KEYS,
  vectorNormalize: VECTOR_NORMALIZE_REQUEST_KEYS,
  cqtToAudio: CQT_TO_AUDIO_REQUEST_KEYS,
  vqtToAudio: VQT_TO_AUDIO_REQUEST_KEYS,
  griffinLim: GRIFFIN_LIM_REQUEST_KEYS,
  phaseVocoder: PHASE_VOCODER_REQUEST_KEYS,
  melToStft: MEL_TO_STFT_REQUEST_KEYS,
  melToAudio: MEL_TO_AUDIO_REQUEST_KEYS,
  mfccToMel: MFCC_TO_MEL_REQUEST_KEYS,
  mfccToAudio: MFCC_TO_AUDIO_REQUEST_KEYS,
  tone: TONE_REQUEST_KEYS,
  chirp: CHIRP_REQUEST_KEYS,
  clicks: CLICKS_REQUEST_KEYS,
  lufsInterleaved: LUFS_INTERLEAVED_REQUEST_KEYS,
  lufsSeriesInterleaved: LUFS_INTERLEAVED_REQUEST_KEYS,
  ebur128LoudnessRange: FEATURE_SAMPLES_REQUEST_KEYS,
  lufs: LUFS_REQUEST_KEYS,
  momentaryLufs: LUFS_REQUEST_KEYS,
  shortTermLufs: LUFS_REQUEST_KEYS,
  chroma: STFT_REQUEST_KEYS,
  chromaCens: CHROMA_REQUEST_KEYS,
  chromaCqt: CHROMA_REQUEST_KEYS,
  bassChroma: BASS_CHROMA_REQUEST_KEYS,
  cqt: CQT_REQUEST_KEYS,
  pseudoCqt: CQT_REQUEST_KEYS,
  hybridCqt: CQT_REQUEST_KEYS,
  vqt: VQT_REQUEST_KEYS,
  pitchTuning: PITCH_TUNING_REQUEST_KEYS,
  estimateTuning: ESTIMATE_TUNING_REQUEST_KEYS,
  piptrack: PIPTRACK_REQUEST_KEYS,
  pitchYin: PITCH_REQUEST_KEYS,
  pitchPyin: PITCH_REQUEST_KEYS,
  noteSegments: NOTE_SEGMENTS_REQUEST_KEYS,
  tonnetz: TONNETZ_REQUEST_KEYS,
  nnlsChroma: NNLS_CHROMA_REQUEST_KEYS,
  onsetBacktrack: ONSET_BACKTRACK_REQUEST_KEYS,
  peakPick: PEAK_PICK_REQUEST_KEYS,
  tempogram: TEMPOGRAM_REQUEST_KEYS,
  cyclicTempogram: CYCLIC_TEMPOGRAM_REQUEST_KEYS,
  plp: PLP_REQUEST_KEYS,
  onsetEnvelope: ONSET_ENVELOPE_REQUEST_KEYS,
  onsetStrengthMulti: ONSET_STRENGTH_MULTI_REQUEST_KEYS,
  fourierTempogram: TEMPOGRAM_REQUEST_KEYS,
  tempogramRatio: TEMPOGRAM_RATIO_REQUEST_KEYS,
  meteringPeakDb: METERING_SAMPLES_REQUEST_KEYS,
  meteringRmsDb: METERING_SAMPLES_REQUEST_KEYS,
  meteringSilenceRatio: METERING_SILENCE_RATIO_REQUEST_KEYS,
  meteringCrestFactorDb: METERING_SAMPLES_REQUEST_KEYS,
  meteringCrestFactorDbStereo: METERING_STEREO_REQUEST_KEYS,
  meteringDcOffset: METERING_SAMPLES_REQUEST_KEYS,
  meteringTruePeakDb: METERING_TRUE_PEAK_REQUEST_KEYS,
  meteringDetectClipping: METERING_DETECT_CLIPPING_REQUEST_KEYS,
  meteringDynamicRange: METERING_DYNAMIC_RANGE_REQUEST_KEYS,
  meteringStereoCorrelation: METERING_STEREO_REQUEST_KEYS,
  meteringStereoWidth: METERING_STEREO_REQUEST_KEYS,
  meteringVectorscope: METERING_SCOPE_REQUEST_KEYS,
  meteringPhaseScope: METERING_SCOPE_REQUEST_KEYS,
  meteringSpectrum: METERING_SPECTRUM_REQUEST_KEYS,
  meteringSpectrumFrame: METERING_SPECTRUM_FRAME_REQUEST_KEYS,
  waveformPeaks: WAVEFORM_PEAKS_REQUEST_KEYS,
  waveformPeakPyramid: WAVEFORM_PEAK_PYRAMID_REQUEST_KEYS,
  resample: RESAMPLE_REQUEST_KEYS,
  mixStereo: MIX_STEREO_REQUEST_KEYS,
  analyzePolyphonic: ANALYZE_POLYPHONIC_REQUEST_KEYS,
  suggestMixScene: SUGGEST_MIX_SCENE_REQUEST_KEYS,
  suggestMixSceneJson: SUGGEST_MIX_SCENE_REQUEST_KEYS,
  transcribe: TRANSCRIBE_REQUEST_KEYS,
  restoreVocalEditSession: VOCAL_RESTORE_REQUEST_KEYS,
  masterAudio: MASTER_AUDIO_REQUEST_KEYS,
  masterAudioAsync: MASTER_AUDIO_ASYNC_REQUEST_KEYS,
  masterAudioStereo: MASTER_AUDIO_STEREO_REQUEST_KEYS,
  masterAudioStereoAsync: MASTER_AUDIO_STEREO_ASYNC_REQUEST_KEYS,
  analyzeAsync: MUSIC_ANALYZE_REQUEST_KEYS,
  masteringAssistantSuggestStereo: MASTERING_ASSISTANT_SUGGEST_STEREO_REQUEST_KEYS,
  masteringAssistantSuggestChainStereo: MASTERING_ASSISTANT_SUGGEST_STEREO_REQUEST_KEYS,
  masteringAudioProfileStereo: MASTERING_AUDIO_PROFILE_STEREO_REQUEST_KEYS,
  masteringStreamingPreviewStereo: MASTERING_STREAMING_PREVIEW_STEREO_REQUEST_KEYS,
  roomGeometryFromEstimate: ROOM_GEOMETRY_FROM_ESTIMATE_REQUEST_KEYS,
  createVocalEditSession: VOCAL_CREATE_REQUEST_KEYS,
} as const;

export const NESTED_REQUEST_KEYS = {
  analyzeWithProgress: { options: MUSIC_ANALYZE_OPTIONS_KEYS },
  noteSegments: { config: NOTE_SEGMENTS_CONFIG_KEYS },
  renderPlayback: { config: PLAYBACK_RENDERER_CONFIG_KEYS },
  restoreVocalEditSession: { limits: VOCAL_SESSION_LIMITS_KEYS },
  createVocalEditSession: { limits: VOCAL_SESSION_LIMITS_KEYS },
} as const;

export const NESTED_KEY_HINTS: Partial<Record<RequestFn, Record<string, KeySet<string>>>> = {
  masteringChain: { config: CHAIN_CONFIG_SECTIONS },
  masteringChainStereo: { config: CHAIN_CONFIG_SECTIONS },
  streamingLoudnessGain: { config: CHAIN_CONFIG_SECTIONS },
  streamingLoudnessGainStereo: { config: CHAIN_CONFIG_SECTIONS },
  masterAudio: { overrides: CHAIN_CONFIG_SECTIONS },
  masterAudioAsync: { overrides: CHAIN_CONFIG_SECTIONS },
  masterAudioStereo: { overrides: CHAIN_CONFIG_SECTIONS },
  masterAudioStereoAsync: { overrides: CHAIN_CONFIG_SECTIONS },
};

/** Entry points that take a request object. */
export type RequestFn = keyof typeof REQUEST_KEYS;
