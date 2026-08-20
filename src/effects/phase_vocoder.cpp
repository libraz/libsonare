#include "effects/phase_vocoder.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/window.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/phase.h"

namespace sonare {

using sonare::constants::kTwoPi;
using sonare::constants::kTwoPiD;

namespace {

size_t checked_output_count(size_t input_count, float rate) {
  size_t output_count = 0;
  constexpr size_t kMaxOutputCount =
      std::min<size_t>(kMaxAudioBufferSize, static_cast<size_t>(INT_MAX));
  SONARE_CHECK(numeric::checked_projected_count(input_count, rate, kMaxOutputCount, &output_count),
               ErrorCode::InvalidParameter);
  return output_count;
}

int checked_output_frames(int n_bins, int input_frames, float rate) {
  SONARE_CHECK(n_bins > 0 && input_frames > 0, ErrorCode::InvalidParameter);
  const size_t output_frames = checked_output_count(static_cast<size_t>(input_frames), rate);
  size_t output_elements = 0;
  SONARE_CHECK(numeric::checked_size_product(static_cast<size_t>(n_bins), output_frames,
                                             kMaxAudioBufferSize, &output_elements),
               ErrorCode::InvalidParameter);
  (void)output_elements;
  return std::max(1, static_cast<int>(output_frames));
}

}  // namespace

std::vector<float> compute_instantaneous_frequency(const float* phase, const float* prev_phase,
                                                   int n_bins, int hop_length, int sample_rate) {
  SONARE_CHECK(phase != nullptr && prev_phase != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bins >= 2 && sample_rate > 0 && hop_length > 0, ErrorCode::InvalidParameter);

  std::vector<float> inst_freq(n_bins);

  float time_step = static_cast<float>(hop_length) / static_cast<float>(sample_rate);

  /// FFT length implied by the one-sided bin count (n_bins == n_fft/2 + 1), so
  /// the bin-frequency formula matches phase_vocoder() / phase_vocoder_phaselocked().
  const int n_fft = (n_bins - 1) * 2;

  for (int k = 0; k < n_bins; ++k) {
    /// Expected phase advance based on bin frequency
    float bin_freq =
        static_cast<float>(k) * static_cast<float>(sample_rate) / static_cast<float>(n_fft);
    float expected_phase_advance = kTwoPi * bin_freq * time_step;

    /// Actual phase difference
    float phase_diff = phase[k] - prev_phase[k];

    /// Phase deviation from expected
    float phase_deviation = phase::wrap(phase_diff - expected_phase_advance);

    /// Instantaneous frequency
    inst_freq[k] = bin_freq + phase_deviation / (kTwoPi * time_step);
  }

  return inst_freq;
}

StreamingPhaseVocoder::StreamingPhaseVocoder(StreamingPhaseVocoderConfig config) : config_(config) {
  SONARE_CHECK(config_.sample_rate > 0, ErrorCode::InvalidParameter);
  validate_cola_geometry(config_.n_fft, config_.hop_length);
  if (config_.win_length <= 0) config_.win_length = config_.n_fft;
  SONARE_CHECK(config_.win_length <= config_.n_fft, ErrorCode::InvalidParameter);
}

void StreamingPhaseVocoder::reset() {
  input_.clear();
  input_base_sample_ = 0;
  ola_base_sample_ = 0;
  emitted_output_samples_ = 0;
  active_rate_ = 0.0f;
  finalized_ = false;
  analysis_frames_.clear();
  std::fill(phase_acc_.begin(), phase_acc_.end(), 0.0);
  peaks_.clear();
  ola_output_.clear();
  ola_window_sum_.clear();
  analysis_frame_base_ = 0;
  next_analysis_frame_ = 0;
  next_output_frame_ = 0;
}

void StreamingPhaseVocoder::reserve(size_t max_input_samples, size_t max_output_samples) {
  ensure_stream_state();
  input_.reserve(max_input_samples);
  const int n_bins = config_.n_fft / 2 + 1;
  const size_t max_padded = max_input_samples + static_cast<size_t>(config_.n_fft);
  const size_t max_analysis_frames = max_padded >= static_cast<size_t>(config_.n_fft)
                                         ? 1 + (max_padded - static_cast<size_t>(config_.n_fft)) /
                                                   static_cast<size_t>(config_.hop_length)
                                         : 1;
  analysis_frames_.reserve(max_analysis_frames * static_cast<size_t>(n_bins));
  ola_output_.reserve(max_output_samples + static_cast<size_t>(config_.n_fft));
  ola_window_sum_.reserve(max_output_samples + static_cast<size_t>(config_.n_fft));
}

void StreamingPhaseVocoder::push(const float* samples, size_t count) {
  if (count == 0) return;
  SONARE_CHECK(samples != nullptr, ErrorCode::InvalidParameter);
  input_.insert(input_.end(), samples, samples + count);
}

void StreamingPhaseVocoder::push(const Audio& audio) {
  SONARE_CHECK(audio.sample_rate() == config_.sample_rate, ErrorCode::InvalidParameter);
  push(audio.data(), audio.size());
}

int StreamingPhaseVocoder::latency_samples() const noexcept { return config_.n_fft / 2; }

void StreamingPhaseVocoder::bind_rate(float rate) {
  SONARE_CHECK(numeric::finite_positive(rate), ErrorCode::InvalidParameter);
  if (active_rate_ == 0.0f) {
    active_rate_ = rate;
    return;
  }
  SONARE_CHECK(std::abs(active_rate_ - rate) <= 1.0e-6f, ErrorCode::InvalidParameter);
}

void StreamingPhaseVocoder::ensure_stream_state() {
  if (fft_ != nullptr) return;

  fft_ = std::make_unique<FFT>(config_.n_fft);
  const auto analysis_window_handle = get_window_cached(WindowType::Hann, config_.win_length, true);
  const auto synthesis_window_handle =
      get_window_cached(WindowType::Hann, config_.win_length, false);
  const std::vector<float>& analysis_win_short = *analysis_window_handle;
  const std::vector<float>& synthesis_win_short = *synthesis_window_handle;

  analysis_window_.assign(static_cast<size_t>(config_.n_fft), 0.0f);
  synthesis_window_.assign(static_cast<size_t>(config_.n_fft), 0.0f);
  const int win_offset = (config_.n_fft - config_.win_length) / 2;
  std::copy(analysis_win_short.begin(), analysis_win_short.end(),
            analysis_window_.begin() + win_offset);
  std::copy(synthesis_win_short.begin(), synthesis_win_short.end(),
            synthesis_window_.begin() + win_offset);

  window_product_.resize(static_cast<size_t>(config_.n_fft));
  for (int i = 0; i < config_.n_fft; ++i) {
    window_product_[static_cast<size_t>(i)] =
        analysis_window_[static_cast<size_t>(i)] * synthesis_window_[static_cast<size_t>(i)];
  }

  const int n_bins = config_.n_fft / 2 + 1;
  frame_.assign(static_cast<size_t>(config_.n_fft), 0.0f);
  frame_spectrum_.assign(static_cast<size_t>(n_bins), {});
  phase_acc_.assign(static_cast<size_t>(n_bins), 0.0);
  mag_.assign(static_cast<size_t>(n_bins), 0.0f);
  ana_phase_.assign(static_cast<size_t>(n_bins), 0.0f);
  inst_freq_.assign(static_cast<size_t>(n_bins), 0.0f);
  nearest_peak_.assign(static_cast<size_t>(n_bins), -1);
  peaks_.reserve(static_cast<size_t>(n_bins));
}

void StreamingPhaseVocoder::analyze_available_frames(bool final) {
  ensure_stream_state();
  const int pad = config_.n_fft / 2;
  const size_t absolute_input_end = input_base_sample_ + input_.size();
  const size_t padded_length =
      absolute_input_end + static_cast<size_t>(pad) + (final ? static_cast<size_t>(pad) : 0);

  while (true) {
    const size_t start =
        static_cast<size_t>(next_analysis_frame_) * static_cast<size_t>(config_.hop_length);
    if (start + static_cast<size_t>(config_.n_fft) > padded_length) break;

    for (int i = 0; i < config_.n_fft; ++i) {
      const int64_t raw_index =
          static_cast<int64_t>(start) + static_cast<int64_t>(i) - static_cast<int64_t>(pad);
      const float sample = raw_index >= 0 && static_cast<size_t>(raw_index) >= input_base_sample_ &&
                                   static_cast<size_t>(raw_index) < absolute_input_end
                               ? input_[static_cast<size_t>(raw_index) - input_base_sample_]
                               : 0.0f;
      frame_[static_cast<size_t>(i)] = sample * analysis_window_[static_cast<size_t>(i)];
    }

    fft_->forward(frame_.data(), frame_spectrum_.data());
    analysis_frames_.insert(analysis_frames_.end(), frame_spectrum_.begin(), frame_spectrum_.end());
    ++next_analysis_frame_;
  }
}

void StreamingPhaseVocoder::synthesize_available_frames(bool final) {
  const int available_input_frames = next_analysis_frame_;
  if (available_input_frames - analysis_frame_base_ < 2) return;
  bind_rate(active_rate_);
  int target_output_frames = final ? 0 : next_output_frame_;
  if (final) {
    target_output_frames =
        checked_output_frames(config_.n_fft / 2 + 1, available_input_frames, active_rate_);
  } else {
    while (true) {
      const int t_in = static_cast<int>(static_cast<float>(target_output_frames) * active_rate_);
      if (t_in + 1 >= available_input_frames) break;
      ++target_output_frames;
    }
  }

  while (next_output_frame_ < target_output_frames) {
    synthesize_output_frame(next_output_frame_, active_rate_);
    ++next_output_frame_;
  }
}

const std::complex<float>& StreamingPhaseVocoder::analysis_frame_at(int frame,
                                                                    int bin) const noexcept {
  const int n_bins = config_.n_fft / 2 + 1;
  const int local_frame = frame - analysis_frame_base_;
  assert(local_frame >= 0);
  assert(bin >= 0 && bin < n_bins);
  assert(static_cast<size_t>(local_frame + 1) * static_cast<size_t>(n_bins) <=
         analysis_frames_.size());
  return analysis_frames_[static_cast<size_t>(local_frame) * static_cast<size_t>(n_bins) +
                          static_cast<size_t>(bin)];
}

void StreamingPhaseVocoder::synthesize_output_frame(int t_out, float rate) {
  ensure_stream_state();
  const int n_bins = config_.n_fft / 2 + 1;
  const int n_frames_in = next_analysis_frame_;
  const double time_step =
      static_cast<double>(config_.hop_length) / static_cast<double>(config_.sample_rate);

  float t_in_f = static_cast<float>(t_out) * rate;
  int t_in = static_cast<int>(t_in_f);
  float frac = t_in_f - static_cast<float>(t_in);
  if (t_in >= n_frames_in - 1) {
    t_in = n_frames_in - 2;
    frac = 1.0f;
  }
  if (t_in < 0) {
    t_in = 0;
    frac = 0.0f;
  }

  for (int k = 0; k < n_bins; ++k) {
    const auto frame0 = analysis_frame_at(t_in, k);
    const auto frame1 = analysis_frame_at(t_in + 1, k);
    const float mag0 = std::abs(frame0);
    const float mag1 = std::abs(frame1);
    mag_[static_cast<size_t>(k)] = mag0 * (1.0f - frac) + mag1 * frac;

    const float phase0 = std::arg(frame0);
    const float phase1 = std::arg(frame1);
    ana_phase_[static_cast<size_t>(k)] = phase0 + frac * phase::wrap(phase1 - phase0);

    const float bin_freq = static_cast<float>(k) * static_cast<float>(config_.sample_rate) /
                           static_cast<float>(config_.n_fft);
    const float expected_advance = kTwoPi * bin_freq * static_cast<float>(time_step);
    const float phase_diff = phase::wrap(phase1 - phase0 - expected_advance);
    inst_freq_[static_cast<size_t>(k)] =
        bin_freq + phase_diff / (kTwoPi * static_cast<float>(time_step));
  }

  phase::synthesize_locked_frame(mag_.data(), ana_phase_.data(), inst_freq_.data(), n_bins,
                                 config_.phase_lock, t_out == 0, time_step, phase_acc_, peaks_,
                                 nearest_peak_);
  for (int k = 0; k < n_bins; ++k) {
    frame_spectrum_[static_cast<size_t>(k)] = std::polar(
        mag_[static_cast<size_t>(k)], static_cast<float>(phase_acc_[static_cast<size_t>(k)]));
  }

  fft_->inverse(frame_spectrum_.data(), frame_.data());
  const size_t start = static_cast<size_t>(t_out) * static_cast<size_t>(config_.hop_length);
  const size_t needed = start + static_cast<size_t>(config_.n_fft);
  if (needed > ola_base_sample_) {
    const size_t local_needed = needed - ola_base_sample_;
    if (ola_output_.size() < local_needed) {
      ola_output_.resize(local_needed, 0.0f);
      ola_window_sum_.resize(local_needed, 0.0f);
    }
  }
  for (int i = 0; i < config_.n_fft; ++i) {
    const size_t idx = start + static_cast<size_t>(i);
    if (idx < ola_base_sample_) continue;
    const size_t local_idx = idx - ola_base_sample_;
    ola_output_[local_idx] +=
        frame_[static_cast<size_t>(i)] * synthesis_window_[static_cast<size_t>(i)];
    ola_window_sum_[local_idx] += window_product_[static_cast<size_t>(i)];
  }
}

float StreamingPhaseVocoder::normalized_output_sample(size_t user_sample) const noexcept {
  const size_t full_index = user_sample + static_cast<size_t>(config_.n_fft / 2);
  if (full_index < ola_base_sample_) return 0.0f;
  const size_t local_index = full_index - ola_base_sample_;
  if (local_index >= ola_output_.size()) return 0.0f;
  const float sum = local_index < ola_window_sum_.size() ? ola_window_sum_[local_index] : 0.0f;
  return sum > sonare::constants::kSpectrumEpsilon ? ola_output_[local_index] / sum
                                                   : ola_output_[local_index];
}

void StreamingPhaseVocoder::compact_buffers() {
  const int n_bins = config_.n_fft / 2 + 1;
  const int retained_frames =
      static_cast<int>(analysis_frames_.size() / static_cast<size_t>(n_bins));
  if (retained_frames > 0 && active_rate_ > 0.0f) {
    const int next_needed_input_frame = std::max(
        analysis_frame_base_,
        static_cast<int>(std::floor(static_cast<float>(next_output_frame_) * active_rate_)));
    // Interpolation always reads t_in and t_in + 1. Keep the final two retained
    // frames even when a fast rate advances past them between drains.
    const int frames_to_drop = std::clamp(next_needed_input_frame - analysis_frame_base_, 0,
                                          std::max(0, retained_frames - 2));
    if (frames_to_drop > 0) {
      const size_t bins_to_drop = static_cast<size_t>(frames_to_drop) * static_cast<size_t>(n_bins);
      analysis_frames_.erase(analysis_frames_.begin(),
                             analysis_frames_.begin() + static_cast<std::ptrdiff_t>(bins_to_drop));
      analysis_frame_base_ += frames_to_drop;
    }
  }

  const size_t next_frame_raw_start =
      static_cast<size_t>(next_analysis_frame_) * static_cast<size_t>(config_.hop_length);
  const size_t pad = static_cast<size_t>(config_.n_fft / 2);
  const size_t min_needed_input =
      next_frame_raw_start > pad ? next_frame_raw_start - pad : static_cast<size_t>(0);
  if (min_needed_input > input_base_sample_) {
    const size_t samples_to_drop = std::min(min_needed_input - input_base_sample_, input_.size());
    if (samples_to_drop > 0) {
      input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(samples_to_drop));
      input_base_sample_ += samples_to_drop;
    }
  }

