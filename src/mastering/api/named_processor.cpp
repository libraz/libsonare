#include "mastering/api/named_processor.h"

#include <cmath>
#include <sstream>

#include "core/audio.h"
#include "mastering/api/audio_utils.h"
#include "mastering/api/internal_processor_runner.h"
#include "mastering/api/processor_params.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/ducking_processor.h"
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
#include "mastering/eq/equalizer.h"
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/pultec.h"
#include "mastering/eq/shelving.h"
#include "mastering/eq/tilt.h"
#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"
#include "mastering/final/output_chain.h"
#include "mastering/match/ab_switcher.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_loudness.h"
#include "mastering/match/reference_spectrum.h"
#include "mastering/match/tonal_balance.h"
#include "mastering/maximizer/adaptive_release.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/maximizer.h"
#include "mastering/maximizer/soft_knee_max.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/multiband/multiband_dynamic_eq.h"
#include "mastering/multiband/multiband_expander.h"
#include "mastering/multiband/multiband_imager.h"
#include "mastering/multiband/multiband_limiter.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
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
#include "mastering/stereo/mono_compat_check.h"
#include "mastering/stereo/mono_maker.h"
#include "mastering/stereo/phase_align.h"
#include "mastering/stereo/stereo_balance.h"
#include "util/exception.h"

