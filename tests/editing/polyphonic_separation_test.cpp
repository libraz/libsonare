/// @file polyphonic_separation_test.cpp
/// @brief What a mask divided, measured where an edit can expose it.
///
/// Editing one note of a chord and asking whether the others changed cannot see a
/// mask that divided the energy wrongly. The renderer adds each note's own render
/// to the residual with no cross-note term, so an unedited note's contribution is
/// identical between two renders -- identical, not close. A mask that took half of
/// another note's energy is equally present in both, so it never appears as a
/// change, and an assertion about the others holding still passes whatever the
/// masks did.
///
/// So the two failures a mask can have are measured where each is expressible. An
/// under-claiming mask leaves part of its note in the residual, which no edit
/// touches, so moving the note leaves that part sounding at the old pitch. An
/// over-claiming mask is invisible inside one render and shows up against a second
/// population: the same note analysed on its own.
///
/// This file measures and does not gate: every quantity is reported with WARN and
/// the only assertions are the ones no number can change -- shapes agree, buffers
/// are finite and non-empty, the chord resolves two notes, and every band a ratio
/// is taken over carries something.

#include "editing/polyphony/polyphonic_edit.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/note_model/note_object.h"
#include "editing/note_model/note_renderer.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "util/constants.h"

namespace note_model = sonare::editing::note_model;

using sonare::editing::note_model::NoteObject;
using sonare::editing::note_model::NoteRenderConfig;
using namespace sonare::editing::polyphony;

namespace {

/// The framing polyphony_stft_defaults recommends, which the mask and extraction
/// suites already read ridges off at this sample rate.
constexpr int kSampleRate = 44100;
constexpr int kNfft = 4096;
constexpr int kHopLength = 512;
constexpr size_t kSourceSamples = 22050;

/// E4 and B4. At 10.77 Hz bin width E4's 3rd partial (988.88 Hz) and B4's 2nd
/// (987.77 Hz) land in one bin, so shared bins genuinely exist here and an equal
/// split has something it can get wrong.
constexpr float kLowHz = 329.6276f;
constexpr float kHighHz = 493.8833f;
constexpr int kPartials = 10;
constexpr float kToneAmplitude = 0.25f;

/// Half-width of a harmonic's band, in bins. The Hann main lobe at win_length
/// equal to n_fft is four bins wide, so two either side is one whole lobe. Every
/// number in this file uses this one width: a narrower band shrinks them all.
constexpr int kBandHalfBins = 2;

/// @brief How far the additivity identity may sit from zero, against the render's
///        own peak.
/// @details Not a quality figure. The two sides are one set of float additions in
///          two orders, so the gap is a few ULP of the accumulator; 1e-6 of a peak
///          near 0.9 is some fifteen ULP of it, and the sibling suite measures the
///          same telescoping at 2.5e-07 over three notes at this framing.
constexpr double kAdditivityRelative = 1e-6;

// --- Synthetic material ----------------------------------------------------

/// @brief Adds a steady harmonic tone, partials at 1/h amplitude.
void add_tone(std::vector<float>& into, float f0_hz, float amplitude, int n_partials) {
  const double nyquist = 0.5 * static_cast<double>(kSampleRate);
  for (int h = 1; h <= n_partials; ++h) {
    const double hz = static_cast<double>(h) * static_cast<double>(f0_hz);
    if (hz >= nyquist) break;
    // The fixed per-harmonic phase keeps the partials from summing into an
    // impulse train.
    const double phase = 0.37 * static_cast<double>(h) * static_cast<double>(h);
    const float level = amplitude / static_cast<float>(h);
    for (size_t i = 0; i < into.size(); ++i) {
      into[i] += level * static_cast<float>(
                             std::sin(sonare::constants::kTwoPiD * hz * static_cast<double>(i) /
                                          static_cast<double>(kSampleRate) +
                                      phase));
    }
  }
}

sonare::Audio audio_of(std::vector<float> samples) {
  return sonare::Audio::from_vector(std::move(samples), kSampleRate);
}

/// @brief Both fixture pitches, held over the whole source.
sonare::Audio chord_audio() {
  std::vector<float> samples(kSourceSamples, 0.0f);
  add_tone(samples, kLowHz, kToneAmplitude, kPartials);
  add_tone(samples, kHighHz, kToneAmplitude, kPartials);
  return audio_of(std::move(samples));
}

/// @brief One of the fixture pitches alone, at the level it carries in the chord.
sonare::Audio tone_audio(float f0_hz) {
  std::vector<float> samples(kSourceSamples, 0.0f);
  add_tone(samples, f0_hz, kToneAmplitude, kPartials);
  return audio_of(std::move(samples));
}

sonare::Spectrogram spectrogram_of(const sonare::Audio& audio) {
  return sonare::Spectrogram::compute(audio, polyphony_stft_defaults());
}

// --- Reading a buffer ------------------------------------------------------

double peak_of(const sonare::Audio& audio) {
  double highest = 0.0;
  for (size_t i = 0; i < audio.size(); ++i) {
    highest = std::max(highest, std::abs(static_cast<double>(audio[i])));
  }
  return highest;
}

bool all_finite(const sonare::Audio& audio) {
  for (size_t i = 0; i < audio.size(); ++i) {
    if (!std::isfinite(audio[i])) return false;
  }
  return true;
}

// --- Reading a band -------------------------------------------------------

double harmonic_hz(float f0_hz, int h) {
  return static_cast<double>(h) * static_cast<double>(f0_hz);
}

bool below_nyquist(double hz) { return hz < 0.5 * static_cast<double>(kSampleRate); }

int bin_of(double hz) {
  return static_cast<int>(
      std::lround(hz * static_cast<double>(kNfft) / static_cast<double>(kSampleRate)));
}

struct FrameSpan {
  int begin = 0;
  int end = 0;

