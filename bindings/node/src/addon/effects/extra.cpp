#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/voice_changer/voice_changer.h"
#include "effects/decompose.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/repair_common.h"
#include "effects/time_stretch.h"
#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/suggester.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_streaming.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;
using namespace sonare_node::repair_detail;

// ============================================================================
// Effects - librosa.decompose / effects.remix / phase-vocoder,
// wired through the flat C ABI (sonare_c_effects.h).
// ============================================================================

namespace {

// Copy a heap C buffer into a Float32Array and free the C allocation.
Napi::Float32Array EffectsFloatResult(Napi::Env env, float* data, size_t count) {
  auto out = Napi::Float32Array::New(env, count);
  if (count > 0 && data != nullptr) {
    std::memcpy(out.Data(), data, count * sizeof(float));
  }
  sonare_free_floats(data);
  return out;
}

// Copy a heap C int buffer into an Int32Array and free the C allocation.
Napi::Int32Array EffectsIntResult(Napi::Env env, int* data, size_t count) {
  auto out = Napi::Int32Array::New(env, count);
  if (count > 0 && data != nullptr) {
    std::memcpy(out.Data(), data, count * sizeof(int));
  }
  sonare_free_ints(data);
  return out;
}

Napi::Value EffectsCheckCResult(Napi::Env env, SonareError err) {
  sonare_node::ThrowSonareError(env, err);
  return env.Undefined();
}

// Shared by decomposeStems and decomposeStemsLinked. @p init receives the
// "init" string so its storage outlives the C call that reads config.init.
SonareDecomposeStemsConfig ReadDecomposeStemsConfig(const Napi::Value& value, std::string* init) {
  SonareDecomposeStemsConfig config;
  sonare_decompose_stems_config_init(&config);
  if (!value.IsObject()) return config;
  Napi::Object options = value.As<Napi::Object>();
  config.n_components = IntProperty(options, "nComponents", config.n_components);
  config.n_fft = IntProperty(options, "nFft", config.n_fft);
  config.hop_length = DomainProperty<int>(options, "hopLength", config.hop_length,
                                          sonare::kDecomposeStemsHopLengthDomain);
  config.n_iter = IntProperty(options, "nIter", config.n_iter);
  config.beta = FloatProperty(options, "beta", config.beta);
  config.mask_power = FloatProperty(options, "maskPower", config.mask_power);
  if (!options.Get("init").IsUndefined() && !options.Get("init").IsNull()) {
    *init = StringProperty(options, "init", "");
    config.init = init->c_str();
  }
  return config;
}

/// @brief Reads a remix `intervals` list, refusing a trailing start with no end.
/// @throws Napi::RangeError for an odd length.
std::vector<int> ReadRemixIntervals(Napi::Env env, const Napi::Value& value) {
  std::vector<int> intervals = IntVectorFromValue(value, "intervals");
  if (intervals.size() % 2 != 0) {
    throw Napi::RangeError::New(env, "remix intervals must be (start, end) pairs");
  }
  return intervals;
}

}  // namespace

