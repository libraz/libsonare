#pragma once

/// @file stream_config.h
/// @brief Configuration for streaming audio analysis.

#include <cstddef>

#include "util/constants.h"
#include "util/exception.h"
#include "util/types.h"

namespace sonare {

/// @brief Output format for streaming data.
enum class OutputFormat {
  Float32 = 0,  ///< Full precision float
  Int16 = 1,    ///< 16-bit signed integer (for bandwidth reduction)
  Uint8 = 2,    ///< 8-bit unsigned integer (for visualization)
};

inline constexpr size_t kDefaultStreamMaxPendingFrames = 4096;
inline constexpr size_t kMaxStreamPendingFrames = 1u << 20;
inline constexpr size_t kDefaultStreamMaxProgressionEntries = 4096;
inline constexpr size_t kMaxStreamProgressionEntries = 1u << 20;

/// @brief Accepted A4 tuning reference range, one octave either side of concert
///        pitch.
/// @details StreamConfig validation and StreamAnalyzer::set_tuning_ref_hz share
///          these bounds so the parameter has one accepted range no matter which
///          entry point sets it: a value the live setter would refuse must not
///          be constructible at create time either, or the same user setting
///          would bin the chromagram differently depending on when it arrived.
inline constexpr float kMinTuningRefHz = 220.0f;
inline constexpr float kMaxTuningRefHz = 880.0f;

/// @brief Accepted analysis normalization gain, as a linear factor (±40 dB).
/// @details A value outside this range is rejected, not clamped, for the same
///          reason as the tuning reference above: there is no getter, so a
///          silently substituted gain leaves the analyzer running on input the
///          caller never asked for with nothing to detect it by. The range
///          assumes input in the conventional ±1 float domain — a buffer on a
///          different scale (integer-valued samples, say) belongs converted
///          before the analyzer, not corrected with an extreme gain.
inline constexpr float kMinNormalizationGain = 0.01f;
inline constexpr float kMaxNormalizationGain = 100.0f;

/// @brief Configuration for StreamAnalyzer.
struct StreamConfig {
  // Basic parameters
  /// @brief Sample rate in Hz.
  /// @details Defaults to 44100 Hz, *intentionally different* from the batch
  ///          MusicAnalyzer default of 22050 Hz (constants::kDefaultSampleRate,
  ///          chosen for librosa parity). Real-time audio reaches the analyzer
  ///          straight from the playback/capture graph (AudioWorklet, device
  ///          callbacks), which almost always runs at 44100/48000 Hz. Matching
  ///          the native graph rate avoids an extra resample on the hot path and
  ///          keeps timestamps aligned with the audio clock. The analyzer
  ///          resamples internally only when the input exceeds 44100 Hz.
  int sample_rate = 44100;
  int n_fft = 2048;                      ///< FFT size
  int hop_length = 512;                  ///< Hop length between frames
  WindowType window = WindowType::Hann;  ///< Window function type

  // Feature computation flags
  /// @brief Populate StreamFrame::magnitude (raw per-frame magnitude spectrum).
  /// @details Defaults to false: no SOA read path surfaces the per-frame
  /// magnitude, so enabling it only costs a per-frame allocation+copy on the
  /// realtime path with no readable result (centroid/flatness use a separate
  /// internal buffer). The flat C ABI rejects a non-zero value for the same
  /// reason, so leaving it off keeps the default config portable across surfaces.
  bool compute_magnitude = false;  ///< Compute magnitude spectrum (not surfaced; see above)
  bool compute_mel = true;         ///< Compute mel spectrogram
  bool compute_chroma = true;      ///< Compute chromagram
  /// @brief Compute onset strength (spectral flux of the log-mel spectrum).
  /// @details Onset strength — and the progressive BPM estimate built on top of
  ///          it — is derived from an internal mel path. compute_onset may be
  ///          enabled while compute_mel is false; in that case mel is computed
  ///          internally but is not included in output feature arrays.
  bool compute_onset = true;
  bool compute_spectral = true;  ///< Compute spectral features