  int count() const { return end - begin; }
};

FrameSpan span_of(const NoteObject& note, const sonare::Spectrogram& spec) {
  FrameSpan span;
  span.begin = std::max(0, note.frame_start);
  span.end = std::min(spec.n_frames(), note.frame_end);
  return span;
}

FrameSpan intersect(const FrameSpan& a, const FrameSpan& b) {
  FrameSpan span;
  span.begin = std::max(a.begin, b.begin);
  span.end = std::min(a.end, b.end);
  return span;
}

/// @brief Summed |X| over the bins within kBandHalfBins of @p hz, over @p span.
double band_magnitude(const sonare::Spectrogram& spec, double hz, const FrameSpan& span) {
  const int centre = bin_of(hz);
  const int first = std::max(0, centre - kBandHalfBins);
  const int last = std::min(spec.n_bins() - 1, centre + kBandHalfBins);
  double total = 0.0;
  for (int bin = first; bin <= last; ++bin) {
    for (int frame = span.begin; frame < span.end; ++frame) {
      total += static_cast<double>(std::abs(spec.at(bin, frame)));
    }
  }
  return total;
}

double db_of(double got, double reference) { return 20.0 * std::log10(got / reference); }

struct Bins {
  int first = 0;
  int last = 0;
};

/// @brief The bins harmonic @p h of @p f0_hz is read over, clamped to @p spec.
Bins band_bins(const sonare::Spectrogram& spec, float f0_hz, int h) {
  const int centre = bin_of(harmonic_hz(f0_hz, h));
  Bins band;
  band.first = std::max(0, centre - kBandHalfBins);
  band.last = std::min(spec.n_bins() - 1, centre + kBandHalfBins);
  return band;
}

/// @brief Whether a sounding partial of @p other_f0 falls inside @p h's band.
bool other_note_sounds_in_band(float own_f0, int h, float other_f0) {
  const int centre = bin_of(harmonic_hz(own_f0, h));
  for (int g = 1; g <= kPartials; ++g) {
    const double hz = harmonic_hz(other_f0, g);
    if (!below_nyquist(hz)) break;
    if (std::abs(bin_of(hz) - centre) <= kBandHalfBins) return true;
  }
  return false;
}

/// @brief Whether @p other_f0's mask claims any bin of @p h's band.
/// @details Not the question above, and the difference is where the sharpest
///          numbers below come from: the default geometry claims 20 partials while
///          these tones carry 10, so a mask reaches bins its note has nothing at.
bool other_mask_claims_band(const sonare::Spectrogram& spec, float own_f0, int h, float other_f0,
                            const NoteMaskConfig& config) {
  const Bins band = band_bins(spec, own_f0, h);
  for (const PartialClaim& claim : partial_claims(spec, other_f0, config)) {
    if (claim.first_bin <= band.last && claim.last_bin >= band.first) return true;
  }
  return false;
}

/// @brief What else is in @p h's band, so a ratio taken over it is attributable.
std::string band_company(const sonare::Spectrogram& spec, float own_f0, int h, float other_f0,
                         const NoteMaskConfig& config) {
  std::string company;
  if (other_note_sounds_in_band(own_f0, h, other_f0)) company += ", the other note sounds here";
  if (other_mask_claims_band(spec, own_f0, h, other_f0, config)) {
    company += ", the other mask claims it";
  }
  return company;
}

// --- The chain's own per-note step -----------------------------------------

/// @brief One note's contribution to the masked render, built the way
///        @ref render_masked_notes builds it.
sonare::Audio contribution_of(const sonare::Spectrogram& spec, const NoteMask& mask,
                              const NoteObject& note, int length,
                              const NoteRenderConfig& config = {}) {
  const sonare::Audio masked = apply_note_mask(spec, mask).to_audio(length);
  return note_model::render_notes(masked, {note}, config);
}

/// @brief The note of @p notes whose median pitch is closest to @p hz.
size_t note_nearest(const std::vector<NoteObject>& notes, float hz) {
  REQUIRE(!notes.empty());
  size_t best = 0;
  double closest = std::abs(static_cast<double>(notes[0].median_hz) - static_cast<double>(hz));
  for (size_t i = 1; i < notes.size(); ++i) {
    const double away = std::abs(static_cast<double>(notes[i].median_hz) - static_cast<double>(hz));
    if (away < closest) {
      closest = away;
      best = i;
    }
  }
  return best;
}

}  // namespace

