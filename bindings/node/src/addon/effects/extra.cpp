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
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
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
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;

// ============================================================================
// Effects - librosa.decompose / effects.remix / hpss-with-residual /
// phase-vocoder, wired through the flat C ABI (sonare_c_effects.h).
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

// Input planes for a channel-linked call that returns a freshly heap-allocated
// result rather than writing into caller-owned output planes (unlike the
// repair family's LinkedPlanes), so only the input side is read here.
struct ChannelInputs {
  std::vector<Napi::Float32Array> arrays;
  std::vector<const float*> ptrs;
  size_t length = 0;
};

// Refuses what the C form cannot express: a non-Float32Array element, and a
// length disagreement, which the single `length` argument has no way to carry.
// An empty list goes through to the C entry, which owns that rejection.
bool ReadChannelInputs(Napi::Env env, const Napi::Value& value, const char* fn_name,
                       ChannelInputs* inputs) {
  Napi::Array channels = value.As<Napi::Array>();
  const uint32_t count = channels.Length();
  inputs->arrays.reserve(count);
  inputs->ptrs.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    Napi::Value channel = channels.Get(index);
    if (!IsFloat32Array(channel)) {
      Napi::TypeError::New(env, std::string(fn_name) + ": every channel must be a Float32Array")
          .ThrowAsJavaScriptException();
      return false;
    }
    inputs->arrays.push_back(channel.As<Napi::Float32Array>());
    const size_t length = inputs->arrays.back().ElementLength();
    if (index == 0) {
      inputs->length = length;
    } else if (length != inputs->length) {
      Napi::Error::New(env, std::string(fn_name) + ": every channel must have the same length")
          .ThrowAsJavaScriptException();
      return false;
    }
    inputs->ptrs.push_back(inputs->arrays.back().Data());
  }
  return true;
}

// Shared by decomposeStems and decomposeStemsLinked. @p init receives the
// "init" string so its storage outlives the C call that reads config.init.
SonareDecomposeStemsConfig ReadDecomposeStemsConfig(const Napi::Value& value, std::string* init) {
  SonareDecomposeStemsConfig config{};
  config.struct_version = 1;
  if (!value.IsObject()) return config;
  Napi::Object options = value.As<Napi::Object>();
  config.n_components = IntProperty(options, "nComponents", kZeroIsSentinel);
  config.n_fft = IntProperty(options, "nFft", kZeroIsSentinel);
  config.hop_length = IntProperty(options, "hopLength", kZeroIsSentinel);
  config.n_iter = IntProperty(options, "nIter", kZeroIsSentinel);
  config.beta = FloatProperty(options, "beta", 0.0f);
  config.mask_power = FloatProperty(options, "maskPower", 0.0f);
  Napi::Value init_value = options.Get("init");
  if (init_value.IsString()) {
    *init = init_value.As<Napi::String>().Utf8Value();
    config.init = init->c_str();
  }
  return config;
}

}  // namespace

