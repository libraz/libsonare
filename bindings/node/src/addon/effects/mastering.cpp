#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "effects/repair_common.h"
#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/zero_is_default.h"

using namespace sonare_node;

namespace {

// The core summary carries no unit in the field name; the surface does.
float LoudnessRangeLu(const sonare::mastering::api::MasteringLoudnessSummary& summary) {
  return summary.loudness_range_lu;
}

template <typename Summary>
Napi::Object MasteringLoudnessSummaryToObject(Napi::Env env, const Summary& summary) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("integratedLufs", Napi::Number::New(env, summary.integrated_lufs));
  out.Set("maxMomentaryLufs", Napi::Number::New(env, summary.max_momentary_lufs));
  out.Set("maxShortTermLufs", Napi::Number::New(env, summary.max_short_term_lufs));
  out.Set("truePeakDbtp", Napi::Number::New(env, summary.true_peak_dbtp));
  out.Set("loudnessRangeLu", Napi::Number::New(env, LoudnessRangeLu(summary)));
  return out;
}

template <typename Report>
void SetMasteringReport(Napi::Env env, Napi::Object out, const Report& report) {
  Napi::Object report_object = Napi::Object::New(env);
  report_object.Set("before", MasteringLoudnessSummaryToObject(env, report.before));
  report_object.Set("after", MasteringLoudnessSummaryToObject(env, report.after));
  report_object.Set("appliedGainDb", Napi::Number::New(env, report.applied_gain_db));
  report_object.Set("maxGainReductionDb", Napi::Number::New(env, report.max_gain_reduction_db));
  report_object.Set("loudnessTargetLimited",
                    Napi::Boolean::New(env, report.loudness_target_limited != 0));
  Napi::Float32Array band_energy = Napi::Float32Array::New(env, SONARE_MASTERING_REPORT_BAND_COUNT);
  for (size_t index = 0; index < SONARE_MASTERING_REPORT_BAND_COUNT; ++index) {
    band_energy[index] = report.band_energy_delta_db[index];
  }
  report_object.Set("bandEnergyDeltaDb", band_energy);
  out.Set("report", report_object);
}

// Append the chain-metric fields (output true peak, LRA, per-stage gain
// reductions, substitution count) shared by every mastering-chain result
// object. Takes the whole result rather than its ChainMetrics base: the
// substitution count lives on the audio-result base instead, and a builder
// given only one of the two bases would silently drop whichever it cannot see.
template <typename Result>
void SetChainMetrics(Napi::Env env, Napi::Object out, const Result& metrics) {
  out.Set("outputTruePeakDbtp", Napi::Number::New(env, metrics.output_true_peak_dbtp));
  out.Set("outputLoudnessRangeLu", Napi::Number::New(env, metrics.output_loudness_range_lu));
  out.Set("loudnessTargetLimited", Napi::Boolean::New(env, metrics.loudness_target_limited));
  out.Set("nonFiniteSubstitutionCount",
          Napi::Number::New(env, metrics.non_finite_substitution_count));
  Napi::Array reductions = Napi::Array::New(env, metrics.stage_gain_reductions.size());
  for (size_t i = 0; i < metrics.stage_gain_reductions.size(); ++i) {
    Napi::Object entry = Napi::Object::New(env);
    entry.Set("stage", Napi::String::New(env, metrics.stage_gain_reductions[i].stage));
    entry.Set("gainReductionDb",
              Napi::Number::New(env, metrics.stage_gain_reductions[i].gain_reduction_db));
    reductions.Set(static_cast<uint32_t>(i), entry);
  }
  out.Set("stageGainReductions", reductions);
  SetMasteringReport(env, out, metrics.report);
}

namespace mastering_api = sonare::mastering::api;

bool IsDocumentOrAbsent(const Napi::Value& value) {
  return value.IsString() || value.IsUndefined() || value.IsNull();
}

mastering_api::MasteringChainConfig OfflineChainConfig(const std::optional<std::string>& document) {
  return document ? mastering_api::chain_config_from_json(*document)
                  : mastering_api::MasteringChainConfig{};
}

