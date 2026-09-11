#pragma once

/// @file multi_f0.h
/// @brief Multiple-F0 estimation and the ridges it follows across time.
///
/// A salience column is turned into voices by iterated estimation-subtraction:
/// take the strongest F0, remove its partials, look again. Across frames the
/// voices are then followed as ridges, so the result is a set of pitches each
/// holding over a span.
///
/// It is an analysis and not a separation. A ridge says a pitch was present over
/// a span; it carries no signal of its own, and nothing here decides how two
/// notes sharing a partial divide it.

#include <cstdint>
#include <vector>

#include "core/audio.h"
#include "editing/polyphony/f0_salience.h"

namespace sonare::editing::polyphony {

/// @brief One F0 found in one frame.
/// @details A default-constructed one is not a valid input to
///          @ref track_f0_ridges: @c f0_hz defaults to 0, which that function
///          rejects. The default exists so the struct is an aggregate, not so a
///          zeroed one means anything.
struct F0Candidate {
  float f0_hz = 0.0f;

  /// The harmonic sum that selected it, in the cent spectrum's own scale, which
  /// is the STFT magnitude's and is not normalized. Rank candidates inside one
  /// frame with it; a threshold written against one take's values is not
  /// portable to another.
  float salience = 0.0f;

  /// Share of the column's energy this candidate's partials removed, in [0, 1],
  /// measured against the column's total before any subtraction.
  ///
  /// It says how much of what the frame holds this F0 accounts for, not whether
  /// the frame is tonal at all: measured, the fourth voice of a chord reads 0.14
  /// while a noise frame's first candidate reads 0.10 to 0.24. So it ranks
  /// within a frame and does not separate a chord from noise.
  float harmonic_share = 0.0f;
};

struct MultiF0Config {
  SalienceConfig salience{};

  /// Hard cap on voices in one frame, itself capped at 64 because the iteration
  /// bound is derived from it. Iterated subtraction is not the method for a
  /// thick piano chord, so raising it past a handful buys little anyway.
  int max_polyphony = 4;

  /// Stops the iteration when a peak falls under this share of **the frame's
  /// first peak** -- relative, because salience carries the magnitude's own
  /// scale. Measured, a real voice's peak holds above 0.45 of the first and a
  /// spent iteration falls to 0.07, so the default sits between them rather than
  /// near either edge. It does not reject a noise frame, whose peaks decay
  /// gently and stay over it; @ref RidgeConfig::min_duration_ms drops those.
  ///
  /// That band was measured on rendered audio, where an STFT's leakage smears
  /// the partials a ghost would stand on. A column built from exact partial
  /// positions has no such smearing and puts a harmonic of a single voice at
  /// 0.23 of the first peak -- inside the gap, so a synthetic column returns a
  /// ghost the same material would not.
  ///
  /// Not @ref RidgeConfig::min_ridge_peak_ratio, which is measured against one
  /// ridge's own peak over time rather than against one frame's strongest voice.
  float min_frame_peak_ratio = 0.20f;

  /// Two peaks closer together than this are one voice. Without it the same F0
  /// is taken every iteration until the cap, because subtracting a peak leaves
  /// the interpolation's neighbours standing.
  ///
  /// It does not decide anything about unison. Two voices at one pitch raise one
  /// salience peak between them, so the method has one voice to find however
  /// this is set -- lowering it to zero does not recover the second, it only
  /// re-admits the duplicates this exists to remove.
  ///
  /// What a detuned unison does reach is the ridge, through the beat rather than
  /// through this: two voices ten cents apart at 300 Hz beat at 1.7 Hz, and the
  /// null carries the salience under @ref RidgeConfig::min_ridge_peak_ratio
  /// about twice a second. Every fragment sits at the one pitch, so the pitch is
  /// right and the continuity is not -- a unison held over a long analysis
  /// arrives as several ridges at one F0, not one ridge.
  float min_separation_cents = 50.0f;

  /// Share of a candidate's partials removed before the next iteration. 1 takes
  /// them whole, which lets the first of two notes sharing a partial have all of
  /// it -- dividing a shared partial is not decided here.
  float subtraction_factor = 1.0f;
};

/// @brief Estimates the F0s in one cent-spectrum column.
/// @details Not thread-safe. One instance owns the scratch an estimation runs
///          in, the same way a Spectrogram owns its lazy caches, so give each
///          thread its own.
class MultiF0Estimator {
 public:
  /// @throws SonareException(InvalidParameter) for the reasons
  ///         @ref SalienceKernel throws, plus a @c max_polyphony outside
  ///         [1, 64], a
  ///         @c min_frame_peak_ratio outside [0, 1], a negative
  ///         @c min_separation_cents, or a @c subtraction_factor outside (0, 1].
  MultiF0Estimator(const CentAxis& spectrum_axis, const MultiF0Config& config);

  const SalienceKernel& kernel() const noexcept { return kernel_; }
  const MultiF0Config& config() const noexcept { return config_; }

  /// @brief Estimates one column.
  /// @param column @c kernel().spectrum_axis().n_bins values, not modified.
  /// @return Candidates in the order they were taken, so by descending salience.
  ///         Empty for a silent column. A peak the separation rule rejects still
  ///         has its partials removed before the iteration continues, so
  ///         rejecting one does not leave it to be taken again.
  /// @details The F0 of each is refined off the salience peak by parabolic
  ///          interpolation, so a returned value is not confined to the axis.
  std::vector<F0Candidate> estimate(const float* column) const;

