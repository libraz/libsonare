/// @file room_acoustics.cpp
/// @brief Embind bindings for room-acoustics analysis and simulation APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

#include "acoustic/estimate_geometry.h"
#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/selector_names.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"
#include "wasm/bindings/common/common.h"

val acousticParametersToVal(const AcousticParameters& params) {
  val out = val::object();
  setMeasured(out, "rt60", params.rt60);
  setMeasured(out, "edt", params.edt);
  setMeasured(out, "c50", params.c50);
  setMeasured(out, "c80", params.c80);
  setMeasured(out, "d50", params.d50);
  const std::size_t band_count = params.rt60_bands.size();
  setMeasuredArray(out, "rt60Bands", params.rt60_bands, band_count);
  setMeasuredArray(out, "edtBands", params.edt_bands, band_count);
  setMeasuredArray(out, "c50Bands", params.c50_bands, band_count);
  setMeasuredArray(out, "c80Bands", params.c80_bands, band_count);
  out.set("confidence", params.confidence);
  out.set("isBlind", params.is_blind);
  return out;
}

val js_analyze_impulse_response(val samples, const val& sample_rate_val,
                                const val& n_octave_bands_val, const val& min_decay_db_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_octave_bands =
      domainNumberFromVal<int>(n_octave_bands_val, "nOctaveBands", kAcousticBandCountDomain);
  AcousticConfig config;
  config.n_octave_bands = n_octave_bands;
  config.min_decay_db =
      optionalDomainValue<float>(min_decay_db_val, "minDecayDb", kAcousticMinDecayDbDomain)
          .value_or(30.0f);
  validate_acoustic_config(config);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  return acousticParametersToVal(analyze_impulse_response(audio, config));
}

val js_detect_acoustic(val samples, const val& sample_rate_val, const val& n_octave_bands_val,
                       const val& n_third_octave_subbands_val, const val& min_decay_db_val,
                       const val& noise_floor_margin_db_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_octave_bands =
      domainNumberFromVal<int>(n_octave_bands_val, "nOctaveBands", kAcousticBandCountDomain);
  const int n_third_octave_subbands = domainNumberFromVal<int>(
      n_third_octave_subbands_val, "nThirdOctaveSubbands", kAcousticBandCountDomain);
  const float min_decay_db =
      domainNumberFromVal<float>(min_decay_db_val, "minDecayDb", kAcousticMinDecayDbDomain);
  const float noise_floor_margin_db = domainNumberFromVal<float>(
      noise_floor_margin_db_val, "noiseFloorMarginDb", kAcousticNoiseFloorMarginDbDomain);
  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_octave_bands = n_octave_bands;
  config.n_third_octave_subbands = n_third_octave_subbands;
  config.min_decay_db = min_decay_db;
  config.noise_floor_margin_db = noise_floor_margin_db;
  validate_acoustic_config(config);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  return acousticParametersToVal(detect_acoustic(audio, config));
}

#ifdef SONARE_WITH_ACOUSTIC_SIM
// The per-material-band cap and the finite-[0, 1] coefficient rejection live in
// the core's make_uniform_room, which roomFromVal below calls, so a crafted
// bandAbsorption/bandScattering array cannot drive an unbounded per-wall
// allocation and an out-of-range coefficient raises the same error it does on
// the C ABI. The C ABI is bypassed here (synthesize_rir is called directly),
// which is why the guard has to be reachable from the core rather than sitting
// in the C-ABI translation unit.

// Builds a uniform shoebox from a JS options object, honouring the same wall-
// material precedence as the C ABI: materialPreset (non-zero) > per-band
// bandAbsorption (Float32Array/number[]) > scalar absorption.
sonare::acoustic::ShoeboxRoom roomFromVal(val opts, float def_absorption) {
  using namespace sonare::acoustic;
  const RoomDimensions dims{floatProperty(opts, "lengthM", 7.0f),
                            floatProperty(opts, "widthM", 5.0f),
                            floatProperty(opts, "heightM", 3.0f)};

  // The precedence, the [0, 1] coefficient rejection and the band-wise
  // scattering all come from the core builder the C ABI calls, so the same
  // option bag builds the same room on every surface. Reading the val is the
  // only part that is WASM's.
  WallMaterialRequest request;
  const int material_preset = intProperty(opts, "materialPreset", 0);
  requireOrdinalInRange(material_preset, 0,
                        static_cast<int>(kMaterialPresetSelectorNames.size()) - 1,
                        "materialPreset");
  request.has_preset = material_preset_from_selector(material_preset, &request.preset);
  if (hasProperty(opts, "bandAbsorption")) {
    request.absorption_bands = float32ArrayToVector(opts["bandAbsorption"], "bandAbsorption");
  }
  if (hasProperty(opts, "bandScattering")) {
    request.scattering_bands = float32ArrayToVector(opts["bandScattering"], "bandScattering");
  }
  request.absorption = floatProperty(opts, "absorption", def_absorption);
  return make_uniform_room(dims, request);
}

