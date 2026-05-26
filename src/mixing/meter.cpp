#include "mixing/meter.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::mixing {

using sonare::constants::kEpsilon;
using sonare::constants::kFloorDb;

namespace {
constexpr double kLoudnessOffset = -0.691;
}  // namespace

// K-weighting biquad design shared with metering/lufs.cpp via rt::biquad_design.
MeterProcessor::Biquad MeterProcessor::high_shelf(double frequency, double sample_rate,
                                                  double gain_db, double q) {
  const rt::BiquadCoeffsD c = rt::rbj_high_shelf_d(frequency, sample_rate, gain_db, q);
  return {c.b0, c.b1, c.b2, c.a1, c.a2};
}

MeterProcessor::Biquad MeterProcessor::highpass(double frequency, double sample_rate, double q) {
  const rt::BiquadCoeffsD c = rt::rbj_highpass_d(frequency, sample_rate, q);
  return {c.b0, c.b1, c.b2, c.a1, c.a2};
}

double MeterProcessor::filter_sample(int channel, double x) noexcept {
  const size_t c = static_cast<size_t>(channel);
  // Pre-filter (high shelf), Direct Form II transposed.
  BiquadState& s0 = k_state_pre_[c];
  const double y0 = k_pre_.b0 * x + s0.z1;
  s0.z1 = k_pre_.b1 * x - k_pre_.a1 * y0 + s0.z2;
  s0.z2 = k_pre_.b2 * x - k_pre_.a2 * y0;

  // RLB high-pass.
  BiquadState& s1 = k_state_rlb_[c];
  const double y1 = k_rlb_.b0 * y0 + s1.z1;
  s1.z1 = k_rlb_.b1 * y0 - k_rlb_.a1 * y1 + s1.z2;
  s1.z2 = k_rlb_.b2 * y0 - k_rlb_.a2 * y1;
  return y1;
}

float MeterProcessor::energy_to_lufs(double energy) const noexcept {
  // Finite floor instead of -inf so the meter always shows a bounded value.
  if (energy < static_cast<double>(kEpsilon)) return kFloorDb;
  return static_cast<float>(kLoudnessOffset + 10.0 * std::log10(energy));
}