mastering_api::MasteringChainConfig StreamingChainConfig(
    const std::optional<std::string>& document) {
  return mastering_api::streaming_chain_config_from_json(document ? *document
                                                                  : std::string("{\"params\":{}}"));
}

// A preset's config with the overrides document applied; no preset reads the document alone.
mastering_api::MasteringChainConfig PresetChainConfig(const std::optional<std::string>& preset_name,
                                                      const std::optional<std::string>& overrides) {
  if (!preset_name) return OfflineChainConfig(overrides);
  auto config = mastering_api::preset_config(mastering_api::preset_from_string(*preset_name));
  return overrides ? mastering_api::chain_config_overrides_from_json(std::move(config), *overrides)
                   : config;
}

struct ProgressContext {
  Napi::Env env;
  Napi::Function callback;
  std::optional<Napi::Function> cancel;
};

// A JS callback that throws leaves an exception pending; every further N-API
// call must be skipped, and the chain is cancelled so the throw surfaces
// promptly instead of after the whole render has run. Mirrors progress.cpp's
// c_progress/c_cancel.
void ReportProgress(float progress, const char* stage, void* user_data) {
  auto* context = static_cast<ProgressContext*>(user_data);
  if (context->env.IsExceptionPending()) return;
  context->callback.Call({Napi::Number::New(context->env, progress),
                          Napi::String::New(context->env, stage != nullptr ? stage : "")});
  sonare_node::MarkPendingForeign(context->env);
}

int CancellationRequested(void* user_data) {
  auto* context = static_cast<ProgressContext*>(user_data);
  if (context->env.IsExceptionPending()) return 1;
  if (!context->cancel) return 0;
  const Napi::Value result = context->cancel->Call({});
  sonare_node::MarkPendingForeign(context->env);
  if (context->env.IsExceptionPending()) return 1;
  return result.IsBoolean() && result.As<Napi::Boolean>().Value() ? 1 : 0;
}

// Runs a mono or stereo chain with the JS callbacks; a pending JS exception wins over cancellation.
template <typename Process, typename Emit>
Napi::Value RunChainWithCallbacks(Napi::Env env, mastering_api::MasteringChain& chain,
                                  ProgressContext& progress, Process process, Emit emit) {
  chain.set_progress_callback([&progress](float fraction, const char* stage) {
    ReportProgress(fraction, stage, &progress);
  });
  chain.set_cancel_callback([&progress] { return CancellationRequested(&progress) != 0; });
  auto result = process();
  // The pending-exception check comes first: throwing a SonareError on top of a
  // callback's exception would abort the process under NAPI_DISABLE_CPP_EXCEPTIONS.
  if (env.IsExceptionPending()) return env.Undefined();
  if (!result) {
    ThrowSonareError(env, SONARE_ERROR_CANCELLED);
    return env.Undefined();
  }
  return emit(*result);
}

}  // namespace

