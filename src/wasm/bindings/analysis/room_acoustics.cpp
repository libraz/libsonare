/// @file room_acoustics.cpp
/// @brief Embind bindings for room-acoustics analysis and simulation APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>

#include "acoustic/estimate_geometry.h"
#include "acoustic/material.h"
#include "acoustic/selector_names.h"
#include "util/numeric_validation.h"
#include "util/zero_is_default.h"
#include "wasm/bindings/common/common.h"

val acousticParametersToVal(const AcousticParameters& params) {
  val out = val::object();
  out.set("rt60", params.rt60);
  out.set("edt", params.edt);
  out.set("c50", params.c50);
  out.set("c80", params.c80);
  out.set("d50", params.d50);
  out.set("rt60Bands", vectorToFloat32Array(params.rt60_bands));
  out.set("edtBands", vectorToFloat32Array(params.edt_bands));
  out.set("c50Bands", vectorToFloat32Array(params.c50_bands));
  out.set("c80Bands", vectorToFloat32Array(params.c80_bands));
  out.set("confidence", params.confidence);
  out.set("isBlind", params.is_blind);
  return out;
}

val js_analyze_impulse_response_ex(val samples, const val& sample_rate_val,
                                   const val& n_octave_bands_val, const val& min_decay_db_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_octave_bands = checkedIntFromVal(n_octave_bands_val, "nOctaveBands");
  const float min_decay_db = checkedFloatFromVal(min_decay_db_val, "minDecayDb");
  AcousticConfig config;
  config.n_octave_bands = n_octave_bands;
  config.min_decay_db = min_decay_db;
  validate_acoustic_config(config);
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return acousticParametersToVal(analyze_impulse_response(audio, config));
}

val js_analyze_impulse_response(val samples, const val& sample_rate, const val& n_octave_bands) {
  return js_analyze_impulse_response_ex(samples, sample_rate, n_octave_bands, val(30.0f));
}

val js_detect_acoustic(val samples, const val& sample_rate_val, const val& n_octave_bands_val,
                       const val& n_third_octave_subbands_val, const val& min_decay_db_val,
                       const val& noise_floor_margin_db_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_octave_bands = checkedIntFromVal(n_octave_bands_val, "nOctaveBands");
  const int n_third_octave_subbands =
      checkedIntFromVal(n_third_octave_subbands_val, "nThirdOctaveSubbands");
  const float min_decay_db = checkedFloatFromVal(min_decay_db_val, "minDecayDb");
  const float noise_floor_margin_db =
      checkedFloatFromVal(noise_floor_margin_db_val, "noiseFloorMarginDb");
  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_octave_bands = n_octave_bands;
  config.n_third_octave_subbands = n_third_octave_subbands;
  config.min_decay_db = min_decay_db;
  config.noise_floor_margin_db = noise_floor_margin_db;
  validate_acoustic_config(config);
  Audio audio = loadValidatedAudio(samples, sample_rate);
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
    request.absorption_bands = float32ArrayToVector(opts["bandAbsorption"]);
  }
  if (hasProperty(opts, "bandScattering")) {
    request.scattering_bands = float32ArrayToVector(opts["bandScattering"]);
  }
  request.absorption = floatProperty(opts, "absorption", def_absorption);
  return make_uniform_room(dims, request);
}

// Reads a deterministic late-tail seed over the C ABI's full uint32 domain, keeping @p fallback
// when the option is absent or exactly 0 (the C ABI's "seed == 0 keeps the library default").
// A non-number is a TypeError and a value outside [0, 2^32 - 1] or fractional a RangeError.
unsigned seedFromVal(val opts, unsigned fallback) {
  const uint32_t seed = uintProperty(opts, "seed", 0);
  return seed == 0 ? fallback : seed;
}

sonare::acoustic::SourceListener placementFromVal(val opts) {
  const auto& source = sonare::acoustic::kDefaultPlacement.source;
  const auto& listener = sonare::acoustic::kDefaultPlacement.listener;
  return {
      {floatProperty(opts, "sourceX", source.x), floatProperty(opts, "sourceY", source.y),
       floatProperty(opts, "sourceZ", source.z)},
      {floatProperty(opts, "listenerX", listener.x), floatProperty(opts, "listenerY", listener.y),
       floatProperty(opts, "listenerZ", listener.z)}};
}

