#pragma once

/// @file equalizer_spectrum.h
/// @brief Windowed FFT analyser behind the equalizer magnitude profile.

#include <array>
#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

#include "core/fft.h"
#include "mastering/eq/spectrum_engine.h"

namespace sonare::mastering::eq {

/// @brief Log-binned magnitude analyser feeding EqualizerSpectrumSnapshot::profile_db.
/// @details Each processed block is downmixed to mono and appended to a fixed
/// analysis window. Once a hop has elapsed the window is Hann-windowed,
/// transformed and its bin powers are summed into the @c kSpectrumProfileBands
/// logarithmically spaced bands spanning the audible range, so the published
/// values are frequency-domain magnitudes rather than a decimation of the input
/// waveform. Levels are amplitude-referenced: a full-scale sine centred in a
/// band reads roughly 0 dB there.
///
/// Realtime contract: the FFT plan, the analysis window, the scratch buffers and
/// the band-to-bin map are all built in prepare(). analyze() only reads and
/// writes preallocated storage, so the audio thread never allocates, resizes or
/// throws.
class EqSpectrumAnalyzer {
 public:
  /// @brief Length of the analysis window in samples.
  /// @details 2048 keeps one transform well inside a realtime block budget at
  /// every supported sample rate. The resulting bin spacing is narrower than the
  /// profile bands above a few hundred Hz; the bottom bands are narrower than one
  /// bin at high sample rates and fall back to their nearest bin (see prepare()).
  static constexpr int kFftSize = 2048;
  /// @brief Minimum number of new samples between two transforms.
  static constexpr int kHopSamples = kFftSize / 4;

  /// @brief Allocates the FFT plan, the window and the band-to-bin map.
  /// @param sample_rate Processing sample rate; must be positive.
  void prepare(double sample_rate);
  /// @brief Drops the accumulated window and the smoothed profile.
  void reset() noexcept;
  /// @brief Appends one block and writes the current profile to @p out.
  /// @details @p out is always filled: with the freshly computed profile when a
  /// hop has elapsed, otherwise with the profile of the latest transform.
  void analyze(const float* const* channels, int num_channels, int num_samples,
               std::array<float, kSpectrumProfileBands>& out) noexcept;

 private:
  void append(const float* const* channels, int num_channels, int num_samples) noexcept;
  void transform(int elapsed_samples) noexcept;

  std::unique_ptr<sonare::FFT> fft_;
  double sample_rate_ = 0.0;
  // Analysis window and the two gain corrections derived from it: amplitude
  // scaling (2 / sum(w)) so a windowed sine reports its own amplitude, and the
  // equivalent noise bandwidth (N * sum(w^2) / sum(w)^2) that the per-band power
  // sum is divided by.
  std::vector<float> window_;
  float window_scale_ = 0.0f;
  float window_enbw_ = 1.0f;
  // Circular analysis window, always full: it starts zeroed, so a partially
  // filled window analyses as a zero-padded block instead of being skipped.
  std::vector<float> ring_;
  size_t ring_pos_ = 0;
  std::vector<float> frame_;
  std::vector<std::complex<float>> bins_;
  // Half-open bin range per profile band. A band narrower than one bin collapses
  // to the single bin nearest its centre; a band entirely above Nyquist is empty.
  std::array<int, kSpectrumProfileBands> band_bin_begin_{};
  std::array<int, kSpectrumProfileBands> band_bin_end_{};
  std::array<float, kSpectrumProfileBands> profile_db_{};
  int samples_since_transform_ = 0;
  bool has_profile_ = false;
  // Set only once prepare() has built every buffer. analyze() is inert until
  // then, so a never-prepared or part-way-failed analyser cannot be read.
  bool ready_ = false;
};

}  // namespace sonare::mastering::eq