Napi::Value SonareWrap::Mastering(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);

  sonare::mastering::maximizer::LoudnessOptimizeConfig config;
  if (!OptionalFiniteFloatArg(env, info, 2, "targetLufs", -14.0f, &config.target_lufs) ||
      !OptionalFiniteFloatArg(env, info, 3, "ceilingDbtp", -1.0f, &config.ceiling_dbtp)) {
    return env.Undefined();
  }
  // 0 is the C-ABI sentinel for the library default; any other value reaches
  // the loudness validator instead of being replaced by the default.
  int oversample = 0;
  float release_ms = 0.0f;
  bool apply_gain_at_input_rate = false;
  if (!OptionalIntArg(env, info, 4, "oversampleFactor", 0, &oversample) ||
      !OptionalFiniteFloatArg(env, info, 5, "releaseMs", 0.0f, &release_ms) ||
      !OptionalBoolArg(env, info, 6, "applyGainAtInputRate", false, &apply_gain_at_input_rate)) {
    return env.Undefined();
  }
  if (oversample != 0) config.true_peak_oversample = oversample;
  // release_ms: 0 selects the library default; any other value is applied as
  // asked and rejected by the shared loudness validator if it is not positive.
  config.release_ms = sonare::ZeroIsDefault(release_ms).or_default(config.release_ms);
  config.apply_gain_at_input_rate = apply_gain_at_input_rate;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  auto result = sonare::mastering::maximizer::loudness_optimize(audio, config);
  std::vector<float> out_vec(result.audio.data(), result.audio.data() + result.audio.size());

  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, out_vec));
  out.Set("sampleRate", Napi::Number::New(env, result.audio.sample_rate()));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  out.Set("loudnessTargetLimited", Napi::Boolean::New(env, result.loudness_target_limited));
  out.Set("nonFiniteSubstitutionCount",
          Napi::Number::New(env, result.non_finite_substitution_count));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringProcess(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "processorName must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto name = node_narrow_string(env, info[0], "name");
  auto typed = info[1].As<Napi::Float32Array>();
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", typed.Data(), typed.ElementLength(),
                                       node_narrow_int(env, info[2], "sampleRate"));
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 4 && info[3].IsObject()) {
    params = ParamsFromObject(info[3].As<Napi::Object>());
  }
  auto result = sonare::mastering::api::apply_named_processor(
      name, typed.Data(), typed.ElementLength(), node_narrow_int(env, info[2], "sampleRate"),
      params);
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  out.Set("latencySamples", Napi::Number::New(env, result.latency_samples));
  out.Set("loudnessTargetLimited", Napi::Boolean::New(env, result.loudness_target_limited));
  out.Set("nonFiniteSubstitutionCount",
          Napi::Number::New(env, result.non_finite_substitution_count));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "processorName must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[2])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto name = node_narrow_string(env, info[0], "name");
  auto left = info[1].As<Napi::Float32Array>();
  auto right = info[2].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  const int sr = node_narrow_int(env, info[3], "sampleRate");
  sonare::validate_offline_audio_input(nullptr, "left", left.Data(), left.ElementLength(), sr);
  sonare::validate_offline_audio_input(nullptr, "right", right.Data(), right.ElementLength(), sr);
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 5 && info[4].IsObject()) {
    params = ParamsFromObject(info[4].As<Napi::Object>());
  }
  auto result = sonare::mastering::api::apply_named_processor_stereo(
      name, left.Data(), right.Data(), left.ElementLength(), sr, params);
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, result.left));
  out.Set("right", VecToFloat32(env, result.right));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  out.Set("latencySamples", Napi::Number::New(env, result.latency_samples));
  out.Set("loudnessTargetLimited", Napi::Boolean::New(env, result.loudness_target_limited));
  out.Set("nonFiniteSubstitutionCount",
          Napi::Number::New(env, result.non_finite_substitution_count));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringChain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", typed.Data(), typed.ElementLength(),
                                       node_narrow_int(env, info[1], "sampleRate"));
  std::optional<std::string> document;
  if (!OptionalStringArg(env, info, 2, "configJson", &document)) {
    return env.Undefined();
  }
  sonare::mastering::api::MasteringChain chain(OfflineChainConfig(document));
  auto result = chain.process_mono(typed.Data(), typed.ElementLength(),
                                   node_narrow_int(env, info[1], "sampleRate"));
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  SetChainMetrics(env, out, result);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringChainStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  sonare::validate_offline_audio_input(nullptr, "left", left.Data(), left.ElementLength(), sr);
  sonare::validate_offline_audio_input(nullptr, "right", right.Data(), right.ElementLength(), sr);
  std::optional<std::string> document;
  if (!OptionalStringArg(env, info, 3, "configJson", &document)) {
    return env.Undefined();
  }
  sonare::mastering::api::MasteringChain chain(OfflineChainConfig(document));
  auto result = chain.process_stereo(left.Data(), right.Data(), left.ElementLength(), sr);
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, result.left));
  out.Set("right", VecToFloat32(env, result.right));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  SetChainMetrics(env, out, result);
  return out;
  SONARE_NODE_CATCH(env)
}

namespace {

Napi::Object StreamingLoudnessGainToObject(
    Napi::Env env, const sonare::mastering::api::StreamingLoudnessGain& gain) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("loudnessStaticGainDb", Napi::Number::New(env, gain.loudness_static_gain_db));
  out.Set("truePeakDb", Napi::Number::New(env, gain.true_peak_db));
  out.Set("integratedLufs", Napi::Number::New(env, gain.integrated_lufs));
  return out;
}

}  // namespace

