#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "streaming/stream_analyzer.h"
#include "streaming/stream_analyzer_utils.h"
#include "util/exception.h"

namespace sonare {

using namespace streaming_detail;

void StreamAnalyzer::process(const float* samples, size_t n_samples, size_t sample_offset) {
  if (samples == nullptr || n_samples == 0) {
    process_internal(samples, n_samples);
    publish_stats_snapshot();
    return;
  }
  if (n_samples > std::numeric_limits<size_t>::max() - sample_offset) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamAnalyzer external sample offset overflows");
  }
  if (offset_tracking_mode_ == OffsetTrackingMode::Internal) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "cannot switch StreamAnalyzer offset mode without reset()");
  }
  if (offset_tracking_mode_ == OffsetTrackingMode::External &&
      sample_offset != next_external_sample_offset_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamAnalyzer external sample offset is not contiguous; call reset()");
  }

  /// Seed the buffered frame-start position once. Re-seeding it for every
  /// chunk would relabel a partial frame with the final chunk's offset.
  /// NOTE: this overload is only exact when no internal resampling is active.
  /// When the input sample rate exceeds kMaxDirectSampleRate the analyzer
  /// resamples (introducing filter latency and a changed sample count), so the
  /// re-seeded offset cannot account for the resampler's delay — frame-sample
  /// offsets derived from it are then best-effort/approximate. Use the
  /// offset-less process() overload (which advances a continuous internal
  /// position) when resampling is in play.
  if (offset_tracking_mode_ == OffsetTrackingMode::Unset) {
    cumulative_samples_ = sample_offset;
    cumulative_samples_exact_ = static_cast<double>(sample_offset);
    offset_tracking_mode_ = OffsetTrackingMode::External;
  }
  process_internal(samples, n_samples);
  next_external_sample_offset_ = sample_offset + n_samples;
  publish_stats_snapshot();
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
    /// (start-up filter latency). finalize() will drain that delayed tail.
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

  process_complete_frames();
}

