#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "acoustic/selector_names.h"
#include "analysis/room_estimator.h"
#include "core/audio.h"
#include "effects/acoustic/room_morph.h"
#include "effects/common/room_field_checks.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"

using namespace sonare_node;

namespace {

// Throws (via ThrowAsJavaScriptException) and returns false when the rate is out
// of range, mirroring the C ABI's validate_audio_params bound.
bool ValidateAcousticSampleRate(const Napi::Env& env, int sample_rate) {
  if (sample_rate < sonare::kMinAudioSampleRate || sample_rate > sonare::kMaxAudioSampleRate) {
    Napi::RangeError::New(
        env, "sampleRate " + sonare::util::refusal::integer_range_predicate(
                                 sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate))
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

// The core's geometry and synthesis rules, run ahead of the synthesis so a refusal names the
// key the JS caller wrote.
void RequireRirRequest(const sonare::acoustic::ShoeboxRoom& room,
                       const sonare::acoustic::SourceListener& placement,
                       const sonare::acoustic::RirSynthConfig& cfg) {
  using namespace sonare::acoustic;
  std::vector<sonare::Diagnostic> diagnostics =
      validate_shoebox(room, placement, kShoeboxJsFieldNames);
  const std::vector<sonare::Diagnostic> synthesis =
      validate_rir_synth_config(cfg, kRirSynthJsFieldNames);
  diagnostics.insert(diagnostics.end(), synthesis.begin(), synthesis.end());
  if (sonare::has_error(diagnostics)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  first_error_text(diagnostics));
  }
}

// Refuses a buffer under the shared offline-input policy, as the C ABI and WASM entries do.
bool ValidateAcousticInput(const Napi::Env& env, const char* name, const float* data, size_t length,
                           int sr) {
  const std::string problem = sonare::offline_audio_input_problem(name, data, length, sr);
  if (!problem.empty()) {
    Napi::RangeError::New(env, problem).ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

// Builds a uniform shoebox + placement from a JS options object, honouring the
// same wall-material precedence as the C ABI: materialPreset (non-zero) >
// per-band bandAbsorption > scalar absorption.
sonare::acoustic::ShoeboxRoom RoomFromOptions(const Napi::Object& opts, float def_absorption) {
  using namespace sonare::acoustic;
  // A non-finite dimension is refused by the reader, which names the key the
  // caller wrote rather than the three together.
  const sonare::RoomDimensions dims{FloatProperty(opts, "lengthM", 7.0f),
                                    FloatProperty(opts, "widthM", 5.0f),
                                    FloatProperty(opts, "heightM", 3.0f)};

  // The precedence, the [0, 1] coefficient rejection and the band-wise
  // scattering all come from the core builder the C ABI calls, so the same
  // option bag builds the same room on every surface. Reading the options is
  // the only part that is Node's.
  WallMaterialRequest request;
  request.has_preset = sonare::acoustic::material_preset_from_selector(
      NamedSelectorProperty(opts, "materialPreset", sonare::acoustic::kMaterialPresetSelectorNames,
                            0),
      &request.preset);
  request.absorption_bands = FloatVectorProperty(opts, "bandAbsorption");
  request.scattering_bands = FloatVectorProperty(opts, "bandScattering");
  request.absorption = FloatProperty(opts, "absorption", def_absorption);
  return make_uniform_room(dims, request);
}

// The source and listener asked for, each absent when omitted (undefined or null).
sonare::acoustic::RoomPlacementRequest PlacementRequestFromOptions(const Napi::Object& opts) {
  const auto requested = [&](const char* key) -> std::array<std::optional<float>, 3> {
    const Napi::Value value = opts.Get(key);
    if (value.IsUndefined() || value.IsNull()) return {};
    const auto xyz = XyzProperty(opts, key, {});
    return {xyz[0], xyz[1], xyz[2]};
  };
  sonare::acoustic::RoomPlacementRequest request;
  request.source = requested("source");
  request.listener = requested("listener");
  return request;
}

// The source and listener asked for, an omitted one taking the default placement scaled to @p room.
sonare::acoustic::SourceListener PlacementFromOptions(const Napi::Object& opts,
                                                      const sonare::acoustic::ShoeboxRoom& room) {
  return sonare::acoustic::resolve_room_placement(room.dims, PlacementRequestFromOptions(opts))
      .placement;
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
  const int sample_rate =
      DomainProperty<int>(opts, "sampleRate", 48000, sonare::kAudioSampleRateDomain);
  sonare::acoustic::RirSynthConfig cfg;
  cfg.ism_order = DomainProperty<int>(opts, "ismOrder", cfg.ism_order,
                                      sonare::effects::common::kRirIsmOrderDomain);
  cfg.late_model = BoolProperty(opts, "preferEyring", true) ? sonare::acoustic::ReverbModel::Eyring
                                                            : sonare::acoustic::ReverbModel::Sabine;
  cfg.seed = Uint32Property(opts, "seed", cfg.seed);
  cfg.max_seconds = FloatProperty(opts, "maxSec", cfg.max_seconds);
  cfg.mixing_time_ms = FloatProperty(opts, "mixingTimeMs", cfg.mixing_time_ms);
  cfg.crossfade_ms = FloatProperty(opts, "crossfadeMs", cfg.crossfade_ms);
  cfg.air_absorption_enabled =
      BoolProperty(opts, "airAbsorptionEnabled", cfg.air_absorption_enabled);
  // An implausible climate is an Error diagnostic, thrown below like the geometry errors.
  cfg.air.temperature_c = FloatProperty(opts, "airTemperatureC", cfg.air.temperature_c);
  cfg.air.humidity_percent = FloatProperty(opts, "airHumidityPercent", cfg.air.humidity_percent);

  const auto room = RoomFromOptions(opts, sonare::acoustic::kDefaultWallAbsorption);
  const auto placement = PlacementFromOptions(opts, room);
  RequireRirRequest(room, placement, cfg);
  const auto result = sonare::acoustic::synthesize_rir(room, placement, sample_rate, cfg);
  if (sonare::has_error(result.diagnostics)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  sonare::acoustic::first_error_text(result.diagnostics));
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("rir", Float32ArrayFrom(env, result.rir.data(), result.rir.size()));
  out.Set("sampleRate", Napi::Number::New(env, result.rir.sample_rate()));
  out.Set("diagnostics", DiagnosticsArray(env, result.diagnostics));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::EstimateRoom(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  int sample_rate{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sample_rate)) return env.Undefined();
  auto typed = info[0].As<Napi::Float32Array>();
  if (!ValidateAcousticSampleRate(env, sample_rate)) return env.Undefined();
  if (!ValidateAcousticInput(env, "samples", sonare_node::ElementData(typed), typed.ElementLength(),
                             sample_rate))
    return env.Undefined();
  const sonare::Audio audio = sonare::Audio::from_buffer(sonare_node::ElementData(typed),
                                                         typed.ElementLength(), sample_rate);
  Napi::Object opts = info.Length() >= 3 && info[2].IsObject() ? info[2].As<Napi::Object>()
                                                               : Napi::Object::New(env);

  // As the C ABI reads its struct: an omitted key keeps the library default and a given
  // value is read literally, so a 0 outside a field's domain is refused by name.
  sonare::RoomEstimateConfig cfg;
  cfg.aspect_hint_lw = FloatProperty(opts, "aspectHintLw", cfg.aspect_hint_lw);
  cfg.aspect_hint_lh = FloatProperty(opts, "aspectHintLh", cfg.aspect_hint_lh);
  cfg.reference_absorption = FloatProperty(opts, "referenceAbsorption", cfg.reference_absorption);
  cfg.prefer_eyring = BoolProperty(opts, "preferEyring", cfg.prefer_eyring);
  cfg.acoustic.n_octave_bands = IntProperty(opts, "nOctaveBands", cfg.acoustic.n_octave_bands);
  cfg.acoustic.min_decay_db = FloatProperty(opts, "minDecayDb", cfg.acoustic.min_decay_db);
  cfg.acoustic.noise_floor_margin_db =
      FloatProperty(opts, "noiseFloorMarginDb", cfg.acoustic.noise_floor_margin_db);
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

  // The core refuses these too, under member names; checked first to name the JS field.
  {
    namespace check = sonare::util::check;
    check::greater_than(cfg.aspect_hint_lw, 0.0, "aspectHintLw");
    check::greater_than(cfg.aspect_hint_lh, 0.0, "aspectHintLh");
    check::in_range(cfg.reference_absorption, sonare::kRoomReferenceAbsorptionMin,
                    sonare::kRoomReferenceAbsorptionMax, "referenceAbsorption");
    check::greater_than(cfg.acoustic.min_decay_db, 0.0, "minDecayDb");
    check::at_least(cfg.acoustic.noise_floor_margin_db, 0.0, "noiseFloorMarginDb");
    check::integer_at_least(cfg.acoustic.n_octave_bands, 1, "nOctaveBands");
  }
  const sonare::RoomEstimate est = sonare::estimate_room(audio, cfg);
  // Absorption (from the inverse problem) and RT60 (from the decay fit) are independent estimates
  // and either can fail on its own: both are padded with NaN to the longer one's length, a failed
  // side to all NaN, so the bands line up as the C ABI hands them.
  const size_t band_count = std::max(est.absorption_bands.size(), est.rt60_bands.size());
  const auto pad_with_nan = [band_count](std::vector<float> values) {
    values.resize(band_count, std::numeric_limits<float>::quiet_NaN());
    return values;
  };
  const std::vector<float> absorption_bands = pad_with_nan(est.absorption_bands);
  const std::vector<float> rt60_bands = pad_with_nan(est.rt60_bands);
  Napi::Object out = Napi::Object::New(env);
  SetMeasured(out, "volume", est.volume);
  SetMeasured(out, "lengthM", est.dims.length);
  SetMeasured(out, "widthM", est.dims.width);
  SetMeasured(out, "heightM", est.dims.height);
  SetMeasured(out, "drrDb", est.drr_db);
  out.Set("confidence", Napi::Number::New(env, est.confidence));
  // nOctaveBands is at least 1, so no band at all means both estimates failed.
  if (band_count > 0) {
    SetMeasuredArray(out, "bandAbsorption", absorption_bands.data(), band_count);
    SetMeasuredArray(out, "rt60Bands", rt60_bands.data(), band_count);
  }
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
  // RoomEstimateResult omits a dimension it could not measure; the C ABI spells that NaN.
  constexpr float kNotMeasured = std::numeric_limits<float>::quiet_NaN();
  c_estimate.length_m = FloatDataProperty(estimate, "lengthM", kNotMeasured);
  c_estimate.width_m = FloatDataProperty(estimate, "widthM", kNotMeasured);
  c_estimate.height_m = FloatDataProperty(estimate, "heightM", kNotMeasured);
  c_estimate.band_absorption = bands.empty() ? nullptr : bands.data();
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
  source.Set("x", Napi::Number::New(env, geometry.source.x));
  source.Set("y", Napi::Number::New(env, geometry.source.y));
  source.Set("z", Napi::Number::New(env, geometry.source.z));
  out.Set("source", source);
  Napi::Object listener = Napi::Object::New(env);
  listener.Set("x", Napi::Number::New(env, geometry.listener.x));
  listener.Set("y", Napi::Number::New(env, geometry.listener.y));
  listener.Set("z", Napi::Number::New(env, geometry.listener.z));
  out.Set("listener", listener);
  if (std::any_of(bands.begin(), bands.end(), [](float b) { return std::isfinite(b); })) {
    out.Set("absorption", Napi::Number::New(env, geometry.absorption));
  }
  if (geometry.band_absorption_count > 0) {
    out.Set("bandAbsorption",
            Float32ArrayFrom(env, std::vector<float>(
                                      geometry.band_absorption,
                                      geometry.band_absorption + geometry.band_absorption_count)));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

// Options shared by roomMorph and roomMorphStereo. The stereo entry reads `receiverSpacingM` and
// passes it in; the mono entry has none, and an absent stereo spacing is fitted to the room.
sonare::effects::acoustic::RoomMorphConfig RoomMorphConfigFromOptions(
    const Napi::Object& opts, std::optional<float> receiver_spacing_m) {
  sonare::effects::acoustic::RoomMorphConfig cfg;
  cfg.target = RoomFromOptions(opts, sonare::acoustic::kDefaultWallAbsorption);
  sonare::acoustic::RoomPlacementRequest request = PlacementRequestFromOptions(opts);
  request.receiver_spacing_m = receiver_spacing_m;
  const sonare::acoustic::ResolvedRoomPlacement resolved =
      sonare::acoustic::resolve_room_placement(cfg.target.dims, request);
  cfg.placement = resolved.placement;
  cfg.receiver_spacing_m = receiver_spacing_m;
  cfg.supplied = resolved.supplied;
  cfg.source_tail_suppression =
      DomainFloatProperty(opts, "sourceTailSuppression", cfg.source_tail_suppression);
  cfg.wet = DomainFloatProperty(opts, "wet", cfg.wet);
  cfg.ism_order = IntProperty(opts, "ismOrder", cfg.ism_order);
  cfg.seed = Uint32Property(opts, "seed", cfg.seed);
  cfg.max_seconds = DomainFloatProperty(opts, "maxSec", cfg.max_seconds);
  cfg.late_model = BoolProperty(opts, "preferEyring", true) ? sonare::acoustic::ReverbModel::Eyring
                                                            : sonare::acoustic::ReverbModel::Sabine;
  cfg.mixing_time_ms = DomainFloatProperty(opts, "mixingTimeMs", cfg.mixing_time_ms);
  // The morph core validates crossfade and climate, throwing on an implausible value.
  cfg.crossfade_ms = DomainFloatProperty(opts, "crossfadeMs", cfg.crossfade_ms);
  cfg.air_absorption_enabled =
      BoolProperty(opts, "airAbsorptionEnabled", cfg.air_absorption_enabled);
  cfg.air.temperature_c = DomainFloatProperty(opts, "airTemperatureC", cfg.air.temperature_c);
  cfg.air.humidity_percent =
      DomainFloatProperty(opts, "airHumidityPercent", cfg.air.humidity_percent);
  return cfg;
}

Napi::Value SonareWrap::RoomMorph(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  if (!info[2].IsObject()) {
    Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  if (!ValidateAcousticSampleRate(env, sr)) return env.Undefined();
  if (!ValidateAcousticInput(env, "samples", sonare_node::ElementData(typed), typed.ElementLength(),
                             sr))
    return env.Undefined();
  const sonare::Audio audio =
      sonare::Audio::from_buffer(sonare_node::ElementData(typed), typed.ElementLength(), sr);
  Napi::Object opts = info[2].As<Napi::Object>();

  const sonare::effects::acoustic::RoomMorphConfig cfg =
      RoomMorphConfigFromOptions(opts, std::nullopt);

  const auto result = sonare::util::check::with_camel_paths(
      [&] { return sonare::effects::acoustic::room_morph(audio, cfg); },
      sonare::effects::acoustic::kRoomMorphRenamedKeys);

  // Shaped like SynthesizeRir's result rather than a bare buffer: the same
  // synthesis runs underneath and its warnings describe a room the caller did
  // not ask for. An unusable request throws, so every entry here is a warning.
  Napi::Object out = Napi::Object::New(env);
  out.Set("audio", Float32ArrayFrom(env, result.audio.data(), result.audio.size()));
  out.Set("sampleRate", Napi::Number::New(env, result.audio.sample_rate()));
  out.Set("diagnostics", DiagnosticsArray(env, result.diagnostics));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RoomMorphStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "left must be a Float32Array")) return env.Undefined();
  if (!RequireFloat32Array(info, 1, "right must be a Float32Array")) return env.Undefined();
  int sr{};
  if (!RequiredIntArg(env, info, 2, "sampleRate", &sr)) return env.Undefined();
  if (!info[3].IsObject()) {
    Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (!ValidateAcousticSampleRate(env, sr)) return env.Undefined();
  if (!ValidateAcousticInput(env, "left", sonare_node::ElementData(left), left.ElementLength(), sr))
    return env.Undefined();
  if (!ValidateAcousticInput(env, "right", sonare_node::ElementData(right), right.ElementLength(),
                             sr))
    return env.Undefined();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const sonare::Audio left_audio =
      sonare::Audio::from_buffer(sonare_node::ElementData(left), left.ElementLength(), sr);
  const sonare::Audio right_audio =
      sonare::Audio::from_buffer(sonare_node::ElementData(right), right.ElementLength(), sr);
  Napi::Object opts = info[3].As<Napi::Object>();

  const Napi::Value spacing = opts.Get("receiverSpacingM");
  const std::optional<float> receiver_spacing_m =
      spacing.IsUndefined() || spacing.IsNull()
          ? std::nullopt
          : std::optional<float>(DomainFloatProperty(opts, "receiverSpacingM", 0.0f));
  const sonare::effects::acoustic::RoomMorphConfig cfg =
      RoomMorphConfigFromOptions(opts, receiver_spacing_m);

  const auto result = sonare::util::check::with_camel_paths(
      [&] { return sonare::effects::acoustic::room_morph_stereo(left_audio, right_audio, cfg); },
      sonare::effects::acoustic::kRoomMorphRenamedKeys);

  Napi::Object out = Napi::Object::New(env);
  out.Set("left", Float32ArrayFrom(env, result.left.data(), result.left.size()));
  out.Set("right", Float32ArrayFrom(env, result.right.data(), result.right.size()));
  out.Set("sampleRate", Napi::Number::New(env, result.left.sample_rate()));
  out.Set("diagnostics", DiagnosticsArray(env, result.diagnostics));
  return out;
  SONARE_NODE_CATCH(env)
}
