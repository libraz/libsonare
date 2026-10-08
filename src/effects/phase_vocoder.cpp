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
#include "util/time_map.h"

namespace sonare {

using sonare::constants::kTwoPi;
using sonare::constants::kTwoPiD;

namespace {

int checked_output_frames(int n_bins, int input_frames, const TimeStretchMap& map) {
  SONARE_CHECK(n_bins > 0 && input_frames > 0, ErrorCode::InvalidParameter);
  const size_t output_frames = static_cast<size_t>(map.output_frame_count(input_frames));
  size_t output_elements = 0;
  SONARE_CHECK(numeric::checked_size_product(static_cast<size_t>(n_bins), output_frames,
                                             kMaxAudioBufferSize, &output_elements),
               ErrorCode::InvalidParameter);
  (void)output_elements;
  return static_cast<int>(output_frames);
}

StreamingPhaseVocoderConfig normalized_streaming_config(StreamingPhaseVocoderConfig config) {
  if (config.win_length <= 0) config.win_length = config.n_fft;
  return config;
}

Spectrogram stretch_spectrogram(const Spectrogram& spec, float rate,
                                const PhaseVocoderConfig& config, bool phase_lock) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(spec.n_frames() >= 2, ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_positive(rate), ErrorCode::InvalidParameter);

  const int n_bins = spec.n_bins();
  const int n_frames_in = spec.n_frames();
  const int n_fft = spec.n_fft();
  const int hop_length = config.hop_length > 0 ? config.hop_length : spec.hop_length();
  // The stretched spectrum is only ever resynthesized by overlap-add at this
  // same hop, so the geometry is checked on the resolved pair rather than on
  // whichever of the two sources supplied it.
  PhaseVocoderSynthesizer synth(n_fft, hop_length, spec.sample_rate(), spec.window(),
                                spec.win_length(), phase_lock);
  SONARE_CHECK(synth.n_bins() == n_bins, ErrorCode::InvalidParameter);

  const TimeStretchMap map(rate);
  const int n_frames_out = checked_output_frames(n_bins, n_frames_in, map);
  const std::complex<float>* input = spec.complex_data();
  std::vector<std::complex<float>> output(static_cast<size_t>(n_bins) *
                                          static_cast<size_t>(n_frames_out));
  std::vector<std::complex<float>> frame0(static_cast<size_t>(n_bins));
  std::vector<std::complex<float>> frame1(static_cast<size_t>(n_bins));

  for (int t_out = 0; t_out < n_frames_out; ++t_out) {
    int t_in = 0;
    float frac = 0.0f;
    PhaseVocoderSynthesizer::locate_input_frame(map, t_out, n_frames_in, &t_in, &frac);
    for (int k = 0; k < n_bins; ++k) {
      const size_t column = static_cast<size_t>(k) * static_cast<size_t>(n_frames_in);
      frame0[static_cast<size_t>(k)] = input[column + static_cast<size_t>(t_in)];
      frame1[static_cast<size_t>(k)] = input[column + static_cast<size_t>(t_in) + 1];
    }
    synth.next_frame(frame0.data(), frame1.data(), frac);
    for (int k = 0; k < n_bins; ++k) {
      output[static_cast<size_t>(k) * static_cast<size_t>(n_frames_out) +
             static_cast<size_t>(t_out)] =
          std::polar(synth.magnitude()[k], static_cast<float>(synth.synthesis_phase()[k]));
    }
  }

  return Spectrogram::from_complex(output.data(), n_bins, n_frames_out, n_fft, hop_length,
                                   spec.sample_rate(), spec.window(), spec.center(),
                                   spec.win_length());
}

}  // namespace

PhaseVocoderSynthesizer::PhaseVocoderSynthesizer(int n_fft, int hop_length, int sample_rate,
                                                 WindowType window, int win_length, bool phase_lock)
    : n_fft_(n_fft), n_bins_(n_fft / 2 + 1), sample_rate_(sample_rate), phase_lock_(phase_lock) {
  SONARE_CHECK(sample_rate > 0, ErrorCode::InvalidParameter);
  validate_cola_geometry(n_fft, hop_length, window, win_length);
  time_step_ = static_cast<double>(hop_length) / static_cast<double>(sample_rate);
  const size_t bins = static_cast<size_t>(n_bins_);
  magnitude_.assign(bins, 0.0f);
  analysis_phase_.assign(bins, 0.0f);
  inst_freq_.assign(bins, 0.0f);
  accumulator_.assign(bins, 0.0);
  synthesis_phase_.assign(bins, 0.0);
  peaks_.reserve(bins);
  nearest_peak_.assign(bins, -1);
}

