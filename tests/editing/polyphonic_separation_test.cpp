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
/// Three axes are moved one at a time, because a figure from a single configuration
/// cannot say whether it is a constant of the method or a property of the fixture:
/// which note carries the edit, how far the edit moves it, and whether the pair
/// shares a bin at all. The disjoint pair is what the over-claim measurement turns
/// on -- a pair sharing no bin that still shows a loss is not measuring
/// apportionment.
///
/// Every quantity is reported with WARN and both sides of every ratio appear as raw
/// magnitudes beside it, because a bound is only as good as the reader's ability to
/// see what it bounded.
///
/// What is gated is bounded per band, by what sits in the band. A band no other note
/// reaches is where the apportionment had nothing to decide, so its share must be the
/// solo share and the ceiling there is tight. A band the other note has a partial in
/// is what the apportionment exists for, and its ceiling is this file's one quality
/// claim. A band the other note only *claims* carries the cost of a claim made from
/// the predicted geometry rather than from the data, which is a limit with no fix
/// worth shipping -- that ceiling records the size it has today so it cannot grow,
/// and is not a statement that the size is acceptable. A band the chord and the solo
/// analysis claimed differently is skipped and counted, since the apportionment never
/// adds or drops a bin and therefore cannot be what differs.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/note_model/note_object.h"
#include "editing/note_model/note_renderer.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "editing/polyphony/polyphonic_edit.h"
#include "editing/polyphony/shared_bins.h"
#include "util/constants.h"
#include "util/exception.h"

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

/// The second pair's upper pitch, chosen for bin disjointness rather than for an
/// interval: nothing of it comes within 6 bins of anything of E4, counting each
/// note's partials against the other's partials and against the other's claim grid.
constexpr float kDisjointHighHz = 1448.5f;

constexpr int kPartials = 10;
constexpr float kToneAmplitude = 0.25f;

/// Half-width of a harmonic's band, in bins. The Hann main lobe at win_length
/// equal to n_fft is four bins wide, so two either side is one whole lobe. Every
/// number in this file uses this one width: a narrower band shrinks them all,
/// which is why the raw magnitudes are reported beside every ratio.
constexpr int kBandHalfBins = 2;

/// @brief How far the additivity identity may sit from zero, against the render's
///        own peak.
/// @details Not a quality figure. The two sides are one set of float additions in
///          two orders, so the gap is a few ULP of the accumulator; 1e-6 of a peak
///          near 0.9 is some fifteen ULP of it, and the sibling suite measures the
///          same telescoping at 2.5e-07 over three notes at this framing.
constexpr double kAdditivityRelative = 1e-6;

/// @brief Two pitches sounding together, and what their relation is for.
struct Pair {
  const char* what;
  float low_hz;
  float high_hz;

  float other_than(float pitch) const { return pitch == low_hz ? high_hz : low_hz; }
};

/// A fifth flattened until E4's 3rd partial and its own 2nd sound 15 Hz apart
/// instead of 1 -- 1.09 rad/frame at this hop, and 2.19 at the next shared pair.
/// Both partials are real there, which the sharing pair's one shared bin cannot be
/// made to do at that separation.
constexpr float kDetunedHighHz = 486.9414f;

const Pair kFifth{"a fifth, sharing bins", kLowHz, kHighHz};
const Pair kDisjoint{"no bin shared", kLowHz, kDisjointHighHz};
const Pair kDetunedFifth{"a flattened fifth, two real poles far apart in rate", kLowHz,
                         kDetunedHighHz};

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

/// @brief Both of a pair's pitches, held over the whole source.
sonare::Audio chord_audio(const Pair& pair) {
  std::vector<float> samples(kSourceSamples, 0.0f);
  add_tone(samples, pair.low_hz, kToneAmplitude, kPartials);
  add_tone(samples, pair.high_hz, kToneAmplitude, kPartials);
  return audio_of(std::move(samples));
}

/// @brief One pitch alone, at the level it carries in a chord.
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

double wrap_to_pi(double angle) {
  while (angle > sonare::constants::kPiD) angle -= sonare::constants::kTwoPiD;
  while (angle < -sonare::constants::kPiD) angle += sonare::constants::kTwoPiD;
  return angle;
}

/// @brief Rate a partial at @p hz advances at, radians per frame.
double predicted_rate(double hz) {
  return wrap_to_pi(sonare::constants::kTwoPiD * hz * static_cast<double>(kHopLength) /
                    static_cast<double>(kSampleRate));
}

/// @brief Rate one bin's own trajectory advances at, between @p frame and the next.
/// @details The poles the assignment chooses between are not on the public surface,
///          so this is what is observable: where one pole dominates a bin this is
///          that pole's rate, and where two beat it wanders between them.
double observed_rate(const sonare::Spectrogram& spec, int bin, int frame) {
  return wrap_to_pi(static_cast<double>(std::arg(spec.at(bin, frame + 1))) -
                    static_cast<double>(std::arg(spec.at(bin, frame))));
}

/// @brief Harmonic of @p f0_hz whose claim covers @p bin, or 0 if none does.
int claiming_harmonic(const sonare::Spectrogram& spec, float f0_hz, int bin,
                      const NoteMaskConfig& config) {
  for (const PartialClaim& claim : partial_claims(spec, f0_hz, config)) {
    if (bin >= claim.first_bin && bin <= claim.last_bin) return claim.harmonic;
  }
  return 0;
}

// --- What sits where: partials against claims ------------------------------

/// @brief The partials a tone of @p f0_hz actually carries.
std::vector<double> sounding_partials(float f0_hz) {
  std::vector<double> positions;
  for (int h = 1; h <= kPartials; ++h) {
    const double hz = harmonic_hz(f0_hz, h);
    if (!below_nyquist(hz)) break;
    positions.push_back(hz);
  }
  return positions;
}

/// @brief The partials a mask of @p f0_hz claims, which is not the list above: the
///        default geometry claims 20 while these tones carry 10.
std::vector<double> claim_centres(const sonare::Spectrogram& spec, float f0_hz,
                                  const NoteMaskConfig& config) {
  std::vector<double> positions;
  for (const PartialClaim& claim : partial_claims(spec, f0_hz, config)) {
    positions.push_back(static_cast<double>(claim.centre_hz));
  }
  return positions;
}

/// @brief Closest approach between two frequency sets, in bins.
int closest_bins(const std::vector<double>& a, const std::vector<double>& b) {
  REQUIRE(!a.empty());
  REQUIRE(!b.empty());
  int closest = std::numeric_limits<int>::max();
  for (const double x : a) {
    for (const double y : b) {
      closest = std::min(closest, std::abs(bin_of(x) - bin_of(y)));
    }
  }
  return closest;
}

/// @brief Closest approach, in bins, of everything else that can sit in @p h's band.
/// @details Quantitative rather than a flag, because a boolean would put a threshold
///          of this file's between the reader and the number. Both a partial and a
///          claim carry a two-bin main lobe either side of what is reported here, so
///          anything inside about four bins reaches the band.
struct BandCompany {
  int other_partial = 0;
  int other_claim = 0;
  int own_moved_partial = 0;
};

