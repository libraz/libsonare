#pragma once

/// @file rhythm.h
/// @brief Rhythm-domain features: tempogram and Fourier tempogram.
/// @details tempogram() and fourier_tempogram() mirror librosa.feature.tempogram
///          and librosa.feature.fourier_tempogram. tempogram_ratio() and
///          cyclic_tempogram() are NOT drop-in librosa equivalents: see their
///          declarations below for what they actually compute and how they
///          diverge from librosa.

#include <vector>

#include "core/audio.h"
#include "core/window.h"
#include "util/parameter_checks.h"

namespace sonare {

enum class TempogramMode {
  kAutocorrelation,
  kCosine,
};

/// @brief Largest accepted @ref TempogramConfig::win_length, counted in onset frames.
/// @details A backstop rather than a domain bound: 32 times the 384-frame default, which
/// at the default geometry is a lag near 285 seconds, so it sits above every consumer's
/// useful maximum instead of binding one. The autocorrelation is quadratic in the window
/// and linear in the frame count -- 16384 frames already costs 5 s over one second of
/// audio -- so an unbounded window does not finish rather than failing.
inline constexpr int kMaxTempogramWinLength = 384 * 32;

/// @brief The tempogram windows in onset frames: [2, @ref kMaxTempogramWinLength].
inline constexpr util::check::NumericDomain kTempogramWinLengthDomain =
    util::check::integer_domain(2, kMaxTempogramWinLength);

/// @brief The positive counts of the rhythm functions (hop_length, sr, n_bins, win_length of the
///        tempogram ratio).
inline constexpr util::check::NumericDomain kRhythmCountDomain =
    util::check::integer_domain_of<int>(1);

/// @brief Configuration for tempogram / fourier_tempogram.
struct TempogramConfig {
  int hop_length = 512;  ///< Hop length used for the onset envelope
  /// Window length in onset-envelope frames; in [2, @ref kMaxTempogramWinLength]
  int win_length = 384;
  WindowType window = WindowType::Hann;  ///< Analysis window
  bool center = true;                    ///< Center-pad the onset envelope
  bool norm = true;                      ///< Max-abs (L-inf) normalize each column (librosa)
  TempogramMode mode = TempogramMode::kAutocorrelation;  ///< Tempogram similarity mode
};

/// @brief Onset autocorrelation tempogram.
/// @details Local autocorrelation of the onset envelope. Mirrors
///          librosa.feature.tempogram by default. Set mode=kCosine to compute
///          window-local cosine similarity between each lagged onset slice.
/// @param onset_envelope Pre-computed onset strength envelope (frames per
///        hop). Length must be > 0.
/// @param sr Sample rate (required to scale tempo axis when consumed by
///        tempogram_ratio; tempogram itself does not depend on sr).
/// @param config Configuration
/// @return Tempogram matrix [win_length x n_frames], row-major.
///         Row i, frame j is the autocorrelation value at lag i for the window
///         centered on frame j of the onset envelope.
///         With center=false frame j covers env[j : j + win_length] and
///         n_frames = len - win_length + 1, as librosa frames it; an envelope
///         shorter than win_length throws InvalidParameter. The same length rule holds
///         for fourier_tempogram and cyclic_tempogram.
/// @throws SonareException(InvalidParameter) when a caller-supplied envelope contains a
///         non-finite value, named by index.
std::vector<float> tempogram(const std::vector<float>& onset_envelope, int sr,
                             const TempogramConfig& config = TempogramConfig());

/// @brief Compute the tempogram directly from audio (via onset_strength).
std::vector<float> tempogram(const Audio& audio, const TempogramConfig& config = TempogramConfig());

/// @brief Fourier (FFT-based) tempogram: STFT of the onset envelope.
/// @return Magnitude matrix [n_bins x n_frames] where n_bins = win_length/2 + 1.
/// @throws SonareException(InvalidParameter) when @p onset_envelope holds a non-finite value.
std::vector<float> fourier_tempogram(const std::vector<float>& onset_envelope, int sr,
                                     const TempogramConfig& config = TempogramConfig());

namespace detail {
/// Processes an onset envelope without scanning it; for library-computed envelopes.
std::vector<float> tempogram_of_envelope(const std::vector<float>& onset_envelope,
                                         const TempogramConfig& config);
/// No finiteness refusal: a library-computed envelope may be non-finite on finite input.
std::vector<float> fourier_tempogram_of_envelope(const std::vector<float>& onset_envelope,
                                                 const TempogramConfig& config);
}  // namespace detail

std::vector<float> fourier_tempogram(const Audio& audio,
                                     const TempogramConfig& config = TempogramConfig());

/// @brief Default lower tempo of the octave @ref cyclic_tempogram folds into, in BPM.
inline constexpr float kCyclicTempogramBpmMin = 60.0f;
/// @brief Default number of cyclic tempo classes @ref cyclic_tempogram returns.
inline constexpr int kCyclicTempogramBins = 60;

/// @brief Cyclic tempogram by octave-folding a Fourier tempogram.
/// @details Tempo bins are folded into one octave [bpm_min, 2*bpm_min), so
///          octave-equivalent tempi such as 60, 120, and 240 BPM contribute
///          to the same cyclic tempo class.
/// @warning APPROXIMATE / custom — this is NOT numerically compatible with
///          librosa's cyclic tempogram. It performs a direct phase-fold of each
///          Fourier-tempogram bin: the bin's BPM is mapped to a phase
///          `frac(log2(bpm / bpm_min))`, rounded to the nearest of @p n_bins
///          cyclic classes, and the magnitudes are summed into that class with
///          no interpolation and no per-class normalization. librosa instead
///          resamples onto a log-tempo axis and averages octave-equivalent
///          bins, so absolute values and bin alignment will differ. Use it as a
///          tempo-class energy summary, not as a librosa replacement.
/// @param onset_envelope Pre-computed onset strength envelope
/// @param sr Sample rate
/// @param bpm_min Lower tempo of the cyclic octave
/// @param n_bins Number of cyclic tempo classes
/// @return Matrix [n_bins x n_frames], row-major
std::vector<float> cyclic_tempogram(const std::vector<float>& onset_envelope, int sr,
                                    const TempogramConfig& config = TempogramConfig(),
                                    float bpm_min = kCyclicTempogramBpmMin,
                                    int n_bins = kCyclicTempogramBins);

std::vector<float> cyclic_tempogram(const Audio& audio,
                                    const TempogramConfig& config = TempogramConfig(),
                                    float bpm_min = kCyclicTempogramBpmMin,
                                    int n_bins = kCyclicTempogramBins);

/// @brief Heuristic harmonic-ratio summary of an autocorrelation tempogram.
/// @warning NOT a drop-in for librosa.feature.tempogram_ratio, which builds a BPM
///          axis and resamples onto tempo-ratio bins. This picks the strongest
///          frame-averaged autocorrelation lag (excluding 0) as the reference and
///          samples each ratio f at round(reference_lag / f), giving a coarse
///          lag-domain summary rather than a tempo-axis feature. @p sr is ignored
///          and kept only for API stability.
/// @param tempogram_data Tempogram matrix as returned by tempogram()
/// @param win_length Tempogram win_length used to compute the lag axis
/// @param sr Sample rate (currently unused, see warning above)
/// @param hop_length Hop length used for the onset envelope
/// @param factors Lag ratios to evaluate (default {0.5, 1, 2, 3, 4}); each must
///        be finite and > 0
/// @return Vector of length factors.size() with the frame-averaged
///         autocorrelation sampled at each ratio's integer lag.
/// @throw sonare::SonareException on non-positive @p win_length / @p hop_length,
///        an empty @p tempogram_data or @p factors, a @p factors entry that is
///        not finite and > 0, or a @p tempogram_data shorter than
///        @p win_length (fewer than one frame).
/// @note These are rejections, not zero-filled results: a zero at a ratio means
///       "no autocorrelation energy at that lag", so an unanswerable call must
///       not produce one. A NaN factor would otherwise reach
///       round(reference_lag / factor) as undefined behaviour, and an infinite
///       one resolves to lag 0 — the DC peak the reference search excludes.
std::vector<float> tempogram_ratio(const std::vector<float>& tempogram_data, int win_length, int sr,
                                   int hop_length,
                                   const std::vector<float>& factors = {0.5f, 1.0f, 2.0f, 3.0f,
                                                                        4.0f});

/// @brief Configuration for Predominant Local Pulse (PLP).
struct PlpConfig {
  int sr = 22050;
  int hop_length = 512;
  /// Inclusive tempo band in BPM; 60-200 like every public tempo entry.
  float tempo_min = 60.0f;
  float tempo_max = 200.0f;
  /// Tempogram window in onset frames; in [2, @ref kMaxTempogramWinLength], the
  /// same range the field takes on TempogramConfig.
  int win_length = 384;
};

/// @brief Predominant Local Pulse estimation (librosa.beat.plp).
/// @details Builds a Fourier tempogram, picks the most-likely tempo per frame
/// inside [tempo_min, tempo_max], then performs an inverse STFT of the masked
/// tempogram to produce a pulse curve aligned with the onset envelope.
/// @param onset_envelope Onset strength envelope frames.
/// @param config Configuration parameters.
/// @return Pulse curve of length onset_envelope.size().
/// @throws SonareException(InvalidParameter) when a caller-supplied envelope contains a
///         non-finite value, named by index.
std::vector<float> plp(const std::vector<float>& onset_envelope,
                       const PlpConfig& config = PlpConfig());

namespace detail {
/// Processes an onset envelope without scanning it; for library-computed envelopes.
std::vector<float> plp_of_envelope(const std::vector<float>& onset_envelope,
                                   const PlpConfig& config);
}  // namespace detail

/// @brief Convenience overload computing PLP directly from audio.
std::vector<float> plp(const Audio& audio, const PlpConfig& config = PlpConfig());

}  // namespace sonare
