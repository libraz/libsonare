#pragma once

/// @file f0_salience.h
/// @brief The cent-axis salience surface multiple-F0 estimation reads.
///
/// Three steps that commit to no note: an instantaneous-frequency spectrum, a
/// per-bin tonality weight, and a harmonic sum over a cent axis. The axis is
/// cents rather than Hz because dividing a partial's frequency by its harmonic
/// number is a constant shift there, so the harmonic sum is a weighted sum of
/// shifted copies of one array rather than a scattered gather.
///
/// Octave errors are the surface's dominant failure. A candidate an octave under
/// a real note collects that note's every second partial and one an octave over
/// collects its even ones, so both stand where nothing is sounding. The partial
/// weighting reduces them and does not remove them; surviving them is estimation's
/// problem, in multi_f0.h.

#include <vector>

#include "core/spectrum.h"

namespace sonare::editing::polyphony {

/// @brief STFT geometry this model is calibrated for.
/// @details At 44.1 kHz a four-note chord resolves from about C3 up at n_fft
///          4096, and 2048 reaches only C4. A chord rooted at C2 is under all of
///          them: 4096 resolves none of its four voices, 8192 resolves three,
///          and 16384 is the first to resolve four. The voice 8192 drops is the
///          chord's top note rather than its root -- the lower voices take the
///          salience peaks and the highest is left under the frame floor. The
///          hop is an eighth of the window because the instantaneous frequency
///          is a phase difference across one hop and so averages over it.
inline StftConfig polyphony_stft_defaults() { return make_stft_config(4096, 512); }

/// @brief A log-frequency axis, shared by the cent spectrum and the F0 surface.
struct CentAxis {
  /// Frequency of bin 0.
  float ref_hz = 55.0f;
  /// A third of a semitone: finer than the estimator's own separation rule, and
  /// coarse enough to keep the surface small.
  ///
  /// Everything that takes an axis rejects one finer than one cent or wider than
  /// 32768 bins -- including a hand-built axis, so the bound is the axis's and
  /// not @ref compute_cent_spectrum's. The floor is reachable, not a margin: on
  /// the calibrated STFT (44.1 kHz, n_fft 4096, hop 512) the phase-based
  /// instantaneous frequency this axis reads resolves under 1 cent at 55 Hz
  /// above ~20 dB SNR, and well under it by 8 kHz.
  float cents_per_bin = 100.0f / 3.0f;
  /// Covers [ref_hz, max_hz] inclusive: @c ceil(cents(max_hz) / cents_per_bin)
  /// plus one, so the last bin is at or above @c max_hz rather than under it.
  ///
  /// A span that lands on a whole number gets one bin more than it needs, since
  /// the resolution is a float and a third of a semitone is not one: 55 Hz to
  /// 1760 Hz is exactly 6000 cents and still measures 180.0000069 bins wide.
  /// Harmless, and stated so that the spare bin is not read as an off-by-one.
  int n_bins = 0;

