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

  /// Frames back from this one to where the plain STFT magnitude summed at
  /// this candidate's first eight partials last crossed half of its rise over
  /// the two preceding analysis windows. The window's coverage of an attack is
  /// half at its centre, so for a note struck at once that point is the
  /// strike; the magnitude is read without phase placement, since a partial's
  /// phase advance settles only a hop after the attack, and the latest
  /// crossing is taken so an earlier event still in view is not read as this
  /// one's. 0 where the magnitude did not double over the history (no attack
  /// in view), and always when @ref RidgeConfig::attack_window_ms is 0.
  float attack_frames = 0.0f;
};

/// Ceiling on @ref MultiF0Config::max_polyphony. The iteration bound is derived
/// from that field, so this is what bounds the iteration too.
inline constexpr int kMaxPolyphonyVoices = 64;

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
///
///          The kernel's F0 axis runs one octave past @c salience.f0_max_hz. A
///          note over the range then peaks at its own F0, is subtracted, and is
///          dropped; on an axis ending at the bound its only place to land is
///          the sub-octave bin, which reads as a note an octave low.
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
  /// @return Candidates in the order they were taken, which is by descending
  ///         salience except where a peak was explained by a lower note (below).
  ///         Empty for a silent column. A peak the separation rule rejects, or
  ///         one over @c f0_max_hz, still has its partials removed before the
  ///         iteration continues, so rejecting one does not leave it to be taken
  ///         again.
  /// @details The F0 of each is refined off the salience peak by parabolic
  ///          interpolation, so a returned value is not confined to the axis.
  ///
  ///          A peak is not taken as it stands when it sits on a partial of a
  ///          lower note that is itself sounding: the kernel weights a
  ///          candidate's own fundamental about 2.6 times what the true F0 gets
  ///          for that same partial, so a strong third partial, which a piano
  ///          has, puts the peak at the twelfth. For each sub-harmonic position
  ///          (1/4, 1/3 and 1/2 of the peak, deepest first) the sum of the
  ///          partials the peak's series does not share is read; where it holds
  ///          at least @ref kSubharmonicOwnShare of that candidate's whole sum,
  ///          @ref kSubharmonicPeakShare of the peak, and rests on a fundamental
  ///          of its own (@ref kSubharmonicFundamentalShare), the lower note is
  ///          taken first and the peak is left to the residual. Measured on
  ///          rendered piano, a true F0 under its own harmonic keeps 0.5 to 0.65
  ///          of its sum on unshared partials and a sub-harmonic image of a lone
  ///          note under 0.01. A peak at the octave or the twelfth of a note
  ///          already taken this frame is what that note's subtraction left --
  ///          the partials over the kernel's reach and the stretched ones -- and
  ///          is cleared without being returned; so a note at an octave or a
  ///          twelfth over a sounding one is not recovered, which full-partial
  ///          subtraction had already decided whenever the lower note came first.
  std::vector<F0Candidate> estimate(const float* column) const;

 private:
  /// The bin to take for @p peak (refined to @p peak_hz): a sounding
  /// sub-harmonic, else @p peak itself; negative when a candidate in @p taken
  /// already accounts for it.
  int explaining_subharmonic(int peak, float peak_hz, const std::vector<F0Candidate>& taken) const;

  SalienceKernel kernel_;
  MultiF0Config config_;
  /// Top of the configured range as the unextended axis would have placed it.
  float report_max_hz_ = 0.0f;
  mutable std::vector<float> residual_;
  mutable std::vector<float> salience_;
};

/// Share of a sub-harmonic candidate's salience that has to lie on partials its
/// harmonic does not share before it is read as a sounding note; the measured
/// cases sit at 0.5 and over for a true F0 and under 0.01 for an image.
inline constexpr float kSubharmonicOwnShare = 0.3f;
/// That unshared sum, as a share of the peak it would displace: measured 0.38
/// (A2 under its twelfth) and 0.60 (C4 under its twelfth) on rendered piano.
inline constexpr float kSubharmonicPeakShare = 0.25f;
/// Share of the unshared sum the candidate's own fundamental has to carry, so a
/// sub-octave image of a chord note -- which has every partial but a fundamental
/// -- is not read as a note; measured 0.58 and 0.73 for true F0s.
inline constexpr float kSubharmonicFundamentalShare = 0.2f;

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

  /// Splits a ridge where its own salience climbs past this multiple of the
  /// level just before, the same pitch struck again while it still sounds.
  /// 0 turns the split off; any other value has to be finite and above 1. The
  /// climb is read over about 35 ms, which holds an attack but too little of a
  /// 1-2 Hz beat's rise to reach the ratio, against the highest frame of the
  /// 35 ms before it, so a one-frame dip -- the frame in which another
  /// candidate took this pitch's partials first -- is not read as a decay. A
  /// level swell of the ratio within that span is a re-strike by this rule
  /// whatever produced it; a looped sample's swell of 2x over 100 ms is cut at 2.
  float reattack_ratio = 0.0f;

  /// How long a ridge survives frames offering it no candidate, so a one- or
  /// two-frame dropout -- a beat minimum, or the frame's search ending before
  /// it reached the pitch -- does not return a held note as two ridges. The
  /// skipped frames are filled by linear interpolation between the values on
  /// either side; a ridge still in a gap when the frames end stops at its last
  /// measured frame. 0 ends a ridge on the first frame without a candidate.
  /// Finite and non-negative.
  float max_gap_ms = 0.0f;

  /// Length of the analysis window the frames came from. When set, a ridge
  /// begins at the frame its @ref F0Candidate::attack_frames places the strike
  /// on, read half a window into the ridge so the window of that reading
  /// brackets the attack, rather than at the frame a candidate was first
  /// admitted: the window reaches a lone note up to half a window before its
  /// centre does, and a note under a louder voice, or one with a long
  /// transient, is admitted frames after it struck. The reading reaches two
  /// windows back, so a bass note under a chord, admitted 60-100 ms after its
  /// strike, still has the strike in view. Leading frames before the
  /// strike are dropped and missing ones filled with the first value, though a
  /// filled frame is not followed evidence and does not count toward
  /// @ref min_duration_ms; the start never reaches back into a ridge within
  /// @ref max_jump_cents. 0 keeps the admitted frame. Finite and non-negative.
  float attack_window_ms = 0.0f;
};

