#include "mastering/api/insert_factory.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/insert_construction.h"
#include "mastering/api/legacy_ids.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/param_field_tables.h"
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
#include "mastering/eq/band_pass.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/passive_tube.h"
#include "mastering/eq/shelving.h"
#include "mastering/eq/stepped_proportional_q.h"
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
#include "mastering/repair/decrackle_streaming.h"
#include "mastering/repair/dehum_streaming.h"
#include "mastering/repair/denoise_streaming.h"
#include "mastering/repair/dereverb_streaming.h"
#include "mastering/saturation/amp_presets.h"
#include "mastering/saturation/amp_sim.h"
#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/hard_clipper.h"
#include "mastering/saturation/multiband_exciter.h"
#include "mastering/saturation/pedal.h"
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
#include "mastering/stereo/binaural_panner.h"
#include "mastering/stereo/haas_enhancer.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "mastering/stereo/phase_align.h"
#include "mastering/stereo/stereo_balance.h"
#include "mastering/utility/gain.h"
#include "util/base64.h"
#include "util/exception.h"
#include "util/json_budget.h"
#include "util/resource_limits.h"

#ifdef SONARE_WITH_VOICE_CHANGER
#include "editing/voice_changer/voice_changer_insert.h"
#endif

#ifdef SONARE_HAVE_FX
#include <algorithm>
#include <cmath>

#include "effects/common/control_ranges.h"
#include "effects/common/field_domain.h"
#include "effects/delay/stereo_delay.h"
#include "effects/filter/vowel_filter.h"
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
#include "midi/synth/gs_efx.h"
#ifdef SONARE_HAVE_ACOUSTIC
#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "effects/acoustic/room_morph.h"
#include "effects/reverb/room_reverb.h"
#include "util/insertion_sort.h"
#include "util/number_format.h"
#include "util/zero_is_default.h"
#endif
#endif

namespace sonare::mastering::api {
namespace {

using detail::b;
using detail::build_insert;
using detail::compressor_config;
using detail::crossover_config;
using detail::display_range;
using detail::f;
using detail::kCents;
using detail::kCount;
using detail::kDb;
using detail::kDbfs;
using detail::kDegrees;
using detail::kDegreesCelsius;
using detail::kHz;
using detail::kHzLog;
using detail::kMeters;
using detail::kMs;
using detail::kMsLog;
using detail::kNone;
using detail::kPercent;
using detail::kRatio;
using detail::kSamples;
using detail::kSeconds;
using detail::kSecondsLog;
using detail::kSemitones;
using detail::limiter_config;
using detail::ParamKind;
using detail::ParamMap;
using sonare::util::json::Value;

// Admits @p json_params as a JSON object under @p limits, or returns nullptr
// into @p out_root for an empty (absent) document -- the processor's own
// defaults apply and nothing was allocated to admit it. Insert params arrive
// as a string lifted out of an already-admitted project document, so the
// fragment is parsed under that document's own budget rather than none: a
// string inside a document cannot be larger than the document.
const Value* admit_insert_params(const std::string& json_params,
                                 const sonare::resource::ProjectImportResourceLimits& limits,
                                 Value* out_root) {
  if (json_params.empty()) return nullptr;
  try {
    // Strict: insert params are a flat map of `{name: value}` and a duplicate
    // key would silently shadow the earlier value, which is almost certainly a
    // caller bug worth surfacing.
    *out_root = sonare::util::json::admit_strict(json_params, limits);
  } catch (const sonare::util::json::JsonResourceError& e) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        std::string("make_insert: json_params exceeds the JSON parse budget (") + e.what() + ")");
  } catch (const sonare::util::json::JsonError& e) {
    throw SonareException(ErrorCode::InvalidParameter, std::string("make_insert: ") + e.what());
  } catch (const std::invalid_argument& e) {
    throw SonareException(ErrorCode::InvalidParameter, std::string("make_insert: ") + e.what());
  }
  if (!out_root->is_object()) {
    throw SonareException(ErrorCode::InvalidParameter, "expected JSON object");
  }
  return out_root;
}

