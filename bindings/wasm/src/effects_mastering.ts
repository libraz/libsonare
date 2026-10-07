export {
  assignNoteTargets,
  decomposeNotePitch,
  extractNotes,
  mergeNotes,
  noteMove,
  noteStretch,
  noteTargetsFromSmf,
  renderNotes,
  splitNote,
} from './effects_note_ops.js';
export { extractPercussiveEvents, renderPercussiveEvents } from './effects_percussive.js';
export { harmonic, hpss, percussive } from './effects_separation.js';
export { spectralEdit } from './effects_spectral.js';
export {
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  pitchShift,
  timeStretch,
} from './effects_timepitch.js';
export type {
  VoiceChangeOptions,
  VoiceChangeRealtimeOptions,
  VoiceChangeRealtimeRequest,
  VoiceChangeRequest,
} from './effects_voice_change.js';
export { voiceChange, voiceChangeRealtime } from './effects_voice_change.js';
export {
  masterAudio,
  masterAudioStereo,
  masterAudioStereoWithProgress,
  masterAudioWithProgress,
  masteringChain,
  masteringChainStereo,
  masteringChainStereoWithProgress,
  masteringChainWithProgress,
  masteringPlatformNames,
  masteringPresetNames,
  masteringPresetParams,
  normalize,
  normalizeStereo,
} from './mastering_chain.js';
export type {
  MasteringAbMatchLoudnessRequest,
  MasteringAbMatchLoudnessStereoRequest,
  MasteringAmpPresetCatalogEntry,
  MasteringAssistantParamsRequest,
  MasteringAssistantStereoParamsRequest,
  MasteringChannelPolicy,
  MasteringInsertParamChoice,
  MasteringInsertParamInfo,
  MasteringInsertSlot,
  MasteringInsertTiming,
  MasteringPairAnalyzeRequest,
  MasteringPairProcessRequest,
  MasteringPairProcessStereoRequest,
  MasteringProcessorCatalogEntry,
  MasteringProcessorCategory,
  MasteringProcessRequest,
  MasteringProcessStereoRequest,
  MasteringRealtimeCost,
  MasteringSamplesParamsRequest,
  MasteringStereoAnalyzeRequest,
  MasteringStereoParamsRequest,
  MasteringStreamingPreviewRequest,
  MasteringStreamingPreviewStereoRequest,
} from './mastering_core.js';
export {
  mastering,
  masteringAbMatchLoudness,
  masteringAbMatchLoudnessStereo,
  masteringAmpPresetCatalog,
  masteringAssistantSuggest,
  masteringAssistantSuggestChain,
  masteringAssistantSuggestChainStereo,
  masteringAssistantSuggestStereo,
  masteringAudioProfile,
  masteringAudioProfileStereo,
  masteringInsertNames,
  masteringInsertParamInfo,
  masteringInsertParamNames,
  masteringInsertTiming,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringPairProcessStereo,
  masteringProcess,
  masteringProcessorCatalog,
  masteringProcessorNames,
  masteringProcessStereo,
  masteringStereoAnalysisNames,
  masteringStereoAnalyze,
  masteringStreamingPreview,
  masteringStreamingPreviewStereo,
} from './mastering_core.js';
export type {
  CompressorDetector,
  CompressorOptions,
  DynamicsProcessorResult,
  GateOptions,
  MasteringDynamicsCompressorRequest,
  MasteringDynamicsGateRequest,
  MasteringDynamicsTransientShaperRequest,
  TransientShaperOptions,
} from './mastering_dynamics.js';
export {
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
} from './mastering_dynamics.js';
export type { MixStereoRequest } from './mixing_oneshot.js';
export { mixingScenePresetJson, mixingScenePresetNames, mixStereo } from './mixing_oneshot.js';
export type {
  DereverbClassicalOptions,
  MasteringRepairDereverbClassicalLinkedRequest,
  MasteringRepairDereverbClassicalRequest,
  MasteringRepairDereverbClassicalStereoRequest,
  MasteringRepairDereverbConfigForRoomRequest,
  MasteringRepairDetectReverbRequest,
} from './repair_dereverb.js';
export {
  masteringRepairDereverbClassical,
  masteringRepairDereverbClassicalLinked,
  masteringRepairDereverbClassicalStereo,
  masteringRepairDereverbConfigForRoom,
  masteringRepairDetectReverb,
} from './repair_dereverb.js';
export type {
  DeclickOptions,
  DeclipOptions,
  DecrackleMode,
  DecrackleOptions,
  MasteringRepairDeclickRequest,
  MasteringRepairDeclickStereoRequest,
  MasteringRepairDeclipRequest,
  MasteringRepairDeclipStereoRequest,
  MasteringRepairDecrackleRequest,
  MasteringRepairDecrackleStereoRequest,
  MasteringRepairDetectClicksRequest,
  MasteringRepairDetectClippingRequest,
  MasteringRepairDetectCrackleRequest,
} from './repair_impulsive.js';
export {
  masteringRepairDeclick,
  masteringRepairDeclickStereo,
  masteringRepairDeclip,
  masteringRepairDeclipStereo,
  masteringRepairDecrackle,
  masteringRepairDecrackleStereo,
  masteringRepairDetectClicks,
  masteringRepairDetectClipping,
  masteringRepairDetectCrackle,
} from './repair_impulsive.js';
export type {
  DehumMode,
  DehumOptions,
  DenoiseClassicalMode,
  DenoiseClassicalNoiseEstimator,
  DenoiseClassicalOptions,
  MasteringRepairDehumRequest,
  MasteringRepairDehumStereoRequest,
  MasteringRepairDenoiseClassicalLinkedRequest,
  MasteringRepairDenoiseClassicalRequest,
  MasteringRepairDenoiseClassicalStereoRequest,
  MasteringRepairDetectHumRequest,
  MasteringRepairDetectNoiseFloorRequest,
  MasteringRepairNoiseBandBinsRequest,
} from './repair_noise.js';
export {
  masteringRepairDehum,
  masteringRepairDehumStereo,
  masteringRepairDenoiseClassical,
  masteringRepairDenoiseClassicalLinked,
  masteringRepairDenoiseClassicalStereo,
  masteringRepairDetectHum,
  masteringRepairDetectNoiseFloor,
  masteringRepairNoiseBandBins,
} from './repair_noise.js';
export type {
  MasteringRepairDetectTrimRangeRequest,
  MasteringRepairDetectTrimRangeStereoRequest,
  MasteringRepairTrimSilenceRequest,
  MasteringRepairTrimSilenceStereoRequest,
  TrimSilenceMode,
  TrimSilenceOptions,
} from './repair_trim.js';
export {
  masteringRepairDetectTrimRange,
  masteringRepairDetectTrimRangeStereo,
  masteringRepairTrimSilence,
  masteringRepairTrimSilenceStereo,
} from './repair_trim.js';
