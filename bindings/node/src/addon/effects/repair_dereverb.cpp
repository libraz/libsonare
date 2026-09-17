/// @file
/// @brief Node bindings for the dereverb repair.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "core/audio.h"
#include "effects/repair_common.h"
#include "mastering/repair/dereverb_classical.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;
using namespace sonare_node::repair_detail;

namespace {

/// @brief Read a dereverb options bag over @p config, leaving absent keys alone.
sonare::mastering::repair::DereverbClassicalConfig read_dereverb_config(
    const Napi::Object& options, sonare::mastering::repair::DereverbClassicalConfig config) {
  config.threshold = FloatProperty(options, "threshold", config.threshold);
  config.attenuation = FloatProperty(options, "attenuation", config.attenuation);
  config.n_fft = IntProperty(options, "nFft", config.n_fft);
  config.hop_length = IntProperty(options, "hopLength", config.hop_length);
  config.t60_sec = FloatProperty(options, "t60Sec", config.t60_sec);
  config.late_delay_ms = FloatProperty(options, "lateDelayMs", config.late_delay_ms);
  config.over_subtraction = FloatProperty(options, "overSubtraction", config.over_subtraction);
  config.spectral_floor = FloatProperty(options, "spectralFloor", config.spectral_floor);
  config.wpe_enabled = BoolProperty(options, "wpeEnabled", config.wpe_enabled);
  config.wpe_iterations = IntProperty(options, "wpeIterations", config.wpe_iterations);
  config.wpe_taps = IntProperty(options, "wpeTaps", config.wpe_taps);
  config.wpe_strength = FloatProperty(options, "wpeStrength", config.wpe_strength);
  return config;
}

// The library default (sonare_c_mastering.h), the base an options bag is read over.
constexpr SonareDereverbClassicalConfig kDereverbConfigDefaults{0.0f, 1.0f,  1024, 256, 0.4f, 50.0f,
                                                                1.0f, 0.08f, 0,    2,   3,    0.7f};

/// @brief Marshal the pair-level reverb analysis into the JS shape the dereverb
///        stereo report carries.
Napi::Object EmitReverbDetection(Napi::Env env, const SonareReverbDetection& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("lateDecayRatioDb", Napi::Number::New(env, detection.late_decay_ratio_db));
  out.Set("latePredictability", Napi::Number::New(env, detection.late_predictability));
  return out;
}

Napi::Object EmitDereverbReport(Napi::Env env, const SonareDereverbReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitReverbDetection(env, report.detected));
  out.Set("meanReductionDb", Napi::Number::New(env, report.mean_reduction_db));
  out.Set("suppressedFraction", Napi::Number::New(env, report.suppressed_fraction));
  out.Set("wpePredictorNorm", Napi::Number::New(env, report.wpe_predictor_norm));
  return out;
}

/// @brief Read a SonareDereverbClassicalConfig options bag, applying the same
///        field names and defaults as read_dereverb_config above -- the two
///        structs share their shape, this is the C-ABI mirror of that reading.
SonareDereverbClassicalConfig read_dereverb_config_c(const Napi::Object& options,
                                                     SonareDereverbClassicalConfig config) {
  config.threshold = FloatProperty(options, "threshold", config.threshold);
  config.attenuation = FloatProperty(options, "attenuation", config.attenuation);
  config.n_fft = IntProperty(options, "nFft", config.n_fft);
  config.hop_length = IntProperty(options, "hopLength", config.hop_length);
  config.t60_sec = FloatProperty(options, "t60Sec", config.t60_sec);
  config.late_delay_ms = FloatProperty(options, "lateDelayMs", config.late_delay_ms);
  config.over_subtraction = FloatProperty(options, "overSubtraction", config.over_subtraction);
  config.spectral_floor = FloatProperty(options, "spectralFloor", config.spectral_floor);
  config.wpe_enabled = BoolProperty(options, "wpeEnabled", config.wpe_enabled != 0) ? 1 : 0;
  config.wpe_iterations = IntProperty(options, "wpeIterations", config.wpe_iterations);
  config.wpe_taps = IntProperty(options, "wpeTaps", config.wpe_taps);
  config.wpe_strength = FloatProperty(options, "wpeStrength", config.wpe_strength);
  return config;
}