// Decodes a little-endian f32 array carried as base64 under @p key. Two inserts
// take an IR this way (the convolution reverb and the amp sim's cabinet), so the
// key is a parameter rather than being baked in. Not FX-gated: the amp sim ships
// in every configuration.
std::vector<float> read_ir_f32_base64_key(const Value* root, const char* key) {
  if (root == nullptr) return {};
  const auto* value = root->find(key);
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

// Reads a string-valued key from the admitted params document. The flat ParamMap
// holds doubles, so anything that is not a number has to be fetched this way.
// Absent -> empty; present but not a string -> a caller error worth surfacing.
std::string read_string_key(const Value* root, const char* key) {
  if (root == nullptr) return {};
  const auto* value = root->find(key);
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

std::vector<Param> insert_params_from_root(const Value* root, const std::string& insert_name) {
  if (root == nullptr) return {};
  const bool allow_acoustic_material_arrays = insert_name == "effects.acoustic.roomMorph";
  std::vector<Param> params;
  params.reserve(root->as_object().size());
  for (const auto& [key, value] : root->as_object()) {
    if (value.is_bool()) {
      params.push_back(Param{key, value.as_bool() ? 1.0 : 0.0});
    } else if (value.is_number()) {
      params.push_back(Param{key, value.as_number()});
    } else if (allow_acoustic_material_arrays &&
               (key == "bandAbsorption" || key == "bandScattering") && value.is_array()) {
      // The flat ParamMap cannot carry an array. Acoustic room-morph consumes
      // these two options from the admitted JSON object in build_effects();
      // accepting them here keeps the generic JSON validation layer from
      // rejecting an option that the acoustic facade already supports.
      continue;
    } else if (value.is_string() && insert_reads_json_string_key(insert_name, key)) {
      // A named rig (the discrete topology/tube/cab/capsule switches no numeric
      // param can reach) or a base64 impulse response. The flat ParamMap holds
      // doubles only, so the insert that owns the key reads it from the same
      // admitted document; accepting it here keeps the generic validation layer
      // from rejecting a param the insert supports. Every OTHER insert falls
      // through to the rejection below, so a key aimed at the wrong processor is
      // reported instead of dropped.
      continue;
    } else {
      throw SonareException(ErrorCode::InvalidParameter,
                            "JSON params values must be numbers or booleans");
    }
  }
  return params;
}

using Processor = sonare::rt::ProcessorBase;

// -ln(1000): the tank amplitude reaches -60 dB after the decay time.
constexpr float kNegLn1000 = -6.907755f;

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

std::unique_ptr<Processor> build_eq(const std::string& name, const ParamMap& params,
                                    double sample_rate) {
  if (name == "eq.tilt") {
    auto p = std::make_unique<eq::TiltEq>();
    detail::configure_tilt(*p, params);
    return p;
  }
  if (name == "eq.steppedProportionalQ") {
    auto p = std::make_unique<eq::SteppedProportionalQEq>();
    detail::configure_stepped_proportional_q(*p, params);
    return p;
  }
  if (name == "eq.parametric") {
    auto p = std::make_unique<eq::ParametricEq>();
    prepare_for_build_rate(*p, sample_rate);
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
    prepare_for_build_rate(*p, sample_rate);
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
  if (name == "eq.passiveTube") {
    auto p = std::make_unique<eq::PassiveTubeEq>();
    detail::configure_passive_tube(*p, params);
    return p;
  }
  if (name == "eq.cutFilter") {
    auto p = std::make_unique<eq::CutFilter>();
    detail::configure_cut_filter(*p, params);
    return p;
  }
  if (name == "eq.bandPass") {
    auto p = std::make_unique<eq::BandPassEq>();
    prepare_for_build_rate(*p, sample_rate);
    detail::configure_band_pass(*p, params);
    return p;
  }
  if (name == "eq.shelving") {
    auto p = std::make_unique<eq::ShelvingEq>();
    prepare_for_build_rate(*p, sample_rate);
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
    prepare_for_build_rate(*p, sample_rate);
    detail::configure_mid_side(*p, params);
    return p;
  }
  return nullptr;
}

std::unique_ptr<Processor> build_saturation(const std::string& name, const ParamMap& params,
                                            const Value* json_root) {
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
  if (name == "saturation.overdrive") {
    return make<saturation::Overdrive>(detail::overdrive_config(params));
  }
  if (name == "saturation.distortion") {
    return make<saturation::Distortion>(detail::distortion_config(params));
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
    detail::note_side_channel_key(params, "preset", ParamKind::String);
    detail::note_side_channel_key(params, "cabIrF32Base64", ParamKind::String);
    saturation::AmpSimConfig base;
    const std::string preset = read_string_key(json_root, "preset");
    if (!preset.empty()) {
      base = saturation::amp_preset_config(saturation::amp_preset_from_string(preset));
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
    const double ir_rate = static_cast<double>(detail::f(params, "cabIrSampleRate", 0.0f, kHz));
    // A cabinet can also be SYNTHESIZED rather than supplied, which is how a
    // caller gets an IR cab without sourcing a recording. The spec follows the
    // cabinet and mic already configured above, so there is no second set of
    // controls to keep in step; `cabIrDrivers` is the one thing the analytic
    // chain has no equivalent for — whether the cabinet's other drivers are
    // summed, which is the whole difference between an IR and an EQ curve.
    detail::apply_amp_cab_ir(params, *static_cast<saturation::AmpSim*>(amp.get()));
    const std::vector<float> ir = read_ir_f32_base64_key(json_root, "cabIrF32Base64");
    if (!ir.empty()) {
      // A supplied capture wins over a generated cabinet.
      static_cast<saturation::AmpSim*>(amp.get())->load_cab_ir(ir, ir_rate);
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
  if (name == "stereo.binaural") {
    return make<stereo::BinauralPanner>(detail::binaural_panner_config(params));
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

std::unique_ptr<Processor> build_utility(const std::string& name, const ParamMap& params) {
  if (name == "utility.gain") {
    return make<utility::Gain>(detail::gain_config(params));
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
// Field coverage for the reverbs, which the catalog test cannot ask about.
// Everywhere else an insert's construction keys stand one to one with its
// config's fields, and the test compares the two counts. These six break that
// correspondence on purpose -- `damping` and `hfDamping` write one field,
// `decaySec` derives another field's value, and a room's geometry is one field
// per three keys -- so the arity is pinned here instead, beside the builders
// that have to wire a new field.
//
// The nested members are pinned separately: a field added to a room's
// dimensions or to its climate does not move the arity of the config holding
// them, and would otherwise arrive unreachable with nothing red.
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(effects::reverb::DattorroReverbConfig, 12);
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(effects::reverb::FdnReverbConfig, 3);
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(effects::reverb::VelvetReverbConfig, 5);
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(effects::reverb::ConvolutionReverbConfig, 4);
#ifdef SONARE_HAVE_ACOUSTIC
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(effects::reverb::RoomReverbConfig, 11);
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(effects::acoustic::RoomMorphConfig, 13);
// Reached through a room's own keys: three each for the box and the two
// endpoints, two for the climate pair.
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(sonare::RoomDimensions, 3);
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(sonare::acoustic::Vec3, 3);
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(sonare::acoustic::AirAbsorption, 2);
// The morph target's room arrives through acoustic_room_from_json below, which
// resolves its walls by the material precedence the offline facade uses.
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(sonare::acoustic::ShoeboxRoom, 2);
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(sonare::acoustic::SourceListener, 2);

// Reads one material-band array out of the insert's JSON side-channel. Only the
// JSON shape is checked here; the coefficients themselves are validated by the
// core builder, on the same rule as every other surface.
std::vector<float> acoustic_material_bands(const Value* value, const char* key) {
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
                                                      const Value* json_root) {
  using namespace sonare::acoustic;

  const RoomDimensions dims{f(params, "lengthM", kDefaultRoomDims.length, kMeters),
                            f(params, "widthM", kDefaultRoomDims.width, kMeters),
                            f(params, "heightM", kDefaultRoomDims.height, kMeters)};
  // The precedence, the [0, 1] coefficient rejection and the band-wise
  // scattering come from the core builder the offline acoustic facade calls, so
  // an insert and a synthesizeRir call resolve the same option bag identically
  // — including rejecting an out-of-range scalar absorption rather than
  // clamping it, which is the one place these two used to disagree.
  WallMaterialRequest request;
  const int material_preset = detail::i(params, "materialPreset", 0, kNone);
  request.has_preset = material_preset_from_selector(material_preset, &request.preset);
  if (material_preset != 0 && !request.has_preset) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "materialPreset must be 0 (none) or a preset from 1 to 5");
  }
  request.absorption = f(params, "absorption", 0.2f, kNone);
  if (json_root != nullptr) {
    request.absorption_bands =
        acoustic_material_bands(json_root->find("bandAbsorption"), "bandAbsorption");
    request.scattering_bands =
        acoustic_material_bands(json_root->find("bandScattering"), "bandScattering");
  }
  return make_uniform_room(dims, request);
}
// Reads <prefix>X/Y/Z in that order, each defaulting to the matching field of @p defaults.
template <typename V>
V read_vec3(const ParamMap& params, const std::string& prefix, const V& defaults) {
  return {f(params, (prefix + "X").c_str(), defaults.x, kMeters),
          f(params, (prefix + "Y").c_str(), defaults.y, kMeters),
          f(params, (prefix + "Z").c_str(), defaults.z, kMeters)};
}

// Climate pair and receiver spacing; 0 selects the library value for each.
template <typename Config>
void read_air_and_spacing(const ParamMap& params, Config& config) {
  config.air.temperature_c = ZeroIsDefault(f(params, "airTemperatureC", 0.0f, kDegreesCelsius))
                                 .or_default(config.air.temperature_c);
  config.air.humidity_percent = ZeroIsDefault(f(params, "airHumidityPercent", 0.0f, kPercent))
                                    .or_default(config.air.humidity_percent);
  config.receiver_spacing_m = ZeroIsDefault(f(params, "receiverSpacingM", 0.0f, kMeters))
                                  .or_default(config.receiver_spacing_m);
}

// Source and listener sit inside the shoebox, so each coordinate is bounded by its dimension.
void note_room_containment(const ParamMap& params) {
  static constexpr const char* kAxes[3][3] = {{"sourceX", "listenerX", "lengthM"},
                                              {"sourceY", "listenerY", "widthM"},
                                              {"sourceZ", "listenerZ", "heightM"}};
  for (const auto& axis : kAxes) {
    params.note_depends(axis[0], axis[2], detail::Relation::Le);
    params.note_depends(axis[1], axis[2], detail::Relation::Le);
  }
}

// The receiver pair must stay inside the walls, so the plan view and the spacing bound one
// another beyond the containment above; the air pair is validated only while absorption is on.
void note_room_coupling(const ParamMap& params) {
  const auto couple = [&](const char* key, std::initializer_list<const char*> siblings) {
    for (const char* sibling : siblings) params.note_coupled(key, sibling);
  };
  couple("lengthM", {"listenerX", "listenerY", "receiverSpacingM", "sourceX", "sourceY"});
  couple("widthM", {"listenerX", "listenerY", "receiverSpacingM", "sourceX", "sourceY"});
  couple("listenerX", {"listenerY", "receiverSpacingM", "sourceX", "sourceY"});
  couple("listenerY", {"listenerX", "receiverSpacingM", "sourceX", "sourceY"});
  couple("receiverSpacingM", {"lengthM", "widthM", "listenerX", "listenerY", "sourceX", "sourceY"});
  couple("airTemperatureC", {"airAbsorptionEnabled"});
  couple("airHumidityPercent", {"airAbsorptionEnabled"});
}
#endif

std::unique_ptr<Processor> build_effects(const std::string& name, const ParamMap& params,
                                         const Value* json_root) {
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
    // Read only when present, so the key has a type and no fallback.
    detail::declare_number(params, "decaySec", kSecondsLog);
    detail::declare_number(params, "preDelayMs", kMs);
    if (params.find("decaySec") != params.end()) {
      constexpr float kTankLoopSeconds = 21589.0f / 29761.0f;  // both halves at ref rate
      constexpr float kMinDecaySeconds = 0.05f;
      // The tank feedback reaches the core's ceiling at this tail length.
      const float max_decay_sec =
          kNegLn1000 * kTankLoopSeconds / (4.0f * std::log(effects::common::kMaxFeedback));
      const float decay_sec = f(params, "decaySec", 5.0f, kSecondsLog);
      effects::common::require_in_domain(name.c_str(), "decaySec", decay_sec, kMinDecaySeconds,
                                         max_decay_sec);
      config.decay = std::exp(kNegLn1000 * kTankLoopSeconds / (4.0f * decay_sec));
    } else {
      config.decay = f(params, "decay", config.decay, kNone);
    }
    params.note_effective("decay", config.decay);
    config.damping = f(params, "damping", config.damping, kNone);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    // Figure-8 tank modulation (the plate's chorused tail); wire both fields so
    // they are reachable at construction, matching Velvet's full-field mapping.
    config.mod_rate_hz = f(params, "modRateHz", config.mod_rate_hz, kHz);
    // Samples at the tank's 29761 Hz reference rate; prepare() rescales them.
    config.mod_depth_samples = f(params, "modDepthSamples", config.mod_depth_samples, kSamples);
    // pre_delay_samples is defined at the reverb's reference rate (header
    // comment), so convert preDelayMs using kReferenceSampleRate; prepare()
    // rescales the resulting sample count to the working sample rate.
    if (params.find("preDelayMs") != params.end()) {
      config.pre_delay_samples = f(params, "preDelayMs", 0.0f, kMs) *
                                 static_cast<float>(DattorroReverb::kReferenceSampleRate) / 1000.0f;
    }
    params.note_effective("preDelayMs",
                          config.pre_delay_samples * 1000.0f /
                              static_cast<float>(DattorroReverb::kReferenceSampleRate));
    config.damping_hz = f(params, "dampingHz", config.damping_hz, kHz);
    config.gate_threshold_db = f(params, "gateThresholdDb", config.gate_threshold_db, kDb);
    config.gate_hold_ms = f(params, "gateHoldMs", config.gate_hold_ms, kMs);
    detail::read_field(params, "gateType", config.gate_type);
    // Also realtime id 10; every tank set's lines are prepared up front.
    detail::read_field(params, "character", config.character, kNone);
    detail::read_field(params, "mixLaw", config.mix_law);
    return make<DattorroReverb>(config);
  }
  if (name == "effects.reverb.fdn") {
    // Construction-time keys only: decaySec (seconds -> decay/10) and hfDamping
    // are convenience aliases parsed here. The RT-automatable parameter surface
    // (parameter_descriptors / set_parameter) exposes the unit-normalized keys
    // decay (0..1.5), damping (0..1) and dryWet instead — a host automating the
    // tail at audio rate must target those, not decaySec/hfDamping.
    FdnReverbConfig config;
    detail::declare_number(params, "decaySec", kSecondsLog);
    if (params.find("decaySec") != params.end()) {
      // decaySec is the approximate RT60 tail length in seconds. The FDN's
      // T60_lf = max(0.01, decay * 10), so decaySec maps to T60 directly via
      // decay = decaySec / 10, and the core's decay ceiling is the tail ceiling.
      constexpr float kDecayPerSecond = 0.1f;
      const float decay_sec = f(params, "decaySec", 5.5f, kSecondsLog);
      effects::common::require_in_domain(name.c_str(), "decaySec", decay_sec, 0.0f,
                                         FdnReverbConfig::kMaxDecay / kDecayPerSecond);
      config.decay = decay_sec * kDecayPerSecond;
    } else {
      config.decay = f(params, "decay", config.decay, kNone);
    }
    params.note_effective("decay", config.decay);
    config.hf_damping =
        f(params, "damping", f(params, "hfDamping", config.hf_damping, kNone), kNone);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    return make<FdnReverb>(config);
  }
  if (name == "effects.reverb.velvet") {
    VelvetReverbConfig config;
    config.decay = f(params, "decay", config.decay, kNone);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    detail::declare_number(params, "decaySec", kSecondsLog);
    if (params.find("decaySec") != params.end()) {
      // Velvet's effective T60 = reverb_time_s * (0.5 + decay). To make decaySec
      // mean approximately the same RT60 as FDN (decaySec == ~T60), set
      // reverb_time_s = decaySec / (0.5 + decay) so the product lands on decaySec.
      const float decay_factor = 0.5f + config.decay;
      const float decay_sec = f(params, "decaySec", config.reverb_time_s, kSecondsLog);
      // A decay outside its own domain is refused by the core; the derived tail
      // is only checked against the range it implies for a valid one.
      if (effects::common::in_domain(config.decay, 0.0f, 1.0f)) {
        effects::common::require_in_domain(
            name.c_str(), "decaySec", decay_sec,
            VelvetReverbConfig::kMinReverbTimeSeconds * decay_factor,
            VelvetReverbConfig::kMaxReverbTimeSeconds * decay_factor);
      }
      config.reverb_time_s = decay_sec / decay_factor;
    } else {
      config.reverb_time_s = f(params, "reverbTimeS", config.reverb_time_s, kSecondsLog);
    }
    params.note_effective("reverbTimeS", config.reverb_time_s);
    config.density_hz = f(params, "densityHz", config.density_hz, kHz);
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
    detail::note_side_channel_key(params, "irF32Base64", ParamKind::String);
    ConvolutionReverbConfig config;
    detail::declare_number(params, "decaySec", kSecondsLog);
    if (params.find("decaySec") != params.end()) {
      // An out-of-range request like {decaySec:40} is refused by key, not truncated
      // to the ceiling.
      config.decay_sec = f(params, "decaySec", config.decay_sec, kSecondsLog);
      effects::common::require_in_domain(name.c_str(), "decaySec", config.decay_sec, 0.0f,
                                         ConvolutionReverbConfig::kMaxDecaySeconds);
    }
    config.pre_delay_ms = f(params, "preDelayMs", config.pre_delay_ms, kMs);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.seed = detail::u32(params, "seed", config.seed, kNone);
    auto reverb = std::make_unique<ConvolutionReverb>(config);
    const std::vector<float> ir = read_ir_f32_base64_key(json_root, "irF32Base64");
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
    config.dims = {f(params, "lengthM", config.dims.length, kMeters),
                   f(params, "widthM", config.dims.width, kMeters),
                   f(params, "heightM", config.dims.height, kMeters)};
    config.source = read_vec3(params, "source", config.source);
    config.listener = read_vec3(params, "listener", config.listener);
    note_room_containment(params);
    note_room_coupling(params);
    // Rejected rather than clamped, matching sonare_synthesize_rir: the same
    // out-of-range absorption has to surface the same error whether the room is
    // built offline or as an insert.
    config.absorption = f(params, "absorption", config.absorption, kNone);
    sonare::acoustic::validate_material_coefficient(config.absorption, "absorption");
    config.ism_order = detail::i(params, "ismOrder", config.ism_order, kCount);
    config.seed = detail::u32(params, "seed", config.seed, kNone);
    config.max_seconds = f(params, "maxSeconds", config.max_seconds, kSeconds);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.air_absorption_enabled =
        b(params, "airAbsorptionEnabled", config.air_absorption_enabled);
    // The climate pair follows the acoustic ABI's "0 selects the library value"
    // rule (the ISO reference climate), so the same option bag resolves to the
    // same room here as on the offline facade.
    read_air_and_spacing(params, config);
    return make<RoomReverb>(config, sonare::acoustic::ReceiverLayout::MonoAndPair);
  }
  if (name == "effects.acoustic.roomMorph") {
    // Source-reverb tail suppression in front of a target-room convolution. The
    // target-room material follows the same precedence as the offline acoustic
    // facade: materialPreset > bandAbsorption > scalar absorption.
    effects::acoustic::RoomMorphConfig config;
    // Probe the array keys so insert_param_names() publishes all construction
    // options even though the flat ParamMap cannot hold their values.
    detail::note_side_channel_key(params, "bandAbsorption", ParamKind::Array);
    detail::note_side_channel_key(params, "bandScattering", ParamKind::Array);
    config.target = acoustic_room_from_json(params, json_root);
    const auto& source = sonare::acoustic::kDefaultPlacement.source;
    const auto& listener = sonare::acoustic::kDefaultPlacement.listener;
    config.placement.source = read_vec3(params, "source", source);
    config.placement.listener = read_vec3(params, "listener", listener);
    note_room_containment(params);
    note_room_coupling(params);
    config.source_tail_suppression =
        f(params, "sourceTailSuppression", config.source_tail_suppression, kNone);
    config.wet = f(params, "dryWet", config.wet, kNone);
    config.ism_order = detail::i(params, "ismOrder", config.ism_order, kCount);
    config.seed = detail::u32(params, "seed", config.seed, kNone);
    config.max_seconds = f(params, "maxSeconds", config.max_seconds, kSeconds);
    config.late_model = b(params, "preferEyring", true) ? sonare::acoustic::ReverbModel::Eyring
                                                        : sonare::acoustic::ReverbModel::Sabine;
    config.mixing_time_ms = f(params, "mixingTimeMs", config.mixing_time_ms, kMs);
    // A zero crossfade means "use the library default" on the offline acoustic
    // facade; preserve that normalization for the streaming insert as well.
    config.crossfade_ms = ZeroIsDefault(f(params, "crossfadeMs", 0.0f, kMs))
                              .checked(config.crossfade_ms, 0.0f,
                                       sonare::acoustic::kMaxRirCrossfadeMs, "crossfadeMs");
    config.air_absorption_enabled =
        b(params, "airAbsorptionEnabled", config.air_absorption_enabled);
    // Same climate convention as effects.reverb.room above.
    read_air_and_spacing(params, config);
    return make<effects::acoustic::RoomMorphProcessor>(
        config, sonare::acoustic::ReceiverLayout::MonoAndPair);
  }
#endif
  if (name == "effects.modulation.chorus") {
    effects::modulation::ChorusConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz, display_range(kHz, 0, 10));
    config.depth_ms = f(params, "depthMs", config.depth_ms, kMs);
    config.center_delay_ms = f(params, "centerDelayMs", config.center_delay_ms, kMs);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.pre_filter_hz = f(params, "preFilterHz", config.pre_filter_hz, kHz);
    // Enum selectors go through the field overlay, which refuses a fractional
    // value rather than rounding it onto a neighbouring mode.
    detail::read_field(params, "preFilterMode", config.pre_filter_mode);
    config.feedback = f(params, "feedback", config.feedback, kNone);
    config.phase_deg = f(params, "phaseDeg", config.phase_deg, kDegrees);
    detail::read_field(params, "interpolation", config.interpolation);
    detail::read_field(params, "mixLaw", config.mix_law);
    return make<effects::modulation::Chorus>(config);
  }
  if (name == "effects.modulation.ensemble") {
    effects::modulation::EnsembleConfig config;
    config.rate_slow_hz = f(params, "rateSlowHz", config.rate_slow_hz, kHz);
    config.rate_fast_hz = f(params, "rateFastHz", config.rate_fast_hz, kHz);
    config.depth_slow_ms = f(params, "depthSlowMs", config.depth_slow_ms, kMs);
    config.depth_fast_ms = f(params, "depthFastMs", config.depth_fast_ms, kMs);
    config.center_delay_ms = f(params, "centerDelayMs", config.center_delay_ms, kMs);
    config.tone_hz = f(params, "toneHz", config.tone_hz, kHzLog);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.rate_hz = f(params, "rateHz", config.rate_hz, kHz);
    config.pre_delay_dev_ms = f(params, "preDelayDevMs", config.pre_delay_dev_ms, kMs);
    config.depth_dev = f(params, "depthDev", config.depth_dev, kNone);
    config.pan_dev = f(params, "panDev", config.pan_dev, kNone);
    detail::read_field(params, "interpolation", config.interpolation);
    detail::read_field(params, "mixLaw", config.mix_law);
    return make<effects::modulation::Ensemble>(config);
  }
  if (name == "effects.modulation.flanger") {
    effects::modulation::FlangerConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz, display_range(kHz, 0, 10));
    config.depth_ms = f(params, "depthMs", config.depth_ms, kMs);
    config.center_delay_ms = f(params, "centerDelayMs", config.center_delay_ms, kMs);
    config.feedback = f(params, "feedback", config.feedback, kNone);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.pre_filter_hz = f(params, "preFilterHz", config.pre_filter_hz, kHz);
    detail::read_field(params, "preFilterMode", config.pre_filter_mode);
    config.phase_deg = f(params, "phaseDeg", config.phase_deg, kDegrees);
    config.step_rate_hz = f(params, "stepRateHz", config.step_rate_hz, kHz);
    detail::read_field(params, "interpolation", config.interpolation);
    detail::read_field(params, "mixLaw", config.mix_law);
    return make<effects::modulation::Flanger>(config);
  }
  if (name == "effects.modulation.phaser") {
    effects::modulation::PhaserConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz, kHz);
    config.min_hz = f(params, "minHz", config.min_hz, kHzLog);
    config.max_hz = f(params, "maxHz", config.max_hz, kHzLog);
    config.stages = detail::i(params, "stages", config.stages, kCount);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.feedback = f(params, "feedback", config.feedback, kNone);
    detail::read_field(params, "mixMode", config.mix_mode);
    config.depth = f(params, "depth", config.depth, kNone);
    return make<effects::modulation::Phaser>(config);
  }
  if (name == "effects.modulation.wah") {
    effects::modulation::WahConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz, kHz);
    config.min_hz = f(params, "minHz", config.min_hz, kHzLog);
    config.max_hz = f(params, "maxHz", config.max_hz, kHzLog);
    config.resonance = f(params, "resonance", config.resonance, kNone);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    detail::read_field(params, "filterType", config.filter_type);
    detail::read_field(params, "sweepLaw", config.sweep_law);
    return make<effects::modulation::Wah>(config);
  }
  if (name == "effects.modulation.autoWah") {
    effects::modulation::AutoWahConfig config;
    config.sensitivity = f(params, "sensitivity", config.sensitivity, kNone);
    config.min_hz = f(params, "minHz", config.min_hz, kHzLog);
    config.max_hz = f(params, "maxHz", config.max_hz, kHzLog);
    config.resonance = f(params, "resonance", config.resonance, kNone);
    config.attack_ms = f(params, "attackMs", config.attack_ms, kMsLog);
    config.release_ms = f(params, "releaseMs", config.release_ms, kMsLog);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.lfo_rate_hz = f(params, "lfoRateHz", config.lfo_rate_hz, kHz);
    config.lfo_depth = f(params, "lfoDepth", config.lfo_depth, kNone);
    detail::read_field(params, "filterType", config.filter_type);
    detail::read_field(params, "direction", config.direction);
    detail::read_field(params, "sweepLaw", config.sweep_law);
    return make<effects::modulation::AutoWah>(config);
  }
  if (name == "effects.filter.vowel") {
    effects::filter::VowelFilterConfig config;
    config.vowel = f(params, "vowel", config.vowel, kNone);
    config.accel_ms = f(params, "accelMs", config.accel_ms, kMs);
    config.drive = f(params, "drive", config.drive, kNone);
    config.drive_on = b(params, "driveOn", config.drive_on);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    return make<effects::filter::VowelFilter>(config);
  }
  if (name == "effects.modulation.rotary") {
    effects::modulation::RotaryConfig config;
    config.rate_hz = f(params, "rateHz", config.rate_hz, kHz);
    config.drum_rate_hz = f(params, "drumRateHz", config.drum_rate_hz, kHz);
    config.depth_ms = f(params, "depthMs", config.depth_ms, kMs);
    config.tremolo = f(params, "tremolo", config.tremolo, kNone);
    config.stereo_spread = f(params, "stereoSpread", config.stereo_spread, kNone);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.accel_tau_s = f(params, "accelTauS", config.accel_tau_s, kSeconds);
    config.decel_tau_s = f(params, "decelTauS", config.decel_tau_s, kSeconds);
    config.undershoot_hz = f(params, "undershootHz", config.undershoot_hz, kHz);
    config.drum_undershoot_hz = f(params, "drumUndershootHz", config.drum_undershoot_hz, kHz);
    config.horn_slow_hz = f(params, "hornSlowHz", config.horn_slow_hz, kHz);
    config.horn_fast_hz = f(params, "hornFastHz", config.horn_fast_hz, kHz);
    config.drum_slow_hz = f(params, "drumSlowHz", config.drum_slow_hz, kHz);
    config.drum_fast_hz = f(params, "drumFastHz", config.drum_fast_hz, kHz);
    config.speed = f(params, "speed", config.speed, kNone);
    config.horn_level_db = f(params, "hornLevelDb", config.horn_level_db, kDb);
    config.drum_level_db = f(params, "drumLevelDb", config.drum_level_db, kDb);
    detail::read_field(params, "interpolation", config.interpolation);
    detail::read_field(params, "model", config.model);
    return make<effects::modulation::Rotary>(config);
  }
  if (name == "effects.modulation.ringModulator") {
    effects::modulation::RingModulatorConfig config;
    config.carrier_hz = f(params, "carrierHz", config.carrier_hz, kHzLog);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    detail::read_field(params, "shape", config.shape);
    config.phase_deg = f(params, "phaseDeg", config.phase_deg, kDegrees);
    config.stereo_spread = f(params, "stereoSpread", config.stereo_spread, kNone);
    detail::read_field(params, "mixLaw", config.mix_law);
    return make<effects::modulation::RingModulator>(config);
  }
  if (name == "effects.modulation.pitchShifter") {
    effects::modulation::PitchShifterConfig config;
    config.semitones = f(params, "semitones", config.semitones, kSemitones);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.window_ms = f(params, "windowMs", config.window_ms, kMs);
    config.cents = f(params, "cents", config.cents, kCents);
    config.pan = f(params, "pan", config.pan, kNone);
    config.semitones2 = f(params, "semitones2", config.semitones2, kSemitones);
    config.cents2 = f(params, "cents2", config.cents2, kCents);
    config.level2 = f(params, "level2", config.level2, kNone);
    config.pan2 = f(params, "pan2", config.pan2, kNone);
    config.pre_delay_ms = f(params, "preDelayMs", config.pre_delay_ms, kMs);
    config.pre_delay2_ms = f(params, "preDelay2Ms", config.pre_delay2_ms, kMs);
    config.feedback = f(params, "feedback", config.feedback, kNone);
    detail::read_field(params, "mixLaw", config.mix_law);
    detail::read_field(params, "interpolation", config.interpolation);
    config.anti_alias = b(params, "antiAlias", config.anti_alias);
    return make<effects::modulation::PitchShifter>(config);
  }
  if (name == "effects.delay.stereo") {
    effects::delay::StereoDelayConfig config;
    config.delay_time_l_ms = f(params, "delayTimeLMs", config.delay_time_l_ms, kMs);
    config.delay_time_r_ms = f(params, "delayTimeRMs", config.delay_time_r_ms, kMs);
    config.feedback = f(params, "feedback", config.feedback, kNone);
    config.ping_pong = f(params, "pingPong", config.ping_pong, kNone);
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    config.damping_hz = f(params, "dampingHz", config.damping_hz, kHz);
    config.tap3_ms = f(params, "tap3Ms", config.tap3_ms, kMs);
    config.tap4_ms = f(params, "tap4Ms", config.tap4_ms, kMs);
    config.tap1_level_db = f(params, "tap1LevelDb", config.tap1_level_db, kDb);
    config.tap2_level_db = f(params, "tap2LevelDb", config.tap2_level_db, kDb);
    config.tap3_level_db = f(params, "tap3LevelDb", config.tap3_level_db, kDb);
    config.tap4_level_db = f(params, "tap4LevelDb", config.tap4_level_db, kDb);
    config.tap3_pan = f(params, "tap3Pan", config.tap3_pan, kNone);
    config.tap4_pan = f(params, "tap4Pan", config.tap4_pan, kNone);
    config.invert_l = b(params, "invertL", config.invert_l);
    config.invert_r = b(params, "invertR", config.invert_r);
    config.mod_rate_hz = f(params, "modRateHz", config.mod_rate_hz, kHz);
    config.mod_depth_ms = f(params, "modDepthMs", config.mod_depth_ms, kMs);
    config.mod_phase_deg = f(params, "modPhaseDeg", config.mod_phase_deg, kDegrees);
    config.glide_ms = f(params, "glideMs", config.glide_ms, kMs);
    detail::read_field(params, "crossMode", config.cross_mode);
    detail::read_field(params, "mixLaw", config.mix_law);
    detail::read_field(params, "interpolation", config.interpolation);
    return make<effects::delay::StereoDelay>(config);
  }
  return nullptr;
}

std::unique_ptr<Processor> build_gs_efx(const ParamMap& params) {
  using sonare::midi::synth::GsEfx;
  using sonare::midi::synth::GsEfxBuildPolicy;
  using sonare::midi::synth::GsEfxProcessor;
  using sonare::midi::synth::GsEfxRealization;

  // Require both TYPE bytes: defaulting a missing half would select a different effect.
  const auto type_msb = params.find("typeMsb");
  const auto type_lsb = params.find("typeLsb");
  if ((type_msb == params.end()) != (type_lsb == params.end())) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "effects.gsEfx requires typeMsb and typeLsb together");
  }
  const int msb = detail::i(params, "typeMsb", 0, kNone);
  const int lsb = detail::i(params, "typeLsb", 0, kNone);
  if (msb < 0 || msb > 127 || lsb < 0 || lsb > 127) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "effects.gsEfx type bytes must be in [0, 127]");
  }
  const int realization_value = detail::i(params, "realization", 0);
  if (params.records_declarations()) {
    params.note_choices("realization", {{"modern", 0}, {"classic", 1}});
  }
  if (realization_value < 0 || realization_value > 1) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "effects.gsEfx realization must be 0 (modern) or 1 (classic)");
  }

  GsEfx state;
  state.type = static_cast<uint16_t>((msb << 8) | lsb);
  state.type_msb = static_cast<uint8_t>(msb);
  state.assigned = state.type != 0;
  if (state.type != 0) {
    const auto* defaults = sonare::midi::synth::gs_efx_type_defaults(state.type);
    if (defaults == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "effects.gsEfx unsupported type " + std::to_string(state.type));
    }
    state.params = defaults->params;
  }

  for (std::size_t slot = 0; slot < state.params.size(); ++slot) {
    const std::string key = "byte" + std::to_string(slot);
    const int value = detail::i(params, key.c_str(), state.params[slot], kNone);
    if (value < 0 || value > 127) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "effects.gsEfx " + key + " must be in [0, 127]");
    }
    state.params[slot] = static_cast<uint8_t>(value);
    if (state.type != 0 && !sonare::midi::synth::gs_efx_parameter_takes(
                               state.type, static_cast<uint8_t>(slot), state.params[slot])) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "effects.gsEfx " + key + " is outside the GS state list");
    }
  }

  const auto realization = static_cast<GsEfxRealization>(realization_value);
  const sonare::midi::synth::GsEfxStageFactory stage_factory = [](std::string_view stage_name,
                                                                  std::string_view stage_params) {
    return make_insert(std::string(stage_name), std::string(stage_params));
  };
  return std::make_unique<GsEfxProcessor>(state, realization, stage_factory,
                                          GsEfxBuildPolicy::kRequireCompleteGraph);
}
#endif  // SONARE_HAVE_FX

