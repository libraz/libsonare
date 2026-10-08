#include "mastering/maximizer/true_peak_limiter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "mastering/common/parameter_domain.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/dynamics/lookahead_validation.h"
#include "rt/scoped_no_denormals.h"
#include "rt/sliding_max.h"
#include "rt/true_peak_fir.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/non_finite_sample.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::maximizer {

using sonare::discard_group_if_non_finite;
using sonare::discard_if_non_finite;
using sonare::resolve_non_finite;
using sonare::resolve_non_finite_run;
using sonare::SampleDestination;

namespace {

// Fast/slow gain-smoother attack times (ms). The fast smoother clamps inter-
// sample peaks quickly; the slow smoother avoids audible distortion on
// sustained material. The per-sample gain is the minimum of the two.
constexpr float kFastAttackMs = 0.1f;
constexpr float kSlowAttackMs = 1.0f;

}  // namespace

void TruePeakOutputGuard::prepare(int factor, int max_channels, int max_block_size) {
  fir_ = &sonare::rt::true_peak_fir_for(factor);
  reach_ = std::max(0, fir_->taps_per_phase - 1);
  // One stencil's reach to complete a position, plus the recheck window's own
  // lookahead so a recheck only ever touches samples not yet emitted.
  latency_ = (1 + kRecheckReaches) * reach_;
  work_.assign(
      static_cast<size_t>(std::max(0, max_channels)),
      std::vector<float>(static_cast<size_t>(latency_ + std::max(0, max_block_size)), 0.0f));
}

void TruePeakOutputGuard::reset() noexcept {
  for (auto& channel : work_) std::fill(channel.begin(), channel.end(), 0.0f);
}

float TruePeakOutputGuard::process(float* const* channels, int num_channels, int num_samples,
                                   int excluded_channel, float ceiling) {
  const size_t pending = static_cast<size_t>(latency_);
  const size_t count = static_cast<size_t>(num_samples);
  const size_t length = pending + count;
  for (int ch = 0; ch < num_channels; ++ch) {
    std::copy_n(channels[ch], count,
                work_[static_cast<size_t>(ch)].begin() + static_cast<std::ptrdiff_t>(pending));
  }

  // Interpolated position k reads base samples [k - lookbehind, k + half]; the
  // previous block evaluated every position whose stencil it could complete.
  const size_t half = static_cast<size_t>(fir_->taps_per_phase / 2);
  const size_t lookbehind = static_cast<size_t>(fir_->taps_per_phase - 1) - half;
  const size_t reach = static_cast<size_t>(reach_);
  const size_t first = pending - half;
  const size_t last = length - 1 - half;
  float min_gain = 1.0f;
  // Scales position k's stencil, channel-linked, when it reads over the ceiling.
  const auto correct = [&](size_t k) {
    float linked = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (ch == excluded_channel) continue;
      const float* data = work_[static_cast<size_t>(ch)].data();
      linked = std::max(linked, std::abs(data[k]));
      for (int phase = 0; phase < fir_->phases; ++phase) {
        linked = std::max(linked, std::abs(sonare::rt::interpolate_polyphase_sample(data, length, k,
                                                                                    phase, *fir_)));
      }
    }
    if (!(linked > ceiling)) return false;
    const float gain = ceiling / linked;
    for (int ch = 0; ch < num_channels; ++ch) {
      float* data = work_[static_cast<size_t>(ch)].data();
      for (size_t s = k - lookbehind; s <= k + half; ++s) data[s] *= gain;
    }
    min_gain = std::min(min_gain, gain);
    return true;
  };
  for (size_t k = first; k <= last; ++k) {
    if (!correct(k)) continue;
    // A scaled stencil can raise an earlier position that shares part of it, since
    // the taps are signed, and that position's own correction can do the same
    // again. Positions within the window still write only unemitted samples, so
    // they are rechecked until none reads over.
    const size_t window = static_cast<size_t>(kRecheckReaches) * reach;
    for (int pass = 0; pass < kMaxRecheckPasses; ++pass) {
      bool corrected = false;
      for (size_t j = k; j + window > k; --j) corrected |= correct(j);
      if (!corrected) break;
    }
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    auto& work = work_[static_cast<size_t>(ch)];
    std::copy_n(work.begin(), count, channels[ch]);
    std::copy(work.begin() + static_cast<std::ptrdiff_t>(count),
              work.begin() + static_cast<std::ptrdiff_t>(length), work.begin());
  }
  return min_gain;
}

