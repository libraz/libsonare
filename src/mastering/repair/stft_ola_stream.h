#pragma once

/// @file stft_ola_stream.h
/// @brief Block-driven multichannel STFT analysis and overlap-add synthesis for the streaming
///        repair processors.
///
/// Owns everything below the per-frame spectral operation: the input rings, the centred frame
/// grid (the first frame starts half a window before sample zero, over zeros), windowing and
/// FFT, masked resynthesis, normalization by the accumulated analysis*synthesis window sum, and
/// the delay queue primed with the reported latency. The arithmetic matches
/// Spectrogram::to_audio, so a processor built on it reaches the offline result delayed by its
/// latency. A processor supplies only what it does to one frame's spectra. Everything is sized in
/// prepare(); process() and reset() allocate nothing.

#include <algorithm>
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

#include "core/fft.h"
#include "core/window.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::repair::detail {

/// @brief Ring, overlap-add and delay-queue machinery shared by StreamingDenoise and
///        StreamingDereverb.
class StftOlaStream {
 public:
  /// @param n_fft Frame length; the ring and window length.
  /// @param hop_length Samples between frame starts.
  StftOlaStream(int n_fft, int hop_length)
      : n_fft_(n_fft),
        hop_length_(hop_length),
        n_bins_(n_fft / 2 + 1),
        prefix_(n_fft / 2),
        input_write_(prefix_),
        samples_to_next_frame_(n_fft - prefix_),
        fft_(n_fft) {}

  int n_bins() const noexcept { return n_bins_; }
  int active_channels() const noexcept { return active_channels_; }

  /// Spectra of the frame being processed, n_bins() values per active channel.
  std::complex<float>* spectra() noexcept { return spectra_.data(); }
  std::vector<std::complex<float>>& spectra_storage() noexcept { return spectra_; }

  /// @brief Sizes every buffer and primes the queue with @p latency_samples zeros via reset().
  /// @param latency_samples Delay reported to the host; the queue holds it plus one block.
  void prepare(int max_block_size, int max_channels, int latency_samples) {
    max_block_size_ = max_block_size;
    max_channels_ = max_channels;
    latency_samples_ = latency_samples;

    const auto fft = static_cast<std::size_t>(n_fft_);
    const auto bins = static_cast<std::size_t>(n_bins_);
    const auto channels = static_cast<std::size_t>(max_channels_);

    // Analysis periodic, synthesis symmetric, normalizer their product: the pair
    // Spectrogram::to_audio accumulates. win_length is n_fft, so neither window needs the zero
    // padding a shorter one would sit in.
    const auto periodic = get_window_cached(WindowType::Hann, n_fft_, true);
    const auto symmetric = get_window_cached(WindowType::Hann, n_fft_, false);
    analysis_window_.assign(periodic->begin(), periodic->end());
    synthesis_window_.assign(symmetric->begin(), symmetric->end());
    window_product_.assign(fft, 0.0f);
    for (std::size_t i = 0; i < fft; ++i) {
      window_product_[i] = analysis_window_[i] * synthesis_window_[i];
    }

    input_ring_.assign(fft * channels, 0.0f);
    frame_.assign(fft, 0.0f);
    spectra_.assign(bins * channels, std::complex<float>{});
    masked_.assign(bins, std::complex<float>{});
    synthesis_ring_.assign(fft * channels, 0.0f);
    window_sum_ring_.assign(fft, 0.0f);

    // The queue holds the reported delay plus at most one block, which is the most that can sit
    // in it: a block's own output is queued before it drains.
    queue_capacity_ = latency_samples_ + max_block_size_;
    output_queue_.assign(static_cast<std::size_t>(queue_capacity_) * channels, 0.0f);

    // A transform kind builds its backend state on its first use; build both here so no
    // process() block is the one that pays for it.
    fft_.prepare(/*real_forward=*/true, /*real_inverse=*/true, /*complex_forward=*/false);
  }

  /// @brief Returns rings, spectra and queue to their post-prepare state.
  /// @details The queue starts as latency_samples() zeros, so every block finds a block's worth
  ///   waiting from the first one onwards.
  void reset() {
    std::fill(input_ring_.begin(), input_ring_.end(), 0.0f);
    input_write_ = prefix_;
    samples_to_next_frame_ = n_fft_ - prefix_;
    std::fill(spectra_.begin(), spectra_.end(), std::complex<float>{});
    std::fill(synthesis_ring_.begin(), synthesis_ring_.end(), 0.0f);
    std::fill(window_sum_ring_.begin(), window_sum_ring_.end(), 0.0f);
    ring_base_ = 0;
    next_frame_start_ = 0;
    std::fill(output_queue_.begin(), output_queue_.end(), 0.0f);
    queue_read_ = 0;
    queue_size_ = std::min(latency_samples_, queue_capacity_);
    active_channels_ = 0;
  }

