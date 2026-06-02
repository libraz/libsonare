#include "mastering/eq/dynamic_eq.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "mastering/dynamics/channel_limits.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::eq {

using sonare::constants::kFloorDb;
using sonare::constants::kTwoPiD;
using sonare::mastering::dynamics::kRealtimePreparedChannels;

void DynamicEq::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  sample_rate_ = sample_rate;
  eq_.prepare(sample_rate, max_block_size);
  // Preallocate every band's per-channel detector state up front so the audio-
  // thread process()/ensure_detector path never allocates: kRealtimePreparedChannels
  // channels and a lookahead ring sized to the maximum supported lookahead. The
  // live FIFO length (look_size) is then varied within this capacity without
  // resizing, mirroring the fixed-size limiter lookahead.
  max_lookahead_samples_ = static_cast<int>(std::round(sample_rate_ * kMaxLookaheadMs * 0.001));
  for (auto& detector : detectors_) {
    detector.channels.assign(kRealtimePreparedChannels, {});
    for (auto& channel : detector.channels) {
      channel.look_ring.assign(static_cast<size_t>(std::max(max_lookahead_samples_, 0)), 0.0f);
      channel.look_size = 0;
      channel.look_pos = 0;
    }
  }
  prepared_ = true;
  rebuild();
}

void DynamicEq::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "DynamicEq");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
  }

  validate_sidechain(num_samples);
  const float* const* detector_channels = sidechain_channels_ != nullptr
                                              ? sidechain_channels_
                                              : const_cast<const float* const*>(channels);
  const int detector_num_channels =
      sidechain_channels_ != nullptr ? sidechain_num_channels_ : num_channels;
  last_detector_db_ = detector_db(detector_channels, detector_num_channels, num_samples);
  for (size_t i = 0; i < kMaxBands; ++i) {
    last_band_detector_db_[i] =
        bands_[i].enabled
            ? band_detector_db(detector_channels, detector_num_channels, num_samples, i)
            : kFloorDb;
  }

  // Compute each band's target gain (static + dynamic delta) once per block, then
  // evolve the applied gain toward that target at SAMPLE rate inside the loop and
  // refresh the biquad coefficients every kCoeffUpdateInterval samples. Disabled
  // bands clear their EQ slot and contribute nothing.
  const double smoothing_samples = std::max(sample_rate_ * 0.005, 1.0);
  const float per_sample_coeff =
      prepared_ ? static_cast<float>(1.0 - std::exp(-1.0 / smoothing_samples)) : 1.0f;
  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto& dynamic_band = bands_[i];
    if (!dynamic_band.enabled) {
      eq_.clear_band(i);
      last_applied_gain_db_[i] = 0.0f;
      target_gain_db_[i] = 0.0f;
      continue;
    }
    target_gain_db_[i] =
        dynamic_band.static_gain_db + dynamic_gain_delta(dynamic_band, last_band_detector_db_[i]);
  }

  // Reusable view buffer for the sub-block pointers (resized at most once per
  // channel-count change; no per-sub-block allocation on the audio thread).
  if (sub_channels_.size() != static_cast<size_t>(num_channels)) {
    sub_channels_.assign(static_cast<size_t>(num_channels), nullptr);
  }
  for (int offset = 0; offset < num_samples; offset += kCoeffUpdateInterval) {
    const int chunk = std::min(kCoeffUpdateInterval, num_samples - offset);
    for (size_t i = 0; i < kMaxBands; ++i) {
      if (!bands_[i].enabled) {
        continue;
      }
      // Advance the smoothed gain by `chunk` per-sample one-pole steps toward the
      // target using the closed form so the sub-block update matches sample-rate
      // smoothing exactly: g += (1 - (1-c)^chunk) * (target - g).
      const float decay = std::pow(1.0f - per_sample_coeff, static_cast<float>(chunk));
      smoothed_gain_db_[i] += (1.0f - decay) * (target_gain_db_[i] - smoothed_gain_db_[i]);
      apply_band_gain(i, smoothed_gain_db_[i]);
    }
    for (int ch = 0; ch < num_channels; ++ch) {
      sub_channels_[static_cast<size_t>(ch)] = channels[ch] + offset;
    }
    eq_.process(sub_channels_.data(), num_channels, chunk);
  }

  clear_sidechain();
}