// Reads `{ x, y, z }` under @p key: absent (undefined or null) is not given, anything else
// must be an object whose three components are finite numbers, each refused by name.
std::array<std::optional<float>, 3> requestedPositionFromVal(val opts, const char* key) {
  val position = objectProperty(opts, key);
  if (position.isUndefined() || position.isNull()) return {};
  if (position.typeOf().as<std::string>() != "object" || position.isArray()) {
    throw WasmTypeError(std::string(key) + " must be an object with x, y and z");
  }
  const std::string name = key;
  const auto component = [&](const char* axis) {
    const std::string label = name + "." + axis;
    return checkedFloatFromVal(position[axis], label.c_str());
  };
  return {component("x"), component("y"), component("z")};
}

// The source and listener asked for, each absent when omitted.
sonare::acoustic::RoomPlacementRequest placementRequestFromVal(val opts) {
  sonare::acoustic::RoomPlacementRequest request;
  request.source = requestedPositionFromVal(opts, "source");
  request.listener = requestedPositionFromVal(opts, "listener");
  return request;
}

// The source and listener asked for, an omitted one taking the default placement scaled to @p room.
sonare::acoustic::SourceListener placementFromVal(val opts,
                                                  const sonare::acoustic::ShoeboxRoom& room) {
  return sonare::acoustic::resolve_room_placement(room.dims, placementRequestFromVal(opts))
      .placement;
}

// Acoustic sample-rate bounds, sourced from the shared core limits
// (sonare::kMinAudioSampleRate / kMaxAudioSampleRate) so every binding rejects
// the same out-of-range rates (the C++ functions are otherwise called directly).
void validateAcousticSampleRate(int sample_rate) {
  if (sample_rate < sonare::kMinAudioSampleRate || sample_rate > sonare::kMaxAudioSampleRate) {
    throw WasmRangeError("sampleRate " +
                         sonare::util::refusal::integer_range_predicate(
                             sonare::kMinAudioSampleRate, sonare::kMaxAudioSampleRate));
  }
}

// The core's geometry and synthesis rules, run ahead of the synthesis so a refusal names the
// key the JS caller wrote.
void requireRirRequest(const sonare::acoustic::ShoeboxRoom& room,
                       const sonare::acoustic::SourceListener& placement,
                       const sonare::acoustic::RirSynthConfig& config) {
  using namespace sonare::acoustic;
  std::vector<sonare::Diagnostic> diagnostics =
      validate_shoebox(room, placement, kShoeboxJsFieldNames);
  const std::vector<sonare::Diagnostic> synthesis =
      validate_rir_synth_config(config, kRirSynthJsFieldNames);
  diagnostics.insert(diagnostics.end(), synthesis.begin(), synthesis.end());
  if (sonare::has_error(diagnostics)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  first_error_text(diagnostics));
  }
}

Audio acousticAudioFromVector(const char* name, std::vector<float> data, int sample_rate) {
  const std::string problem =
      offline_audio_input_problem(name, data.data(), data.size(), sample_rate);
  if (!problem.empty()) throw WasmRangeError(problem);
  return Audio::from_vector(std::move(data), sample_rate);
}

// Loads a JS buffer under the shared offline-input policy, refusing it as a RangeError.
Audio loadAcousticAudio(const char* name, val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples, name);
  return acousticAudioFromVector(name, std::move(data), sample_rate);
}

