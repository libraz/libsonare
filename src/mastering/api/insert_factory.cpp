#include "mastering/api/insert_factory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "mastering/api/named_processor.h"
#include "mastering/api/processor_params.h"
#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/expander.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/limiter.h"
#include "mastering/dynamics/parallel_comp.h"
#include "mastering/dynamics/sidechain_router.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/upward_compressor.h"
#include "mastering/dynamics/upward_expander.h"
#include "mastering/dynamics/vocal_rider.h"
#include "mastering/eq/api_style.h"
#include "mastering/eq/band_pass.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/pultec.h"
#include "mastering/eq/shelving.h"
#include "mastering/eq/tilt.h"
#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/maximizer.h"
#include "mastering/maximizer/soft_knee_max.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/multiband/multiband_dynamic_eq.h"
#include "mastering/multiband/multiband_expander.h"
#include "mastering/multiband/multiband_imager.h"
#include "mastering/multiband/multiband_limiter.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/saturation/amp_presets.h"
#include "mastering/saturation/amp_sim.h"
#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/hard_clipper.h"
#include "mastering/saturation/multiband_exciter.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/transformer.h"
#include "mastering/saturation/tube.h"
#include "mastering/saturation/waveshaper.h"
#include "mastering/spectral/air_band.h"
#include "mastering/spectral/low_end_focus.h"
#include "mastering/spectral/presence_enhancer.h"
#include "mastering/spectral/spectral_shaper.h"
#include "mastering/stereo/auto_pan.h"
#include "mastering/stereo/haas_enhancer.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "mastering/stereo/phase_align.h"
#include "mastering/stereo/stereo_balance.h"
#include "util/base64.h"
#include "util/exception.h"
#include "util/json.h"

#ifdef SONARE_HAVE_FX
#include <algorithm>
#include <cmath>

#include "effects/delay/stereo_delay.h"
#include "effects/modulation/auto_wah.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/ensemble.h"
#include "effects/modulation/flanger.h"
#include "effects/modulation/phaser.h"
#include "effects/modulation/pitch_shifter.h"
#include "effects/modulation/ring_modulator.h"
#include "effects/modulation/rotary.h"
#include "effects/modulation/wah.h"
#include "effects/reverb/convolution_reverb.h"
#include "effects/reverb/dattorro_reverb.h"
#include "effects/reverb/fdn_reverb.h"
#include "effects/reverb/velvet_reverb.h"
#ifdef SONARE_HAVE_ACOUSTIC
#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "effects/acoustic/room_morph.h"
#include "effects/reverb/room_reverb.h"
#include "util/number_format.h"
#include "util/zero_is_default.h"
#endif
#endif

namespace sonare::mastering::api {
namespace {

using detail::b;
using detail::compressor_config;
using detail::crossover_config;
using detail::f;
using detail::limiter_config;
using detail::ParamKind;
using detail::ParamMap;

// Decodes a little-endian f32 array carried as base64 under @p key. Two inserts
// take an IR this way (the convolution reverb and the amp sim's cabinet), so the
// key is a parameter rather than being baked in. Not FX-gated: the amp sim ships
// in every configuration.
std::vector<float> parse_ir_f32_base64_json(const std::string& json_params, const char* key) {
  if (json_params.empty()) return {};
  const auto root = sonare::util::json::parse_strict(json_params);
  if (!root.is_object()) {
    throw SonareException(ErrorCode::InvalidParameter, "expected JSON object");
  }
  const auto* value = root.find(key);
  if (value == nullptr) return {};
  if (!value->is_string()) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be a string");
  }

  std::vector<uint8_t> bytes;
  if (!base64_decode(value->as_string(), &bytes)) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " is malformed");
  }
  if (bytes.size() % sizeof(float) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(key) + " byte length must be f32 aligned");
  }

  std::vector<float> ir(bytes.size() / sizeof(float), 0.0f);
  for (size_t i = 0; i < ir.size(); ++i) {
    const size_t offset = i * sizeof(float);
    const uint32_t bits = static_cast<uint32_t>(bytes[offset]) |
                          (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                          (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                          (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    float sample = 0.0f;
    std::memcpy(&sample, &bits, sizeof(sample));
    if (!std::isfinite(sample)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(key) + " contains non-finite samples");
    }
    ir[i] = sample;
  }
  return ir;
}

// Reads a string-valued key straight from the original JSON. The flat ParamMap
// holds doubles, so anything that is not a number has to be fetched this way.
// Absent -> empty; present but not a string -> a caller error worth surfacing.
std::string parse_string_json(const std::string& json_params, const char* key) {
  if (json_params.empty()) return {};
  const auto root = sonare::util::json::parse_strict(json_params);
  if (!root.is_object()) {
    throw SonareException(ErrorCode::InvalidParameter, "expected JSON object");
  }
  const auto* value = root.find(key);
  if (value == nullptr) return {};
  if (!value->is_string()) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be a string");
  }
  return value->as_string();
}

// The keys whose value cannot live in the flat ParamMap, and the single insert
// each one belongs to. They used to be skipped for EVERY insert, so a `preset`
// or a base64 IR handed to an insert that has no such control was accepted,
// dropped, and — because a skipped key never enters the map — absent from
// unprobed_keys() as well, leaving no surface on which the caller could notice.
bool insert_reads_json_string_key(const std::string& insert_name, const std::string& key) {
  if (key == "preset") return insert_name == "saturation.ampSim";
  if (key == "cabIrF32Base64") return insert_name == "saturation.ampSim";
  if (key == "irF32Base64") return insert_name == "effects.reverb.convolution";
  return false;
}

std::vector<Param> parse_insert_params_json(const std::string& json_params,
                                            const std::string& insert_name) {
  const bool allow_acoustic_material_arrays = insert_name == "effects.acoustic.roomMorph";
  try {
    if (json_params.empty()) return {};
    // Strict parse: insert params are a flat map of `{name: value}` and a
    // duplicate key would silently shadow the earlier value, which is almost
    // certainly a caller bug worth surfacing.
    const auto root = sonare::util::json::parse_strict(json_params);
    if (!root.is_object())
      throw SonareException(ErrorCode::InvalidParameter, "expected JSON object");
    std::vector<Param> params;
    params.reserve(root.as_object().size());
    for (const auto& [key, value] : root.as_object()) {
      if (value.is_bool()) {
        params.push_back(Param{key, value.as_bool() ? 1.0 : 0.0});
      } else if (value.is_number()) {
        params.push_back(Param{key, value.as_number()});
      } else if (allow_acoustic_material_arrays &&
                 (key == "bandAbsorption" || key == "bandScattering") && value.is_array()) {
        // The flat ParamMap cannot carry an array. Acoustic room-morph consumes
        // these two options from the original JSON object in build_effects();
        // accepting them here keeps the generic JSON validation layer from
        // rejecting an option that the acoustic facade already supports.
        continue;
      } else if (value.is_string() && insert_reads_json_string_key(insert_name, key)) {
        // A named rig (the discrete topology/tube/cab/capsule switches no
        // numeric param can reach) or a base64 impulse response. The flat
        // ParamMap holds doubles only, so the insert that owns the key reads it
        // straight from the original JSON object; accepting it here keeps the
        // generic validation layer from rejecting a param the insert supports.
        // Every OTHER insert falls through to the rejection below, so a key
        // aimed at the wrong processor is reported instead of dropped.
        continue;
      } else {
        throw SonareException(ErrorCode::InvalidParameter,
                              "JSON params values must be numbers or booleans");
      }
    }
    return params;
  } catch (const std::invalid_argument& e) {
    throw SonareException(ErrorCode::InvalidParameter, std::string("make_insert: ") + e.what());
  } catch (const sonare::util::json::JsonError& e) {
    throw SonareException(ErrorCode::InvalidParameter, std::string("make_insert: ") + e.what());
  }
}