Napi::Value SonareWrap::VoiceCharacterPresetId(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!info[0].IsNumber()) {
    Napi::TypeError::New(env, "presetOrdinal must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int ordinal = node_narrow_int(env, info[0], "ordinal");
  const char* id =
      sonare_voice_character_preset_id(static_cast<SonareVoiceCharacterPreset>(ordinal));
  if (id == nullptr || id[0] == '\0') return env.Null();
  return Napi::String::New(env, id);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RealtimeVoiceChangerPresetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!info[0].IsNumber()) {
    Napi::TypeError::New(env, "presetOrdinal must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int ordinal = node_narrow_int(env, info[0], "ordinal");
  const char* id =
      sonare_voice_character_preset_id(static_cast<SonareVoiceCharacterPreset>(ordinal));
  if (id == nullptr || id[0] == '\0') {
    return EffectsCheckCResult(env, SONARE_ERROR_INVALID_PARAMETER);
  }
  namespace vc = sonare::editing::voice_changer;
  return RealtimeVoiceChangerConfigObject(
      env, vc::realtime_voice_changer_preset(vc::realtime_voice_changer_preset_from_id(id)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Decompose(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  int n_features{};
  if (!RequiredDomainArg(env, info, 1, "nFeatures", sonare::kDecomposeNFeaturesDomain,
                         &n_features)) {
    return env.Undefined();
  }
  int n_frames{};
  if (!RequiredDomainArg(env, info, 2, "nFrames", sonare::kDecomposeNFramesDomain, &n_frames)) {
    return env.Undefined();
  }
  int n_components{};
  if (!RequiredDomainArg(env, info, 3, "nComponents", sonare::kDecomposeNComponentsDomain,
                         &n_components)) {
    return env.Undefined();
  }
  if (!ValidateMatrixDims(env, n_features, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  int n_iter{};
  if (!OptionalDomainArg(env, info, 4, "nIter", 0, sonare::kDecomposeIterDomain, &n_iter)) {
    return env.Undefined();
  }
  float beta{};
  if (!OptionalFloatArg(env, info, 5, "beta", sonare::kDecomposeDefaultBeta, &beta)) {
    return env.Undefined();
  }
  // Optional 7th arg selects the initialiser ("random" | "nndsvd"); omitted is the default.
  std::string init;
  if (!OptionalStringArg(env, info, 6, "init", "", &init)) {
    return env.Undefined();
  }
  SonareDecomposeOptions options;
  sonare_decompose_options_init(&options);
  if (!init.empty()) options.init = init.c_str();
  float* out_w = nullptr;
  size_t out_w_length = 0;
  float* out_h = nullptr;
  size_t out_h_length = 0;
  SonareError err = sonare_decompose(arr.Data(), n_features, n_frames, n_components, n_iter, beta,
                                     &options, &out_w, &out_w_length, &out_h, &out_h_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  Napi::Object w = Napi::Object::New(env);
  w.Set("rows", Napi::Number::New(env, n_features));
  w.Set("cols", Napi::Number::New(env, n_components));
  w.Set("data", EffectsFloatResult(env, out_w, out_w_length));
  Napi::Object h = Napi::Object::New(env);
  h.Set("rows", Napi::Number::New(env, n_components));
  h.Set("cols", Napi::Number::New(env, n_frames));
  h.Set("data", EffectsFloatResult(env, out_h, out_h_length));
  Napi::Object result = Napi::Object::New(env);
  result.Set("w", w);
  result.Set("h", h);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DecomposeStems(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireMonoArgs(info)) return env.Undefined();
  SONARE_NODE_TRY
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  // The init string must outlive the C call, so keep it in a local.
  std::string init;
  const SonareDecomposeStemsConfig config =
      ReadDecomposeStemsConfig(info.Length() >= 3 ? info[2] : env.Undefined(), &init);
  float* out = nullptr;
  size_t component_count = 0;
  size_t component_length = 0;
  float* out_w = nullptr;
  size_t out_w_length = 0;
  float* out_h = nullptr;
  size_t out_h_length = 0;
  SonareError err =
      sonare_decompose_stems(arr.Data(), arr.ElementLength(), sr, &config, &out, &component_count,
                             &component_length, &out_w, &out_w_length, &out_h, &out_h_length);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Array components = Napi::Array::New(env, component_count);
  for (size_t c = 0; c < component_count; ++c) {
    auto component = Napi::Float32Array::New(env, component_length);
    if (component_length > 0 && out != nullptr) {
      std::memcpy(component.Data(), out + c * component_length, component_length * sizeof(float));
    }
    components.Set(static_cast<uint32_t>(c), component);
  }
  sonare_free_floats(out);
  Napi::Object result = Napi::Object::New(env);
  result.Set("components", components);
  result.Set("w", EffectsFloatResult(env, out_w, out_w_length));
  result.Set("h", EffectsFloatResult(env, out_h, out_h_length));
  result.Set("sampleRate", Napi::Number::New(env, sr));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DecomposeStemsLinked(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!CheckLinkedArgs(env, info)) return env.Undefined();
  ChannelInputs inputs;
  if (!ReadChannelInputs(env, info[0], &inputs)) {
    return env.Undefined();
  }
  int sr = node_narrow_int(env, info[1], "sampleRate");
  // The init string must outlive the C call, so keep it in a local.
  std::string init;
  const SonareDecomposeStemsConfig config =
      ReadDecomposeStemsConfig(info.Length() >= 3 ? info[2] : env.Undefined(), &init);
  float* out = nullptr;
  size_t component_count = 0;
  const size_t channel_count = inputs.ptrs.size();
  size_t component_length = 0;
  float* out_w = nullptr;
  size_t out_w_length = 0;
  float* out_h = nullptr;
  size_t out_h_length = 0;
  SonareError err = sonare_decompose_stems_linked(
      inputs.ptrs.data(), inputs.ptrs.size(), inputs.length, sr, &config, &out, &component_count,
      &component_length, &out_w, &out_w_length, &out_h, &out_h_length);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  // Layout: component c, channel ch starts at (c * channel_count + ch) * component_length.
  Napi::Array components = Napi::Array::New(env, component_count);
  for (size_t c = 0; c < component_count; ++c) {
    Napi::Array component_channels = Napi::Array::New(env, channel_count);
    for (size_t ch = 0; ch < channel_count; ++ch) {
      auto slice = Napi::Float32Array::New(env, component_length);
      if (component_length > 0 && out != nullptr) {
        std::memcpy(slice.Data(), out + (c * channel_count + ch) * component_length,
                    component_length * sizeof(float));
      }
      component_channels.Set(static_cast<uint32_t>(ch), slice);
    }
    components.Set(static_cast<uint32_t>(c), component_channels);
  }
  sonare_free_floats(out);
  Napi::Object result = Napi::Object::New(env);
  result.Set("components", components);
  result.Set("w", EffectsFloatResult(env, out_w, out_w_length));
  result.Set("h", EffectsFloatResult(env, out_h, out_h_length));
  result.Set("sampleRate", Napi::Number::New(env, sr));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NnFilter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "nFeatures must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "nFrames must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int n_features = node_narrow_int(env, info[1], "nFeatures");
  int n_frames = node_narrow_int(env, info[2], "nFrames");
  if (!ValidateMatrixDims(env, n_features, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  std::string aggregate;
  if (!OptionalStringArg(env, info, 3, "aggregate", "mean", &aggregate)) {
    return env.Undefined();
  }
  int k{};
  if (!OptionalIntArg(env, info, 4, "k", sonare::kNnFilterDefaultK, &k)) return env.Undefined();
  int width{};
  if (!OptionalIntArg(env, info, 5, "width", sonare::kNnFilterDefaultWidth, &width))
    return env.Undefined();
  // -1 is the C entry's spelling of "library default"; absence is this surface's.
  if (k < 1 || width < 0) {
    const std::string int_max = node_int_max_text();
    Napi::RangeError::New(env, k < 1 ? "k must be an integer in [1, " + int_max + "]"
                                     : "width must be an integer in [0, " + int_max + "]")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_nn_filter(arr.Data(), n_features, n_frames, aggregate.c_str(), k, width,
                                     &out, &out_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("rows", Napi::Number::New(env, n_features));
  result.Set("cols", Napi::Number::New(env, n_frames));
  result.Set("data", EffectsFloatResult(env, out, out_length));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Remix(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  auto arr = info[0].As<Napi::Float32Array>();
  std::vector<int> intervals = ReadRemixIntervals(env, info[1]);
  int sr{};
  if (!RequiredIntArg(env, info, 2, "sampleRate", &sr)) return env.Undefined();
  bool align_zeros = true;
  if (!OptionalBoolArg(env, info, 3, "alignZeros", true, &align_zeros)) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_remix(arr.Data(), arr.ElementLength(), sr, intervals.data(),
                                 intervals.size() / 2, align_zeros, &out, &out_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  return EffectsFloatResult(env, out, out_length);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RemixAlignedIntervals(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  auto arr = info[0].As<Napi::Float32Array>();
  std::vector<int> intervals = ReadRemixIntervals(env, info[1]);
  int sr{};
  if (!RequiredIntArg(env, info, 2, "sampleRate", &sr)) return env.Undefined();
  bool align_zeros = true;
  if (!OptionalBoolArg(env, info, 3, "alignZeros", true, &align_zeros)) {
    return env.Undefined();
  }
  int* out = nullptr;
  size_t out_count = 0;
  SonareError err =
      sonare_remix_aligned_intervals(arr.Data(), arr.ElementLength(), sr, intervals.data(),
                                     intervals.size() / 2, align_zeros, &out, &out_count);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  return EffectsIntResult(env, out, out_count * 2);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PhaseVocoder(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireMonoArgs(info)) return env.Undefined();
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "speedRatio must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  float rate = node_narrow_float(env, info[2], "speedRatio");
  int n_fft{};
  if (!OptionalIntArg(env, info, 3, "nFft", sonare::constants::kDefaultNFft, &n_fft))
    return env.Undefined();
  int hop_length{};
  if (!OptionalResynthesisHopArg(env, info, 4, &hop_length)) return env.Undefined();
  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_phase_vocoder(arr.Data(), arr.ElementLength(), sr, rate, n_fft,
                                         hop_length, &out, &out_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  return EffectsFloatResult(env, out, out_length);
  SONARE_NODE_CATCH(env)
}