Napi::Value SonareWrap::MasteringStreamingLoudnessGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate = node_narrow_int(env, info[1], "sampleRate");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", typed.Data(), typed.ElementLength(),
                                       sample_rate);
  std::optional<std::string> document;
  if (!OptionalStringArg(env, info, 2, "configJson", &document)) {
    return env.Undefined();
  }
  const auto config = StreamingChainConfig(document);
  return StreamingLoudnessGainToObject(
      env, sonare::mastering::api::streaming_loudness_gain_mono(
               config, typed.Data(), typed.ElementLength(), sample_rate));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringStreamingLoudnessGainStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sample_rate = node_narrow_int(env, info[2], "sampleRate");
  sonare::validate_offline_audio_input(nullptr, "left", left.Data(), left.ElementLength(),
                                       sample_rate);
  sonare::validate_offline_audio_input(nullptr, "right", right.Data(), right.ElementLength(),
                                       sample_rate);
  std::optional<std::string> document;
  if (!OptionalStringArg(env, info, 3, "configJson", &document)) {
    return env.Undefined();
  }
  const auto config = StreamingChainConfig(document);
  return StreamingLoudnessGainToObject(
      env, sonare::mastering::api::streaming_loudness_gain_stereo(
               config, left.Data(), right.Data(), left.ElementLength(), sample_rate));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasterAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsDocumentOrAbsent(info[0])) {
    Napi::TypeError::New(env, "presetName must be a string or null").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::optional<std::string> preset_name;
  if (info[0].IsString()) preset_name = node_narrow_string(env, info[0], "presetName");
  auto typed = info[1].As<Napi::Float32Array>();
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", typed.Data(), typed.ElementLength(),
                                       node_narrow_int(env, info[2], "sampleRate"));
  std::optional<std::string> overrides;
  if (!OptionalStringArg(env, info, 3, "overridesJson", &overrides)) {
    return env.Undefined();
  }
  sonare::mastering::api::MasteringChain chain(PresetChainConfig(preset_name, overrides));
  auto result = chain.process_mono(typed.Data(), typed.ElementLength(),
                                   node_narrow_int(env, info[2], "sampleRate"));
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  SetChainMetrics(env, out, result);
  return out;
  SONARE_NODE_CATCH(env)
}

namespace {

// Local copy of VecToFloat32 (the one on SonareWrap is private and the async
// workers are not friends of SonareWrap).
Napi::Float32Array VecToTypedArray(Napi::Env env, const std::vector<float>& vec) {
  Napi::Float32Array array = Napi::Float32Array::New(env, vec.size());
  if (!vec.empty()) {
    std::memcpy(array.Data(), vec.data(), vec.size() * sizeof(float));
  }
  return array;
}

// Helper: serialise a MonoChainResult into a JS object on the main thread.
Napi::Object MonoResultToObject(Napi::Env env,
                                const sonare::mastering::api::MonoChainResult& result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToTypedArray(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  SetChainMetrics(env, out, result);
  return out;
}

// Helper: serialise a StereoChainResult into a JS object on the main thread.
Napi::Object StereoResultToObject(Napi::Env env,
                                  const sonare::mastering::api::StereoChainResult& result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToTypedArray(env, result.left));
  out.Set("right", VecToTypedArray(env, result.right));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  SetChainMetrics(env, out, result);
  return out;
}

// Off-main-thread mono master_audio. Copies samples + overrides into the
// worker so the JS thread can release its Float32Array view.
class MasterAudioAsyncWorker : public Napi::AsyncWorker {
 public:
  MasterAudioAsyncWorker(Napi::Env env, std::optional<std::string> preset_name,
                         std::vector<float> samples, int sample_rate,
                         std::optional<std::string> overrides)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        preset_name_(std::move(preset_name)),
        samples_(std::move(samples)),
        sample_rate_(sample_rate),
        overrides_(std::move(overrides)) {}

