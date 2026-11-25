/// @file chain_streaming.cpp
/// @brief Streaming implementation of the high-level mastering chain.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "mastering/api/audio_utils.h"
#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/eq/tilt.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/repair/denoise_streaming.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/tape.h"
#include "mastering/spectral/air_band.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "rt/overflow_counter.h"
#include "rt/processor_base.h"
#include "rt/tail_budget.h"
#include "streaming/realtime_limits.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/json_schema.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

namespace sonare::mastering::api {
namespace {

static_assert(std::is_nothrow_move_assignable<MasteringChainConfig>::value,
              "validated config commit must not throw");

// Reject the chain stages that fundamentally require whole-signal buffering and
// therefore cannot run block-by-block. Loudness is handled separately because
// it can be approximated with a precomputed static gain (see the
// options-taking constructor below).
void reject_non_streaming_repair(const MasteringChainConfig& config) {
  const auto refuse_stage = [](bool enabled, const char* stage) {
    if (!enabled) return;
    throw SonareParameterRefusal::predicate(
        ErrorCode::NotSupported, stage,
        "is not supported for a streaming chain: it needs the whole signal")
        .as_caller_spelled();
  };
  refuse_stage(config.repair.declick.enabled, "repair.declick");
  refuse_stage(config.repair.declip.enabled, "repair.declip");
  refuse_stage(config.repair.decrackle.enabled, "repair.decrackle");
  refuse_stage(config.repair.dehum.enabled, "repair.dehum");
  refuse_stage(config.repair.dereverb.enabled, "repair.dereverb");
  // denoise runs block by block only with an estimator that is recursive in time; the default
  // ranks the whole signal, so it is refused by name rather than swapped behind the caller's back.
  if (config.repair.denoise.enabled &&
      !repair::streaming_supports(config.repair.denoise.config.noise_estimator)) {
    throw SonareParameterRefusal::predicate(
        ErrorCode::NotSupported, "repair.denoise.noiseEstimator",
        "quantile is not supported for a streaming chain: it ranks every frame of the whole "
        "signal by energy; spp, mcra and imcra track recursively")
        .as_caller_spelled();
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// StreamingMasteringChain
// ---------------------------------------------------------------------------

struct StreamingParameterTarget {
  std::string full_key;
  rt::ProcessorBase* processor = nullptr;
  unsigned int id = 0;
};

struct StreamingMasteringChain::Impl {
  std::vector<std::unique_ptr<rt::ProcessorBase>> processors;
  std::vector<StreamingParameterTarget> parameter_targets;
  double prepared_sample_rate = 0.0;
  // Optional final loudness limiter (gain + true-peak limit), present only when
  // the loudness stage is enabled and a precomputed static gain was supplied.
  // The static gain is applied in process_block() immediately before this
  // limiter, mirroring the offline chain's loudness stage (gain -> limiter).
  std::unique_ptr<rt::ProcessorBase> loudness_limiter;
  // Non-owning views of the stages above that substitute a non-finite sample.
  // ProcessorBase does not carry the count, so the stages that keep one are
  // recorded as prepare() builds them.
  std::vector<const mastering::maximizer::TruePeakLimiter*> substitution_sources;
  // Blocks in which some stage discarded its own state. Held here rather than on
  // the chain so the defaulted move stays defaulted: the counter owns an atomic
  // and is neither copyable nor movable, which would propagate to any owner
  // holding it by value.
  rt::NonFiniteDiscardCounter non_finite_discards;

  /// @brief Every stage's discard count added together, for the block delta in
  ///        process_prevalidated(). RT-safe: relaxed atomic loads only.
  /// @details A stage is owned here and reachable from outside only as a count,
  ///   so a discard inside one is observable nowhere unless the chain records
  ///   it. The sum answers "did any of them move", which is the question the
  ///   chain's own per-call count asks; it is never published as a count of its
  ///   own, and summing is safe only because of that -- a stage may be driven
  ///   several times per the chain's call. This is the opposite of
  ///   non_finite_substitution_count, which is a count of SAMPLES and therefore
  ///   sums across stages correctly.
  uint64_t stage_discard_sum() const noexcept {
    uint64_t total = 0;
    for (const auto& proc : processors) {
      if (proc) total += proc->non_finite_discard_count();
    }
    if (loudness_limiter) total += loudness_limiter->non_finite_discard_count();
    return total;
  }
};

void add_parameter_targets(std::vector<StreamingParameterTarget>& targets,
                           const std::string& stage_name, rt::ProcessorBase* processor,
                           bool loudness_stage = false) {
  const auto descriptors = processor->parameter_descriptors();
  for (const auto& descriptor : descriptors) {
    std::string full_key;
    if (loudness_stage) {
      // The realized stage is loudness.optimize; its public keys are loudness.*.
      full_key = "loudness." + descriptor.key;
    } else if (stage_name == "dynamics.multibandComp") {
      // Every band descriptor (`band<i>.<field>`) is a target under the chain's
      // `bands.<i>.<field>` key.
      full_key = stage_name + "." + descriptor.key;
      constexpr std::string_view kBandHead = "band";
      const std::string_view band_key = descriptor.key;
      const std::size_t dot = band_key.find('.');
      if (band_key.substr(0, kBandHead.size()) == kBandHead && dot != std::string_view::npos) {
        full_key = stage_name + ".bands." +
                   std::string(band_key.substr(kBandHead.size(), dot - kBandHead.size())) + "." +
                   std::string(band_key.substr(dot + 1));
      }
    } else {
      full_key = stage_name + "." + descriptor.key;
    }
    targets.push_back({std::move(full_key), processor, descriptor.id});
  }
}

bool same_stage_enablement(const MasteringChainConfig& left,
                           const MasteringChainConfig& right) noexcept {
  return left.repair.declick.enabled == right.repair.declick.enabled &&
         left.repair.declip.enabled == right.repair.declip.enabled &&
         left.repair.decrackle.enabled == right.repair.decrackle.enabled &&
         left.repair.dehum.enabled == right.repair.dehum.enabled &&
         left.repair.dereverb.enabled == right.repair.dereverb.enabled &&
         left.repair.denoise.enabled == right.repair.denoise.enabled &&
         left.eq.tilt.enabled == right.eq.tilt.enabled &&
         left.dynamics.deesser.enabled == right.dynamics.deesser.enabled &&
         left.dynamics.transient_shaper.enabled == right.dynamics.transient_shaper.enabled &&
         left.dynamics.compressor.enabled == right.dynamics.compressor.enabled &&
         left.dynamics.multiband_comp.enabled == right.dynamics.multiband_comp.enabled &&
         left.saturation.tape.enabled == right.saturation.tape.enabled &&
         left.saturation.exciter.enabled == right.saturation.exciter.enabled &&
         left.spectral.air_band.enabled == right.spectral.air_band.enabled &&
         left.stereo.imager.enabled == right.stereo.imager.enabled &&
         left.stereo.mono_maker.enabled == right.stereo.mono_maker.enabled &&
         left.maximizer.true_peak_limiter.enabled == right.maximizer.true_peak_limiter.enabled &&
         left.loudness.enabled == right.loudness.enabled;
}

// Highest filter frequency a prepared processor accepts: just under Nyquist.
float frequency_ceiling_hz(double sample_rate) { return static_cast<float>(sample_rate * 0.49); }

void validate_realtime_parameter_domain(const std::string& key, float value, double sample_rate) {
  // Bounds that are tighter than, or depend on the prepared rate beyond, what the
  // processor's own live setter refuses. Anything the processor refuses surfaces
  // from its set_parameter.
  if (key == "dynamics.deesser.frequencyHz") {
    const float maximum = frequency_ceiling_hz(sample_rate);
    if (value < 10.0f || value > maximum) {
      throw SonareException(ErrorCode::InvalidParameter,
                            key + " is outside the prepared processor frequency range");
    }
  }
  if (key == "dynamics.deesser.bandPassQ" && value < 1.0e-3f) {
    throw SonareException(ErrorCode::InvalidParameter, key + " must be >= 0.001");
  }
  if (key == "dynamics.compressor.sidechainHighPassHz" && value < 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, key + " must be >= 1");
  }
  if (key == "spectral.airBand.shelfFrequencyHz") {
    const float maximum = frequency_ceiling_hz(sample_rate);
    if (value < 20.0f || value > maximum) {
      throw SonareException(ErrorCode::InvalidParameter,
                            key + " is outside the prepared processor frequency range");
    }
  }
  if (key == "saturation.exciter.frequencyHz") {
    const float maximum = frequency_ceiling_hz(sample_rate);
    if (value < 10.0f || value > maximum) {
      throw SonareException(ErrorCode::InvalidParameter,
                            key + " is outside the prepared processor frequency range");
    }
  }
  if ((key == "saturation.tape.speedIps" || key == "saturation.exciter.q") &&
      value < std::numeric_limits<float>::min()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          key + " is below the processor's minimum positive value");
  }
  if (key == "dynamics.compressor.sidechainHighPassHz" &&
      value > frequency_ceiling_hz(sample_rate)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          key + " is above the prepared processor frequency range");
  }
  if (key == "stereo.monoMaker.frequencyHz" && value > frequency_ceiling_hz(sample_rate)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          key + " is above the prepared processor frequency range");
  }
  if (key == "eq.tilt.pivotHz") {
    const float nyquist = 0.5f * static_cast<float>(sample_rate);
    if (value < 1.0e-3f || value > nyquist - 1.0e-3f) {
      throw SonareException(ErrorCode::InvalidParameter,
                            key + " is outside the prepared sample-rate range");
    }
  }
}