TruePeakLimiter::TruePeakLimiter(TruePeakLimiterConfig config) : config_(config) {
  validate_config(config_);
}

void TruePeakLimiter::prepare(double sample_rate, int max_block_size) {
  // Realtime callers use the two-argument ProcessorBase API and may switch
  // between any supported channel count without an allocation in process().
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void TruePeakLimiter::prepare(double sample_rate, int max_block_size, int max_channels) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  if (max_channels < 1 || max_channels > static_cast<int>(dynamics::kRealtimePreparedChannels)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "max_channels exceeds TruePeakLimiter capacity");
  }
  const int lookahead_samples = dynamics::checked_lookahead_samples(
      sample_rate, config_.lookahead_ms,
      std::numeric_limits<int>::max() / config_.oversample_factor - 1);
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_working_channels_ = max_channels;
  lookahead_samples_ = lookahead_samples;
  update_time_constants();
  limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
  limiter_.prepare(sample_rate_, max_block_size_);
  oversampler_.set_factor(config_.oversample_factor);
  oversampled_peak_window_.prepare(
      static_cast<size_t>(std::max(1, lookahead_samples_ + 1) * oversampler_.factor()));
  const size_t max_oversampled_samples = static_cast<size_t>(std::max(0, max_block_size_)) *
                                         static_cast<size_t>(oversampler_.factor());
  // Scratch buffers scale with the caller's actual channel bound. Offline
  // mastering is mono/stereo, while the two-argument prepare() above keeps the
  // 64-channel realtime capacity. Lookahead remains sized to the realtime
  // channel ceiling because its per-channel state must survive live routing
  // changes without allocation on the audio thread.
  const size_t working_channels = static_cast<size_t>(max_working_channels_);
  oversampled_buffers_.assign(working_channels, std::vector<float>(max_oversampled_samples, 0.0f));
  limited_oversampled_buffers_.assign(working_channels,
                                      std::vector<float>(max_oversampled_samples, 0.0f));
  oversampler_states_.assign(working_channels, {});
  for (auto& state : oversampler_states_) {
    oversampler_.prepare_streaming(&state, static_cast<size_t>(std::max(0, max_block_size_)));
  }
  output_guard_.prepare(config_.oversample_factor, max_working_channels_, max_block_size_);
  linked_abs_.assign(max_oversampled_samples, 0.0f);
  input_rate_gain_.assign(static_cast<size_t>(std::max(0, max_block_size_)), 1.0f);
  downsampled_.assign(static_cast<size_t>(std::max(0, max_block_size_)), 0.0f);
  // Rebuild the per-channel delay lines from scratch (assign() replaces the
  // prior contents outright): a re-prepare with a different lookahead /
  // oversample factor must not keep stale delay-line lengths, which would
  // misalign the gain envelope against the signal.
  // process_polyphase_detect_only's own lookahead: the configured lookahead_ms
  // plus the interpolator's group delay, since the streaming upsample delays
  // its output by that much and the base-rate signal the derived gain
  // multiplies must be delayed alike, or the gain envelope and the samples
  // drift out of alignment across every block boundary. Mirrored in
  // latency_samples() below.
  const int detect_only_lookahead_samples =
      std::max(lookahead_samples_, 0) + std::max(oversampler_.latency_samples(), 0);
  lookahead_.assign(dynamics::kRealtimePreparedChannels, {});
  for (auto& buffer : lookahead_) {
    buffer.prepare(static_cast<size_t>(detect_only_lookahead_samples));
  }
  oversampled_lookahead_.assign(dynamics::kRealtimePreparedChannels, {});
  for (auto& buffer : oversampled_lookahead_) {
    buffer.prepare(
        static_cast<size_t>(std::max(lookahead_samples_, 0) * std::max(oversampler_.factor(), 1)));
  }
  prepared_ = true;
  non_finite_substitution_count_.reset();
  reset();
}