void StreamAnalyzer::process_complete_frames() {
  /// Process complete frames.
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

  // Advance the persistent high-rate resampler through its filter latency and
  // append exactly the analytic rounded output length. This happens only at
  // end-of-stream, so the zero input used to advance the filter cannot create a
  // discontinuity between live chunks.
  if (needs_resampling_) {
    resample_buffer_.clear();
    stream_resampler_->finalize(resample_buffer_);
    const size_t previous_size = overlap_buffer_.size();
    overlap_buffer_.resize(previous_size + resample_buffer_.size());
    if (normalization_gain_ != 1.0f) {
      for (size_t i = 0; i < resample_buffer_.size(); ++i) {
        overlap_buffer_[previous_size + i] = resample_buffer_[i] * normalization_gain_;
      }
    } else {
      std::copy(resample_buffer_.begin(), resample_buffer_.end(),
                overlap_buffer_.begin() + static_cast<std::ptrdiff_t>(previous_size));
    }
    process_complete_frames();
  }

  if (overlap_buffer_.empty()) {
    flush_pending_chord();
    publish_stats_snapshot();
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

  // Emit the chord still held after every terminal frame has been analyzed.
  // The live path only appends when the chord changes, so the last held chord
  // otherwise never reaches chord_progression.
  flush_pending_chord();
  publish_stats_snapshot();
}

void StreamAnalyzer::emit_frame(const float* frame_start, size_t frame_sample_offset,
                                bool force_emit) {
  ++emitted_frame_count_;
  const bool will_emit = force_emit || emitted_frame_count_ >= config_.emit_every_n_frames;
  if (will_emit) {
    emitted_frame_count_ = 0;
    size_t write_index = 0;
    if (try_begin_output_write(&write_index)) {
      process_single_frame(frame_start, frame_sample_offset, output_buffer_[write_index]);
      publish_output_write();
      return;
    }
  }
  process_single_frame(frame_start, frame_sample_offset, scratch_frame_);
}

void StreamAnalyzer::append_onset(float value) {
  if (onset_accumulator_size_ < onset_accumulator_.size()) {
    const size_t index =
        (onset_accumulator_start_ + onset_accumulator_size_) % onset_accumulator_.size();
    onset_accumulator_[index] = value;
    ++onset_accumulator_size_;
    return;
  }
  onset_accumulator_[onset_accumulator_start_] = value;
  onset_accumulator_start_ = (onset_accumulator_start_ + 1) % onset_accumulator_.size();
}

void StreamAnalyzer::append_recent_chroma(const std::array<float, 12>& chroma) {
  if (chroma_history_size_ < chroma_history_.size()) {
    const size_t index = (chroma_history_start_ + chroma_history_size_) % chroma_history_.size();
    chroma_history_[index] = chroma;
    ++chroma_history_size_;
    return;
  }
  chroma_history_[chroma_history_start_] = chroma;
  chroma_history_start_ = (chroma_history_start_ + 1) % chroma_history_.size();
}

void StreamAnalyzer::append_full_chroma(const std::array<float, 12>& chroma) {
  if (full_chroma_history_size_ < full_chroma_history_.size()) {
    const size_t index =
        (full_chroma_history_start_ + full_chroma_history_size_) % full_chroma_history_.size();
    full_chroma_history_[index] = chroma;
    ++full_chroma_history_size_;
    return;
  }
  full_chroma_history_[full_chroma_history_start_] = chroma;
  full_chroma_history_start_ = (full_chroma_history_start_ + 1) % full_chroma_history_.size();
  ++full_chroma_history_offset_;
}

std::array<float, 12> StreamAnalyzer::median_recent_chroma() {
  return compute_median_chroma(chroma_history_, chroma_history_start_, chroma_history_size_,
                               median_chroma_scratch_);
}

std::array<float, 12> StreamAnalyzer::median_full_chroma(size_t start, size_t count) {
  const size_t physical_start = (full_chroma_history_start_ + start) % full_chroma_history_.size();
  return compute_median_chroma(full_chroma_history_, physical_start, count, median_chroma_scratch_);
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

void StreamAnalyzer::process_single_frame(const float* frame_start, size_t sample_offset,
                                          StreamFrame& frame) {
  frame.magnitude.clear();
  frame.mel.clear();
  frame.chroma.clear();
  frame.spectral_centroid = 0.0f;
  frame.spectral_flatness = 0.0f;
  frame.rms_energy = 0.0f;
  frame.onset_strength = 0.0f;
  frame.onset_valid = false;
  frame.chord_root = -1;
  frame.chord_quality = 0;
  frame.chord_confidence = 0.0f;

  /// Invalidate the per-frame smoothed-chord cache. It is repopulated below only
  /// when this frame actually runs chord detection, so the progressive consumers
  /// fall back to recomputing whenever that block is skipped.
  frame_chord_cache_valid_ = false;

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
  if (needs_mel_analysis_) {
    compute_mel();
  }
  if (config_.compute_mel) {
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
      append_recent_chroma(current_chroma);

      /// Store to the fixed full-chroma ring for retroactive bar detection.
      /// Once full, appending overwrites the oldest frame in O(1).
      append_full_chroma(current_chroma);

      /// Compute median-filtered chroma (more robust to noise than averaging)
      std::array<float, 12> smoothed_chroma = median_recent_chroma();

      /// Find best chord using smoothed chroma
      auto [best_chord, chord_corr] = find_best_chord(smoothed_chroma.data(), chord_templates_);

      /// Cache this frame's detection so update_progressive_estimate() and
      /// update_bar_chord_tracking() — which run next over the same, unmodified
      /// chroma_history_ — reuse it instead of recomputing the same median and
      /// find_best_chord().
      frame_chord_root_ = static_cast<int>(best_chord.root);
      frame_chord_quality_ = static_cast<int>(best_chord.quality);
      frame_chord_corr_ = chord_corr;
      frame_chord_cache_valid_ = true;

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
      append_onset(frame.onset_strength);
    }
  }

  /// Compute spectral features
  if (config_.compute_spectral) {
    compute_spectral_features(frame);
  }

  /// Compute RMS energy (from time-domain)
  frame.rms_energy = compute_rms_frame(frame_start, config_.n_fft);
}

}  // namespace sonare
