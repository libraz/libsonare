/// @file
/// @brief Node bindings for the impulsive-defect repairs: declick, declip, decrackle.

#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

#include "core/audio.h"
#include "effects/repair_common.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;
using namespace sonare_node::repair_detail;

namespace {

sonare::mastering::repair::DecrackleMode parse_decrackle_mode(
    const Napi::Object& options, sonare::mastering::repair::DecrackleMode fallback) {
  using sonare::mastering::repair::DecrackleMode;
  static constexpr NodeChoice<DecrackleMode> kChoices[] = {
      {"median", DecrackleMode::Median},
      {"waveletshrinkage", DecrackleMode::WaveletShrinkage},
      {"wavelet_shrinkage", DecrackleMode::WaveletShrinkage},
      {"wavelet", DecrackleMode::WaveletShrinkage}};
  return ChoiceProperty(options, "mode", kChoices, fallback);
}

// The library default (sonare_c_mastering.h), the base an options bag is read over.
constexpr SonareDeclickConfig kDeclickConfigDefaults{0.8f, 4.0f, 8, 20, 8.0f};

// The library default (sonare_c_mastering.h), the base an options bag is read over.
// clip_threshold_db is 20*log10(0.98) dBFS.
constexpr SonareDeclipConfig kDeclipConfigDefaults{-0.17547849f, 36, 2, 0.65f};

// The library default (sonare_c_mastering.h), the base an options bag is read over.
constexpr SonareDecrackleConfig kDecrackleConfigDefaults{0.4f, SONARE_DECRACKLE_MODE_MEDIAN, 4};

/// @brief Marshal one channel's click detection into the JS shape shared by
///        the mono and stereo declick reports.
template <typename T>
Napi::Object EmitClickDetection(Napi::Env env, const T& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("count", Napi::Number::New(env, static_cast<double>(detection.count)));
  out.Set("rejected", Napi::Number::New(env, static_cast<double>(detection.rejected)));
  out.Set("longestRunSamples",
          Napi::Number::New(env, static_cast<double>(detection.longest_run_samples)));
  out.Set("perSecond", Napi::Number::New(env, detection.per_second));
  return out;
}

template <typename T>
Napi::Object EmitDeclickReport(Napi::Env env, const T& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitClickDetection(env, report.detected));
  out.Set("repairedRuns", Napi::Number::New(env, static_cast<double>(report.repaired_runs)));
  out.Set("repairedSamples", Napi::Number::New(env, static_cast<double>(report.repaired_samples)));
  out.Set("linkedRuns", Napi::Number::New(env, static_cast<double>(report.linked_runs)));
  out.Set("lpcModelUsed", Napi::Boolean::New(env, report.lpc_model_used != 0));
  return out;
}

/// @brief Read a SonareDeclickConfig options bag, applying the same field
///        names, defaults and maxClickSamples guard as the mono facade's C++
///        DeclickConfig mapping above -- the two structs share their shape,
///        this is the C-ABI mirror of that reading.
SonareDeclickConfig read_declick_config_c(Napi::Env env, const Napi::Object& options,
                                          SonareDeclickConfig config) {
  config.threshold = FloatProperty(options, "threshold", config.threshold);
  config.neighbor_ratio = FloatProperty(options, "neighborRatio", config.neighbor_ratio);
  if (options.Has("maxClickSamples")) {
    const int max_click_samples =
        IntProperty(options, "maxClickSamples", static_cast<int>(config.max_click_samples));
    if (max_click_samples <= 0) {
      throw Napi::RangeError::New(
          env, "maxClickSamples must be an integer in [1, " + node_int_max_text() + "]");
    }
    config.max_click_samples = static_cast<size_t>(max_click_samples);
  }
  config.lpc_order = IntProperty(options, "lpcOrder", config.lpc_order);
  config.residual_ratio = FloatProperty(options, "residualRatio", config.residual_ratio);
  return config;
}

