#pragma once

/// @file stream_analyzer.h
/// @brief Streaming audio analyzer for real-time visualization.

#include <array>
#include <complex>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "analysis/chord_templates.h"
#include "streaming/stream_config.h"
#include "streaming/stream_frame.h"
#include "streaming/stream_resampler.h"

namespace sonare {

// Forward declarations
class FFT;
struct StreamAnalyzerPublication;

/// @brief Number of enumerators in ChordQuality.
/// @details Alias of @ref sonare::kChordQualityCount, which lives next to the
///          enum in util/types.h and is the one place the cardinality is
///          written. Kept as a name here because the streaming bar-synchronized
///          chord vote table is sized from it and reads better spelled this
///          way; a static_assert in stream_analyzer.cpp still checks that the
///          vote table matches, so appending a quality widens the table instead
///          of silently dropping the new value.
inline constexpr int kNumChordQualities = kChordQualityCount;

/// @brief Streaming audio analyzer for real-time visualization.
/// @details Processes audio in chunks, maintaining overlap state between calls.
/// Produces StreamFrame objects with timestamp and features.
///
/// Timestamps represent "stream time" (input sample position), not necessarily
/// AudioContext.currentTime. See documentation for synchronization guidance.
///
/// @par Feature coverage vs. the batch MusicAnalyzer
/// Everything emitted here is causal: computable from the samples seen so far.
/// That covers magnitude/mel/chroma spectra, onset strength, per-frame spectral
/// centroid / flatness / RMS, and progressive BPM, key and chord estimates.
///
/// Excluded because they are non-causal or global: melody contour (feasible per
/// frame, but monophonic-only and an extra autocorrelation per hop — run
/// feature::yin_track offline), EBU R128 LRA (defined over the whole program;
/// per-frame RMS is the closest causal proxy), section boundaries (song-wide
/// self-similarity), and the aggregated Timbre summary (average the per-frame
/// primitives offline).
///
/// Usage:
/// @code
///   StreamAnalyzer analyzer(config);
///
///   analyzer.process(samples, n_samples);
///   auto frames = analyzer.read_frames(10);
///   for (const auto& frame : frames) {
///     visualize(frame);
///   }
/// @endcode
///
/// @par Thread-safety
/// One producer thread may call process(), process(..., sample_offset), or
/// finalize() while one consumer thread concurrently calls available_frames(),
/// one of the read_frames*() methods, stats(), frame_count(), or current_time().
/// Completed frames and immutable statistics snapshots are published with
/// release/acquire ordering; the producer never locks or allocates for this
/// handoff. Producer entry points must not overlap each other, consumer entry
/// points must not overlap each other, and there may be at most one thread in
/// each role. reset(), the set_* configuration methods, move, and destruction
/// require both roles to be stopped. When the pending ring is full, a new output
/// frame is dropped rather than overwriting a slot the consumer may be reading;
/// analysis state and total_frames still advance.
class StreamAnalyzer {
 public:
  /// @brief Constructs analyzer with configuration.
  /// @param config Stream configuration
  explicit StreamAnalyzer(const StreamConfig& config);

  ~StreamAnalyzer();

  // Non-copyable, movable
  StreamAnalyzer(const StreamAnalyzer&) = delete;
  StreamAnalyzer& operator=(const StreamAnalyzer&) = delete;
  StreamAnalyzer(StreamAnalyzer&&) noexcept;
  StreamAnalyzer& operator=(StreamAnalyzer&&) noexcept;

  /// @brief Processes audio chunk (internal offset tracking).
  /// @param samples Input samples
  /// @param n_samples Number of samples
  /// @details Internally tracks cumulative sample count for timestamp calculation.
  /// @throws SonareException ErrorCode::InvalidState if the stream was already
  ///         finalized; call reset() to start a new stream first.
  void process(const float* samples, size_t n_samples);

  /// @brief Processes audio chunk (external offset synchronization).
  /// @param samples Input samples
  /// @param n_samples Number of samples
  /// @param sample_offset Cumulative sample count at start of this chunk
  /// @details Use this overload when you need precise synchronization with
  ///          an external timeline (e.g., AudioContext). Consecutive calls must
  ///          use contiguous original-rate offsets: the next offset must equal
  ///          the previous offset plus its input sample count. A gap, seek, or
  ///          switch between offset-tracking overloads is rejected; call
  ///          reset() first to discard the buffered partial frame and start a
  ///          new timeline segment. Empty calls do not select a tracking mode.
  /// @throws SonareException ErrorCode::InvalidState if the stream was already
  ///         finalized; call reset() to start a new stream first.
  void process(const float* samples, size_t n_samples, size_t sample_offset);