void TruePeakLimiter::process(float* const* channels, int num_channels, int num_samples) {
  ensure_prepared(prepared_, "TruePeakLimiter");
  if (num_samples > max_block_size_)
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared max_block_size");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;

  // Before the upsampler and the detector read the block, not after: a
  // non-finite sample left in place spreads across the reconstruction stencil,
  // is counted once per oversampled position it reached, and drives the linked
  // gain to zero so the reported reduction describes work never performed.
  std::uint32_t substituted = 0;
  for (int ch = 0; ch < num_channels; ++ch) {
    substituted += static_cast<std::uint32_t>(
        resolve_non_finite_run(SampleDestination::kIrreversibleOutput, channels[ch],
                               static_cast<std::size_t>(num_samples)));
  }
  non_finite_substitution_count_.add(substituted);

  // All supported oversampling factors use the sample-accurate polyphase
  // brickwall path; validate_config rejects any other value.
  sonare::rt::ScopedNoDenormals no_denormals;
  process_polyphase(channels, num_channels, num_samples);
}

void TruePeakLimiter::process_polyphase(float* const* channels, int num_channels, int num_samples) {
  prepare_buffers(num_channels);
  if (config_.apply_gain_at_input_rate) {
    process_polyphase_detect_only(channels, num_channels, num_samples);
    return;
  }

  const int factor = oversampler_.factor();
  const size_t oversampled_samples = static_cast<size_t>(num_samples) * static_cast<size_t>(factor);
  for (int ch = 0; ch < num_channels; ++ch) {
    auto& oversampled = oversampled_buffers_[static_cast<size_t>(ch)];
    oversampler_.upsample_to_streaming(channels[ch], static_cast<size_t>(num_samples),
                                       oversampled.data(), oversampled.size(),
                                       &oversampler_states_[static_cast<size_t>(ch)]);
  }

  std::fill_n(linked_abs_.begin(), static_cast<std::ptrdiff_t>(oversampled_samples), 0.0f);
  const int excluded_channel = detector_excluded_channel(num_channels);
  if (excluded_channel < 0) {
    for (int ch = 0; ch < num_channels; ++ch) {
      const auto& channel = oversampled_buffers_[static_cast<size_t>(ch)];
      for (size_t i = 0; i < oversampled_samples; ++i) {
        linked_abs_[i] = std::max(linked_abs_[i], std::abs(channel[i]));
      }
    }
  } else {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (ch == excluded_channel) continue;
      const auto& channel = oversampled_buffers_[static_cast<size_t>(ch)];
      for (size_t i = 0; i < oversampled_samples; ++i) {
        linked_abs_[i] = std::max(linked_abs_[i], std::abs(channel[i]));
      }
    }
  }

  const float ceiling = db_to_linear(config_.ceiling_db);
  float min_gain = 1.0f;
  std::uint32_t substituted = 0;
  for (size_t os = 0; os < oversampled_samples; ++os) {
    oversampled_peak_window_.push(linked_abs_[os]);

    const float peak = oversampled_peak_window_.max();
    const float target_gain = peak > ceiling && peak > 0.0f ? ceiling / peak : 1.0f;
    const float release_coeff = adaptive_release_coeff(peak);
    if (target_gain < fast_gain_) {
      fast_gain_ = fast_attack_coeff_ * fast_gain_ + (1.0f - fast_attack_coeff_) * target_gain;
    } else {
      fast_gain_ = release_coeff * fast_gain_ + (1.0f - release_coeff) * target_gain;
    }
    if (target_gain < slow_gain_) {
      slow_gain_ = slow_attack_coeff_ * slow_gain_ + (1.0f - slow_attack_coeff_) * target_gain;
    } else {
      slow_gain_ = release_coeff * slow_gain_ + (1.0f - release_coeff) * target_gain;
    }
    const float gain = std::min(fast_gain_, slow_gain_);
    min_gain = std::min(min_gain, gain);

    for (int ch = 0; ch < num_channels; ++ch) {
      const float delayed = oversampled_lookahead_[static_cast<size_t>(ch)].process(
          oversampled_buffers_[static_cast<size_t>(ch)][os]);
      // The gain is a recursive cell, so a previous block can hand this multiply
      // a non-finite one. A ceiling here would be the loudest sample the stage
      // can write, standing in for a product it never computed.
      float limited = delayed * gain;
      if (resolve_non_finite(SampleDestination::kIrreversibleOutput, limited)) ++substituted;
      limited_oversampled_buffers_[static_cast<size_t>(ch)][os] = limited;
    }

    // The gain above lags the ideal one on a fast transient, so a post-lookahead
    // sample can still sit over the ceiling. Pull it back channel-linked, the
    // same way the decimated correction below does: a per-channel residual gain
    // here would shift the stereo image on exactly the loudest transients.
    float linked_output = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (ch == excluded_channel) continue;
      linked_output = std::max(linked_output,
                               std::abs(limited_oversampled_buffers_[static_cast<size_t>(ch)][os]));
    }
    if (linked_output > ceiling && linked_output > 0.0f) {
      const float hard_gain = ceiling / linked_output;
      for (int ch = 0; ch < num_channels; ++ch) {
        limited_oversampled_buffers_[static_cast<size_t>(ch)][os] *= hard_gain;
      }
      min_gain = std::min(min_gain, gain * hard_gain);
    }
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    oversampler_.downsample_to_streaming(
        limited_oversampled_buffers_[static_cast<size_t>(ch)].data(), oversampled_samples,
        downsampled_.data(), static_cast<size_t>(num_samples),
        &oversampler_states_[static_cast<size_t>(ch)]);
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = downsampled_[static_cast<size_t>(i)];
    }
  }

  // The decimation lowpass rings above the bound the oversampled path enforced,
  // and its output re-interpolated by a meter rings again: the guard bounds
  // what the meter reads, not what was decimated.
  min_gain = std::min(min_gain, output_guard_.process(channels, num_channels, num_samples,
                                                      excluded_channel, ceiling));

  // Once per block, not per sample: nothing downstream reads the count mid-block.
  non_finite_substitution_count_.add(substituted);
  last_gain_reduction_db_ = std::min(0.0f, linear_to_db(min_gain));
  minimum_gain_reduction_db_ = std::min(minimum_gain_reduction_db_, last_gain_reduction_db_);
  discard_non_finite_state();
}

