#include "mastering/spectral/air_band.h"

#include <algorithm>
#include <cmath>

#include "mastering/dynamics/channel_limits.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::spectral {
namespace {

using sonare::constants::kInvSqrt2D;
using sonare::constants::kPiD;

AirBand::Biquad make_highpass(double frequency_hz, double sample_rate, double q) {
  const float w0 = static_cast<float>(
      2.0 * kPiD * std::clamp(frequency_hz, 20.0, sample_rate * 0.49) / sample_rate);
  AirBand::Biquad b;
  b.c = rt::rbj_highpass(w0, static_cast<float>(q));
  return b;
}

void assign_biquad(AirBand::Biquad& b, const rt::BiquadCoeffsD& coeffs) {
  b.c.b0 = static_cast<float>(coeffs.b0);
  b.c.b1 = static_cast<float>(coeffs.b1);
  b.c.b2 = static_cast<float>(coeffs.b2);
  b.c.a1 = static_cast<float>(coeffs.a1);
  b.c.a2 = static_cast<float>(coeffs.a2);
}

AirBand::Biquad make_high_shelf(double frequency_hz, double sample_rate, float gain_db) {
  AirBand::Biquad b;
  const double frequency = std::clamp(frequency_hz, 20.0, sample_rate * 0.49);
  assign_biquad(b, rt::rbj_high_shelf_d(frequency, sample_rate, gain_db, kInvSqrt2D));
  return b;
}

}  // namespace

AirBand::AirBand(AirBandConfig config) : config_(config) { validate_config(config_); }

void AirBand::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid prepare arguments");
  }
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  // Preallocate per-channel state so process() never resizes on the audio
  // thread (matches Tube/AmpSim).
  const size_t n = dynamics::kRealtimePreparedChannels;
  envelope_.assign(n, 0.0f);
  shelf_gain_db_.assign(n, 0.0f);
  band_rms_sq_.assign(n, 0.0f);
  harmonic_rms_sq_.assign(n, 0.0f);
  harmonic_gain_.assign(n, 0.0f);
  shelf_control_samples_.assign(n, 0);
  normalization_rms_alpha_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.030 * sample_rate_)));
  harmonic_gain_alpha_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * sample_rate_)));
  band_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  oversampled_scratch_.assign(static_cast<size_t>(max_block_size_) * kHarmonicOversampleFactor,
                              0.0f);
  harmonic_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  harmonic_oversampler_states_.resize(n);
  for (auto& state : harmonic_oversampler_states_) {
    harmonic_oversampler_.prepare_streaming(&state, static_cast<size_t>(max_block_size_));
  }
  rebuild_filters(static_cast<int>(n));
  reset();
}