using Processor = sonare::rt::ProcessorBase;

template <typename T, typename... Args>
std::unique_ptr<Processor> make(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

std::unique_ptr<Processor> build_dynamics(const std::string& name, const ParamMap& params) {
  if (name == "dynamics.brickwallLimiter") {
    return make<dynamics::BrickwallLimiter>(detail::brickwall_limiter_config(params));
  }
  if (name == "dynamics.compressor") {
    return make<dynamics::Compressor>(compressor_config(params));
  }
  if (name == "dynamics.deesser") {
    return make<dynamics::DeEsser>(detail::deesser_config(params));
  }
  if (name == "dynamics.expander") {
    return make<dynamics::Expander>(detail::expander_config(params));
  }
  if (name == "dynamics.gate") {
    return make<dynamics::Gate>(detail::gate_config(params));
  }
  if (name == "dynamics.limiter") {
    return make<dynamics::Limiter>(limiter_config(params));
  }
  if (name == "dynamics.parallelComp") {
    return make<dynamics::ParallelComp>(detail::parallel_comp_config(params));
  }
  if (name == "dynamics.sidechainRouter") {
    return make<dynamics::SidechainRouter>(detail::sidechain_router_config(params));
  }
  if (name == "dynamics.duckingProcessor") {
    return make<dynamics::DuckingProcessor>(detail::ducking_config(params));
  }
  if (name == "dynamics.transientShaper") {
    return make<dynamics::TransientShaper>(detail::transient_shaper_config(params));
  }
  if (name == "dynamics.upwardCompressor") {
    return make<dynamics::UpwardCompressor>(detail::upward_compressor_config(params));
  }
  if (name == "dynamics.upwardExpander") {
    return make<dynamics::UpwardExpander>(detail::upward_expander_config(params));
  }
  if (name == "dynamics.vocalRider") {
    return make<dynamics::VocalRider>(detail::vocal_rider_config(params));
  }
  return nullptr;
}

std::unique_ptr<Processor> build_eq(const std::string& name, const ParamMap& params) {
  if (name == "eq.tilt") {
    auto p = std::make_unique<eq::TiltEq>();
    detail::configure_tilt(*p, params);
    return p;
  }
  if (name == "eq.apiStyle") {
    auto p = std::make_unique<eq::ApiStyleEq>();
    detail::configure_api_style(*p, params);
    return p;
  }
  if (name == "eq.parametric") {
    auto p = std::make_unique<eq::ParametricEq>();
    detail::configure_parametric(*p, params);
    return p;
  }
  if (name == "eq.equalizer") {
    auto p = std::make_unique<eq::EqualizerProcessor>(detail::equalizer_config(params, 2));
    detail::configure_equalizer(*p, params);
    return p;
  }
  if (name == "eq.minimumPhase") {
    auto p = std::make_unique<eq::MinimumPhaseEq>();
    detail::configure_minimum_phase(*p, params);
    return p;
  }
  if (name == "eq.linearPhase") {
    auto p = std::make_unique<eq::LinearPhaseEq>(detail::linear_phase_config(params));
    detail::configure_linear_phase_bands(*p, params);
    return p;
  }
  if (name == "eq.dynamic") {
    auto p = std::make_unique<eq::DynamicEq>();
    detail::configure_dynamic_eq_bands(*p, params);
    return p;
  }
  if (name == "eq.pultec") {
    auto p = std::make_unique<eq::PultecEq>();
    detail::configure_pultec(*p, params);
    return p;
  }
  if (name == "eq.cutFilter") {
    auto p = std::make_unique<eq::CutFilter>();
    detail::configure_cut_filter(*p, params);
    return p;
  }
  if (name == "eq.bandPass") {
    auto p = std::make_unique<eq::BandPassEq>();
    detail::configure_band_pass(*p, params);
    return p;
  }
  if (name == "eq.shelving") {
    auto p = std::make_unique<eq::ShelvingEq>();
    detail::configure_shelving(*p, params);
    return p;
  }
  if (name == "eq.graphic") {
    auto p = std::make_unique<eq::GraphicEq>();
    detail::configure_graphic(*p, params);
    return p;
  }
  if (name == "eq.midSide") {
    auto p = std::make_unique<eq::MidSideEq>();
    detail::configure_mid_side(*p, params);
    return p;
  }
  return nullptr;
}

std::unique_ptr<Processor> build_saturation(const std::string& name, const ParamMap& params,
                                            const std::string* json_params) {
  if (name == "saturation.tape") {
    return make<saturation::Tape>(detail::tape_config(params));
  }
  if (name == "saturation.exciter") {
    return make<saturation::Exciter>(detail::exciter_config(params));
  }
  if (name == "saturation.bitcrusher") {
    return make<saturation::BitCrusher>(detail::bitcrusher_config(params));
  }
  if (name == "saturation.hardClipper") {
    return make<saturation::HardClipper>(detail::hard_clipper_config(params));
  }
  if (name == "saturation.softClipper") {
    return make<saturation::SoftClipper>(detail::soft_clipper_config(params));
  }
  if (name == "saturation.waveshaper") {
    return make<saturation::Waveshaper>(detail::waveshaper_config(params));
  }
  if (name == "saturation.tube") {
    return make<saturation::Tube>(detail::tube_config(params));
  }
  if (name == "saturation.transformer") {
    return make<saturation::Transformer>(detail::transformer_config(params));
  }
  if (name == "saturation.multibandExciter") {
    return make<saturation::MultibandExciter>(detail::multiband_exciter_config(params));
  }
  if (name == "saturation.ampSim") {
    // Guitar amp-sim: drive -> tone stack -> cab-EQ (the track-insert layer of
    // the electric-guitar sound; the string itself is the KS synth voice).
    //
    // A named rig, if given, is the BASE the numeric params ride on top of, so a
    // caller can take a whole amp and turn one control. Without one the base is
    // the processor's own defaults, exactly as before.
    //
    // Both string-valued options are probed so insert_param_names() publishes
    // them: they are read from the JSON side-channel below rather than from the
    // flat map, and a construction option a caller cannot discover may as well
    // not exist. Same reason the acoustic builder probes its band arrays.
    (void)params.find("preset");
    (void)params.find("cabIrF32Base64");
    saturation::AmpSimConfig base;
    if (json_params != nullptr) {
      const std::string preset = parse_string_json(*json_params, "preset");
      if (!preset.empty()) {
        base = saturation::amp_preset_config(saturation::amp_preset_from_string(preset));
      }
    }
    auto amp = make<saturation::AmpSim>(detail::amp_sim_config(params, base));
    // A cabinet IR may ride in the param bag as base64 f32, so a scene or a
    // preset can carry one without a separate binary channel. `cabIrSampleRate`
    // declares the rate it was captured at; omitting it keeps the old contract
    // ("already at the processor's rate") rather than guessing.
    //
    // Probed unconditionally, not inside the `if`: the catalog discovers a
    // processor's params by building it against an empty bag, so a key only read
    // when an IR happens to be present would never be published.
    const double ir_rate = static_cast<double>(detail::f(params, "cabIrSampleRate", 0.0f));
    // A cabinet can also be SYNTHESIZED rather than supplied, which is how a
    // caller gets an IR cab without sourcing a recording. The spec follows the
    // cabinet and mic already configured above, so there is no second set of
    // controls to keep in step; `cabIrDrivers` is the one thing the analytic
    // chain has no equivalent for — whether the cabinet's other drivers are
    // summed, which is the whole difference between an IR and an EQ curve.
    const bool generate_ir = detail::b(params, "cabIrGenerate", false);
    const bool ir_drivers = detail::b(params, "cabIrDrivers", true);
    if (generate_ir) {
      auto* sim = static_cast<saturation::AmpSim*>(amp.get());
      const saturation::AmpSimConfig& configured = sim->amp_config();
      saturation::CabIrSpec spec;
      spec.cab_model = configured.cab_model;
      spec.mic_model = configured.mic_model;
      spec.mic_axis = configured.mic_axis;
      spec.mic_distance_cm = configured.mic_distance_cm;
      spec.presence_db = configured.presence_db;
      spec.multi_driver = ir_drivers;
      sim->load_generated_cab_ir(spec);
    }
    if (json_params != nullptr) {
      const std::vector<float> ir = parse_ir_f32_base64_json(*json_params, "cabIrF32Base64");
      if (!ir.empty()) {
        // A supplied capture wins over a generated cabinet.
        static_cast<saturation::AmpSim*>(amp.get())->load_cab_ir(ir, ir_rate);
      }
    }
    return amp;
  }
  return nullptr;
}

std::unique_ptr<Processor> build_spectral(const std::string& name, const ParamMap& params) {
  if (name == "spectral.airBand") {
    return make<spectral::AirBand>(detail::air_band_config(params));
  }
  if (name == "spectral.lowEndFocus") {
    return make<spectral::LowEndFocus>(detail::low_end_focus_config(params));
  }
  if (name == "spectral.presenceEnhancer") {
    return make<spectral::PresenceEnhancer>(detail::presence_enhancer_config(params));
  }
  if (name == "spectral.spectralShaper") {
    return make<spectral::SpectralShaper>(detail::spectral_shaper_config(params));
  }
  return nullptr;
}

std::unique_ptr<Processor> build_stereo(const std::string& name, const ParamMap& params) {
  if (name == "stereo.autoPan") {
    return make<stereo::AutoPan>(detail::auto_pan_config(params));
  }
  if (name == "stereo.haasEnhancer") {
    return make<stereo::HaasEnhancer>(detail::haas_enhancer_config(params));
  }
  if (name == "stereo.imager") {
    return make<stereo::Imager>(detail::imager_config(params));
  }
  if (name == "stereo.monoMaker") {
    return make<stereo::MonoMaker>(detail::mono_maker_config(params));
  }
  if (name == "stereo.phaseAlign") {
    return make<stereo::PhaseAlign>(detail::phase_align_config(params));
  }
  if (name == "stereo.stereoBalance") {
    return make<stereo::StereoBalance>(detail::stereo_balance_config(params));
  }
  return nullptr;
}

std::unique_ptr<Processor> build_maximizer(const std::string& name, const ParamMap& params) {
  if (name == "maximizer.maximizer") {
    return make<maximizer::Maximizer>(detail::maximizer_config(params));
  }
  // Note: there is intentionally no "maximizer.loudnessOptimize" insert. LUFS
  // normalization is an offline/whole-signal operation (it needs the full
  // integrated-loudness measurement before it can pick a gain) and cannot be
  // expressed as a streaming block processor. Exposing it here would silently
  // drop `targetLufs` and degrade to a bare true-peak limiter, which is
  // surprising. Callers who want true-peak limiting as an insert should use
  // "maximizer.truePeakLimiter"; LUFS targeting lives in the offline mastering
  // chain (loudness.* config).
  if (name == "maximizer.truePeakLimiter") {
    return make<maximizer::TruePeakLimiter>(detail::true_peak_limiter_config(params));
  }
  if (name == "maximizer.softKneeMax") {
    return make<maximizer::SoftKneeMax>(detail::soft_knee_max_config(params));
  }
  if (name == "maximizer.adaptiveRelease") {
    return make<maximizer::AdaptiveRelease>(detail::adaptive_release_config(params));
  }
  return nullptr;
}

std::unique_ptr<Processor> build_multiband(const std::string& name, const ParamMap& params) {
  if (name == "multiband.compressor") {
    multiband::MultibandCompressorConfig config;
    config.crossover = crossover_config(params);
    detail::populate_compressor_bands(config, params);
    return make<multiband::MultibandCompressor>(config);
  }
  if (name == "multiband.expander") {
    multiband::MultibandExpanderConfig config;
    config.crossover = crossover_config(params);
    detail::populate_expander_bands(config, params);
    return make<multiband::MultibandExpander>(config);
  }
  if (name == "multiband.limiter") {
    multiband::MultibandLimiterConfig config;
    config.crossover = crossover_config(params);
    detail::populate_limiter_bands(config, params);
    return make<multiband::MultibandLimiter>(config);
  }
  if (name == "multiband.imager") {
    multiband::MultibandImagerConfig config;
    config.crossover = crossover_config(params);
    detail::populate_imager_bands(config, params);
    return make<multiband::MultibandImager>(config);
  }
  if (name == "multiband.saturation") {
    multiband::MultibandSaturationConfig config;
    config.crossover = crossover_config(params);
    detail::populate_saturation_bands(config, params);
    return make<multiband::MultibandSaturation>(config);
  }
  if (name == "multiband.dynamicEq") {
    multiband::MultibandDynamicEqConfig config;
    config.crossover = crossover_config(params);
    detail::populate_dynamic_eq_bands(config, params);
    return make<multiband::MultibandDynamicEq>(config);
  }
  return nullptr;
}

#ifdef SONARE_HAVE_FX
#ifdef SONARE_HAVE_ACOUSTIC
bool acoustic_material_preset_from_int(int selector, sonare::acoustic::MaterialPreset* out) {
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

// Reads one material-band array out of the insert's JSON side-channel. Only the
// JSON shape is checked here; the coefficients themselves are validated by the
// core builder, on the same rule as every other surface.
std::vector<float> acoustic_material_bands(const sonare::util::json::Value* value,
                                           const char* key) {
  if (value == nullptr) return {};
  if (!value->is_array()) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be an array");
  }

  std::vector<float> bands;
  bands.reserve(value->size());
  for (const auto& band : value->as_array()) {
    if (!band.is_number()) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(key) + " values must be within [0, 1]");
    }
    bands.push_back(band.as_float());
  }
  return bands;
}

