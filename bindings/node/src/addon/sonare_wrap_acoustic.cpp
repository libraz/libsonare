#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "analysis/room_estimator.h"
#include "core/audio.h"
#include "effects/acoustic/room_morph.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

// Acoustic sample-rate bounds, kept in sync with the C ABI's
// sonare_c_detail::kMinSampleRate / kMaxSampleRate so every binding rejects the
// same out-of-range rates (the C++ functions are otherwise called directly).
constexpr int kAcousticMinSampleRate = 8000;
constexpr int kAcousticMaxSampleRate = 384000;

// Throws (via ThrowAsJavaScriptException) and returns false when the rate is out
// of range, mirroring the C ABI's validate_audio_params bound.
bool ValidateAcousticSampleRate(const Napi::Env& env, int sample_rate) {
  if (sample_rate < kAcousticMinSampleRate || sample_rate > kAcousticMaxSampleRate) {
    Napi::RangeError::New(env, "sampleRate out of supported range [8000, 384000]")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

// Mirrors the C ABI's per-material-band cap (sonare_c_acoustic.cpp) so a crafted
// bandAbsorption/bandScattering array cannot drive an unbounded per-wall
// allocation. The C ABI is otherwise bypassed here (synthesize_rir is called
// directly), so this binding must re-apply the same guard.
constexpr size_t kMaxMaterialBands = 64;

// Finite [0, 1] test matching the C ABI's `unit` predicate for absorption /
// scattering coefficients. Out-of-range values are rejected (not silently
// clamped) so the same mistake surfaces the same error on every surface.
bool IsUnitCoefficient(float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; }

// Validates the RIR shape/timing config against the same bounds the C ABI checks
// before building a room, so Node rejects (rather than silently accepts) the
// NaN/out-of-range inputs the C ABI/Python already refuse.
void ValidateRirShapeAndTiming(const sonare::acoustic::SourceListener& placement,
                               const sonare::acoustic::RirSynthConfig& cfg) {
  using namespace sonare::acoustic;
  const auto finite3 = [](const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  };
  if (!finite3(placement.source) || !finite3(placement.listener)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "source/listener position must be finite");
  }
  if (!std::isfinite(cfg.max_seconds) || cfg.max_seconds < 0.0f ||
      cfg.max_seconds > kMaxRirSeconds || !std::isfinite(cfg.mixing_time_ms) ||
      cfg.mixing_time_ms < 0.0f || cfg.mixing_time_ms > kMaxRirMixingTimeMs ||
      !std::isfinite(cfg.crossfade_ms) || cfg.crossfade_ms < 0.0f ||
      cfg.crossfade_ms > kMaxRirCrossfadeMs) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "RIR timing parameters out of range");
  }
}

// Rejects an empty input buffer and any non-finite sample, matching the C ABI's
// validate_audio_params contract for the estimate/morph entry points.
bool ValidateAcousticInput(const Napi::Env& env, const float* data, size_t length) {
  if (data == nullptr || length == 0) {
    Napi::RangeError::New(env, "input buffer is empty").ThrowAsJavaScriptException();
    return false;
  }
  // Match the C ABI's validate_offline_audio_input upper bound so an oversized
  // buffer is rejected instead of driving an unbounded allocation (OOM).
  if (length > sonare::kMaxAudioBufferSize) {
    Napi::RangeError::New(env, "input buffer exceeds the maximum offline length")
        .ThrowAsJavaScriptException();
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!std::isfinite(data[i])) {
      Napi::RangeError::New(env, "input contains NaN or Inf samples").ThrowAsJavaScriptException();
      return false;
    }
  }
  return true;
}

// Maps a materialPreset selector (mirroring SONARE_MATERIAL_PRESET_*: 1 concrete,
// 2 wood, 3 curtain, 4 carpet, 5 glass) onto a MaterialPreset. Returns false for
// 0/none or any unknown value, in which case the per-band/scalar path applies.
bool MaterialPresetFromInt(int selector, sonare::acoustic::MaterialPreset* out) {
  using sonare::acoustic::MaterialPreset;
  switch (selector) {
    case 1:
      *out = MaterialPreset::Concrete;
      return true;
    case 2:
      *out = MaterialPreset::Wood;
      return true;
    case 3:
      *out = MaterialPreset::Curtain;
      return true;
    case 4:
      *out = MaterialPreset::Carpet;
      return true;
    case 5:
      *out = MaterialPreset::Glass;
      return true;
    default:
      return false;
  }
}