BandCompany band_company(const sonare::Spectrogram& spec, float own_f0, int h, float other_f0,
                         const NoteMaskConfig& config, double factor) {
  const int centre = bin_of(harmonic_hz(own_f0, h));
  const auto nearest = [centre](const std::vector<double>& positions) {
    int closest = std::numeric_limits<int>::max();
    for (const double hz : positions) closest = std::min(closest, std::abs(bin_of(hz) - centre));
    return closest;
  };
  BandCompany company;
  company.other_partial = nearest(sounding_partials(other_f0));
  company.other_claim = nearest(claim_centres(spec, other_f0, config));
  std::vector<double> moved;
  for (int g = 1; g <= kPartials; ++g) {
    if (g == h) continue;
    const double hz = harmonic_hz(own_f0, g) * factor;
    if (!below_nyquist(hz)) break;
    moved.push_back(hz);
  }
  company.own_moved_partial = moved.empty() ? std::numeric_limits<int>::max() : nearest(moved);
  return company;
}

/// @brief How much of a band one mask actually holds, over @p span.
/// @details Separates the two ways a note can be missing its own partial: a bin its
///          mask never claimed, and a bin it claimed at a weight the fit cut down.
struct MaskReach {
  int claimed = 0;
  int cells = 0;
  double mean_modulus = 0.0;
};

MaskReach mask_reach(const NoteMask& mask, const sonare::Spectrogram& spec, double hz,
                     const FrameSpan& span) {
  const int centre = bin_of(hz);
  const int first = std::max(0, centre - kBandHalfBins);
  const int last = std::min(spec.n_bins() - 1, centre + kBandHalfBins);
  MaskReach reach;
  double total = 0.0;
  for (int frame = span.begin; frame < span.end; ++frame) {
    reach.cells += last - first + 1;
    if (frame < mask.frame_start || frame >= mask.frame_end()) continue;
    const size_t f = static_cast<size_t>(frame - mask.frame_start);
    for (int32_t i = mask.frame_offset[f]; i < mask.frame_offset[f + 1]; ++i) {
      const int bin = static_cast<int>(mask.bins[static_cast<size_t>(i)]);
      if (bin < first || bin > last) continue;
      ++reach.claimed;
      total += static_cast<double>(std::abs(mask.weights[static_cast<size_t>(i)]));
    }
  }
  if (reach.claimed > 0) reach.mean_modulus = total / static_cast<double>(reach.claimed);
  return reach;
}

// --- What may be asserted at a band ----------------------------------------

/// Reach of a neighbour, in bins. Wider than the band's own half-width because a
/// partial just outside it still leaks in, and the leak no claim covers is carried
/// in the residual: at 7 bins a neighbour moved a ghost figure 6.03 dB, at 13 none
/// moved more than 2.25.
constexpr int kCompanyReachBins = 8;

/// @brief What sits in a band, which decides what may be asserted there.
enum class BandRole {
  Uncontested,   ///< No other note's partial and no other note's claim within reach.
  PhantomClaim,  ///< The other note claims the band and has no partial in it.
  Shared,        ///< The other note has a partial within reach, so the band is contested.
};

BandRole role_of(const BandCompany& company) {
  if (company.other_partial <= kCompanyReachBins) return BandRole::Shared;
  if (company.other_claim <= kCompanyReachBins) return BandRole::PhantomClaim;
  return BandRole::Uncontested;
}

/// @brief Worst deviation per role, and how many bands each role covered.
/// @details The counts are not bookkeeping: a ceiling over zero bands asserts
///          nothing, so every caller reports them and requires the ones its
///          fixture exists to produce.
struct BandTally {
  int uncontested = 0;
  int phantom = 0;
  int shared = 0;
  int geometry_mismatch = 0;
  double worst_uncontested = 0.0;
  double worst_phantom = 0.0;
  double worst_shared = 0.0;

  void add(BandRole role, double deviation_db) {
    const double size = std::abs(deviation_db);
    switch (role) {
      case BandRole::Uncontested:
        ++uncontested;
        worst_uncontested = std::max(worst_uncontested, size);
        return;
      case BandRole::PhantomClaim:
        ++phantom;
        worst_phantom = std::max(worst_phantom, size);
        return;
      case BandRole::Shared:
        ++shared;
        worst_shared = std::max(worst_shared, size);
        return;
    }
  }

  BandTally& operator+=(const BandTally& other) {
    uncontested += other.uncontested;
    phantom += other.phantom;
    shared += other.shared;
    geometry_mismatch += other.geometry_mismatch;
    worst_uncontested = std::max(worst_uncontested, other.worst_uncontested);
    worst_phantom = std::max(worst_phantom, other.worst_phantom);
    worst_shared = std::max(worst_shared, other.worst_shared);
    return *this;
  }
};

void report_tally(const char* what, const BandTally& tally) {
  WARN("  " << what << ": " << tally.uncontested << " uncontested bands, worst "
            << tally.worst_uncontested << " dB; " << tally.shared << " shared, worst "
            << tally.worst_shared << " dB; " << tally.phantom << " spare-claim, worst "
            << tally.worst_phantom << " dB; " << tally.geometry_mismatch
            << " skipped where the two analyses claimed the band differently");
}

/// Worst measured 0.03 dB over 26 bands. With nothing contesting the band the
/// apportionment has nothing to decide, so the share must be the solo share; this
/// is the one ceiling here that is a correctness claim rather than a recorded size.
constexpr double kUncontestedTheftDb = 0.5;

/// Worst measured 1.67 dB over 8 bands. This is what the apportionment is for, and
/// the only quality figure this file gates: the share is read off the data, so it
/// cannot come out exact.
constexpr double kSharedTheftDb = 3.0;

/// Worst measured 13.46 dB. Not a quality bar -- it records the size of the spare
/// claim @ref sonare::editing::polyphony::build_note_masks documents, which has no
/// fix worth shipping, so the ceiling stops it growing rather than blessing it.
constexpr double kPhantomClaimTheftDb = 20.0;

/// Worst measured 2.25 dB over 38 bands, against the same shift on the tone alone.
/// The shifter's own leakage is in both sides, so only the gap between them is the
/// mask's, and only where nothing else can hold the old band up.
constexpr double kGhostTracksSoloDb = 5.0;

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

// --- The two measurements, one configuration at a time ---------------------

