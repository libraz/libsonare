#pragma once

/// @file stft_stream.h
/// @brief Frame-at-a-time STFT analysis and overlap-add synthesis, bit-identical
///        to Spectrogram::compute / Spectrogram::to_audio.
///
/// A consumer that only ever looks at one frame at a time still pays for the
/// whole [n_bins x n_frames] plane when it goes through Spectrogram. These two
/// helpers keep the same arithmetic and drop the plane: the reader synthesizes
/// the centred padding per frame instead of building a padded copy of the
/// signal, and the accumulator overlap-adds through a ring of n_fft samples,
/// emitting each output sample once no later frame can reach it.
///
/// Bit-identity is the contract, not an approximation: same window pair, same
/// FFT plan size, same per-sample accumulation order, and the same
/// window_sum > kSpectrumEpsilon select. It is what lets a caller swap a
/// Spectrogram round trip for these without moving a golden hash.

#include <complex>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "core/fft.h"
#include "core/spectrum.h"

namespace sonare::mastering::common {

/// @brief Frame-wise forward STFT over a caller-owned sample buffer.
/// @details Reproduces Spectrogram::compute cell for cell without materializing
///          the spectrum or a padded copy of the signal. @p samples must outlive
///          the reader.
///
///          Frames may be requested in any order and any number of times; each
///          call recomputes from the source buffer, so nothing carries over
///          between calls.
///
///          One divergence from Spectrogram::compute, at its only input it
///          short-circuits on: compute returns an empty Spectrogram for empty
///          audio, while the reader follows stft_frame_count and reports the
///          frames the padded signal would yield, every one of them zero.
class StftFrameReader {
 public:
  /// @brief Binds a reader to @p samples under @p config.
  /// @param samples Unpadded signal; must remain valid for the reader's lifetime.
  /// @param size Length of @p samples in samples.
  /// @param sample_rate Rate the frames belong to; carried, not used by the transform.
  /// @param config Analysis geometry, validated as Spectrogram::compute validates it.
  /// @throws SonareException(InvalidParameter) if @p config is unusable, or if
  ///         @p samples is null with a non-zero @p size.
  StftFrameReader(const float* samples, std::size_t size, int sample_rate,
                  const StftConfig& config);

  /// @brief Returns the frame count, equal to stft_frame_count(size, config).
  int n_frames() const { return n_frames_; }

  /// @brief Returns the one-sided bin count, n_fft / 2 + 1.
  int n_bins() const { return n_bins_; }

  /// @brief Returns the rate passed to the constructor.
  int sample_rate() const { return sample_rate_; }

  /// @brief Computes frame @p index and returns a view of n_bins() complex values.
  /// @details Valid until the next call. Frames may be requested in any order.
  /// @throws SonareException(InvalidParameter) if @p index is out of range.
  const std::complex<float>* frame(int index);

 private:
  /// Selects the constructor whose config has already passed validation.
  struct ValidatedTag {};

  StftFrameReader(const float* samples, std::size_t size, int sample_rate,
                  const StftConfig& checked, ValidatedTag);

  /// Value at @p index of the signal the framing loop walks, padding included.
  float padded_sample(std::size_t index) const;

  /// Start of a contiguous run of @p count padded samples, or null if the run
  /// crosses into synthesized padding and must be gathered element by element.
  const float* interior_span(std::size_t start, int count) const;

  const float* samples_;
  std::size_t size_;
  std::size_t padded_length_;
  std::size_t pad_;
  int sample_rate_;
  int n_fft_;
  int hop_length_;
  int n_bins_;
  int n_frames_;
  bool center_;
  PadMode pad_mode_;
  std::vector<float> padded_window_;
  FFT fft_;
  std::vector<float> frame_;
  std::vector<std::complex<float>> frame_spectrum_;
};

/// @brief Streaming overlap-add synthesis matching Spectrogram::to_audio(length).
/// @details Working state is O(n_fft) and independent of the frame count: a ring
///          of n_fft output samples and n_fft window sums, plus the inverse
///          transform's own scratch. A sample leaves the ring once the next
///          frame starts past it, which is where to_audio's normalize-and-trim
///          is applied to it.
///
///          Only to_audio's length > 0 branch is implemented; a non-positive
///          target length is rejected rather than falling back to the
///          trim-to-valid-region behaviour.
class IstftAccumulator {
 public:
  /// @brief Prepares synthesis for exactly @p n_frames frames.
  /// @param n_frames Frames that will be pushed; must be positive.
  /// @param sample_rate Rate of the returned Audio.
  /// @param config Geometry the frames were analyzed under.
  /// @param target_length Length of the returned Audio in samples; must be positive.
  /// @throws SonareException(InvalidParameter) if @p config is unusable, or if
  ///         @p n_frames or @p target_length is not positive.
  IstftAccumulator(int n_frames, int sample_rate, const StftConfig& config, int target_length);

  /// @brief Returns the frame count the accumulator was built for.
  int n_frames() const { return n_frames_; }

  /// @brief Returns the one-sided bin count each pushed frame must carry.
  int n_bins() const { return n_bins_; }

  /// @brief Overlap-adds one frame of n_bins() complex values.
  /// @details Frames must be pushed in ascending order starting at 0.
  /// @throws SonareException(InvalidParameter) if @p frame is null or more than
  ///         n_frames() frames are pushed.
  void push(const std::complex<float>* frame);

  /// @brief Finishes the overlap-add and returns the trimmed, normalized signal.
  /// @throws SonareException(InvalidParameter) if fewer than n_frames() frames
  ///         were pushed.
  Audio finish();

 private:
  /// Selects the constructor whose config has already passed validation.
  struct ValidatedTag {};

  IstftAccumulator(int n_frames, int sample_rate, const StftConfig& checked, int target_length,
                   ValidatedTag);

  /// Normalizes, emits and clears every ring sample below absolute index @p end.
  void finalize_before(std::size_t end);

  int n_frames_;
  int sample_rate_;
  int n_fft_;
  int hop_length_;
  int n_bins_;
  int pushed_;
  std::size_t full_length_;
  std::size_t trim_start_;
  std::size_t trim_end_;
  std::size_t ring_base_;
  std::vector<float> synthesis_window_;
  std::vector<float> window_product_;
  std::vector<float> ring_output_;
  std::vector<float> ring_window_sum_;
  std::vector<float> frame_;
  std::vector<float> trimmed_;
  FFT fft_;
};

}  // namespace sonare::mastering::common