void validate_streaming_chain_config(const MasteringChainConfig& config) {
  validate_mastering_chain_config(config);
  reject_non_streaming_repair(config);
}

float streaming_chain_static_gain(const MasteringChainConfig& config,
                                  const StreamingMasteringChainOptions& options) {
  if (!config.loudness.enabled || std::isnan(options.loudness_static_gain_db)) return 1.0f;
  if (std::isinf(options.loudness_static_gain_db)) {
    util::check::refuse("loudness_static_gain_db",
                        "must be finite, or NaN when the loudness stage has no measurable input");
  }
  // Bound the static gain by the ceiling headroom when the stage-input peak is known.
  const float gain_db = detail::bound_loudness_gain_db(
      options.loudness_static_gain_db, config.loudness.ceiling_dbtp,
      options.loudness_static_gain_peak_dbtp, config.loudness.max_limiter_gain_reduction_db);
  const float gain = ::sonare::db_to_linear(gain_db);
  if (!numeric::finite(gain)) {
    util::check::refuse("loudness_static_gain_db", "must produce a finite linear gain");
  }
  return gain;
}

StreamingMasteringChain::StreamingMasteringChain(MasteringChainConfig config,
                                                 StreamingMasteringChainOptions options)
    : config_(std::move(config)) {
  validate_streaming_chain_config(config_);
  loudness_static_gain_linear_ = streaming_chain_static_gain(config_, options);
  impl_ = std::make_unique<Impl>();
}