// --- (A) Additivity --------------------------------------------------------

TEST_CASE("the render is additive in the notes, which says nothing about apportionment",
          "[polyphony_separation]") {
  // Structural, and the only claim here that is. Note i's contribution is a
  // function of masks.notes[i] and notes[i] alone, so editing note k moves the
  // whole render by exactly note k's own contribution difference. It is blind to
  // how the masks divided the energy: a mask that took half of the other note is
  // equally present in both renders and cancels out of the difference.
  REQUIRE(polyphony_stft_defaults().n_fft == kNfft);
  REQUIRE(polyphony_stft_defaults().hop_length == kHopLength);

  const sonare::Audio audio = chord_audio();
  const PolyphonicAnalysis analysis = analyze_polyphonic(audio);
  REQUIRE(analysis.notes.size() == 2);
  REQUIRE(analysis.length == static_cast<int>(kSourceSamples));

  const sonare::Audio base = render_polyphonic(analysis);
  REQUIRE(base.size() == kSourceSamples);
  REQUIRE(all_finite(base));
  const double base_peak = peak_of(base);
  REQUIRE(base_peak > 0.0);
  WARN("the chord renders to peak " << base_peak << " over " << analysis.spectrum.n_frames()
                                    << " frames");

  const std::vector<std::pair<std::string, std::function<void(NoteObject&)>>> edits = {
      {"a gain of -9 dB", [](NoteObject& note) { note.edit.gain_db = -9.0f; }},
      {"a pitch shift of +1 semitone",
       [](NoteObject& note) { note.edit.pitch_shift_semitones = 1.0f; }},
      {"a mute", [](NoteObject& note) { note.edit.muted = true; }}};

  for (const size_t k : {size_t{0}, size_t{1}}) {
    for (const auto& edit : edits) {
      PolyphonicAnalysis edited = analysis;
      edit.second(edited.notes[k]);
      REQUIRE(!edited.notes[k].edit.is_identity());

      const sonare::Audio after = render_polyphonic(edited);
      const sonare::Audio before_k = contribution_of(analysis.spectrum, analysis.masks.notes[k],
                                                     analysis.notes[k], analysis.length);
      const sonare::Audio after_k = contribution_of(analysis.spectrum, analysis.masks.notes[k],
                                                    edited.notes[k], analysis.length);
      REQUIRE(after.size() == base.size());
      REQUIRE(before_k.size() == base.size());
      REQUIRE(after_k.size() == base.size());
      REQUIRE(all_finite(after));
      REQUIRE(all_finite(before_k));
      REQUIRE(all_finite(after_k));

      double worst = 0.0;
      double moved = 0.0;
      size_t at = 0;
      for (size_t i = 0; i < base.size(); ++i) {
        const double render_delta = static_cast<double>(after[i]) - static_cast<double>(base[i]);
        const double note_delta = static_cast<double>(after_k[i]) - static_cast<double>(before_k[i]);
        moved = std::max(moved, std::abs(render_delta));
        const double gap = std::abs(render_delta - note_delta);
        if (gap > worst) {
          worst = gap;
          at = i;
        }
      }

      const std::string what = "note " + std::to_string(k) + " with " + edit.first;
      WARN(what << ": additivity gap " << worst << " at sample " << at << " (" << (worst / base_peak)
                << " of the render peak), the edit itself moved the render by " << moved);
      // The movement is the non-vacuity control: an edit that changed nothing
      // satisfies the identity trivially, both sides being zero.
      REQUIRE(moved > 0.0);
      REQUIRE(worst <= kAdditivityRelative * base_peak);
    }
  }
}