void DynamicEq::apply_band_gain(size_t index, float gain_db) {
  const auto& dynamic_band = bands_[index];
  last_applied_gain_db_[index] = gain_db;
  // Steady state: skip the biquad recompute when the gain has not moved enough
  // to matter, avoiding needless coefficient design on the audio thread.
  if (std::abs(gain_db - last_applied_coeff_gain_db_[index]) < kGainEpsilonDb &&
      eq_.band(index).enabled) {
    return;
  }
  last_applied_coeff_gain_db_[index] = gain_db;
  eq_.set_band(index,
               {dynamic_band.type, dynamic_band.frequency_hz, gain_db, dynamic_band.q, true});
}

void DynamicEq::reset() {
  eq_.reset();
  last_detector_db_ = kFloorDb;
  last_band_detector_db_.fill(kFloorDb);
  last_applied_gain_db_.fill(0.0f);
  smoothed_gain_db_.fill(0.0f);
  target_gain_db_.fill(0.0f);
  // Force the next apply to reprogram every band's coefficients (the steady-
  // state skip must never compare against a stale value across a reset).
  last_applied_coeff_gain_db_.fill(std::numeric_limits<float>::quiet_NaN());
  for (auto& detector : detectors_) {
    for (auto& channel : detector.channels) channel.reset();
  }
  clear_sidechain();
}

void DynamicEq::set_band(size_t index, const DynamicEqBand& band) {
  validate_index(index);
  validate_band(band);
  bands_[index] = band;
  if (prepared_) {
    rebuild();
  }
}

void DynamicEq::clear_band(size_t index) {
  validate_index(index);
  bands_[index] = {};
  last_applied_gain_db_[index] = 0.0f;
  if (prepared_) {
    eq_.clear_band(index);
  }
}

void DynamicEq::clear() {
  for (size_t i = 0; i < kMaxBands; ++i) {
    clear_band(i);
  }
}

const DynamicEqBand& DynamicEq::band(size_t index) const {
  validate_index(index);
  return bands_[index];
}

float DynamicEq::last_applied_gain_db(size_t index) const {
  validate_index(index);
  return last_applied_gain_db_[index];
}

float DynamicEq::last_band_detector_db(size_t index) const {
  validate_index(index);
  return last_band_detector_db_[index];
}

void DynamicEq::set_sidechain(const float* const* channels, int num_channels, int num_samples) {
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sidechain dimensions must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    clear_sidechain();
    return;
  }
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "sidechain channels must not be null");
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "sidechain channel must not be null");
  }
  sidechain_channels_ = channels;
  sidechain_num_channels_ = num_channels;
  sidechain_num_samples_ = num_samples;
}

void DynamicEq::clear_sidechain() {
  sidechain_channels_ = nullptr;
  sidechain_num_channels_ = 0;
  sidechain_num_samples_ = 0;
}