/// @brief Marshal one channel's clip detection into the JS shape shared by the
///        declip stereo report.
template <typename T>
Napi::Object EmitClipDetection(Napi::Env env, const T& detection) {
  Napi::Object out = Napi::Object::New(env);
  if constexpr (std::is_same_v<T, SonareClipDetection>) {
    out.Set("sampleCount", Napi::Number::New(env, static_cast<double>(detection.clipped_samples)));
    out.Set("sampleFraction", Napi::Number::New(env, detection.clipped_ratio));
  } else {
    out.Set("sampleCount", Napi::Number::New(env, static_cast<double>(detection.sample_count)));
    out.Set("sampleFraction", Napi::Number::New(env, detection.sample_fraction));
  }
  out.Set("runCount", Napi::Number::New(env, static_cast<double>(detection.run_count)));
  out.Set("longestRunSamples",
          Napi::Number::New(env, static_cast<double>(detection.longest_run_samples)));
  out.Set("flatRunCount", Napi::Number::New(env, static_cast<double>(detection.flat_run_count)));
  out.Set("longestFlatRunSamples",
          Napi::Number::New(env, static_cast<double>(detection.longest_flat_run_samples)));
  out.Set("flatSampleCount",
          Napi::Number::New(env, static_cast<double>(detection.flat_sample_count)));
  out.Set("flatLevel", Napi::Number::New(env, detection.flat_level));
  return out;
}

template <typename T>
Napi::Object EmitDeclipReport(Napi::Env env, const T& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitClipDetection(env, report.detected));
  out.Set("lpcReconstructedRuns",
          Napi::Number::New(env, static_cast<double>(report.lpc_reconstructed_runs)));
  out.Set("interpolatedRuns",
          Napi::Number::New(env, static_cast<double>(report.interpolated_runs)));
  out.Set("repairedSamples", Napi::Number::New(env, static_cast<double>(report.repaired_samples)));
  out.Set("linkedRuns", Napi::Number::New(env, static_cast<double>(report.linked_runs)));
  return out;
}

/// @brief Read a SonareDeclipConfig options bag, applying the same field names
///        and defaults as the mono facade's C++ DeclipConfig mapping above --
///        the two structs share their shape, this is the C-ABI mirror of that
///        reading.
SonareDeclipConfig read_declip_config_c(const Napi::Object& options, SonareDeclipConfig config) {
  config.clip_threshold_db =
      FiniteFloatProperty(options, "clipThresholdDb", config.clip_threshold_db);
  config.lpc_order = IntProperty(options, "lpcOrder", config.lpc_order);
  config.iterations = IntProperty(options, "iterations", config.iterations);
  config.lpc_blend = FloatProperty(options, "lpcBlend", config.lpc_blend);
  return config;
}

/// @brief Marshal one channel's crackle detection into the JS shape shared by
///        the decrackle stereo report.
template <typename T>
Napi::Object EmitCrackleDetection(Napi::Env env, const T& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("sampleCount", Napi::Number::New(env, static_cast<double>(detection.sample_count)));
  out.Set("sampleFraction", Napi::Number::New(env, detection.sample_fraction));
  out.Set("perSecond", Napi::Number::New(env, detection.per_second));
  return out;
}

template <typename T>
Napi::Object EmitDecrackleReport(Napi::Env env, const T& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitCrackleDetection(env, report.detected));
  out.Set("replacedSamples", Napi::Number::New(env, static_cast<double>(report.replaced_samples)));
  out.Set("detailCoefficients",
          Napi::Number::New(env, static_cast<double>(report.detail_coefficients)));
  out.Set("shrunkCoefficients",
          Napi::Number::New(env, static_cast<double>(report.shrunk_coefficients)));
  out.Set("noiseSigma", Napi::Number::New(env, report.noise_sigma));
  return out;
}

/// @brief Read a SonareDecrackleConfig options bag, reusing the mono facade's
///        own mode-string reader above so the two paths cannot recognize
///        different spellings of the same mode.
SonareDecrackleConfig read_decrackle_config_c(const Napi::Object& options,
                                              SonareDecrackleConfig config) {
  config.threshold = FloatProperty(options, "threshold", config.threshold);
  config.mode = static_cast<int>(parse_decrackle_mode(
      options, static_cast<sonare::mastering::repair::DecrackleMode>(config.mode)));
  config.levels = IntProperty(options, "levels", config.levels);
  return config;
}

}  // namespace

