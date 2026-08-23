#include "streaming/stream_analyzer.h"

#include <algorithm>
#include <cmath>

#include "analysis/chord_templates.h"
#include "core/fft.h"
#include "core/window.h"
#include "filters/chroma.h"
#include "filters/mel.h"
#include "streaming/stream_analyzer_publication.h"
#include "streaming/stream_analyzer_utils.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

using namespace streaming_detail;

/// @brief Compile-time guard against drift between the bar-vote table size and
///        the ChordQuality enum cardinality.
/// @details The bar-vote table is indexed as
///          @c root * kNumChordQualities + quality , so every enumerator added
///          to ChordQuality must be accompanied by a bump of
///          kChordQualityCount. Without this assertion an enum expansion
///          would silently truncate qualities whose index exceeds the old
///          fixed array size and quietly drop those chords from the bar
///          progression — exactly the P0 bug this whole module is designed
///          to prevent. If this fires, raise kChordQualityCount in
///          util/types.h to match the enum beside it, and kBarVoteSlots here.
static_assert(StreamAnalyzer::kBarVoteSlots == 12 * kNumChordQualities,
              "StreamAnalyzer::kBarVoteSlots must equal 12 * kNumChordQualities");
static_assert(static_cast<int>(ChordQuality::Dominant7s9) < kNumChordQualities,
              "kChordQualityCount must cover every ChordQuality enumerator; "
              "bump it in util/types.h when adding a new quality");