namespace sonare::mastering::api {
namespace {

using detail::b;
using detail::compressor_config;
using detail::configure_parametric;
using detail::crossover_config;
using detail::f;
using detail::i;
using detail::limiter_config;
using detail::make_map;
using detail::ParamMap;

// Run a processor over a mono buffer with latency compensation. The reported
// latency is captured into @p latency_samples for informational purposes
// (`MonoResult::latency_samples`); the returned audio in @p samples is already
// time-aligned (leading `latency` samples have been dropped and the tail has
// been flushed via zero-padding by the shared runner).
template <typename Processor>
void run_processor(Processor& processor, std::vector<float>& samples, int sample_rate,
                   int& latency_samples) {
  internal::run_processor_mono(processor, samples, sample_rate);
  latency_samples = processor.latency_samples();
}

// Stereo counterpart to run_processor(). Both channels are processed and
// trimmed together by the shared runner so they stay sample-accurately aligned.
template <typename Processor>
void run_processor_stereo(Processor& processor, std::vector<float>& left, std::vector<float>& right,
                          int sample_rate, int& latency_samples) {
  internal::run_processor_stereo(processor, left, right, sample_rate);
  latency_samples = processor.latency_samples();
}

float lufs_for(const std::vector<float>& samples, int sample_rate) {
  return common::measure_lufs(samples.data(), samples.size(), sample_rate);
}

void configure_processor(const std::string& name, const ParamMap& params,
                         std::vector<float>& samples, int sample_rate, int& latency_samples,
                         float& applied_gain_db) {
  if (name == "dynamics.brickwallLimiter") {
    dynamics::BrickwallLimiter p(detail::brickwall_limiter_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.compressor") {
    dynamics::Compressor p(compressor_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.deesser") {
    dynamics::DeEsser p(detail::deesser_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.expander") {
    dynamics::Expander p(detail::expander_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.gate") {
    dynamics::Gate p(detail::gate_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.limiter") {
    dynamics::Limiter p(limiter_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.parallelComp") {
    dynamics::ParallelComp p(detail::parallel_comp_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.sidechainRouter") {
    dynamics::SidechainRouter p(detail::sidechain_router_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.duckingProcessor") {
    dynamics::DuckingProcessor p(detail::ducking_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.transientShaper") {
    dynamics::TransientShaper p(detail::transient_shaper_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.upwardCompressor") {
    dynamics::UpwardCompressor p(detail::upward_compressor_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.upwardExpander") {
    dynamics::UpwardExpander p(detail::upward_expander_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "dynamics.vocalRider") {
    dynamics::VocalRider p(detail::vocal_rider_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.tilt") {
    eq::TiltEq p;
    detail::configure_tilt(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.apiStyle") {
    eq::ApiStyleEq p;
    detail::configure_api_style(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.parametric") {
    eq::ParametricEq p;
    configure_parametric(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.equalizer") {
    eq::EqualizerProcessor p(detail::equalizer_config(params, 1));
    detail::configure_equalizer(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.minimumPhase") {
    eq::MinimumPhaseEq p;
    detail::configure_minimum_phase(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.linearPhase") {
    eq::LinearPhaseEq p(detail::linear_phase_config(params));
    detail::configure_linear_phase_bands(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.dynamic") {
    eq::DynamicEq p;
    detail::configure_dynamic_eq_bands(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.pultec") {
    eq::PultecEq p;
    detail::configure_pultec(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.cutFilter") {
    eq::CutFilter p;
    detail::configure_cut_filter(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.bandPass") {
    eq::BandPassEq p;
    detail::configure_band_pass(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.shelving") {
    eq::ShelvingEq p;
    detail::configure_shelving(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "eq.graphic") {
    eq::GraphicEq p;
    detail::configure_graphic(p, params);
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.maximizer") {
    maximizer::Maximizer p(detail::maximizer_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.truePeakLimiter") {
    maximizer::TruePeakLimiter p(detail::true_peak_limiter_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.softKneeMax") {
    maximizer::SoftKneeMax p(detail::soft_knee_max_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.adaptiveRelease") {
    maximizer::AdaptiveRelease p(detail::adaptive_release_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "maximizer.loudnessOptimize") {
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    maximizer::LoudnessOptimizeConfig config;
    config.target_lufs = f(params, "targetLufs", config.target_lufs);
    config.ceiling_db = f(params, "ceilingDb", config.ceiling_db);
    config.true_peak_oversample = i(params, "truePeakOversample", config.true_peak_oversample);
    auto result = maximizer::loudness_optimize(audio, config);
    samples.assign(result.audio.data(), result.audio.data() + result.audio.size());
    applied_gain_db += result.applied_gain_db;
  } else if (name == "saturation.tape") {
    saturation::Tape p(detail::tape_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.exciter") {
    saturation::Exciter p(detail::exciter_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.bitcrusher") {
    saturation::BitCrusher p(detail::bitcrusher_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.hardClipper") {
    saturation::HardClipper p(detail::hard_clipper_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.softClipper") {
    saturation::SoftClipper p(detail::soft_clipper_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.waveshaper") {
    saturation::Waveshaper p(detail::waveshaper_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.tube") {
    saturation::Tube p(detail::tube_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.transformer") {
    saturation::Transformer p(detail::transformer_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "saturation.multibandExciter") {
    saturation::MultibandExciter p(detail::multiband_exciter_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "spectral.airBand") {
    spectral::AirBand p(detail::air_band_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "spectral.lowEndFocus") {
    spectral::LowEndFocus p(detail::low_end_focus_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "spectral.presenceEnhancer") {
    spectral::PresenceEnhancer p(detail::presence_enhancer_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "spectral.spectralShaper") {
    spectral::SpectralShaper p(detail::spectral_shaper_config(params));
    run_processor(p, samples, sample_rate, latency_samples);
  } else if (name == "repair.declick") {
    repair::DeclickConfig config;
    config.threshold = f(params, "threshold", config.threshold);
    config.neighbor_ratio = f(params, "neighborRatio", config.neighbor_ratio);
    config.max_click_samples =
        static_cast<size_t>(i(params, "maxClickSamples", config.max_click_samples));
    config.lpc_order = i(params, "lpcOrder", config.lpc_order);
    config.residual_ratio = f(params, "residualRatio", config.residual_ratio);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::declick(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.declip") {
    repair::DeclipConfig config;
    config.clip_threshold = f(params, "clipThreshold", config.clip_threshold);
    config.lpc_order = i(params, "lpcOrder", config.lpc_order);
    config.iterations = i(params, "iterations", config.iterations);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::declip(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.decrackle") {
    repair::DecrackleConfig config;
    config.threshold = f(params, "threshold", config.threshold);
    config.mode = static_cast<repair::DecrackleMode>(i(params, "mode", 0));
    config.levels = i(params, "levels", config.levels);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::decrackle(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.dehum") {
    repair::DehumConfig config;
    config.fundamental_hz = f(params, "fundamentalHz", config.fundamental_hz);
    config.harmonics = i(params, "harmonics", config.harmonics);
    config.q = f(params, "q", config.q);
    config.adaptive = b(params, "adaptive", config.adaptive);
    config.search_range_hz = f(params, "searchRangeHz", config.search_range_hz);
    config.adaptation = f(params, "adaptation", config.adaptation);
    config.frame_size = i(params, "frameSize", config.frame_size);
    config.pll_bandwidth = f(params, "pllBandwidth", config.pll_bandwidth);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::dehum(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.denoiseClassical" || name == "repair.denoise") {
    repair::DenoiseClassicalConfig config;
    config.mode = static_cast<repair::DenoiseMode>(i(params, "mode", 0));
    config.noise_estimator =
        static_cast<repair::DenoiseNoiseEstimator>(i(params, "noiseEstimator", 0));
    config.n_fft = i(params, "nFft", config.n_fft);
    config.hop_length = i(params, "hopLength", config.hop_length);
    config.dd_alpha = f(params, "ddAlpha", config.dd_alpha);
    config.gain_floor = f(params, "gainFloor", config.gain_floor);
    config.over_subtraction = f(params, "overSubtraction", config.over_subtraction);
    config.spectral_floor = f(params, "spectralFloor", config.spectral_floor);
    config.noise_estimation_quantile =
        f(params, "noiseEstimationQuantile", config.noise_estimation_quantile);
    config.speech_presence_gain = b(params, "speechPresenceGain", config.speech_presence_gain);
    config.gain_smoothing = b(params, "gainSmoothing", config.gain_smoothing);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::denoise_classical(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.dereverbClassical") {
    repair::DereverbClassicalConfig config;
    config.threshold = f(params, "threshold", config.threshold);
    config.attenuation = f(params, "attenuation", config.attenuation);
    config.n_fft = i(params, "nFft", config.n_fft);
    config.hop_length = i(params, "hopLength", config.hop_length);
    config.t60_sec = f(params, "t60Sec", config.t60_sec);
    config.late_delay_ms = f(params, "lateDelayMs", config.late_delay_ms);
    config.over_subtraction = f(params, "overSubtraction", config.over_subtraction);
    config.spectral_floor = f(params, "spectralFloor", config.spectral_floor);
    config.wpe_enabled = b(params, "wpeEnabled", config.wpe_enabled);
    config.wpe_iterations = i(params, "wpeIterations", config.wpe_iterations);
    config.wpe_taps = i(params, "wpeTaps", config.wpe_taps);
    config.wpe_strength = f(params, "wpeStrength", config.wpe_strength);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::dereverb_classical(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "repair.trimSilence") {
    repair::TrimSilenceConfig config;
    config.threshold = f(params, "threshold", config.threshold);
    config.padding_samples =
        static_cast<size_t>(i(params, "paddingSamples", config.padding_samples));
    config.mode = static_cast<repair::TrimSilenceMode>(i(params, "mode", 0));
    config.gate_lufs = f(params, "gateLufs", config.gate_lufs);
    config.window_ms = f(params, "windowMs", config.window_ms);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = repair::trim_silence(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "final.bitDepth") {
    final::BitDepthConfig config;
    config.target_bits = i(params, "targetBits", config.target_bits);
    config.clamp = b(params, "clamp", config.clamp);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = final::bit_depth(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "final.dither") {
    final::DitherConfig config;
    config.type = static_cast<final::DitherType>(i(params, "type", 2));
    config.target_bits = i(params, "targetBits", config.target_bits);
    config.seed = static_cast<uint32_t>(i(params, "seed", config.seed));
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = final::dither(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else if (name == "final.outputChain") {
    final::OutputChainConfig config;
    config.target_bits = i(params, "targetBits", config.target_bits);
    config.dither_type = static_cast<final::DitherType>(i(params, "ditherType", 2));
    config.clamp = b(params, "clamp", config.clamp);
    auto audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
    auto out = final::output_chain(audio, config);
    samples.assign(out.data(), out.data() + out.size());
  } else {
    throw SonareException(ErrorCode::InvalidParameter, "unknown mastering processor: " + name);
  }
}

}  // namespace

MonoResult apply_named_processor(const std::string& name, const float* samples, std::size_t length,
                                 int sample_rate, const std::vector<Param>& params) {
  MonoResult result;
  result.samples.assign(samples, samples + length);
  result.sample_rate = sample_rate;
  result.input_lufs = lufs_for(result.samples, sample_rate);
  auto map = make_map(params);
  configure_processor(name, map, result.samples, sample_rate, result.latency_samples,
                      result.applied_gain_db);
  result.output_lufs = lufs_for(result.samples, sample_rate);
  return result;
}

StereoResult apply_named_processor_stereo(const std::string& name, const float* left,
                                          const float* right, std::size_t length, int sample_rate,
                                          const std::vector<Param>& params) {
  StereoResult result;
  result.left.assign(left, left + length);
  result.right.assign(right, right + length);
  result.sample_rate = sample_rate;
  result.input_lufs = lufs_for(detail::mono_mix(result.left, result.right), sample_rate);
  auto map = make_map(params);

  if (name == "stereo.autoPan") {
    stereo::AutoPan p(detail::auto_pan_config(map));
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.haasEnhancer") {
    stereo::HaasEnhancer p(detail::haas_enhancer_config(map));
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.imager") {
    stereo::Imager p(detail::imager_config(map));
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.monoMaker") {
    stereo::MonoMaker p(detail::mono_maker_config(map));
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.phaseAlign") {
    stereo::PhaseAlign p(detail::phase_align_config(map));
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "stereo.stereoBalance") {
    stereo::StereoBalance p(detail::stereo_balance_config(map));
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "eq.midSide") {
    eq::MidSideEq p;
    detail::configure_mid_side(p, map);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "eq.equalizer") {
    eq::EqualizerProcessor p(detail::equalizer_config(map, 2));
    detail::configure_equalizer(p, map);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.compressor") {
    multiband::MultibandCompressorConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandCompressor p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.expander") {
    multiband::MultibandExpanderConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandExpander p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.limiter") {
    multiband::MultibandLimiterConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandLimiter p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.imager") {
    multiband::MultibandImagerConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandImager p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.saturation") {
    multiband::MultibandSaturationConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandSaturation p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "multiband.dynamicEq") {
    multiband::MultibandDynamicEqConfig config;
    config.crossover = crossover_config(map);
    multiband::MultibandDynamicEq p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else if (name == "maximizer.loudnessOptimize") {
    const float current = lufs_for(detail::mono_mix(result.left, result.right), sample_rate);
    if (std::isfinite(current)) {
      const float gain_db = f(map, "targetLufs", -14.0f) - current;
      detail::apply_gain_db(result.left, result.right, gain_db);
      result.applied_gain_db += gain_db;
    }
    maximizer::TruePeakLimiterConfig config;
    config.ceiling_db = f(map, "ceilingDb", config.ceiling_db);
    config.oversample_factor = i(map, "truePeakOversample", config.oversample_factor);
    config.apply_gain_at_input_rate =
        b(map, "applyGainAtInputRate", config.apply_gain_at_input_rate);
    maximizer::TruePeakLimiter p(config);
    run_processor_stereo(p, result.left, result.right, sample_rate, result.latency_samples);
  } else {
    int left_latency = 0;
    int right_latency = 0;
    float left_gain_db = 0.0f;
    float right_gain_db = 0.0f;
    configure_processor(name, map, result.left, sample_rate, left_latency, left_gain_db);
    configure_processor(name, map, result.right, sample_rate, right_latency, right_gain_db);
    if (result.left.size() != result.right.size()) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "stereo processor produced mismatched channel lengths: " + name);
    }
    result.latency_samples = std::max(left_latency, right_latency);
    result.applied_gain_db += 0.5f * (left_gain_db + right_gain_db);
  }

  result.output_lufs = lufs_for(detail::mono_mix(result.left, result.right), sample_rate);
  return result;
}

MonoResult apply_named_pair_processor(const std::string& name, const float* source,
                                      const float* reference, std::size_t length, int sample_rate,
                                      const std::vector<Param>& params) {
  auto map = make_map(params);
  auto source_audio = Audio::from_buffer(source, length, sample_rate);
  auto reference_audio = Audio::from_buffer(reference, length, sample_rate);
  Audio out;
  if (name == "match.applyMatchEq") {
    ::sonare::mastering::match::MatchEqConfig match_config;
    match_config.max_bands = static_cast<size_t>(i(map, "maxBands", match_config.max_bands));
    match_config.max_gain_db = f(map, "maxGainDb", match_config.max_gain_db);
    match_config.min_frequency_hz = f(map, "minFrequencyHz", match_config.min_frequency_hz);
    match_config.max_frequency_hz = f(map, "maxFrequencyHz", match_config.max_frequency_hz);
    match_config.q = f(map, "q", match_config.q);
    match_config.smoothing_bins = i(map, "smoothingBins", match_config.smoothing_bins);
    ::sonare::mastering::match::MatchEqFirConfig fir_config;
    fir_config.fft_size = i(map, "fftSize", fir_config.fft_size);
    fir_config.kernel_size = i(map, "kernelSize", fir_config.kernel_size);
    fir_config.phase = static_cast<::sonare::mastering::match::MatchEqFirPhase>(i(map, "phase", 0));
    fir_config.partition_size = i(map, "partitionSize", fir_config.partition_size);
    auto source_spectrum = ::sonare::mastering::match::reference_spectrum(source_audio);
    auto reference_spectrum = ::sonare::mastering::match::reference_spectrum(reference_audio);
    out = ::sonare::mastering::match::apply_match_eq(source_audio, source_spectrum,
                                                     reference_spectrum, match_config, fir_config);
  } else if (name == "match.alignReferenceToSource") {
    out = ::sonare::mastering::match::align_reference_to_source(source_audio, reference_audio,
                                                                i(map, "maxAbsDelay", 4096));
  } else if (name == "match.abSwitch") {
    out = ::sonare::mastering::match::ab_switch(
        source_audio, reference_audio,
        static_cast<::sonare::mastering::match::ABSelection>(i(map, "selection", 0)));
  } else if (name == "match.abCrossfade") {
    out = ::sonare::mastering::match::ab_crossfade(source_audio, reference_audio,
                                                   f(map, "mix", 0.5f));
  } else {
    throw SonareException(ErrorCode::InvalidParameter, "unknown mastering pair processor: " + name);
  }

  MonoResult result;
  result.samples.assign(out.data(), out.data() + out.size());
  result.sample_rate = out.sample_rate();
  result.input_lufs = lufs_for(std::vector<float>(source, source + length), sample_rate);
  result.output_lufs = lufs_for(result.samples, result.sample_rate);
  return result;
}

std::string analyze_named_pair(const std::string& name, const float* source, const float* reference,
                               std::size_t length, int sample_rate,
                               const std::vector<Param>& params) {
  auto map = make_map(params);
  auto source_audio = Audio::from_buffer(source, length, sample_rate);
  auto reference_audio = Audio::from_buffer(reference, length, sample_rate);
  std::ostringstream json;
  json << "{";
  if (name == "match.referenceLoudness") {
    auto result = ::sonare::mastering::match::reference_loudness(source_audio, reference_audio);
    json << "\"sourceLufs\":" << result.source_lufs
         << ",\"referenceLufs\":" << result.reference_lufs
         << ",\"gainToMatchDb\":" << result.gain_to_match_db;
  } else if (name == "match.tonalBalance" || name == "match.tonalBalanceLogBands") {
    auto source_spectrum = ::sonare::mastering::match::reference_spectrum(source_audio);
    auto reference_spectrum = ::sonare::mastering::match::reference_spectrum(reference_audio);
    auto bands =
        name == "match.tonalBalance"
            ? ::sonare::mastering::match::tonal_balance(source_spectrum, reference_spectrum)
            : ::sonare::mastering::match::tonal_balance_log_bands(
                  source_spectrum, reference_spectrum, i(map, "bandsPerOctave", 3),
                  f(map, "lowHz", 20.0f), f(map, "highHz", 20000.0f));
    json << "\"bands\":[";
    for (size_t index = 0; index < bands.size(); ++index) {
      if (index > 0) json << ",";
      const auto& band = bands[index];
      json << "{\"lowHz\":" << band.low_hz << ",\"highHz\":" << band.high_hz
           << ",\"sourceDb\":" << band.source_db << ",\"referenceDb\":" << band.reference_db
           << ",\"deviationDb\":" << band.deviation_db << "}";
    }
    json << "]";
  } else if (name == "match.matchEqCurve") {
    ::sonare::mastering::match::MatchEqConfig config;
    config.max_bands = static_cast<size_t>(i(map, "maxBands", config.max_bands));
    config.max_gain_db = f(map, "maxGainDb", config.max_gain_db);
    config.min_frequency_hz = f(map, "minFrequencyHz", config.min_frequency_hz);
    config.max_frequency_hz = f(map, "maxFrequencyHz", config.max_frequency_hz);
    config.q = f(map, "q", config.q);
    config.smoothing_bins = i(map, "smoothingBins", config.smoothing_bins);
    auto curve = ::sonare::mastering::match::match_eq_curve(
        ::sonare::mastering::match::reference_spectrum(source_audio),
        ::sonare::mastering::match::reference_spectrum(reference_audio), config);
    json << "\"frequencies\":[";
    for (size_t index = 0; index < curve.frequencies.size(); ++index) {
      if (index > 0) json << ",";
      json << curve.frequencies[index];
    }
    json << "],\"gainDb\":[";
    for (size_t index = 0; index < curve.gain_db.size(); ++index) {
      if (index > 0) json << ",";
      json << curve.gain_db[index];
    }
    json << "]";
  } else if (name == "match.estimateReferenceDelaySamples") {
    const float delay = ::sonare::mastering::match::estimate_reference_delay_samples(
        source_audio, reference_audio, i(map, "maxAbsDelay", 4096));
    json << "\"delaySamples\":" << delay;
  } else {
    throw SonareException(ErrorCode::InvalidParameter, "unknown mastering pair analysis: " + name);
  }
  json << "}";
  return json.str();
}

std::string analyze_named_stereo(const std::string& name, const float* left, const float* right,
                                 std::size_t length, int sample_rate,
                                 const std::vector<Param>& params) {
  auto map = make_map(params);
  std::ostringstream json;
  json << "{";
  if (name == "stereo.monoCompatCheck") {
    auto result = ::sonare::mastering::stereo::mono_compat_check(
        left, right, length, f(map, "correlationThreshold", 0.0f));
    json << "\"correlation\":" << result.correlation << ",\"width\":" << result.width
         << ",\"monoPeak\":" << result.mono_peak << ",\"sideRms\":" << result.side_rms
         << ",\"likelyMonoCompatible\":" << (result.likely_mono_compatible ? "true" : "false");
  } else if (name == "stereo.monoCompatCheckLogBands") {
    auto bands = ::sonare::mastering::stereo::mono_compat_check_log_bands(
        left, right, length, sample_rate, i(map, "bandsPerOctave", 3), f(map, "lowHz", 20.0f),
        f(map, "highHz", 20000.0f));
    json << "\"bands\":[";
    for (size_t index = 0; index < bands.size(); ++index) {
      if (index > 0) json << ",";
      const auto& band = bands[index];
      json << "{\"lowHz\":" << band.low_hz << ",\"highHz\":" << band.high_hz
           << ",\"correlation\":" << band.correlation << ",\"sideRms\":" << band.side_rms << "}";
    }
    json << "]";
  } else {
    throw SonareException(ErrorCode::InvalidParameter,
                          "unknown mastering stereo analysis: " + name);
  }
  json << "}";
  return json.str();
}

}  // namespace sonare::mastering::api