Napi::Value SonareWrap::MasteringRepairDeclick(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input("samples", typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DeclickConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = FloatProperty(options, "threshold", config.threshold);
    config.neighbor_ratio = FloatProperty(options, "neighborRatio", config.neighbor_ratio);
    if (options.Has("maxClickSamples")) {
      const int max_click_samples =
          IntProperty(options, "maxClickSamples", static_cast<int>(config.max_click_samples));
      if (max_click_samples <= 0) {
        Napi::RangeError::New(
            env, "maxClickSamples must be an integer in [1, " + node_int_max_text() + "]")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      config.max_click_samples = static_cast<size_t>(max_click_samples);
    }
    config.lpc_order = IntProperty(options, "lpcOrder", config.lpc_order);
    config.residual_ratio = FloatProperty(options, "residualRatio", config.residual_ratio);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DeclickReport report;
  sonare::Audio result = sonare::mastering::repair::declick(audio, config, &report);
  return EmitMonoResult(env, result, EmitDeclickReport(env, report));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDeclickStereo(const Napi::CallbackInfo& info) {
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
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDeclickConfig), applied
  // before any options key overrides a field.
  SonareDeclickConfig config{0.8f, 4.0f, 8, 20, 8.0f};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_declick_config_c(env, info[3].As<Napi::Object>(), config);
  }
  SonareDeclickStereoResult result{};
  SonareError err = sonare_mastering_repair_declick_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  StereoResultGuard<SonareDeclickStereoResult> guard(&result);
  Napi::Object out = EmitStereoResult(env, result);
  out.Set("leftReport", EmitDeclickReport(env, result.left_report));
  out.Set("rightReport", EmitDeclickReport(env, result.right_report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectClicks(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDeclickConfig config = kDeclickConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_declick_config_c(env, info[2].As<Napi::Object>(), config);
  SonareClickDetection detection{};
  SonareError err = sonare_mastering_repair_detect_clicks(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitClickDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDeclip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input("samples", typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DeclipConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.clip_threshold = DbfsAsLinearProperty(options, "clipThresholdDb", config.clip_threshold,
                                                 sonare::mastering::repair::clip_threshold_from_db);
    config.lpc_order = IntProperty(options, "lpcOrder", config.lpc_order);
    config.iterations = IntProperty(options, "iterations", config.iterations);
    config.lpc_blend = FloatProperty(options, "lpcBlend", config.lpc_blend);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DeclipReport report;
  sonare::Audio result = sonare::mastering::repair::declip(audio, config, &report);
  return EmitMonoResult(env, result, EmitDeclipReport(env, report));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDeclipStereo(const Napi::CallbackInfo& info) {
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
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDeclipConfig), applied before
  // any options key overrides a field.
  SonareDeclipConfig config = kDeclipConfigDefaults;
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_declip_config_c(info[3].As<Napi::Object>(), config);
  }
  SonareDeclipStereoResult result{};
  SonareError err = sonare_mastering_repair_declip_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  StereoResultGuard<SonareDeclipStereoResult> guard(&result);
  Napi::Object out = EmitStereoResult(env, result);
  out.Set("leftReport", EmitDeclipReport(env, result.left_report));
  out.Set("rightReport", EmitDeclipReport(env, result.right_report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectClipping(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDeclipConfig config = kDeclipConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) {
    config = read_declip_config_c(info[2].As<Napi::Object>(), config);
  }
  SonareClipDetection detection{};
  SonareError err = sonare_mastering_repair_detect_clipping(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitClipDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDecrackle(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input("samples", typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DecrackleConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = FloatProperty(options, "threshold", config.threshold);
    config.mode = parse_decrackle_mode(options, config.mode);
    config.levels = IntProperty(options, "levels", config.levels);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DecrackleReport report;
  sonare::Audio result = sonare::mastering::repair::decrackle(audio, config, &report);
  return EmitMonoResult(env, result, EmitDecrackleReport(env, report));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDecrackleStereo(const Napi::CallbackInfo& info) {
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
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDecrackleConfig), applied before
  // any options key overrides a field.
  SonareDecrackleConfig config{0.4f, SONARE_DECRACKLE_MODE_MEDIAN, 4};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_decrackle_config_c(info[3].As<Napi::Object>(), config);
  }
  SonareDecrackleStereoResult result{};
  SonareError err = sonare_mastering_repair_decrackle_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  StereoResultGuard<SonareDecrackleStereoResult> guard(&result);
  Napi::Object out = EmitStereoResult(env, result);
  out.Set("leftReport", EmitDecrackleReport(env, result.left_report));
  out.Set("rightReport", EmitDecrackleReport(env, result.right_report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectCrackle(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDecrackleConfig config = kDecrackleConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_decrackle_config_c(info[2].As<Napi::Object>(), config);
  SonareCrackleDetection detection{};
  SonareError err = sonare_mastering_repair_detect_crackle(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitCrackleDetection(env, detection);
  SONARE_NODE_CATCH(env)
}