bool DynamicEq::set_parameter(unsigned int param_id, float value) {
  const size_t band_index = param_id / kParamsPerBand;
  if (band_index >= kMaxBands) {
    return false;
  }
  DynamicEqBand& band = bands_[band_index];
  switch (param_id % kParamsPerBand) {
    case 0:
      // Clamp to the open interval (0 Hz, Nyquist) so coefficient design never
      // throws on the audio thread.
      band.frequency_hz =
          std::clamp(value, 1.0e-3f, static_cast<float>(sample_rate_ * 0.5) - 1.0e-3f);
      break;
    case 1:
      band.static_gain_db = value;
      break;
    case 2:
      band.q = std::max(value, 1.0e-6f);
      break;
    case 3:
      band.threshold_db = value;
      break;
    case 4:
      band.ratio = std::max(1.0f, value);
      break;
    case 5:
      band.range_db = value;
      break;
    case 6:
      band.sidechain_q = std::max(value, 1.0e-6f);
      break;
    case 7:
      // -1 (or any non-positive value) is the sentinel meaning "follow the band
      // frequency"; positive values set an explicit sidechain frequency.
      band.sidechain_freq_hz = value > 0.0f ? value : -1.0f;
      break;
    case 8:
      band.attack_ms = std::max(0.0f, value);
      break;
    case 9:
      band.release_ms = std::max(0.0f, value);
      break;
    case 10:
      band.lookahead_ms = std::max(0.0f, value);
      break;
    default:
      return false;
  }
  // Recompute the affected band's gain and biquad coefficients in place. rebuild
  // with num_samples == 0 applies the change immediately (no extra smoothing)
  // and routes through ParametricEq::set_band, which preserves filter state.
  // Disabled bands clear their EQ slot and stay silent until enabled.
  if (prepared_) {
    rebuild();
  }
  return true;
}

void DynamicEq::validate_index(size_t index) {
  if (index >= kMaxBands) {
    throw SonareException(ErrorCode::InvalidParameter, "dynamic EQ band index out of range");
  }
}

void DynamicEq::validate_band(const DynamicEqBand& band) {
  if (!band.enabled) {
    return;
  }
  if (band.type == EqBandType::TiltShelf || band.type == EqBandType::FlatTilt) {
    // The underlying ParametricEq backend has no tilt coefficient design and
    // would throw from rebuild() — reject on this control-thread path so a tilt
    // band can never be installed and later trip the RT automation path.
    throw SonareException(ErrorCode::InvalidParameter,
                          "dynamic EQ band type does not support TiltShelf/FlatTilt");
  }
  if (!(band.frequency_hz > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "frequency_hz must be positive");
  }
  if (!(band.q > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "Q must be positive");
  }
  if (!(band.ratio >= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "ratio must be at least 1");
  }
  if (!(band.sidechain_q > 0.0f) || band.attack_ms < 0.0f || band.release_ms < 0.0f ||
      band.lookahead_ms < 0.0f ||
      (band.sidechain_freq_hz != -1.0f && band.sidechain_freq_hz <= 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "invalid dynamic EQ sidechain configuration");
  }
}

float DynamicEq::detector_db(const float* const* channels, int num_channels, int num_samples) {
  double sum = 0.0;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      const double sample = channels[ch][i];
      sum += sample * sample;
    }
  }

  const double count = static_cast<double>(num_channels) * static_cast<double>(num_samples);
  return linear_to_db(static_cast<float>(std::sqrt(sum / std::max(count, 1.0))));
}