sonare::acoustic::ShoeboxRoom acoustic_room_from_json(const detail::ParamMap& params,
                                                      const std::string* json_params) {
  using namespace sonare::acoustic;

  const RoomDimensions dims{f(params, "lengthM", 7.0f), f(params, "widthM", 5.0f),
                            f(params, "heightM", 3.0f)};
  // The precedence, the [0, 1] coefficient rejection and the band-wise
  // scattering come from the core builder the offline acoustic facade calls, so
  // an insert and a synthesizeRir call resolve the same option bag identically
  // — including rejecting an out-of-range scalar absorption rather than
  // clamping it, which is the one place these two used to disagree.
  WallMaterialRequest request;
  request.has_preset =
      acoustic_material_preset_from_int(detail::i(params, "materialPreset", 0), &request.preset);
  request.absorption = f(params, "absorption", 0.2f);
  if (json_params != nullptr && !json_params->empty()) {
    const auto root = sonare::util::json::parse_strict(*json_params);
    if (!root.is_object()) {
      throw SonareException(ErrorCode::InvalidParameter, "expected JSON object");
    }
    request.absorption_bands =
        acoustic_material_bands(root.find("bandAbsorption"), "bandAbsorption");
    request.scattering_bands =
        acoustic_material_bands(root.find("bandScattering"), "bandScattering");
  }
  return make_uniform_room(dims, request);
}
#endif