// --- (B) Ghost: an under-claiming mask -------------------------------------

TEST_CASE("ghost: what a shifted note leaves behind at its original harmonics",
          "[polyphony_separation]") {
  // A mask that left part of its note in the residual cannot move that part: the
  // residual is carried through unedited, so the residue keeps sounding at the old
  // pitch. Measured as the drop at the original harmonic positions when the note is
  // shifted up a semitone, with the rise at the shifted positions reported beside
  // it so a band window too narrow to see anything is visible as both being small.
  const sonare::Audio audio = chord_audio();
  const PolyphonicAnalysis analysis = analyze_polyphonic(audio);
  REQUIRE(analysis.notes.size() == 2);

  const sonare::Audio base = render_polyphonic(analysis);
  REQUIRE(all_finite(base));
  const sonare::Spectrogram before = spectrogram_of(base);
  REQUIRE(before.n_frames() > 1);

  const double semitone = std::pow(2.0, 1.0 / 12.0);
  WARN("the masks claim " << analysis.masks.config.n_harmonics
                          << " partials a note; the tones carry " << kPartials);

  for (const float pitch : {kLowHz, kHighHz}) {
    const float other = (pitch == kLowHz) ? kHighHz : kLowHz;
    const size_t k = note_nearest(analysis.notes, pitch);
    const FrameSpan span = span_of(analysis.notes[k], before);
    WARN("shifting the note at " << analysis.notes[k].median_hz << " Hz (fixture " << pitch
                                 << "), frames [" << span.begin << ", " << span.end << ")");
    REQUIRE(span.count() > 0);

    PolyphonicAnalysis edited = analysis;
    edited.notes[k].edit.pitch_shift_semitones = 1.0f;
    const sonare::Audio shifted = render_polyphonic(edited);
    REQUIRE(shifted.size() == base.size());
    REQUIRE(all_finite(shifted));
    const sonare::Spectrogram after = spectrogram_of(shifted);
    REQUIRE(after.n_frames() == before.n_frames());
    REQUIRE(after.n_bins() == before.n_bins());

    for (int h = 1; h <= kPartials; ++h) {
      const double hz = harmonic_hz(pitch, h);
      if (!below_nyquist(hz)) break;
      const double moved_hz = hz * semitone;
      // A semitone at a low harmonic is under one band width, so the two bands
      // overlap and the pair of numbers is one band read twice.
      const int separation = bin_of(moved_hz) - bin_of(hz);

      const double at_old_before = band_magnitude(before, hz, span);
      const double at_old_after = band_magnitude(after, hz, span);
      const double at_new_before = band_magnitude(before, moved_hz, span);
      const double at_new_after = band_magnitude(after, moved_hz, span);
      REQUIRE(std::isfinite(at_old_before));
      REQUIRE(std::isfinite(at_old_after));
      REQUIRE(std::isfinite(at_new_before));
      REQUIRE(std::isfinite(at_new_after));
      REQUIRE(at_old_before > 0.0);
      REQUIRE(at_old_after > 0.0);
      REQUIRE(at_new_before > 0.0);
      REQUIRE(at_new_after > 0.0);

      WARN("h" << h << " " << hz << " Hz: old band " << db_of(at_old_after, at_old_before)
               << " dB, new band " << db_of(at_new_after, at_new_before) << " dB, |X| old "
               << at_old_before << "->" << at_old_after << ", new " << at_new_before << "->"
               << at_new_after << ", bands " << separation << " bins apart"
               << (separation <= 2 * kBandHalfBins ? " (overlapping)" : "")
               << band_company(before, pitch, h, other, analysis.masks.config));
    }
  }
}