/// @brief Reports the drop at @p pitch's original harmonics when it moves, with
///        the same shift on the tone alone beside every figure.
BandTally measure_ghost(const Pair& pair, float pitch, float semitones) {
  const PolyphonicAnalysis analysis = analyze_polyphonic(chord_audio(pair));
  REQUIRE(analysis.notes.size() == 2);
  const sonare::Audio base = render_polyphonic(analysis);
  REQUIRE(all_finite(base));
  const sonare::Spectrogram before = spectrogram_of(base);
  REQUIRE(before.n_frames() > 1);

  const float other = pair.other_than(pitch);
  const size_t k = note_nearest(analysis.notes, pitch);
  const FrameSpan span = span_of(analysis.notes[k], before);
  WARN(pair.what << ": shifting the note at " << analysis.notes[k].median_hz << " Hz (fixture "
                 << pitch << ") by " << semitones << " semitones, frames [" << span.begin << ", "
                 << span.end << ")");
  REQUIRE(span.count() > 0);

  PolyphonicAnalysis edited = analysis;
  edited.notes[k].edit.pitch_shift_semitones = semitones;
  const sonare::Audio shifted = render_polyphonic(edited);
  REQUIRE(shifted.size() == base.size());
  REQUIRE(all_finite(shifted));
  const sonare::Spectrogram after = spectrogram_of(shifted);
  REQUIRE(after.n_frames() == before.n_frames());
  REQUIRE(after.n_bins() == before.n_bins());

  // The same shift on the tone alone, which is the shifter's own leakage plus
  // whatever an uncontested mask still leaves in the residual.
  const PolyphonicAnalysis alone = analyze_polyphonic(tone_audio(pitch));
  REQUIRE(!alone.notes.empty());
  const size_t a = note_nearest(alone.notes, pitch);
  PolyphonicAnalysis alone_edited = alone;
  alone_edited.notes[a].edit.pitch_shift_semitones = semitones;
  const sonare::Audio alone_base = render_polyphonic(alone);
  const sonare::Audio alone_shifted = render_polyphonic(alone_edited);
  REQUIRE(all_finite(alone_base));
  REQUIRE(all_finite(alone_shifted));
  const sonare::Spectrogram alone_before = spectrogram_of(alone_base);
  const sonare::Spectrogram alone_after = spectrogram_of(alone_shifted);
  REQUIRE(alone_before.n_frames() == before.n_frames());
  const FrameSpan alone_span = span_of(alone.notes[a], alone_before);
  WARN("  the tone alone resolves " << alone.notes.size() << " notes; its span is ["
                                    << alone_span.begin << ", " << alone_span.end << ")");
  REQUIRE(alone_span.count() > 0);

  // The two masks are read over one stretch of time, so a claim count that differs
  // says the geometry differs rather than that the spans were placed differently.
  const FrameSpan common = intersect(span, alone_span);
  REQUIRE(common.count() > 0);

  BandTally tally;
  const double factor = std::pow(2.0, static_cast<double>(semitones) / 12.0);
  for (int h = 1; h <= kPartials; ++h) {
    const double hz = harmonic_hz(pitch, h);
    if (!below_nyquist(hz)) break;
    const double moved_hz = hz * factor;
    // A small shift at a low harmonic moves less than one band width, so the two
    // bands overlap and the pair of numbers is one band read twice.
    const int separation = bin_of(moved_hz) - bin_of(hz);

    const BandCompany company =
        band_company(before, pitch, h, other, analysis.masks.config, factor);

    const double at_old_before = band_magnitude(before, hz, span);
    const double at_old_after = band_magnitude(after, hz, span);
    const double at_new_before = band_magnitude(before, moved_hz, span);
    const double at_new_after = band_magnitude(after, moved_hz, span);
    const double solo_before = band_magnitude(alone_before, hz, alone_span);
    const double solo_after = band_magnitude(alone_after, hz, alone_span);
    REQUIRE(std::isfinite(at_old_before));
    REQUIRE(std::isfinite(at_old_after));
    REQUIRE(std::isfinite(at_new_before));
    REQUIRE(std::isfinite(at_new_after));
    REQUIRE(std::isfinite(solo_before));
    REQUIRE(std::isfinite(solo_after));
    REQUIRE(at_old_before > 0.0);
    REQUIRE(at_old_after > 0.0);
    REQUIRE(at_new_before > 0.0);
    REQUIRE(at_new_after > 0.0);
    REQUIRE(solo_before > 0.0);
    REQUIRE(solo_after > 0.0);

    WARN("  h" << h << " " << hz << " Hz: old band " << db_of(at_old_after, at_old_before)
               << " dB in the chord, " << db_of(solo_after, solo_before) << " dB alone; new band "
               << db_of(at_new_after, at_new_before) << " dB; bands " << separation << " bins apart"
               << (separation <= 2 * kBandHalfBins ? " (overlapping)" : "")
               << "; nearest in bins: other partial " << company.other_partial << ", other claim "
               << company.other_claim << ", own moved partial " << company.own_moved_partial);
    WARN("  h" << h << " |X| chord " << at_old_before << "->" << at_old_after << ", alone "
               << solo_before << "->" << solo_after << ", new band " << at_new_before << "->"
               << at_new_after << "; the tracked pitch puts this partial "
               << (bin_of(harmonic_hz(analysis.notes[k].median_hz, h)) - bin_of(hz))
               << " bins off the band centre");

    // The apportionment never adds or drops a bin, so a band the chord and the solo
    // claimed differently differs by its claim geometry and says nothing here.
    const MaskReach held = mask_reach(analysis.masks.notes[k], analysis.spectrum, hz, common);
    const MaskReach held_alone = mask_reach(alone.masks.notes[a], alone.spectrum, hz, common);
    if (held.claimed != held_alone.claimed) {
      ++tally.geometry_mismatch;
      continue;
    }
    tally.add(role_of(company),
              db_of(at_old_after, at_old_before) - db_of(solo_after, solo_before));
  }

  report_tally("ghost against the same shift on the tone alone", tally);
  // Gated only where nothing else can hold the old band up. Where the other note has
  // a partial in the band its content legitimately stays after this note moves, and
  // where the other note merely claims the band the spare claim's own cost is there,
  // so both are reported rather than bounded.
  REQUIRE(tally.uncontested > 0);
  REQUIRE(tally.worst_uncontested <= kGhostTracksSoloDb);
  return tally;
}