  void Execute() override {
    try {
      // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
      sonare::validate_offline_audio_input(nullptr, "samples", samples_.data(), samples_.size(),
                                           sample_rate_);
      // An absent preset runs the overrides document alone, as the synchronous entry does.
      sonare::mastering::api::MasteringChain chain(PresetChainConfig(preset_name_, overrides_));
      result_ = chain.process_mono(samples_.data(), samples_.size(), sample_rate_);
    } catch (const sonare::SonareException& e) {
      error_code_ = sonare_node::CErrorFromException(e);
      shape_refusal_ = sonare_node::IsShapeRefusal(e);
      SetError(sonare_node::ExceptionMessage(e));
    } catch (const std::bad_alloc&) {
      error_code_ = SONARE_ERROR_OUT_OF_MEMORY;
      SetError("Out of memory");
    } catch (const std::exception& e) {
      SetError(sonare_node::ExceptionMessage(e));
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    deferred_.Resolve(MonoResultToObject(Env(), result_));
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    sonare_node::DecorateSonareError(Env(), error.Value(), error_code_, shape_refusal_);
    deferred_.Reject(error.Value());
  }

  Napi::Promise GetPromise() { return deferred_.Promise(); }

 private:
  Napi::Promise::Deferred deferred_;
  std::optional<std::string> preset_name_;
  std::vector<float> samples_;
  int sample_rate_;
  std::optional<std::string> overrides_;
  sonare::mastering::api::MonoChainResult result_;
  SonareError error_code_ = SONARE_ERROR_UNKNOWN;
  bool shape_refusal_ = false;
};

class MasterAudioStereoAsyncWorker : public Napi::AsyncWorker {
 public:
  MasterAudioStereoAsyncWorker(Napi::Env env, std::optional<std::string> preset_name,
                               std::vector<float> left, std::vector<float> right, int sample_rate,
                               std::optional<std::string> overrides)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        preset_name_(std::move(preset_name)),
        left_(std::move(left)),
        right_(std::move(right)),
        sample_rate_(sample_rate),
        overrides_(std::move(overrides)) {}

  void Execute() override {
    try {
      // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
      sonare::validate_offline_audio_input(nullptr, "left", left_.data(), left_.size(),
                                           sample_rate_);
      sonare::validate_offline_audio_input(nullptr, "right", right_.data(), right_.size(),
                                           sample_rate_);
      sonare::mastering::api::MasteringChain chain(PresetChainConfig(preset_name_, overrides_));
      result_ = chain.process_stereo(left_.data(), right_.data(), left_.size(), sample_rate_);
    } catch (const sonare::SonareException& e) {
      error_code_ = sonare_node::CErrorFromException(e);
      shape_refusal_ = sonare_node::IsShapeRefusal(e);
      SetError(sonare_node::ExceptionMessage(e));
    } catch (const std::bad_alloc&) {
      error_code_ = SONARE_ERROR_OUT_OF_MEMORY;
      SetError("Out of memory");
    } catch (const std::exception& e) {
      SetError(sonare_node::ExceptionMessage(e));
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    deferred_.Resolve(StereoResultToObject(Env(), result_));
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    sonare_node::DecorateSonareError(Env(), error.Value(), error_code_, shape_refusal_);
    deferred_.Reject(error.Value());
  }

  Napi::Promise GetPromise() { return deferred_.Promise(); }

 private:
  Napi::Promise::Deferred deferred_;
  std::optional<std::string> preset_name_;
  std::vector<float> left_;
  std::vector<float> right_;
  int sample_rate_;
  std::optional<std::string> overrides_;
  sonare::mastering::api::StereoChainResult result_;
  SonareError error_code_ = SONARE_ERROR_UNKNOWN;
  bool shape_refusal_ = false;
};

}  // namespace

Napi::Value SonareWrap::MasterAudioAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // A Promise-returning entry point must reject rather than throw synchronously,
  // so invalid input never escapes a caller's `.catch()`.
  const bool preset_absent = info.Length() >= 1 && (info[0].IsUndefined() || info[0].IsNull());
  if (!(info[0].IsString() || preset_absent)) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::TypeError::New(env, "presetName must be a string or null").Value());
    return deferred.Promise();
  }
  if (!IsFloat32Array(info[1])) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::TypeError::New(env, "samples must be a Float32Array").Value());
    return deferred.Promise();
  }
  if (!info[2].IsNumber()) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::TypeError::New(env, "sampleRate must be a number").Value());
    return deferred.Promise();
  }
  std::optional<std::string> preset_name;
  if (!preset_absent) preset_name = node_narrow_string(env, info[0], "presetName");
  auto typed = info[1].As<Napi::Float32Array>();
  std::vector<float> samples(typed.Data(), typed.Data() + typed.ElementLength());
  int sample_rate = node_narrow_int(env, info[2], "sampleRate");
  std::optional<std::string> overrides;
  if (!OptionalStringArg(env, info, 3, "overridesJson", &overrides)) {
    return sonare_node::RejectedFromPending(env);
  }
  auto* worker = new MasterAudioAsyncWorker(env, std::move(preset_name), std::move(samples),
                                            sample_rate, std::move(overrides));
  Napi::Promise promise = worker->GetPromise();
  worker->Queue();
  return promise;
  SONARE_NODE_CATCH_REJECT(env)
}