std::unique_ptr<Processor> build_effects(const std::string& name, const ParamMap& params,
                                         const std::string* json_params) {
  using namespace sonare::effects::reverb;
  // "effects.reverb.plate" is an alias for "effects.reverb.dattorro": both names
  // construct the same DattorroReverb processor with identical parameters.
  if (name == "effects.reverb.plate" || name == "effects.reverb.dattorro") {
    DattorroReverbConfig config;
    // decaySec is a tail-length intent in seconds; DattorroReverbConfig.decay is
    // a normalized tank feedback clamped internally to [0, 0.98]. The plate tank
    // has no closed-form RT60, but we map decaySec to an APPROXIMATE T60 so it
    // tracks roughly the same seconds as FDN/velvet/convolution instead of being
    // an unrelated 0..0.98 knob. Model: the figure-8 tank circulates once every
    // ~21589 samples (sum of both halves' allpass+delay lengths at the 29761 Hz
    // reference rate) and multiplies energy by decay^4 per full round trip (decay
    // is applied twice per half). Requiring the amplitude to reach 1e-3 (-60 dB)
    // after T60 seconds gives decay = exp(-ln(1000) * Tloop / (4 * T60)).
    if (params.find("decaySec") != params.end()) {
      constexpr float kTankLoopSeconds = 21589.0f / 29761.0f;  // both halves at ref rate
      const float decay_sec = std::max(0.05f, f(params, "decaySec", 5.0f));
      const float feedback = std::exp(-6.907755f * kTankLoopSeconds / (4.0f * decay_sec));
      config.decay = std::min(0.98f, std::max(0.0f, feedback));
    } else {
      config.decay = f(params, "decay", config.decay);
    }
    config.damping = f(params, "damping", config.damping);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    // Figure-8 tank modulation (the plate's chorused tail); wire both fields so
    // they are reachable at construction, matching Velvet's full-field mapping.
    config.mod_rate_hz = f(params, "modRateHz", config.mod_rate_hz);
    config.mod_depth_samples = f(params, "modDepthSamples", config.mod_depth_samples);
    // pre_delay_samples is defined at the reverb's reference rate (header
    // comment), so convert preDelayMs using kReferenceSampleRate; prepare()
    // rescales the resulting sample count to the working sample rate.
    if (params.find("preDelayMs") != params.end()) {
      config.pre_delay_samples = f(params, "preDelayMs", 0.0f) *
                                 static_cast<float>(DattorroReverb::kReferenceSampleRate) / 1000.0f;
    }
    return make<DattorroReverb>(config);
  }
  if (name == "effects.reverb.fdn") {
    // Construction-time keys only: decaySec (seconds -> decay/10) and hfDamping
    // are convenience aliases parsed here. The RT-automatable parameter surface
    // (parameter_descriptors / set_parameter) exposes the unit-normalized keys
    // decay (0..1.5), damping (0..1) and dryWet instead — a host automating the
    // tail at audio rate must target those, not decaySec/hfDamping.
    FdnReverbConfig config;
    if (params.find("decaySec") != params.end()) {
      // decaySec is the approximate RT60 tail length in seconds. The FDN's
      // T60_lf = max(0.01, clamp(decay, 0, 1.5) * 10), so decaySec maps to
      // T60 directly via decay = decaySec / 10. The core clamps decay to 1.5
      // (T60 = 15 s); clamp here too so an out-of-range request like
      // {decaySec:40} resolves to the documented 15 s ceiling rather than being
      // silently truncated only after construction.
      config.decay = std::clamp(f(params, "decaySec", 5.5f) / 10.0f, 0.0f, 1.5f);
    } else {
      config.decay = f(params, "decay", config.decay);
    }
    config.hf_damping = f(params, "damping", f(params, "hfDamping", config.hf_damping));
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<FdnReverb>(config);
  }
  if (name == "effects.reverb.velvet") {
    VelvetReverbConfig config;
    config.decay = f(params, "decay", config.decay);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    if (params.find("decaySec") != params.end()) {
      // Velvet's effective T60 = reverb_time_s * (0.5 + decay). To make decaySec
      // mean approximately the same RT60 as FDN (decaySec == ~T60), set
      // reverb_time_s = decaySec / (0.5 + decay) so the product lands on decaySec.
      const float decay_factor = 0.5f + std::clamp(config.decay, 0.0f, 1.0f);
      config.reverb_time_s =
          std::clamp(std::max(0.0f, f(params, "decaySec", config.reverb_time_s)) /
                         std::max(0.01f, decay_factor),
                     0.05f, VelvetReverbConfig::kMaxReverbTimeSeconds);
    } else {
      config.reverb_time_s = std::clamp(f(params, "reverbTimeS", config.reverb_time_s), 0.05f,
                                        VelvetReverbConfig::kMaxReverbTimeSeconds);
    }
    config.density_hz = f(params, "densityHz", config.density_hz);
    config.enable_shelf = b(params, "enableShelf", config.enable_shelf);
    return make<VelvetReverb>(config);
  }
  if (name == "effects.reverb.convolution") {
    // When no explicit IR is injected (make_insert_with_ir), the convolver
    // synthesizes a decaying-noise IR from these scalar params at prepare() time
    // so the insert produces an actual reverb tail just like its algorithmic
    // siblings. decaySec is the approximate RT60 tail length in seconds (matched
    // to effects.reverb.fdn, where decaySec maps directly to ~T60).
    //
    // Probed so insert_param_names() publishes it: the base64 IR is read from
    // the JSON side-channel below, not from the flat map.
    (void)params.find("irF32Base64");
    ConvolutionReverbConfig config;
    if (params.find("decaySec") != params.end()) {
      // Clamp to the synthesizer's ceiling at construction so an out-of-range
      // request like {decaySec:40} resolves to the documented maximum tail
      // rather than being silently truncated only later in prepare().
      config.decay_sec = std::clamp(f(params, "decaySec", config.decay_sec), 0.0f,
                                    ConvolutionReverbConfig::kMaxDecaySeconds);
    }
    config.pre_delay_ms = f(params, "preDelayMs", config.pre_delay_ms);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    config.seed = static_cast<uint32_t>(std::max(0, detail::i(params, "seed", config.seed)));
    auto reverb = std::make_unique<ConvolutionReverb>(config);
    std::vector<float> ir =
        json_params ? parse_ir_f32_base64_json(*json_params, "irF32Base64") : std::vector<float>{};
    if (!ir.empty()) {
      reverb->load_ir(ir);
    }
    return reverb;
  }
#ifdef SONARE_HAVE_ACOUSTIC
  if (name == "effects.reverb.room") {
    // Geometry-driven 5th engine: the RIR is synthesized from the shoebox
    // dimensions + uniform absorption at prepare() time, then convolved.
    RoomReverbConfig config;
    config.dims = {f(params, "lengthM", config.dims.length), f(params, "widthM", config.dims.width),
                   f(params, "heightM", config.dims.height)};
    config.source = {f(params, "sourceX", config.source.x), f(params, "sourceY", config.source.y),
                     f(params, "sourceZ", config.source.z)};
    config.listener = {f(params, "listenerX", config.listener.x),
                       f(params, "listenerY", config.listener.y),
                       f(params, "listenerZ", config.listener.z)};
    // Rejected rather than clamped, matching sonare_synthesize_rir: the same
    // out-of-range absorption has to surface the same error whether the room is
    // built offline or as an insert.
    config.absorption = f(params, "absorption", config.absorption);
    sonare::acoustic::validate_material_coefficient(config.absorption, "absorption");
    config.ism_order = std::max(0, detail::i(params, "ismOrder", config.ism_order));
    config.seed = static_cast<unsigned>(
        std::max(0, detail::i(params, "seed", static_cast<int>(config.seed))));
    config.max_seconds = f(params, "maxSeconds", config.max_seconds);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    config.air_absorption_enabled =
        b(params, "airAbsorptionEnabled", config.air_absorption_enabled);
    // The climate pair follows the acoustic ABI's "0 selects the library value"
    // rule (the ISO reference climate), so the same option bag resolves to the
    // same room here as on the offline facade.
    config.air.temperature_c =
        ZeroIsDefault(f(params, "airTemperatureC", 0.0f)).or_default(config.air.temperature_c);
    config.air.humidity_percent = ZeroIsDefault(f(params, "airHumidityPercent", 0.0f))
                                      .or_default(config.air.humidity_percent);
    return make<RoomReverb>(config);
  }
  if (name == "effects.acoustic.roomMorph") {
    // Source-reverb tail suppression in front of a target-room convolution. The
    // target-room material follows the same precedence as the offline acoustic
    // facade: materialPreset > bandAbsorption > scalar absorption.
    effects::acoustic::RoomMorphConfig config;
    // Probe the array keys so insert_param_names() publishes all construction
    // options even though the flat ParamMap cannot hold their values.
    (void)params.find("bandAbsorption");
    (void)params.find("bandScattering");
    config.target = acoustic_room_from_json(params, json_params);
    config.placement.source = {f(params, "sourceX", 1.0f), f(params, "sourceY", 1.0f),
                               f(params, "sourceZ", 1.2f)};
    config.placement.listener = {f(params, "listenerX", 5.0f), f(params, "listenerY", 4.0f),
                                 f(params, "listenerZ", 1.7f)};
    config.source_tail_suppression =
        f(params, "sourceTailSuppression", config.source_tail_suppression);
    config.wet = f(params, "dryWet", config.wet);
    config.ism_order = detail::i(params, "ismOrder", config.ism_order);
    config.seed = static_cast<unsigned>(
        std::max(0, detail::i(params, "seed", static_cast<int>(config.seed))));
    config.max_seconds = f(params, "maxSeconds", config.max_seconds);
    config.late_model = b(params, "preferEyring", true) ? sonare::acoustic::ReverbModel::Eyring
                                                        : sonare::acoustic::ReverbModel::Sabine;
    config.mixing_time_ms = f(params, "mixingTimeMs", config.mixing_time_ms);
    // A zero crossfade means "use the library default" on the offline acoustic
    // facade; preserve that normalization for the streaming insert as well.
    config.crossfade_ms = ZeroIsDefault(f(params, "crossfadeMs", 0.0f))
                              .checked(config.crossfade_ms, 0.0f,
                                       sonare::acoustic::kMaxRirCrossfadeMs, "crossfadeMs");
    config.air_absorption_enabled =
        b(params, "airAbsorptionEnabled", config.air_absorption_enabled);
    // Same climate convention as effects.reverb.room above.
    config.air.temperature_c =
        ZeroIsDefault(f(params, "airTemperatureC", 0.0f)).or_default(config.air.temperature_c);
    config.air.humidity_percent = ZeroIsDefault(f(params, "airHumidityPercent", 0.0f))
                                      .or_default(config.air.humidity_percent);
    return make<effects::acoustic::RoomMorphProcessor>(config);
  }
#endif
  if (name == "effects.modulation.chorus") {
    effects::modulation::ChorusConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz);
    config.depth_ms = f(params, "depthMs", config.depth_ms);
    config.center_delay_ms = f(params, "centerDelayMs", config.center_delay_ms);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::modulation::Chorus>(config);
  }
  if (name == "effects.modulation.ensemble") {
    effects::modulation::EnsembleConfig config;
    config.rate_slow_hz = f(params, "rateSlowHz", config.rate_slow_hz);
    config.rate_fast_hz = f(params, "rateFastHz", config.rate_fast_hz);
    config.depth_slow_ms = f(params, "depthSlowMs", config.depth_slow_ms);
    config.depth_fast_ms = f(params, "depthFastMs", config.depth_fast_ms);
    config.center_delay_ms = f(params, "centerDelayMs", config.center_delay_ms);
    config.tone_hz = f(params, "toneHz", config.tone_hz);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::modulation::Ensemble>(config);
  }
  if (name == "effects.modulation.flanger") {
    effects::modulation::FlangerConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz);
    config.depth_ms = f(params, "depthMs", config.depth_ms);
    config.center_delay_ms = f(params, "centerDelayMs", config.center_delay_ms);
    config.feedback = f(params, "feedback", config.feedback);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::modulation::Flanger>(config);
  }
  if (name == "effects.modulation.phaser") {
    effects::modulation::PhaserConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz);
    config.min_hz = f(params, "minHz", config.min_hz);
    config.max_hz = f(params, "maxHz", config.max_hz);
    config.stages = detail::i(params, "stages", config.stages);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::modulation::Phaser>(config);
  }
  if (name == "effects.modulation.wah") {
    effects::modulation::WahConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz);
    config.min_hz = f(params, "minHz", config.min_hz);
    config.max_hz = f(params, "maxHz", config.max_hz);
    config.resonance = f(params, "resonance", config.resonance);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::modulation::Wah>(config);
  }
  if (name == "effects.modulation.autoWah") {
    effects::modulation::AutoWahConfig config;
    config.sensitivity = f(params, "sensitivity", config.sensitivity);
    config.min_hz = f(params, "minHz", config.min_hz);
    config.max_hz = f(params, "maxHz", config.max_hz);
    config.resonance = f(params, "resonance", config.resonance);
    config.attack_ms = f(params, "attackMs", config.attack_ms);
    config.release_ms = f(params, "releaseMs", config.release_ms);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::modulation::AutoWah>(config);
  }
  if (name == "effects.modulation.rotary") {
    effects::modulation::RotaryConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz);
    config.depth_ms = f(params, "depthMs", config.depth_ms);
    config.tremolo = f(params, "tremolo", config.tremolo);
    config.stereo_spread = f(params, "stereoSpread", config.stereo_spread);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::modulation::Rotary>(config);
  }
  if (name == "effects.modulation.ringModulator") {
    effects::modulation::RingModulatorConfig config;
    config.carrier_hz = f(params, "carrierHz", config.carrier_hz);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::modulation::RingModulator>(config);
  }
  if (name == "effects.modulation.pitchShifter") {
    effects::modulation::PitchShifterConfig config;
    config.semitones = f(params, "semitones", config.semitones);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::modulation::PitchShifter>(config);
  }
  if (name == "effects.delay.stereo") {
    effects::delay::StereoDelayConfig config;
    config.delay_time_l_ms = f(params, "delayTimeLMs", config.delay_time_l_ms);
    config.delay_time_r_ms = f(params, "delayTimeRMs", config.delay_time_r_ms);
    config.feedback = f(params, "feedback", config.feedback);
    config.ping_pong = f(params, "pingPong", config.ping_pong);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    return make<effects::delay::StereoDelay>(config);
  }
  return nullptr;
}
#endif  // SONARE_HAVE_FX

}  // namespace