StreamAnalyzer::StreamAnalyzer(const StreamConfig& config) : config_(config) {
  /// Reject malformed geometry that would silently yield *wrong* results. Every
  /// StreamConfig field is refused rather than substituted here, and so are the
  /// sizing params below — nothing in this constructor clamps a request into
  /// range. The flat C ABI rejects the same set before construction; mirror the
  /// same relationship and positive-value contract here so direct
  /// C++/Node/WASM construction fails identically instead of producing garbage
  /// spectra. Note compute_magnitude
  /// is a real core feature (although SOA bindings do not surface it) and window
  /// is an enum, so those two are intentionally not re-checked here. The legacy
  /// output_format selector is checked separately below.
  const auto finite_positive = [](float v) { return std::isfinite(v) && v > 0.0f; };
  const auto finite_non_negative = [](float v) { return std::isfinite(v) && v >= 0.0f; };
  if (config_.sample_rate <= 0)
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: sample_rate must be positive");
  if (config_.n_fft <= 0)
    throw SonareException(ErrorCode::InvalidParameter, "StreamConfig: n_fft must be positive");
  if (config_.n_mels <= 0)
    throw SonareException(ErrorCode::InvalidParameter, "StreamConfig: n_mels must be positive");
  if (config_.hop_length > config_.n_fft)
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: hop_length must not exceed n_fft");
  if (!finite_non_negative(config_.fmin) || !finite_non_negative(config_.fmax))
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: fmin/fmax must be finite and non-negative");
  if (config_.fmax > 0.0f && config_.fmax <= config_.fmin)
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: fmax must be greater than fmin");
  if (!finite_positive(config_.tuning_ref_hz) || config_.tuning_ref_hz < kMinTuningRefHz ||
      config_.tuning_ref_hz > kMaxTuningRefHz)
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: tuning_ref_hz must be finite and within 220..880 Hz");
  if (!finite_positive(config_.key_update_interval_sec) ||
      !finite_positive(config_.bpm_update_interval_sec))
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: key/bpm_update_interval_sec must be finite and positive");

  /// Reject loop/sizing parameters that the C-ABI also rejects, so direct
  /// C++/Node/WASM construction is consistent with the C-ABI oracle instead of
  /// silently clamping (which would morph the request into a different, denser
  /// analyzer). A magnitude_downsample of 0 would integer-divide n_bins() by
  /// zero when sizing the per-frame magnitude vector; a non-positive hop_length
  /// would stall the frame loop (the read position never advances) so the
  /// analyzer would emit nothing forever; emit_every_n_frames <= 0 likewise
  /// breaks the emission throttle. The C-ABI validates before constructing, so
  /// it never reaches here with these values; this guard catches the binding
  /// layers that construct directly.
  if (config_.hop_length <= 0)
    throw SonareException(ErrorCode::InvalidParameter, "StreamConfig: hop_length must be positive");
  if (config_.emit_every_n_frames <= 0)
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: emit_every_n_frames must be positive");
  if (config_.magnitude_downsample <= 0)
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: magnitude_downsample must be positive");
  if (config_.max_pending_frames == 0 || config_.max_pending_frames > kMaxStreamPendingFrames) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: max_pending_frames is outside supported bounds");
  }
  if (config_.max_progression_entries == 0 ||
      config_.max_progression_entries > kMaxStreamProgressionEntries) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "StreamConfig: max_progression_entries is outside supported bounds");
  }

  if (config_.output_format != OutputFormat::Float32) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "StreamConfig: output_format is deprecated; use an explicit quantized read method");
  }

  // Onset is derived from log-mel flux. Keep that dependency internal so
  // compute_onset=true/compute_mel=false produces onset output without falsely
  // advertising or populating the disabled mel output array.
  needs_mel_analysis_ = config_.compute_mel || config_.compute_onset;

  /// Determine if resampling is needed for high sample rates
  if (config_.sample_rate > kMaxDirectSampleRate) {
    needs_resampling_ = true;
    internal_sample_rate_ = kInternalSampleRate;
    resample_ratio_ = static_cast<float>(kInternalSampleRate) / config_.sample_rate;
    /// Use a single persistent, phase-continuous resampler for the whole
    /// stream. The previous per-chunk one-shot resample() rebuilt the filter
    /// and flushed its tail with zeros on every process() call, which injected
    /// a discontinuity (click) at every chunk boundary and let rounding drift
    /// accumulate. A stateful resampler carries filter history across chunks so
    /// boundaries are seamless. See streaming/stream_resampler.h.
    stream_resampler_ = std::make_unique<streaming_detail::StreamResampler>(config_.sample_rate,
                                                                            internal_sample_rate_);
  } else {
    needs_resampling_ = false;
    internal_sample_rate_ = config_.sample_rate;
    resample_ratio_ = 1.0f;
  }

  int n_bins = config_.n_bins();

  /// Size the bounded onset history window. Frames-per-second is
  /// internal_sample_rate_ / hop_length; multiply by kOnsetWindowSeconds to get
  /// the retained frame count. Floor it at a few times the maximum
  /// autocorrelation lag (bpm_to_lag(kBpmMin)) so the BPM estimator always has
  /// enough lags even for unusually large hop lengths.
  {
    const int hop = std::max(config_.hop_length, 1);
    const size_t window_from_seconds = static_cast<size_t>(
        kOnsetWindowSeconds * static_cast<float>(internal_sample_rate_) / static_cast<float>(hop));
    const int max_lag = streaming_detail::bpm_to_lag(streaming_detail::kBpmMin,
                                                     internal_sample_rate_, config_.hop_length);
    const size_t min_window = static_cast<size_t>(std::max(max_lag, 1)) * 4;
    onset_window_frames_ = std::max(window_from_seconds, min_window);
  }

  // Prepare every bounded history and BPM scratch buffer before process().
  // resize() is intentional for ring storage; logical sizes are tracked by
  // separate counters and begin at zero.
  onset_accumulator_.resize(onset_window_frames_);
  onset_window_scratch_.reserve(onset_window_frames_);
  const int prepared_max_lag =
      std::max(1, streaming_detail::bpm_to_lag(streaming_detail::kBpmMin, internal_sample_rate_,
                                               config_.hop_length));
  bpm_autocorr_scratch_.reserve(static_cast<size_t>(prepared_max_lag));
  chroma_history_.resize(static_cast<size_t>(kChordSmoothingFrames));
  full_chroma_history_.resize(kMaxChromaHistoryFrames);

  /// Initialize FFT
  fft_ = std::make_unique<FFT>(config_.n_fft);

  /// Cache window function
  window_ = *get_window_cached(config_.window, config_.n_fft);

  /// Pre-compute mel filterbank (use internal sample rate)
  if (needs_mel_analysis_) {
    MelFilterConfig mel_config;
    mel_config.n_mels = config_.n_mels;
    mel_config.fmin = config_.fmin;
    mel_config.fmax = needs_resampling_ ? std::min(config_.effective_fmax(),
                                                   static_cast<float>(internal_sample_rate_) * 0.5f)
                                        : config_.effective_fmax();
    mel_filterbank_ = create_mel_filterbank(internal_sample_rate_, config_.n_fft, mel_config);
  }

  /// Pre-compute chroma filterbank (use internal sample rate)
  if (config_.compute_chroma) {
    ChromaFilterConfig chroma_config;
    chroma_config.n_chroma = 12;
    /// Convert tuning_ref_hz to semitone offset: tuning = 12 * log2(ref/440)
    /// Positive tuning means audio is sharp, so we subtract to correct
    chroma_config.tuning =
        constants::kSemitonesPerOctave * std::log2(config_.tuning_ref_hz / constants::kA4Hz);
    /// Use C2 (~65 Hz) as minimum frequency to skip very low bass.
    /// This helps avoid interference from sub-bass and low-frequency noise.
    chroma_config.fmin = streaming_detail::kStreamingChromaFminHz;
    chroma_filterbank_ =
        create_chroma_filterbank(internal_sample_rate_, config_.n_fft, chroma_config);
  }

  /// Pre-compute frequencies for spectral features (use internal sample rate)
  if (config_.compute_spectral) {
    frequencies_ = compute_bin_frequencies(n_bins, internal_sample_rate_, config_.n_fft);
  }

  /// Allocate working buffers
  frame_buffer_.resize(config_.n_fft);
  spectrum_.resize(n_bins);
  magnitude_.resize(n_bins);
  power_.resize(n_bins);

  if (needs_mel_analysis_) {
    mel_buffer_.resize(config_.n_mels);
    mel_log_.resize(config_.n_mels);
    prev_mel_log_.resize(config_.n_mels, 0.0f);
  }

  if (config_.compute_chroma) {
    chroma_buffer_.resize(12);
    chroma_sum_.fill(0.0f);
    bar_chord_votes_.fill(0);
    /// Initialize chord templates for chord detection
    chord_templates_ = generate_triad_templates();
  }

  // Prepare the common callback block range (AudioWorklet/device callbacks are
  // normally 128-2048 samples). Larger one-shot chunks remain supported and
  // may grow these control buffers, but ordinary realtime callbacks do not.
  constexpr size_t kPreparedInputBlockSamples = 16384;
  const size_t prepared_input =
      std::max(kPreparedInputBlockSamples, static_cast<size_t>(config_.n_fft));
  sanitize_buffer_.reserve(prepared_input);
  resample_buffer_.reserve(prepared_input);
  overlap_buffer_.reserve(prepared_input + static_cast<size_t>(config_.n_fft));

  output_buffer_.resize(config_.max_pending_frames);
  for (auto& frame : output_buffer_) {
    prepare_output_frame(frame);
  }
  prepare_output_frame(scratch_frame_);
  prepare_progressive_estimate();
  publication_ = std::make_unique<StreamAnalyzerPublication>();
  initialize_stats_publication();
  publish_stats_snapshot();
}

StreamAnalyzer::~StreamAnalyzer() = default;

StreamAnalyzer::StreamAnalyzer(StreamAnalyzer&&) noexcept = default;
StreamAnalyzer& StreamAnalyzer::operator=(StreamAnalyzer&&) noexcept = default;

void StreamAnalyzer::process(const float* samples, size_t n_samples) {
  if (samples != nullptr && n_samples > 0) {
    /// Same class of misuse as a non-contiguous external offset, and rejected
    /// the same way: finalize() has already drained the overlap buffer, so
    /// resuming would analyze the next chunk without the preceding n_fft-1
    /// samples of context.
    if (finalized_) {
      throw SonareException(
          ErrorCode::InvalidState,
          "StreamAnalyzer was finalized; call reset() before processing more audio");
    }
    if (offset_tracking_mode_ == OffsetTrackingMode::External) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "cannot switch StreamAnalyzer offset mode without reset()");
    }
    offset_tracking_mode_ = OffsetTrackingMode::Internal;
  }
  process_internal(samples, n_samples);
  publish_stats_snapshot();
}

}  // namespace sonare