// Wire string for a diagnostic severity, matching the RirDiagnostic type.
const char* rirSeverityName(sonare::Diagnostic::Severity severity) {
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
val diagnosticsArray(const std::vector<sonare::Diagnostic>& diagnostics) {
  val entries = val::array();
  for (const sonare::Diagnostic& diagnostic : diagnostics) {
    val entry = val::object();
    entry.set("code", diagnostic.code);
    entry.set("message", diagnostic.message);
    entry.set("severity", std::string(rirSeverityName(diagnostic.severity)));
    entries.call<void>("push", entry);
  }
  return entries;
}

val js_synthesize_rir(val opts) {
  const int sample_rate = intProperty(opts, "sampleRate", 48000);
  validateAcousticSampleRate(sample_rate);
  sonare::acoustic::RirSynthConfig config;
  config.ism_order = intProperty(opts, "ismOrder", config.ism_order);
  config.late_model = boolProperty(opts, "preferEyring", true)
                          ? sonare::acoustic::ReverbModel::Eyring
                          : sonare::acoustic::ReverbModel::Sabine;
  config.seed = uintProperty(opts, "seed", config.seed);
  config.max_seconds = floatProperty(opts, "maxSec", config.max_seconds);
  config.mixing_time_ms = floatProperty(opts, "mixingTimeMs", config.mixing_time_ms);
  config.crossfade_ms = floatProperty(opts, "crossfadeMs", config.crossfade_ms);
  config.air_absorption_enabled =
      boolProperty(opts, "airAbsorptionEnabled", config.air_absorption_enabled);
  // An implausible climate is an Error diagnostic, thrown below like the geometry errors.
  config.air.temperature_c = floatProperty(opts, "airTemperatureC", config.air.temperature_c);
  config.air.humidity_percent =
      floatProperty(opts, "airHumidityPercent", config.air.humidity_percent);

  const auto room = roomFromVal(opts, sonare::acoustic::kDefaultWallAbsorption);
  const auto placement = placementFromVal(opts, room);
  requireRirRequest(room, placement, config);
  const auto result = sonare::acoustic::synthesize_rir(room, placement, sample_rate, config);
  if (sonare::has_error(result.diagnostics)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  sonare::acoustic::first_error_text(result.diagnostics));
  }
  val out = val::object();
  out.set("rir", vectorToFloat32Array(result.rir.data(), result.rir.size()));
  out.set("sampleRate", result.rir.sample_rate());
  out.set("diagnostics", diagnosticsArray(result.diagnostics));
  return out;
}

val js_estimate_room(val samples, const val& sample_rate_val, val opts) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateAcousticSampleRate(sample_rate);
  Audio audio = loadAcousticAudio("samples", samples, sample_rate);
  // As the C ABI reads its struct: an omitted key keeps the library default and a given
  // value is read literally, so a 0 outside a field's domain is refused by name.
  sonare::RoomEstimateConfig config;
  config.aspect_hint_lw = floatProperty(opts, "aspectHintLw", config.aspect_hint_lw);
  config.aspect_hint_lh = floatProperty(opts, "aspectHintLh", config.aspect_hint_lh);
  config.reference_absorption =
      floatProperty(opts, "referenceAbsorption", config.reference_absorption);
  config.prefer_eyring = boolProperty(opts, "preferEyring", config.prefer_eyring);
  config.acoustic.n_octave_bands =
      intProperty(opts, "nOctaveBands", config.acoustic.n_octave_bands);
  config.acoustic.min_decay_db = floatProperty(opts, "minDecayDb", config.acoustic.min_decay_db);
  config.acoustic.noise_floor_margin_db =
      floatProperty(opts, "noiseFloorMarginDb", config.acoustic.noise_floor_margin_db);
  const int mode = intProperty(opts, "mode", 0);
  requireOrdinalInRange(
      mode, 0, static_cast<int>(sonare::acoustic::kAcousticModeSelectorNames.size()) - 1, "mode");
  config.acoustic.mode = static_cast<sonare::AcousticConfig::Mode>(mode);

  // The core refuses these too, under member names; checked first to name the JS field.
  {
    namespace check = sonare::util::check;
    check::greater_than(config.aspect_hint_lw, 0.0, "aspectHintLw");
    check::greater_than(config.aspect_hint_lh, 0.0, "aspectHintLh");
    check::in_range(config.reference_absorption, sonare::kRoomReferenceAbsorptionMin,
                    sonare::kRoomReferenceAbsorptionMax, "referenceAbsorption");
    check::greater_than(config.acoustic.min_decay_db, 0.0, "minDecayDb");
    check::at_least(config.acoustic.noise_floor_margin_db, 0.0, "noiseFloorMarginDb");
    check::integer_at_least(config.acoustic.n_octave_bands, 1, "nOctaveBands");
  }

  const sonare::RoomEstimate est = sonare::estimate_room(audio, config);
  // Absorption (from the inverse problem) and RT60 (from the decay fit) are independent estimates
  // and either can fail on its own: a failed side is absent, and a surviving shorter one is padded
  // with NaN to the other's length so the bands line up.
  const size_t band_count = std::max(est.absorption_bands.size(), est.rt60_bands.size());
  const auto pad_with_nan = [band_count](std::vector<float> values) {
    if (!values.empty()) values.resize(band_count, std::numeric_limits<float>::quiet_NaN());
    return values;
  };
  val out = val::object();
  setMeasured(out, "volume", est.volume);
  setMeasured(out, "lengthM", est.dims.length);
  setMeasured(out, "widthM", est.dims.width);
  setMeasured(out, "heightM", est.dims.height);
  setMeasured(out, "drrDb", est.drr_db);
  out.set("confidence", est.confidence);
  // nOctaveBands is at least 1, so no band at all means both estimates failed.
  if (band_count > 0) {
    setMeasuredArray(out, "bandAbsorption", pad_with_nan(est.absorption_bands), band_count);
    setMeasuredArray(out, "rt60Bands", pad_with_nan(est.rt60_bands), band_count);
  }
  return out;
}