  const size_t center = static_cast<size_t>(config_.n_fft / 2);
  const size_t min_needed_ola = emitted_output_samples_ + center;
  if (min_needed_ola > ola_base_sample_) {
    const size_t samples_to_drop = std::min(min_needed_ola - ola_base_sample_, ola_output_.size());
    if (samples_to_drop > 0) {
      ola_output_.erase(ola_output_.begin(),
                        ola_output_.begin() + static_cast<std::ptrdiff_t>(samples_to_drop));
      ola_window_sum_.erase(ola_window_sum_.begin(),
                            ola_window_sum_.begin() + static_cast<std::ptrdiff_t>(samples_to_drop));
      ola_base_sample_ += samples_to_drop;
    }
  }
}

Audio StreamingPhaseVocoder::drain_available(bool final) {
  size_t stable_user_samples = 0;
  if (final) {
    stable_user_samples =
        std::max<size_t>(1, checked_output_count(input_base_sample_ + input_.size(), active_rate_));
  } else {
    const size_t stable_full_samples =
        static_cast<size_t>(next_output_frame_) * static_cast<size_t>(config_.hop_length);
    const size_t center = static_cast<size_t>(config_.n_fft / 2);
    stable_user_samples = stable_full_samples > center ? stable_full_samples - center : 0;
  }

  if (stable_user_samples <= emitted_output_samples_) {
    return Audio::from_vector({}, config_.sample_rate);
  }

  std::vector<float> chunk(stable_user_samples - emitted_output_samples_);
  const size_t written = drain_into(final, chunk.data(), chunk.size());
  if (written == 0) {
    return Audio::from_vector({}, config_.sample_rate);
  }
  chunk.resize(written);
  return Audio::from_vector(std::move(chunk), config_.sample_rate);
}