  /// @brief Finalizes the current stream by analyzing the remaining partial frame.
  /// @details If the stream ends with fewer than @ref StreamConfig::n_fft
  ///          samples buffered, this zero-pads that tail and emits one final
  ///          frame. Calling finalize() more than once is idempotent, and a
  ///          call that throws leaves the stream un-finalized so a retry
  ///          resumes from the same point instead of reporting success without
  ///          the tail. Call reset() before reusing the analyzer for another
  ///          stream: feeding more audio to a finalized analyzer is rejected
  ///          with ErrorCode::InvalidState rather than silently dropping the
  ///          overlap context the finalized tail already consumed.
  ///
  /// @note At sample rates above @ref kMaxDirectSampleRate, finalize() first
  ///       drains the persistent resampler to the exact rounded output length,
  ///       so even clips shorter than its startup latency retain their tail.
  void finalize();

  /// @brief Returns number of frames available to read.
  size_t available_frames() const;

  /// @brief Reads processed frames from internal buffer.
  /// @param max_frames Maximum number of frames to read
  /// @return Vector of frames (up to max_frames, may be empty)
  /// @details Frames are consumed from internal buffer after reading.
  std::vector<StreamFrame> read_frames(size_t max_frames);

  /// @brief Reads processed frames into SOA buffer.
  /// @param max_frames Maximum number of frames to read
  /// @param buffer Output buffer (cleared and filled)
  /// @details More efficient for WASM/postMessage transfer.
  void read_frames_soa(size_t max_frames, FrameBuffer& buffer);

  /// @brief Reads processed frames into quantized 8-bit buffer.
  /// @param max_frames Maximum number of frames to read
  /// @param buffer Output buffer (cleared and filled)
  /// @param qconfig Quantization configuration
  /// @details Reduces bandwidth by 4x compared to Float32.
  void read_frames_quantized_u8(size_t max_frames, QuantizedFrameBufferU8& buffer,
                                const QuantizeConfig& qconfig = QuantizeConfig());

  /// @brief Reads processed frames into quantized 16-bit buffer.
  /// @param max_frames Maximum number of frames to read
  /// @param buffer Output buffer (cleared and filled)
  /// @param qconfig Quantization configuration
  /// @details Reduces bandwidth by 2x compared to Float32, higher precision than U8.
  void read_frames_quantized_i16(size_t max_frames, QuantizedFrameBufferI16& buffer,
                                 const QuantizeConfig& qconfig = QuantizeConfig());

  /// @brief Resets per-stream analysis state for a new stream.
  /// @details Clears cumulative samples, frame counts, overlap/output buffers,
  /// chroma history, bar/pattern tracking and the resampler. It deliberately
  /// RETAINS the config-affecting setters — normalization gain
  /// (@ref set_normalization_gain), expected duration (@ref set_expected_duration)
  /// and tuning reference / chroma filterbank (@ref set_tuning_ref_hz) — so a
  /// reused analyzer keeps its calibration across clips. Re-call those setters
  /// after reset() if the next stream needs different values. This is why it is
  /// NOT equivalent to constructing a fresh analyzer.
  /// @param base_sample_offset Starting sample offset (default 0)
  /// @note Both SPSC roles must be stopped while reset() runs.
  void reset(size_t base_sample_offset = 0);

  /// @brief Sets expected total duration of the audio.
  /// @param duration_seconds Total duration in seconds
  /// @details Used to calculate optimal pattern lock timing.
  ///          If not set, a default threshold is used.
  void set_expected_duration(float duration_seconds);

  /// @brief Sets normalization gain for loud audio.
  /// @param gain Gain factor to apply to input samples (e.g., 0.5 for -6dB),
  ///        within @ref kMinNormalizationGain .. @ref kMaxNormalizationGain
  ///        (0.01 .. 100, i.e. ±40 dB)
  /// @details Use this to normalize loud/compressed audio before analysis.
  ///          Typical usage on input in the conventional ±1 float domain:
  ///          compute peak or RMS from the buffer, then set
  ///          gain = target_level / measured_level.
  /// @throws SonareException ErrorCode::InvalidParameter for a non-finite,
  ///         non-positive or out-of-range gain. The request is refused rather
  ///         than clamped into range, and the previous gain is kept: a recipe
  ///         built on a measurement can easily land outside the range (an
  ///         integer-scaled buffer asks for about 3e-4), and with no getter a
  ///         substituted value would be undetectable. Convert such a buffer to
  ///         the ±1 domain before it reaches the analyzer.
  void set_normalization_gain(float gain);