Napi::Value SonareWrap::VoiceCharacterPresetId(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetOrdinal)").ThrowAsJavaScriptException();
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
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetOrdinal)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int ordinal = node_narrow_int(env, info[0], "ordinal");
  SonareRealtimeVoiceChangerConfig config{};
  SonareError err = sonare_realtime_voice_changer_preset_config(
      static_cast<SonareVoiceCharacterPreset>(ordinal), &config);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  Napi::Object out = Napi::Object::New(env);
  out.Set("inputGainDb", Napi::Number::New(env, config.input_gain_db));
  out.Set("outputGainDb", Napi::Number::New(env, config.output_gain_db));
  out.Set("wetMix", Napi::Number::New(env, config.wet_mix));
  out.Set("retuneSemitones", Napi::Number::New(env, config.retune_semitones));
  out.Set("retuneMix", Napi::Number::New(env, config.retune_mix));
  out.Set("retuneGrainSize", Napi::Number::New(env, config.retune_grain_size));
  out.Set("formantFactor", Napi::Number::New(env, config.formant_factor));
  out.Set("formantAmount", Napi::Number::New(env, config.formant_amount));
  out.Set("formantBody", Napi::Number::New(env, config.formant_body));
  out.Set("formantBrightness", Napi::Number::New(env, config.formant_brightness));
  out.Set("formantNasal", Napi::Number::New(env, config.formant_nasal));
  out.Set("eqHighpassHz", Napi::Number::New(env, config.eq_highpass_hz));
  out.Set("eqBodyDb", Napi::Number::New(env, config.eq_body_db));
  out.Set("eqPresenceDb", Napi::Number::New(env, config.eq_presence_db));
  out.Set("eqAirDb", Napi::Number::New(env, config.eq_air_db));
  out.Set("gateThresholdDb", Napi::Number::New(env, config.gate_threshold_db));
  out.Set("gateAttackMs", Napi::Number::New(env, config.gate_attack_ms));
  out.Set("gateReleaseMs", Napi::Number::New(env, config.gate_release_ms));
  out.Set("gateRangeDb", Napi::Number::New(env, config.gate_range_db));
  out.Set("compressorThresholdDb", Napi::Number::New(env, config.compressor_threshold_db));
  out.Set("compressorRatio", Napi::Number::New(env, config.compressor_ratio));
  out.Set("compressorAttackMs", Napi::Number::New(env, config.compressor_attack_ms));
  out.Set("compressorReleaseMs", Napi::Number::New(env, config.compressor_release_ms));
  out.Set("compressorMakeupGainDb", Napi::Number::New(env, config.compressor_makeup_gain_db));
  out.Set("deesserFrequencyHz", Napi::Number::New(env, config.deesser_frequency_hz));
  out.Set("deesserThresholdDb", Napi::Number::New(env, config.deesser_threshold_db));
  out.Set("deesserRatio", Napi::Number::New(env, config.deesser_ratio));
  out.Set("deesserRangeDb", Napi::Number::New(env, config.deesser_range_db));
  out.Set("reverbMix", Napi::Number::New(env, config.reverb_mix));
  out.Set("reverbTimeMs", Napi::Number::New(env, config.reverb_time_ms));
  out.Set("reverbDamping", Napi::Number::New(env, config.reverb_damping));
  out.Set("reverbSeed", Napi::Number::New(env, config.reverb_seed));
  out.Set("limiterCeilingDb", Napi::Number::New(env, config.limiter_ceiling_db));
  out.Set("limiterReleaseMs", Napi::Number::New(env, config.limiter_release_ms));
  out.Set("limiterEnableIspLimiter",
          Napi::Boolean::New(env, config.limiter_enable_isp_limiter != 0));
  out.Set("limiterIspCeilingDbtp", Napi::Number::New(env, config.limiter_isp_ceiling_dbtp));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Decompose(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, nFeatures, nFrames, nComponents, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int n_features = node_narrow_int(env, info[1], "nFeatures");
  int n_frames = node_narrow_int(env, info[2], "nFrames");
  int n_components = node_narrow_int(env, info[3], "nComponents");
  if (!ValidateMatrixDims(env, "decompose", n_features, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  int n_iter = node_arg_int(info, 4, 50);
  float beta = node_arg_finite_float(info, 5, 2.0f);
  // Optional 7th arg selects the initialiser ("random" | "nndsvd"). When given,
  // route through the with-init variant for the NNDSVD warm-start.
  std::string init =
      info.Length() >= 7 && info[6].IsString() ? info[6].As<Napi::String>().Utf8Value() : "";
  float* out_w = nullptr;
  size_t out_w_length = 0;
  float* out_h = nullptr;
  size_t out_h_length = 0;
  SonareError err =
      init.empty()
          ? sonare_decompose(arr.Data(), n_features, n_frames, n_components, n_iter, beta, &out_w,
                             &out_w_length, &out_h, &out_h_length)
          : sonare_decompose_with_init(arr.Data(), n_features, n_frames, n_components, n_iter, beta,
                                       init.c_str(), &out_w, &out_w_length, &out_h, &out_h_length);
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
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = node_narrow_int(env, info[1], "sr");
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
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
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
  if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array[] channels, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  ChannelInputs inputs;
  if (!ReadChannelInputs(env, info[0], "decomposeStemsLinked", &inputs)) {
    return env.Undefined();
  }
  int sr = node_narrow_int(env, info[1], "sr");
  // The init string must outlive the C call, so keep it in a local.
  std::string init;
  const SonareDecomposeStemsConfig config =
      ReadDecomposeStemsConfig(info.Length() >= 3 ? info[2] : env.Undefined(), &init);
  float* out = nullptr;
  size_t component_count = 0;
  size_t channel_count = 0;
  size_t component_length = 0;
  float* out_w = nullptr;
  size_t out_w_length = 0;
  float* out_h = nullptr;
  size_t out_h_length = 0;
  SonareError err = sonare_decompose_stems_linked(
      inputs.ptrs.data(), inputs.ptrs.size(), inputs.length, sr, &config, &out, &component_count,
      &channel_count, &component_length, &out_w, &out_w_length, &out_h, &out_h_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
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
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, nFeatures, nFrames, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int n_features = node_narrow_int(env, info[1], "nFeatures");
  int n_frames = node_narrow_int(env, info[2], "nFrames");
  if (!ValidateMatrixDims(env, "nnFilter", n_features, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  std::string aggregate =
      info.Length() >= 4 && info[3].IsString() ? info[3].As<Napi::String>().Utf8Value() : "mean";
  int k = node_arg_int(info, 4, 7);
  int width = node_arg_int(info, 5, 1);
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
  if (info.Length() < 2 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array, intervals, sampleRate?, alignZeros?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto arr = info[0].As<Napi::Float32Array>();
  std::vector<int> intervals = IntVectorFromValue(info[1], "intervals");
  if (intervals.size() % 2 != 0) {
    Napi::TypeError::New(env, "remix intervals must be (start, end) pairs")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr = node_arg_int(info, 2, 22050);
  int align_zeros =
      info.Length() >= 4 && info[3].IsBoolean() && info[3].As<Napi::Boolean>().Value() ? 1 : 0;
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
  if (info.Length() < 2 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array, intervals, sampleRate?, alignZeros?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto arr = info[0].As<Napi::Float32Array>();
  std::vector<int> intervals = IntVectorFromValue(info[1], "intervals");
  if (intervals.size() % 2 != 0) {
    Napi::TypeError::New(env, "remix intervals must be (start, end) pairs")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr = node_arg_int(info, 2, 22050);
  int align_zeros =
      info.Length() >= 4 && info[3].IsBoolean() ? (info[3].As<Napi::Boolean>().Value() ? 1 : 0) : 1;
  int* out = nullptr;
  size_t out_count = 0;
  SonareError err =
      sonare_remix_aligned_intervals(arr.Data(), arr.ElementLength(), sr, intervals.data(),
                                     intervals.size() / 2, align_zeros, &out, &out_count);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  return EffectsIntResult(env, out, out_count * 2);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::HpssWithResidual(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = node_narrow_int(env, info[1], "sr");
  // Shared with Hpss, so the defaults and the narrowing agree on both.
  HpssArguments args;
  if (!ReadHpssArguments(env, info, &args)) return env.Undefined();

  sonare::validate_offline_audio_input(arr.Data(), arr.ElementLength(), sr);
  sonare::Audio audio = sonare::Audio::from_buffer(arr.Data(), arr.ElementLength(), sr);
  sonare::HpssConfig config;
  config.kernel_size_harmonic = args.kernel_harmonic;
  config.kernel_size_percussive = args.kernel_percussive;
  config.use_soft_mask = !args.hard_mask;
  sonare::StftConfig stft_config;
  stft_config.n_fft = args.n_fft;
  stft_config.hop_length = args.hop_length;
  sonare::HpssAudioResultWithResidual source_result =
      sonare::hpss_with_residual(audio, config, stft_config);

  const size_t out_length = source_result.harmonic.size();
  Napi::Float32Array out_harmonic = Napi::Float32Array::New(env, out_length);
  Napi::Float32Array out_percussive = Napi::Float32Array::New(env, out_length);
  Napi::Float32Array out_residual = Napi::Float32Array::New(env, out_length);
  if (out_length > 0) {
    std::memcpy(out_harmonic.Data(), source_result.harmonic.data(), out_length * sizeof(float));
    std::memcpy(out_percussive.Data(), source_result.percussive.data(), out_length * sizeof(float));
    std::memcpy(out_residual.Data(), source_result.residual.data(), out_length * sizeof(float));
  }
  Napi::Object result = Napi::Object::New(env);
  result.Set("harmonic", out_harmonic);
  result.Set("percussive", out_percussive);
  result.Set("residual", out_residual);
  result.Set("sampleRate", Napi::Number::New(env, source_result.harmonic.sample_rate()));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PhaseVocoder(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, rate, nFft?, hopLength?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = node_narrow_int(env, info[1], "sr");
  float rate = node_narrow_finite_float(env, info[2], "rate");
  int n_fft = node_arg_int(info, 3, sonare::constants::kDefaultNFft);
  int hop_length = node_arg_int(info, 4, sonare::constants::kDefaultHopLength);
  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_phase_vocoder(arr.Data(), arr.ElementLength(), sr, rate, n_fft,
                                         hop_length, &out, &out_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  return EffectsFloatResult(env, out, out_length);
  SONARE_NODE_CATCH(env)
}