#ifdef SONARE_WITH_VOICE_CHANGER
using sonare::editing::voice_changer::RealtimeVoiceChangerConfig;

// The config is three top-level gains, the formant mode, eight stage structs and seven voice
// transformer structs; every leaf field has a table row (the three harmony steps one each).
SONARE_ASSERT_EVERY_FIELD_IS_WIRED(RealtimeVoiceChangerConfig, 19);
static_assert(
    SONARE_FIELD_TABLE_SIZE(SONARE_FIELDS_VOICE_CHANGER) ==
        4 + detail::field_count<sonare::editing::voice_changer::StreamingRetuneConfig>() +
            detail::field_count<sonare::editing::voice_changer::StreamingFormantConfig>() +
            detail::field_count<sonare::editing::voice_changer::CharacterEqConfig>() +
            detail::field_count<sonare::editing::voice_changer::VoiceGateConfig>() +
            detail::field_count<sonare::editing::voice_changer::VoiceCompressorConfig>() +
            detail::field_count<sonare::editing::voice_changer::VoiceDeesserConfig>() +
            detail::field_count<sonare::editing::voice_changer::ReverbConfig>() +
            detail::field_count<sonare::editing::voice_changer::LimiterConfig>() +
            detail::field_count<sonare::editing::voice_changer::VoiceEngineConfig>() +
            detail::field_count<sonare::editing::voice_changer::PitchCorrectionConfig>() +
            detail::field_count<sonare::editing::voice_changer::HarmonyConfig>() +
            detail::field_count<sonare::editing::voice_changer::VocoderConfig>() +
            detail::field_count<sonare::editing::voice_changer::CharacterConfig>() +
            detail::field_count<sonare::editing::voice_changer::ReverbVariationConfig>() +
            detail::field_count<sonare::editing::voice_changer::MidiConfig>(),
    "SONARE_FIELDS_VOICE_CHANGER does not account for every voice changer field");

