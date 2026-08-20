#include "mastering/eq/equalizer_spectrum.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "core/window.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_registry.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::eq {

using sonare::constants::kFloorDb;

namespace {

// Audible range the profile bands are geometrically spaced across. Band b spans
// [profile_band_edge_hz(b), profile_band_edge_hz(b + 1)).
constexpr double kProfileMinHz = 20.0;
constexpr double kProfileMaxHz = 20000.0;
// Fall time of the profile ballistics. Rises are immediate so a transient is
// never under-reported; falls are smoothed so the display does not flicker.
constexpr double kProfileReleaseSeconds = 0.2;

// Lower frequency edge of profile band @p band; kSpectrumProfileBands returns
// the top edge. Evaluated directly rather than by repeated multiplication so
// the edges carry no accumulated rounding, which keeps them identical to a
// caller's own frequency-to-band lookup.
double profile_band_edge_hz(size_t band) {
  const double normalized = static_cast<double>(band) / static_cast<double>(kSpectrumProfileBands);
  return kProfileMinHz * std::pow(kProfileMaxHz / kProfileMinHz, normalized);
}

}  // namespace

void EqSpectrumAnalyzer::prepare(double sample_rate) {
  // Cleared first so a prepare() that throws part-way leaves analyze() inert
  // rather than reading half-sized buffers.
  ready_ = false;
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "EqSpectrumAnalyzer sample_rate must be positive");
  }
  sample_rate_ = sample_rate;
  if (fft_ == nullptr) {
    fft_ = std::make_unique<sonare::FFT>(kFftSize);
  }
  window_ = hann_window(kFftSize);
  double window_sum = 0.0;
  double window_sum_squares = 0.0;
  for (const float coefficient : window_) {
    window_sum += coefficient;
    window_sum_squares += static_cast<double>(coefficient) * coefficient;
  }
  window_scale_ = window_sum > 0.0 ? static_cast<float>(2.0 / window_sum) : 0.0f;
  window_enbw_ = window_sum > 0.0
                     ? static_cast<float>(kFftSize * window_sum_squares / (window_sum * window_sum))
                     : 1.0f;

  ring_.assign(static_cast<size_t>(kFftSize), 0.0f);
  frame_.assign(static_cast<size_t>(kFftSize), 0.0f);
  bins_.assign(static_cast<size_t>(fft_->n_bins()), {});

  // Exclusive upper bound: bin 0 is DC and the final bin is Nyquist. Neither is
  // a two-sided bin, so the single-sided window_scale_ would over-count them.
  const int max_bin = fft_->n_bins() - 1;
  const double bin_hz = sample_rate / kFftSize;
  for (size_t band = 0; band < kSpectrumProfileBands; ++band) {
    const double low_hz = profile_band_edge_hz(band);
    const double high_hz = profile_band_edge_hz(band + 1);
    // Half-open bin range so no bin is counted by two adjacent bands.
    const double begin_bins = std::ceil(low_hz / bin_hz);
    const double end_bins = std::ceil(high_hz / bin_hz);
    int begin = static_cast<int>(std::min(begin_bins, static_cast<double>(max_bin)));
    int end = static_cast<int>(std::min(end_bins, static_cast<double>(max_bin)));
    begin = std::max(begin, 1);
    if (end <= begin) {
      // The band is narrower than the bin spacing (the bottom bands at high
      // sample rates). Read the single bin nearest the band centre, which is
      // still a frequency-domain magnitude, only coarser than the band.
      const double centre_bin = std::floor(std::sqrt(low_hz * high_hz) / bin_hz + 0.5);
      if (centre_bin >= 1.0 && centre_bin < static_cast<double>(max_bin)) {
        begin = static_cast<int>(centre_bin);
        end = begin + 1;
      } else {
        // Entirely above Nyquist (or below the first bin): no estimate exists.
        begin = 0;
        end = 0;
      }
    }
    band_bin_begin_[band] = begin;
    band_bin_end_[band] = end;
  }
  reset();
  ready_ = true;
}

void EqSpectrumAnalyzer::reset() noexcept {
  std::fill(ring_.begin(), ring_.end(), 0.0f);
  ring_pos_ = 0;
  profile_db_.fill(kFloorDb);
  samples_since_transform_ = 0;
  has_profile_ = false;
}

void EqSpectrumAnalyzer::analyze(const float* const* channels, int num_channels, int num_samples,
                                 std::array<float, kSpectrumProfileBands>& out) noexcept {
  if (!ready_) {
    out.fill(kFloorDb);
    return;
  }
  append(channels, num_channels, num_samples);
  samples_since_transform_ += std::max(num_samples, 0);
  // The first block transforms its zero-padded window rather than publishing a
  // placeholder, so the profile is spectral from the very first snapshot.
  if (!has_profile_ || samples_since_transform_ >= kHopSamples) {
    transform(samples_since_transform_);
    samples_since_transform_ = 0;
    has_profile_ = true;
  }
  out = profile_db_;
}

