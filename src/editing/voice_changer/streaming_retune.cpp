#include "editing/voice_changer/streaming_retune.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"

namespace sonare::editing::voice_changer {

using sonare::constants::kSemitonesPerOctave;
using sonare::constants::kSpectrumEpsilon;
using sonare::constants::kTwoPi;

namespace {
constexpr int kGrainSize = 2048;        // ~46 ms at 44.1k; acceptable preview latency.
constexpr float kMaxSemitones = 24.0f;  // Clamp shift range to +/- 2 octaves.
}  // namespace

StreamingRetune::StreamingRetune(StreamingRetuneConfig config) : config_(config) {}

void StreamingRetune::update_ratio() noexcept {
  const float semis = std::clamp(config_.semitones, -kMaxSemitones, kMaxSemitones);
  pitch_ratio_ = std::pow(2.0, static_cast<double>(semis) / kSemitonesPerOctave);
}

void StreamingRetune::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;

  grain_size_ = kGrainSize;
  hop_a_ = grain_size_ / 4;
  ring_cap_ = static_cast<std::size_t>(4 * grain_size_);
  accum_cap_ = static_cast<std::size_t>(2 * grain_size_);

  // Precompute periodic Hann window.
  window_.assign(static_cast<std::size_t>(grain_size_), 0.0f);
  for (int n = 0; n < grain_size_; ++n) {
    const float phase = kTwoPi * static_cast<float>(n) / static_cast<float>(grain_size_);
    window_[static_cast<std::size_t>(n)] = 0.5f * (1.0f - std::cos(phase));
  }

  ring_buf_.assign(ring_cap_, 0.0f);
  synth_acc_.assign(accum_cap_, 0.0f);
  norm_acc_.assign(accum_cap_, 0.0f);

  update_ratio();
  reset();
}

void StreamingRetune::reset() {
  std::fill(ring_buf_.begin(), ring_buf_.end(), 0.0f);
  std::fill(synth_acc_.begin(), synth_acc_.end(), 0.0f);
  std::fill(norm_acc_.begin(), norm_acc_.end(), 0.0f);
  write_head_ = 0;
  input_phase_ = 0;
  drain_pos_ = 0;
  synth_pos_ = 0;
}

void StreamingRetune::set_config(const StreamingRetuneConfig& config) {
  config_ = config;
  update_ratio();
}

float StreamingRetune::read_ring_linear(double position) const noexcept {
  // Wrap fractional position into [0, ring_cap_) and linearly interpolate.
  const double cap = static_cast<double>(ring_cap_);
  double pos = std::fmod(position, cap);
  if (pos < 0.0) {
    pos += cap;
  }
  const std::size_t i0 = static_cast<std::size_t>(pos);
  const std::size_t i1 = (i0 + 1) % ring_cap_;
  const float frac = static_cast<float>(pos - static_cast<double>(i0));
  return ring_buf_[i0] * (1.0f - frac) + ring_buf_[i1] * frac;
}

void StreamingRetune::emit_grain() noexcept {
  // Grain-resampling pitch shift: a grain of grain_size output samples is
  // sourced from grain_size * pitch_ratio input samples via linear
  // interpolation. The source window ends at the current write head (most
  // recent complete grain). Grains are overlap-added at hop_a on both the
  // analysis and synthesis sides; the per-grain resampling is what shifts
  // pitch. For pitch_ratio > 1 (positive semitones) the read advances faster
  // than one sample per output sample, packing more signal cycles into the
  // fixed grain length and raising the pitch.
  const double source_span = static_cast<double>(grain_size_) * pitch_ratio_;
  // Start so the resampled grain ends at the latest sample written.
  const double start = static_cast<double>(write_head_) - source_span;

  for (int n = 0; n < grain_size_; ++n) {
    const double read_pos = start + static_cast<double>(n) * pitch_ratio_;
    const float w = window_[static_cast<std::size_t>(n)];
    const float sample = read_ring_linear(read_pos) * w;
    const std::size_t idx = (synth_pos_ + static_cast<std::size_t>(n)) % accum_cap_;
    synth_acc_[idx] += sample;
    norm_acc_[idx] += w * w;
  }

  synth_pos_ = (synth_pos_ + static_cast<std::size_t>(hop_a_)) % accum_cap_;
}

void StreamingRetune::process_block(const float* input, float* output, int num_samples) {
  if (sample_rate_ <= 0.0) {
    throw std::logic_error("StreamingRetune must be prepared before processing");
  }
  if (num_samples < 0 || num_samples > max_block_size_) {
    throw std::invalid_argument("invalid block size");
  }
  if ((input == nullptr || output == nullptr) && num_samples > 0) {
    throw std::invalid_argument("buffers must not be null");
  }
  if (num_samples == 0) {
    return;
  }

  const float mix = std::clamp(config_.mix, 0.0f, 1.0f);

  for (int i = 0; i < num_samples; ++i) {
    // 1) Write incoming sample into the history ring.
    ring_buf_[write_head_ % ring_cap_] = input[i];
    ++write_head_;

    // 2) Emit a grain every hop_a samples.
    if (++input_phase_ >= hop_a_) {
      input_phase_ = 0;
      emit_grain();
    }

    // 3) Drain exactly one output sample from the front of the OLA accumulator.
    const float norm = norm_acc_[drain_pos_] + kSpectrumEpsilon;
    const float out_sample = synth_acc_[drain_pos_] / norm;
    synth_acc_[drain_pos_] = 0.0f;
    norm_acc_[drain_pos_] = 0.0f;
    drain_pos_ = (drain_pos_ + 1) % accum_cap_;

    output[i] = input[i] * (1.0f - mix) + out_sample * mix;
  }
}

}  // namespace sonare::editing::voice_changer
