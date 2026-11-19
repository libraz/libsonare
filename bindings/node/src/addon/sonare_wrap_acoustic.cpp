#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "acoustic/selector_names.h"
#include "analysis/room_estimator.h"
#include "core/audio.h"
#include "effects/acoustic/room_morph.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/zero_is_default.h"

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

// The per-material-band cap and the finite-[0, 1] coefficient rejection live in
// the core's make_uniform_room, which RoomFromOptions below calls, so a crafted
// bandAbsorption/bandScattering array cannot drive an unbounded per-wall
// allocation and an out-of-range coefficient raises the same error it does on
// the C ABI. The C ABI is otherwise bypassed here (synthesize_rir is called
// directly), which is why the guard has to be reachable from the core rather
// than sitting in the C-ABI translation unit.

// Validates the RIR timing config against the same bounds the C ABI checks
// before building a room, so Node rejects (rather than silently accepts) the
// out-of-range inputs the C ABI/Python already refuse.
void ValidateRirShapeAndTiming(const sonare::acoustic::RirSynthConfig& cfg) {
  using namespace sonare::acoustic;
  // Finiteness is FiniteFloatProperty's at the read; what is left is each field's own bound.
  if (cfg.max_seconds < 0.0f || cfg.max_seconds > kMaxRirSeconds || cfg.mixing_time_ms < 0.0f ||
      cfg.mixing_time_ms > kMaxRirMixingTimeMs || cfg.crossfade_ms < 0.0f ||
      cfg.crossfade_ms > kMaxRirCrossfadeMs) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "RIR timing parameters out of range");
  }
}

// Refuses a buffer under the shared offline-input policy, as the C ABI and WASM entries do.
bool ValidateAcousticInput(const Napi::Env& env, const char* name, const float* data, size_t length,
                           int sr) {
  const std::string problem = sonare::offline_audio_input_problem(nullptr, name, data, length, sr);
  if (!problem.empty()) {
    Napi::RangeError::New(env, problem).ThrowAsJavaScriptException();
    return false;
  }
  return true;
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
      out.push_back(node_narrow_finite_float_element(opts.Env(), arr.Get(i), key, i));
    }
    return out;
  }
  return {};
}

// The room and synthesis keys synthesizeRir, roomMorph and roomMorphStereo share; each entry point
// adds its own on top.
const std::vector<const char*>& RoomKeys() {
  static const std::vector<const char*> keys = {"lengthM",
                                                "widthM",
                                                "heightM",
                                                "absorption",
                                                "bandAbsorption",
                                                "bandScattering",
                                                "materialPreset",
                                                "source",
                                                "listener",
                                                "ismOrder",
                                                "seed",
                                                "maxSec",
                                                "preferEyring",
                                                "mixingTimeMs",
                                                "crossfadeMs",
                                                "airAbsorptionEnabled",
                                                "airTemperatureC",
                                                "airHumidityPercent"};
  return keys;
}

// Builds a uniform shoebox + placement from a JS options object, honouring the
// same wall-material precedence as the C ABI: materialPreset (non-zero) >
// per-band bandAbsorption > scalar absorption.
sonare::acoustic::ShoeboxRoom RoomFromOptions(const Napi::Object& opts, float def_absorption) {
  using namespace sonare::acoustic;
  // A non-finite dimension is refused by the reader, which names the key the
  // caller wrote rather than the three together.
  const sonare::RoomDimensions dims{FiniteFloatProperty(opts, "lengthM", 7.0f),
                                    FiniteFloatProperty(opts, "widthM", 5.0f),
                                    FiniteFloatProperty(opts, "heightM", 3.0f)};

  // The precedence, the [0, 1] coefficient rejection and the band-wise
  // scattering all come from the core builder the C ABI calls, so the same
  // option bag builds the same room on every surface. Reading the options is
  // the only part that is Node's.
  WallMaterialRequest request;
  request.has_preset = sonare::acoustic::material_preset_from_selector(
      NamedSelectorProperty(opts, "materialPreset", sonare::acoustic::kMaterialPresetSelectorNames,
                            0),
      &request.preset);
  request.absorption_bands = NodeFloatArrayOption(opts, "bandAbsorption");
  request.scattering_bands = NodeFloatArrayOption(opts, "bandScattering");
  request.absorption = FloatProperty(opts, "absorption", def_absorption);
  return make_uniform_room(dims, request);
}