// The stereo entry reads `receiverSpacingM` and passes it in; an absent spacing is fitted to the
// room and placement.
static sonare::effects::acoustic::RoomMorphConfig roomMorphConfigFromVal(
    val opts, std::optional<float> receiver_spacing_m) {
  sonare::effects::acoustic::RoomMorphConfig config;
  config.target = roomFromVal(opts, sonare::acoustic::kDefaultWallAbsorption);
  sonare::acoustic::RoomPlacementRequest request = placementRequestFromVal(opts);
  request.receiver_spacing_m = receiver_spacing_m;
  const sonare::acoustic::ResolvedRoomPlacement resolved =
      sonare::acoustic::resolve_room_placement(config.target.dims, request);
  config.placement = resolved.placement;
  config.receiver_spacing_m = resolved.receiver_spacing_m;
  config.supplied = resolved.supplied;
  config.source_tail_suppression =
      domainFloatProperty(opts, "sourceTailSuppression", config.source_tail_suppression);
  config.wet = domainFloatProperty(opts, "wet", config.wet);
  config.ism_order = intProperty(opts, "ismOrder", config.ism_order);
  config.seed = uintProperty(opts, "seed", config.seed);
  config.max_seconds = domainFloatProperty(opts, "maxSec", config.max_seconds);
  config.late_model = boolProperty(opts, "preferEyring", true)
                          ? sonare::acoustic::ReverbModel::Eyring
                          : sonare::acoustic::ReverbModel::Sabine;
  config.mixing_time_ms = domainFloatProperty(opts, "mixingTimeMs", config.mixing_time_ms);
  config.crossfade_ms = domainFloatProperty(opts, "crossfadeMs", config.crossfade_ms);
  // An implausible climate throws here (the morph core validates rather than diagnosing).
  config.air_absorption_enabled =
      boolProperty(opts, "airAbsorptionEnabled", config.air_absorption_enabled);
  config.air.temperature_c = domainFloatProperty(opts, "airTemperatureC", config.air.temperature_c);
  config.air.humidity_percent =
      domainFloatProperty(opts, "airHumidityPercent", config.air.humidity_percent);
  sonare::effects::acoustic::validate_room_morph_config(
      config, sonare::acoustic::kShoeboxJsFieldNames, sonare::acoustic::kRirSynthJsFieldNames);
  return config;
}

val js_room_morph(val samples, const val& sample_rate_val, val opts) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateAcousticSampleRate(sample_rate);
  Audio audio = loadAcousticAudio("samples", samples, sample_rate);
  // The config's own validation runs inside the respelling, so it refuses in the request's keys.
  const auto result = sonare::util::check::with_camel_paths(
      [&] {
        const auto config = roomMorphConfigFromVal(opts, std::nullopt);
        return sonare::effects::acoustic::room_morph(audio, config);
      },
      sonare::effects::acoustic::kRoomMorphRenamedKeys);

  // Shaped like js_synthesize_rir's result rather than a bare buffer: the same
  // synthesis runs underneath and its warnings describe a room the caller did
  // not ask for. An unusable request throws, so every entry here is a warning.
  val out = val::object();
  out.set("audio", vectorToFloat32Array(result.audio.data(), result.audio.size()));
  out.set("sampleRate", result.audio.sample_rate());
  out.set("diagnostics", diagnosticsArray(result.diagnostics));
  return out;
}