void TruePeakLimiter::process_polyphase_detect_only(float* const* channels, int num_channels,
                                                    int num_samples) {
  const int factor = oversampler_.factor();
  const size_t oversampled_samples = static_cast<size_t>(num_samples) * static_cast<size_t>(factor);
  for (int ch = 0; ch < num_channels; ++ch) {
    auto& oversampled = oversampled_buffers_[static_cast<size_t>(ch)];
    oversampler_.upsample_to_streaming(channels[ch], static_cast<size_t>(num_samples),
                                       oversampled.data(), oversampled.size(),
                                       &oversampler_states_[static_cast<size_t>(ch)]);
  }

  std::fill_n(linked_abs_.begin(), static_cast<std::ptrdiff_t>(oversampled_samples), 0.0f);
  const int excluded_channel = detector_excluded_channel(num_channels);
  if (excluded_channel < 0) {
    for (int ch = 0; ch < num_channels; ++ch) {
      const auto& channel = oversampled_buffers_[static_cast<size_t>(ch)];
      for (size_t i = 0; i < oversampled_samples; ++i) {
        linked_abs_[i] = std::max(linked_abs_[i], std::abs(channel[i]));
      }
    }
  } else {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (ch == excluded_channel) continue;
      const auto& channel = oversampled_buffers_[static_cast<size_t>(ch)];
      for (size_t i = 0; i < oversampled_samples; ++i) {
        linked_abs_[i] = std::max(linked_abs_[i], std::abs(channel[i]));
      }
    }
  }

  const float ceiling = db_to_linear(config_.ceiling_db);
  float min_gain = 1.0f;
  std::uint32_t substituted = 0;
  std::fill_n(input_rate_gain_.begin(), num_samples, 1.0f);
  for (size_t os = 0; os < oversampled_samples; ++os) {
    oversampled_peak_window_.push(linked_abs_[os]);

    const float peak = oversampled_peak_window_.max();
    const float target_gain = peak > ceiling && peak > 0.0f ? ceiling / peak : 1.0f;
    const float release_coeff = adaptive_release_coeff(peak);
    if (target_gain < fast_gain_) {
      fast_gain_ = fast_attack_coeff_ * fast_gain_ + (1.0f - fast_attack_coeff_) * target_gain;
    } else {
      fast_gain_ = release_coeff * fast_gain_ + (1.0f - release_coeff) * target_gain;
    }
    if (target_gain < slow_gain_) {
      slow_gain_ = slow_attack_coeff_ * slow_gain_ + (1.0f - slow_attack_coeff_) * target_gain;
    } else {
      slow_gain_ = release_coeff * slow_gain_ + (1.0f - release_coeff) * target_gain;
    }
    const float gain = std::min(fast_gain_, slow_gain_);
    min_gain = std::min(min_gain, gain);
    // Map the oversampled gain back to its base sample as the MINIMUM gain over
    // the factor subsamples that belong to that base sample (os / factor). Any
    // inter-sample peak detected at OS rate therefore forces the corresponding
    // base sample down. This is the best the detect-only mode can do without
    // re-synthesising the limited signal at OS rate (see header note); the
    // polyphase path is the sample-accurate, true-peak-guaranteeing route.
    input_rate_gain_[os / static_cast<size_t>(factor)] =
        std::min(input_rate_gain_[os / static_cast<size_t>(factor)], gain);
  }

  for (int i = 0; i < num_samples; ++i) {
    const float gain = input_rate_gain_[static_cast<size_t>(i)];
    for (int ch = 0; ch < num_channels; ++ch) {
      const float delayed = lookahead_[static_cast<size_t>(ch)].process(channels[ch][i]);
      // See the polyphase path: the gain is recursive, so the product is guarded
      // even though the input reached this stage finite.
      float limited = delayed * gain;
      if (resolve_non_finite(SampleDestination::kIrreversibleOutput, limited)) ++substituted;
      channels[ch][i] = limited;
    }
  }

  // A gain that steps between base samples reshapes the reconstruction between
  // them; the guard bounds what a meter reads, as on the polyphase path.
  min_gain = std::min(min_gain, output_guard_.process(channels, num_channels, num_samples,
                                                      excluded_channel, ceiling));

  // Once per block, not per sample: nothing downstream reads the count mid-block.
  non_finite_substitution_count_.add(substituted);
  last_gain_reduction_db_ = std::min(0.0f, linear_to_db(min_gain));
  minimum_gain_reduction_db_ = std::min(minimum_gain_reduction_db_, last_gain_reduction_db_);
  discard_non_finite_state();
}

