#pragma once

#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <sonare/sonare_c.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "analysis/acoustic_analyzer.h"
#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/room_estimator.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "automation/parameter.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/db_convert.h"
#include "core/pcen.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/pitch_editor/scale_quantizer.h"
#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/streaming_retune.h"
#include "editing/voice_changer/voice_changer.h"
#include "effects/acoustic/room_morph.h"
#include "effects/decompose.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/phase_vocoder.h"
#include "effects/pitch_shift.h"
#include "effects/preemphasis.h"
#include "effects/remix.h"
#include "effects/silence.h"
#include "effects/spectral_edit.h"
#include "effects/time_stretch.h"
#include "engine/realtime_engine.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/nnls_chroma.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/rhythm.h"
#include "feature/spectral.h"
#include "feature/tonnetz.h"
#include "feature/vqt.h"
#include "graph/graph.h"
#include "mastering/api/chain.h"
#include "mastering/api/internal_processor_runner.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/config_from_params.h"
#include "mastering/assistant/suggester.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/tilt.h"
#include "mastering/final/dither.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/tape.h"
#include "mastering/spectral/air_band.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "metering/basic.h"
#include "metering/clipping.h"
#include "metering/dynamic_range.h"
#include "metering/lufs.h"
#include "metering/normalize.h"
#include "metering/phase_scope.h"
#include "metering/spectrum.h"
#include "metering/stereo.h"
#include "metering/true_peak.h"
#include "metering/waveform.h"
#include "midi/builtin_synth.h"
#include "mixing/api/presets.h"
#include "mixing/channel_strip.h"
#include "quick.h"
#include "rt/command.h"
#include "rt/gain_processor.h"
#include "rt/processor_base.h"
#include "sonare.h"
#include "streaming/stream_analyzer.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/frame.h"
#include "util/padding.h"
#include "util/peak.h"
#include "util/types.h"
#include "util/vector_normalize.h"

using namespace emscripten;
using namespace sonare;

val vectorToFloat32Array(const std::vector<float>& vec);
val vectorToInt32Array(const std::vector<int>& vec);
val vectorToUint8Array(const std::vector<uint8_t>& vec);
std::vector<float> float32ArrayToVector(val arr);
std::vector<int32_t> int32ArrayToVector(val arr);
std::vector<uint8_t> uint8ArrayToVector(val arr);
bool hasProperty(val object, const char* key);
val objectProperty(val object, const char* key);
float floatProperty(val object, const char* key, float default_value);
int intProperty(val object, const char* key, int default_value);
bool boolProperty(val object, const char* key, bool default_value);
std::string stringProperty(val object, const char* key, const std::string& default_value);
std::vector<mastering::api::Param> masteringParamsFromObject(val object);
mastering::api::MasteringChainConfig masteringChainConfigFromVal(val config);

void registerProjectBindings();
void registerStreamAnalyzerBindings();
void registerRealtimeEngineBindings();
void registerStreamingMasteringChainBindings();
void registerStreamingEqualizerBindings();
void registerStreamingRetuneBindings();
void registerRealtimeVoiceChangerStreamingBindings();
void registerMixingBindings();
void registerOfflineBindings();
void registerOfflineDynamicsEditingBindings();
void registerRepairBindings();
void registerMeteringBindings();
void registerFeatureSpectrogramBindings();
void registerFeatureMusicBindings();
void registerFeatureSpectralBindings();
void registerFeaturePitchBindings();
void registerFeatureCoreBindings();
void registerEffectsAudioBindings();
void registerMasteringChainBindings();
void registerMasteringApiBindings();
void registerQuickAnalysisBindings();
void registerQuickDetailedAnalysisBindings();

#endif  // __EMSCRIPTEN__
