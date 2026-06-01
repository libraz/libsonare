#include "mastering/api/insert_factory.h"

#include <memory>
#include <stdexcept>
#include <string>
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
#include "util/exception.h"
#include "util/json.h"

#ifdef SONARE_HAVE_FX
#include <algorithm>

#include "effects/reverb/convolution_reverb.h"
#include "effects/reverb/dattorro_reverb.h"
#include "effects/reverb/fdn_reverb.h"
#include "effects/reverb/velvet_reverb.h"
#endif

namespace sonare::mastering::api {
namespace {

using detail::b;
using detail::compressor_config;
using detail::crossover_config;
using detail::f;
using detail::limiter_config;
using detail::ParamMap;

std::vector<Param> parse_insert_params_json(const std::string& json_params) {
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

std::unique_ptr<Processor> build_saturation(const std::string& name, const ParamMap& params) {
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
  if (name == "maximizer.loudnessOptimize") {
    maximizer::TruePeakLimiterConfig config;
    config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
    config.oversample_factor = detail::i(params, "truePeakOversample", config.oversample_factor);
    return make<maximizer::TruePeakLimiter>(config);
  }
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
    return make<multiband::MultibandCompressor>(config);
  }
  if (name == "multiband.expander") {
    multiband::MultibandExpanderConfig config;
    config.crossover = crossover_config(params);
    return make<multiband::MultibandExpander>(config);
  }
  if (name == "multiband.limiter") {
    multiband::MultibandLimiterConfig config;
    config.crossover = crossover_config(params);
    return make<multiband::MultibandLimiter>(config);
  }
  if (name == "multiband.imager") {
    multiband::MultibandImagerConfig config;
    config.crossover = crossover_config(params);
    return make<multiband::MultibandImager>(config);
  }
  if (name == "multiband.saturation") {
    multiband::MultibandSaturationConfig config;
    config.crossover = crossover_config(params);
    return make<multiband::MultibandSaturation>(config);
  }
  if (name == "multiband.dynamicEq") {
    multiband::MultibandDynamicEqConfig config;
    config.crossover = crossover_config(params);
    return make<multiband::MultibandDynamicEq>(config);
  }
  return nullptr;
}

#ifdef SONARE_HAVE_FX
std::unique_ptr<Processor> build_effects(const std::string& name, const ParamMap& params) {
  using namespace sonare::effects::reverb;
  // "effects.reverb.plate" is an alias for "effects.reverb.dattorro": both names
  // construct the same DattorroReverb processor with identical parameters.
  if (name == "effects.reverb.plate" || name == "effects.reverb.dattorro") {
    DattorroReverbConfig config;
    // decaySec is a tail-length intent in seconds; DattorroReverbConfig.decay is
    // a normalized tank feedback clamped internally to [0, 0.98]. Map seconds to
    // feedback as decay/10 (~10 s -> max tail) and clamp into the documented range.
    if (params.find("decaySec") != params.end()) {
      config.decay = std::min(0.98f, std::max(0.0f, f(params, "decaySec", 5.0f) / 10.0f));
    } else {
      config.decay = f(params, "decay", config.decay);
    }
    config.damping = f(params, "damping", config.damping);
    config.dry_wet = f(params, "dryWet", config.dry_wet);
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
    FdnReverbConfig config;
    if (params.find("decaySec") != params.end()) {
      config.decay = std::max(0.0f, f(params, "decaySec", 5.5f) / 10.0f);
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
      config.reverb_time_s = std::max(0.0f, f(params, "decaySec", config.reverb_time_s));
    } else {
      config.reverb_time_s = f(params, "reverbTimeS", config.reverb_time_s);
    }
    config.density_hz = f(params, "densityHz", config.density_hz);
    config.enable_shelf = b(params, "enableShelf", config.enable_shelf);
    return make<VelvetReverb>(config);
  }
  if (name == "effects.reverb.convolution") {
    // Constructs as a near-passthrough until an IR is supplied. The IR cannot be
    // expressed through the scalar JSON params, so callers that need a working
    // convolution insert must use make_insert_with_ir(); via plain make_insert()
    // the convolver stays a passthrough. dryWet/decay are not part of
    // ConvolutionReverb's config, so no params are translated.
    return make<ConvolutionReverb>();
  }
  return nullptr;
}
#endif  // SONARE_HAVE_FX

}  // namespace

std::unique_ptr<sonare::rt::ProcessorBase> make_insert(const std::string& name,
                                                       const std::string& json_params) {
  const std::vector<Param> param_list = parse_insert_params_json(json_params);
  const ParamMap params = detail::make_map(param_list);

  if (auto p = build_dynamics(name, params)) return p;
  if (auto p = build_eq(name, params)) return p;
  if (auto p = build_saturation(name, params)) return p;
  if (auto p = build_spectral(name, params)) return p;
  if (auto p = build_stereo(name, params)) return p;
  if (auto p = build_maximizer(name, params)) return p;
  if (auto p = build_multiband(name, params)) return p;
#ifdef SONARE_HAVE_FX
  if (auto p = build_effects(name, params)) return p;
#endif
  return nullptr;
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
    (void)parse_insert_params_json(json_params);
    auto reverb = std::make_unique<effects::reverb::ConvolutionReverb>();
    reverb->load_ir(impulse_response, ir_num_samples);
    return reverb;
  }
#endif
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
      "maximizer.loudnessOptimize",
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
#endif
  };
}

}  // namespace sonare::mastering::api