  /// @brief Sets tuning reference frequency and recreates chroma filterbank.
  /// @param ref_hz Reference frequency for A4 (default 440 Hz)
  /// @details Use this when the audio has non-standard tuning.
  ///          For example, if the audio is 1 semitone sharp (A4 = 466.16 Hz),
  ///          pass ref_hz = 466.16f to correct the chroma analysis.
  ///          Should be called before processing audio (after reset if needed).
  void set_tuning_ref_hz(float ref_hz);

  /// @brief Returns current statistics and progressive estimate.
  AnalyzerStats stats() const;

  /// @brief Returns configuration.
  const StreamConfig& config() const { return config_; }

  /// @brief Returns total frames processed.
  int frame_count() const;

  /// @brief Returns current time position (seconds).
  float current_time() const;

  /// @brief Number of bar-vote slots: 12 pitch classes * every ChordQuality.
  /// @details Exposed so a translation-unit-level static_assert can pin this
  ///          value to the ChordQuality enum cardinality. Indexed as
  ///          @c root * kNumChordQualities + quality .
  static constexpr int kBarVoteSlots = 12 * kNumChordQualities;

  /// @brief Test-only accessor for the current onset accumulator size.
  /// @details Exposes the size of the bounded onset history so regression tests
  ///          can assert the sliding-window cap holds over long streams. Not
  ///          part of the public streaming API; intended for white-box tests.
  size_t onset_accumulator_size_for_test() const { return onset_accumulator_size_; }

  /// @brief Test-only accessor for the onset accumulator's frame cap.
  size_t onset_window_frames_for_test() const { return onset_window_frames_; }

  /// @brief Test-only accessor for the current full chroma history size.
  /// @details Exposes the size of the bounded retroactive-bar chroma history so
  ///          regression tests can assert the fixed-ring cap holds. Not part of
  ///          the public streaming API.
  size_t full_chroma_history_size_for_test() const { return full_chroma_history_size_; }

  /// @brief Test-only accessor for the full chroma history frame cap.
  static constexpr size_t full_chroma_history_cap_for_test() { return kMaxChromaHistoryFrames; }

  /// @brief Test-only seeding of the voted pattern and detected key.
  /// @details The known-pattern correction only reaches its correcting branch
  ///          for a voted pattern that is confusable-but-not-equal to a known
  ///          pattern, which synthetic audio reaches only by chance. Seeding the
  ///          input directly lets a test exercise that branch on purpose rather
  ///          than hoping a fixture wanders into it. Not part of the public
  ///          streaming API.
  void seed_voted_pattern_for_test(const std::vector<BarChord>& voted, int key);

  /// @brief Test-only seeding of the bar-chord history the pattern scoring pass
  ///        reads.
  void seed_bar_progression_for_test(const std::vector<BarChord>& bars);

  /// @brief Test-only seeding of the detected key.
  /// @details The key normally arrives from a periodic re-estimate, so a test
  ///          that wants to exercise the key-dependent branches of the
  ///          progression path (including the unknown-key sentinel) would
  ///          otherwise have to shape audio until the estimator happens to
  ///          agree. Not part of the public streaming API.
  void seed_key_for_test(int key, bool minor = false) {
    current_estimate_.key = key;
    current_estimate_.key_minor = minor;
  }

  /// @brief Test-only entry point running the pattern voting pass.
  void compute_voted_pattern_for_test(int pattern_length) { compute_voted_pattern(pattern_length); }

  /// @brief Test-only entry point running the known-pattern correction exactly
  ///        as the realtime path does, with nothing else in the call.
  void run_pattern_correction_for_test() { correct_voted_pattern_by_known_patterns(); }

  /// @brief Test-only entry point running the pattern scoring pass.
  void detect_progression_pattern_for_test() { detect_progression_pattern(); }