  // Mel configuration
  int n_mels = 128;   ///< Number of mel bands
  float fmin = 0.0f;  ///< Minimum frequency for mel
  /// @brief Maximum frequency for mel (0 = sr/2).
  /// @details Above 44100 Hz the analyzer resamples to 44100 Hz internally, so
  ///          the mel filterbank cannot reach past 22050 Hz however this is set
  ///          — a higher request (including the 0 default, which asks for sr/2)
  ///          is capped at the internal Nyquist rather than refused, because
  ///          refusing it would reject the default config for every input above
  ///          44100 Hz. This is the one place the analyzer narrows a requested
  ///          value instead of rejecting it, and the ceiling is a property of
  ///          the analysis rate, not a policy choice.
  float fmax = 0.0f;

  // Tuning configuration
  float tuning_ref_hz = constants::kA4Hz;  ///< Reference frequency for A4

  // Output configuration
  /// @deprecated Generic reads have a fixed Float32 type. Must remain Float32;
  /// use the explicit U8/I16 read methods for quantized output.
  OutputFormat output_format = OutputFormat::Float32;
  int emit_every_n_frames = 1;   ///< Emit every N frames (for throttling)
  int magnitude_downsample = 1;  ///< Downsample factor for magnitude
  /// Maximum unread output frames. On overflow the newly produced frame is
  /// dropped, keeping a concurrent SPSC consumer's current slot immutable.
  size_t max_pending_frames = kDefaultStreamMaxPendingFrames;
  /// Maximum retained entries in each chord/bar progression. On overflow the
  /// oldest entry is dropped and the matching AnalyzerStats counter advances.
  size_t max_progression_entries = kDefaultStreamMaxProgressionEntries;

  // Progressive estimation configuration
  float key_update_interval_sec = 5.0f;   ///< Interval for key re-estimation
  float bpm_update_interval_sec = 10.0f;  ///< Interval for BPM re-estimation

  // Helper methods

  /// @brief Returns number of frequency bins.
  int n_bins() const { return n_fft / 2 + 1; }

  /// @brief Returns overlap size in samples.
  int overlap() const { return n_fft - hop_length; }

  /// @brief Returns frame duration in seconds.
  float frame_duration() const {
    return static_cast<float>(hop_length) / static_cast<float>(sample_rate);
  }

  /// @brief Returns maximum frequency for mel.
  float effective_fmax() const {
    return fmax > 0.0f ? fmax : static_cast<float>(sample_rate) / 2.0f;
  }
};

/// @brief Rejects a config that a caller limited to the SOA read paths could
///        ask for but could never read back.
///
/// @warning **StreamAnalyzer's constructor deliberately does NOT call this, and
///          must not be changed to.** The rule belongs to the SOA read paths a
///          caller holds, not to the config or the analyzer, and wiring it into
///          the constructor would silently kill @c read_frames()'s magnitude
///          output for every direct C++ host. Only
///          @c tests/streaming/stream_analyzer_core_test.cpp would catch that;
///          the other constructions never read the array back.
///
/// @details One shared answer for four surfaces that do not share a call path —
///          the C ABI validates before constructing, while Node and WASM
///          construct StreamAnalyzer directly. Node accepted
///          @ref StreamConfig::compute_magnitude where the others refused it, so
///          one options object was valid on one surface and an InvalidParameter
///          on the rest, buying a per-frame copy no read method there returns.
///          Only that field is in scope:
///          @ref StreamConfig::magnitude_downsample is unused with magnitude
///          off, so refusing it would reject a default config round trip.
/// @throws SonareException(InvalidParameter) when the config asks for a result
///         the caller's read paths could never return.
inline void validate_soa_stream_config(const StreamConfig& config) {
  if (config.compute_magnitude) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "computeMagnitude is not supported because magnitude frames are not "
                          "exposed by StreamAnalyzer read paths");
  }
}

}  // namespace sonare