void TruePeakLimiter::discard_non_finite_state() noexcept {
  // Four floats, once per block. The block's samples reach the detector finite,
  // so these cells are only ever stranded by a non-finite coefficient of their
  // own; a stranded one does not surface as a non-finite output either, because
  // the post-gain guard substitutes every sample instead and the stage falls
  // silent for the rest of the handle. The crest peak's std::max fold drops a
  // NaN but keeps an infinity, which is the value an envelope over |x| actually
  // acquires, and an infinite crest holds the release at its shortest for good.
  // The gain smoothers rest at unity, the crest detector's two halves together at
  // silence.
  bool discarded = discard_if_non_finite(fast_gain_, 1.0f);
  discarded |= discard_if_non_finite(slow_gain_, 1.0f);
  discarded |= discard_group_if_non_finite(crest_peak_, crest_rms_);
  if (discarded) note_non_finite_discard();
}

void TruePeakLimiter::reset() {
  limiter_.reset();
  for (auto& buffer : lookahead_) buffer.reset();
  for (auto& buffer : oversampled_lookahead_) buffer.reset();
  fast_gain_ = 1.0f;
  slow_gain_ = 1.0f;
  crest_peak_ = 0.0f;
  crest_rms_ = 0.0f;
  adaptive_release_coeff_ = release_coeff_;
  adaptive_release_counter_ = 0;
  oversampled_peak_window_.reset();
  for (auto& state : oversampler_states_) oversampler_.reset_streaming(&state);
  output_guard_.reset();
  last_gain_reduction_db_ = 0.0f;
  minimum_gain_reduction_db_ = 0.0f;
}

