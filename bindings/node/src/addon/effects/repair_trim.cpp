/// @file
/// @brief Node bindings for the silence-trim repair.

#include <cstddef>
#include <string>
#include <vector>

#include "core/audio.h"
#include "effects/repair_common.h"
#include "mastering/repair/trim_silence.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;
using namespace sonare_node::repair_detail;

namespace {

sonare::mastering::repair::TrimSilenceMode parse_trim_silence_mode(
    const Napi::Object& options, sonare::mastering::repair::TrimSilenceMode fallback) {
  using sonare::mastering::repair::TrimSilenceMode;
  static constexpr NodeChoice<TrimSilenceMode> kChoices[] = {
      {"peak", TrimSilenceMode::Peak},
      {"lufsgated", TrimSilenceMode::LufsGated},
      {"lufs_gated", TrimSilenceMode::LufsGated},
      {"lufs", TrimSilenceMode::LufsGated}};
  return ChoiceProperty(options, "mode", kChoices, fallback);
}

template <typename T>
Napi::Object EmitTrimRange(Napi::Env env, const T& range) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("first", Napi::Number::New(env, static_cast<double>(range.first)));
  out.Set("lastExclusive", Napi::Number::New(env, static_cast<double>(range.last_exclusive)));
  return out;
}

template <typename T>
Napi::Object EmitTrimReport(Napi::Env env, const T& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("range", EmitTrimRange(env, report.range));
  out.Set("removedHeadSamples",
          Napi::Number::New(env, static_cast<double>(report.removed_head_samples)));
  out.Set("removedTailSamples",
          Napi::Number::New(env, static_cast<double>(report.removed_tail_samples)));
  return out;
}

/// @brief Read a trim options bag over the library defaults, validated in the request's keys.
/// @details The read is type-only where the core states the domain; the core's own conversion and
///   validator then refuse in the request's spelling. @p threshold_db receives the dBFS threshold
///   the C entries take, the library default when the key is absent.
sonare::mastering::repair::TrimSilenceConfig ReadTrimConfig(const Napi::Object& options,
                                                            float* threshold_db) {
  sonare::mastering::repair::TrimSilenceConfig config;
  *threshold_db = sonare::mastering::repair::kTrimThresholdDefaultDb;
  config.threshold = DbfsAsLinearProperty(
      options, "thresholdDb", config.threshold, [threshold_db](float db, const char* key) {
        *threshold_db = db;
        return sonare::mastering::repair::trim_threshold_from_db(db, key);
      });
  if (options.Has("paddingSamples")) {
    const int padding_samples =
        IntProperty(options, "paddingSamples", static_cast<int>(config.padding_samples));
    if (padding_samples < 0) {
      throw Napi::RangeError::New(
          options.Env(), "paddingSamples must be an integer in [0, " + node_int_max_text() + "]");
    }
    config.padding_samples = static_cast<size_t>(padding_samples);
  }
  config.mode = parse_trim_silence_mode(options, config.mode);
  config.gate_lufs = DomainFloatProperty(options, "gateLufs", config.gate_lufs);
  config.window_ms = DomainFloatProperty(options, "windowMs", config.window_ms);
  sonare::mastering::repair::validate_config(config);
  return config;
}

/// @brief The C config the C-ABI entries take, for a request @ref ReadTrimConfig has read.
SonareTrimSilenceConfig ToCTrimConfig(const sonare::mastering::repair::TrimSilenceConfig& config,
                                      float threshold_db) {
  SonareTrimSilenceConfig out{};
  sonare_trim_silence_config_init(&out);
  out.threshold_db = threshold_db;
  out.padding_samples = config.padding_samples;
  out.mode = config.mode == sonare::mastering::repair::TrimSilenceMode::LufsGated
                 ? SONARE_TRIM_SILENCE_MODE_LUFS_GATED
                 : SONARE_TRIM_SILENCE_MODE_PEAK;
  out.gate_lufs = config.gate_lufs;
  out.window_ms = config.window_ms;
  return out;
}

}  // namespace

Napi::Value SonareWrap::MasteringRepairTrimSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input("samples", typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::TrimSilenceConfig config;
  float threshold_db = 0.0f;
  if (info.Length() >= 3 && info[2].IsObject()) {
    config = ReadTrimConfig(info[2].As<Napi::Object>(), &threshold_db);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::TrimReport report;
  sonare::Audio result = sonare::mastering::repair::trim_silence(audio, config, &report);
  return EmitMonoResult(env, result, EmitTrimReport(env, report));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairTrimSilenceStereo(const Napi::CallbackInfo& info) {
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
  float threshold_db = sonare::mastering::repair::kTrimThresholdDefaultDb;
  sonare::mastering::repair::TrimSilenceConfig core_config;
  if (info.Length() >= 4 && info[3].IsObject()) {
    core_config = ReadTrimConfig(info[3].As<Napi::Object>(), &threshold_db);
  }
  const SonareTrimSilenceConfig config = ToCTrimConfig(core_config, threshold_db);
  SonareTrimSilenceStereoResult result{};
  SonareError err = sonare_mastering_repair_trim_silence_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  StereoResultGuard<SonareTrimSilenceStereoResult> guard(&result);
  // result.length is the OUTPUT length, not the input's: trimming shortens the
  // pair, and an all-silent pair comes back at 0.
  Napi::Object out = EmitStereoResult(env, result);
  out.Set("report", EmitTrimReport(env, result.report));
  out.Set("leftRange", EmitTrimRange(env, result.left_range));
  out.Set("rightRange", EmitTrimRange(env, result.right_range));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectTrimRange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  float threshold_db = sonare::mastering::repair::kTrimThresholdDefaultDb;
  sonare::mastering::repair::TrimSilenceConfig core_config;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) core_config = ReadTrimConfig(info[2].As<Napi::Object>(), &threshold_db);
  const SonareTrimSilenceConfig config = ToCTrimConfig(core_config, threshold_db);
  SonareTrimRange range{};
  SonareError err = sonare_mastering_repair_detect_trim_range(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &range);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitTrimRange(env, range);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectTrimRangeStereo(const Napi::CallbackInfo& info) {
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
  float threshold_db = sonare::mastering::repair::kTrimThresholdDefaultDb;
  sonare::mastering::repair::TrimSilenceConfig core_config;
  const bool has_options = info.Length() >= 4 && info[3].IsObject();
  if (has_options) core_config = ReadTrimConfig(info[3].As<Napi::Object>(), &threshold_db);
  const SonareTrimSilenceConfig config = ToCTrimConfig(core_config, threshold_db);
  SonareTrimRange range{};
  SonareError err = sonare_mastering_repair_detect_trim_range_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, has_options ? &config : nullptr, &range);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitTrimRange(env, range);
  SONARE_NODE_CATCH(env)
}