std::unique_ptr<Processor> build_voice(const std::string& name, const ParamMap& params) {
  if (name != "voice.changer") return nullptr;
  RealtimeVoiceChangerConfig config;
  SONARE_FIELDS_VOICE_CHANGER(SONARE_READ_FIELD)
  // Only the grain engine warps the formants ahead of the retune, so only there does the formant
  // mode bound the shift; psola resamples its grains and takes every shift in either mode.
  if (config.voice_engine.engine == sonare::editing::voice_changer::PitchEngine::Grain) {
    params.note_coupled("retuneSemitones", "formantMode");
  }
  return make<sonare::editing::voice_changer::VoiceChangerInsert>(config);
}
#endif  // SONARE_WITH_VOICE_CHANGER

}  // namespace

namespace {

// The denoise insert's settings. Its estimator defaults to Spp, the recursive one that tracks no
// minimum: the offline default, Quantile, ranks a whole signal and the insert refuses it by name.
repair::DenoiseClassicalConfig streaming_denoise_config(const ParamMap& params) {
  repair::DenoiseClassicalConfig config;
  config.noise_estimator = repair::DenoiseNoiseEstimator::Spp;
  SONARE_FIELDS_DENOISE_CLASSICAL(SONARE_READ_FIELD)
  detail::note_pair_order(params, "hopLength", detail::Relation::Le, "nFft", 0.5);
  return config;
}

// The repair stages with a causal setting. Each refuses an offline-only setting by name rather
// than substituting a streaming one, so a measured descriptor lists only what the insert takes.
std::unique_ptr<Processor> build_repair(const std::string& name, const ParamMap& params) {
  if (name == "repair.decrackle") {
    return make<repair::StreamingDecrackle>(detail::decrackle_config(params));
  }
  if (name == "repair.denoiseClassical") {
    return make<repair::StreamingDenoise>(streaming_denoise_config(params));
  }
  if (name == "repair.dehum") return make<repair::StreamingDehum>(detail::dehum_config(params));
  if (name == "repair.dereverbClassical") {
    return make<repair::StreamingDereverb>(detail::dereverb_classical_config(params));
  }
  return nullptr;
}

}  // namespace