size_t StreamingPhaseVocoder::drain_into(bool final, float* out, size_t out_capacity) {
  size_t stable_user_samples = 0;
  if (final) {
    stable_user_samples =
        std::max<size_t>(1, checked_output_count(input_base_sample_ + input_.size(), active_rate_));
  } else {
    const size_t stable_full_samples =
        static_cast<size_t>(next_output_frame_) * static_cast<size_t>(config_.hop_length);
    const size_t center = static_cast<size_t>(config_.n_fft / 2);
    stable_user_samples = stable_full_samples > center ? stable_full_samples - center : 0;
  }

  if (stable_user_samples <= emitted_output_samples_) return 0;
  const size_t available = stable_user_samples - emitted_output_samples_;
  SONARE_CHECK(out != nullptr || available == 0, ErrorCode::InvalidParameter);
  const size_t to_write = std::min(available, out_capacity);
  for (size_t i = 0; i < to_write; ++i) {
    out[i] = normalized_output_sample(emitted_output_samples_ + i);
  }
  emitted_output_samples_ += to_write;
  compact_buffers();
  return to_write;
}

Audio StreamingPhaseVocoder::process(const float* samples, size_t count, float rate) {
  bind_rate(rate);
  push(samples, count);
  if (input_.empty()) {
    return Audio::from_vector({}, config_.sample_rate);
  }

  analyze_available_frames(false);
  synthesize_available_frames(false);
  return drain_available(false);
}