Napi::Value SonareWrap::MasterAudioStereoAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // A Promise-returning entry point must reject rather than throw synchronously,
  // so invalid input never escapes a caller's `.catch()`.
  const bool preset_absent = info.Length() >= 1 && (info[0].IsUndefined() || info[0].IsNull());
  if (!(info[0].IsString() || preset_absent)) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::TypeError::New(env, "presetName must be a string or null").Value());
    return deferred.Promise();
  }
  if (!IsFloat32Array(info[1])) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::TypeError::New(env, "left must be a Float32Array").Value());
    return deferred.Promise();
  }
  if (!IsFloat32Array(info[2])) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::TypeError::New(env, "right must be a Float32Array").Value());
    return deferred.Promise();
  }
  if (!info[3].IsNumber()) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::TypeError::New(env, "sampleRate must be a number").Value());
    return deferred.Promise();
  }
  std::optional<std::string> preset_name;
  if (!preset_absent) preset_name = node_narrow_string(env, info[0], "presetName");
  auto left_typed = info[1].As<Napi::Float32Array>();
  auto right_typed = info[2].As<Napi::Float32Array>();
  if (left_typed.ElementLength() != right_typed.ElementLength()) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::RangeError::New(env, "left and right must have the same length").Value());
    return deferred.Promise();
  }
  std::vector<float> left(left_typed.Data(), left_typed.Data() + left_typed.ElementLength());
  std::vector<float> right(right_typed.Data(), right_typed.Data() + right_typed.ElementLength());
  int sample_rate = node_narrow_int(env, info[3], "sampleRate");
  std::optional<std::string> overrides;
  if (!OptionalStringArg(env, info, 4, "overridesJson", &overrides)) {
    return sonare_node::RejectedFromPending(env);
  }
  auto* worker =
      new MasterAudioStereoAsyncWorker(env, std::move(preset_name), std::move(left),
                                       std::move(right), sample_rate, std::move(overrides));
  Napi::Promise promise = worker->GetPromise();
  worker->Queue();
  return promise;
  SONARE_NODE_CATCH_REJECT(env)
}