void PhaseVocoderSynthesizer::locate_input_frame(const TimeStretchMap& map, int t_out,
                                                 int n_frames_in, int* t_in, float* frac) {
  const float t_in_f = map.input_position(t_out);
  *t_in = static_cast<int>(t_in_f);
  *frac = t_in_f - static_cast<float>(*t_in);
  if (*t_in >= n_frames_in - 1) {
    *t_in = n_frames_in - 2;
    *frac = 1.0f;
  }
  if (*t_in < 0) {
    *t_in = 0;
    *frac = 0.0f;
  }
}

void PhaseVocoderSynthesizer::next_frame(const std::complex<float>* frame0,
                                         const std::complex<float>* frame1, float frac) {
  const float time_step = static_cast<float>(time_step_);
  for (int k = 0; k < n_bins_; ++k) {
    const size_t bin = static_cast<size_t>(k);
    magnitude_[bin] = std::abs(frame0[k]) * (1.0f - frac) + std::abs(frame1[k]) * frac;
    const float phase0 = std::arg(frame0[k]);
    const float phase1 = std::arg(frame1[k]);
    analysis_phase_[bin] = phase0 + frac * phase::wrap(phase1 - phase0);
    const float bin_freq =
        static_cast<float>(k) * static_cast<float>(sample_rate_) / static_cast<float>(n_fft_);
    const float expected_advance = kTwoPi * bin_freq * time_step;
    const float phase_diff = phase::wrap(phase1 - phase0 - expected_advance);
    inst_freq_[bin] = bin_freq + phase_diff / (kTwoPi * time_step);
  }

  if (first_frame_) {
    std::copy(analysis_phase_.begin(), analysis_phase_.end(), accumulator_.begin());
    first_frame_ = false;
  }
  phase::synthesize_locked_frame(magnitude_.data(), analysis_phase_.data(), accumulator_.data(),
                                 n_bins_, phase_lock_, synthesis_phase_.data(), peaks_,
                                 nearest_peak_);
  // The transition leading out of this frame is applied only after it is emitted.
  for (size_t bin = 0; bin < synthesis_phase_.size(); ++bin) {
    accumulator_[bin] = phase::wrap(synthesis_phase_[bin] +
                                    kTwoPiD * static_cast<double>(inst_freq_[bin]) * time_step_);
  }
}

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

StreamingPhaseVocoder::StreamingPhaseVocoder(StreamingPhaseVocoderConfig config)
    : config_(normalized_streaming_config(config)),
      synth_(config_.n_fft, config_.hop_length, config_.sample_rate, WindowType::Hann,
             config_.win_length, config_.phase_lock) {}

void StreamingPhaseVocoder::reset() {
  input_.clear();
  input_base_sample_ = 0;
  ola_base_sample_ = 0;
  emitted_output_samples_ = 0;
  map_bound_ = false;
  finalized_ = false;
  analysis_frames_.clear();
  synth_.reset();
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
  // Rewritten in place rather than assigned from a map built here: building one
  // would allocate on a path documented as allocation-free after reserve().
  if (active_map_.constant()) {
    if (active_map_.constant_rate() == rate) {
      map_bound_ = true;
      return;
    }
    // Two constant maps' single pieces coincide only at the same rate, so a
    // changed rate cannot agree over a frame already synthesized.
    SONARE_CHECK(!map_bound_ || next_output_frame_ <= 0, ErrorCode::InvalidParameter);
    active_map_.assign(rate);
    map_bound_ = true;
    return;
  }
  bind_map(TimeStretchMap(rate));
}

void StreamingPhaseVocoder::bind_map(const TimeStretchMap& map) {
  // Frames already synthesized fix input the stream has discarded; above them
  // the profile is still free.
  SONARE_CHECK(!map_bound_ || active_map_.agrees_through(map, next_output_frame_ - 1),
               ErrorCode::InvalidParameter);
  active_map_ = map;
  map_bound_ = true;
}

void StreamingPhaseVocoder::ensure_stream_state() {
  if (fft_ != nullptr) return;

  fft_ = std::make_unique<FFT>(config_.n_fft);
  // reserve() promises an allocation-free processing path, so both directions are built here.
  fft_->prepare(/*real_forward=*/true, /*real_inverse=*/true, /*complex_forward=*/false);
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

  frame_.assign(static_cast<size_t>(config_.n_fft), 0.0f);
  frame_spectrum_.assign(static_cast<size_t>(synth_.n_bins()), {});
}

