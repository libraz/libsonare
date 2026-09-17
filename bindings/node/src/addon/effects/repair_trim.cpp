/// @file
/// @brief Node bindings for the silence-trim repair.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <stdexcept>
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
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("trim silence mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "peak") return sonare::mastering::repair::TrimSilenceMode::Peak;
  if (s == "lufsgated" || s == "lufs_gated" || s == "lufs") {
    return sonare::mastering::repair::TrimSilenceMode::LufsGated;
  }
  throw std::runtime_error("unknown trim silence mode: " + value.As<Napi::String>().Utf8Value());
}

// The library default (sonare_c_mastering.h), the base an options bag is read over.
constexpr SonareTrimSilenceConfig kTrimSilenceConfigDefaults{
    0.001f, 0, SONARE_TRIM_SILENCE_MODE_PEAK, -60.0f, 400.0f};

Napi::Object EmitTrimRange(Napi::Env env, const SonareTrimRange& range) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("first", Napi::Number::New(env, static_cast<double>(range.first)));
  out.Set("lastExclusive", Napi::Number::New(env, static_cast<double>(range.last_exclusive)));
  return out;
}

Napi::Object EmitTrimReport(Napi::Env env, const SonareTrimReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("range", EmitTrimRange(env, report.range));
  out.Set("removedHeadSamples",
          Napi::Number::New(env, static_cast<double>(report.removed_head_samples)));
  out.Set("removedTailSamples",
          Napi::Number::New(env, static_cast<double>(report.removed_tail_samples)));
  return out;
}

/// @brief Read a SonareTrimSilenceConfig options bag, applying the same field
///        names as the mono entry above reads onto the C++ config.
/// @details `paddingSamples` goes through NonNegativeSizeTProperty rather than
///   an int reader: the field is a size_t, so -1 arrives as SIZE_MAX and lands
///   above the core's SIZE_MAX/2 bound, where it reads as an out-of-range
///   padding rather than as the negative the caller wrote. Refusing it by name
///   here reports what was actually wrong, and the false return is why this
///   reports success rather than returning the config.
/// @return false with a JS exception pending; the caller must bail.
bool ReadTrimSilenceConfig(Napi::Env env, const Napi::Object& options,
                           SonareTrimSilenceConfig* config) {
  config->threshold = FloatProperty(options, "threshold", config->threshold);
  if (!NonNegativeSizeTProperty(env, options, "paddingSamples", config->padding_samples,
                                &config->padding_samples)) {
    return false;
  }
  const auto mode =
      parse_trim_silence_mode(options, config->mode == SONARE_TRIM_SILENCE_MODE_LUFS_GATED
                                           ? sonare::mastering::repair::TrimSilenceMode::LufsGated
                                           : sonare::mastering::repair::TrimSilenceMode::Peak);
  config->mode = mode == sonare::mastering::repair::TrimSilenceMode::LufsGated
                     ? SONARE_TRIM_SILENCE_MODE_LUFS_GATED
                     : SONARE_TRIM_SILENCE_MODE_PEAK;
  config->gate_lufs = FloatProperty(options, "gateLufs", config->gate_lufs);
  config->window_ms = FloatProperty(options, "windowMs", config->window_ms);
  return true;
}

/// @brief Frees both heap-owned channels of a SonareTrimSilenceStereoResult on
///        scope exit -- mirrors DereverbStereoResultGuard above.
/// @details A pass that kept nothing hands back two NULLs rather than two
///   zero-length allocations, which no other repair stereo entry can produce;
///   sonare_free_floats accepts NULL, so that case needs no branch here.
class TrimSilenceStereoResultGuard {
 public:
  explicit TrimSilenceStereoResultGuard(SonareTrimSilenceStereoResult* result) : result_(result) {}
  TrimSilenceStereoResultGuard(const TrimSilenceStereoResultGuard&) = delete;
  TrimSilenceStereoResultGuard& operator=(const TrimSilenceStereoResultGuard&) = delete;
  ~TrimSilenceStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareTrimSilenceStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairTrimSilence(const Napi::CallbackInfo& info) {
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
  sonare::mastering::repair::TrimSilenceConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = FloatProperty(options, "threshold", config.threshold);
    if (options.Has("paddingSamples")) {
      const int padding_samples =
          node_int_option(options, "paddingSamples", static_cast<int>(config.padding_samples));
      if (padding_samples < 0) {
        Napi::RangeError::New(env, "paddingSamples must be non-negative")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      config.padding_samples = static_cast<size_t>(padding_samples);
    }
    config.mode = parse_trim_silence_mode(options, config.mode);
    config.gate_lufs = node_float_option(options, "gateLufs", config.gate_lufs);
    config.window_ms = node_float_option(options, "windowMs", config.window_ms);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::trim_silence(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairTrimSilenceStereo(const Napi::CallbackInfo& info) {
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
    Napi::Error::New(env,
                     "masteringRepairTrimSilenceStereo: left and right must have the same "
                     "length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareTrimSilenceConfig), applied
  // before any options key overrides a field.
  SonareTrimSilenceConfig config{0.001f, 0, SONARE_TRIM_SILENCE_MODE_PEAK, -60.0f, 400.0f};
  if (info.Length() >= 4 && info[3].IsObject()) {
    if (!ReadTrimSilenceConfig(env, info[3].As<Napi::Object>(), &config)) return env.Undefined();
  }
  SonareTrimSilenceStereoResult result{};
  SonareError err = sonare_mastering_repair_trim_silence_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  TrimSilenceStereoResultGuard guard(&result);
  // result.length is the OUTPUT length, not the input's: trimming shortens the
  // pair, and an all-silent pair comes back at 0.
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("report", EmitTrimReport(env, result.report));
  out.Set("leftRange", EmitTrimRange(env, result.left_range));
  out.Set("rightRange", EmitTrimRange(env, result.right_range));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectTrimRange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareTrimSilenceConfig config = kTrimSilenceConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options && !ReadTrimSilenceConfig(env, info[2].As<Napi::Object>(), &config)) {
    return env.Undefined();
  }
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
        env, "masteringRepairDetectTrimRangeStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  SonareTrimSilenceConfig config = kTrimSilenceConfigDefaults;
  const bool has_options = info.Length() >= 4 && info[3].IsObject();
  if (has_options && !ReadTrimSilenceConfig(env, info[3].As<Napi::Object>(), &config)) {
    return env.Undefined();
  }
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