// --- (C) Theft: an over-claiming mask --------------------------------------

TEST_CASE("theft: a note's masked share inside the chord against the same note analysed alone",
          "[polyphony_separation]") {
  // The two populations this needs. Inside one render a mask that took more than
  // its note is not expressible -- the sum telescopes for any division at all. The
  // second population is an analysis of audio holding only that note, where the
  // mask has nothing to take from, so the per-harmonic ratio of the two masked
  // contributions is what the chord's division did to that partial.
  const sonare::Audio audio = chord_audio();
  const PolyphonicAnalysis chord = analyze_polyphonic(audio);
  REQUIRE(chord.notes.size() == 2);

  WARN("the masks claim " << chord.masks.config.n_harmonics << " partials a note; the tones carry "
                          << kPartials);

  for (const float pitch : {kLowHz, kHighHz}) {
    const float other = (pitch == kLowHz) ? kHighHz : kLowHz;
    const size_t j = note_nearest(chord.notes, pitch);
    REQUIRE(chord.notes[j].edit.is_identity());

    const PolyphonicAnalysis alone = analyze_polyphonic(tone_audio(pitch));
    REQUIRE(!alone.notes.empty());
    const size_t a = note_nearest(alone.notes, pitch);
    REQUIRE(alone.notes[a].edit.is_identity());
    WARN("the tone at " << pitch << " alone resolves " << alone.notes.size() << " notes; taking "
                        << alone.notes[a].median_hz << " Hz against the chord's "
                        << chord.notes[j].median_hz << " Hz");

    const sonare::Audio in_chord =
        contribution_of(chord.spectrum, chord.masks.notes[j], chord.notes[j], chord.length);
    const sonare::Audio isolated =
        contribution_of(alone.spectrum, alone.masks.notes[a], alone.notes[a], alone.length);
    REQUIRE(in_chord.size() == kSourceSamples);
    REQUIRE(isolated.size() == kSourceSamples);
    REQUIRE(all_finite(in_chord));
    REQUIRE(all_finite(isolated));

    const sonare::Spectrogram chord_spec = spectrogram_of(in_chord);
    const sonare::Spectrogram alone_spec = spectrogram_of(isolated);
    REQUIRE(chord_spec.n_frames() == alone_spec.n_frames());
    REQUIRE(chord_spec.n_bins() == alone_spec.n_bins());

    // The frames both notes cover, so the two sides are the same stretch of time
    // and not two spans the tracking happened to place differently.
    const FrameSpan span =
        intersect(span_of(chord.notes[j], chord_spec), span_of(alone.notes[a], alone_spec));
    WARN("comparing over frames [" << span.begin << ", " << span.end << "), from chord ["
                                   << chord.notes[j].frame_start << ", "
                                   << chord.notes[j].frame_end << ") and alone ["
                                   << alone.notes[a].frame_start << ", "
                                   << alone.notes[a].frame_end << "); peaks " << peak_of(in_chord)
                                   << " against " << peak_of(isolated));
    REQUIRE(span.count() > 0);

    for (int h = 1; h <= kPartials; ++h) {
      const double hz = harmonic_hz(pitch, h);
      if (!below_nyquist(hz)) break;
      const double in_chord_band = band_magnitude(chord_spec, hz, span);
      const double isolated_band = band_magnitude(alone_spec, hz, span);
      REQUIRE(std::isfinite(in_chord_band));
      REQUIRE(std::isfinite(isolated_band));
      REQUIRE(in_chord_band > 0.0);
      REQUIRE(isolated_band > 0.0);
      WARN("h" << h << " " << hz << " Hz: " << db_of(in_chord_band, isolated_band)
               << " dB, |X| chord " << in_chord_band << " alone " << isolated_band
               << band_company(chord_spec, pitch, h, other, chord.masks.config));
    }
  }
}