  /// @brief Test-only accessor for the in-progress progressive estimate.
  const ProgressiveEstimate& progressive_estimate_for_test() const { return current_estimate_; }

 private:
  StreamConfig config_;

  // Internal sample rate for analysis (downsample if input is higher)
  // Using 44100 Hz internally ensures consistent results regardless of input sample rate
  static constexpr int kInternalSampleRate = 44100;
  static constexpr int kMaxDirectSampleRate = 44100;  // Resample anything above 44100 Hz
  int internal_sample_rate_;                          // Actual rate used for analysis
  bool needs_resampling_ = false;
  bool needs_mel_analysis_ = false;
  float resample_ratio_ = 1.0f;
  std::vector<float> resample_buffer_;  // Buffer for resampled audio
  std::vector<float> sanitize_buffer_;  // Scratch for NaN/Inf-sanitized input
  // Stateful, phase-continuous resampler used only by the streaming path.
  // Unlike a per-chunk one-shot resample (which flushes its filter tail with
  // zeros every call and thus glitches/drifts at chunk boundaries), this keeps
  // filter state across process() calls so consecutive chunks join seamlessly.
  // Constructed lazily only when needs_resampling_ is true.
  std::unique_ptr<streaming_detail::StreamResampler> stream_resampler_;

  // Normalization
  float normalization_gain_ = 1.0f;  // Gain factor for loud audio

  // Cumulative state
  enum class OffsetTrackingMode { Unset, Internal, External };
  OffsetTrackingMode offset_tracking_mode_ = OffsetTrackingMode::Unset;
  size_t next_external_sample_offset_ = 0;
  size_t cumulative_samples_ = 0;
  double cumulative_samples_exact_ = 0.0;
  /// Stream time of this segment's first sample: 0 for the internally tracked
  /// overload, base_sample_offset / sample_rate once reset(base) or the
  /// external-offset overload anchors the timeline. Every published time field
  /// is expressed on that one timeline; anything derived from a frame *count*
  /// (retroactive bar starts, which come from the chroma history's eviction
  /// counter) has to add this back or it silently restarts at zero while
  /// StreamFrame::timestamp does not.
  float base_time_sec_ = 0.0f;
  int frame_count_ = 0;
  int emitted_frame_count_ = 0;  // For emit_every_n_frames
  bool finalized_ = false;

  // Overlap buffer (stores last n_fft - hop_length samples).
  // overlap_read_pos_ is the index of the current frame start within
  // overlap_buffer_; frames advance the read position by hop_length instead of
  // erasing per hop (O(N) memmove). The consumed prefix is compacted once per
  // process() chunk rather than once per frame.
  std::vector<float> overlap_buffer_;
  size_t overlap_read_pos_ = 0;

  // Fixed SPSC output ring. Every StreamFrame and its fixed-shape feature vectors
  // are prepared by the constructor so emit_frame never allocates, including
  // when a consumer drains the queue between every callback.
  std::vector<StreamFrame> output_buffer_;
  std::unique_ptr<StreamAnalyzerPublication> publication_;
  StreamFrame scratch_frame_;  // Reused for throttled (non-emitted) frames

  bool try_begin_output_write(size_t* write_index) noexcept;
  void publish_output_write() noexcept;
  void initialize_stats_publication();
  void publish_stats_snapshot() noexcept;
  void reset_publication() noexcept;

  // FFT processor (reusable)
  std::unique_ptr<FFT> fft_;

  // Cached window function
  std::vector<float> window_;

  // Pre-computed filterbanks
  std::vector<float> mel_filterbank_;     // [n_mels x n_bins]
  std::vector<float> chroma_filterbank_;  // [12 x n_bins]

  // Chord templates for chord detection
  std::vector<ChordTemplate> chord_templates_;

  // Frequency array for spectral features
  std::vector<float> frequencies_;

  // Previous frame state (for onset detection)
  std::vector<float> prev_mel_log_;
  bool has_prev_frame_ = false;

  // Working buffers (reused to avoid allocation)
  std::vector<float> frame_buffer_;            // [n_fft]
  std::vector<std::complex<float>> spectrum_;  // [n_bins]
  std::vector<float> magnitude_;               // [n_bins]
  std::vector<float> power_;                   // [n_bins]
  std::vector<float> mel_buffer_;              // [n_mels]
  std::vector<float> mel_log_;                 // [n_mels]
  std::vector<float> chroma_buffer_;           // [12] - L2 normalized