Napi::Value SonareWrap::MasterAudioStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsDocumentOrAbsent(info[0])) {
    Napi::TypeError::New(env, "presetName must be a string or null").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[2])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::optional<std::string> preset_name;
  if (info[0].IsString()) preset_name = node_narrow_string(env, info[0], "presetName");
  auto left = info[1].As<Napi::Float32Array>();
  auto right = info[2].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  const int sr = node_narrow_int(env, info[3], "sampleRate");
  sonare::validate_offline_audio_input(nullptr, "left", left.Data(), left.ElementLength(), sr);
  sonare::validate_offline_audio_input(nullptr, "right", right.Data(), right.ElementLength(), sr);
  std::optional<std::string> overrides;
  if (!OptionalStringArg(env, info, 4, "overridesJson", &overrides)) {
    return env.Undefined();
  }
  sonare::mastering::api::MasteringChain chain(PresetChainConfig(preset_name, overrides));
  auto result = chain.process_stereo(left.Data(), right.Data(), left.ElementLength(), sr);
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, result.left));
  out.Set("right", VecToFloat32(env, result.right));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  SetChainMetrics(env, out, result);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringChainWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsFunction()) {
    Napi::TypeError::New(env, "onProgress must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() > 4 && !info[4].IsFunction()) {
    Napi::TypeError::New(env, "cancel must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  // The TypedArray's pointer is lent across the progress/cancel callbacks on
  // purpose. MasteringChain::process_mono_impl copies the whole input into its
  // own vector (src/mastering/api/chain.cpp:263) before the first
  // progress_callback_ invocation, so a callback that transfers or detaches the
  // caller's ArrayBuffer cannot reach memory the chain is still reading. An
  // addon-side copy would only duplicate a whole recording a second time; the
  // core contract is pinned by "MasteringChain copies its input before the first
  // progress callback" in tests/mastering/chain_test.cpp.
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  sonare::validate_offline_audio_input(nullptr, "samples", typed.Data(), typed.ElementLength(), sr);
  std::optional<std::string> document;
  if (!OptionalStringArg(env, info, 2, "configJson", &document)) return env.Undefined();
  sonare::mastering::api::MasteringChain chain(OfflineChainConfig(document));
  ProgressContext progress{env, info[3].As<Napi::Function>(),
                           info.Length() > 4
                               ? std::optional<Napi::Function>(info[4].As<Napi::Function>())
                               : std::nullopt};
  return RunChainWithCallbacks(
      env, chain, progress,
      [&] { return chain.process_mono_cancellable(typed.Data(), typed.ElementLength(), sr); },
      [&](const sonare::mastering::api::MonoChainResult& result) -> Napi::Value {
        return MonoResultToObject(env, result);
      });
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringChainStereoWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[4].IsFunction()) {
    Napi::TypeError::New(env, "onProgress must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() > 5 && !info[5].IsFunction()) {
    Napi::TypeError::New(env, "cancel must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  sonare::validate_offline_audio_input(nullptr, "left", left.Data(), left.ElementLength(), sr);
  sonare::validate_offline_audio_input(nullptr, "right", right.Data(), right.ElementLength(), sr);
  std::optional<std::string> document;
  if (!OptionalStringArg(env, info, 3, "configJson", &document)) return env.Undefined();
  sonare::mastering::api::MasteringChain chain(OfflineChainConfig(document));
  // Pointers lent across the callbacks for the same reason as the mono variant:
  // process_stereo_impl copies both channels before any progress callback runs.
  ProgressContext progress{env, info[4].As<Napi::Function>(),
                           info.Length() > 5
                               ? std::optional<Napi::Function>(info[5].As<Napi::Function>())
                               : std::nullopt};
  return RunChainWithCallbacks(
      env, chain, progress,
      [&] {
        return chain.process_stereo_cancellable(left.Data(), right.Data(), left.ElementLength(),
                                                sr);
      },
      [&](const sonare::mastering::api::StereoChainResult& result) -> Napi::Value {
        return StereoResultToObject(env, result);
      });
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasterAudioWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsDocumentOrAbsent(info[0])) {
    Napi::TypeError::New(env, "presetName must be a string or null").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[4].IsFunction()) {
    Napi::TypeError::New(env, "onProgress must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() > 5 && !info[5].IsFunction()) {
    Napi::TypeError::New(env, "cancel must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::optional<std::string> preset_name;
  if (info[0].IsString()) preset_name = node_narrow_string(env, info[0], "presetName");
  // Pointer lent across the callbacks for the same reason as the chain variants:
  // the preset path runs the same MasteringChain, which copies first.
  auto typed = info[1].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  sonare::validate_offline_audio_input(nullptr, "samples", typed.Data(), typed.ElementLength(), sr);
  std::optional<std::string> overrides;
  if (!OptionalStringArg(env, info, 3, "overridesJson", &overrides)) return env.Undefined();
  sonare::mastering::api::MasteringChain chain(PresetChainConfig(preset_name, overrides));
  ProgressContext progress{env, info[4].As<Napi::Function>(),
                           info.Length() > 5
                               ? std::optional<Napi::Function>(info[5].As<Napi::Function>())
                               : std::nullopt};
  return RunChainWithCallbacks(
      env, chain, progress,
      [&] { return chain.process_mono_cancellable(typed.Data(), typed.ElementLength(), sr); },
      [&](const sonare::mastering::api::MonoChainResult& result) -> Napi::Value {
        return MonoResultToObject(env, result);
      });
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasterAudioStereoWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsDocumentOrAbsent(info[0])) {
    Napi::TypeError::New(env, "presetName must be a string or null").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[2])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[5].IsFunction()) {
    Napi::TypeError::New(env, "onProgress must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() > 6 && !info[6].IsFunction()) {
    Napi::TypeError::New(env, "cancel must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::optional<std::string> preset_name;
  if (info[0].IsString()) preset_name = node_narrow_string(env, info[0], "presetName");
  auto left = info[1].As<Napi::Float32Array>();
  auto right = info[2].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[3], "sampleRate");
  sonare::validate_offline_audio_input(nullptr, "left", left.Data(), left.ElementLength(), sr);
  sonare::validate_offline_audio_input(nullptr, "right", right.Data(), right.ElementLength(), sr);
  std::optional<std::string> overrides;
  if (!OptionalStringArg(env, info, 4, "overridesJson", &overrides)) return env.Undefined();
  sonare::mastering::api::MasteringChain chain(PresetChainConfig(preset_name, overrides));
  // Pointers lent across the callbacks for the same reason as the chain variants.
  ProgressContext progress{env, info[5].As<Napi::Function>(),
                           info.Length() > 6
                               ? std::optional<Napi::Function>(info[6].As<Napi::Function>())
                               : std::nullopt};
  return RunChainWithCallbacks(
      env, chain, progress,
      [&] {
        return chain.process_stereo_cancellable(left.Data(), right.Data(), left.ElementLength(),
                                                sr);
      },
      [&](const sonare::mastering::api::StereoChainResult& result) -> Napi::Value {
        return StereoResultToObject(env, result);
      });
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairAnalyze(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info[0].IsArray()) {
    Napi::TypeError::New(env, "channels must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsString()) {
    Napi::TypeError::New(env, "requestJson must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  repair_detail::LinkedPlanes planes;
  if (!repair_detail::ReadLinkedPlanes(env, info[0], &planes)) {
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  const std::string request = node_narrow_string(env, info[2], "requestJson");
  char* json = nullptr;
  const SonareError err = sonare_mastering_repair_analyze(
      planes.in_ptrs.data(), planes.in_ptrs.size(), planes.length, sr, request.c_str(), &json);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::String out = Napi::String::New(env, json);
  sonare_free_string(json);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairApply(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info[0].IsArray()) {
    Napi::TypeError::New(env, "channels must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsString()) {
    Napi::TypeError::New(env, "stagesJson must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsFunction()) {
    Napi::TypeError::New(env, "onProgress must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[4].IsFunction()) {
    Napi::TypeError::New(env, "cancel must be a function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  repair_detail::LinkedPlanes planes;
  if (!repair_detail::ReadLinkedPlanes(env, info[0], &planes)) {
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  const std::string stages = node_narrow_string(env, info[2], "stagesJson");
  // The input planes are lent across the callbacks: the core copies them before the first one.
  ProgressContext progress{env, info[3].As<Napi::Function>(),
                           std::optional<Napi::Function>(info[4].As<Napi::Function>())};
  char* reports = nullptr;
  const SonareError err = sonare_mastering_repair_apply(
      planes.in_ptrs.data(), planes.in_ptrs.size(), planes.length, sr, stages.c_str(),
      ReportProgress, &progress, planes.out_ptrs.data(), &reports, CancellationRequested,
      &progress);
  // A callback's pending exception wins over the error code it caused.
  if (env.IsExceptionPending()) {
    sonare_free_string(reports);
    return env.Undefined();
  }
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("channels", repair_detail::EmitLinkedChannels(env, planes));
  out.Set("reportsJson", Napi::String::New(env, reports));
  sonare_free_string(reports);
  return out;
  SONARE_NODE_CATCH(env)
}