/// @brief Reports @p pitch's masked share inside @p pair against its share when
///        analysed alone, per harmonic.
BandTally measure_theft(const Pair& pair, float pitch) {
  const PolyphonicAnalysis chord = analyze_polyphonic(chord_audio(pair));
  REQUIRE(chord.notes.size() == 2);
  const float other = pair.other_than(pitch);
  const size_t j = note_nearest(chord.notes, pitch);
  REQUIRE(chord.notes[j].edit.is_identity());

  const PolyphonicAnalysis alone = analyze_polyphonic(tone_audio(pitch));
  REQUIRE(!alone.notes.empty());
  const size_t a = note_nearest(alone.notes, pitch);
  REQUIRE(alone.notes[a].edit.is_identity());
  WARN(pair.what << ": the tone at " << pitch << " alone resolves " << alone.notes.size()
                 << " notes; taking " << alone.notes[a].median_hz << " Hz against the chord's "
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
  WARN("  comparing over frames ["
       << span.begin << ", " << span.end << "), from chord [" << chord.notes[j].frame_start << ", "
       << chord.notes[j].frame_end << ") and alone [" << alone.notes[a].frame_start << ", "
       << alone.notes[a].frame_end << "); peaks " << peak_of(in_chord) << " against "
       << peak_of(isolated));
  REQUIRE(span.count() > 0);

  BandTally tally;
  for (int h = 1; h <= kPartials; ++h) {
    const double hz = harmonic_hz(pitch, h);
    if (!below_nyquist(hz)) break;
    const double in_chord_band = band_magnitude(chord_spec, hz, span);
    const double isolated_band = band_magnitude(alone_spec, hz, span);
    REQUIRE(std::isfinite(in_chord_band));
    REQUIRE(std::isfinite(isolated_band));
    REQUIRE(in_chord_band > 0.0);
    REQUIRE(isolated_band > 0.0);
    const MaskReach held = mask_reach(chord.masks.notes[j], chord.spectrum, hz, span);
    const MaskReach held_alone = mask_reach(alone.masks.notes[a], alone.spectrum, hz, span);
    const BandCompany company = band_company(chord_spec, pitch, h, other, chord.masks.config, 1.0);
    WARN("  h" << h << " " << hz << " Hz: " << db_of(in_chord_band, isolated_band)
               << " dB, |X| chord " << in_chord_band << " alone " << isolated_band
               << "; nearest in bins: other partial " << company.other_partial << ", other claim "
               << company.other_claim);
    WARN("  h" << h << " its own mask holds " << held.claimed << "/" << held.cells
               << " cells at mean |w| " << held.mean_modulus << " in the chord, "
               << held_alone.claimed << "/" << held_alone.cells << " at " << held_alone.mean_modulus
               << " alone; the tracked pitch puts this partial "
               << (bin_of(harmonic_hz(chord.notes[j].median_hz, h)) - bin_of(hz))
               << " bins off the band centre (" << chord.notes[j].median_hz << " vs " << pitch
               << " Hz)");

    // The apportionment never adds or drops a bin, so a band the chord and the solo
    // claimed differently differs by its claim geometry and says nothing here.
    if (held.claimed != held_alone.claimed) {
      ++tally.geometry_mismatch;
      continue;
    }
    tally.add(role_of(company), db_of(in_chord_band, isolated_band));
  }

  report_tally("share in the chord against the share alone", tally);
  REQUIRE(tally.uncontested > 0);
  REQUIRE(tally.shared > 0);
  REQUIRE(tally.worst_uncontested <= kUncontestedTheftDb);
  REQUIRE(tally.worst_shared <= kSharedTheftDb);
  REQUIRE(tally.worst_phantom <= kPhantomClaimTheftDb);
  return tally;
}

// --- What the apportionment decided, bin by bin ----------------------------

const char* outcome_name(SharedBinOutcome outcome) {
  switch (outcome) {
    case SharedBinOutcome::Unclaimed:
      return "Unclaimed";
    case SharedBinOutcome::Unshared:
      return "Unshared";
    case SharedBinOutcome::TooFewFrames:
      return "TooFewFrames";
    case SharedBinOutcome::TooManyClaimants:
      return "TooManyClaimants";
    case SharedBinOutcome::F0NotRefined:
      return "F0NotRefined";
    case SharedBinOutcome::PartialsTooClose:
      return "PartialsTooClose";
    case SharedBinOutcome::PolesNotFound:
      return "PolesNotFound";
    case SharedBinOutcome::FitDiverged:
      return "FitDiverged";
    case SharedBinOutcome::AssignmentAmbiguous:
      return "AssignmentAmbiguous";
    case SharedBinOutcome::DegenerateWeight:
      return "DegenerateWeight";
    case SharedBinOutcome::Solved:
      return "Solved";
  }
  return "unnamed";
}

/// @brief One mask's weight at one cell, if it holds that cell at all.
bool mask_weight_at(const NoteMask& mask, int bin, int frame, std::complex<float>& out) {
  if (frame < mask.frame_start || frame >= mask.frame_end()) return false;
  const size_t f = static_cast<size_t>(frame - mask.frame_start);
  for (int32_t i = mask.frame_offset[f]; i < mask.frame_offset[f + 1]; ++i) {
    if (static_cast<int>(mask.bins[static_cast<size_t>(i)]) != bin) continue;
    out = mask.weights[static_cast<size_t>(i)];
    return true;
  }
  return false;
}

/// @brief Smallest and largest of a set, reported as a pair so a constant reads as
///        a constant rather than as an average of two different things.
struct Range {
  double low = std::numeric_limits<double>::infinity();
  double high = -std::numeric_limits<double>::infinity();
  double sum = 0.0;
  int count = 0;

  void add(double value) {
    low = std::min(low, value);
    high = std::max(high, value);
    sum += value;
    ++count;
  }

  double mean() const { return count > 0 ? sum / static_cast<double>(count) : 0.0; }
};

/// @brief One note's weight at one bin over @p span, frame by frame.
Range bin_share(const NoteMask& mask, int bin, const FrameSpan& span) {
  Range range;
  for (int frame = span.begin; frame < span.end; ++frame) {
    std::complex<float> weight;
    if (mask_weight_at(mask, bin, frame, weight)) range.add(static_cast<double>(std::abs(weight)));
  }
  return range;
}

/// @brief The ridge of @p track whose median pitch is closest to @p hz.
size_t ridge_nearest(const MultiF0Track& track, float hz) {
  REQUIRE(!track.ridges.empty());
  size_t best = 0;
  double closest =
      std::abs(static_cast<double>(track.ridges[0].median_hz) - static_cast<double>(hz));
  for (size_t i = 1; i < track.ridges.size(); ++i) {
    const double away =
        std::abs(static_cast<double>(track.ridges[i].median_hz) - static_cast<double>(hz));
    if (away < closest) {
      closest = away;
      best = i;
    }
  }
  return best;
}

/// @brief Reports what the apportionment did at the bin nearest @p hz.
void dump_bin(const std::string& what, const sonare::Spectrogram& spec, const MultiF0Track& track,
              const NoteMaskSet& equal, const NoteMaskSet& fitted, const SharedBinReport& report,
              double hz, const FrameSpan& span) {
  const int bin = bin_of(hz);
  REQUIRE(bin >= 0);
  REQUIRE(bin < spec.n_bins());
  const size_t stride = static_cast<size_t>(spec.n_frames());
  REQUIRE(report.outcome.size() == static_cast<size_t>(spec.n_bins()) * stride);

  // Runs of one outcome rather than a line per frame: a constant verdict over the
  // span is the normal case and reads as one fact.
  std::vector<std::pair<SharedBinOutcome, std::pair<int, int>>> runs;
  Range separation;
  Range residual;
  Range claimants;
  for (int frame = span.begin; frame < span.end; ++frame) {
    const size_t at = static_cast<size_t>(bin) * stride + static_cast<size_t>(frame);
    const SharedBinOutcome outcome = report.outcome[at];
    if (runs.empty() || runs.back().first != outcome) {
      runs.push_back({outcome, {frame, frame + 1}});
    } else {
      runs.back().second.second = frame + 1;
    }
    separation.add(static_cast<double>(report.partial_separation[at]));
    residual.add(static_cast<double>(report.fit_residual[at]));
    int claiming = 0;
    std::complex<float> weight;
    for (const NoteMask& mask : fitted.notes) {
      if (mask_weight_at(mask, bin, frame, weight)) ++claiming;
    }
    claimants.add(static_cast<double>(claiming));
  }

  std::string verdicts;
  for (const auto& run : runs) {
    verdicts += std::string(verdicts.empty() ? "" : ", ") + outcome_name(run.first) + " over [" +
                std::to_string(run.second.first) + ", " + std::to_string(run.second.second) + ")";
  }
  WARN(what << ": bin " << bin << " (" << hz << " Hz), claimants " << claimants.low << " to "
            << claimants.high << "; " << verdicts);
  WARN("  partial_separation " << separation.low << " to " << separation.high
                               << " (the gate is min_partial_separation "
                               << SharedBinConfig{}.min_partial_separation << "), fit_residual "
                               << residual.low << " to " << residual.high
                               << " (the gate is max_fit_residual "
                               << SharedBinConfig{}.max_fit_residual << ")");

  for (size_t i = 0; i < fitted.notes.size(); ++i) {
    Range before;
    Range after;
    for (int frame = span.begin; frame < span.end; ++frame) {
      std::complex<float> weight;
      if (mask_weight_at(equal.notes[i], bin, frame, weight)) {
        before.add(static_cast<double>(std::abs(weight)));
      }
      if (mask_weight_at(fitted.notes[i], bin, frame, weight)) {
        after.add(static_cast<double>(std::abs(weight)));
      }
    }
    if (before.count == 0 && after.count == 0) continue;
    WARN("  note " << i << " at " << track.ridges[i].median_hz << " Hz holds it in " << after.count
                   << " of " << span.count() << " frames: |w| equal " << before.low << " to "
                   << before.high << ", fitted " << after.low << " to " << after.high);
  }

  // Which note the fit actually handed the bin to, per frame. A min and a max over a
  // span cannot say this: two wide ranges are consistent with the right note winning
  // every frame and with the wrong one doing so.
  if (fitted.notes.size() == 2) {
    int first_larger = 0;
    int frames = 0;
    for (int frame = span.begin; frame < span.end; ++frame) {
      std::complex<float> a;
      std::complex<float> b;
      if (!mask_weight_at(fitted.notes[0], bin, frame, a)) continue;
      if (!mask_weight_at(fitted.notes[1], bin, frame, b)) continue;
      ++frames;
      if (std::abs(a) > std::abs(b)) ++first_larger;
    }
    WARN("  of " << frames << " frames both notes hold, note 0 takes the larger share in "
                 << first_larger << " and note 1 in " << (frames - first_larger));
  }
}

// --- The same two notes, handed over in both orders -------------------------

/// @brief One pair solved twice, the second time with the two notes swapped.
/// @details The swap moves @c masks.notes and @c track.ridges together and restates
///          every @c ridge_index, or the mask and its ridge stop describing each
///          other and the call refuses. Indices are kept per ordering so a caller
///          compares pitch against pitch rather than index against index.
struct TwoOrders {
  sonare::Spectrogram spec;
  MultiF0Track track;
  NoteMaskSet equal;
  NoteMaskSet forward;
  NoteMaskSet other;
  SharedBinReport forward_report;
  SharedBinReport other_report;
  std::vector<float> refined;
  size_t low_forward = 0;
  size_t high_forward = 0;
  size_t low_other = 0;
  size_t high_other = 0;
  bool ran = false;
  sonare::ErrorCode refusal = sonare::ErrorCode::Ok;
};

TwoOrders solve_both_orders(const Pair& pair) {
  const PolyphonicEditConfig config;
  TwoOrders both;
  const sonare::Audio audio = chord_audio(pair);
  both.spec = sonare::Spectrogram::compute(audio, config.extraction.stft);
  both.track = extract_multi_f0(audio, both.spec, config.extraction);
  REQUIRE(both.track.ridges.size() == 2);
  both.equal = build_note_masks(both.spec, both.track, config.masks);
  both.refined = refine_track_f0(both.spec, both.track, both.equal, config.shared_bins);

  MultiF0Track other_track = both.track;
  std::swap(other_track.ridges[0], other_track.ridges[1]);
  NoteMaskSet other_equal = both.equal;
  std::swap(other_equal.notes[0], other_equal.notes[1]);
  for (size_t i = 0; i < other_equal.notes.size(); ++i) {
    other_equal.notes[i].ridge_index = static_cast<int>(i);
  }

  try {
    both.forward = solve_shared_bins(both.spec, both.equal, both.track, config.shared_bins,
                                     &both.forward_report);
    both.other = solve_shared_bins(both.spec, other_equal, other_track, config.shared_bins,
                                   &both.other_report);
    both.ran = true;
  } catch (const sonare::SonareException& error) {
    both.refusal = error.code();
    return both;
  }

  both.low_forward = ridge_nearest(both.track, pair.low_hz);
  both.high_forward = ridge_nearest(both.track, pair.high_hz);
  both.low_other = ridge_nearest(other_track, pair.low_hz);
  both.high_other = ridge_nearest(other_track, pair.high_hz);
  return both;
}

/// @brief Whether the note holding the larger share of @p bin changed with the order.
/// @details Returns false when either order leaves a note without the cell, which is
///          not a flip and not a non-flip.
bool winner_moved(const TwoOrders& both, int bin, int frame, bool& comparable) {
  std::complex<float> low_a;
  std::complex<float> high_a;
  std::complex<float> low_b;
  std::complex<float> high_b;
  comparable = mask_weight_at(both.forward.notes[both.low_forward], bin, frame, low_a) &&
               mask_weight_at(both.forward.notes[both.high_forward], bin, frame, high_a) &&
               mask_weight_at(both.other.notes[both.low_other], bin, frame, low_b) &&
               mask_weight_at(both.other.notes[both.high_other], bin, frame, high_b);
  if (!comparable) return false;
  return (std::abs(low_a) > std::abs(high_a)) != (std::abs(low_b) > std::abs(high_b));
}

}  // namespace