sonare::acoustic::SourceListener PlacementFromOptions(const Napi::Object& opts) {
  const auto position = [&](const char* key, const sonare::acoustic::Vec3& fallback) {
    const auto xyz = XyzProperty(opts, key, {fallback.x, fallback.y, fallback.z});
    return sonare::acoustic::Vec3{xyz[0], xyz[1], xyz[2]};
  };
  return {position("source", sonare::acoustic::kDefaultPlacement.source),
          position("listener", sonare::acoustic::kDefaultPlacement.listener)};
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
      return "info";
  }
  return "info";
}

// Transcribes the synthesizer's diagnostic list (the clamp / no-tail warnings of a successful
// call) onto the result object.
Napi::Array DiagnosticsArray(Napi::Env env, const std::vector<sonare::Diagnostic>& diagnostics) {
  Napi::Array entries = Napi::Array::New(env, diagnostics.size());
  for (size_t index = 0; index < diagnostics.size(); ++index) {
    const sonare::Diagnostic& diagnostic = diagnostics[index];
    Napi::Object entry = Napi::Object::New(env);
    entry.Set("code", Napi::String::New(env, diagnostic.code));
    entry.Set("message", Napi::String::New(env, diagnostic.message));
    entry.Set("severity", Napi::String::New(env, SeverityName(diagnostic.severity)));
    entries.Set(static_cast<uint32_t>(index), entry);
  }
  return entries;
}

}  // namespace