 private:
  SalienceKernel kernel_;
  MultiF0Config config_;
  mutable std::vector<float> residual_;
  mutable std::vector<float> salience_;
};

/// @brief One F0 followed across consecutive frames.
struct F0Ridge {
  /// [frame_start, frame_end()) in the analysis framing's frames.
  int frame_start = 0;
  /// One value per frame of the span, never empty.
  std::vector<float> f0_hz;
  /// Salience over the same frames, the same length as @ref f0_hz.
  std::vector<float> salience;

  /// Span in source samples the frames cover. @ref extract_multi_f0 holds it
  /// inside the audio it read, so a set it returns is always sliceable over that
  /// audio. @ref track_f0_ridges is handed no length and cannot, so a ridge
  /// reaching the last frame ends past it -- centre padding makes that the
  /// normal case rather than an edge one.
  int64_t onset_sample = 0;
  int64_t offset_sample = 0;

  /// Median of @ref f0_hz.
  float median_hz = 0.0f;

  int frame_end() const noexcept { return frame_start + static_cast<int>(f0_hz.size()); }
  int64_t length_samples() const noexcept { return offset_sample - onset_sample; }
};

struct RidgeConfig {
  /// Breaks a ridge when the move between two frames exceeds this. It bounds the
  /// per-frame move and not the excursion, and the per-frame move a vibrato of
  /// depth @c d cents at rate @c r Hz makes is @c d*2*pi*r*hop/sample_rate.
  ///
  /// So it is calibrated to the hop and does not travel with the framing. The
  /// default and 5.5 Hz:
  ///
  /// | depth | hop 512 | hop 1024 |
  /// |---|---|---|
  /// | +-45 cents | 18 cents/frame | 36 cents/frame |
  /// | +-100 cents | 40 cents/frame | 80 cents/frame, which breaks |
  ///
  /// The longer hop is the one @ref polyphony_stft_defaults recommends for bass
  /// material, so raising the window for the register and leaving this alone
  /// splits a deep vibrato into ridges. Scale it with the hop, or accept that
  /// the bass framing tracks a shallower vibrato than the default one does.
  float max_jump_cents = 50.0f;

  /// Breaks a ridge when a candidate's salience falls under this share of **that
  /// ridge's own running peak**, so it follows a note fading out rather than a
  /// frame's loudest voice. Not @ref MultiF0Config::min_frame_peak_ratio.
  float min_ridge_peak_ratio = 0.10f;

  /// Drops a ridge shorter than this. Every ridge white or pink noise produces
  /// is under ten frames, and a held chord's voices run the length of the
  /// signal, so the two separate anywhere from about 120 to 175 ms; under that
  /// noise survives, and over it a fragmented low-register voice starts being
  /// dropped.
  float min_duration_ms = 140.0f;
};

/// @brief Follows per-frame candidates across time.
/// @details Each live ridge takes the nearest unclaimed candidate of the frame,
///          ridges being offered in the order they started and candidates
///          considered by descending salience with ties going to the lower F0,
///          so the same input always produces the same ridges. A candidate no
///          ridge takes starts one.
/// @param frames One entry per analysis frame; an empty entry is a frame with no
///        voices, which ends every ridge crossing it.
/// @param hop_length Framing the frames came from, used for the sample spans.
/// @param sample_rate Source rate, used for the sample spans and the duration.
/// @return Ridges by ascending @c frame_start, ties by ascending @c median_hz.
/// @throws SonareException(InvalidParameter) on a non-positive @p hop_length or
///         @p sample_rate, a non-positive @c max_jump_cents, a
///         @c min_ridge_peak_ratio outside [0, 1], a negative @c min_duration_ms,
///         a candidate whose @c f0_hz is not positive and finite, or one whose
///         @c salience is not finite -- the ordering is by salience, so a NaN
///         there makes the comparison intransitive rather than merely odd.
std::vector<F0Ridge> track_f0_ridges(const std::vector<std::vector<F0Candidate>>& frames,
                                     int hop_length, int sample_rate,
                                     const RidgeConfig& config = {});

/// @brief Everything one extraction found.
struct MultiF0Track {
  std::vector<F0Ridge> ridges;
  /// Voices estimated per frame, before tracking dropped anything, so it can
  /// exceed the ridges alive at that frame. @c n_frames long.
  std::vector<int> polyphony;
  int n_frames = 0;
  int hop_length = 0;
  int sample_rate = 0;

  float frame_rate_hz() const noexcept;
};

struct MultiF0ExtractorConfig {
  StftConfig stft = polyphony_stft_defaults();
  CentSpectrumConfig spectrum{};
  MultiF0Config estimation{};
  RidgeConfig ridges{};
};

/// @brief Finds the F0s in @p audio and follows each across time.
/// @details The register the framing reaches is the first thing to check against
///          a disappointing result: at the default n_fft a four-note chord
///          resolves from about C3 up, and under that the estimator reports
///          composite pitches rather than nothing.
/// @throws SonareException(InvalidParameter) on empty audio, plus every reason
///         @ref compute_cent_spectrum, @ref MultiF0Estimator and
///         @ref track_f0_ridges throw.
MultiF0Track extract_multi_f0(const Audio& audio, const MultiF0ExtractorConfig& config = {});

}  // namespace sonare::editing::polyphony