  // Progressive estimation accumulators.
  //
  // onset_accumulator_ holds the most-recent onset-strength frames consumed by
  // the progressive BPM autocorrelation. It is bounded to a sliding window of
  // onset_window_frames_ (≈ kOnsetWindowSeconds of audio): once it exceeds the
  // cap the oldest frames are dropped from the front so memory and the
  // per-BPM-update autocorrelation cost stay O(1) over an arbitrarily long
  // stream. The window is sized to comfortably exceed the maximum
  // autocorrelation lag (bpm_to_lag(kBpmMin) ≈ 86 frames at 44.1 kHz / hop 512),
  // so trimming never removes lags the BPM estimator needs — it only discards
  // ancient onsets that no longer reflect the current tempo. Declared as a
  // fixed ring so both growth and front-trimming are allocation-free.
  std::vector<float> onset_accumulator_;
  size_t onset_accumulator_start_ = 0;
  size_t onset_accumulator_size_ = 0;
  std::vector<float> onset_window_scratch_;
  std::vector<float> bpm_autocorr_scratch_;

  /// @brief Seconds of onset history retained by onset_accumulator_.
  /// @details Far larger than the BPM autocorrelation's maximum lag (~1 s at
  ///          kBpmMin = 60 BPM), so the sliding window keeps every lag the
  ///          tempo estimator inspects while still bounding memory/CPU.
  static constexpr float kOnsetWindowSeconds = 60.0f;

  /// @brief Cap on onset_accumulator_ size, in frames (computed at construction
  ///        from the internal sample rate and hop length).
  size_t onset_window_frames_ = 0;
  std::array<float, 12> chroma_sum_;
  int chroma_frame_count_ = 0;
  float last_key_update_time_ = 0.0f;
  float last_bpm_update_time_ = 0.0f;
  ProgressiveEstimate current_estimate_;
  size_t dropped_chord_progression_entries_ = 0;
  size_t dropped_bar_progression_entries_ = 0;

  // Scratch for the known-pattern correction, which runs on the audio thread.
  // These were per-call locals whose only protection was that the shipped
  // pattern table never drove them past their small-buffer capacity, so an edit
  // to the table alone could put a heap allocation in process(). Reserved from
  // the table's own bounds in prepare_progressive_estimate() and reused, so the
  // capacity cannot be exceeded by any table the loop can iterate.
  std::vector<std::pair<int, int>> pattern_corrections_;
  std::vector<std::pair<int, int>> best_pattern_correction_;
  std::string correction_pattern_name_;
  std::string detected_pattern_scratch_name_;
  /// Entries of current_estimate_.all_pattern_scores the last scoring pass
  /// wrote. The vector itself stays at full size so each entry's name buffer
  /// survives; this is what gets published, so a consumer still sees no scores
  /// until the first pass has run.
  size_t all_pattern_scores_count_ = 0;

  // Chord progression tracking
  int prev_chord_root_ = -1;
  int prev_chord_quality_ = -1;
  float chord_stable_time_ = 0.0f;         ///< Time chord has been stable
  float current_chord_start_time_ = 0.0f;  ///< Stream time the current chord began
  /// Running-max confidence of the chord currently being held (prev_chord_*).
  /// Accumulated each frame the chord persists so that, at a transition, the
  /// ChordChange records the *completed* chord's own confidence rather than the
  /// new chord's. Max (not mean) is used because the per-frame chord confidence
  /// is a correlation score that dips on the noisy boundary frames where the
  /// smoothing window straddles two chords; the peak best reflects how strongly
  /// the held chord was identified during its stable span.
  float prev_chord_confidence_ = 0.0f;
  static constexpr float kChordMinDuration = 0.3f;  ///< Min duration to register change
  static constexpr int kChordSmoothingFrames =
      12;  ///< Number of frames to smooth (~0.25s at default settings)
  static constexpr float kChordConfidenceThreshold = 0.5f;  ///< Min correlation for chord detection
  std::vector<std::array<float, 12>> chroma_history_;       ///< Prepared chord-smoothing ring
  size_t chroma_history_start_ = 0;
  size_t chroma_history_size_ = 0;
  std::array<float, kChordSmoothingFrames> median_chroma_scratch_ = {};