void DynamicEq::ensure_detector(size_t index, int num_channels) {
  DetectorState& state = detectors_[index];
  const DynamicEqBand& band = bands_[index];

  // Reject blocks wider than the prepared per-channel state; the audio thread
  // never resizes (which would malloc), matching the limiter pattern.
  if (static_cast<size_t>(num_channels) > state.channels.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared DynamicEq detector state");
  }

  // Clamp the lookahead to the capacity preallocated in prepare() so the ring is
  // never reallocated on the audio thread (automation past the bound saturates).
  const int lookahead_samples =
      std::clamp(static_cast<int>(std::round(sample_rate_ * band.lookahead_ms * 0.001)), 0,
                 max_lookahead_samples_);

  // (Re)design the bandpass prototype and timing only when the relevant band
  // fields change — coefficient design is double-precision and not free.
  const bool design_changed =
      state.frequency_hz != band.frequency_hz ||
      state.sidechain_freq_hz != band.sidechain_freq_hz || state.sidechain_q != band.sidechain_q ||
      state.attack_ms != band.attack_ms || state.release_ms != band.release_ms;
  if (design_changed) {
    const double detector_frequency =
        band.sidechain_freq_hz > 0.0f ? band.sidechain_freq_hz : band.frequency_hz;
    const double frequency =
        std::clamp(static_cast<double>(detector_frequency), 1.0, sample_rate_ * 0.5 - 1.0);
    const auto coeffs = rt::rbj_bandpass(
        static_cast<float>(kTwoPiD * frequency / sample_rate_),
        static_cast<float>(std::max(static_cast<double>(band.sidechain_q), 1.0e-6)));
    state.prototype.b0 = coeffs.b0;
    state.prototype.b1 = coeffs.b1;
    state.prototype.b2 = coeffs.b2;
    state.prototype.a1 = coeffs.a1;
    state.prototype.a2 = coeffs.a2;
    state.attack = sonare::time_to_attack_release_rate(sample_rate_, band.attack_ms);
    state.release = sonare::time_to_attack_release_rate(sample_rate_, band.release_ms);
    state.frequency_hz = band.frequency_hz;
    state.sidechain_freq_hz = band.sidechain_freq_hz;
    state.sidechain_q = band.sidechain_q;
    state.attack_ms = band.attack_ms;
    state.release_ms = band.release_ms;
  }

  // The per-channel state is preallocated in prepare(); channels_changed here
  // means the observed channel count differs from the previous block (no
  // allocation, just a re-seed of the now-active channels' coefficients).
  const bool channels_changed = state.active_channels != num_channels;
  const bool lookahead_changed =
      state.lookahead_ms != band.lookahead_ms || state.lookahead_samples != lookahead_samples;
  if (channels_changed) {
    state.active_channels = num_channels;
  }
  if (lookahead_changed) {
    state.lookahead_samples = lookahead_samples;
    state.lookahead_ms = band.lookahead_ms;
  }

  // Propagate fresh coefficients (and re-window the lookahead ring) only when
  // something relevant changed; the per-sample filter state (z1/z2) and ring
  // contents are otherwise preserved across blocks for continuity. The ring is
  // never resized here — its live FIFO length (look_size) is set within the
  // capacity preallocated in prepare(), and the newly active window is zeroed.
  if (design_changed || channels_changed || lookahead_changed) {
    for (int ch = 0; ch < num_channels; ++ch) {
      DetectorChannel& channel = state.channels[static_cast<size_t>(ch)];
      const auto copy = [&](DetectorBiquad& f) {
        f.b0 = state.prototype.b0;
        f.b1 = state.prototype.b1;
        f.b2 = state.prototype.b2;
        f.a1 = state.prototype.a1;
        f.a2 = state.prototype.a2;
      };
      copy(channel.filter_a);
      copy(channel.filter_b);
      if (channel.look_size != static_cast<size_t>(lookahead_samples)) {
        channel.look_size = static_cast<size_t>(lookahead_samples);
        std::fill(channel.look_ring.begin(), channel.look_ring.begin() + channel.look_size, 0.0f);
        channel.look_pos = 0;
      }
    }
  }
}