Audio StreamingPhaseVocoder::process(const Audio& audio, float rate) {
  SONARE_CHECK(audio.sample_rate() == config_.sample_rate, ErrorCode::InvalidParameter);
  return process(audio.data(), audio.size(), rate);
}

size_t StreamingPhaseVocoder::process_into(const float* samples, size_t count, float rate,
                                           float* out, size_t out_capacity) {
  bind_rate(rate);
  push(samples, count);
  if (input_.empty()) return 0;

  analyze_available_frames(false);
  synthesize_available_frames(false);
  return drain_into(false, out, out_capacity);
}

Audio StreamingPhaseVocoder::finalize(float rate) {
  bind_rate(rate);
  analyze_available_frames(true);
  synthesize_available_frames(true);
  Audio tail = drain_available(true);
  reset();
  return tail;
}

size_t StreamingPhaseVocoder::finalize_into(float rate, float* out, size_t out_capacity) {
  bind_rate(rate);
  analyze_available_frames(true);
  synthesize_available_frames(true);
  const size_t written = drain_into(true, out, out_capacity);
  reset();
  return written;
}

Audio StreamingPhaseVocoder::finish(float rate) {
  SONARE_CHECK(emitted_output_samples_ == 0, ErrorCode::InvalidParameter);
  return finalize(rate);
}