void AirBand::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "AirBand");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared AirBand oversampling scratch");
  }
  ensure_state(num_channels);
  // Frequency terms of the shelf depend only on config/sample rate. Coefficients
  // update at a fixed stream-wide control cadence; keeping that cadence in
  // per-channel state (rather than restarting it at every block) makes output
  // independent of the host's callback size.
  const double shelf_frequency =
      std::clamp(config_.shelf_frequency_hz, 20.0f, static_cast<float>(sample_rate_ * 0.49));
  const auto shelf_design = rt::rbj_high_shelf_design_d(shelf_frequency, sample_rate_, kInvSqrt2D);
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    float envelope = envelope_[static_cast<size_t>(ch)];
    float current_gain_db = shelf_gain_db_[static_cast<size_t>(ch)];
    int shelf_control_samples = shelf_control_samples_[static_cast<size_t>(ch)];
    Biquad& shelf = shelf_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float band = detector_[static_cast<size_t>(ch)].process(channels[ch][i]);
      band_scratch_[static_cast<size_t>(i)] = band;
      envelope = 0.995f * envelope + 0.005f * std::abs(band);
      const float over_db = std::max(0.0f, linear_to_db(envelope) - config_.dynamic_threshold_db);
      if (shelf_control_samples == 0) {
        current_gain_db = std::min(config_.dynamic_range_db, over_db * 0.25f) * config_.amount;
        assign_biquad(shelf, rt::rbj_high_shelf_from_design_d(shelf_design, current_gain_db));
        shelf_control_samples = kShelfControlInterval;
      }
      --shelf_control_samples;
      channels[ch][i] = shelf.process(channels[ch][i]);
    }
    shelf_gain_db_[static_cast<size_t>(ch)] = current_gain_db;
    shelf_control_samples_[static_cast<size_t>(ch)] = shelf_control_samples;

    const size_t sample_count = static_cast<size_t>(num_samples);
    const size_t oversampled_count = sample_count * kHarmonicOversampleFactor;
    auto& oversampler_state = harmonic_oversampler_states_[static_cast<size_t>(ch)];
    harmonic_oversampler_.upsample_to_streaming(band_scratch_.data(), sample_count,
                                                oversampled_scratch_.data(),
                                                oversampled_scratch_.size(), &oversampler_state);
    // Shape the shelf-frequency high-pass detector output directly. The old
    // first difference had the response 2*sin(pi*f/fs), so even multiplying by
    // fs only normalized its low-frequency slope; near Nyquist the drive still
    // changed materially with sample rate. Removing that differentiator makes
    // the band-limited excitation depend on physical filter frequency instead.
    const float harmonic_drive = 1.0f + config_.amount;
    for (size_t i = 0; i < oversampled_count; ++i) {
      oversampled_scratch_[i] = std::tanh(oversampled_scratch_[i] * harmonic_drive);
    }
    harmonic_oversampler_.downsample_to_streaming(oversampled_scratch_.data(), oversampled_count,
                                                  harmonic_scratch_.data(),
                                                  harmonic_scratch_.size(), &oversampler_state);

    float band_rms_sq = band_rms_sq_[static_cast<size_t>(ch)];
    float harmonic_rms_sq = harmonic_rms_sq_[static_cast<size_t>(ch)];
    float harmonic_gain = harmonic_gain_[static_cast<size_t>(ch)];
    for (size_t i = 0; i < sample_count; ++i) {
      harmonic_scratch_[i] =
          harmonic_filter_[static_cast<size_t>(ch)].process(harmonic_scratch_[i]);
      band_rms_sq += normalization_rms_alpha_ * (band_scratch_[i] * band_scratch_[i] - band_rms_sq);
      harmonic_rms_sq += normalization_rms_alpha_ *
                         (harmonic_scratch_[i] * harmonic_scratch_[i] - harmonic_rms_sq);
      // Keep the synthesized component at most -6 dB RMS relative to the
      // detector band. The smoothed ratio and a short gain ramp are updated
      // per sample so splitting the same stream into different blocks cannot
      // change its normalization or introduce a block-edge step.
      constexpr float kMaxHarmonicToBandRms = 0.5f;
      const float target_gain =
          harmonic_rms_sq > 1.0e-20f && band_rms_sq > 0.0f
              ? config_.amount *
                    std::min(1.0f, kMaxHarmonicToBandRms * std::sqrt(band_rms_sq / harmonic_rms_sq))
              : 0.0f;
      harmonic_gain += harmonic_gain_alpha_ * (target_gain - harmonic_gain);
      channels[ch][i] += harmonic_scratch_[i] * harmonic_gain;
    }
    envelope_[static_cast<size_t>(ch)] = envelope;
    band_rms_sq_[static_cast<size_t>(ch)] = band_rms_sq;
    harmonic_rms_sq_[static_cast<size_t>(ch)] = harmonic_rms_sq;
    harmonic_gain_[static_cast<size_t>(ch)] = harmonic_gain;
  }
}

void AirBand::reset() {
  std::fill(band_scratch_.begin(), band_scratch_.end(), 0.0f);
  std::fill(oversampled_scratch_.begin(), oversampled_scratch_.end(), 0.0f);
  std::fill(harmonic_scratch_.begin(), harmonic_scratch_.end(), 0.0f);
  std::fill(envelope_.begin(), envelope_.end(), 0.0f);
  std::fill(shelf_gain_db_.begin(), shelf_gain_db_.end(), 0.0f);
  std::fill(band_rms_sq_.begin(), band_rms_sq_.end(), 0.0f);
  std::fill(harmonic_rms_sq_.begin(), harmonic_rms_sq_.end(), 0.0f);
  std::fill(harmonic_gain_.begin(), harmonic_gain_.end(), 0.0f);
  std::fill(shelf_control_samples_.begin(), shelf_control_samples_.end(), 0);
  for (auto& state : harmonic_oversampler_states_) {
    harmonic_oversampler_.reset_streaming(&state);
  }
  for (auto& f : shelf_) f.reset();
  for (auto& f : detector_) f.reset();
  for (auto& f : harmonic_filter_) f.reset();
}

void AirBand::set_config(const AirBandConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    const Biquad updated =
        make_highpass(config_.shelf_frequency_hz, sample_rate_, sonare::constants::kButterworthQD);
    for (auto& filter : detector_) filter.c = updated.c;
    for (auto& filter : harmonic_filter_) filter.c = updated.c;
  }
}

bool AirBand::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.amount = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 1: {
      config_.shelf_frequency_hz = std::max(value, 1.0e-3f);
      // Recompute the detector highpass coefficients in place, preserving its
      // filter state (z1/z2). The shelf is rebuilt per-sample in process().
      for (auto& filter : detector_) {
        const Biquad updated = make_highpass(config_.shelf_frequency_hz, sample_rate_,
                                             sonare::constants::kButterworthQD);
        filter.c = updated.c;
      }
      for (auto& filter : harmonic_filter_) {
        const Biquad updated = make_highpass(config_.shelf_frequency_hz, sample_rate_,
                                             sonare::constants::kButterworthQD);
        filter.c = updated.c;
      }
      return true;
    }
    case 2:
      config_.dynamic_threshold_db = value;
      return true;
    case 3:
      config_.dynamic_range_db = std::max(0.0f, value);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> AirBand::parameter_descriptors() const {
  return {{"amount", 0}, {"shelfFrequencyHz", 1}, {"dynamicThresholdDb", 2}, {"dynamicRangeDb", 3}};
}