void TruePeakLimiter::set_config(const TruePeakLimiterConfig& config) {
  validate_config(config);
  if (prepared_) {
    (void)dynamics::checked_lookahead_samples(
        sample_rate_, config.lookahead_ms,
        std::numeric_limits<int>::max() / config.oversample_factor - 1);
  }
  rt::apply_config_diff(config_, config, [this](const rt::ConfigDiff<TruePeakLimiterConfig>& diff) {
    if (!prepared_) return;
    // Only a structural change (lookahead length, oversample factor, gain-application
    // mode, which select the delay lines and polyphase topology) re-prepares; that
    // wipes running state, so it is control-thread-only and MUST NOT race with
    // process(). Ceiling and release are applied in place.
    if (diff.changed(&TruePeakLimiterConfig::lookahead_ms,
                     &TruePeakLimiterConfig::oversample_factor,
                     &TruePeakLimiterConfig::apply_gain_at_input_rate)) {
      prepare(sample_rate_, max_block_size_, max_working_channels_);
      return;
    }
    update_time_constants();
    limiter_.set_config({config_.ceiling_db, config_.lookahead_ms, config_.release_ms});
  });
}

void TruePeakLimiter::set_release_ms(float release_ms) {
  if (!std::isfinite(release_ms) || release_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "true peak limiter release must be non-negative");
  }
  config_.release_ms = release_ms;
  update_time_constants();
  limiter_.set_release_ms(release_ms);
}

void TruePeakLimiter::set_release_ms_in_place(float release_ms) noexcept {
  // RT-safe: update the scalar release time used by the polyphase gain envelope
  // (config_.release_ms feeds adaptive_release_coeff() and release_coeff_) and
  // forward to the inner brickwall limiter's in-place setter. No publish, no
  // allocation. set_config / the published snapshot are left untouched.
  config_.release_ms = std::max(0.0f, release_ms);
  release_coeff_ = time_to_coefficient(smoother_sample_rate(), config_.release_ms);
  adaptive_release_coeff_ = release_coeff_;
  adaptive_release_counter_ = 0;
  limiter_.set_release_ms_in_place(config_.release_ms);
}

bool TruePeakLimiter::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.ceiling_db = std::min(0.0f, value);
      // Update the ceiling in place: set_config() would publish a snapshot and allocate per event.
      return limiter_.set_parameter(0, config_.ceiling_db);
    case 1:
      // In-place: recomputes time constants and forwards to inner limiter
      // without clearing lookahead or gain-envelope state. Must use the noexcept
      // in-place setter (it clamps internally) so this RT-safe parameter never
      // throws or re-prepares on the audio thread.
      set_release_ms_in_place(value);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> TruePeakLimiter::parameter_descriptors() const {
  return {{"ceilingDb", 0}, {"releaseMs", 1}};
}

bool TruePeakLimiter::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id <= 1u;
}