namespace detail {

std::unique_ptr<Processor> build_insert(const std::string& name, const ParamMap& params,
                                        const Value* json_root, double sample_rate) {
  if (auto p = build_dynamics(name, params)) return p;
  if (auto p = build_eq(name, params, sample_rate)) return p;
  if (auto p = build_saturation(name, params, json_root)) return p;
  if (auto p = build_spectral(name, params)) return p;
  if (auto p = build_stereo(name, params)) return p;
  if (auto p = build_utility(name, params)) return p;
  if (auto p = build_maximizer(name, params)) return p;
  if (auto p = build_multiband(name, params)) return p;
  if (auto p = build_repair(name, params)) return p;
#ifdef SONARE_HAVE_FX
  if (name == "effects.gsEfx") return build_gs_efx(params);
  if (auto p = build_effects(name, params, json_root)) return p;
#endif
#ifdef SONARE_WITH_VOICE_CHANGER
  if (auto p = build_voice(name, params)) return p;
#endif
  return nullptr;
}

}  // namespace detail

namespace {

void attach_constructed_parameter_values(Processor* processor, const ParamMap& params) {
  if (processor == nullptr) return;
  std::vector<std::pair<unsigned int, float>> values;
  std::unordered_set<unsigned int> seen;
  for (const auto& descriptor : processor->parameter_descriptors()) {
    if (!processor->parameter_is_realtime_safe(descriptor.id)) continue;
    const auto it = params.effective_values().find(descriptor.key);
    if (it == params.effective_values().end() || !std::isfinite(it->second)) continue;
    if (!seen.insert(descriptor.id).second) continue;
    values.emplace_back(descriptor.id, static_cast<float>(it->second));
  }
  processor->set_constructed_parameter_values(std::move(values));
}

// Attaches the constructed values and, for a built processor, reports the keys it never read.
std::unique_ptr<Processor> finish_insert(std::unique_ptr<Processor> processor,
                                         const ParamMap& params,
                                         std::vector<std::string>* out_unknown_keys) {
  attach_constructed_parameter_values(processor.get(), params);
  if (out_unknown_keys != nullptr && processor != nullptr) {
    *out_unknown_keys = params.unprobed_keys();
  }
  return processor;
}

}  // namespace

