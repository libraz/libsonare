#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "streaming/stream_analyzer.h"
#include "streaming/stream_analyzer_utils.h"

namespace sonare {

using namespace streaming_detail;

void StreamAnalyzer::process(const float* samples, size_t n_samples, size_t sample_offset) {
  /// Sync cumulative samples with the caller-supplied original-rate offset.
  /// NOTE: this overload is only exact when no internal resampling is active.
  /// When the input sample rate exceeds kMaxDirectSampleRate the analyzer
  /// resamples (introducing filter latency and a changed sample count), so the
  /// re-seeded offset cannot account for the resampler's delay — frame-sample
  /// offsets derived from it are then best-effort/approximate. Use the
  /// offset-less process() overload (which advances a continuous internal
  /// position) when resampling is in play.
  cumulative_samples_ = sample_offset;
  cumulative_samples_exact_ = static_cast<double>(sample_offset);
  process_internal(samples, n_samples);
}

void StreamAnalyzer::process_internal(const float* samples, size_t n_samples) {
  finalized_ = false;
  if (samples == nullptr || n_samples == 0) {
    return;
  }

  /// Sanitize input first: replace any NaN/Inf with 0 so a single bad input
  /// sample cannot poison the FFT and, through it, every downstream estimate
  /// (mel, chroma, onset, spectral features) for the rest of the stream. Done
  /// before resampling so corrupted values never enter the resampler's filter
  /// history either. The sanitized copy reuses a persistent scratch buffer.
  const float* clean_samples = sanitize_into(samples, n_samples, sanitize_buffer_);

  const float* process_samples = clean_samples;
  size_t process_n_samples = n_samples;

  /// Resample if needed (for high sample rates like 96000 Hz). Use the
  /// persistent, phase-continuous resampler so chunk boundaries stay seamless
  /// (no per-chunk click / drift). Output is appended into resample_buffer_,
  /// which we clear first because its previous contents were already drained
  /// into overlap_buffer_ on the prior call.
  if (needs_resampling_) {
    resample_buffer_.clear();
    stream_resampler_->process(clean_samples, n_samples, resample_buffer_);
    process_samples = resample_buffer_.data();
    process_n_samples = resample_buffer_.size();
    /// The stateful resampler can return 0 samples for a short first chunk
    /// (start-up filter latency). Nothing to append yet in that case.
    if (process_n_samples == 0) {
      return;
    }
  }

  /// Append (resampled) samples to overlap buffer with normalization gain
  size_t prev_size = overlap_buffer_.size();
  overlap_buffer_.resize(prev_size + process_n_samples);
  if (normalization_gain_ != 1.0f) {
    for (size_t i = 0; i < process_n_samples; ++i) {
      overlap_buffer_[prev_size + i] = process_samples[i] * normalization_gain_;
    }
  } else {
    std::copy(process_samples, process_samples + process_n_samples,
              overlap_buffer_.begin() + prev_size);
  }

  /// Process complete frames
  int n_fft = config_.n_fft;
  int hop_length = config_.hop_length;

  /// Process frames using a read offset into overlap_buffer_ instead of erasing
  /// hop_length samples per frame (which is an O(N) memmove every hop).
  while (overlap_buffer_.size() - overlap_read_pos_ >= static_cast<size_t>(n_fft)) {
    /// Calculate sample offset for this frame (in original sample rate)
    size_t frame_sample_offset = cumulative_samples_;

    emit_frame(overlap_buffer_.data() + overlap_read_pos_, frame_sample_offset, false);

    /// Slide read position by hop_length (deferred compaction below)
    overlap_read_pos_ += static_cast<size_t>(hop_length);

    /// Update cumulative samples (in original sample rate)
    cumulative_samples_exact_ +=
        static_cast<double>(hop_length) / static_cast<double>(resample_ratio_);
    cumulative_samples_ = static_cast<size_t>(std::llround(cumulative_samples_exact_));
    ++frame_count_;

    /// Update progressive estimate if needed
    float current_time_sec = static_cast<float>(cumulative_samples_) / config_.sample_rate;
    update_progressive_estimate(current_time_sec);
  }

  /// Compact the consumed prefix once per chunk (single memmove) so the buffer
  /// does not grow unbounded while keeping the unprocessed tail for overlap.
  if (overlap_read_pos_ > 0) {
    overlap_buffer_.erase(overlap_buffer_.begin(),
                          overlap_buffer_.begin() + static_cast<std::ptrdiff_t>(overlap_read_pos_));
    overlap_read_pos_ = 0;
  }

  /// Capacity guard (safety net): after compaction the unconsumed tail is
  /// normally < n_fft, because the frame loop above drains every complete frame
  /// whenever the buffer reaches n_fft samples. A pathological caller, however,
  /// could feed sub-frame chunks (each smaller than n_fft) for a very long time
  /// in a config where a frame is never completed, letting overlap_buffer_ grow
  /// without an upper bound. Cap it at a generous multiple of n_fft and drop the
  /// oldest excess so a long-running session cannot leak. Under correct
  /// frame-sized operation this branch is never taken, so normal behavior is
  /// unchanged.
  const size_t kMaxOverlapSamples = static_cast<size_t>(config_.n_fft) * 10;
  if (overlap_buffer_.size() > kMaxOverlapSamples) {
    const size_t drop = overlap_buffer_.size() - kMaxOverlapSamples;
    overlap_buffer_.erase(overlap_buffer_.begin(),
                          overlap_buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
  }
}

void StreamAnalyzer::finalize() {
  if (finalized_) {
    return;
  }
  finalized_ = true;

  if (overlap_read_pos_ > 0) {
    overlap_buffer_.erase(overlap_buffer_.begin(),
                          overlap_buffer_.begin() + static_cast<std::ptrdiff_t>(overlap_read_pos_));
    overlap_read_pos_ = 0;
  }
  if (overlap_buffer_.empty()) {
    return;
  }

  std::vector<float> padded(static_cast<size_t>(config_.n_fft), 0.0f);
  const size_t copy_count = std::min(overlap_buffer_.size(), padded.size());
  std::copy(overlap_buffer_.begin(),
            overlap_buffer_.begin() + static_cast<std::ptrdiff_t>(copy_count), padded.begin());

  emit_frame(padded.data(), cumulative_samples_, true);
  cumulative_samples_exact_ +=
      static_cast<double>(copy_count) / static_cast<double>(resample_ratio_);
  cumulative_samples_ = static_cast<size_t>(std::llround(cumulative_samples_exact_));
  ++frame_count_;
  update_progressive_estimate(static_cast<float>(cumulative_samples_) / config_.sample_rate);
  overlap_buffer_.clear();
}

void StreamAnalyzer::emit_frame(const float* frame_start, size_t frame_sample_offset,
                                bool force_emit) {
  StreamFrame frame = process_single_frame(frame_start, frame_sample_offset);

  ++emitted_frame_count_;
  if (force_emit || emitted_frame_count_ >= config_.emit_every_n_frames) {
    emitted_frame_count_ = 0;
    output_buffer_.push_back(std::move(frame));
  }
}

const float* StreamAnalyzer::sanitize_into(const float* src, size_t n_samples,
                                           std::vector<float>& dst) {
  dst.resize(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    const float v = src[i];
    /// std::isfinite is false for NaN and +/-Inf; replace those with silence.
    dst[i] = std::isfinite(v) ? v : 0.0f;
  }
  return dst.data();
}

StreamFrame StreamAnalyzer::process_single_frame(const float* frame_start, size_t sample_offset) {
  StreamFrame frame;

  /// Calculate timestamp
  frame.timestamp = static_cast<float>(sample_offset) / static_cast<float>(config_.sample_rate);
  frame.frame_index = frame_count_;

  /// Compute STFT
  compute_stft(frame_start);

  /// Copy magnitude if requested
  if (config_.compute_magnitude) {
    int downsample = config_.magnitude_downsample;
    int output_bins = config_.n_bins() / downsample;
    frame.magnitude.resize(output_bins);
    for (int i = 0; i < output_bins; ++i) {
      frame.magnitude[i] = magnitude_[i * downsample];
    }
  }

  /// Compute mel spectrogram
  if (config_.compute_mel) {
    compute_mel();
    frame.mel = mel_buffer_;
  }

  /// Compute chroma
  if (config_.compute_chroma) {
    compute_chroma();
    frame.chroma = chroma_buffer_;

    /// Accumulate for key estimation
    for (int i = 0; i < 12; ++i) {
      chroma_sum_[i] += chroma_buffer_[i];
    }
    ++chroma_frame_count_;

    /// Detect chord for this frame using smoothed chroma
    if (!chord_templates_.empty() && chroma_buffer_.size() == 12) {
      /// Add current chroma to history
      std::array<float, 12> current_chroma;
      std::copy(chroma_buffer_.begin(), chroma_buffer_.end(), current_chroma.begin());
      chroma_history_.push_back(current_chroma);

      /// Keep history limited to smoothing window
      while (chroma_history_.size() > static_cast<size_t>(kChordSmoothingFrames)) {
        chroma_history_.pop_front();
      }

      /// Store to full chroma history for retroactive bar detection. Trim the
      /// oldest frame with an O(1) deque pop_front instead of an O(N) vector
      /// erase(begin()) memmove; the retained content is identical.
      full_chroma_history_.push_back(current_chroma);
      while (full_chroma_history_.size() > kMaxChromaHistoryFrames) {
        full_chroma_history_.pop_front();
        /// Advance the absolute index of the surviving front frame so
        /// retroactive bar timing stays correct once old frames are dropped.
        ++full_chroma_history_offset_;
      }

      /// Compute median-filtered chroma (more robust to noise than averaging)
      std::array<float, 12> smoothed_chroma = compute_median_chroma(chroma_history_);

      /// Find best chord using smoothed chroma
      auto [best_chord, chord_corr] = find_best_chord(smoothed_chroma.data(), chord_templates_);

      /// Only report chord if confidence is above threshold
      if (chord_corr >= kChordConfidenceThreshold) {
        frame.chord_root = static_cast<int>(best_chord.root);
        frame.chord_quality = static_cast<int>(best_chord.quality);
        frame.chord_confidence = chord_corr;
      } else {
        /// Low confidence: keep previous chord or default to C major
        frame.chord_root = (prev_chord_root_ >= 0) ? prev_chord_root_ : 0;
        frame.chord_quality = (prev_chord_quality_ >= 0) ? prev_chord_quality_ : 0;
        frame.chord_confidence = std::max(0.0f, chord_corr);
      }
    }
  }

  /// Compute onset strength
  if (config_.compute_onset) {
    /// Save state before compute_onset() modifies it
    bool had_prev_frame = has_prev_frame_;
    frame.onset_strength = compute_onset();
    frame.onset_valid = had_prev_frame;

    /// Accumulate for BPM estimation, bounded to a sliding window. Without this
    /// cap the accumulator grew once per valid onset frame for the whole stream
    /// (~10k entries for a 4-minute track) and every BPM update re-scanned the
    /// entire history, so memory and CPU grew monotonically. Trimming from the
    /// front keeps the most-recent onset_window_frames_ frames, which still far
    /// exceeds the autocorrelation's maximum lag.
    if (frame.onset_valid) {
      onset_accumulator_.push_back(frame.onset_strength);
      while (onset_accumulator_.size() > onset_window_frames_) {
        onset_accumulator_.pop_front();
      }
    }
  }

  /// Compute spectral features
  if (config_.compute_spectral) {
    compute_spectral_features(frame);
  }

  /// Compute RMS energy (from time-domain)
  frame.rms_energy = compute_rms_frame(frame_start, config_.n_fft);

  return frame;
}

}  // namespace sonare
