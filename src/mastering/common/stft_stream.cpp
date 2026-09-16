#include "mastering/common/stft_stream.h"

#include <algorithm>
#include <cstdint>

#include "core/window.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/reflect_padding.h"
#include "util/validated.h"

namespace sonare::mastering::common {

using sonare::constants::kSpectrumEpsilon;

namespace {

/// @brief Window of @p win_length zero-padded to @p n_fft at (n_fft - win_length) / 2.
/// @details The same construction Spectrogram::compute and Spectrogram::to_audio
///          each inline; mirrored here rather than shared because spectrum.cpp's
///          copy is file-local.
std::vector<float> padded_window_of(WindowType window, int win_length, int n_fft, bool periodic) {
  const auto handle = get_window_cached(window, win_length, periodic);
  std::vector<float> padded(static_cast<std::size_t>(n_fft), 0.0f);
  std::copy(handle->begin(), handle->end(), padded.begin() + (n_fft - win_length) / 2);
  return padded;
}

}  // namespace

StftFrameReader::StftFrameReader(const float* samples, std::size_t size, int sample_rate,
                                 const StftConfig& config)
    : StftFrameReader(samples, size, sample_rate, Validated<StftConfig>::make(config).get(),
                      ValidatedTag{}) {}

StftFrameReader::StftFrameReader(const float* samples, std::size_t size, int sample_rate,
                                 const StftConfig& checked, ValidatedTag)
    : samples_(samples),
      size_(size),
      padded_length_(0),
      pad_(static_cast<std::size_t>(checked.n_fft / 2)),
      sample_rate_(sample_rate),
      n_fft_(checked.n_fft),
      hop_length_(checked.hop_length),
      n_bins_(checked.n_fft / 2 + 1),
      n_frames_(stft_frame_count(size, checked)),
      center_(checked.center),
      pad_mode_(checked.pad_mode),
      padded_window_(
          padded_window_of(checked.window, checked.actual_win_length(), checked.n_fft, true)),
      fft_(checked.n_fft),
      frame_(static_cast<std::size_t>(checked.n_fft), 0.0f),
      frame_spectrum_(static_cast<std::size_t>(checked.n_fft / 2 + 1)) {
  SONARE_CHECK_MSG(samples != nullptr || size == 0, ErrorCode::InvalidParameter,
                   "StftFrameReader: samples must not be null for a non-empty signal");
  padded_length_ = center_ ? size_ + 2 * pad_ : size_;
}

float StftFrameReader::padded_sample(std::size_t index) const {
  // pad_center and reflect_center_pad both return an all-zero buffer for an
  // absent signal, so no branch below may reach samples_ in that case.
  if (samples_ == nullptr || size_ == 0) return 0.0f;
  if (!center_) return samples_[index];

  const std::int64_t offset = static_cast<std::int64_t>(index) - static_cast<std::int64_t>(pad_);
  if (pad_mode_ == PadMode::Reflect) {
    return samples_[reflect_index(offset, size_)];
  }
  if (offset < 0 || offset >= static_cast<std::int64_t>(size_)) return 0.0f;
  return samples_[static_cast<std::size_t>(offset)];
}

const float* StftFrameReader::interior_span(std::size_t start, int count) const {
  if (samples_ == nullptr || size_ == 0) return nullptr;
  if (!center_) return samples_ + start;
  if (start < pad_) return nullptr;
  if (start - pad_ + static_cast<std::size_t>(count) > size_) return nullptr;
  return samples_ + (start - pad_);
}

const std::complex<float>* StftFrameReader::frame(int index) {
  SONARE_CHECK_MSG(index >= 0 && index < n_frames_, ErrorCode::InvalidParameter,
                   "StftFrameReader::frame: index out of range");

  const std::size_t start = static_cast<std::size_t>(index) * static_cast<std::size_t>(hop_length_);
  const std::size_t remaining = start < padded_length_ ? padded_length_ - start : 0;
  const int valid_samples = static_cast<int>(std::min(static_cast<std::size_t>(n_fft_), remaining));

  const float* interior = valid_samples > 0 ? interior_span(start, valid_samples) : nullptr;
  if (interior != nullptr) {
    for (int i = 0; i < valid_samples; ++i) {
      frame_[i] = interior[i] * padded_window_[i];
    }
  } else {
    for (int i = 0; i < valid_samples; ++i) {
      frame_[i] = padded_sample(start + static_cast<std::size_t>(i)) * padded_window_[i];
    }
  }
  std::fill(frame_.begin() + std::max(valid_samples, 0), frame_.end(), 0.0f);

  fft_.forward(frame_.data(), frame_spectrum_.data());
  return frame_spectrum_.data();
}

IstftAccumulator::IstftAccumulator(int n_frames, int sample_rate, const StftConfig& config,
                                   int target_length)
    : IstftAccumulator(n_frames, sample_rate, Validated<StftConfig>::make(config).get(),
                       target_length, ValidatedTag{}) {}

IstftAccumulator::IstftAccumulator(int n_frames, int sample_rate, const StftConfig& checked,
                                   int target_length, ValidatedTag)
    : n_frames_(n_frames),
      sample_rate_(sample_rate),
      n_fft_(checked.n_fft),
      hop_length_(checked.hop_length),
      n_bins_(checked.n_fft / 2 + 1),
      pushed_(0),
      full_length_(0),
      trim_start_(checked.center ? static_cast<std::size_t>(checked.n_fft / 2) : 0),
      trim_end_(0),
      ring_base_(0),
      synthesis_window_(
          padded_window_of(checked.window, checked.actual_win_length(), checked.n_fft, false)),
      window_product_(
          padded_window_of(checked.window, checked.actual_win_length(), checked.n_fft, true)),
      ring_output_(static_cast<std::size_t>(checked.n_fft), 0.0f),
      ring_window_sum_(static_cast<std::size_t>(checked.n_fft), 0.0f),
      frame_(static_cast<std::size_t>(checked.n_fft), 0.0f),
      trimmed_(),
      fft_(checked.n_fft) {
  SONARE_CHECK_MSG(n_frames > 0, ErrorCode::InvalidParameter,
                   "IstftAccumulator: nFrames must be positive");
  // to_audio's length <= 0 branch trims to the valid region instead, which has
  // no streaming counterpart here: the result length would not be known until
  // the last frame. Refuse it by name rather than silently taking this branch.
  SONARE_CHECK_MSG(target_length > 0, ErrorCode::InvalidParameter,
                   "IstftAccumulator: targetLength must be positive");

  // window_product_ enters as the periodic analysis window and becomes
  // analysis * synthesis, the normalizer to_audio accumulates.
  for (std::size_t i = 0; i < window_product_.size(); ++i) {
    window_product_[i] *= synthesis_window_[i];
  }

  full_length_ = static_cast<std::size_t>(n_frames - 1) * static_cast<std::size_t>(hop_length_) +
                 static_cast<std::size_t>(n_fft_);
  const std::size_t available =
      trim_start_ < full_length_
          ? std::min(static_cast<std::size_t>(target_length), full_length_ - trim_start_)
          : 0;
  trim_end_ = trim_start_ + available;
  trimmed_.assign(static_cast<std::size_t>(target_length), 0.0f);
}

void IstftAccumulator::finalize_before(std::size_t end) {
  const std::size_t ring_size = static_cast<std::size_t>(n_fft_);
  while (ring_base_ < end) {
    const std::size_t pos = ring_base_ % ring_size;
    const float window_sum = ring_window_sum_[pos];
    const float raw = ring_output_[pos];
    // to_audio's select verbatim: the guard is a strict > eps and the divisor is
    // still clamped, so a lane that fails the guard keeps its un-normalized sample.
    const float value =
        window_sum > kSpectrumEpsilon ? raw / std::max(window_sum, kSpectrumEpsilon) : raw;
    if (ring_base_ >= trim_start_ && ring_base_ < trim_end_) {
      trimmed_[ring_base_ - trim_start_] = value;
    }
    ring_output_[pos] = 0.0f;
    ring_window_sum_[pos] = 0.0f;
    ++ring_base_;
  }
}

void IstftAccumulator::push(const std::complex<float>* frame) {
  SONARE_CHECK_MSG(frame != nullptr, ErrorCode::InvalidParameter,
                   "IstftAccumulator::push: frame must not be null");
  SONARE_CHECK_MSG(pushed_ < n_frames_, ErrorCode::InvalidParameter,
                   "IstftAccumulator::push: more frames pushed than declared");

  const std::size_t start =
      static_cast<std::size_t>(pushed_) * static_cast<std::size_t>(hop_length_);
  // Everything below the new frame's start is final: no later frame starts
  // earlier. This also covers a hop > n_fft gap, whose samples leave the ring
  // already cleared and so carry a zero window sum, as they do in to_audio.
  finalize_before(start);

  fft_.inverse(frame, frame_.data());

  const std::size_t ring_size = static_cast<std::size_t>(n_fft_);
  const std::size_t base = start % ring_size;
  for (int i = 0; i < n_fft_; ++i) {
    const std::size_t offset = base + static_cast<std::size_t>(i);
    const std::size_t pos = offset < ring_size ? offset : offset - ring_size;
    // Kept as two statements so the multiply and the add stay unfused, matching
    // the separate SIMD product and sum to_audio's Eigen expression emits.
    const float contribution = frame_[i] * synthesis_window_[i];
    ring_output_[pos] += contribution;
    ring_window_sum_[pos] += window_product_[i];
  }
  ++pushed_;
}

Audio IstftAccumulator::finish() {
  SONARE_CHECK_MSG(pushed_ == n_frames_, ErrorCode::InvalidParameter,
                   "IstftAccumulator::finish: fewer frames pushed than declared");
  finalize_before(full_length_);
  return Audio::from_vector(std::move(trimmed_), sample_rate_);
}

}  // namespace sonare::mastering::common