  /// @brief Runs one host block in place.
  /// @param name Processor name used in error messages.
  /// @param on_frame Called with every channel's spectrum of a completed frame in spectra();
  ///   expected to call emit_frame() for it, or to skip emission while its mask lags.
  /// @param after_frames Called with whether the block took any frame, before the output is
  ///   handed back, so a poisoned sample is zeroed on its way out.
  /// @throws SonareException(InvalidParameter) for a block wider or longer than prepared, or a
  ///   channel count differing from the one this run started with;
  ///   SonareException(InvalidState) if the queue underruns its reported latency.
  template <class OnFrame, class AfterFrames>
  void process(float* const* channels, int num_channels, int num_samples, const char* name,
               OnFrame&& on_frame, AfterFrames&& after_frames) {
    if (num_channels > max_channels_) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string("num_channels exceeds prepared ") + name + " capacity");
    }
    if (num_samples > max_block_size_) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string("num_samples exceeds prepared ") + name + " block size");
    }
    if (active_channels_ == 0) active_channels_ = num_channels;
    if (num_channels != active_channels_) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(name) +
                                " channel count must not change between resets: one mask is "
                                "built from every channel of the block at once");
    }

    const auto fft = static_cast<std::size_t>(n_fft_);
    bool analyzed = false;
    for (int i = 0; i < num_samples; ++i) {
      for (int ch = 0; ch < num_channels; ++ch) {
        input_ring_[fft * static_cast<std::size_t>(ch) + static_cast<std::size_t>(input_write_)] =
            channels[ch][i];
      }
      input_write_ = input_write_ + 1 == n_fft_ ? 0 : input_write_ + 1;
      if (--samples_to_next_frame_ == 0) {
        analyze_channels();
        on_frame();
        analyzed = true;
        samples_to_next_frame_ = hop_length_;
      }
    }

    after_frames(analyzed);

    // Reads the arithmetic above rather than trusting it: an underrun here would otherwise
    // leave the host a block of stale samples and a silent desync.
    if (queue_size_ < num_samples) {
      throw SonareException(ErrorCode::InvalidState,
                            std::string(name) + " output queue underran its reported latency");
    }
    const auto capacity = static_cast<std::size_t>(queue_capacity_);
    for (int i = 0; i < num_samples; ++i) {
      for (int ch = 0; ch < num_channels; ++ch) {
        channels[ch][i] = output_queue_[capacity * static_cast<std::size_t>(ch) +
                                        static_cast<std::size_t>(queue_read_)];
      }
      queue_read_ = queue_read_ + 1 == queue_capacity_ ? 0 : queue_read_ + 1;
      --queue_size_;
    }
  }

  /// @brief Masks @p source by the real @p gains, overlap-adds it and finalizes one hop.
  /// @param source n_bins() values per active channel, laid out as spectra().
  void emit_frame(const double* gains, const std::complex<float>* source) {
    const auto fft = static_cast<std::size_t>(n_fft_);
    const auto bins = static_cast<std::size_t>(n_bins_);
    const std::size_t base = next_frame_start_ % fft;

    for (int ch = 0; ch < active_channels_; ++ch) {
      const std::complex<float>* channel = source + bins * static_cast<std::size_t>(ch);
      for (int b = 0; b < n_bins_; ++b) {
        masked_[static_cast<std::size_t>(b)] = {static_cast<float>(channel[b].real() * gains[b]),
                                                static_cast<float>(channel[b].imag() * gains[b])};
      }
      fft_.inverse(masked_.data(), frame_.data());
      float* ring = synthesis_ring_.data() + fft * static_cast<std::size_t>(ch);
      for (int i = 0; i < n_fft_; ++i) {
        const std::size_t offset = base + static_cast<std::size_t>(i);
        const std::size_t pos = offset < fft ? offset : offset - fft;
        // Kept as two statements so the multiply and the add stay unfused, as they are in
        // IstftAccumulator and in to_audio's Eigen expression before it.
        const float contribution =
            frame_[static_cast<std::size_t>(i)] * synthesis_window_[static_cast<std::size_t>(i)];
        ring[pos] += contribution;
      }
    }
    for (int i = 0; i < n_fft_; ++i) {
      const std::size_t offset = base + static_cast<std::size_t>(i);
      const std::size_t pos = offset < fft ? offset : offset - fft;
      window_sum_ring_[pos] += window_product_[static_cast<std::size_t>(i)];
    }

    next_frame_start_ += static_cast<std::size_t>(hop_length_);
    // No later frame starts below the next frame's start, so one hop is now final.
    finalize_before(next_frame_start_);
  }

  /// @brief Zeroes any non-finite value in the rings and queue; the fill count and cursors are
  ///        untouched, since they decide how many samples a block still owes.
  /// @details The input ring is included because the sample that caused the discard is still in
  ///   it and would poison the next frame.
  /// @return true when anything was discarded.
  bool discard_non_finite_state() noexcept {
    bool discarded = discard_run_if_non_finite(input_ring_.begin(), input_ring_.end(), 0.0f);
    discarded |= discard_run_if_non_finite(synthesis_ring_.begin(), synthesis_ring_.end(), 0.0f);
    discarded |= discard_run_if_non_finite(window_sum_ring_.begin(), window_sum_ring_.end(), 0.0f);
    // Values only; the fill count is what this block's output owes the caller.
    discarded |= discard_run_if_non_finite(output_queue_.begin(), output_queue_.end(), 0.0f);
    return discarded;
  }

 private:
  /// Windows and transforms every active channel's frame into spectra().
  void analyze_channels() {
    const auto fft = static_cast<std::size_t>(n_fft_);
    const auto bins = static_cast<std::size_t>(n_bins_);
    for (int ch = 0; ch < active_channels_; ++ch) {
      const float* ring = input_ring_.data() + fft * static_cast<std::size_t>(ch);
      // The write cursor points at the oldest ring sample, which is this frame's first: the
      // frame is taken the moment the ring holds exactly its span.
      for (int i = 0; i < n_fft_; ++i) {
        const int pos = input_write_ + i < n_fft_ ? input_write_ + i : input_write_ + i - n_fft_;
        frame_[static_cast<std::size_t>(i)] =
            ring[pos] * analysis_window_[static_cast<std::size_t>(i)];
      }
      fft_.forward(frame_.data(), spectra_.data() + bins * static_cast<std::size_t>(ch));
    }
  }

  /// Moves every synthesis-ring sample below absolute index @p end into the delay queue,
  /// normalized by the accumulated analysis*synthesis window sum.
  void finalize_before(std::size_t end) {
    using sonare::constants::kSpectrumEpsilon;
    const auto fft = static_cast<std::size_t>(n_fft_);
    const auto capacity = static_cast<std::size_t>(queue_capacity_);
    while (ring_base_ < end) {
      const std::size_t pos = ring_base_ % fft;
      const float window_sum = window_sum_ring_[pos];
      // The centred padding ahead of sample zero is walked and cleared without being enqueued,
      // as to_audio trims it, so nothing leaks on wrap.
      const bool enqueue = ring_base_ >= static_cast<std::size_t>(prefix_);
      const auto write =
          enqueue ? static_cast<std::size_t>((queue_read_ + queue_size_) % queue_capacity_) : 0;
      for (int ch = 0; ch < active_channels_; ++ch) {
        const std::size_t lane = fft * static_cast<std::size_t>(ch) + pos;
        const float raw = synthesis_ring_[lane];
        // to_audio's select verbatim: the guard is a strict > eps and the divisor is still
        // clamped, so a lane that fails it keeps its un-normalized sample.
        if (enqueue) {
          output_queue_[capacity * static_cast<std::size_t>(ch) + write] =
              window_sum > kSpectrumEpsilon ? raw / std::max(window_sum, kSpectrumEpsilon) : raw;
        }
        synthesis_ring_[lane] = 0.0f;
      }
      window_sum_ring_[pos] = 0.0f;
      ++ring_base_;
      if (enqueue) ++queue_size_;
    }
  }

  int n_fft_;
  int hop_length_;
  int n_bins_;
  int prefix_;
  int max_block_size_ = 0;
  int max_channels_ = 0;
  int active_channels_ = 0;
  int latency_samples_ = 0;

  std::vector<float> analysis_window_;
  std::vector<float> synthesis_window_;
  std::vector<float> window_product_;

  // One n_fft ring per channel; reset() places the write cursor at prefix_ so the first frame
  // holds the offline centring's zeros ahead of host sample zero.
  std::vector<float> input_ring_;
  int input_write_;
  int samples_to_next_frame_;

  std::vector<float> frame_;
  std::vector<std::complex<float>> spectra_;
  std::vector<std::complex<float>> masked_;

  // One n_fft ring per channel plus the window sum they share, since the mask is one real
  // number per cell and every channel carries it.
  std::vector<float> synthesis_ring_;
  std::vector<float> window_sum_ring_;
  std::size_t ring_base_ = 0;
  std::size_t next_frame_start_ = 0;

  std::vector<float> output_queue_;
  int queue_capacity_ = 0;
  int queue_read_ = 0;
  int queue_size_ = 0;

  FFT fft_;
};

}  // namespace sonare::mastering::repair::detail
