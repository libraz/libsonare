#include "mastering/spectral/air_band.h"

#include <algorithm>
#include <cmath>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::spectral {
namespace {

using sonare::discard_group_if_non_finite;
using sonare::discard_if_non_finite;
using sonare::constants::kInvSqrt2D;
using sonare::constants::kPiD;

// Keep the detector envelope's existing 48 kHz behavior while expressing its
// smoothing in seconds. The legacy coefficient was alpha=0.005 per sample;
// derive its equivalent one-pole time constant once and resample that time
// constant for each prepared host rate.
constexpr double kEnvelopeReferenceSampleRate = 48000.0;
constexpr double kEnvelopeReferenceAlpha = 0.005;

double envelope_time_constant_seconds() {
  return -1.0 / (kEnvelopeReferenceSampleRate * std::log1p(-kEnvelopeReferenceAlpha));
}

float envelope_alpha_for_sample_rate(double sample_rate) {
  const double tau = envelope_time_constant_seconds();
  return static_cast<float>(rt::one_pole_alpha_from_seconds_d(tau, sample_rate));
}

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

AirBand::AirBand(AirBandConfig config) : config_(config) {
  validate_config(config_);
  paths_.set_path_latencies_q8(
      {0, harmonic_oversampler_.streaming_round_trip_latency_samples() << 8});
}

void AirBand::prepare(double sample_rate, int max_block_size) {
  // Realtime callers use the two-argument ProcessorBase API and may switch
  // between any supported channel count without an allocation in process().
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void AirBand::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "AirBand");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_working_channels_ = max_channels;
  prepared_ = true;
  // Preallocate per-channel state so process() never resizes on the audio
  // thread (matches Tube/AmpSim). Scratch scales with the caller's channel
  // bound: offline mastering callers know they are mono/stereo, while the
  // two-argument overload above keeps the realtime capacity.
  const size_t n = static_cast<size_t>(max_working_channels_);
  envelope_.assign(n, 0.0f);
  shelf_gain_db_.assign(n, 0.0f);
  band_rms_sq_.assign(n, 0.0f);
  harmonic_rms_sq_.assign(n, 0.0f);
  harmonic_gain_.assign(n, 0.0f);
  shelf_control_samples_.assign(n, 0);
  envelope_alpha_ = envelope_alpha_for_sample_rate(sample_rate_);
  normalization_rms_alpha_ =
      static_cast<float>(rt::one_pole_alpha_from_seconds_d(0.030, sample_rate_));
  harmonic_gain_alpha_ = static_cast<float>(rt::one_pole_alpha_from_seconds_d(0.005, sample_rate_));
  band_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  oversampled_scratch_.assign(static_cast<size_t>(max_block_size_) * kHarmonicOversampleFactor,
                              0.0f);
  harmonic_scratch_.assign(static_cast<size_t>(max_block_size_), 0.0f);
  harmonic_oversampler_states_.resize(n);
  for (auto& state : harmonic_oversampler_states_) {
    harmonic_oversampler_.prepare_streaming(&state, static_cast<size_t>(max_block_size_));
  }
  paths_.ensure_channels(n);
  rebuild_filters(static_cast<int>(n));
  reset();
}

void AirBand::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "AirBand");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
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
  bool discarded = false;
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
      envelope = (1.0f - envelope_alpha_) * envelope + envelope_alpha_ * std::abs(band);
      if (shelf_control_samples == 0) {
        const float over_db = std::max(0.0f, linear_to_db(envelope) - config_.dynamic_threshold_db);
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
      // Delay the dry shelf sample by the same round trip the harmonic
      // content already went through (via the streaming oversampler's
      // internal history), so the two being summed here both refer to the
      // same original input sample instead of the harmonic content lagging
      // the dry signal by the oversampler's group delay.
      const auto lane = static_cast<size_t>(ch);
      const float dry = paths_.align(0, lane, channels[ch][i]);
      channels[ch][i] = dry + paths_.align(1, lane, harmonic_scratch_[i]) * harmonic_gain;
    }
    envelope_[static_cast<size_t>(ch)] = envelope;
    band_rms_sq_[static_cast<size_t>(ch)] = band_rms_sq;
    harmonic_rms_sq_[static_cast<size_t>(ch)] = harmonic_rms_sq;
    harmonic_gain_[static_cast<size_t>(ch)] = harmonic_gain;
    discarded |= discard_non_finite_state(static_cast<size_t>(ch));
  }
  if (discarded) note_non_finite_discard();
}