void AirBand::validate_config(const AirBandConfig& config) {
  if (!(config.amount >= 0.0f && config.amount <= 1.0f) || !(config.shelf_frequency_hz > 0.0f) ||
      config.dynamic_range_db < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid air band configuration");
  }
}

void AirBand::ensure_state(int num_channels) {
  // prepare() preallocates kRealtimePreparedChannels; only grow (control thread)
  // if a caller exceeds it. Growing preserves existing channels' filter state
  // and seeds the new ones; a narrower block is left untouched (no churn).
  const auto target_size = static_cast<size_t>(num_channels);
  if (envelope_.size() < target_size || shelf_gain_db_.size() < target_size ||
      band_rms_sq_.size() < target_size || harmonic_rms_sq_.size() < target_size ||
      harmonic_gain_.size() < target_size || shelf_control_samples_.size() < target_size ||
      shelf_.size() < target_size || detector_.size() < target_size ||
      harmonic_filter_.size() < target_size || harmonic_oversampler_states_.size() < target_size) {
    const size_t old_envelope_size = envelope_.size();
    const size_t old_shelf_gain_size = shelf_gain_db_.size();
    const size_t old_band_rms_size = band_rms_sq_.size();
    const size_t old_harmonic_rms_size = harmonic_rms_sq_.size();
    const size_t old_harmonic_gain_size = harmonic_gain_.size();
    const size_t old_shelf_control_size = shelf_control_samples_.size();
    const size_t old_shelf_size = shelf_.size();
    const size_t old_detector_size = detector_.size();
    const size_t old_harmonic_filter_size = harmonic_filter_.size();
    const size_t old_oversampler_state_size = harmonic_oversampler_states_.size();
    envelope_.resize(target_size, 0.0f);
    shelf_gain_db_.resize(target_size, 0.0f);
    band_rms_sq_.resize(target_size, 0.0f);
    harmonic_rms_sq_.resize(target_size, 0.0f);
    harmonic_gain_.resize(target_size, 0.0f);
    shelf_control_samples_.resize(target_size, 0);
    shelf_.resize(target_size);
    detector_.resize(target_size);
    harmonic_filter_.resize(target_size);
    harmonic_oversampler_states_.resize(target_size);
    for (size_t i = old_envelope_size; i < target_size; ++i) {
      envelope_[i] = 0.0f;
    }
    for (size_t i = old_shelf_gain_size; i < target_size; ++i) {
      shelf_gain_db_[i] = 0.0f;
    }
    for (size_t i = old_band_rms_size; i < target_size; ++i) band_rms_sq_[i] = 0.0f;
    for (size_t i = old_harmonic_rms_size; i < target_size; ++i) harmonic_rms_sq_[i] = 0.0f;
    for (size_t i = old_harmonic_gain_size; i < target_size; ++i) harmonic_gain_[i] = 0.0f;
    for (size_t i = old_shelf_control_size; i < target_size; ++i) shelf_control_samples_[i] = 0;
    for (size_t i = old_shelf_size; i < target_size; ++i) {
      shelf_[i] = make_high_shelf(config_.shelf_frequency_hz, sample_rate_, 0.0f);
    }
    for (size_t i = old_detector_size; i < target_size; ++i) {
      detector_[i] = make_highpass(config_.shelf_frequency_hz, sample_rate_,
                                   sonare::constants::kButterworthQD);
    }
    for (size_t i = old_harmonic_filter_size; i < target_size; ++i) {
      harmonic_filter_[i] = make_highpass(config_.shelf_frequency_hz, sample_rate_,
                                          sonare::constants::kButterworthQD);
    }
    for (size_t i = old_oversampler_state_size; i < target_size; ++i) {
      harmonic_oversampler_.prepare_streaming(&harmonic_oversampler_states_[i],
                                              static_cast<size_t>(max_block_size_));
    }
  }
}

void AirBand::rebuild_filters(int num_channels) {
  if (num_channels <= 0) return;
  shelf_.assign(static_cast<size_t>(num_channels),
                make_high_shelf(config_.shelf_frequency_hz, sample_rate_, 0.0f));
  detector_.assign(
      static_cast<size_t>(num_channels),
      make_highpass(config_.shelf_frequency_hz, sample_rate_, sonare::constants::kButterworthQD));
  harmonic_filter_.assign(
      static_cast<size_t>(num_channels),
      make_highpass(config_.shelf_frequency_hz, sample_rate_, sonare::constants::kButterworthQD));
}

}  // namespace sonare::mastering::spectral