void prepare_for_build_rate(sonare::rt::ProcessorBase& processor, double sample_rate) {
  if (sample_rate > 0.0) processor.prepare(sample_rate, kInsertProbeBlockSize);
}

std::unique_ptr<sonare::rt::ProcessorBase> make_insert(
    const std::string& requested_name, const std::string& json_params,
    std::vector<std::string>* out_unknown_keys,
    const sonare::resource::ProjectImportResourceLimits& limits, double build_sample_rate) {
  const std::string name = canonical_processor_id(requested_name);
  // One admission, one document: every reader below works off the same parsed
  // value, so the call's parse cost is the cost of this admission.
  Value root;
  const Value* json_root = admit_insert_params(json_params, limits, &root);
  const std::vector<Param> param_list = insert_params_from_root(json_root, name);
  const ParamMap params = detail::make_map(param_list);
  // Only a recognized processor reports ignored keys: build_insert() probes every key it reads
  // (even absent ones), so a supplied key it never touched took no effect.
  return finish_insert(build_insert(name, params, json_root, build_sample_rate), params,
                       out_unknown_keys);
}

std::unique_ptr<sonare::rt::ProcessorBase> make_insert_from_params(
    const std::string& requested_name, const std::vector<Param>& param_list,
    std::vector<std::string>* out_unknown_keys) {
  const std::string name = canonical_processor_id(requested_name);
  const ParamMap params = detail::make_map(param_list);
  return finish_insert(build_insert(name, params), params, out_unknown_keys);
}

