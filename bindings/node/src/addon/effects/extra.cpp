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

Napi::Value EffectsCheckCResult(Napi::Env env, SonareError err) {
  Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
  return env.Undefined();
}

std::vector<int> EffectsIntVectorFromValue(const Napi::Value& value) {
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_int32_array) {
    auto arr = value.As<Napi::Int32Array>();
    return std::vector<int>(arr.Data(), arr.Data() + arr.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    std::vector<int> out(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      out[i] = arr.Get(i).As<Napi::Number>().Int32Value();
    }
    return out;
  }
  throw Napi::TypeError::New(value.Env(), "Expected Int32Array or number[]");
}

}  // namespace

Napi::Value SonareWrap::VoiceCharacterPresetId(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetOrdinal)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int ordinal = info[0].As<Napi::Number>().Int32Value();
  const char* id =
      sonare_voice_character_preset_id(static_cast<SonareVoiceCharacterPreset>(ordinal));
  if (id == nullptr || id[0] == '\0') return env.Null();
  return Napi::String::New(env, id);
}

Napi::Value SonareWrap::RealtimeVoiceChangerPresetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetOrdinal)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int ordinal = info[0].As<Napi::Number>().Int32Value();
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
}

Napi::Value SonareWrap::Decompose(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, nFeatures, nFrames, nComponents, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int n_features = info[1].As<Napi::Number>().Int32Value();
  int n_frames = info[2].As<Napi::Number>().Int32Value();
  int n_components = info[3].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "decompose", n_features, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  int n_iter =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 50;
  float beta =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 2.0f;
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
}

Napi::Value SonareWrap::NnFilter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, nFeatures, nFrames, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int n_features = info[1].As<Napi::Number>().Int32Value();
  int n_frames = info[2].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "nnFilter", n_features, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  std::string aggregate =
      info.Length() >= 4 && info[3].IsString() ? info[3].As<Napi::String>().Utf8Value() : "mean";
  int k = info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 7;
  int width =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 1;
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
  std::vector<int> intervals = EffectsIntVectorFromValue(info[1]);
  if (intervals.size() % 2 != 0) {
    Napi::TypeError::New(env, "remix intervals must be (start, end) pairs")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
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

Napi::Value SonareWrap::HpssWithResidual(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int kernel_harmonic =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 31;
  int kernel_percussive =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 31;
  float* out_harmonic = nullptr;
  float* out_percussive = nullptr;
  float* out_residual = nullptr;
  size_t out_length = 0;
  int out_sample_rate = 0;
  SonareError err = sonare_hpss_with_residual(arr.Data(), arr.ElementLength(), sr, kernel_harmonic,
                                              kernel_percussive, &out_harmonic, &out_percussive,
                                              &out_residual, &out_length, &out_sample_rate);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("harmonic", EffectsFloatResult(env, out_harmonic, out_length));
  result.Set("percussive", EffectsFloatResult(env, out_percussive, out_length));
  result.Set("residual", EffectsFloatResult(env, out_residual, out_length));
  result.Set("sampleRate", Napi::Number::New(env, out_sample_rate));
  return result;
}

Napi::Value SonareWrap::PhaseVocoder(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, rate, nFft?, hopLength?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float rate = info[2].As<Napi::Number>().FloatValue();
  int n_fft =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 512;
  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_phase_vocoder(arr.Data(), arr.ElementLength(), sr, rate, n_fft,
                                         hop_length, &out, &out_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  return EffectsFloatResult(env, out, out_length);
}