val js_room_morph_stereo(val left, val right, const val& sample_rate_val, val opts) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateAcousticSampleRate(sample_rate);
  std::vector<float> left_data = float32ArrayToVector(left, "left");
  std::vector<float> right_data = float32ArrayToVector(right, "right");
  if (left_data.size() != right_data.size()) {
    throw WasmRangeError("left and right must have the same length");
  }
  Audio left_audio = acousticAudioFromVector("left", std::move(left_data), sample_rate);
  Audio right_audio = acousticAudioFromVector("right", std::move(right_data), sample_rate);
  const auto result = sonare::util::check::with_camel_paths(
      [&] {
        const std::optional<float> receiver_spacing_m =
            hasProperty(opts, "receiverSpacingM")
                ? std::optional<float>(domainFloatProperty(opts, "receiverSpacingM", 0.0f))
                : std::nullopt;
        const auto config = roomMorphConfigFromVal(opts, receiver_spacing_m);
        return sonare::effects::acoustic::room_morph_stereo(left_audio, right_audio, config);
      },
      sonare::effects::acoustic::kRoomMorphRenamedKeys);
  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left.data(), result.left.size()));
  out.set("right", vectorToFloat32Array(result.right.data(), result.right.size()));
  out.set("sampleRate", result.left.sample_rate());
  out.set("diagnostics", diagnosticsArray(result.diagnostics));
  return out;
}

// Shoebox geometry for synthesizeRir taken from an estimate result, under the synthesizer's own
// field names, with the default placement scaled into the estimated room and the scalar absorption
// set to the mean of the finite bands. Absorption bands that did not converge are left out.
val js_room_geometry_from_estimate(val estimate) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  // RoomEstimateResult omits a dimension it could not measure; the core spells that NaN.
  const float length = floatOption(estimate, "lengthM", nan);
  const float width = floatOption(estimate, "widthM", nan);
  const float height = floatOption(estimate, "heightM", nan);
  if (!sonare::acoustic::estimated_dimensions_measured(length, width, height)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "room estimate has no measurable dimensions");
  }
  val out = val::object();
  out.set("lengthM", length);
  out.set("widthM", width);
  out.set("heightM", height);
  const sonare::acoustic::SourceListener placement =
      sonare::acoustic::scaled_default_placement(length, width, height);
  val source = val::object();
  source.set("x", placement.source.x);
  source.set("y", placement.source.y);
  source.set("z", placement.source.z);
  out.set("source", source);
  val listener = val::object();
  listener.set("x", placement.listener.x);
  listener.set("y", placement.listener.y);
  listener.set("z", placement.listener.z);
  out.set("listener", listener);
  if (hasProperty(estimate, "bandAbsorption")) {
    const std::vector<float> bands =
        float32ArrayToVector(estimate["bandAbsorption"], "bandAbsorption");
    const float mean = sonare::acoustic::mean_finite_absorption(bands.data(), bands.size());
    if (std::isfinite(mean)) out.set("absorption", mean);
    if (sonare::acoustic::estimated_absorption_measured(bands.data(), bands.size())) {
      out.set("bandAbsorption", vectorToFloat32Array(bands));
    }
  }
  return out;
}
#endif  // SONARE_WITH_ACOUSTIC_SIM

// The selector names the room-acoustics options accept, in the order of their integer values.
val js_acoustic_selector_names() {
  const auto toArray = [](const auto& names) {
    val array = val::array();
    for (const char* name : names) array.call<void>("push", std::string(name));
    return array;
  };
  val out = val::object();
  out.set("materialPreset", toArray(sonare::acoustic::kMaterialPresetSelectorNames));
  out.set("mode", toArray(sonare::acoustic::kAcousticModeSelectorNames));
  return out;
}

void registerRoomAcousticsBindings() {
  function("acousticSelectorNames", &js_acoustic_selector_names);
  function("analyzeImpulseResponse", &js_analyze_impulse_response);
  function("detectAcoustic", &js_detect_acoustic);
#ifdef SONARE_WITH_ACOUSTIC_SIM
  function("synthesizeRir", &js_synthesize_rir);
  function("estimateRoom", &js_estimate_room);
  function("roomMorph", &js_room_morph);
  function("roomMorphStereo", &js_room_morph_stereo);
  function("roomGeometryFromEstimate", &js_room_geometry_from_estimate);
#endif
}

#endif  // __EMSCRIPTEN__