Napi::Value SonareWrap::SynthesizeRir(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  Napi::Object opts = info[0].As<Napi::Object>();
  RefuseUnknownKeys(opts, "options", RoomKeys(), {"sampleRate"});
  const int sample_rate = IntProperty(opts, "sampleRate", 48000);
  if (!ValidateAcousticSampleRate(env, sample_rate)) return env.Undefined();
  sonare::acoustic::RirSynthConfig cfg;
  // Match the C ABI: reject a negative ISM order instead of clamping it to 0, so
  // the same invalid input fails identically on every surface.
  cfg.ism_order = IntProperty(opts, "ismOrder", cfg.ism_order);
  if (cfg.ism_order < 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "ismOrder must be an integer in [0, " + node_int_max_text() + "]");
  }
  cfg.late_model = BoolProperty(opts, "preferEyring", true) ? sonare::acoustic::ReverbModel::Eyring
                                                            : sonare::acoustic::ReverbModel::Sabine;
  cfg.seed = Uint32Property(opts, "seed", cfg.seed);
  cfg.max_seconds = FiniteFloatProperty(opts, "maxSec", cfg.max_seconds);
  cfg.mixing_time_ms = FiniteFloatProperty(opts, "mixingTimeMs", cfg.mixing_time_ms);
  cfg.crossfade_ms = FiniteFloatProperty(opts, "crossfadeMs", cfg.crossfade_ms);
  cfg.air_absorption_enabled =
      BoolProperty(opts, "airAbsorptionEnabled", cfg.air_absorption_enabled);
  // An implausible climate is an Error diagnostic, thrown below like the geometry errors.
  cfg.air.temperature_c = FloatProperty(opts, "airTemperatureC", cfg.air.temperature_c);
  cfg.air.humidity_percent = FloatProperty(opts, "airHumidityPercent", cfg.air.humidity_percent);

  const auto placement = PlacementFromOptions(opts);
  ValidateRirShapeAndTiming(cfg);
  const auto result = sonare::acoustic::synthesize_rir(
      RoomFromOptions(opts, sonare::acoustic::kDefaultWallAbsorption), placement, sample_rate, cfg);
  if (sonare::has_error(result.diagnostics)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  sonare::acoustic::first_error_text(result.diagnostics));
  }
  std::vector<float> rir = AudioToVector(result.rir);

  Napi::Object out = Napi::Object::New(env);
  out.Set("rir", VecToFloat32(env, rir));
  out.Set("sampleRate", Napi::Number::New(env, result.rir.sample_rate()));
  out.Set("diagnostics", DiagnosticsArray(env, result.diagnostics));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::EstimateRoom(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  if (!RequireNumberValue(env, info[1], "sampleRate")) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate = node_narrow_int(env, info[1], "sampleRate");
  if (!ValidateAcousticSampleRate(env, sample_rate)) return env.Undefined();
  if (!ValidateAcousticInput(env, "samples", typed.Data(), typed.ElementLength(), sample_rate))
    return env.Undefined();
  const sonare::Audio audio =
      sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sample_rate);
  Napi::Object opts = info.Length() >= 3 && info[2].IsObject() ? info[2].As<Napi::Object>()
                                                               : Napi::Object::New(env);
  // samples and sampleRate ride along from the request object and are not read here.
  RefuseUnknownKeys(
      opts, "options",
      {"aspectHintLw", "aspectHintLh", "referenceAbsorption", "preferEyring", "nOctaveBands",
       "minDecayDb", "noiseFloorMarginDb", "mode", "samples", "sampleRate"});

  sonare::RoomEstimateConfig cfg;
  // Match the C ABI: an explicit 0 aspect hint means "use the default 1.0", so
  // the same input is accepted identically on every surface (raw 0 would be
  // rejected by the core's finite-positive check).
  cfg.aspect_hint_lw = sonare::ZeroIsDefault(FloatProperty(opts, "aspectHintLw", 0.0f))
                           .or_default(cfg.aspect_hint_lw);
  cfg.aspect_hint_lh = sonare::ZeroIsDefault(FloatProperty(opts, "aspectHintLh", 0.0f))
                           .or_default(cfg.aspect_hint_lh);
  cfg.reference_absorption = sonare::ZeroIsDefault(FloatProperty(opts, "referenceAbsorption", 0.0f))
                                 .or_default(cfg.reference_absorption);
  cfg.prefer_eyring = BoolProperty(opts, "preferEyring", true);
  const int n_bands = IntProperty(opts, "nOctaveBands", kZeroIsSentinel);
  if (n_bands != 0) cfg.acoustic.n_octave_bands = n_bands;
  cfg.acoustic.min_decay_db = sonare::ZeroIsDefault(FloatProperty(opts, "minDecayDb", 0.0f))
                                  .or_default(cfg.acoustic.min_decay_db);
  cfg.acoustic.noise_floor_margin_db =
      sonare::ZeroIsDefault(FloatProperty(opts, "noiseFloorMarginDb", 0.0f))
          .or_default(cfg.acoustic.noise_floor_margin_db);
  switch (NamedSelectorProperty(opts, "mode", sonare::acoustic::kAcousticModeSelectorNames, 0)) {
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
  out.Set("lengthM", Napi::Number::New(env, est.dims.length));
  out.Set("widthM", Napi::Number::New(env, est.dims.width));
  out.Set("heightM", Napi::Number::New(env, est.dims.height));
  out.Set("drrDb", Napi::Number::New(env, est.drr_db));
  out.Set("confidence", Napi::Number::New(env, est.confidence));
  out.Set("bandAbsorption", VecToFloat32(env, absorption_bands));
  out.Set("rt60Bands", VecToFloat32(env, rt60_bands));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RoomGeometryFromEstimate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "estimate must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  const Napi::Object estimate = info[0].As<Napi::Object>();
  std::vector<float> bands = FloatArrayProperty(estimate, "bandAbsorption");
  SonareRoomEstimate c_estimate{};
  c_estimate.length_m = FloatProperty(estimate, "lengthM", c_estimate.length_m);
  c_estimate.width_m = FloatProperty(estimate, "widthM", c_estimate.width_m);
  c_estimate.height_m = FloatProperty(estimate, "heightM", c_estimate.height_m);
  c_estimate.absorption_bands = bands.empty() ? nullptr : bands.data();
  c_estimate.band_count = bands.size();

  SonareRirSynthConfig geometry{};
  const SonareError err = sonare_room_geometry_from_estimate(&c_estimate, &geometry);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("lengthM", Napi::Number::New(env, geometry.length_m));
  out.Set("widthM", Napi::Number::New(env, geometry.width_m));
  out.Set("heightM", Napi::Number::New(env, geometry.height_m));
  Napi::Object source = Napi::Object::New(env);
  source.Set("x", Napi::Number::New(env, geometry.source_x));
  source.Set("y", Napi::Number::New(env, geometry.source_y));
  source.Set("z", Napi::Number::New(env, geometry.source_z));
  out.Set("source", source);
  Napi::Object listener = Napi::Object::New(env);
  listener.Set("x", Napi::Number::New(env, geometry.listener_x));
  listener.Set("y", Napi::Number::New(env, geometry.listener_y));
  listener.Set("z", Napi::Number::New(env, geometry.listener_z));
  out.Set("listener", listener);
  if (std::any_of(bands.begin(), bands.end(), [](float b) { return std::isfinite(b); })) {
    out.Set("absorption", Napi::Number::New(env, geometry.absorption));
  }
  if (geometry.absorption_band_count > 0) {
    out.Set("bandAbsorption",
            VecToFloat32(env, std::vector<float>(
                                  geometry.absorption_bands,
                                  geometry.absorption_bands + geometry.absorption_band_count)));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

// Options shared by roomMorph and roomMorphStereo.
sonare::effects::acoustic::RoomMorphConfig RoomMorphConfigFromOptions(const Napi::Object& opts) {
  sonare::effects::acoustic::RoomMorphConfig cfg;
  cfg.target = RoomFromOptions(opts, sonare::acoustic::kDefaultWallAbsorption);
  cfg.placement = PlacementFromOptions(opts);
  cfg.source_tail_suppression =
      FiniteFloatProperty(opts, "sourceTailSuppression", cfg.source_tail_suppression);
  cfg.wet = FiniteFloatProperty(opts, "wet", cfg.wet);
  cfg.ism_order = IntProperty(opts, "ismOrder", cfg.ism_order);
  cfg.seed = Uint32Property(opts, "seed", cfg.seed);
  cfg.max_seconds = FiniteFloatProperty(opts, "maxSec", cfg.max_seconds);
  cfg.late_model = BoolProperty(opts, "preferEyring", true) ? sonare::acoustic::ReverbModel::Eyring
                                                            : sonare::acoustic::ReverbModel::Sabine;
  cfg.mixing_time_ms = FiniteFloatProperty(opts, "mixingTimeMs", cfg.mixing_time_ms);
  // The morph core validates crossfade and climate, throwing on an implausible value.
  cfg.crossfade_ms = FiniteFloatProperty(opts, "crossfadeMs", cfg.crossfade_ms);
  cfg.air_absorption_enabled =
      BoolProperty(opts, "airAbsorptionEnabled", cfg.air_absorption_enabled);
  cfg.air.temperature_c = FloatProperty(opts, "airTemperatureC", cfg.air.temperature_c);
  cfg.air.humidity_percent = FloatProperty(opts, "airHumidityPercent", cfg.air.humidity_percent);
  return cfg;
}

Napi::Value SonareWrap::RoomMorph(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  if (!RequireNumberValue(env, info[1], "sampleRate")) return env.Undefined();
  if (!info[2].IsObject()) {
    Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  if (!ValidateAcousticSampleRate(env, sr)) return env.Undefined();
  if (!ValidateAcousticInput(env, "samples", typed.Data(), typed.ElementLength(), sr))
    return env.Undefined();
  const sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  Napi::Object opts = info[2].As<Napi::Object>();
  RefuseUnknownKeys(opts, "options", RoomKeys(),
                    {"wet", "sourceTailSuppression", "samples", "sampleRate"});

  const sonare::effects::acoustic::RoomMorphConfig cfg = RoomMorphConfigFromOptions(opts);

  const auto result = sonare::effects::acoustic::room_morph(audio, cfg);
  std::vector<float> morphed = AudioToVector(result.audio);

  // Shaped like SynthesizeRir's result rather than a bare buffer: the same
  // synthesis runs underneath and its warnings describe a room the caller did
  // not ask for. An unusable request throws, so every entry here is a warning.
  Napi::Object out = Napi::Object::New(env);
  out.Set("audio", VecToFloat32(env, morphed));
  out.Set("sampleRate", Napi::Number::New(env, result.audio.sample_rate()));
  out.Set("diagnostics", DiagnosticsArray(env, result.diagnostics));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RoomMorphStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "left must be a Float32Array")) return env.Undefined();
  if (!RequireFloat32Array(info, 1, "right must be a Float32Array")) return env.Undefined();
  if (!RequireNumberValue(env, info[2], "sampleRate")) return env.Undefined();
  if (!info[3].IsObject()) {
    Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  if (!ValidateAcousticSampleRate(env, sr)) return env.Undefined();
  if (!ValidateAcousticInput(env, "left", left.Data(), left.ElementLength(), sr))
    return env.Undefined();
  if (!ValidateAcousticInput(env, "right", right.Data(), right.ElementLength(), sr))
    return env.Undefined();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const sonare::Audio left_audio =
      sonare::Audio::from_buffer(left.Data(), left.ElementLength(), sr);
  const sonare::Audio right_audio =
      sonare::Audio::from_buffer(right.Data(), right.ElementLength(), sr);
  Napi::Object opts = info[3].As<Napi::Object>();
  RefuseUnknownKeys(
      opts, "options", RoomKeys(),
      {"wet", "sourceTailSuppression", "receiverSpacingM", "left", "right", "sampleRate"});

  sonare::effects::acoustic::RoomMorphConfig cfg = RoomMorphConfigFromOptions(opts);
  cfg.receiver_spacing_m = FiniteFloatProperty(opts, "receiverSpacingM", cfg.receiver_spacing_m);

  const auto result = sonare::effects::acoustic::room_morph_stereo(left_audio, right_audio, cfg);

  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, AudioToVector(result.left)));
  out.Set("right", VecToFloat32(env, AudioToVector(result.right)));
  out.Set("sampleRate", Napi::Number::New(env, result.left.sample_rate()));
  out.Set("diagnostics", DiagnosticsArray(env, result.diagnostics));
  return out;
  SONARE_NODE_CATCH(env)
}