// Reads an optional Float32Array/number[] option into a vector (empty if absent).
std::vector<float> NodeFloatArrayOption(const Napi::Object& opts, const char* key) {
  Napi::Value value = opts.Get(key);
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array) {
    auto typed = value.As<Napi::Float32Array>();
    return std::vector<float>(typed.Data(), typed.Data() + typed.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    std::vector<float> out;
    out.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value v = arr.Get(i);
      out.push_back(v.IsNumber() ? v.As<Napi::Number>().FloatValue() : 0.0f);
    }
    return out;
  }
  return {};
}

// Builds a uniform shoebox + placement from a JS options object, honouring the
// same wall-material precedence as the C ABI: materialPreset (non-zero) >
// per-band bandAbsorption > scalar absorption.
sonare::acoustic::ShoeboxRoom RoomFromOptions(const Napi::Object& opts, float def_absorption) {
  using namespace sonare::acoustic;
  const sonare::RoomDimensions dims{node_float_option(opts, "lengthM", 7.0f),
                                    node_float_option(opts, "widthM", 5.0f),
                                    node_float_option(opts, "heightM", 3.0f)};
  if (!std::isfinite(dims.length) || !std::isfinite(dims.width) || !std::isfinite(dims.height)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "room dimensions must be finite");
  }

  MaterialPreset preset{};
  if (MaterialPresetFromInt(node_int_option(opts, "materialPreset", 0), &preset)) {
    ShoeboxRoom room;
    room.dims = dims;
    const Material wall = make_material(preset);
    for (Material& w : room.walls) w = wall;
    return room;
  }

  const std::vector<float> bands = NodeFloatArrayOption(opts, "bandAbsorption");
  if (!bands.empty()) {
    const std::vector<float> scattering_bands = NodeFloatArrayOption(opts, "bandScattering");
    if (bands.size() > kMaxMaterialBands || scattering_bands.size() > kMaxMaterialBands) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "material band count exceeds the maximum of 64");
    }
    // Reject any non-finite or out-of-[0, 1] per-band coefficient, matching the C
    // ABI's `unit` predicate so the same invalid band table fails identically on
    // every surface (rather than being silently clamped only here).
    for (float a : bands) {
      if (!IsUnitCoefficient(a)) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "bandAbsorption values must be within [0, 1]");
      }
    }
    for (float s : scattering_bands) {
      if (!IsUnitCoefficient(s)) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "bandScattering values must be within [0, 1]");
      }
    }
    ShoeboxRoom room;
    room.dims = dims;
    Material wall;
    wall.absorption.reserve(bands.size());
    // Clamp the accepted in-range per-band absorption to [0, 0.999], matching the
    // C-ABI oracle's make_room clamp so the same band table yields the same
    // reflection energy on every surface (a raw 1.0 gives beta=0 here but 0.0316
    // in the C ABI, diverging the RIR early reflections).
    for (float a : bands) {
      wall.absorption.push_back(std::clamp(a, 0.0f, 0.999f));
    }
    wall.scattering.reserve(bands.size());
    for (size_t i = 0; i < bands.size(); ++i) {
      const float scattering = i < scattering_bands.size() ? scattering_bands[i] : 0.0f;
      wall.scattering.push_back(scattering);
    }
    for (Material& w : room.walls) w = wall;
    return room;
  }

  const float scalar = node_float_option(opts, "absorption", def_absorption);
  if (!IsUnitCoefficient(scalar)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "absorption must be within [0, 1]");
  }
  return uniform_shoebox(dims, scalar);
}

sonare::acoustic::SourceListener PlacementFromOptions(const Napi::Object& opts) {
  return {{node_float_option(opts, "sourceX", 1.0f), node_float_option(opts, "sourceY", 1.0f),
           node_float_option(opts, "sourceZ", 1.2f)},
          {node_float_option(opts, "listenerX", 5.0f), node_float_option(opts, "listenerY", 4.0f),
           node_float_option(opts, "listenerZ", 1.7f)}};
}