  /// @brief Frequency at a bin, which may be fractional.
  float hz_at(float bin) const;
  /// @brief Bin a frequency sits at. Fractional, and may fall outside
  ///        [0, n_bins) -- callers range-check rather than being rejected here.
  float bin_at(float hz) const;
};

/// @brief Per-bin steadiness of the instantaneous frequency, in [0, 1].
/// @details Across the bins of one steady partial the instantaneous frequency is
///          flat; across noise it tracks the bin centres and so advances by one
///          bin per bin. The weight is one minus that advance **signed**,
///          clamped, and has no free parameter. Edge bins take a one-sided
///          difference.
///
///          Signed means a falling advance reads as perfectly steady, which
///          looks like a defect and is load-bearing: between two partials too
///          close to resolve, the instantaneous frequency falls, and keeping
///          those bins at full weight is what holds a low chord's unresolved
///          partials in the sum. Taking the absolute value is tidier, separates
///          noise marginally better frame by frame, and costs a voice of a
///          four-note chord at C3 once ridges are tracked.
/// @param inst_freq_hz Instantaneous frequency per bin, @p n_bins long.
/// @param n_bins Bin count, at least 2.
/// @param bin_hz Spacing between bin centres.
/// @return One weight per bin.
/// @throws SonareException(InvalidParameter) on fewer than two bins or a
///         non-positive @p bin_hz.
std::vector<float> tonality_weights(const float* inst_freq_hz, int n_bins, float bin_hz);

struct CentSpectrumConfig {
  float ref_hz = 55.0f;
  /// Rejected under one cent, and the axis it builds is rejected over 32768
  /// bins: positive-and-finite bounds a size not at all, and an unbounded axis
  /// costs whatever the allocator decides. The floor is reachable rather than a
  /// margin -- see @ref CentAxis::cents_per_bin for the measurement.
  float cents_per_bin = 100.0f / 3.0f;
  /// Top of the axis. The harmonic sum reads partial positions rather than F0s,
  /// so a partial over it contributes nothing, and a candidate high enough for
  /// its upper partials to clear the ceiling sums fewer of them than a low one
  /// does. At the default that is every F0 over 400 Hz, which is most of the F0
  /// range -- so the bias is the normal case rather than an edge one.
  ///
  /// It is still the measured default. Raising it does not recover an F0 at any
  /// register tested, including one whose partials are almost all over the
  /// ceiling, and at 20 kHz it loses a true F0 under a loud transient by letting
  /// the transient's high end into the sum. The ceiling is doing more as a noise
  /// bound than the missing partials cost.
  float max_hz = 8000.0f;
  /// Weights each bin by @ref tonality_weights. Measured contribution: none on
  /// clean or lightly noisy material, and on a chord under a transient 12 dB
  /// above it, one true F0 recovered and one false one dropped.
  bool use_tonality = true;
};

/// @brief One column per STFT frame, magnitude folded onto the cent axis.
struct CentSpectrum {
  CentAxis axis{};
  /// [n_frames][axis.n_bins]. Frame-major, so one column is contiguous -- the
  /// transpose of Spectrogram's layout, because estimation runs a column at a
  /// time and subtracts into it repeatedly.
  ///
  /// The footprint is the product, and only the axis is bounded: a long file at
  /// a fine axis is large whatever @c cents_per_bin was allowed to be. It is the
  /// same everywhere, since nothing here allocates per thread, but a runtime with
  /// its own memory ceiling reaches it sooner than the host does.
  std::vector<float> values;
  int n_frames = 0;
  int hop_length = 0;
  int sample_rate = 0;

  const float* column(int frame) const;
  float* column(int frame);
};

/// @brief Folds @p spec onto a cent axis, placing each bin at its instantaneous
///        frequency rather than at its centre.
/// @details The frequency comes from the phase difference against the previous
///          frame, so frame 0 has none of its own and takes frame 1's. Each bin
///          is split between the two cent bins it falls between, by linear
///          interpolation, so a partial drifting within one bin moves smoothly
///          instead of snapping.
/// @throws SonareException(InvalidParameter) on a spectrogram with fewer than
///         two frames or two bins, a non-positive @c ref_hz, a
///         @c cents_per_bin under one cent, a @c max_hz at or below
///         @c ref_hz, or an axis of over 32768 bins.
CentSpectrum compute_cent_spectrum(const Spectrogram& spec, const CentSpectrumConfig& config = {});

struct SalienceConfig {
  float f0_min_hz = 55.0f;
  float f0_max_hz = 1760.0f;
  /// At most 128, which the tables are sized by. Partial 128 of even the lowest
  /// F0 the default axis reaches is past audio, so the ceiling costs nothing a
  /// caller can use.
  int n_harmonics = 20;
  /// The (f0 + alpha) / (h * f0 + beta) partial weight, both in Hz. It is what
  /// keeps a candidate an octave under a real note from winning on that note's
  /// even partials alone. See Klapuri, ISMIR 2006.
  float alpha_hz = 27.0f;
  float beta_hz = 320.0f;
  /// B in f_h = h * f0 * sqrt(1 + B * h^2). 0 is the ideal harmonic series; a
  /// stiff string is positive and stretches its upper partials sharp.
  float inharmonicity = 0.0f;
};

/// @brief Harmonic sum of one cent spectrum, one column per frame.
struct SalienceSurface {
  /// Runs from @c f0_min_hz, at the cent spectrum's own resolution.
  CentAxis axis{};
  /// [n_frames][axis.n_bins], frame-major like @ref CentSpectrum.
  std::vector<float> values;
  int n_frames = 0;
  int hop_length = 0;
  int sample_rate = 0;