StreamingMasteringChain::~StreamingMasteringChain() = default;
StreamingMasteringChain::StreamingMasteringChain(StreamingMasteringChain&&) noexcept = default;
StreamingMasteringChain& StreamingMasteringChain::operator=(StreamingMasteringChain&&) noexcept =
    default;

void StreamingMasteringChain::prepare(double sample_rate, int max_block_size, int num_channels) {
  util::check::within(sample_rate, kAudioSampleRateDomain, "sample_rate");
  util::check::integer_within(max_block_size, kRealtimeBlockSizeDomain, "max_block_size");
  util::check::integer_within(num_channels, kStreamingChainChannelCountDomain, "num_channels");

  // Build replacement stages in locals. Keep the current stages, counters,
  // readiness, and drain state intact until every new stage has prepared.
  std::vector<std::unique_ptr<rt::ProcessorBase>> processors;
  std::vector<StreamingParameterTarget> parameter_targets;
  std::vector<std::string> stage_names;
  std::vector<const mastering::maximizer::TruePeakLimiter*> substitution_sources;
  auto add_stage = [&](std::unique_ptr<rt::ProcessorBase> proc, const char* name) {
    // The streaming contract has already limited this chain to mono or stereo.
    // Preserve that bound so channel-aware stages do not allocate scratch for
    // every realtime-supported channel.
    proc->prepare(sample_rate, max_block_size, num_channels);
    add_parameter_targets(parameter_targets, name, proc.get());
    processors.push_back(std::move(proc));
    stage_names.emplace_back(name);
  };

  // 1. repair.denoise
  // Ahead of every mastering stage, matching the offline chain: a broadband floor
  // left under the programme reads as material to every stage after it.
  if (config_.repair.denoise.enabled) {
    add_stage(std::make_unique<mastering::repair::StreamingDenoise>(config_.repair.denoise.config),
              "repair.denoise");
  }

  // 2. eq.tilt
  if (config_.eq.tilt.enabled) {
    auto tilt = std::make_unique<mastering::eq::TiltEq>();
    tilt->set_tilt_db(config_.eq.tilt.tilt_db);
    tilt->set_pivot_hz(config_.eq.tilt.pivot_hz);
    add_stage(std::move(tilt), "eq.tilt");
  }

  // 3. dynamics.deesser
  if (config_.dynamics.deesser.enabled) {
    add_stage(std::make_unique<mastering::dynamics::DeEsser>(config_.dynamics.deesser.config),
              "dynamics.deesser");
  }

  // 4. dynamics.transientShaper
  if (config_.dynamics.transient_shaper.enabled) {
    add_stage(std::make_unique<mastering::dynamics::TransientShaper>(
                  config_.dynamics.transient_shaper.config),
              "dynamics.transientShaper");
  }

  // 5. dynamics.compressor
  if (config_.dynamics.compressor.enabled) {
    add_stage(std::make_unique<mastering::dynamics::Compressor>(config_.dynamics.compressor.config),
              "dynamics.compressor");
  }

  // 6. dynamics.multibandComp
  if (config_.dynamics.multiband_comp.enabled) {
    add_stage(std::make_unique<mastering::multiband::MultibandCompressor>(
                  config_.dynamics.multiband_comp.config),
              "dynamics.multibandComp");
  }

  // 7. saturation.tape
  if (config_.saturation.tape.enabled) {
    add_stage(std::make_unique<mastering::saturation::Tape>(config_.saturation.tape.config),
              "saturation.tape");
  }

  // 8. saturation.exciter
  if (config_.saturation.exciter.enabled) {
    add_stage(std::make_unique<mastering::saturation::Exciter>(config_.saturation.exciter.config),
              "saturation.exciter");
  }

  // 9. spectral.airBand
  if (config_.spectral.air_band.enabled) {
    add_stage(std::make_unique<mastering::spectral::AirBand>(config_.spectral.air_band.config),
              "spectral.airBand");
  }

  // 10. stereo.imager (stereo only)
  if (num_channels == 2 && config_.stereo.imager.enabled) {
    add_stage(std::make_unique<mastering::stereo::Imager>(config_.stereo.imager.config),
              "stereo.imager");
  }

  // 11. stereo.monoMaker (stereo only)
  if (num_channels == 2 && config_.stereo.mono_maker.enabled) {
    add_stage(std::make_unique<mastering::stereo::MonoMaker>(config_.stereo.mono_maker.config),
              "stereo.monoMaker");
  }

  // 12. maximizer.truePeakLimiter
  if (config_.maximizer.true_peak_limiter.enabled) {
    auto limiter = std::make_unique<mastering::maximizer::TruePeakLimiter>(
        config_.maximizer.true_peak_limiter.config);
    substitution_sources.push_back(limiter.get());
    add_stage(std::move(limiter), "maximizer.truePeakLimiter");
  }

  // 13. loudness (precomputed static gain + dedicated true-peak limiter).
  // Built only when loudness is enabled (which requires a finite static gain,
  // enforced in the options constructor). The static gain itself is applied in
  // process_block(); here we prepare the matching final limiter that mirrors the
  // offline chain's loudness stage so the streaming preview's ceiling behaviour
  // matches the offline render.
  std::unique_ptr<rt::ProcessorBase> loudness_limiter;
  if (config_.loudness.enabled) {
    const mastering::maximizer::TruePeakLimiterConfig limiter_config =
        mastering::maximizer::loudness_limiter_config(
            config_.loudness.ceiling_dbtp, config_.loudness.true_peak_oversample,
            config_.loudness.release_ms, config_.loudness.apply_gain_at_input_rate);
    auto limiter = std::make_unique<mastering::maximizer::TruePeakLimiter>(limiter_config);
    limiter->prepare(sample_rate, max_block_size, num_channels);
    add_parameter_targets(parameter_targets, "loudness.optimize", limiter.get(), true);
    substitution_sources.push_back(limiter.get());
    loudness_limiter = std::move(limiter);
    stage_names.emplace_back("loudness.optimize");
  }

  // Commit. Nothing below here can throw, so the object goes from "unprepared"
  // to "prepared for these arguments" in one step.
  impl_->processors = std::move(processors);
  impl_->parameter_targets = std::move(parameter_targets);
  impl_->prepared_sample_rate = sample_rate;
  impl_->loudness_limiter = std::move(loudness_limiter);
  impl_->substitution_sources = std::move(substitution_sources);
  // Reset the chain-level discard epoch only when its replacement stages commit.
  impl_->non_finite_discards.reset();
  stage_names_ = std::move(stage_names);
  prepared_channels_ = num_channels;
  max_block_size_ = max_block_size;
  flush_samples_remaining_ = 0;
  flush_started_ = false;
}