namespace {

std::unique_ptr<Processor> build_insert(const std::string& name, const ParamMap& params,
                                        const std::string* json_params = nullptr) {
  if (auto p = build_dynamics(name, params)) return p;
  if (auto p = build_eq(name, params)) return p;
  if (auto p = build_saturation(name, params, json_params)) return p;
  if (auto p = build_spectral(name, params)) return p;
  if (auto p = build_stereo(name, params)) return p;
  if (auto p = build_maximizer(name, params)) return p;
  if (auto p = build_multiband(name, params)) return p;
#ifdef SONARE_HAVE_FX
  if (auto p = build_effects(name, params, json_params)) return p;
#endif
  return nullptr;
}

}  // namespace

std::unique_ptr<sonare::rt::ProcessorBase> make_insert(const std::string& name,
                                                       const std::string& json_params,
                                                       std::vector<std::string>* out_unknown_keys) {
  const std::vector<Param> param_list = parse_insert_params_json(json_params, name);
  const ParamMap params = detail::make_map(param_list);
  auto processor = build_insert(name, params, &json_params);
  // Only report ignored keys for a recognized processor: build_insert() probes
  // every key the processor reads (even absent ones), so any supplied key it
  // never touched took no effect. An unknown name is surfaced as a hard error by
  // the caller, not as an ignored-keys warning.
  if (out_unknown_keys != nullptr && processor != nullptr) {
    *out_unknown_keys = params.unprobed_keys();
  }
  return processor;
}