void TruePeakLimiter::validate_config(const TruePeakLimiterConfig& config) {
  if (!common::valid_ceiling_db(config.ceiling_db) || !std::isfinite(config.lookahead_ms) ||
      !std::isfinite(config.release_ms) || config.lookahead_ms < 0.0f || config.release_ms < 0.0f ||
      (config.oversample_factor != 1 && config.oversample_factor != 2 &&
       config.oversample_factor != 4 && config.oversample_factor != 8 &&
       config.oversample_factor != 16)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid true peak limiter configuration");
  }
}

int TruePeakLimiter::latency_samples() const noexcept {
  const int oversampling_delay = config_.apply_gain_at_input_rate
                                     ? oversampler_.latency_samples()
                                     : oversampler_.streaming_round_trip_latency_samples();
  return limiter_.latency_samples() + oversampling_delay + output_guard_.latency_samples();
}

void TruePeakLimiter::prepare_buffers(int num_channels) {
  // Every scratch buffer is preallocated in prepare(). Never resize on the
  // audio thread; reject a route wider than the requested working capacity.
  if (num_channels <= max_working_channels_) {
    return;
  }
  throw SonareException(ErrorCode::InvalidParameter,
                        "num_channels exceeds prepared TruePeakLimiter state");
}

double TruePeakLimiter::smoother_sample_rate() const noexcept {
  return sample_rate_ * std::max(1, config_.oversample_factor);
}

void TruePeakLimiter::update_time_constants() {
  // The fast/slow attack smoothers are deliberately near-instant peak clamps and
  // stay at the base rate: a shorter effective attack only tightens limiting. The
  // release and crest-detector time constants, however, control the audible
  // release envelope and MUST be converted at the rate the smoother loop actually
  // advances at (the oversampled rate), or a nominal 50 ms release runs a factor
  // of oversample_factor too fast (pumping/distortion).
  const double release_rate = smoother_sample_rate();
  fast_attack_coeff_ = time_to_coefficient(sample_rate_, kFastAttackMs);
  slow_attack_coeff_ = time_to_coefficient(sample_rate_, kSlowAttackMs);
  release_coeff_ = time_to_coefficient(release_rate, config_.release_ms);
  crest_coeff_ = time_to_coefficient(release_rate, 200.0f);
  adaptive_release_coeff_ = release_coeff_;
  adaptive_release_counter_ = 0;
}

float TruePeakLimiter::adaptive_release_coeff(float linked_peak) {
  crest_peak_ =
      std::max(linked_peak, crest_coeff_ * crest_peak_ + (1.0f - crest_coeff_) * linked_peak);
  crest_rms_ = crest_coeff_ * crest_rms_ + (1.0f - crest_coeff_) * linked_peak * linked_peak;
  // Floor on the smoothed RMS to avoid division by zero on silence.
  constexpr float kCrestRmsFloor = 1e-12f;
  // Crest factor maps to a transient amount via (crest - offset) / range, where a
  // steady tone (crest ~= 1..2) yields ~0 and sharp transients saturate toward 1.
  constexpr float kCrestTransientOffset = 2.0f;
  constexpr float kCrestTransientRange = 8.0f;
  // Maximum fraction by which the release is shortened for full transients.
  constexpr float kMaxReleaseShorten = 0.75f;
  if (adaptive_release_counter_ == 0) {
    const float rms = std::sqrt(std::max(crest_rms_, kCrestRmsFloor));
    const float crest = crest_peak_ / rms;
    const float transient =
        std::clamp((crest - kCrestTransientOffset) / kCrestTransientRange, 0.0f, 1.0f);
    const float release_scale = 1.0f - kMaxReleaseShorten * transient;
    adaptive_release_coeff_ =
        time_to_coefficient(smoother_sample_rate(), config_.release_ms * release_scale);
  }
  adaptive_release_counter_ = (adaptive_release_counter_ + 1) % kReleaseControlInterval;
  return adaptive_release_coeff_;
}

}  // namespace sonare::mastering::maximizer