std::vector<float> AudioToVector(const sonare::Audio& audio) {
  if (audio.empty()) return {};
  return std::vector<float>(audio.data(), audio.data() + audio.size());
}

// Wire string for a diagnostic severity, matching the RirDiagnostic type.
const char* SeverityName(sonare::Diagnostic::Severity severity) {
  switch (severity) {
    case sonare::Diagnostic::Severity::Error:
      return "error";
    case sonare::Diagnostic::Severity::Warning:
      return "warning";
    case sonare::Diagnostic::Severity::Info:
      break;
  }
  return "info";
}

// Transcribes the synthesizer's whole diagnostic list onto the result object.
// The synthesizer reports five distinct geometry errors plus the clamp / no-tail
// warnings, so a lone hasError boolean cannot tell a caller which one fired, nor
// that a maxSeconds clamp shortened the tail of an otherwise successful RIR.
// `errorMessage` carries the first error as "code: message", matching the string
// the C ABI leaves in sonare_last_error_message() and Python's RirResult.
void SetRirDiagnostics(Napi::Env env, Napi::Object out,
                       const std::vector<sonare::Diagnostic>& diagnostics) {
  Napi::Array entries = Napi::Array::New(env, diagnostics.size());
  std::string error_message;
  for (size_t index = 0; index < diagnostics.size(); ++index) {
    const sonare::Diagnostic& diagnostic = diagnostics[index];
    Napi::Object entry = Napi::Object::New(env);
    entry.Set("code", Napi::String::New(env, diagnostic.code));
    entry.Set("message", Napi::String::New(env, diagnostic.message));
    entry.Set("severity", Napi::String::New(env, SeverityName(diagnostic.severity)));
    entries.Set(static_cast<uint32_t>(index), entry);
    if (error_message.empty() && diagnostic.severity == sonare::Diagnostic::Severity::Error) {
      error_message = diagnostic.code + ": " + diagnostic.message;
    }
  }
  out.Set("diagnostics", entries);
  out.Set("errorMessage", Napi::String::New(env, error_message));
}

}  // namespace