std::unique_ptr<sonare::rt::ProcessorBase> make_insert_from_params(
    const std::string& name, const std::vector<Param>& param_list) {
  const ParamMap params = detail::make_map(param_list);
  return build_insert(name, params);
}

std::unique_ptr<sonare::rt::ProcessorBase> make_insert_with_ir(const std::string& name,
                                                               const std::string& json_params,
                                                               const float* impulse_response,
                                                               int ir_num_samples) {
  if (ir_num_samples < 0 || (ir_num_samples > 0 && impulse_response == nullptr)) {
    throw SonareException(ErrorCode::InvalidParameter, "make_insert_with_ir: invalid IR");
  }
#ifdef SONARE_HAVE_FX
  if (name == "effects.reverb.convolution") {
    // Validate params for malformed JSON parity with make_insert(), then build a
    // real, IR-loaded convolution insert. load_ir() stores the IR and is safe to
    // call before prepare(); prepare() reapplies it to the FFT convolvers.
    const std::vector<Param> param_list = parse_insert_params_json(json_params, name);
    const ParamMap params = detail::make_map(param_list);
    effects::reverb::ConvolutionReverbConfig config;
    config.dry_wet = f(params, "dryWet", config.dry_wet);
    auto reverb = std::make_unique<effects::reverb::ConvolutionReverb>(config);
    reverb->load_ir(impulse_response, ir_num_samples);
    return reverb;
  }
#endif
  if (name == "saturation.ampSim") {
    // The amp sim's cabinet takes the same IR channel. load_cab_ir() stores it
    // and is safe before prepare(), which sizes the history from whatever is
    // loaded by then. An explicit IR wins over one carried in the param bag.
    auto amp = make_insert(name, json_params);
    if (amp != nullptr && ir_num_samples > 0) {
      static_cast<saturation::AmpSim*>(amp.get())->load_cab_ir(impulse_response, ir_num_samples);
    }
    return amp;
  }
  // Every other insert ignores the IR and falls back to the standard factory.
  return make_insert(name, json_params);
}