void EqSpectrumAnalyzer::append(const float* const* channels, int num_channels,
                                int num_samples) noexcept {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  // Only the newest window of samples can reach the next transform; a block
  // longer than the window overwrites itself otherwise.
  const int start = num_samples > kFftSize ? num_samples - kFftSize : 0;
  // Channels are averaged: the profile describes the programme the EQ is acting
  // on, so a downmix is the intended view rather than a per-channel spectrum.
  const float channel_scale = 1.0f / static_cast<float>(num_channels);
  for (int i = start; i < num_samples; ++i) {
    float sum = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      sum += channels[ch] != nullptr ? channels[ch][i] : 0.0f;
    }
    ring_[ring_pos_] = sum * channel_scale;
    if (++ring_pos_ == ring_.size()) {
      ring_pos_ = 0;
    }
  }
}

void EqSpectrumAnalyzer::transform(int elapsed_samples) noexcept {
  const size_t size = ring_.size();
  size_t src = ring_pos_;  // The write cursor also marks the oldest sample.
  for (size_t i = 0; i < size; ++i) {
    frame_[i] = ring_[src] * window_[i];
    if (++src == size) {
      src = 0;
    }
  }
  fft_->forward(frame_.data(), bins_.data());

  const double elapsed_seconds =
      sample_rate_ > 0.0 ? static_cast<double>(std::max(elapsed_samples, 0)) / sample_rate_ : 0.0;
  const float release = static_cast<float>(std::exp(-elapsed_seconds / kProfileReleaseSeconds));
  for (size_t band = 0; band < kSpectrumProfileBands; ++band) {
    double power = 0.0;
    for (int bin = band_bin_begin_[band]; bin < band_bin_end_[band]; ++bin) {
      const std::complex<float>& value = bins_[static_cast<size_t>(bin)];
      const double real = static_cast<double>(value.real()) * window_scale_;
      const double imag = static_cast<double>(value.imag()) * window_scale_;
      power += real * real + imag * imag;
    }
    // Dividing the summed bin power by the window's equivalent noise bandwidth
    // undoes the spreading of a tone across the window's main lobe, so a
    // full-scale sine reports 0 dB in the band that contains it.
    const float level_db =
        std::max(kFloorDb, power_to_db_scalar(static_cast<float>(power / window_enbw_)));
    if (!has_profile_ || level_db >= profile_db_[band]) {
      profile_db_[band] = level_db;
    } else {
      profile_db_[band] = level_db + (profile_db_[band] - level_db) * release;
    }
  }
}

EqualizerSpectrumSnapshot EqualizerProcessor::spectrum_snapshot() const noexcept {
  EqualizerSpectrumSnapshot copy;
  for (int attempt = 0; attempt < 3; ++attempt) {
    uint32_t before = spectrum_guard_.load(std::memory_order_acquire);
    // Spin while a write is in progress without consuming the retry budget.
    while ((before & 1U) != 0U) {
      before = spectrum_guard_.load(std::memory_order_acquire);
    }
    copy = spectrum_snapshot_;
    // Ensure the payload read completes before observing the trailing guard.
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t after = spectrum_guard_.load(std::memory_order_acquire);
    if (before == after && (after & 1U) == 0U) {
      return copy;
    }
  }
  return copy;
}

void EqualizerProcessor::capture_stream(const float* const* channels, int num_channels,
                                        int num_samples,
                                        std::array<SpectrumPoint, kSpectrumStreamCapacity>& stream,
                                        size_t& count) noexcept {
  // Uniformly decimated time-domain samples: a scope feed, not an estimate of
  // the signal's spectrum. The spectral view is profile_db.
  count = 0;
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  const int step = std::max(1, (num_samples + static_cast<int>(kSpectrumStreamCapacity) - 1) /
                                   static_cast<int>(kSpectrumStreamCapacity));
  for (int src = 0; src < num_samples && count < kSpectrumStreamCapacity; src += step) {
    const float left = channels[0] != nullptr ? channels[0][src] : 0.0f;
    const float right = num_channels > 1 && channels[1] != nullptr ? channels[1][src] : left;
    stream[count++] = {left, right};
  }
}

void EqualizerProcessor::publish_spectrum_snapshot(const EqualizerSpectrumSnapshot& pre_snapshot,
                                                   const float* const* channels, int num_channels,
                                                   int num_samples) noexcept {
  EqualizerSpectrumSnapshot next = pre_snapshot;
  capture_stream(channels, num_channels, num_samples, next.post, next.post_count);
  next.band_gain_db.fill(0.0f);
  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto& band = bands_[i];
    if (band.enabled && !band.bypassed) {
      next.band_gain_db[i] =
          band.dyn.enabled ? last_applied_gain_db_[i] : band.gain_db * gain_scale_;
    }
  }
  // band_gain_db reports what the EQ applies; profile_db reports what the
  // post-EQ signal contains, measured in the frequency domain.
  spectrum_analyzer_.analyze(channels, num_channels, num_samples, next.profile_db);
  next.seq = ++spectrum_seq_;

  const uint32_t guard = spectrum_guard_.load(std::memory_order_relaxed);
  spectrum_guard_.store(guard + 1U, std::memory_order_release);
  // Publish the odd guard before mutating the payload so readers observe it.
  std::atomic_thread_fence(std::memory_order_release);
  spectrum_snapshot_ = next;
  spectrum_guard_.store(guard + 2U, std::memory_order_release);

  if (config_.spectrum_instance_id != 0) {
    SpectrumProfile profile;
    profile.instance_id = config_.spectrum_instance_id;
    profile.band_db = next.profile_db;
    profile.seq = next.seq;
    profile.active = true;
    SpectrumRegistry::instance().publish(profile);
  }
}
}  // namespace sonare::mastering::eq