// --- The fixtures the axes rest on -----------------------------------------

TEST_CASE("the two pairs differ in the one way the over-claim measurement turns on",
          "[polyphony_separation]") {
  // Axis 3's precondition, and it is three conditions rather than one. A pair is
  // disjoint only if neither note's partials reach the other's partials and neither
  // reaches the other's claim grid -- the default geometry claims 20 partials while
  // these tones carry 10, so a claim lands where its own note sounds nothing.
  const sonare::Spectrogram spec = spectrogram_of(chord_audio(kFifth));
  const NoteMaskConfig geometry = PolyphonicEditConfig{}.masks;
  REQUIRE(geometry.n_harmonics > kPartials);
  WARN("the masks claim " << geometry.n_harmonics << " partials a note; the tones carry "
                          << kPartials);

  for (const Pair& pair : {kFifth, kDisjoint}) {
    const std::vector<double> low_sounds = sounding_partials(pair.low_hz);
    const std::vector<double> high_sounds = sounding_partials(pair.high_hz);
    const std::vector<double> low_claims = claim_centres(spec, pair.low_hz, geometry);
    const std::vector<double> high_claims = claim_centres(spec, pair.high_hz, geometry);
    const int sounding = closest_bins(low_sounds, high_sounds);
    const int low_against_claims = closest_bins(low_sounds, high_claims);
    const int high_against_claims = closest_bins(high_sounds, low_claims);
    WARN(pair.what << " (" << pair.low_hz << " + " << pair.high_hz
                   << " Hz): closest approach in bins -- partials " << sounding
                   << ", the lower note's partials against the upper's claims "
                   << low_against_claims << ", the upper's against the lower's "
                   << high_against_claims << "; partials sounding " << low_sounds.size() << " and "
                   << high_sounds.size() << ", claims " << low_claims.size() << " and "
                   << high_claims.size());
  }

  // The fifth shares a bin outright, so the cases reading it have something to
  // divide.
  REQUIRE(closest_bins(sounding_partials(kLowHz), sounding_partials(kHighHz)) <= kBandHalfBins);

  // And the disjoint pair clears a whole band either way, in all three senses, so
  // the over-claim figures it produces cannot come from a neighbour in the window.
  const std::vector<double> disjoint_high = sounding_partials(kDisjointHighHz);
  REQUIRE(disjoint_high.size() == static_cast<size_t>(kPartials));
  REQUIRE(closest_bins(sounding_partials(kLowHz), disjoint_high) > 2 * kBandHalfBins);
  REQUIRE(closest_bins(sounding_partials(kLowHz), claim_centres(spec, kDisjointHighHz, geometry)) >
          2 * kBandHalfBins);
  REQUIRE(closest_bins(disjoint_high, claim_centres(spec, kLowHz, geometry)) > 2 * kBandHalfBins);
  // The tracking has to be able to find the upper pitch, or the pair is untestable
  // rather than disjoint.
  REQUIRE(kDisjointHighHz < PolyphonicEditConfig{}.extraction.estimation.salience.f0_max_hz);
}

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

  const std::vector<std::pair<std::string, std::function<void(NoteObject&)>>> edits = {
      {"a gain of -9 dB", [](NoteObject& note) { note.edit.gain_db = -9.0f; }},
      {"a pitch shift of +1 semitone",
       [](NoteObject& note) { note.edit.pitch_shift_semitones = 1.0f; }},
      {"a pitch shift of +4 semitones",
       [](NoteObject& note) { note.edit.pitch_shift_semitones = 4.0f; }},
      {"a mute", [](NoteObject& note) { note.edit.muted = true; }}};

  // Both pairs, because the one bound this file asserts should not depend on
  // whether the masks had a bin to divide.
  for (const Pair& pair : {kFifth, kDisjoint}) {
    const PolyphonicAnalysis analysis = analyze_polyphonic(chord_audio(pair));
    REQUIRE(analysis.notes.size() == 2);
    REQUIRE(analysis.length == static_cast<int>(kSourceSamples));

    const sonare::Audio base = render_polyphonic(analysis);
    REQUIRE(base.size() == kSourceSamples);
    REQUIRE(all_finite(base));
    const double base_peak = peak_of(base);
    REQUIRE(base_peak > 0.0);
    WARN(pair.what << ": the chord renders to peak " << base_peak << " over "
                   << analysis.spectrum.n_frames() << " frames");

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
          const double note_delta =
              static_cast<double>(after_k[i]) - static_cast<double>(before_k[i]);
          moved = std::max(moved, std::abs(render_delta));
          const double gap = std::abs(render_delta - note_delta);
          if (gap > worst) {
            worst = gap;
            at = i;
          }
        }

        WARN("  note " << k << " with " << edit.first << ": additivity gap " << worst
                       << " at sample " << at << " (" << (worst / base_peak)
                       << " of the render peak " << base_peak
                       << "), the edit itself moved the render by " << moved);
        // The movement is the non-vacuity control: an edit that changed nothing
        // satisfies the identity trivially, both sides being zero.
        REQUIRE(moved > 0.0);
        REQUIRE(worst <= kAdditivityRelative * base_peak);
      }
    }
  }
}