float DynamicEq::band_detector_db(const float* const* channels, int num_channels, int num_samples,
                                  size_t index) {
  if (num_samples <= 0 || num_channels <= 0) return kFloorDb;
  ensure_detector(index, num_channels);
  DetectorState& state = detectors_[index];
  const int look = state.lookahead_samples;

  // Detector-history continuity fix (NOT true zero-latency lookahead). The
  // bandpass filters and envelope follower are persistent across blocks, and a
  // per-channel ring (look_ring) carries the last `look` input samples from the
  // previous block into this one. The detector therefore processes a CONTINUOUS
  // input stream — `[carried tail] ++ [this block minus its own tail]` — instead
  // of the previous code's `min(num_samples-1, i+look)` which repeated the final
  // sample for the last `look` positions of every block and systematically
  // under-detected at each boundary.
  //
  // Trade-off: because the audio path stays latency-free, the detection horizon
  // effectively LAGS by up to `look` samples rather than leading; a transient at
  // a block boundary is still detected (within ~look samples), just not ahead of
  // time. True lookahead would require delaying the audio, which this processor
  // intentionally does not do.
  double sum = 0.0;
  for (int ch = 0; ch < num_channels; ++ch) {
    DetectorChannel& dc = state.channels[static_cast<size_t>(ch)];
    auto step = [&](float sample) {
      const double rectified = std::abs(dc.filter_b.process(dc.filter_a.process(sample)));
      const double coeff = rectified > dc.envelope ? state.attack : state.release;
      dc.envelope += coeff * (rectified - dc.envelope);
      sum += dc.envelope * dc.envelope;
    };
    // Continuous stream: the carried tail (oldest first) feeds the detector,
    // followed by the current block. The detector consumes exactly num_samples
    // inputs per channel (look carried + (num_samples - look) fresh), so the RMS
    // count below stays num_channels * num_samples. After processing, the ring is
    // a plain FIFO holding the most recent `look` input samples for the next
    // call. No per-block allocation: the ring is sized once in ensure_detector().
    for (int i = 0; i < num_samples; ++i) {
      if (look > 0) {
        // FIFO: emit the oldest stored sample, then store the new input in its
        // slot. This delays each input by exactly `look` samples, giving the
        // continuous stream described above.
        const float oldest = dc.look_ring[dc.look_pos];
        dc.look_ring[dc.look_pos] = channels[ch][i];
        dc.look_pos = (dc.look_pos + 1) % dc.look_size;
        step(oldest);
      } else {
        step(channels[ch][i]);
      }
    }
  }
  const double count = static_cast<double>(num_channels) * static_cast<double>(num_samples);
  const double rms = std::sqrt(sum / std::max(count, 1.0));
  return linear_to_db(static_cast<float>(rms));
}

float DynamicEq::dynamic_gain_delta(const DynamicEqBand& band, float detector_db) {
  if (!band.enabled || detector_db <= band.threshold_db || band.range_db == 0.0f) {
    return 0.0f;
  }

  const float over_db = detector_db - band.threshold_db;
  const float compressed_db = over_db * (1.0f - 1.0f / band.ratio);
  const float range = std::abs(band.range_db);
  const float amount = std::min(range, compressed_db);
  return band.range_db < 0.0f ? -amount : amount;
}

void DynamicEq::rebuild(int /*num_samples*/) {
  // Immediate (non-process) reconfiguration path used by set_band/set_parameter:
  // recompute each band's static target gain and program the biquad now without
  // smoothing (no audio is flowing). The per-sample smoothing during process()
  // then evolves from this seed. Dynamic deltas are recomputed each block in
  // process(), so here we seed with the static gain plus the last known
  // detector-driven delta to avoid a jump on the first processed block.
  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto& dynamic_band = bands_[i];
    if (!dynamic_band.enabled) {
      eq_.clear_band(i);
      last_applied_gain_db_[i] = 0.0f;
      last_applied_coeff_gain_db_[i] = std::numeric_limits<float>::quiet_NaN();
      smoothed_gain_db_[i] = 0.0f;
      target_gain_db_[i] = 0.0f;
      continue;
    }
    const float target_gain =
        dynamic_band.static_gain_db + dynamic_gain_delta(dynamic_band, last_band_detector_db_[i]);
    smoothed_gain_db_[i] = target_gain;
    target_gain_db_[i] = target_gain;
    last_applied_gain_db_[i] = target_gain;
    last_applied_coeff_gain_db_[i] = target_gain;
    eq_.set_band(i,
                 {dynamic_band.type, dynamic_band.frequency_hz, target_gain, dynamic_band.q, true});
  }
}

void DynamicEq::validate_sidechain(int expected_samples) const {
  if (sidechain_channels_ == nullptr) return;
  if (sidechain_num_samples_ != expected_samples) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "sidechain length must match process block length");
  }
}

}  // namespace sonare::mastering::eq