void StreamingMasteringChain::process_block(float* const* channels, int num_channels,
                                            int num_samples) {
  if (prepared_channels_ == 0) {
    throw SonareException(ErrorCode::InvalidState,
                          "StreamingMasteringChain::process_block called before prepare()");
  }
  if (num_channels != prepared_channels_) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, "num_channels",
                                            "must match the prepared channel count");
  }
  if (num_samples < 0 || num_samples > max_block_size_) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "StreamingMasteringChain::process_block num_samples exceeds max_block_size from prepare()");
  }
  if (num_samples == 0) {
    return;
  }
  rt::ProcessorBase::validate_channel_buffers(channels, num_channels);
  if (num_channels == 1) {
    validate_offline_audio_samples("samples", channels[0], static_cast<size_t>(num_samples), 0);
  } else {
    validate_offline_audio_samples("left", channels[0], static_cast<size_t>(num_samples), 0);
    validate_offline_audio_samples("right", channels[1], static_cast<size_t>(num_samples), 0);
  }
  // A rejected block must preserve an in-progress drain as well as DSP state.
  flush_samples_remaining_ = 0;
  flush_started_ = false;
  process_prevalidated(channels, num_channels, num_samples);
}

void StreamingMasteringChain::process_prevalidated(float* const* channels, int num_channels,
                                                   int num_samples) {
  // Read on either side of the whole call so a call bumps at most once, however
  // many stages discarded during it. Both process_block() and flush() land here,
  // which is what makes the unit one processing call rather than one block.
  const uint64_t stage_discards_before = impl_->stage_discard_sum();
  for (auto& proc : impl_->processors) {
    proc->process(channels, num_channels, num_samples);
  }
  // Loudness stage: apply the precomputed static gain, then the dedicated
  // true-peak limiter (mirrors the offline chain's loudness stage order). The
  // gain is 1.0 (no-op) unless the chain was constructed with a finite
  // loudness_static_gain_db and the config enables loudness.
  if (impl_->loudness_limiter) {
    if (loudness_static_gain_linear_ != 1.0f) {
      for (int ch = 0; ch < num_channels; ++ch) {
        float* buffer = channels[ch];
        for (int i = 0; i < num_samples; ++i) buffer[i] *= loudness_static_gain_linear_;
      }
    }
    impl_->loudness_limiter->process(channels, num_channels, num_samples);
  }
  if (impl_->stage_discard_sum() != stage_discards_before) impl_->non_finite_discards.bump();
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("hidden")))
#endif
void seed_non_finite_discards(StreamingMasteringChain& chain, std::size_t count) noexcept {
  chain.impl_->non_finite_discards.add(count);
}