bool AirBand::discard_non_finite_state(size_t channel) noexcept {
  // Ten floats per channel, once per block. The detector feeds the envelope
  // that programmes the shelf, so the three sections and the four followers
  // they drive come back together.
  bool discarded = discard_group_if_non_finite(shelf_[channel].z1, shelf_[channel].z2);
  discarded |= discard_group_if_non_finite(detector_[channel].z1, detector_[channel].z2);
  discarded |=
      discard_group_if_non_finite(harmonic_filter_[channel].z1, harmonic_filter_[channel].z2);
  discarded |= discard_if_non_finite(envelope_[channel], 0.0f);
  discarded |= discard_if_non_finite(shelf_gain_db_[channel], 0.0f);
  discarded |= discard_group_if_non_finite(band_rms_sq_[channel], harmonic_rms_sq_[channel],
                                           harmonic_gain_[channel]);
  return discarded;
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
  paths_.reset();
  for (auto& f : shelf_) f.reset();
  for (auto& f : detector_) f.reset();
  for (auto& f : harmonic_filter_) f.reset();
}

void AirBand::set_config(const AirBandConfig& config) {
  validate_config(config);
  rt::apply_config_diff(config_, config, [this](const rt::ConfigDiff<AirBandConfig>& diff) {
    if (!prepared_ || !diff.changed(&AirBandConfig::shelf_frequency_hz)) return;
    // Coefficients only; the filters keep their state.
    const Biquad updated =
        make_highpass(config_.shelf_frequency_hz, sample_rate_, sonare::constants::kButterworthQD);
    for (auto& filter : detector_) filter.c = updated.c;
    for (auto& filter : harmonic_filter_) filter.c = updated.c;
  });
}

namespace {
constexpr bool amount_valid(float v) noexcept { return v >= 0.0f && v <= 1.0f; }
constexpr bool shelf_frequency_valid(float v) noexcept { return v > 0.0f; }
bool dynamic_range_valid(float v) noexcept { return std::isfinite(v) && v >= 0.0f; }
}  // namespace

bool AirBand::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      if (!amount_valid(value)) return false;
      config_.amount = value;
      return true;
    case 1: {
      if (!shelf_frequency_valid(value)) return false;
      config_.shelf_frequency_hz = value;
      // Recompute the detector highpass coefficients in place, preserving its
      // filter state (z1/z2). The shelf is rebuilt per-sample in process().
      const Biquad updated = make_highpass(config_.shelf_frequency_hz, sample_rate_,
                                           sonare::constants::kButterworthQD);
      for (auto& filter : detector_) filter.c = updated.c;
      for (auto& filter : harmonic_filter_) filter.c = updated.c;
      return true;
    }
    case 2:
      config_.dynamic_threshold_db = value;
      return true;
    case 3:
      if (!dynamic_range_valid(value)) return false;
      config_.dynamic_range_db = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> AirBand::parameter_descriptors() const {
  return {{"amount", 0}, {"shelfFrequencyHz", 1}, {"dynamicThresholdDb", 2}, {"dynamicRangeDb", 3}};
}

void AirBand::validate_config(const AirBandConfig& config) {
  if (!amount_valid(config.amount) || !shelf_frequency_valid(config.shelf_frequency_hz) ||
      !dynamic_range_valid(config.dynamic_range_db)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid air band configuration");
  }
}

void AirBand::ensure_state(int num_channels) {
  // prepare() sizes every per-channel vector to max_working_channels_.
  require_prepared_channels(num_channels, static_cast<size_t>(max_working_channels_), "AirBand");
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
