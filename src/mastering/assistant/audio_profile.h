#pragma once

/// @file audio_profile.h
/// @brief Mastering assistant audio profiling.

#include <cstddef>
#include <string>
#include <vector>

#include "core/audio.h"

namespace sonare {
class Spectrogram;
}  // namespace sonare

namespace sonare::mastering::assistant {

struct LoudnessProfile {
  float integrated_lufs = 0.0f;
  float lra_lu = 0.0f;
  float true_peak_db = 0.0f;
  float crest_factor_db = 0.0f;
};

struct SpectralProfile {
  float sub_rms_db = 0.0f;       ///< 20-60 Hz
  float low_rms_db = 0.0f;       ///< 60-250 Hz
  float low_mid_rms_db = 0.0f;   ///< 250-500 Hz
  float mid_rms_db = 0.0f;       ///< 500-2000 Hz
  float high_mid_rms_db = 0.0f;  ///< 2-6 kHz
  float high_rms_db = 0.0f;      ///< 6-12 kHz
  float air_rms_db = 0.0f;       ///< 12 kHz-Nyquist
  float centroid_hz = 0.0f;
  float flatness = 0.0f;
  float rolloff_hz = 0.0f;
};

struct DynamicsProfile {
  float short_term_lufs_std = 0.0f;
  float attack_density = 0.0f;  ///< Onset peaks per second.
  float sustain_ratio = 0.0f;   ///< 0 = transient-heavy, 1 = sustained.
};

struct GenreCandidate {
  std::string name;
  float score = 0.0f;
};

/// @brief What the six repair detectors measured in the profiled signal.
/// @details Each field is one detector's own scalar carried unchanged; the
///          repair headers define what each measures. Filled only when
///          @ref AudioProfileConfig::detect_defects asks for it, so @ref measured
///          is what separates a clean recording from one nothing looked at --
///          the two are the same field values otherwise.
///
///          Every detector runs with its own default config, which confines the
///          hum search to a couple of Hz around 50 Hz. A 60 Hz mains series is
///          reported at that search boundary rather than at 60, with a
///          prominence barely above what the search picks out of clean
///          programme material: neither found nor reported as absent.
struct DefectProfile {
  /// True once all six detectors have run over this signal. False says none of
  /// them did: the caller did not ask, or the input was shorter than the noise
  /// detector's STFT.
  bool measured = false;

  // repair/declick.h
  std::size_t click_count = 0;
  /// Outlier runs the criteria excluded. A large value says the detector could
  /// not decide on this material, not that the material is clean.
  std::size_t click_rejected = 0;
  std::size_t click_longest_run_samples = 0;
  float click_per_second = 0.0f;

  // repair/decrackle.h
  std::size_t crackle_sample_count = 0;
  float crackle_sample_fraction = 0.0f;
  float crackle_per_second = 0.0f;

  // repair/declip.h
  std::size_t clip_sample_count = 0;
  std::size_t clip_run_count = 0;
  std::size_t clip_longest_run_samples = 0;
  float clip_sample_fraction = 0.0f;

  // repair/denoise_classical.h
  float noise_floor_dbfs = 0.0f;
  /// Loudest of the detector's 32 bands, and which band it is. The profile
  /// carries the peak rather than the array: a caller decides from the level and
  /// from whether the floor sits low or high, and reads the shape out of
  /// detect_noise_floor() when it needs the rest.
  float noise_band_peak_dbfs = 0.0f;
  int noise_band_peak_index = -1;  ///< -1 when no band was measured.

  // repair/dehum.h
  float hum_fundamental_hz = 0.0f;
  /// How far the winning candidate stood above the rest of the search. 1.0 means
  /// the search found no peak at all -- the detector could not decide, which is
  /// not the same answer as no hum.
  float hum_fundamental_prominence = 1.0f;
  int hum_harmonics = 0;
  float hum_fundamental_dbfs = 0.0f;    ///< Input level at f0.
  float hum_peak_harmonic_dbfs = 0.0f;  ///< Loudest k*f0, carried instead of the
                                        ///  detector's per-harmonic array.