Spectrogram phase_vocoder(const Spectrogram& spec, float rate, const PhaseVocoderConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(spec.n_frames() >= 2, ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_positive(rate), ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames_in = spec.n_frames();
  int n_fft = spec.n_fft();
  int hop_length = config.hop_length > 0 ? config.hop_length : spec.hop_length();
  int sample_rate = spec.sample_rate();
  // The stretched spectrum is only ever resynthesized by overlap-add at this
  // same hop, so the geometry is checked on the resolved pair rather than on
  // whichever of the two sources supplied it.
  validate_cola_geometry(n_fft, hop_length);

  /// Calculate output number of frames
  const int n_frames_out = checked_output_frames(n_bins, n_frames_in, rate);

  /// Get input complex spectrum
  const std::complex<float>* input = spec.complex_data();

  /// Output complex spectrum
  std::vector<std::complex<float>> output(static_cast<size_t>(n_bins) * n_frames_out);

  /// Phase accumulator (double precision to avoid drift over long signals).
  std::vector<double> phase_acc(n_bins, 0.0);

  /// Time step ratio (double precision: hop/sr is used to scale every per-frame
  /// phase advance, so single-precision rounding here biases the accumulator).
  const double time_step = static_cast<double>(hop_length) / static_cast<double>(sample_rate);

  for (int t_out = 0; t_out < n_frames_out; ++t_out) {
    /// Input time position
    float t_in_f = static_cast<float>(t_out) * rate;
    int t_in = static_cast<int>(t_in_f);
    float frac = t_in_f - static_cast<float>(t_in);

    /// Clamp to valid range
    if (t_in >= n_frames_in - 1) {
      t_in = n_frames_in - 2;
      frac = 1.0f;
    }
    if (t_in < 0) {
      t_in = 0;
      frac = 0.0f;
    }

    for (int k = 0; k < n_bins; ++k) {
      /// Get adjacent frames
      std::complex<float> frame0 = input[k * n_frames_in + t_in];
      std::complex<float> frame1 = input[k * n_frames_in + t_in + 1];

      /// Interpolate magnitude
      float mag0 = std::abs(frame0);
      float mag1 = std::abs(frame1);
      float mag = mag0 * (1.0f - frac) + mag1 * frac;

      /// Compute phase advance (analysis side stays in float — bounded per-frame).
      float phase0 = std::arg(frame0);
      float phase1 = std::arg(frame1);

      /// Expected phase advance based on bin frequency
      float bin_freq =
          static_cast<float>(k) * static_cast<float>(sample_rate) / static_cast<float>(n_fft);
      float expected_advance = kTwoPi * bin_freq * static_cast<float>(time_step);

      /// Phase difference with unwrapping
      float phase_diff = phase::wrap(phase1 - phase0 - expected_advance);
      float inst_freq = bin_freq + phase_diff / (kTwoPi * static_cast<float>(time_step));

      /// Accumulate phase in double precision.
      if (t_out == 0) {
        phase_acc[k] =
            static_cast<double>(phase0) +
            static_cast<double>(frac) * static_cast<double>(phase::wrap(phase1 - phase0));
      } else {
        phase_acc[k] += kTwoPiD * static_cast<double>(inst_freq) * time_step;
        phase_acc[k] = phase::wrap(phase_acc[k]);
      }

      /// Construct output complex value (cast back to float for FFT-domain storage).
      output[k * n_frames_out + t_out] = std::polar(mag, static_cast<float>(phase_acc[k]));
    }
  }

  return Spectrogram::from_complex(output.data(), n_bins, n_frames_out, n_fft, hop_length,
                                   sample_rate, spec.window(), spec.center(), spec.win_length());
}

Spectrogram phase_vocoder_phaselocked(const Spectrogram& spec, float rate,
                                      const PhaseVocoderConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(spec.n_frames() >= 2, ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_positive(rate), ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames_in = spec.n_frames();
  int n_fft = spec.n_fft();
  int hop_length = config.hop_length > 0 ? config.hop_length : spec.hop_length();
  int sample_rate = spec.sample_rate();
  validate_cola_geometry(n_fft, hop_length);

  const int n_frames_out = checked_output_frames(n_bins, n_frames_in, rate);

  const std::complex<float>* input = spec.complex_data();
  std::vector<std::complex<float>> output(static_cast<size_t>(n_bins) * n_frames_out);

  /// Synthesis phase accumulator (per bin, double precision to avoid drift).
  std::vector<double> phase_acc(n_bins, 0.0);

  /// Time step ratio in double precision (see phase_vocoder() for rationale).
  const double time_step = static_cast<double>(hop_length) / static_cast<double>(sample_rate);

  /// Reused per-frame scratch buffers (avoid per-frame heap churn).
  std::vector<float> mag(n_bins, 0.0f);
  std::vector<float> ana_phase(n_bins, 0.0f);
  std::vector<float> inst_freq(n_bins, 0.0f);
  std::vector<int> peaks;
  peaks.reserve(n_bins);
  std::vector<int> nearest_peak(n_bins, -1);

  for (int t_out = 0; t_out < n_frames_out; ++t_out) {
    float t_in_f = static_cast<float>(t_out) * rate;
    int t_in = static_cast<int>(t_in_f);
    float frac = t_in_f - static_cast<float>(t_in);

    if (t_in >= n_frames_in - 1) {
      t_in = n_frames_in - 2;
      frac = 1.0f;
    }
    if (t_in < 0) {
      t_in = 0;
      frac = 0.0f;
    }

    /// Magnitude, analysis phase and instantaneous frequency per bin (same path as
    /// phase_vocoder()).
    for (int k = 0; k < n_bins; ++k) {
      std::complex<float> frame0 = input[k * n_frames_in + t_in];
      std::complex<float> frame1 = input[k * n_frames_in + t_in + 1];

      float mag0 = std::abs(frame0);
      float mag1 = std::abs(frame1);
      mag[k] = mag0 * (1.0f - frac) + mag1 * frac;

      float phase0 = std::arg(frame0);
      float phase1 = std::arg(frame1);
      ana_phase[k] = phase0 + frac * phase::wrap(phase1 - phase0);

      float bin_freq =
          static_cast<float>(k) * static_cast<float>(sample_rate) / static_cast<float>(n_fft);
      float expected_advance = kTwoPi * bin_freq * static_cast<float>(time_step);
      float phase_diff = phase::wrap(phase1 - phase0 - expected_advance);
      inst_freq[k] = bin_freq + phase_diff / (kTwoPi * static_cast<float>(time_step));
    }

    phase::synthesize_locked_frame(mag.data(), ana_phase.data(), inst_freq.data(), n_bins,
                                   /*phase_lock=*/true, t_out == 0, time_step, phase_acc, peaks,
                                   nearest_peak);
    for (int k = 0; k < n_bins; ++k) {
      output[k * n_frames_out + t_out] = std::polar(mag[k], static_cast<float>(phase_acc[k]));
    }
  }

  return Spectrogram::from_complex(output.data(), n_bins, n_frames_out, n_fft, hop_length,
                                   sample_rate, spec.window(), spec.center(), spec.win_length());
}

}  // namespace sonare