/// @brief Frees both heap-owned channels of a SonareDereverbStereoResult on
///        scope exit -- mirrors DenoiseStereoResultGuard above.
class DereverbStereoResultGuard {
 public:
  explicit DereverbStereoResultGuard(SonareDereverbStereoResult* result) : result_(result) {}
  DereverbStereoResultGuard(const DereverbStereoResultGuard&) = delete;
  DereverbStereoResultGuard& operator=(const DereverbStereoResultGuard&) = delete;
  ~DereverbStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareDereverbStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairDereverbClassical(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DereverbClassicalConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    config = read_dereverb_config(info[2].As<Napi::Object>(), config);
  }
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    Napi::RangeError::New(env, "nFft must be a positive power of two").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (config.hop_length <= 0 || config.hop_length > config.n_fft) {
    Napi::RangeError::New(env, "hopLength must be in (0, nFft]").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::dereverb_classical(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDereverbClassicalStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(
        env, "masteringRepairDereverbClassicalStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDereverbClassicalConfig),
  // applied before any options key overrides a field.
  SonareDereverbClassicalConfig config{0.0f, 1.0f,  1024, 256, 0.4f, 50.0f,
                                       1.0f, 0.08f, 0,    2,   3,    0.7f};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_dereverb_config_c(info[3].As<Napi::Object>(), config);
  }
  SonareDereverbStereoResult result{};
  SonareError err = sonare_mastering_repair_dereverb_classical_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  DereverbStereoResultGuard guard(&result);
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("report", EmitDereverbReport(env, result.report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDereverbConfigForRoom(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected (roomEstimate, config?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  Napi::Object estimate = info[0].As<Napi::Object>();
  // Read AND written: the caller's config is the base, and only the two fields
  // the measurement determines come back changed.
  sonare::mastering::repair::DereverbClassicalConfig config;
  if (info.Length() >= 2 && info[1].IsObject()) {
    config = read_dereverb_config(info[1].As<Napi::Object>(), config);
  }
  // Record fields, not options: an absent band array is "did not converge"
  // rather than a zero reverberation time.
  const std::vector<float> rt60_bands = FloatArrayProperty(estimate, "rt60Bands");
  sonare::mastering::repair::apply_room_measurement(config, sonare::mid_frequency_rt60(rt60_bands),
                                                    FloatProperty(estimate, "volume", 0.0f));

  Napi::Object out = Napi::Object::New(env);
  out.Set("threshold", Napi::Number::New(env, config.threshold));
  out.Set("attenuation", Napi::Number::New(env, config.attenuation));
  out.Set("nFft", Napi::Number::New(env, config.n_fft));
  out.Set("hopLength", Napi::Number::New(env, config.hop_length));
  out.Set("t60Sec", Napi::Number::New(env, config.t60_sec));
  out.Set("lateDelayMs", Napi::Number::New(env, config.late_delay_ms));
  out.Set("overSubtraction", Napi::Number::New(env, config.over_subtraction));
  out.Set("spectralFloor", Napi::Number::New(env, config.spectral_floor));
  out.Set("wpeEnabled", Napi::Boolean::New(env, config.wpe_enabled));
  out.Set("wpeIterations", Napi::Number::New(env, config.wpe_iterations));
  out.Set("wpeTaps", Napi::Number::New(env, config.wpe_taps));
  out.Set("wpeStrength", Napi::Number::New(env, config.wpe_strength));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectReverb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDereverbClassicalConfig config = kDereverbConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_dereverb_config_c(info[2].As<Napi::Object>(), config);
  SonareReverbDetection detection{};
  SonareError err = sonare_mastering_repair_detect_reverb(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitReverbDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDereverbClassicalLinked(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckLinkedArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  LinkedPlanes planes;
  if (!ReadLinkedPlanes(env, info[0], "masteringRepairDereverbClassicalLinked", &planes)) {
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDereverbClassicalConfig config = kDereverbConfigDefaults;
  if (info.Length() >= 3 && info[2].IsObject()) {
    config = read_dereverb_config_c(info[2].As<Napi::Object>(), config);
  }
  SonareDereverbReport report{};
  SonareError err = sonare_mastering_repair_dereverb_classical_linked(
      planes.in_ptrs.data(), planes.in_ptrs.size(), planes.length, sr, &config,
      planes.out_ptrs.data(), &report);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("channels", EmitLinkedChannels(env, planes));
  out.Set("report", EmitDereverbReport(env, report));
  return out;
  SONARE_NODE_CATCH(env)
}