  const float* column(int frame) const;
};

/// @brief The harmonic-sum tables, built once and reused across every frame and
///        every iteration of one frame's estimation.
/// @details Holding them is the point: the partial positions depend only on the
///          two axes and the harmonic count, so the per-frame work is a gather
///          and a multiply-add rather than a logarithm per partial.
class SalienceKernel {
 public:
  /// @throws SonareException(InvalidParameter) on a @p spectrum_axis that is
  ///         empty, over 32768 bins, or finer than one cent; a harmonic count
  ///         outside [1, 128]; an @c f0_max_hz at or below @c f0_min_hz; an
  ///         @c f0_min_hz at or below zero; a non-positive @c beta_hz; or a
  ///         negative @c inharmonicity.
  SalienceKernel(const CentAxis& spectrum_axis, const SalienceConfig& config);

  const CentAxis& spectrum_axis() const noexcept { return spectrum_axis_; }
  const CentAxis& f0_axis() const noexcept { return f0_axis_; }
  int n_harmonics() const noexcept { return n_harmonics_; }

  /// @brief Cent-spectrum bin, fractional, where a partial of an F0 sits.
  /// @param f0_bin Index into @ref f0_axis.
  /// @param harmonic 1-based, so 1 is the fundamental.
  /// @details May fall outside the spectrum axis, which is how a partial over
  ///          @c max_hz reads as absent rather than as an error.
  float partial_bin(int f0_bin, int harmonic) const;

  /// @brief Weight that partial carries into the sum.
  float partial_weight(int f0_bin, int harmonic) const;

  /// @brief Harmonic-sums one cent-spectrum column into one salience column.
  /// @param column @c spectrum_axis().n_bins values.
  /// @param out @c f0_axis().n_bins values, overwritten.
  void evaluate(const float* column, float* out) const;

  /// @brief Removes one F0's partials from a column, in place.
  /// @details Reads and writes through the same interpolation weights
  ///          @ref evaluate reads with, so what a call removes is exactly what
  ///          the next evaluation stops seeing. That exactness needs the amount
  ///          divided by those weights' squared norm: writing back the value as
  ///          read returns @c 2f(1-f) of it, which would leave a partial halfway
  ///          between two bins at half magnitude and make @p factor mean a
  ///          different share at every position on the grid. Clamped at zero,
  ///          and a partial outside the axis removes nothing.
  /// @param column @c spectrum_axis().n_bins values, modified.
  /// @param f0_bin Index into @ref f0_axis.
  /// @param factor Share of each partial to take, in (0, 1].
  /// @return Total removed, in the column's own units.
  float subtract(float* column, int f0_bin, float factor) const;

 private:
  CentAxis spectrum_axis_{};
  CentAxis f0_axis_{};
  int n_harmonics_ = 0;
  /// [f0_bin][harmonic - 1], both harmonic-major within one F0.
  std::vector<float> partial_bins_;
  std::vector<float> partial_weights_;
};

/// @brief Harmonic-sums every column of @p spectrum.
/// @throws SonareException(InvalidParameter) for the reasons @ref SalienceKernel
///         throws, plus an empty spectrum.
SalienceSurface compute_salience(const CentSpectrum& spectrum, const SalienceConfig& config = {});

}  // namespace sonare::editing::polyphony