std::size_t StreamingMasteringChain::non_finite_discard_count() const noexcept {
  return impl_->non_finite_discards.load();
}

int StreamingMasteringChain::flush(float* const* channels, int num_channels, int max_samples) {
  if (prepared_channels_ == 0) {
    throw SonareException(ErrorCode::InvalidState,
                          "StreamingMasteringChain::flush called before prepare()");
  }
  if (num_channels != prepared_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain::flush num_channels mismatch with prepare()");
  }
  if (max_samples < 0 || max_samples > max_block_size_) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "StreamingMasteringChain::flush max_samples exceeds max_block_size from prepare()");
  }
  if (max_samples == 0) return 0;
  rt::ProcessorBase::validate_channel_buffers(channels, num_channels);
  const int current_tail = tail_samples();
  if (current_tail == rt::TailBudget::kUnbounded) {
    throw SonareException(ErrorCode::NotSupported,
                          "cannot automatically flush a chain with an unbounded tail");
  }
  if (!flush_started_) {
    flush_samples_remaining_ = rt::TailBudget::reported(latency_samples())
                                   .then(rt::TailBudget::reported(current_tail))
                                   .samples();
    flush_started_ = true;
  }
  if (flush_samples_remaining_ == 0) return 0;
  const int num_samples = std::min(max_samples, flush_samples_remaining_);
  for (int ch = 0; ch < num_channels; ++ch) {
    std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
  }
  process_prevalidated(channels, num_channels, num_samples);
  flush_samples_remaining_ -= num_samples;
  return num_samples;
}