std::vector<std::string> insert_factory_names() {
  return {
      "dynamics.brickwallLimiter",
      "dynamics.compressor",
      "dynamics.deesser",
      "dynamics.expander",
      "dynamics.gate",
      "dynamics.limiter",
      "dynamics.parallelComp",
      "dynamics.sidechainRouter",
      "dynamics.duckingProcessor",
      "dynamics.transientShaper",
      "dynamics.upwardCompressor",
      "dynamics.upwardExpander",
      "dynamics.vocalRider",
      "eq.tilt",
      "eq.apiStyle",
      "eq.parametric",
      "eq.minimumPhase",
      "eq.linearPhase",
      "eq.dynamic",
      "eq.equalizer",
      "eq.pultec",
      "eq.cutFilter",
      "eq.bandPass",
      "eq.shelving",
      "eq.graphic",
      "eq.midSide",
      "saturation.tape",
      "saturation.exciter",
      "saturation.bitcrusher",
      "saturation.hardClipper",
      "saturation.softClipper",
      "saturation.waveshaper",
      "saturation.tube",
      "saturation.transformer",
      "saturation.multibandExciter",
      "saturation.ampSim",
      "spectral.airBand",
      "spectral.lowEndFocus",
      "spectral.presenceEnhancer",
      "spectral.spectralShaper",
      "stereo.autoPan",
      "stereo.haasEnhancer",
      "stereo.imager",
      "stereo.monoMaker",
      "stereo.phaseAlign",
      "stereo.stereoBalance",
      "maximizer.maximizer",
      "maximizer.truePeakLimiter",
      "maximizer.softKneeMax",
      "maximizer.adaptiveRelease",
      "multiband.compressor",
      "multiband.expander",
      "multiband.limiter",
      "multiband.imager",
      "multiband.saturation",
      "multiband.dynamicEq",
#ifdef SONARE_HAVE_FX
      // "effects.reverb.plate" is an alias for "effects.reverb.dattorro" (same
      // processor); both names are listed so either resolves via make_insert.
      "effects.reverb.plate",
      "effects.reverb.dattorro",
      "effects.reverb.fdn",
      "effects.reverb.velvet",
      "effects.reverb.convolution",
#ifdef SONARE_HAVE_ACOUSTIC
      "effects.reverb.room",
      "effects.acoustic.roomMorph",
#endif
      "effects.modulation.chorus",
      "effects.modulation.ensemble",
      "effects.modulation.flanger",
      "effects.modulation.phaser",
      "effects.modulation.wah",
      "effects.modulation.autoWah",
      "effects.modulation.rotary",
      "effects.modulation.ringModulator",
      "effects.modulation.pitchShifter",
      "effects.delay.stereo",
#endif
  };
}

std::vector<std::string> insert_param_names(const std::string& name) {
  // Build the processor against an empty param map: every config builder probes
  // the keys it reads (falling back to defaults when absent), so the probed set
  // is exactly the parameter names this processor consumes. The throwaway
  // processor is discarded immediately.
  ParamMap params;
  auto processor = build_insert(name, params);
  if (processor == nullptr) {
    return {};
  }
  const auto& probed = params.probed_keys();
  std::vector<std::string> names(probed.begin(), probed.end());
  std::sort(names.begin(), names.end());
  return names;
}