// --- (B) Ghost: an under-claiming mask -------------------------------------

TEST_CASE("ghost: what a shifted note leaves behind at its original harmonics",
          "[polyphony_separation]") {
  // A mask that left part of its note in the residual cannot move that part: the
  // residual is carried through unedited, so the residue keeps sounding at the old
  // pitch. Measured as the drop at the original harmonic positions when the note is
  // shifted up, with the rise at the shifted positions reported beside it so a band
  // window too narrow to see anything is visible as both being small.
  //
  // The drop is not the mask's alone. The shifter leaks back into the band it left
  // whatever the mask did, so the same shift on the tone analysed by itself -- one
  // note, nothing competing for a bin -- is reported beside every number as the
  // floor under it. Neither figure attributes anything on its own.
  SECTION("both notes of the sharing pair, one semitone") {
    // Axis 1: the interval is asymmetric, so which note carries the edit is not a
    // relabelling of the same measurement.
    measure_ghost(kFifth, kLowHz, 1.0f);
    measure_ghost(kFifth, kHighHz, 1.0f);
  }

  SECTION("the same pair, four semitones") {
    // Axis 2: a figure measured at one edit size cannot say whether it is the
    // shifter's floor or a function of how far the note went.
    measure_ghost(kFifth, kLowHz, 4.0f);
    measure_ghost(kFifth, kHighHz, 4.0f);
  }

  SECTION("the disjoint pair, one semitone") {
    // Axis 3: with no bin shared there is no other note's partial to hold the old
    // band up, so whatever remains is the shifter's and the residual's.
    measure_ghost(kDisjoint, kLowHz, 1.0f);
    measure_ghost(kDisjoint, kDisjointHighHz, 1.0f);
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
  //
  // The ceilings are per-role and applied inside the measurement; what a section has
  // to add is the spare claim's, which exists on one note of a pair and not the other,
  // so its ceiling would be vacuous if only one note were measured.
  SECTION("the pair that shares bins") {
    BandTally both;
    both += measure_theft(kFifth, kLowHz);
    both += measure_theft(kFifth, kHighHz);
    REQUIRE(both.phantom > 0);
  }

  SECTION("the pair that shares none") {
    // The control the whole measurement rests on: a pair sharing no bin that still
    // shows a loss is not measuring apportionment.
    BandTally both;
    both += measure_theft(kDisjoint, kLowHz);
    both += measure_theft(kDisjoint, kDisjointHighHz);
    // Disjoint in its partials, not in its claims: a claim grid of 20 partials a note
    // reaches over the other note's partials here too.
    REQUIRE(both.phantom > 0);
  }
}

// --- What the apportionment decided where the losses are -------------------

TEST_CASE("the apportionment's own verdict at the bins that lost the most",
          "[polyphony_separation]") {
  // The fit's outcome beside the weight it produced, at four bins chosen for what
  // they hold rather than for what they measured: a bin two notes sound in, a bin
  // one note sounds in while both masks claim it, and a bin nothing contests.
  // Reported and not gated -- which verdict is correct at a bin with one pole and
  // two claimants is the open question this is for.
  const PolyphonicEditConfig config;
  const sonare::Audio audio = chord_audio(kFifth);
  const sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config.extraction.stft);
  const MultiF0Track track = extract_multi_f0(audio, spec, config.extraction);
  const NoteMaskSet equal = build_note_masks(spec, track, config.masks);
  SharedBinReport report;
  const NoteMaskSet fitted = solve_shared_bins(spec, equal, track, config.shared_bins, &report);
  REQUIRE(fitted.notes.size() == 2);

  // The report has to describe the solve the chain ran, or the verdicts below are
  // another call's. The weights are compared exactly: the call is documented as
  // computing each from the spectrum alone, so two runs are one run.
  const PolyphonicAnalysis analysis = analyze_polyphonic(audio);
  REQUIRE(analysis.masks.notes.size() == fitted.notes.size());
  for (size_t i = 0; i < fitted.notes.size(); ++i) {
    INFO("mask " << i);
    REQUIRE(analysis.masks.notes[i].bins == fitted.notes[i].bins);
    REQUIRE(analysis.masks.notes[i].weights == fitted.notes[i].weights);
  }

  const size_t low = note_nearest(analysis.notes, kLowHz);
  const size_t high = note_nearest(analysis.notes, kHighHz);
  const FrameSpan low_span = span_of(analysis.notes[low], spec);
  const FrameSpan high_span = span_of(analysis.notes[high], spec);
  WARN("E4 is note " << low << " over frames [" << low_span.begin << ", " << low_span.end
                     << "), B4 is note " << high << " over [" << high_span.begin << ", "
                     << high_span.end << ")");

  // The two bins the report is for: B4 sounds there and E4's claim grid reaches
  // them from above its own highest sounding partial.
  dump_bin("B4 h8, one pole and two claimants", spec, track, equal, fitted, report,
           harmonic_hz(kHighHz, 8), high_span);
  dump_bin("B4 h10, one pole and two claimants", spec, track, equal, fitted, report,
           harmonic_hz(kHighHz, 10), high_span);
  // Two controls: a bin both notes really sound in, and one neither contests.
  dump_bin("E4 h3 against B4 h2, two poles", spec, track, equal, fitted, report,
           harmonic_hz(kLowHz, 3), high_span);
  dump_bin("B4 h7, uncontested", spec, track, equal, fitted, report, harmonic_hz(kHighHz, 7),
           high_span);
  // And E4's own side of the shared bin, read over E4's span, since a span scalar
  // is reported per span and the two notes' spans need not agree.
  dump_bin("E4 h3 over E4's own span", spec, track, equal, fitted, report, harmonic_hz(kLowHz, 3),
           low_span);
}

// --- Is the division a function of the order the ridges arrived in? ---------