  // Per-frame smoothed-chord cache. The smoothed chroma + best-chord detection
  // over chroma_history_ is identical across process_single_frame(),
  // update_progressive_estimate(), and update_bar_chord_tracking(), which run
  // back-to-back for the same frame without mutating chroma_history_ in between.
  // process_single_frame() computes it once and stores the result here; the two
  // progressive consumers read it instead of recomputing the same median +
  // find_best_chord(). frame_chord_cache_valid_ is cleared at the start of every
  // frame, so a consumer that runs when the producing block was skipped falls
  // back to recomputing (identical to the un-cached path).
  bool frame_chord_cache_valid_ = false;
  int frame_chord_root_ = 0;
  int frame_chord_quality_ = 0;
  float frame_chord_corr_ = 0.0f;

  // Bar-synchronized chord tracking (requires stable BPM)
  static constexpr float kBpmConfidenceThreshold = 0.3f;  ///< Min BPM confidence for bar sync
  static constexpr int kBeatsPerBar = 4;                  ///< Beats per bar (4/4 time signature)
  bool bar_tracking_active_ = false;                      ///< True when BPM is stable enough
  float bar_duration_ = 0.0f;                             ///< Duration of one bar in seconds
  int current_bar_index_ = -1;                            ///< Current bar index (0-based)
  float bar_start_time_ = 0.0f;                           ///< Start time of current bar
  // Chord voting within bar (alternative to chroma averaging).
  // Sized for 12 pitch classes * every ChordQuality enumerator so qualities
  // beyond the basic triads (e.g. Dominant7, Sus4) are not silently dropped
  // from the bar progression. Bounds-check accordingly.
  std::array<int, kBarVoteSlots> bar_chord_votes_;
  int bar_vote_count_ = 0;  ///< Total votes in current bar

  // Pattern locking (once detected with high confidence, don't change)
  bool pattern_locked_ = false;     ///< True if pattern is locked
  float expected_duration_ = 0.0f;  ///< Expected total duration (0 = unknown)

  // Full chroma history for retroactive bar chord detection. Storage is
  // prepared to the cap and used as a ring, so the callback never grows a
  // container while retaining O(1) drop-oldest behavior.
  static constexpr size_t kMaxChromaHistoryFrames = 3000;  ///< ~35s at default settings
  std::vector<std::array<float, 12>> full_chroma_history_;
  size_t full_chroma_history_start_ = 0;
  size_t full_chroma_history_size_ = 0;
  // Absolute frame index of full_chroma_history_.front(). Starts at 0 and
  // advances by one every time the cap drops the oldest frame. Retroactive bar
  // detection multiplies (this + local index) by the per-frame duration to get
  // a correct absolute start_time, instead of wrongly assuming the surviving
  // history still begins at t=0.
  size_t full_chroma_history_offset_ = 0;

  // Internal methods
  void compute_retroactive_bar_chords();
  void compute_voted_pattern(int pattern_length = 4);
  void correct_voted_pattern_by_known_patterns();
  void detect_progression_pattern();
  void process_internal(const float* samples, size_t n_samples);
  void process_complete_frames();
  void emit_frame(const float* frame_start, size_t frame_sample_offset, bool force_emit);
  /// @brief Copies @p n_samples from @p src into @p dst, replacing any NaN/Inf
  ///        with 0. Returns dst.data(). Used to keep one bad input sample from
  ///        poisoning every downstream estimate (FFT, mel, chroma, onset).
  static const float* sanitize_into(const float* src, size_t n_samples, std::vector<float>& dst);
  void process_single_frame(const float* frame_start, size_t sample_offset, StreamFrame& frame);
  void compute_stft(const float* frame_start);
  void compute_mel();
  void compute_chroma();
  float compute_onset();
  void compute_spectral_features(StreamFrame& frame);
  void update_progressive_estimate(float current_time);
  void update_bar_chord_tracking(float current_time);
  void prepare_output_frame(StreamFrame& frame) const;
  void prepare_progressive_estimate();
  const StreamFrame& output_front() const;
  void pop_output_front();
  void append_onset(float value);
  void append_recent_chroma(const std::array<float, 12>& chroma);
  void append_full_chroma(const std::array<float, 12>& chroma);
  std::array<float, 12> median_recent_chroma();
  std::array<float, 12> median_full_chroma(size_t start, size_t count);
  void append_chord_progression(const ChordChange& change);
  void append_bar_progression(const BarChord& chord);
  void flush_pending_chord();
};

}  // namespace sonare