Napi::Value SonareWrap::SynthesizeRir(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected (options) object").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  Napi::Object opts = info[0].As<Napi::Object>();
  const int sample_rate = node_int_option(opts, "sampleRate", 48000);
  if (!ValidateAcousticSampleRate(env, sample_rate)) return env.Undefined();
  sonare::acoustic::RirSynthConfig cfg;
  // Match the C ABI: reject a negative ISM order instead of clamping it to 0, so
  // the same invalid input fails identically on every surface.
  cfg.ism_order = node_int_option(opts, "ismOrder", cfg.ism_order);
  if (cfg.ism_order < 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "ismOrder must be non-negative");
  }
  cfg.late_model = node_bool_option(opts, "preferEyring", true)
                       ? sonare::acoustic::ReverbModel::Eyring
                       : sonare::acoustic::ReverbModel::Sabine;
  // seed <= 0 keeps the RirSynthConfig default (1), matching the C ABI's
  // "seed == 0 keeps the library default" so seed:0 yields the same RIR on every
  // surface instead of seeding the PRNG with 0.
  if (const int seed_in = node_int_option(opts, "seed", 0); seed_in > 0)
    cfg.seed = static_cast<unsigned>(seed_in);
  cfg.max_seconds = node_float_option(opts, "maxSeconds", cfg.max_seconds);
  cfg.mixing_time_ms = node_float_option(opts, "mixingTimeMs", cfg.mixing_time_ms);
  // crossfadeMs == 0 keeps the RirSynthConfig default (5 ms), matching the C ABI's
  // "crossfade_ms == 0 means keep the library default"; a literal zero crossfade
  // shifts the splice by ~1 sample and clicks, so only a positive override applies.
  if (const float crossfade_ms = node_float_option(opts, "crossfadeMs", 0.0f); crossfade_ms > 0.0f)
    cfg.crossfade_ms = crossfade_ms;
  cfg.air_absorption_enabled =
      node_bool_option(opts, "airAbsorptionEnabled", cfg.air_absorption_enabled);
  // airTemperatureC / airHumidityPercent == 0 keep the ISO reference climate
  // (20 degC, 50 % RH), matching the C ABI's "0 means the library default" rule
  // so the same options object yields the same RIR on every surface. An
  // implausible climate is reported through the diagnostics/hasError channel by
  // the core, the way the geometry errors already are.
  if (const float air_temperature_c = node_float_option(opts, "airTemperatureC", 0.0f);
      air_temperature_c != 0.0f)
    cfg.air.temperature_c = air_temperature_c;
  if (const float air_humidity_percent = node_float_option(opts, "airHumidityPercent", 0.0f);
      air_humidity_percent != 0.0f)
    cfg.air.humidity_percent = air_humidity_percent;

  const auto placement = PlacementFromOptions(opts);
  ValidateRirShapeAndTiming(placement, cfg);
  const auto result =
      sonare::acoustic::synthesize_rir(RoomFromOptions(opts, 0.2f), placement, sample_rate, cfg);
  std::vector<float> rir = AudioToVector(result.rir);

  Napi::Object out = Napi::Object::New(env);
  out.Set("rir", VecToFloat32(env, rir));
  out.Set("sampleRate", Napi::Number::New(env, result.rir.sample_rate()));
  out.Set("hasError", Napi::Boolean::New(env, sonare::has_error(result.diagnostics)));
  SetRirDiagnostics(env, out, result.diagnostics);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::EstimateRoom(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate = info[1].As<Napi::Number>().Int32Value();
  if (!ValidateAcousticSampleRate(env, sample_rate)) return env.Undefined();
  if (!ValidateAcousticInput(env, typed.Data(), typed.ElementLength())) return env.Undefined();
  const sonare::Audio audio =
      sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sample_rate);
  Napi::Object opts = info.Length() >= 3 && info[2].IsObject() ? info[2].As<Napi::Object>()
                                                               : Napi::Object::New(env);

  sonare::RoomEstimateConfig cfg;
  // Match the C ABI: an explicit 0 aspect hint means "use the default 1.0", so
  // the same input is accepted identically on every surface (raw 0 would be
  // rejected by the core's finite-positive check).
  cfg.aspect_hint_lw = node_float_option(opts, "aspectHintLw", cfg.aspect_hint_lw);
  if (cfg.aspect_hint_lw == 0.0f) cfg.aspect_hint_lw = 1.0f;
  cfg.aspect_hint_lh = node_float_option(opts, "aspectHintLh", cfg.aspect_hint_lh);
  if (cfg.aspect_hint_lh == 0.0f) cfg.aspect_hint_lh = 1.0f;
  cfg.reference_absorption =
      node_float_option(opts, "referenceAbsorption", cfg.reference_absorption);
  cfg.prefer_eyring = node_bool_option(opts, "preferEyring", true);
  const int n_bands = node_int_option(opts, "nOctaveBands", 0);
  if (n_bands != 0) cfg.acoustic.n_octave_bands = n_bands;
  const float min_decay_db = node_float_option(opts, "minDecayDb", 0.0f);
  if (min_decay_db != 0.0f) cfg.acoustic.min_decay_db = min_decay_db;
  const float noise_floor_margin_db = node_float_option(opts, "noiseFloorMarginDb", 0.0f);
  if (noise_floor_margin_db != 0.0f) cfg.acoustic.noise_floor_margin_db = noise_floor_margin_db;
  switch (node_int_option(opts, "mode", 0)) {
    case 1:
      cfg.acoustic.mode = sonare::AcousticConfig::Mode::Blind;
      break;
    case 2:
      cfg.acoustic.mode = sonare::AcousticConfig::Mode::ImpulseResponse;
      break;
    default:
      cfg.acoustic.mode = sonare::AcousticConfig::Mode::Auto;
      break;
  }

  const sonare::RoomEstimate est = sonare::estimate_room(audio, cfg);
  // Absorption (from the inverse problem) and RT60 (from the decay fit) are
  // independent estimates and either can fail on its own. Both arrays report at
  // the longer length with the failed side NaN-filled, exactly as the C ABI does
  // (sonare_c_acoustic.cpp): truncating to the shorter side discarded a
  // fully-computed vector precisely when its sibling failed, which is when the
  // caller needs the surviving one most.
  const size_t band_count = std::max(est.absorption_bands.size(), est.rt60_bands.size());
  const auto pad_with_nan = [band_count](std::vector<float> values) {
    values.resize(band_count, std::numeric_limits<float>::quiet_NaN());
    return values;
  };
  const std::vector<float> absorption_bands = pad_with_nan(est.absorption_bands);
  const std::vector<float> rt60_bands = pad_with_nan(est.rt60_bands);
  Napi::Object out = Napi::Object::New(env);
  out.Set("volume", Napi::Number::New(env, est.volume));
  out.Set("length", Napi::Number::New(env, est.dims.length));
  out.Set("width", Napi::Number::New(env, est.dims.width));
  out.Set("height", Napi::Number::New(env, est.dims.height));
  out.Set("drrDb", Napi::Number::New(env, est.drr_db));
  out.Set("confidence", Napi::Number::New(env, est.confidence));
  out.Set("absorptionBands", VecToFloat32(env, absorption_bands));
  out.Set("rt60Bands", VecToFloat32(env, rt60_bands));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RoomMorph(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsObject()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  if (!ValidateAcousticSampleRate(env, sr)) return env.Undefined();
  if (!ValidateAcousticInput(env, typed.Data(), typed.ElementLength())) return env.Undefined();
  const sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  Napi::Object opts = info[2].As<Napi::Object>();

  sonare::effects::acoustic::RoomMorphConfig cfg;
  cfg.target = RoomFromOptions(opts, 0.2f);
  cfg.placement = PlacementFromOptions(opts);
  cfg.source_tail_suppression =
      node_float_option(opts, "sourceTailSuppression", cfg.source_tail_suppression);
  cfg.wet = node_float_option(opts, "wet", cfg.wet);
  cfg.ism_order = node_int_option(opts, "ismOrder", cfg.ism_order);
  // seed <= 0 keeps the RirSynthConfig default (1), matching the C ABI's
  // "seed == 0 keeps the library default" so seed:0 yields the same RIR on every
  // surface instead of seeding the PRNG with 0.
  if (const int seed_in = node_int_option(opts, "seed", 0); seed_in > 0)
    cfg.seed = static_cast<unsigned>(seed_in);
  cfg.max_seconds = node_float_option(opts, "maxSeconds", cfg.max_seconds);
  cfg.late_model = node_bool_option(opts, "preferEyring", true)
                       ? sonare::acoustic::ReverbModel::Eyring
                       : sonare::acoustic::ReverbModel::Sabine;
  cfg.mixing_time_ms = node_float_option(opts, "mixingTimeMs", cfg.mixing_time_ms);
  // crossfadeMs == 0 keeps the RoomMorphConfig default (5 ms), matching the C ABI's
  // "crossfade_ms == 0 means keep the library default"; a literal zero crossfade
  // shifts the splice by ~1 sample and clicks, so only a positive override applies.
  if (const float crossfade_ms = node_float_option(opts, "crossfadeMs", 0.0f); crossfade_ms != 0.0f)
    cfg.crossfade_ms = crossfade_ms;
  // Air absorption on the target room; the zero-means-ISO-reference rule is the
  // same as synthesizeRir above. An implausible climate throws here (the morph
  // core validates rather than diagnosing), matching the C ABI.
  cfg.air_absorption_enabled =
      node_bool_option(opts, "airAbsorptionEnabled", cfg.air_absorption_enabled);
  if (const float air_temperature_c = node_float_option(opts, "airTemperatureC", 0.0f);
      air_temperature_c != 0.0f)
    cfg.air.temperature_c = air_temperature_c;
  if (const float air_humidity_percent = node_float_option(opts, "airHumidityPercent", 0.0f);
      air_humidity_percent != 0.0f)
    cfg.air.humidity_percent = air_humidity_percent;

  const sonare::Audio result = sonare::effects::acoustic::room_morph(audio, cfg);
  std::vector<float> out = AudioToVector(result);
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}