/// @brief Follows per-frame candidates across time.
/// @details Each live ridge takes the nearest unclaimed candidate of the frame,
///          ridges being offered in the order they started and candidates
///          considered by descending salience with ties going to the lower F0,
///          so the same input always produces the same ridges. A candidate no
///          ridge takes starts one.
/// @param frames One entry per analysis frame; an empty entry is a frame with no
///        voices, which ends every ridge crossing it once its @c max_gap_ms is
///        spent.
/// @param hop_length Framing the frames came from, used for the sample spans.
/// @param sample_rate Source rate, used for the sample spans and the duration.
/// @return Ridges by ascending @c frame_start, ties by ascending @c median_hz.
/// @throws SonareException(InvalidParameter) on a non-positive @p hop_length or
///         @p sample_rate, a non-positive @c max_jump_cents, a
///         @c min_ridge_peak_ratio outside [0, 1], a negative
///         @c min_duration_ms, @c max_gap_ms or @c attack_window_ms,
///         a @c reattack_ratio that is neither 0 nor finite above 1, a candidate
///         whose @c f0_hz is not positive and finite, or one whose
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
  /// Samples at @ref kPolyphonyReferenceRate, converted to the input's rate.
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
///
///          The STFT is read through @ref for_each_stft_tile and never held whole,
///          so the footprint is the candidates and the ridges rather than the
///          spectrogram. The result is the spectrogram form's bit for bit: a tile
///          holds the same frames, and the cent spectrum of a frame reads only
///          that frame and the one before it.
/// @throws SonareException(InvalidParameter) on empty audio, plus every reason
///         @ref compute_cent_spectrum, @ref MultiF0Estimator and
///         @ref track_f0_ridges throw.
MultiF0Track extract_multi_f0(const Audio& audio, const MultiF0ExtractorConfig& config = {});

/// @brief The same extraction over an STFT the caller already has.
/// @details For the chain that separates and re-renders: @ref build_note_masks
///          needs the same spectrogram this reads, so a caller holding only the
///          audio form computes it twice. @p spec is validated against
///          @c config.stft rather than trusted -- a framing that is not the one
///          the extraction asked for would put every ridge on frames it never
///          measured, and the frame count @p audio implies is part of that check.
///          The comparison is against @c config.stft converted to @p audio's rate,
///          so build @p spec with `polyphony_stft_at_rate(config.stft, rate)`.
///
///          @c config.stft is still read, for that validation. It is not a way to
///          ask for a different framing than @p spec carries.
///
///          What the check reaches is the geometry, not the content: a spectrogram
///          of a different signal of the same length, rate and framing is
///          indistinguishable from the right one and is accepted. Stated rather
///          than fixed -- there is nothing in @p spec to compare against @p audio
///          short of transforming it again, which is the cost this overload exists
///          to avoid.
///
///          The returned track is indexed against @p spec: its @c n_frames is
///          `spec.n_frames()` and every ridge's frames are that spectrogram's. That
///          is the fact a caller chains on -- @ref build_note_masks and
///          @ref make_masked_notes both pair the track with the same @p spec -- so
///          it is stated here rather than left to follow from the framing check.
/// @param audio The signal @p spec was computed from, read for its length and rate
///        and for the span clamping the audio form also does.
/// @param spec That signal's STFT under @c config.stft at its rate
///        (@ref polyphony_stft_at_rate).
/// @throws SonareException(InvalidParameter) on empty @p audio, a @p spec whose
///         geometry is not that of @c config.stft over @p audio -- every framing
///         field, the sample rate, and the frame count @p audio implies -- plus
///         every reason the audio form throws.
MultiF0Track extract_multi_f0(const Audio& audio, const Spectrogram& spec,
                              const MultiF0ExtractorConfig& config = {});

}  // namespace sonare::editing::polyphony