void StreamingMasteringChain::reset() {
  for (auto& proc : impl_->processors) {
    proc->reset();
  }
  if (impl_->loudness_limiter) {
    impl_->loudness_limiter->reset();
  }
  flush_samples_remaining_ = 0;
  flush_started_ = false;
}

void StreamingMasteringChain::set_parameter(const std::string& key, double value) {
  if (prepared_channels_ == 0) {
    throw SonareException(ErrorCode::InvalidState,
                          "StreamingMasteringChain::set_parameter called before prepare()");
  }
  if (key.empty()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamingMasteringChain parameter key must not be empty");
  }
  const auto target = std::find_if(
      impl_->parameter_targets.begin(), impl_->parameter_targets.end(),
      [&key](const StreamingParameterTarget& candidate) { return candidate.full_key == key; });
  if (target == impl_->parameter_targets.end() && !detail::is_declared_chain_key(key)) {
    throw SonareParameterRefusal::unknown_key_argument(ErrorCode::InvalidParameter, "key", key,
                                                       chain_expected_keys())
        .as_caller_spelled();
  }
  if (!std::isfinite(value) ||
      std::fabs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, "value",
                                            ::sonare::util::json::schema::kFloatRangePredicate)
        .as_caller_spelled();
  }
  const float float_value = static_cast<float>(value);

  if (target == impl_->parameter_targets.end()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "unknown or unavailable streaming mastering parameter: " + key);
  }
  if (!target->processor->parameter_is_realtime_safe(target->id)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "streaming mastering parameter is not realtime-safe: " + key);
  }

  // Validate a full candidate first so a rejection leaves config() and the DSP untouched.
  MasteringChainConfig candidate = config_;
  // Pass the double through so enum and boolean fields keep their own validation.
  const Param param{target->full_key, value};
  apply_chain_config_overrides(candidate, &param, 1);
  if (!same_stage_enablement(config_, candidate)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "streaming mastering parameter would change stage enablement: " + key);
  }
  validate_mastering_chain_config(candidate);

  // A repeated value must not reach a setter that rebuilds coefficients; the
  // no-op check precedes the domain check so an accepted value can be repeated.
  const auto requested_value = chain_config_parameter_value(candidate, target->full_key);
  const float canonical_value = static_cast<float>(
      requested_value.has_value() ? *requested_value : static_cast<double>(float_value));
  const auto current_value = chain_config_parameter_value(config_, target->full_key);
  if (key == "dynamics.compressor.sidechainHighPassHz" && !current_value.has_value()) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "streaming mastering parameter would turn the sidechain high-pass on: " + key);
  }
  if (current_value.has_value() && requested_value.has_value() &&
      *current_value == *requested_value) {
    return;
  }

  validate_realtime_parameter_domain(key, canonical_value, impl_->prepared_sample_rate);
  validate_chain_config_for_rate(candidate, impl_->prepared_sample_rate);

  const float live_value = canonical_value;
  if (!target->processor->set_parameter(target->id, live_value)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "streaming mastering parameter was rejected by its processor: " + key);
  }
  config_ = std::move(candidate);
}

int StreamingMasteringChain::latency_samples() const noexcept {
  int total = 0;
  for (const auto& proc : impl_->processors) {
    total += proc->latency_samples();
  }
  if (impl_->loudness_limiter) {
    total += impl_->loudness_limiter->latency_samples();
  }
  return total;
}

std::uint32_t StreamingMasteringChain::non_finite_substitution_count() const noexcept {
  std::uint32_t total = 0;
  for (const auto* source : impl_->substitution_sources) {
    accumulate_substitutions(total, source->non_finite_substitution_count());
  }
  return total;
}

int StreamingMasteringChain::tail_samples() const noexcept {
  rt::TailBudget total;
  for (const auto& proc : impl_->processors) {
    total.then(rt::TailBudget::reported(proc->tail_samples()));
  }
  if (impl_->loudness_limiter) {
    total.then(rt::TailBudget::reported(impl_->loudness_limiter->tail_samples()));
  }
  return total.samples();
}

}  // namespace sonare::mastering::api