std::unique_ptr<sonare::rt::ProcessorBase> make_insert_with_ir(
    const std::string& requested_name, const std::string& json_params,
    const float* impulse_response, int ir_num_samples,
    const sonare::resource::ProjectImportResourceLimits& limits) {
  const std::string name = canonical_processor_id(requested_name);
  if (ir_num_samples < 0 || (ir_num_samples > 0 && impulse_response == nullptr)) {
    throw SonareException(ErrorCode::InvalidParameter, "make_insert_with_ir: invalid IR");
  }
#ifdef SONARE_HAVE_FX
  if (name == "effects.reverb.convolution") {
    // Validate params for malformed JSON parity with make_insert(), then build a
    // real, IR-loaded convolution insert. load_ir() stores the IR and is safe to
    // call before prepare(); prepare() reapplies it to the FFT convolvers.
    Value root;
    const Value* json_root = admit_insert_params(json_params, limits, &root);
    const std::vector<Param> param_list = insert_params_from_root(json_root, name);
    const ParamMap params = detail::make_map(param_list);
    effects::reverb::ConvolutionReverbConfig config;
    config.dry_wet = f(params, "dryWet", config.dry_wet, kNone);
    auto reverb = std::make_unique<effects::reverb::ConvolutionReverb>(config);
    reverb->load_ir(impulse_response, ir_num_samples);
    attach_constructed_parameter_values(reverb.get(), params);
    return reverb;
  }
#endif
  if (name == "saturation.ampSim") {
    // The amp sim's cabinet takes the same IR channel. load_cab_ir() stores it
    // and is safe before prepare(), which sizes the history from whatever is
    // loaded by then. An explicit IR wins over one carried in the param bag.
    auto amp = make_insert(name, json_params, nullptr, limits);
    if (amp != nullptr && ir_num_samples > 0) {
      static_cast<saturation::AmpSim*>(amp.get())->load_cab_ir(impulse_response, ir_num_samples);
    }
    return amp;
  }
  // Every other insert ignores the IR and falls back to the standard factory.
  return make_insert(name, json_params, nullptr, limits);
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
      "eq.steppedProportionalQ",
      "eq.parametric",
      "eq.minimumPhase",
      "eq.linearPhase",
      "eq.dynamic",
      "eq.equalizer",
      "eq.passiveTube",
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
      "saturation.overdrive",
      "saturation.distortion",
      "saturation.ampSim",
      "spectral.airBand",
      "spectral.lowEndFocus",
      "spectral.presenceEnhancer",
      "spectral.spectralShaper",
      "stereo.autoPan",
      "stereo.binaural",
      "stereo.haasEnhancer",
      "stereo.imager",
      "stereo.monoMaker",
      "stereo.phaseAlign",
      "stereo.stereoBalance",
      "utility.gain",
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
      "repair.decrackle",
      "repair.denoiseClassical",
      "repair.dehum",
      "repair.dereverbClassical",
#ifdef SONARE_HAVE_FX
      "effects.gsEfx",
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
      "effects.filter.vowel",
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
#ifdef SONARE_WITH_VOICE_CHANGER
      "voice.changer",
#endif
  };
}

}  // namespace sonare::mastering::api