void MeterProcessor::prepare(double sample_rate, int /*max_block_size*/) {
  sample_rate_ = sample_rate;
  if (config_.measure_true_peak) {
    // ITU-R BS.1770-4 mandates at least 4x oversampling for true-peak
    // measurement; clamp anything below 4 up to 4 while still honoring 8x.
    const int requested = config_.true_peak_oversample;
    const int oversample = requested >= 8 ? 8 : 4;
    true_peak_filter_ = rt::TruePeakFilter(2, oversample);
  }

  if (config_.measure_lufs && sample_rate_ > 0.0) {
    const int sr = static_cast<int>(std::lround(sample_rate_));
    // Cache the K-weighting coefficients for the two most common rates so the
    // (more expensive) shelf/high-pass design is only run for other rates. These
    // precomputed values are bit-for-bit what high_shelf()/highpass() produce at
    // the corresponding sample rate, so behavior is unchanged.
    if (sr == 48000) {
      k_pre_ = {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
                0.73248077421585};
      k_rlb_ = {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    } else if (sr == 44100) {
      k_pre_ = {1.5245497507424821, -2.5910067542593858, 1.126819073893832, -1.6237063834520937,
                0.68406845382902193};
      k_rlb_ = {0.99459217735419247, -1.9891843547083849, 0.99459217735419247, -1.989169673629763,
                0.98919903578700674};
    } else {
      k_pre_ = high_shelf(1681.974450955533, sample_rate_, 3.999843853973347, 0.7071752369554196);
      k_rlb_ = highpass(38.13547087613982, sample_rate_, 0.5003270373238773);
    }

    momentary_len_ = std::max<size_t>(1, static_cast<size_t>(std::lround(0.400 * sample_rate_)));
    short_term_len_ = std::max<size_t>(1, static_cast<size_t>(std::lround(3.0 * sample_rate_)));
    gate_hop_ = std::max<size_t>(1, static_cast<size_t>(std::lround(0.100 * sample_rate_)));
    energy_ring_.assign(short_term_len_, 0.0);  // ring sized to the longer window
  } else {
    energy_ring_.clear();
    momentary_len_ = 0;
    short_term_len_ = 0;
    gate_hop_ = 0;
  }

  reset();
}

void MeterProcessor::reset() {
  snapshot_ = MeterSnapshot{};
  seq_ = 0;
  gain_reduction_db_.store(0.0f, std::memory_order_relaxed);

  k_state_pre_ = {};
  k_state_rlb_ = {};
  std::fill(energy_ring_.begin(), energy_ring_.end(), 0.0);
  ring_pos_ = 0;
  filled_ = 0;
  momentary_sum_ = 0.0;
  short_term_sum_ = 0.0;
  gate_hop_counter_ = 0;
  reset_integrated();

  guard_.store(0, std::memory_order_release);
}

void MeterProcessor::reset_integrated() noexcept {
  hist_count_.fill(0);
  hist_energy_.fill(0.0);
}

void MeterProcessor::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  MeterSnapshot next;
  next.gain_reduction_db = gain_reduction_db_.load(std::memory_order_relaxed);
  double sum_l = 0.0;
  double sum_r = 0.0;
  double sum_lr = 0.0;
  double mid_energy = 0.0;
  double side_energy = 0.0;

  const int meters = std::min(num_channels, 2);
  std::array<float, 2> sample_peaks{0.0f, 0.0f};
  for (int ch = 0; ch < meters; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    float peak = 0.0f;
    double sum_square = 0.0;
    for (int i = 0; i < num_samples; ++i) {
      const float sample = channels[ch][i];
      peak = std::max(peak, std::abs(sample));
      sum_square += static_cast<double>(sample) * sample;
    }
    sample_peaks[static_cast<size_t>(ch)] = peak;
    next.peak_db[static_cast<size_t>(ch)] = linear_to_db(peak);
    next.rms_db[static_cast<size_t>(ch)] =
        linear_to_db(std::sqrt(sum_square / static_cast<double>(num_samples)));
  }

  if (config_.measure_true_peak) {
    float max_true_peak = 0.0f;
    for (int ch = 0; ch < meters; ++ch) {
      if (channels[ch] == nullptr) {
        continue;
      }
      const float* mono[] = {channels[ch]};
      const float true_peak = std::max(sample_peaks[static_cast<size_t>(ch)],
                                       true_peak_filter_.process(mono, 1, num_samples));
      next.true_peak_db[static_cast<size_t>(ch)] = linear_to_db(true_peak);
      max_true_peak = std::max(max_true_peak, true_peak);
    }
    next.max_true_peak_db = linear_to_db(max_true_peak);
  }

  if (num_channels >= 2 && channels[0] != nullptr && channels[1] != nullptr) {
    double side_sq_sum = 0.0;
    for (int i = 0; i < num_samples; ++i) {
      const double left = channels[0][i];
      const double right = channels[1][i];
      sum_l += left * left;
      sum_r += right * right;
      sum_lr += left * right;

      const double mono = 0.5 * (left + right);
      const double side = 0.5 * (left - right);
      next.mono_compat_peak = std::max(next.mono_compat_peak, static_cast<float>(std::abs(mono)));
      side_sq_sum += side * side;

      const double mid = (left + right) * constants::kInvSqrt2;
      const double side_scaled = (left - right) * constants::kInvSqrt2;
      mid_energy += mid * mid;
      side_energy += side_scaled * side_scaled;
    }
    const double denom = std::sqrt(sum_l * sum_r);
    next.correlation = denom > 1.0e-12 ? static_cast<float>(sum_lr / denom) : 0.0f;
    next.mono_compat_side_rms = static_cast<float>(std::sqrt(side_sq_sum / num_samples));
    if (mid_energy > static_cast<double>(kEpsilon)) {
      next.mono_compat_width = static_cast<float>(std::sqrt(side_energy / mid_energy));
    } else {
      next.mono_compat_width = side_energy > static_cast<double>(kEpsilon)
                                   ? std::numeric_limits<float>::infinity()
                                   : 0.0f;
    }
    next.likely_mono_compatible = next.correlation >= config_.mono_compat_correlation_threshold;
  }

  if (config_.measure_lufs && !energy_ring_.empty()) {
    const int lufs_channels = std::min(num_channels, 2);
    // Count the channels actually carrying audio. A true-mono bus presents a
    // single non-null channel; BS.1770 then weights that one channel, but the
    // loudness is referenced to a dual-mono pair (a centered mono source must
    // read identically whether routed as 1 or 2 channels). Without this scale
    // the single-channel energy is ~3 dB below the equivalent dual-mono energy.
    int active_channels = 0;
    for (int ch = 0; ch < lufs_channels; ++ch) {
      if (channels[ch] != nullptr) ++active_channels;
    }
    const double mono_energy_scale = (active_channels == 1) ? 2.0 : 1.0;
    for (int i = 0; i < num_samples; ++i) {
      // Combined K-weighted squared energy summed across stereo channels (BS.1770 weight 1.0 each).
      double combined = 0.0;
      for (int ch = 0; ch < lufs_channels; ++ch) {
        if (channels[ch] == nullptr) continue;
        const double y = filter_sample(ch, static_cast<double>(channels[ch][i]));
        combined += y * y;
      }
      combined *= mono_energy_scale;

      // Slide both running sums over the single ring; subtract the value leaving each window.
      const double leaving_short = energy_ring_[ring_pos_];
      short_term_sum_ += combined - leaving_short;

      size_t out_pos = ring_pos_ + short_term_len_ - momentary_len_;
      if (out_pos >= short_term_len_) out_pos -= short_term_len_;
      const double leaving_mom = energy_ring_[out_pos];
      momentary_sum_ += combined - leaving_mom;

      energy_ring_[ring_pos_] = combined;
      ring_pos_ = (ring_pos_ + 1 == short_term_len_) ? 0 : ring_pos_ + 1;
      if (filled_ < short_term_len_) ++filled_;

      // Integrated: take a 400 ms gating block every 100 ms once the momentary window is full.
      if (++gate_hop_counter_ >= gate_hop_) {
        gate_hop_counter_ = 0;
        if (filled_ >= momentary_len_) {
          const double block_energy = momentary_sum_ / static_cast<double>(momentary_len_);
          const float block_lufs = energy_to_lufs(block_energy);
          if (block_lufs >= static_cast<float>(kHistLowLufs)) {
            int bin = static_cast<int>((block_lufs - kHistLowLufs) / kHistBinLu);
            if (bin >= 0 && bin < kHistBins) {
              ++hist_count_[static_cast<size_t>(bin)];
              hist_energy_[static_cast<size_t>(bin)] += block_energy;
            }
          }
        }
      }
    }

    if (filled_ >= momentary_len_) {
      next.momentary_lufs = energy_to_lufs(momentary_sum_ / static_cast<double>(momentary_len_));
    }
    if (filled_ >= short_term_len_) {
      next.short_term_lufs = energy_to_lufs(short_term_sum_ / static_cast<double>(short_term_len_));
    }

    // Gated integrated loudness via the bounded histogram (libebur128-style).
    uint64_t abs_count = 0;
    double abs_energy = 0.0;
    for (int b = 0; b < kHistBins; ++b) {
      abs_count += hist_count_[static_cast<size_t>(b)];
      abs_energy += hist_energy_[static_cast<size_t>(b)];
    }
    if (abs_count > 0) {
      const double preliminary = abs_energy / static_cast<double>(abs_count);
      const double relative_gate_lufs = static_cast<double>(energy_to_lufs(preliminary)) - 10.0;
      const int gate_bin =
          static_cast<int>(std::ceil((relative_gate_lufs - kHistLowLufs) / kHistBinLu));

      uint64_t rel_count = 0;
      double rel_energy = 0.0;
      for (int b = std::max(0, gate_bin); b < kHistBins; ++b) {
        rel_count += hist_count_[static_cast<size_t>(b)];
        rel_energy += hist_energy_[static_cast<size_t>(b)];
      }
      if (rel_count > 0) {
        next.integrated_lufs = energy_to_lufs(rel_energy / static_cast<double>(rel_count));
      }
    }
  }

  publish(next);
}

void MeterProcessor::publish(const MeterSnapshot& next) noexcept {
  guard_.fetch_add(1, std::memory_order_release);  // now odd: write in progress
  snapshot_ = next;
  snapshot_.seq = ++seq_;
  guard_.fetch_add(1, std::memory_order_release);  // now even: write complete
}

MeterSnapshot MeterProcessor::snapshot() const noexcept {
  for (;;) {
    const uint32_t g1 = guard_.load(std::memory_order_acquire);
    if (g1 & 1u) continue;  // writer mid-update
    MeterSnapshot copy = snapshot_;
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t g2 = guard_.load(std::memory_order_acquire);
    if (g1 == g2) return copy;
  }
}

void MeterProcessor::set_gain_reduction_db(float db) noexcept {
  gain_reduction_db_.store(std::min(0.0f, db), std::memory_order_relaxed);
}

}  // namespace sonare::mixing