TEST_CASE("whether the apportionment depends on the order the ridges arrived in",
          "[polyphony_separation]") {
  // The assignment walks the claiming notes in claim order and each takes the
  // nearest unused pole, so the first claim chooses first. Claim order is the
  // tracker's output order, which is not a property of the audio -- if the division
  // moves when the two notes are handed over the other way round, it depends on a
  // variable that should not be observable. Reported and not gated: which of the two
  // divisions is right is the open question.
  const PolyphonicEditConfig config;
  const sonare::Audio audio = chord_audio(kFifth);
  const sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config.extraction.stft);
  const MultiF0Track track = extract_multi_f0(audio, spec, config.extraction);
  const NoteMaskSet equal = build_note_masks(spec, track, config.masks);
  REQUIRE(equal.notes.size() == 2);

  // Both sides move together, and every mask's ridge_index is restated, or the mask
  // and its ridge no longer describe each other.
  MultiF0Track other_order_track = track;
  std::swap(other_order_track.ridges[0], other_order_track.ridges[1]);
  NoteMaskSet other_order_equal = equal;
  std::swap(other_order_equal.notes[0], other_order_equal.notes[1]);
  for (size_t i = 0; i < other_order_equal.notes.size(); ++i) {
    other_order_equal.notes[i].ridge_index = static_cast<int>(i);
  }

  SharedBinReport forward_report;
  SharedBinReport other_report;
  NoteMaskSet forward;
  NoteMaskSet other;
  bool both_ran = false;
  try {
    forward = solve_shared_bins(spec, equal, track, config.shared_bins, &forward_report);
    other = solve_shared_bins(spec, other_order_equal, other_order_track, config.shared_bins,
                              &other_report);
    both_ran = true;
  } catch (const sonare::SonareException& error) {
    WARN("the swapped order is refused with code "
         << static_cast<int>(error.code())
         << " -- an order dependence it cannot express is "
            "an order dependence nothing can observe");
  }

  if (both_ran) {
    // Which position each pitch sits at in each ordering, so the comparison is
    // pitch against pitch and not index against index.
    const size_t low_forward = ridge_nearest(track, kLowHz);
    const size_t high_forward = ridge_nearest(track, kHighHz);
    const size_t low_other = ridge_nearest(other_order_track, kLowHz);
    const size_t high_other = ridge_nearest(other_order_track, kHighHz);
    WARN("E4 is note " << low_forward << " then " << low_other << "; B4 is note " << high_forward
                       << " then " << high_other);

    // B4's own frames, read off its ridge rather than off a measured note: the
    // report is indexed by frame and needs no note.
    FrameSpan span;
    span.begin = std::max(0, track.ridges[high_forward].frame_start);
    span.end = std::min(spec.n_frames(), track.ridges[high_forward].frame_end());
    const size_t stride = static_cast<size_t>(spec.n_frames());

    struct Named {
      const char* what;
      double hz;
    };
    for (const Named& target :
         {Named{"B4 h8, one pole and two claimants", harmonic_hz(kHighHz, 8)},
          Named{"B4 h10, one pole and two claimants", harmonic_hz(kHighHz, 10)},
          Named{"E4 h3 against B4 h2, two poles", harmonic_hz(kLowHz, 3)},
          Named{"B4 h7, uncontested", harmonic_hz(kHighHz, 7)}}) {
      const int bin = bin_of(target.hz);
      const size_t at = static_cast<size_t>(bin) * stride + static_cast<size_t>(span.begin);
      const Range e4_forward = bin_share(forward.notes[low_forward], bin, span);
      const Range e4_other = bin_share(other.notes[low_other], bin, span);
      const Range b4_forward = bin_share(forward.notes[high_forward], bin, span);
      const Range b4_other = bin_share(other.notes[high_other], bin, span);
      WARN(target.what << ": bin " << bin << " (" << target.hz << " Hz), outcome "
                       << outcome_name(forward_report.outcome[at]) << " then "
                       << outcome_name(other_report.outcome[at]));
      WARN("  E4 mean |w| " << e4_forward.mean() << " then " << e4_other.mean() << " (range "
                            << e4_forward.low << "-" << e4_forward.high << " then " << e4_other.low
                            << "-" << e4_other.high << " over " << e4_forward.count << " and "
                            << e4_other.count << " frames)");
      WARN("  B4 mean |w| " << b4_forward.mean() << " then " << b4_other.mean() << " (range "
                            << b4_forward.low << "-" << b4_forward.high << " then " << b4_other.low
                            << "-" << b4_other.high << " over " << b4_forward.count << " and "
                            << b4_other.count << " frames)");

      // Frame by frame, because two means can agree while every frame disagrees.
      int moved = 0;
      int held = 0;
      double worst = 0.0;
      for (int frame = span.begin; frame < span.end; ++frame) {
        std::complex<float> a;
        std::complex<float> b;
        if (!mask_weight_at(forward.notes[high_forward], bin, frame, a)) continue;
        if (!mask_weight_at(other.notes[high_other], bin, frame, b)) continue;
        ++held;
        const double gap =
            std::abs(static_cast<double>(std::abs(a)) - static_cast<double>(std::abs(b)));
        worst = std::max(worst, gap);
        if (gap > 0.0) ++moved;
      }
      WARN("  B4's own share differs in " << moved << " of " << held
                                          << " frames, worst |delta |w|| " << worst);

      // Which note won, counted in both orders. Minimising over all permutations
      // cannot let the answer depend on which note claimed first, so a flip here is
      // the whole finding and the means only imply it.
      int e4_wins_forward = 0;
      int e4_wins_other = 0;
      int both = 0;
      for (int frame = span.begin; frame < span.end; ++frame) {
        std::complex<float> e4;
        std::complex<float> b4;
        std::complex<float> e4_swapped;
        std::complex<float> b4_swapped;
        if (!mask_weight_at(forward.notes[low_forward], bin, frame, e4)) continue;
        if (!mask_weight_at(forward.notes[high_forward], bin, frame, b4)) continue;
        if (!mask_weight_at(other.notes[low_other], bin, frame, e4_swapped)) continue;
        if (!mask_weight_at(other.notes[high_other], bin, frame, b4_swapped)) continue;
        ++both;
        if (std::abs(e4) > std::abs(b4)) ++e4_wins_forward;
        if (std::abs(e4_swapped) > std::abs(b4_swapped)) ++e4_wins_other;
      }
      WARN("  of " << both << " frames both notes hold, E4 takes the larger share in "
                   << e4_wins_forward << " with E4 claiming first and " << e4_wins_other
                   << " with B4 claiming first");
    }
  }
}

// --- Is the ambiguity confined to close poles? ------------------------------

