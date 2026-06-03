#include "streaming/stream_analyzer.h"

#include <algorithm>
#include <cmath>

#include "analysis/chord_templates.h"
#include "core/fft.h"
#include "core/window.h"
#include "filters/chroma.h"
#include "filters/mel.h"
#include "streaming/stream_analyzer_utils.h"
#include "util/constants.h"
#include "util/math_utils.h"

namespace sonare {

using namespace streaming_detail;

/// @brief Compile-time guard against drift between the bar-vote table size and
///        the ChordQuality enum cardinality.
/// @details The bar-vote table is indexed as
///          @c root * kNumChordQualities + quality , so every enumerator added
///          to ChordQuality must be accompanied by a bump of
///          kNumChordQualities. Without this assertion an enum expansion
///          would silently truncate qualities whose index exceeds the old
///          fixed array size and quietly drop those chords from the bar
///          progression — exactly the P0 bug this whole module is designed
///          to prevent. If this fires, raise kNumChordQualities in
///          stream_analyzer.h to match the enum in util/types.h.
static_assert(StreamAnalyzer::kBarVoteSlots == 12 * kNumChordQualities,
              "StreamAnalyzer::kBarVoteSlots must equal 12 * kNumChordQualities");
static_assert(static_cast<int>(ChordQuality::Sus2Add4) < kNumChordQualities,
              "kNumChordQualities must cover every ChordQuality enumerator; "
              "bump it in stream_analyzer.h when adding a new quality");

StreamAnalyzer::StreamAnalyzer(const StreamConfig& config) : config_(config) {
  /// Clamp loop/sizing parameters that the C-ABI validates but that direct
  /// C++/Node/WASM construction can pass through unchecked. A
  /// magnitude_downsample of 0 would integer-divide n_bins() by zero when
  /// sizing the per-frame magnitude vector; a non-positive hop_length would
  /// stall the frame loop (the read position never advances) so the analyzer
  /// would emit nothing forever; emit_every_n_frames <= 0 likewise breaks the
  /// emission throttle. Clamp each to its minimum sane value, mirroring the
  /// C-ABI guards, so these constructors are safe regardless of binding layer.
  config_.hop_length = std::max(config_.hop_length, 1);
  config_.emit_every_n_frames = std::max(config_.emit_every_n_frames, 1);
  config_.magnitude_downsample = std::max(config_.magnitude_downsample, 1);

  /// Onset strength (and therefore progressive BPM) is derived from the
  /// frame-to-frame difference of the log-mel spectrum (see compute_onset()).
  /// If a caller requests onset/BPM but disables the mel path, onset would be
  /// identically 0 forever and BPM would never leave its initial 0 / confidence
  /// 0 state — a silent failure. Enforce the dependency by auto-enabling mel
  /// whenever onset is requested. This coercion is observable through config()
  /// so the caller can see that mel was turned on for them.
  if (config_.compute_onset && !config_.compute_mel) {
    config_.compute_mel = true;
  }

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

  /// Initialize FFT
  fft_ = std::make_unique<FFT>(config_.n_fft);

  /// Cache window function
  window_ = get_window_cached(config_.window, config_.n_fft);

  /// Pre-compute mel filterbank (use internal sample rate)
  if (config_.compute_mel) {
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

  if (config_.compute_mel) {
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

  /// Reserve overlap buffer capacity
  overlap_buffer_.reserve(config_.overlap());
}

StreamAnalyzer::~StreamAnalyzer() = default;

StreamAnalyzer::StreamAnalyzer(StreamAnalyzer&&) noexcept = default;
StreamAnalyzer& StreamAnalyzer::operator=(StreamAnalyzer&&) noexcept = default;

void StreamAnalyzer::process(const float* samples, size_t n_samples) {
  process_internal(samples, n_samples);
}

}  // namespace sonare