  // repair/dereverb_classical.h
  /// Decay across the dereverb module's own late lag, in dB. NOT an RT60: less
  /// negative means the material sustains across the lag, so a reverberant input
  /// reads *higher* here than the same material dry. The detector's
  /// late_predictability is not carried because the WPE stage it comes from is
  /// off in the default config, which would make it zero in every profile.
  float late_decay_ratio_db = 0.0f;
};

struct AudioProfile {
  float duration_sec = 0.0f;
  float bpm = 0.0f;
  float bpm_confidence = 0.0f;
  LoudnessProfile loudness{};
  SpectralProfile spectral{};
  DynamicsProfile dynamics{};
  DefectProfile defects{};
  std::vector<GenreCandidate> genre_candidates;
};

struct AudioProfileConfig {
  int n_fft = 2048;
  int hop_length = 512;
  int true_peak_oversample = 4;
  /// Measure @ref AudioProfile::defects. Off by default: the six detectors are
  /// six analysis passes, two of them a further STFT.
  bool detect_defects = false;
};

AudioProfile analyze_audio_profile(const float* samples, std::size_t length, int sample_rate,
                                   const AudioProfileConfig& config = {});
AudioProfile analyze_audio_profile(const Audio& audio, const AudioProfileConfig& config = {});

/// @brief Profiling overloads that hand back the analysis STFT.
/// @details The spectral and dynamics blocks are measured from one STFT of the
///          profiled signal. A caller that needs the same spectrogram reads it
///          here rather than computing a second one: nothing in the tree caches a
///          spectrogram, so the duplicate costs a whole STFT. Check the geometry
///          with @ref sonare::validate_reused_geometry before reading it — the
///          framing is this profiler's, not the caller's.
/// @param spec_out Receives the STFT; left empty when the input is degenerate and
///          no profile is measured. May be null.
/// @{
AudioProfile analyze_audio_profile(const float* samples, std::size_t length, int sample_rate,
                                   const AudioProfileConfig& config, Spectrogram* spec_out);
AudioProfile analyze_audio_profile(const Audio& audio, const AudioProfileConfig& config,
                                   Spectrogram* spec_out);
/// @}

/// @brief Multi-channel counterpart preserving BS.1770 channel summing.
/// @details Only the `loudness` block is measured from the channels: integrated
///          LUFS and LRA come from the channel-summed program and the true peak
///          is the largest across the channels, so decorrelated stereo is not
///          read roughly 6 dB low the way a `0.5 * (L + R)` downmix reads it.
///          The spectral, dynamics, tempo and defect fields describe spectral
///          shape, timing and damage rather than absolute level and are measured
///          on the downmix, which keeps them comparable with the mono entry point;
///          `dynamics.shortTermLufsStd` is a spread rather than a level, so the
///          downmix's near-constant loudness offset cancels out of it.
/// @param samples Pointer to `frames * channels` interleaved samples.
/// @param frames Number of sample frames.
/// @param channels Channel count; must be positive.
/// @param sample_rate Sample rate in Hz; must be positive.
AudioProfile analyze_audio_profile_interleaved(const float* samples, std::size_t frames,
                                               int channels, int sample_rate,
                                               const AudioProfileConfig& config = {});

/// @brief Multi-channel profiling that hands back the analysis STFT.
/// @details The STFT is the one measured over the downmix, which is the signal
///          the spectral block describes; the loudness block is measured from the
///          channels and has no spectrogram.
/// @param spec_out Receives the downmix STFT. May be null.
AudioProfile analyze_audio_profile_interleaved(const float* samples, std::size_t frames,
                                               int channels, int sample_rate,
                                               const AudioProfileConfig& config,
                                               Spectrogram* spec_out);
std::string audio_profile_to_json(const AudioProfile& profile);

}  // namespace sonare::mastering::assistant