TEST_CASE("order dependence against how far apart the two claimed partials are",
          "[polyphony_separation]") {
  // Where the claim order is still visible, read against how far apart the two
  // claimed partials are. The assignment minimises one order-independent total, so
  // the expected answer is nowhere -- and the value of asking by separation is that
  // it says so across the whole range rather than at one interval. The sharing pair's
  // one genuinely shared bin sits at 0.24 rad/frame and cannot be moved, so a second
  // pair carries shared bins at 1.09 and 2.19 with both partials real.
  //
  // The refusal columns are reported and not asserted. They are the instrument for a
  // change that trades a wrong share for a refusal: such a change has to leave the
  // refusal set itself invariant under the same swap, because a refusal reads as
  // conservative and is not re-measured. Nothing refuses here, so the columns print
  // zero, and a zero they print is not a pass they granted.
  const NoteMaskConfig geometry = PolyphonicEditConfig{}.masks;

  for (const Pair& pair : {kFifth, kDetunedFifth}) {
    const TwoOrders both = solve_both_orders(pair);
    if (!both.ran) {
      WARN(pair.what << ": the swapped order is refused with code "
                     << static_cast<int>(both.refusal));
      continue;
    }
    WARN(pair.what << " (" << pair.low_hz << " + " << pair.high_hz << " Hz): tracked "
                   << both.track.ridges[both.low_forward].median_hz << " and "
                   << both.track.ridges[both.high_forward].median_hz << " Hz, refined "
                   << both.refined[both.low_forward] << " and " << both.refined[both.high_forward]
                   << " Hz");

    // Every cell the fit decided with two claimants, bucketed by the separation the
    // gate read and by whether both claims are partials the tones actually carry.
    const std::vector<double> edges = {0.0, 0.1, 0.3, 1.0, 2.0};
    struct Bucket {
      Range separation;
      Range residual_flipped;
      Range residual_held;
      int cells = 0;
      int flipped = 0;
      int ambiguous = 0;
      int ambiguous_one_order_only = 0;
    };
    std::vector<Bucket> real_poles(edges.size());
    std::vector<Bucket> phantom(edges.size());

    const size_t stride = static_cast<size_t>(both.spec.n_frames());
    for (int bin = 0; bin < both.spec.n_bins(); ++bin) {
      const int low_h = claiming_harmonic(both.spec, both.track.ridges[both.low_forward].median_hz,
                                          bin, geometry);
      const int high_h = claiming_harmonic(
          both.spec, both.track.ridges[both.high_forward].median_hz, bin, geometry);
      if (low_h == 0 || high_h == 0) continue;
      const bool both_sound = low_h <= kPartials && high_h <= kPartials;
      for (int frame = 0; frame + 1 < both.spec.n_frames(); ++frame) {
        const size_t at = static_cast<size_t>(bin) * stride + static_cast<size_t>(frame);
        const SharedBinOutcome outcome = both.forward_report.outcome[at];
        const SharedBinOutcome other_outcome = both.other_report.outcome[at];
        const bool refused = outcome == SharedBinOutcome::AssignmentAmbiguous;
        if (outcome != SharedBinOutcome::Solved && !refused) continue;
        bool comparable = false;
        const bool moved = winner_moved(both, bin, frame, comparable);
        if (!comparable) continue;
        const double separation = static_cast<double>(both.forward_report.partial_separation[at]);
        size_t which = 0;
        for (size_t e = 0; e < edges.size(); ++e) {
          if (separation >= edges[e]) which = e;
        }
        Bucket& bucket = both_sound ? real_poles[which] : phantom[which];
        ++bucket.cells;
        bucket.separation.add(separation);
        bucket.residual_held.add(static_cast<double>(both.forward_report.fit_residual[at]));
        if (refused) {
          ++bucket.ambiguous;
          // A refusal firing in one order and not the other has not removed the order
          // dependence, it has relabelled half of it.
          if (other_outcome != SharedBinOutcome::AssignmentAmbiguous) {
            ++bucket.ambiguous_one_order_only;
          }
          continue;
        }
        if (other_outcome == SharedBinOutcome::AssignmentAmbiguous) {
          ++bucket.ambiguous_one_order_only;
        }
        if (moved) {
          ++bucket.flipped;
          bucket.residual_flipped.add(static_cast<double>(both.forward_report.fit_residual[at]));
        }
      }
    }

    for (int kind = 0; kind < 2; ++kind) {
      const std::vector<Bucket>& buckets = kind == 0 ? real_poles : phantom;
      const char* what = kind == 0 ? "both claims are partials the tones carry"
                                   : "one claim is a partial the tone does not carry";
      for (size_t e = 0; e < buckets.size(); ++e) {
        if (buckets[e].cells == 0) continue;
        WARN("  " << what << ", separation from " << edges[e] << " rad/frame: " << buckets[e].cells
                  << " cells, " << buckets[e].ambiguous << " refused as ambiguous ("
                  << buckets[e].ambiguous_one_order_only
                  << " of them in only one of the two orders), " << buckets[e].flipped
                  << " of the rest changed winner with the order; separation "
                  << buckets[e].separation.low << " to " << buckets[e].separation.high
                  << ", fit_residual over all " << buckets[e].residual_held.low << " to "
                  << buckets[e].residual_held.high << ", over the ones that changed "
                  << buckets[e].residual_flipped.low << " to " << buckets[e].residual_flipped.high
                  << " (" << buckets[e].residual_flipped.count << " cells)");
      }
    }

    // And the frame-level question, at the widest genuinely shared bin this pair has:
    // whether the cells whose winner moved are the cells where the bin's own rate sits
    // far from both predictions.
    int widest = -1;
    double widest_separation = -1.0;
    for (int bin = 0; bin < both.spec.n_bins(); ++bin) {
      const int low_h = claiming_harmonic(both.spec, both.track.ridges[both.low_forward].median_hz,
                                          bin, geometry);
      const int high_h = claiming_harmonic(
          both.spec, both.track.ridges[both.high_forward].median_hz, bin, geometry);
      if (low_h == 0 || high_h == 0 || low_h > kPartials || high_h > kPartials) continue;
      // Every frame, not frame 0: a bin the fit reaches only part way through a
      // span would otherwise be invisible to the search, and the bin named below
      // would be the widest of a subset rather than of the pair.
      for (int frame = 0; frame < both.spec.n_frames(); ++frame) {
        const size_t at = static_cast<size_t>(bin) * stride + static_cast<size_t>(frame);
        if (both.forward_report.outcome[at] != SharedBinOutcome::Solved) continue;
        const double separation = static_cast<double>(both.forward_report.partial_separation[at]);
        if (separation > widest_separation) {
          widest_separation = separation;
          widest = bin;
        }
      }
    }
    if (widest < 0) {
      WARN("  no genuinely shared bin this pair solved, so no frame detail");
      continue;
    }

    const int low_h =
        claiming_harmonic(both.spec, both.refined[both.low_forward], widest, geometry);
    const int high_h =
        claiming_harmonic(both.spec, both.refined[both.high_forward], widest, geometry);
    const double low_rate = predicted_rate(static_cast<double>(low_h) *
                                           static_cast<double>(both.refined[both.low_forward]));
    const double high_rate = predicted_rate(static_cast<double>(high_h) *
                                            static_cast<double>(both.refined[both.high_forward]));
    Range moved_from_low;
    Range moved_from_high;
    Range held_from_low;
    Range held_from_high;
    for (int frame = 0; frame + 1 < both.spec.n_frames(); ++frame) {
      const size_t at = static_cast<size_t>(widest) * stride + static_cast<size_t>(frame);
      if (both.forward_report.outcome[at] != SharedBinOutcome::Solved) continue;
      bool comparable = false;
      const bool moved = winner_moved(both, widest, frame, comparable);
      if (!comparable) continue;
      const double rate = observed_rate(both.spec, widest, frame);
      const double from_low = std::abs(wrap_to_pi(rate - low_rate));
      const double from_high = std::abs(wrap_to_pi(rate - high_rate));
      if (moved) {
        moved_from_low.add(from_low);
        moved_from_high.add(from_high);
      } else {
        held_from_low.add(from_low);
        held_from_high.add(from_high);
      }
    }
    WARN("  bin " << widest << " is the widest genuinely shared one, separation "
                  << widest_separation << " rad/frame, predicted rates " << low_rate << " (h"
                  << low_h << " of the lower note) and " << high_rate << " (h" << high_h
                  << " of the upper)");
    WARN("  the bin's own rate sits this far from the lower note's prediction: "
         << moved_from_low.low << " to " << moved_from_low.high << " over " << moved_from_low.count
         << " cells whose winner moved, " << held_from_low.low << " to " << held_from_low.high
         << " over " << held_from_low.count << " that held");
    WARN("  and this far from the upper note's: "
         << moved_from_high.low << " to " << moved_from_high.high << " over "
         << moved_from_high.count << " that moved, " << held_from_high.low << " to "
         << held_from_high.high << " over " << held_from_high.count << " that held");
  }
}
