#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

/// @brief Map a lowercase window string to the SonareWindowType integer.
/// Returns -1 on an unrecognised name (caller should throw).
int parse_window_type(const std::string& s) {
  if (s == "hann") return SONARE_WINDOW_HANN;
  if (s == "hamming") return SONARE_WINDOW_HAMMING;
  if (s == "blackman") return SONARE_WINDOW_BLACKMAN;
  if (s == "rectangular" || s == "rect") return SONARE_WINDOW_RECTANGULAR;
  return -1;
}

/// @brief Resolve a window given as a name or as a @ref SonareWindowType ordinal.
/// @details Both spellings reach the same rejection, matching the C ABI and the
///   WASM reader. Validating only the name leaves the numeric form -- the one a
///   generated binding produces -- as a type error, and the ordinal is bounded
///   by the enumerators rather than by a literal, so a member added later
///   widens the accepted set instead of silently changing what 4 means.
/// @throws SonareException(InvalidParameter) for a value that is neither, or an
///   ordinal outside the enum.
int read_window_type(Napi::Env env, const Napi::Value& value) {
  if (value.IsNumber()) {
    // An ordinal is a member, not a magnitude, so the fraction is refused here
    // rather than truncated: the positional readers let 31.5 reach a callee as
    // 31 because that does not change what was asked for, but 1.5 selecting
    // hamming is a window the caller never named. A domain check, not a
    // narrowing one -- node_narrow_int has already accepted the value.
    if (sonare_node::node_is_fraction(value)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "spectralEdit: window ordinal must be an integer, not " +
                                        std::to_string(value.As<Napi::Number>().DoubleValue()));
    }
    const int ordinal = sonare_node::node_narrow_int(env, value, "window");
    if (ordinal < SONARE_WINDOW_HANN || ordinal > SONARE_WINDOW_RECTANGULAR) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "spectralEdit: unknown window type: " + std::to_string(ordinal));
    }
    return ordinal;
  }
  if (!value.IsString()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "spectralEdit: window must be a window name or a window ordinal");
  }
  std::string name = value.As<Napi::String>().Utf8Value();
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const int mapped = parse_window_type(name);
  if (mapped < 0) {
    throw std::runtime_error("spectralEdit: unknown window type: " +
                             value.As<Napi::String>().Utf8Value());
  }
  return mapped;
}

/// @brief Map a lowercase spectral-edit mode string to SonareSpectralEditMode.
/// Returns -1 on an unrecognised name (caller should throw).
int parse_spectral_edit_mode(const std::string& s) {
  if (s == "gain") return SONARE_SPECTRAL_EDIT_MODE_GAIN;
  if (s == "attenuate") return SONARE_SPECTRAL_EDIT_MODE_ATTENUATE;
  if (s == "mute") return SONARE_SPECTRAL_EDIT_MODE_MUTE;
  if (s == "heal") return SONARE_SPECTRAL_EDIT_MODE_HEAL;
  return -1;
}

}  // namespace

Napi::Value SonareWrap::SpectralEdit(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples: Float32Array, sampleRate: number, ops: object[], options?: object)
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsArray()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, ops: object[], options?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sr");

  // Build config (optional fourth argument).
  SonareSpectralEditConfig config{};  // zero-init = all defaults
  const SonareSpectralEditConfig* config_ptr = nullptr;
  if (info.Length() >= 4 && info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    config.n_fft = IntProperty(opts, "nFft", kZeroIsSentinel);
    config.hop_length = IntProperty(opts, "hopLength", kZeroIsSentinel);
    config.heal_radius_frames = IntProperty(opts, "healRadiusFrames", kZeroIsSentinel);

    // Optional window, by name or by ordinal.
    Napi::Value win_val = opts.Get("window");
    if (!win_val.IsUndefined() && !win_val.IsNull()) {
      config.window = read_window_type(env, win_val);
    }
    config_ptr = &config;
  }

  // Build ops array from the JS array of plain objects.
  auto js_ops = info[2].As<Napi::Array>();
  const uint32_t n_ops = js_ops.Length();
  std::vector<SonareSpectralRegionOp> ops(n_ops);
  for (uint32_t i = 0; i < n_ops; ++i) {
    Napi::Value item = js_ops.Get(i);
    if (!item.IsObject()) {
      throw std::runtime_error("spectralEdit: each op must be a plain object");
    }
    Napi::Object op = item.As<Napi::Object>();
    // Every field in this bag now refuses a wrong-typed value by name, matching
    // nFft/hopLength/healRadiusFrames in the config object above. The WASM
    // binding reads every one of them through int64Property/floatProperty,
    // which coerce rather than refuse -- an accepted, tracked divergence
    // (reader-family-scope.test.ts), not something this reader can close.
    ops[i].start_sample = Int64Property(op, "startSample", 0);
    ops[i].end_sample = Int64Property(op, "endSample", static_cast<int64_t>(length));
    ops[i].low_hz = FloatProperty(op, "lowHz", 0.0f);
    ops[i].high_hz = FloatProperty(op, "highHz", 0.0f);
    ops[i].gain_db = FloatProperty(op, "gainDb", 0.0f);

    // Resolve mode: required string field.
    Napi::Value mode_val = op.Get("mode");
    if (mode_val.IsUndefined() || mode_val.IsNull()) {
      ops[i].mode = SONARE_SPECTRAL_EDIT_MODE_GAIN;
    } else {
      if (!mode_val.IsString()) {
        throw std::runtime_error("spectralEdit: op.mode must be a string");
      }
      std::string mode_str = mode_val.As<Napi::String>().Utf8Value();
      std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      int mode_int = parse_spectral_edit_mode(mode_str);
      if (mode_int < 0) {
        throw std::runtime_error("spectralEdit: unknown mode: " +
                                 mode_val.As<Napi::String>().Utf8Value());
      }
      ops[i].mode = mode_int;
    }
  }

  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_spectral_edit(
      data, length, sr, config_ptr, n_ops > 0 ? ops.data() : nullptr, n_ops, &out, &out_length);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  auto result = Napi::Float32Array::New(env, out_length);
  if (out_length > 0 && out != nullptr) {
    std::memcpy(result.Data(), out, out_length * sizeof(float));
    sonare_free_floats(out);
  }
  return result;
  SONARE_NODE_CATCH(env)
}