namespace {

// ---------------------------------------------------------------------------
// Parameter bound measurement
//
// Validation is hand-written per processor as a throw out of the config
// constructor, with no declared bounds interface, so the catalog MEASURES the
// bounds rather than mirroring them — the same discipline as the synth's clamp
// probe. A candidate goes through `build_insert` with every other parameter at
// its default, and whether construction throws is the answer. What is published
// is a hard constraint (outside it is an error, not a clip), not a recommended
// UI range: an unvalidated gain reports as unbounded.
//
// Measuring at the DEFAULT configuration makes a bound conservative rather than
// wrong: a validator coupling two parameters yields the interval one accepts
// while the other sits at its default (`maximizer.adaptiveRelease`), and a
// sample-rate-derived bound reflects the pre-prepare rate (the EQ band ceiling
// reads 24 kHz and rises once prepared higher).
//
// Measuring rather than declaring costs three things. The probe visits
// |value| <= kBoundProbeLimit, and anything beyond reports null, which reads the
// same as "no validation". A boundary is bisected and rounded, so an EXCLUSIVE
// bound publishes its limit value — a `> 0` validator reports `min: 0` and still
// rejects 0. Parameters the config builder never reads, and booleans, are not
// probed at all.
// ---------------------------------------------------------------------------

constexpr double kBoundProbeLimit = 1.0e6;
constexpr double kBoundZeroSnap = 1.0e-9;
constexpr double kBoundAbsoluteTolerance = 1.0e-12;
constexpr double kBoundRelativeTolerance = 1.0e-9;
constexpr int kBoundSignificantDigits = 6;
constexpr int kBoundMaxBisections = 64;

// Probe points, ascending. Decade-spaced so a boundary anywhere in the window is
// bracketed by two neighbours, with the sub-unit decades filled in because most
// audio parameters that ARE bounded are bounded at 0, 1 or 2.
const std::vector<double>& bound_probe_points() {
  static const std::vector<double> points = [] {
    static constexpr double kMagnitudes[] = {
        1.0e-6, 1.0e-3, 1.0e-2, 1.0e-1, 1.0, 10.0, 100.0, 1.0e3, 1.0e4, 1.0e5, kBoundProbeLimit};
    std::vector<double> values;
    values.reserve(2 * std::size(kMagnitudes) + 1);
    for (size_t index = std::size(kMagnitudes); index > 0; --index) {
      values.push_back(-kMagnitudes[index - 1]);
    }
    values.push_back(0.0);
    for (const double magnitude : kMagnitudes) values.push_back(magnitude);
    return values;
  }();
  return points;
}

// Whether construction accepts @p value for @p key with every other parameter
// at its default. An unknown name yields no processor and so accepts nothing,
// which keeps a caller from measuring bounds against a processor that does not
// exist in this build configuration.
bool insert_accepts(const std::string& name, const std::string& key, double value) {
  try {
    ParamMap probe;
    probe.stop_recording_declarations();
    probe[key] = value;
    return build_insert(name, probe) != nullptr;
  } catch (...) {
    return false;
  }
}

// Narrows a bracket whose ends disagree down to the extreme value construction
// still accepts. Integer-valued parameters bisect over integers: the flat
// surface rounds the value before it reaches the field, so a real-valued
// midpoint would report a bound halfway between two accepted settings.
double bisect_accepted_boundary(const std::string& name, const std::string& key, double rejected,
                                double accepted, bool integer_valued) {
  if (integer_valued) {
    long long accepted_step = std::llround(accepted);
    long long rejected_step = std::llround(rejected);
    while (std::llabs(rejected_step - accepted_step) > 1) {
      const long long middle = accepted_step + (rejected_step - accepted_step) / 2;
      if (insert_accepts(name, key, static_cast<double>(middle))) {
        accepted_step = middle;
      } else {
        rejected_step = middle;
      }
    }
    return static_cast<double>(accepted_step);
  }
  for (int step = 0; step < kBoundMaxBisections; ++step) {
    const double tolerance =
        std::max(kBoundAbsoluteTolerance, kBoundRelativeTolerance * std::fabs(accepted));
    if (std::fabs(accepted - rejected) <= tolerance) break;
    const double middle = 0.5 * (rejected + accepted);
    if (middle == rejected || middle == accepted) break;
    if (insert_accepts(name, key, middle)) {
      accepted = middle;
    } else {
      rejected = middle;
    }
  }
  return accepted;
}

double round_to_significant_digits(double value, int digits) {
  if (!std::isfinite(value) || value == 0.0) return value;
  const double exponent = std::ceil(std::log10(std::fabs(value)));
  const double scale = std::pow(10.0, static_cast<double>(digits) - exponent);
  if (!std::isfinite(scale) || scale == 0.0) return value;
  return std::round(value * scale) / scale;
}

double settle_measured_bound(double value) {
  const double rounded = round_to_significant_digits(value, kBoundSignificantDigits);
  return std::fabs(rounded) < kBoundZeroSnap ? 0.0 : rounded;
}

struct MeasuredBounds {
  bool has_min = false;
  double min = 0.0;
  bool has_max = false;
  double max = 0.0;
};

MeasuredBounds measure_bounds(const std::string& name, const std::string& key,
                              bool integer_valued) {
  const std::vector<double>& points = bound_probe_points();
  // Each side stops at the first probe point it accepts, so an unconstrained
  // parameter — the majority — costs exactly two builds: the window's two ends
  // both build and there is nothing to narrow. Walking inward is also what makes
  // the answer independent of any assumption about the accepted set's shape:
  // the extreme probe alone decides whether a bound is reported at all.
  size_t lowest_accepted = points.size();
  for (size_t index = 0; index < points.size(); ++index) {
    if (insert_accepts(name, key, points[index])) {
      lowest_accepted = index;
      break;
    }
  }
  // Nothing in the window builds. That is not a bound, it is a processor this
  // key cannot configure at all, so the catalog states no limit rather than an
  // empty interval no host could satisfy.
  if (lowest_accepted == points.size()) return {};
  size_t highest_accepted = lowest_accepted;
  for (size_t index = points.size(); index > lowest_accepted; --index) {
    if (insert_accepts(name, key, points[index - 1])) {
      highest_accepted = index - 1;
      break;
    }
  }

  MeasuredBounds bounds;
  if (lowest_accepted > 0) {
    bounds.has_min = true;
    bounds.min = settle_measured_bound(bisect_accepted_boundary(
        name, key, points[lowest_accepted - 1], points[lowest_accepted], integer_valued));
  }
  if (highest_accepted + 1 < points.size()) {
    bounds.has_max = true;
    bounds.max = settle_measured_bound(bisect_accepted_boundary(
        name, key, points[highest_accepted + 1], points[highest_accepted], integer_valued));
  }
  return bounds;
}

// Renders a catalog number as JSON text. The precision is the shortest that
// round-trips through `float` — the storage nearly every mastering config field
// uses — widened so a value with an integer part never comes out in exponent
// form. Formatting is locale-independent because a host that switched
// LC_NUMERIC would otherwise emit a decimal comma into a JSON document.
std::string format_catalog_number(double value) {
  if (!std::isfinite(value)) return "null";
  int integer_digits = 1;
  const double magnitude = std::fabs(value);
  if (magnitude >= 1.0) {
    integer_digits = static_cast<int>(std::floor(std::log10(magnitude))) + 1;
  }
  std::string widest;
  for (int precision = 1; precision <= std::numeric_limits<double>::max_digits10; ++precision) {
    widest = sonare::util::format_general(value, std::max(precision, integer_digits));
    double round_trip = 0.0;
    if (!sonare::util::parse_double(widest.data(), widest.data() + widest.size(), &round_trip)) {
      continue;
    }
    if (static_cast<float>(round_trip) == static_cast<float>(value)) return widest;
  }
  return widest;
}

// The unit a parameter's value carries, derived from the key's own suffix
// convention. Unlike `type` and the bounds this cannot be measured: the suffix
// IS the declaration.
std::string catalog_unit(const std::string& name, const std::string& key) {
  const auto ends_with = [&key](const char* suffix) {
    const size_t length = std::strlen(suffix);
    return key.size() >= length && key.compare(key.size() - length, length, suffix) == 0;
  };
  if ((name == "effects.reverb.plate" || name == "effects.reverb.dattorro") &&
      key == "modDepthSamples") {
    return "\"referenceSamples@29761Hz\"";
  }
  if (ends_with("Db")) return "\"dB\"";
  if (ends_with("Hz")) return "\"Hz\"";
  if (ends_with("Ms")) return "\"ms\"";
  if (ends_with("Samples")) return "\"samples\"";
  return "null";
}

std::string build_insert_param_info_json(const std::string& name) {
  // Build a throwaway processor (like insert_param_names) and read its published
  // JSON-key -> param_id descriptor table. rtSafe is derived per id so hosts can
  // tell which params accept realtime changes from the audio thread.
  ParamMap params;
  auto processor = build_insert(name, params);
  // The same build recorded, per key, the C++ type the config builder read it
  // as (ParamMap::note_kind, driven by the `b()` / `i()` accessors and by the
  // declared type of the SONARE_FIELDS_* destination field) and the fallback it
  // used for the key (ParamMap::note_default, which against this empty map is
  // the config struct's own field initializer). Reporting from those instead of
  // from the key's spelling is what keeps `type` honest — a boolean config field
  // whose key does not end in "Enabled", CompressorConfig::auto_makeup being the
  // standing example, was published as a number by the old suffix test — and is
  // what lets `default` be published at all without a second hand-written table.
  const auto& kinds = params.probed_kinds();
  const auto& defaults = params.probed_defaults();
  const auto& probed = params.probed_keys();
  std::string out = "[";
  if (processor != nullptr) {
    const auto descriptors = processor->parameter_descriptors();
    for (size_t index = 0; index < descriptors.size(); ++index) {
      const std::string& key = descriptors[index].key;
      if (index > 0) out += ',';
      out += "{\"name\":\"";
      out += key;
      out += "\",\"id\":";
      out += std::to_string(descriptors[index].id);
      out += ",\"rtSafe\":";
      out += processor->parameter_is_realtime_safe(descriptors[index].id) ? "true" : "false";
      out += ",\"type\":\"";
      // A descriptor key the builder never probed has no declared field to read
      // a type from; those are numeric automation targets, which is also the
      // catalog's neutral value.
      const auto kind = kinds.find(key);
      const ParamKind param_kind = kind != kinds.end() ? kind->second : ParamKind::Number;
      out += param_kind == ParamKind::Boolean ? "boolean" : "number";
      out += "\",";

      const bool construction_reads_key = probed.find(key) != probed.end();
      const MeasuredBounds bounds =
          construction_reads_key && param_kind != ParamKind::Boolean
              ? measure_bounds(name, key, param_kind == ParamKind::Integer)
              : MeasuredBounds{};
      out += "\"min\":";
      out += bounds.has_min ? format_catalog_number(bounds.min) : "null";
      out += ",\"max\":";
      out += bounds.has_max ? format_catalog_number(bounds.max) : "null";

      out += ",\"default\":";
      const auto fallback = defaults.find(key);
      if (fallback == defaults.end() || fallback->second.ambiguous) {
        out += "null";
      } else if (param_kind == ParamKind::Boolean) {
        out += fallback->second.value != 0.0 ? "true" : "false";
      } else {
        out += format_catalog_number(fallback->second.value);
      }

      out += ",\"unit\":";
      out += catalog_unit(name, key);
      out += '}';
    }
  }
  out += ']';
  return out;
}

}  // namespace

std::string insert_param_info_json(const std::string& name) {
  // Measuring the bounds costs one processor construction per probe point, so a
  // repeat query — and the capability catalog, which asks for every insert —
  // reads a memo instead of probing again. Thread-local because the C ABI's own
  // catalog buffers already are, and because the memo is pure derived data that
  // is cheaper to recompute per thread than to guard.
  static thread_local std::unordered_map<std::string, std::string> memo;
  const auto cached = memo.find(name);
  if (cached != memo.end()) return cached->second;
  return memo.emplace(name, build_insert_param_info_json(name)).first->second;
}

}  // namespace sonare::mastering::api