// Acoustic sample-rate bounds, sourced from the shared core limits
// (sonare::kMinAudioSampleRate / kMaxAudioSampleRate) so every binding rejects
// the same out-of-range rates (the C++ functions are otherwise called directly).
void validateAcousticSampleRate(int sample_rate) {
  if (sample_rate < sonare::kMinAudioSampleRate || sample_rate > sonare::kMaxAudioSampleRate) {
    throw WasmRangeError("sampleRate out of supported range [" +
                         std::to_string(sonare::kMinAudioSampleRate) + ", " +
                         std::to_string(sonare::kMaxAudioSampleRate) + "]");
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

Audio acousticAudioFromVector(std::vector<float> data, int sample_rate) {
  if (const char* problem = offline_audio_input_problem(data.data(), data.size(), sample_rate)) {
    throw WasmRangeError(problem);
  }
  return Audio::from_vector(std::move(data), sample_rate);
}

// Loads a JS buffer under the shared offline-input policy, refusing it as a RangeError.
Audio loadAcousticAudio(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  return acousticAudioFromVector(std::move(data), sample_rate);
}

// Wire string for a diagnostic severity, matching the RirDiagnostic type.
const char* rirSeverityName(sonare::Diagnostic::Severity severity) {
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
  config.seed = seedFromVal(opts, config.seed);
  config.max_seconds = floatProperty(opts, "maxSeconds", config.max_seconds);
  config.mixing_time_ms = floatProperty(opts, "mixingTimeMs", config.mixing_time_ms);
  // crossfadeMs == 0 keeps the library default; any other value reaches the core's rule as given.
  config.crossfade_ms = sonare::ZeroIsDefault(floatProperty(opts, "crossfadeMs", 0.0f))
                            .or_default(config.crossfade_ms);
  config.air_absorption_enabled =
      boolProperty(opts, "airAbsorptionEnabled", config.air_absorption_enabled);
  // airTemperatureC / airHumidityPercent == 0 keep the ISO reference climate
  // (20 degC, 50 % RH), matching the C ABI's "0 means the library default" rule
  // so the same options object yields the same RIR on every surface. An
  // implausible climate is an Error diagnostic, thrown below like the geometry errors.
  config.air.temperature_c = sonare::ZeroIsDefault(floatProperty(opts, "airTemperatureC", 0.0f))
                                 .or_default(config.air.temperature_c);
  config.air.humidity_percent =
      sonare::ZeroIsDefault(floatProperty(opts, "airHumidityPercent", 0.0f))
          .or_default(config.air.humidity_percent);

  const auto placement = placementFromVal(opts);
  const auto room = roomFromVal(opts, 0.2f);
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
  Audio audio = loadAcousticAudio(samples, sample_rate);
  sonare::RoomEstimateConfig config;
  // Match the C ABI: an explicit 0 aspect hint means "use the default 1.0", so
  // the same input is accepted identically on every surface (raw 0 would be
  // rejected by the core's finite-positive check).
  config.aspect_hint_lw = sonare::ZeroIsDefault(floatProperty(opts, "aspectHintLw", 0.0f))
                              .or_default(config.aspect_hint_lw);
  config.aspect_hint_lh = sonare::ZeroIsDefault(floatProperty(opts, "aspectHintLh", 0.0f))
                              .or_default(config.aspect_hint_lh);
  config.reference_absorption =
      sonare::ZeroIsDefault(floatProperty(opts, "referenceAbsorption", 0.0f))
          .or_default(config.reference_absorption);
  config.prefer_eyring = boolProperty(opts, "preferEyring", true);
  const int n_bands = intProperty(opts, "nOctaveBands", 0);
  if (n_bands != 0) config.acoustic.n_octave_bands = n_bands;
  config.acoustic.min_decay_db = sonare::ZeroIsDefault(floatProperty(opts, "minDecayDb", 0.0f))
                                     .or_default(config.acoustic.min_decay_db);
  config.acoustic.noise_floor_margin_db =
      sonare::ZeroIsDefault(floatProperty(opts, "noiseFloorMarginDb", 0.0f))
          .or_default(config.acoustic.noise_floor_margin_db);
  const int mode = intProperty(opts, "mode", 0);
  requireOrdinalInRange(
      mode, 0, static_cast<int>(sonare::acoustic::kAcousticModeSelectorNames.size()) - 1, "mode");
  config.acoustic.mode = static_cast<sonare::AcousticConfig::Mode>(mode);

  const sonare::RoomEstimate est = sonare::estimate_room(audio, config);
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
  val out = val::object();
  out.set("volume", est.volume);
  out.set("lengthM", est.dims.length);
  out.set("widthM", est.dims.width);
  out.set("heightM", est.dims.height);
  out.set("drrDb", est.drr_db);
  out.set("confidence", est.confidence);
  out.set("bandAbsorption", vectorToFloat32Array(absorption_bands));
  out.set("rt60Bands", vectorToFloat32Array(rt60_bands));
  return out;
}

static sonare::effects::acoustic::RoomMorphConfig roomMorphConfigFromVal(val opts) {
  sonare::effects::acoustic::RoomMorphConfig config;
  config.target = roomFromVal(opts, 0.2f);
  config.placement = placementFromVal(opts);
  config.source_tail_suppression =
      floatProperty(opts, "sourceTailSuppression", config.source_tail_suppression);
  config.wet = floatProperty(opts, "wet", config.wet);
  config.ism_order = intProperty(opts, "ismOrder", config.ism_order);
  config.seed = seedFromVal(opts, config.seed);
  config.max_seconds = floatProperty(opts, "maxSeconds", config.max_seconds);
  config.late_model = boolProperty(opts, "preferEyring", true)
                          ? sonare::acoustic::ReverbModel::Eyring
                          : sonare::acoustic::ReverbModel::Sabine;
  config.mixing_time_ms = floatProperty(opts, "mixingTimeMs", config.mixing_time_ms);
  // Same crossfade sentinel rule as synthesizeRir above.
  config.crossfade_ms = sonare::ZeroIsDefault(floatProperty(opts, "crossfadeMs", 0.0f))
                            .or_default(config.crossfade_ms);
  // Air absorption on the target room; the zero-means-ISO-reference rule is the
  // same as synthesizeRir above. An implausible climate throws here (the morph
  // core validates rather than diagnosing), matching the C ABI.
  config.air_absorption_enabled =
      boolProperty(opts, "airAbsorptionEnabled", config.air_absorption_enabled);
  config.air.temperature_c = sonare::ZeroIsDefault(floatProperty(opts, "airTemperatureC", 0.0f))
                                 .or_default(config.air.temperature_c);
  config.air.humidity_percent =
      sonare::ZeroIsDefault(floatProperty(opts, "airHumidityPercent", 0.0f))
          .or_default(config.air.humidity_percent);
  sonare::effects::acoustic::validate_room_morph_config(
      config, sonare::acoustic::kShoeboxJsFieldNames, sonare::acoustic::kRirSynthJsFieldNames);
  return config;
}

val js_room_morph(val samples, const val& sample_rate_val, val opts) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateAcousticSampleRate(sample_rate);
  Audio audio = loadAcousticAudio(samples, sample_rate);
  const auto config = roomMorphConfigFromVal(opts);

  const auto result = sonare::effects::acoustic::room_morph(audio, config);

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
  std::vector<float> left_data = float32ArrayToVector(left);
  std::vector<float> right_data = float32ArrayToVector(right);
  if (left_data.size() != right_data.size()) {
    throw WasmRangeError("Stereo channel lengths must match.");
  }
  Audio left_audio = acousticAudioFromVector(std::move(left_data), sample_rate);
  Audio right_audio = acousticAudioFromVector(std::move(right_data), sample_rate);
  auto config = roomMorphConfigFromVal(opts);
  config.receiver_spacing_m = floatProperty(opts, "receiverSpacingM", config.receiver_spacing_m);

  const auto result = sonare::effects::acoustic::room_morph_stereo(left_audio, right_audio, config);
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
  out.set("sourceX", placement.source.x);
  out.set("sourceY", placement.source.y);
  out.set("sourceZ", placement.source.z);
  out.set("listenerX", placement.listener.x);
  out.set("listenerY", placement.listener.y);
  out.set("listenerZ", placement.listener.z);
  if (hasProperty(estimate, "bandAbsorption")) {
    const std::vector<float> bands = float32ArrayToVector(estimate["bandAbsorption"]);
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
  function("analyzeImpulseResponseEx", &js_analyze_impulse_response_ex);
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