void StreamingPhaseVocoder::analyze_available_frames(bool final) {
  ensure_stream_state();
  const int pad = config_.n_fft / 2;
  const size_t absolute_input_end = input_base_sample_ + input_.size();
  size_t padded_length =
      absolute_input_end + static_cast<size_t>(pad) + (final ? static_cast<size_t>(pad) : 0);
  // Synthesis interpolates between frame pairs, so non-empty input shorter than
  // a hop is padded to a second analysis frame rather than left unsynthesized.
  if (final && absolute_input_end > 0) {
    padded_length = std::max(padded_length, static_cast<size_t>(config_.hop_length) +
                                                static_cast<size_t>(config_.n_fft));
  }

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
  SONARE_CHECK(map_bound_, ErrorCode::InvalidParameter);
  int target_output_frames = final ? 0 : next_output_frame_;
  if (final) {
    target_output_frames =
        checked_output_frames(config_.n_fft / 2 + 1, available_input_frames, active_map_);
  } else {
    while (true) {
      const int t_in = static_cast<int>(active_map_.input_position(target_output_frames));
      if (t_in + 1 >= available_input_frames) break;
      ++target_output_frames;
    }
  }

  while (next_output_frame_ < target_output_frames) {
    synthesize_output_frame(next_output_frame_);
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

void StreamingPhaseVocoder::synthesize_output_frame(int t_out) {
  ensure_stream_state();
  const int n_bins = synth_.n_bins();
  int t_in = 0;
  float frac = 0.0f;
  PhaseVocoderSynthesizer::locate_input_frame(active_map_, t_out, next_analysis_frame_, &t_in,
                                              &frac);
  synth_.next_frame(&analysis_frame_at(t_in, 0), &analysis_frame_at(t_in + 1, 0), frac);
  for (int k = 0; k < n_bins; ++k) {
    frame_spectrum_[static_cast<size_t>(k)] =
        std::polar(synth_.magnitude()[k], static_cast<float>(synth_.synthesis_phase()[k]));
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
  if (retained_frames > 0 && map_bound_) {
    const int next_needed_input_frame =
        std::max(analysis_frame_base_,
                 static_cast<int>(std::floor(active_map_.input_position(next_output_frame_))));
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
        active_map_.output_sample_count(input_base_sample_ + input_.size(), config_.hop_length);
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

size_t StreamingPhaseVocoder::undelivered_samples(bool final) const {
  size_t stable_user_samples = 0;
  if (final) {
    stable_user_samples =
        active_map_.output_sample_count(input_base_sample_ + input_.size(), config_.hop_length);
  } else {
    const size_t stable_full_samples =
        static_cast<size_t>(next_output_frame_) * static_cast<size_t>(config_.hop_length);
    const size_t center = static_cast<size_t>(config_.n_fft / 2);
    stable_user_samples = stable_full_samples > center ? stable_full_samples - center : 0;
  }
  return stable_user_samples > emitted_output_samples_
             ? stable_user_samples - emitted_output_samples_
             : 0;
}

size_t StreamingPhaseVocoder::drain_into(bool final, float* out, size_t out_capacity) {
  const size_t available = undelivered_samples(final);
  if (available == 0) return 0;
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
  // A tail that did not fit stays for the next call; the stream ends once it is out.
  if (undelivered_samples(true) == 0) reset();
  return written;
}

Audio StreamingPhaseVocoder::finish(float rate) {
  SONARE_CHECK(emitted_output_samples_ == 0, ErrorCode::InvalidParameter);
  return finalize(rate);
}

Spectrogram phase_vocoder_analysis(const Audio& audio, int n_fft, int hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;
  if (hop_length <= 0 || audio.size() >= static_cast<size_t>(hop_length)) {
    return Spectrogram::compute(audio, stft_config);
  }
  std::vector<float> extended(audio.begin(), audio.end());
  extended.resize(static_cast<size_t>(hop_length), 0.0f);
  return Spectrogram::compute(Audio::from_vector(std::move(extended), audio.sample_rate()),
                              stft_config);
}

Spectrogram phase_vocoder(const Spectrogram& spec, float rate, const PhaseVocoderConfig& config) {
  return stretch_spectrogram(spec, rate, config, /*phase_lock=*/false);
}

Spectrogram phase_vocoder_phaselocked(const Spectrogram& spec, float rate,
                                      const PhaseVocoderConfig& config) {
  return stretch_spectrogram(spec, rate, config, /*phase_lock=*/true);
}

}  // namespace sonare
