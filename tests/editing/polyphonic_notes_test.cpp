#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_object.h"
#include "editing/note_model/note_renderer.h"
#include "editing/pitch_editor/f0_provider.h"
#include "editing/polyphony/masked_notes.h"
#include "editing/polyphony/masked_renderer.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "util/constants.h"
#include "util/exception.h"

namespace note_model = sonare::editing::note_model;
namespace pitch_editor = sonare::editing::pitch_editor;

using sonare::editing::note_model::NoteCurve;
using sonare::editing::note_model::NoteExtractorConfig;
using sonare::editing::note_model::NoteObject;
using namespace sonare::editing::polyphony;

namespace {

/// The framing every fixture, every ridge and every span below is reasoned in.
/// Short enough that a case inverting one mask per note stays well inside the
/// default tier, and a hop of 256 makes a frame's sample offset a round number.
constexpr int kSampleRate = 16000;
constexpr int kNfft = 1024;
constexpr int kHopLength = 256;
constexpr size_t kSourceSamples = 8000;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// The two pitches every fixture is built from, a fifth apart so that some
/// partials collide and most do not: a shared bin is where the equal split
/// divides, so the isolated note really differs from the mixture there.
constexpr float kLowHz = 330.0f;
constexpr float kHighHz = 495.0f;

/// @brief How far the render of the returned notes may sit from one inverse of
///        the whole spectrogram, relative to the reconstruction's peak.
/// @details Only the composition case needs a tolerance at all, and the quantity
///          it bounds is the masked renderer's own telescoping error. Measured
///          here: worst 1.78814e-07 against a peak of 0.886, which is 2.02e-07 of
///          it, so this is five times the measurement it bounds.
///
///          Not a quantity a wrong answer could be confused with. The falsifier in
///          the same case -- one note's gain applied through the other's mask --
///          moves the render by 0.434829, five orders over the bound.
constexpr double kTelescopeRelative = 1e-6;

/// @brief How far the measured Hz may move when only the reference pitch moves.
/// @details The one comparison in this file that is not exact, and what makes it
///          the one is that its two sides have different inputs. `median_hz` is
///          `reference_hz * 2^(median_cents/1200)` over a median of
///          `1200 * log2(hz / reference_hz)`, so moving the reference sends the
///          same mathematical value down a different floating-point path: measured
///          at 440 against 100, 495.0 against 494.999938965, a relative 1.23e-07,
///          which is two float ulps of 495.
///
///          The fixture's other ridge came back bit-identical, and that is what
///          says this is the round trip's own rounding rather than a dependence --
///          whether `==` holds is a property of the frequency, and the invariance
///          the contract states is not. A comparison that passes for one pitch and
///          fails for the next is not asserting the contract either way.
///
///          Eight times the worst measured, and seven orders under what a real
///          dependence costs: a `median_hz` carrying the reference would move by
///          the ratio of the two references, 4.4x here, not by 1e-07.
constexpr double kReferenceInvariantRelative = 1e-6;

/// @brief Peak a fixture reconstructs to, below which a comparison is vacuous.
/// @details Asserted next to every relative comparison rather than once in a
///          fixture, so no case can pass on a buffer of zeros.
constexpr double kFixturePeakFloor = 0.2;

/// @brief How far above the tolerance a quantity has to sit to be worth
///        comparing against it.
constexpr double kGuardMargin = 50.0;

/// @brief Per-frame RMS below which an isolated note carries nothing to measure.
/// @details The fixture's tones are at 0.3 and 0.25 linear and the isolated notes
///          measure 0.202032 and 0.259209, so the quieter of them sits four times
///          this while a silent or misaddressed buffer measures zero.
constexpr float kNoteAmplitudeFloor = 0.05f;

/// @brief How much louder the mixture reads than one isolated note.
/// @details The other tone is at a comparable level and sounds throughout, so its
///          energy puts the mixture's RMS over the separated note's: measured
///          1.7106 for the lower ridge and 1.3152 for the upper, so the worse of
///          the two keeps 20% of headroom here.
///
///          Left where it is rather than tightened onto that. It is a structural
///          lower bound and not a tolerance, so it moves with the fixture's level
///          ratio and with how many partials the fifth puts in one bin -- one more
///          collision lowers it, and tightening wants those two measured rather
///          than a second reading of the same two ridges. What catches a mask that
///          degrades is the frame-by-frame comparison in the same case, not this
///          bound.
constexpr float kMixtureMargin = 1.1f;

/// @brief The error code a call throws, or Ok when it does not throw.
template <typename Fn>
sonare::ErrorCode code_of(Fn&& fn) {
  try {
    fn();
  } catch (const sonare::SonareException& error) {
    return error.code();
  }
  return sonare::ErrorCode::Ok;
}

// --- Synthetic material ----------------------------------------------------

/// @brief Adds a steady harmonic tone, partials at 1/h amplitude.
/// @details The fixed per-harmonic phase keeps the partials from summing into an
///          impulse train, so the reconstruction's peak is the tone's own level
///          rather than one sample of it.
void add_tone(std::vector<float>& into, float f0_hz, float amplitude, int n_partials,
              int sample_rate) {
  const double nyquist = 0.5 * static_cast<double>(sample_rate);
  for (int h = 1; h <= n_partials; ++h) {
    const double hz = static_cast<double>(h) * static_cast<double>(f0_hz);
    if (hz >= nyquist) break;
    const double phase = 0.37 * static_cast<double>(h) * static_cast<double>(h);
    const float level = amplitude / static_cast<float>(h);
    for (size_t i = 0; i < into.size(); ++i) {
      into[i] += level * static_cast<float>(
                             std::sin(sonare::constants::kTwoPiD * hz * static_cast<double>(i) /
                                          static_cast<double>(sample_rate) +
                                      phase));
    }
  }
}

sonare::StftConfig stft_config() { return sonare::make_stft_config(kNfft, kHopLength); }

/// @brief Both fixture pitches, held over the whole source.
/// @details Held throughout rather than gated to the ridges' spans, so every
///          frame a ridge spans has signal for its mask to claim and the
///          material outside it is the residual's rather than silence.
sonare::Audio source_audio() {
  std::vector<float> samples(kSourceSamples, 0.0f);
  add_tone(samples, kLowHz, 0.3f, 10, kSampleRate);
  add_tone(samples, kHighHz, 0.25f, 10, kSampleRate);
  return sonare::Audio::from_vector(std::move(samples), kSampleRate);
}

sonare::Spectrogram source_spectrogram() {
  return sonare::Spectrogram::compute(source_audio(), stft_config());
}

// --- Hand-built tracks and masks -------------------------------------------

/// @brief A ridge holding one pitch over [frame_start, frame_start + n_frames).
/// @details The sample span is the frames' own, which is what
///          @ref track_f0_ridges produces: it is handed no length, so a ridge
///          reaching the last frame ends past the audio.
F0Ridge steady_ridge(float f0_hz, int frame_start, int n_frames, int hop_length = kHopLength) {
  F0Ridge ridge;
  ridge.frame_start = frame_start;
  ridge.f0_hz.assign(static_cast<size_t>(n_frames), f0_hz);
  ridge.salience.assign(static_cast<size_t>(n_frames), 1.0f);
  ridge.onset_sample = static_cast<int64_t>(frame_start) * hop_length;
  ridge.offset_sample = static_cast<int64_t>(frame_start + n_frames) * hop_length;
  ridge.median_hz = f0_hz;
  return ridge;
}

/// @brief A ridge alternating between two pitches, frame by frame.
/// @details Two uses: an even frame count makes the median of the Hz values and
///          the median of the cents two different numbers, and a wobble the
///          segmentation threshold can be read against makes the stability
///          figure depend on the config rather than saturating at 1.
F0Ridge alternating_ridge(float low_hz, float high_hz, int frame_start, int n_frames) {
  F0Ridge ridge = steady_ridge(low_hz, frame_start, n_frames);
  for (size_t i = 1; i < ridge.f0_hz.size(); i += 2) ridge.f0_hz[i] = high_hz;
  std::vector<float> sorted = ridge.f0_hz;
  std::sort(sorted.begin(), sorted.end());
  const size_t half = sorted.size() / 2;
  ridge.median_hz =
      sorted.size() % 2 == 1 ? sorted[half] : 0.5f * (sorted[half - 1] + sorted[half]);
  return ridge;
}

MultiF0Track track_over(const sonare::Spectrogram& spec, std::vector<F0Ridge> ridges) {
  MultiF0Track track;
  track.ridges = std::move(ridges);
  track.n_frames = spec.n_frames();
  track.hop_length = spec.hop_length();
  track.sample_rate = spec.sample_rate();
  track.polyphony.assign(static_cast<size_t>(spec.n_frames()), 0);
  for (const F0Ridge& ridge : track.ridges) {
    for (int frame = ridge.frame_start; frame < ridge.frame_end(); ++frame) {
      ++track.polyphony[static_cast<size_t>(frame)];
    }
  }
  return track;
}

/// @brief One note's share of @p spec, inverted at @p length.
/// @details Built out of the note_mask entry points rather than taken from the
///          conversion, so a case reading it is not reading the subject back.
sonare::Audio masked_audio(const sonare::Spectrogram& spec, const NoteMask& mask, int length) {
  return apply_note_mask(spec, mask).to_audio(length);
}

/// @brief The monophonic F0 track one ridge is, over the whole analysis.
/// @details The span handed to @ref note_model::make_note is the ridge's own
///          frames, and the statistics are measured over that span alone, so how
///          the frames outside it are spelled cannot move the result: the zeros
///          here are not a claim about what the conversion writes there.
pitch_editor::F0Track track_for_ridge(const MultiF0Track& track, const F0Ridge& ridge) {
  pitch_editor::F0Track f0;
  f0.f0_hz.assign(static_cast<size_t>(track.n_frames), 0.0f);
  f0.voiced.assign(static_cast<size_t>(track.n_frames), false);
  for (size_t i = 0; i < ridge.f0_hz.size(); ++i) {
    const size_t frame = static_cast<size_t>(ridge.frame_start) + i;
    f0.f0_hz[frame] = ridge.f0_hz[i];
    f0.voiced[frame] = true;
  }
  f0.hop_length = track.hop_length;
  f0.sample_rate = track.sample_rate;
  return f0;
}

/// @brief @ref note_model::make_note over one ridge's own isolated audio.
/// @details The contract's measurement, assembled from the two public functions
///          it names. Every measured field a case below checks is compared
///          against this rather than against a figure written down here.
NoteObject measured_by_make_note(const sonare::Spectrogram& spec, const MultiF0Track& track,
                                 const NoteMaskSet& masks, size_t at, int length,
                                 const NoteExtractorConfig& config = {}) {
  const F0Ridge& ridge = track.ridges[at];
  const sonare::Audio isolated = masked_audio(spec, masks.notes[at], length);
  REQUIRE(!isolated.empty());
  return note_model::make_note(isolated, track_for_ridge(track, ridge), ridge.frame_start,
                               ridge.frame_end(), config);
}

// --- Reading a note --------------------------------------------------------

float median_of(const std::vector<float>& values) {
  REQUIRE(!values.empty());
  std::vector<float> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  const size_t half = sorted.size() / 2;
  return sorted.size() % 2 == 1 ? sorted[half] : 0.5f * (sorted[half - 1] + sorted[half]);
}

void require_same_curve(const NoteCurve& got, const NoteCurve& want, const char* which) {
  INFO(which);
  REQUIRE(got.values.size() == want.values.size());
  REQUIRE(got.frame_rate_hz == want.frame_rate_hz);
  REQUIRE(got.frame_offset == want.frame_offset);
  for (size_t i = 0; i < want.values.size(); ++i) {
    INFO("frame " << i);
    REQUIRE(got.values[i] == want.values[i]);
  }
}

/// @brief Asserts two notes are the same note, field by field.
/// @details Exact comparison, and not a tolerance: the contract is that the
///          fields come from one call of @ref note_model::make_note over one
///          buffer, so the two sides are the same function over the same input
///          and a difference of any size is a different measurement rather than
///          a rounding of the same one. A tolerance here would also pass a
///          conversion that measured over a neighbouring buffer.
///
///          Every field is named rather than compared through a whole-struct
///          equality, so a field added to the note later is visibly unchecked
///          instead of silently absorbed.
void require_same_note(const NoteObject& got, const NoteObject& want, const std::string& what) {
  INFO(what);
  REQUIRE(got.onset_sample == want.onset_sample);
  REQUIRE(got.offset_sample == want.offset_sample);
  REQUIRE(got.frame_start == want.frame_start);
  REQUIRE(got.frame_end == want.frame_end);
  REQUIRE(got.median_hz == want.median_hz);
  REQUIRE(got.median_cents == want.median_cents);
  require_same_curve(got.f0_hz, want.f0_hz, "the pitch curve");
  require_same_curve(got.amplitude, want.amplitude, "the amplitude curve");
  REQUIRE(got.f0_stability == want.f0_stability);
  REQUIRE(got.edit.is_identity());
  REQUIRE(want.edit.is_identity());
}

/// @brief Asserts every returned note is one the edit chain accepts.
/// @details The contract puts each built note through
///          @ref note_model::validate_note_for_render before returning it, so the
///          usable-pitch-curve clause is enforced rather than reasoned. Asserted
///          next to every accepted call rather than in one case of its own,
///          because the failure it guards is a note that comes back looking
///          measured and is refused by the next call -- which no rejection case
///          can see, and which a rejection list only covers for the paths already
///          known to produce one.
void require_usable_notes(const std::vector<NoteObject>& notes) {
  for (size_t i = 0; i < notes.size(); ++i) {
    INFO("note " << i);
    REQUIRE(notes[i].edit.is_identity());
    REQUIRE_NOTHROW(note_model::validate_note_for_render(notes[i]));
    REQUIRE(notes[i].onset_sample < notes[i].offset_sample);
    // A vibrato or drift edit is the one thing a note carrying no usable curve is
    // refused for, so the clause is asserted with one applied and not only on the
    // identity the call hands back.
    NoteObject wobbled = notes[i];
    wobbled.edit.vibrato_depth_change = 0.5f;
    REQUIRE_NOTHROW(note_model::validate_note_for_render(wobbled));
    NoteObject drifted = notes[i];
    drifted.edit.drift_change = -0.5f;
    REQUIRE_NOTHROW(note_model::validate_note_for_render(drifted));
    REQUIRE(notes[i].median_hz > 0.0f);
  }
}

/// @brief Asserts every note of @p got is the measurement the contract names.
void require_measured_by_make_note(const sonare::Spectrogram& spec, const MultiF0Track& track,
                                   const NoteMaskSet& masks, int length,
                                   const NoteExtractorConfig& config = {}) {
  const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length, config);
  REQUIRE(got.size() == track.ridges.size());
  REQUIRE(!got.empty());
  require_usable_notes(got);
  for (size_t i = 0; i < got.size(); ++i) {
    const NoteObject want = measured_by_make_note(spec, track, masks, i, length, config);
    // The oracle measured a note and not an empty window: a curve of zeros
    // agrees with a conversion that measured the wrong buffer.
    REQUIRE(median_of(want.amplitude.values) > kNoteAmplitudeFloor);
    REQUIRE(want.median_hz > 0.0f);
    require_same_note(got[i], want,
                      "ridge " + std::to_string(i) + " at length " + std::to_string(length));
  }
}

// --- Reading a buffer ------------------------------------------------------

struct Agreement {
  double worst = 0.0;
  double scale = 0.0;
  size_t at = 0;
};

Agreement agreement(const sonare::Audio& got, const sonare::Audio& want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(!want.empty());
  Agreement result;
  for (size_t i = 0; i < want.size(); ++i) {
    const double difference = std::abs(static_cast<double>(got[i]) - static_cast<double>(want[i]));
    if (difference > result.worst) {
      result.worst = difference;
      result.at = i;
    }
    result.scale = std::max(result.scale, std::abs(static_cast<double>(want[i])));
  }
  return result;
}

/// @brief Asserts the masked render of @p notes is the spectrogram's own round
///        trip, with the non-vacuity guard at the assertion.
void require_telescopes(const sonare::Audio& got, const sonare::Audio& want, const char* what) {
  REQUIRE(got.size() == want.size());
  REQUIRE(got.sample_rate() == want.sample_rate());
  const Agreement how = agreement(got, want);
  INFO(what << ": worst |difference| " << how.worst << " at sample " << how.at << ", peak "
            << how.scale << ", ratio " << (how.worst / how.scale));
  REQUIRE(how.scale > kFixturePeakFloor);
  REQUIRE(how.worst <= kTelescopeRelative * how.scale);
}

// --- Shared rejection inputs ----------------------------------------------

struct NamedSetBreak {
  std::string what;
  std::function<void(MultiF0Track&, NoteMaskSet&)> apply;
};

/// @brief Every way a set stops describing the spectrogram or its track.
/// @details One list, so a case running it against a set of several masks and a
///          case running it against a set of one cannot diverge. Each break is a
///          mutation of the set alone -- the track is handed over so a break that
///          has to move both can, and the framing breaks below do.
std::vector<NamedSetBreak> set_breaks(const sonare::Spectrogram& spec) {
  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  std::vector<NamedSetBreak> broken;
  // The set has to describe the spectrogram, and n_bins is the one field of that
  // shape the track carries no counterpart for.
  for (const int bad : {bins + 1, bins - 1, 0, -1}) {
    broken.push_back({"the set's n_bins " + std::to_string(bad),
                      [bad](MultiF0Track&, NoteMaskSet& set) { set.n_bins = bad; }});
  }
  for (const int bad : {frames + 1, frames - 1, 0, -1}) {
    broken.push_back({"the set's n_frames " + std::to_string(bad),
                      [bad](MultiF0Track&, NoteMaskSet& set) { set.n_frames = bad; }});
  }
  for (const int bad : {kHopLength + 1, kHopLength / 2, 0, -1}) {
    broken.push_back({"the set's hop_length " + std::to_string(bad),
                      [bad](MultiF0Track&, NoteMaskSet& set) { set.hop_length = bad; }});
  }
  for (const int bad : {kSampleRate + 1, 44100, 0, -1}) {
    broken.push_back({"the set's sample_rate " + std::to_string(bad),
                      [bad](MultiF0Track&, NoteMaskSet& set) { set.sample_rate = bad; }});
  }
  // The framing the measurement runs in, which make_note reads through the
  // track: a cadence that is not positive is not a measurable framing, and the
  // set carries the same two fields so both sides are moved together.
  for (const int bad : {0, -1}) {
    broken.push_back({"a track hop_length of " + std::to_string(bad),
                      [bad](MultiF0Track& track, NoteMaskSet& set) {
                        track.hop_length = bad;
                        set.hop_length = bad;
                      }});
    broken.push_back({"a track sample_rate of " + std::to_string(bad),
                      [bad](MultiF0Track& track, NoteMaskSet& set) {
                        track.sample_rate = bad;
                        set.sample_rate = bad;
                      }});
  }
  return broken;
}

struct NamedConfig {
  std::string what;
  NoteExtractorConfig config;
};

/// @brief Configs whose verdict the contract delegates to the oracle.
/// @details For everything but the reference pitch the contract defers to
///          @ref note_model::make_note rather than restating a range, so the case
///          reading this asserts the two verdicts agree instead of asserting
///          which entries are refused. A conversion stricter than its own oracle
///          is as wrong as one more permissive, which is why the negative
///          @c min_note_ms entries are here: the field belongs to a segmenter
///          this chain never consults, and the oracle checks nothing about it but
///          finiteness.
std::vector<NamedConfig> delegated_configs() {
  std::vector<NamedConfig> list;
  const std::vector<std::pair<const char*, float>> unusable = {
      {"nan", kNaN}, {"inf", kInf}, {"-inf", -kInf}};
  for (const auto& bad : unusable) {
    NoteExtractorConfig config;
    config.voiced_threshold = bad.second;
    list.push_back({std::string("a voiced threshold of ") + bad.first, config});
  }
  for (const auto& bad : unusable) {
    NoteExtractorConfig config;
    config.segmenter.segmentation_threshold_cents = bad.second;
    list.push_back({std::string("a segmentation threshold of ") + bad.first, config});
  }
  for (const auto& bad : unusable) {
    NoteExtractorConfig config;
    config.segmenter.min_note_ms = bad.second;
    list.push_back({std::string("a minimum note length of ") + bad.first, config});
  }
  for (const float bad : {-1.0f, -30.0f, 0.0f}) {
    NoteExtractorConfig config;
    config.segmenter.min_note_ms = bad;
    list.push_back({"a minimum note length of " + std::to_string(bad), config});
  }
  // Finite and not positive, which the contract does not refuse even though the
  // field is one of the two it reads: the stability figure is 0 where the
  // threshold cannot divide. Here so a conversion stricter than its own oracle
  // about a field it does read is visible.
  for (const float bad : {0.0f, -1.0f, -50.0f}) {
    NoteExtractorConfig config;
    config.segmenter.segmentation_threshold_cents = bad;
    list.push_back({"a segmentation threshold of " + std::to_string(bad), config});
  }
  return list;
}

/// @brief Reference pitches this call refuses on its own account.
/// @details A @c reference_hz of 0 makes every frame's cents 0 and the note's
///          median_hz 0, which is how a track spells no pitch, so the note comes
///          back unable to take the curve edit the contract promises it can. The
///          oracle accepts it, so this is the one config ground the conversion
///          holds that make_note does not.
std::vector<float> refused_reference_pitches() { return {0.0f, -1.0f, -440.0f}; }

}  // namespace

// --- The measurement -------------------------------------------------------

TEST_CASE("every measured field is make_note's over that note's own isolated audio",
          "[polyphony_notes]") {
  // The contract's core claim, and the only case that can tell a conversion
  // spelling the monophonic definitions again from one calling them: the fields
  // are compared against make_note over the audio apply_note_mask produces, so
  // nothing here states what an RMS window or a cents median is.
  const sonare::Spectrogram spec = source_spectrogram();
  REQUIRE(spec.n_frames() > 28);
  const int natural = static_cast<int>(spec.to_audio(0).size());
  REQUIRE(natural > 0);
  // Two distinct lengths to measure at, which is also what says the parameter
  // reaches the inverse the amplitude is measured over.
  REQUIRE(natural != static_cast<int>(kSourceSamples));

  SECTION("two notes a fifth apart, so some bins are shared and most are not") {
    const MultiF0Track track =
        track_over(spec, {steady_ridge(kLowHz, 0, 28), steady_ridge(kHighHz, 4, 20)});
    const NoteMaskSet masks = build_note_masks(spec, track);
    REQUIRE(masks.notes.size() == 2);

    // The shared bins are real, or the section only exercises a whole claim and
    // the isolated audio is the mixture's own partials unchanged.
    size_t shared = 0;
    for (const NoteMask& mask : masks.notes) {
      for (const std::complex<float>& weight : mask.weights) {
        if (weight != 1.0f) ++shared;
      }
    }
    REQUIRE(shared > 0);

    for (const int length : {0, natural, static_cast<int>(kSourceSamples)}) {
      require_measured_by_make_note(spec, track, masks, length);
    }
  }

  SECTION("three notes at one pitch, so every share is a third") {
    // A third is not exact in binary, so the isolated audio carries the
    // partition's own rounding and an amplitude measured over the mixture
    // instead cannot coincide with it.
    const MultiF0Track track = track_over(
        spec,
        {steady_ridge(kLowHz, 0, 28), steady_ridge(kLowHz, 0, 28), steady_ridge(kLowHz, 0, 28)});
    const NoteMaskSet masks = build_note_masks(spec, track);
    REQUIRE(masks.notes.size() == 3);
    for (const NoteMask& mask : masks.notes) {
      REQUIRE(!mask.weights.empty());
      for (const std::complex<float>& weight : mask.weights) REQUIRE(weight != 1.0f);
    }

    require_measured_by_make_note(spec, track, masks, static_cast<int>(kSourceSamples));
  }

  SECTION("a ridge that moves, so the measured pitch is not its one value") {
    const MultiF0Track track =
        track_over(spec, {alternating_ridge(kLowHz, 350.0f, 4, 20), steady_ridge(kHighHz, 2, 24)});
    const NoteMaskSet masks = build_note_masks(spec, track);
    require_measured_by_make_note(spec, track, masks, static_cast<int>(kSourceSamples));
  }

  SECTION("the measurement is not an artefact of this framing") {
    const sonare::StftConfig wide = polyphony_stft_defaults();
    constexpr int kWideRate = 44100;
    std::vector<float> samples(static_cast<size_t>(kWideRate / 4), 0.0f);
    add_tone(samples, 220.0f, 0.3f, 12, kWideRate);
    add_tone(samples, 330.0f, 0.25f, 12, kWideRate);
    const sonare::Spectrogram other = sonare::Spectrogram::compute(
        sonare::Audio::from_vector(std::move(samples), kWideRate), wide);
    REQUIRE(other.n_frames() > 8);

    const int hop = other.hop_length();
    const MultiF0Track track =
        track_over(other, {steady_ridge(220.0f, 0, other.n_frames() - 2, hop),
                           steady_ridge(330.0f, 2, other.n_frames() - 6, hop)});
    const NoteMaskSet masks = build_note_masks(other, track);
    require_measured_by_make_note(other, track, masks, kWideRate / 4);
  }
}

TEST_CASE("the amplitude is the separated note's and not the mixture's", "[polyphony_notes]") {
  // The field the masks exist for. Measured over the mixture it would report
  // every voice sounding in the note's frames, and the two differ by the other
  // tone's own energy -- which is far more than the partition's rounding.
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 2, 24), steady_ridge(kHighHz, 6, 18)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length);
  require_usable_notes(got);
  REQUIRE(got.size() == 2);

  const sonare::Audio mixture = spec.to_audio(length);
  REQUIRE(!mixture.empty());

  for (size_t i = 0; i < got.size(); ++i) {
    INFO("ridge " << i);
    const F0Ridge& ridge = track.ridges[i];
    // The same function over the same frames, differing only in which audio it
    // reads, so the comparison isolates the buffer and not the span or the
    // statistic.
    const NoteObject over_mixture = note_model::make_note(mixture, track_for_ridge(track, ridge),
                                                          ridge.frame_start, ridge.frame_end());
    REQUIRE(over_mixture.amplitude.values.size() == got[i].amplitude.values.size());
    // The pitch fields are read off the curve alone, so they agree whichever
    // audio was measured -- which is what says the difference below is the
    // amplitude rather than two unrelated notes.
    REQUIRE(got[i].median_hz == over_mixture.median_hz);
    REQUIRE(got[i].f0_stability == over_mixture.f0_stability);

    const float isolated_level = median_of(got[i].amplitude.values);
    const float mixed_level = median_of(over_mixture.amplitude.values);
    INFO("isolated " << isolated_level << ", over the mixture " << mixed_level);
    REQUIRE(isolated_level > kNoteAmplitudeFloor);
    REQUIRE(mixed_level > kMixtureMargin * isolated_level);
    // Frame by frame as well, so a separation that only holds at the median is
    // not read as one that holds over the span.
    for (size_t f = 0; f < got[i].amplitude.values.size(); ++f) {
      INFO("frame " << f);
      REQUIRE(std::isfinite(got[i].amplitude.values[f]));
      REQUIRE(got[i].amplitude.values[f] < over_mixture.amplitude.values[f]);
    }
  }
}

// --- The pairing -----------------------------------------------------------

TEST_CASE("one note per ridge, in the track's order", "[polyphony_notes]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  // Three distinct pitches at three distinct spans, so every pairing other than
  // the identity is visible in the curves.
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 0, 28), steady_ridge(kHighHz, 4, 20),
                        steady_ridge(2.0f * kLowHz, 8, 14)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE(masks.notes.size() == track.ridges.size());

  const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length);
  require_usable_notes(got);
  REQUIRE(got.size() == track.ridges.size());

  for (size_t i = 0; i < got.size(); ++i) {
    INFO("ridge " << i);
    // notes[i] is the note of masks.notes[i], and masks.notes[i].ridge_index is
    // i, so the three indices are one index.
    REQUIRE(masks.notes[i].ridge_index == static_cast<int>(i));
    REQUIRE(got[i].frame_start == track.ridges[i].frame_start);
    REQUIRE(got[i].frame_end == track.ridges[i].frame_end());
    // The F0 curve is the ridge's values unchanged, which is the one field the
    // contract says carries over directly.
    REQUIRE(got[i].f0_hz.values == track.ridges[i].f0_hz);
    REQUIRE(got[i].f0_hz.frame_offset == track.ridges[i].frame_start);
    REQUIRE(got[i].amplitude.frame_offset == track.ridges[i].frame_start);
  }

  // Distinct enough that a swapped pair would show in the pitch curves above.
  REQUIRE(got[0].median_hz != got[1].median_hz);
  REQUIRE(got[1].median_hz != got[2].median_hz);
}

TEST_CASE("the notes feed the masked renderer without being reordered", "[polyphony_notes]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 0, 28), steady_ridge(kHighHz, 6, 18)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length);
  require_usable_notes(got);
  REQUIRE(got.size() == 2);

  // Every edit is identity, so the render is the spectrogram's own round trip.
  // The same length is passed to both calls, which the contract requires: a
  // conversion and a render disagreeing about it disagree about where a note is.
  require_telescopes(render_masked_notes(spec, masks, got, length), spec.to_audio(length),
                     "the returned notes");

  // And the order means something. With every edit identity nothing is
  // resynthesized and any pairing telescopes, so the falsifier needs an edit:
  // one gain applied through the wrong mask lands on the wrong material.
  std::vector<NoteObject> forward = got;
  forward[0].edit.gain_db = -9.0f;
  std::vector<NoteObject> swapped = forward;
  std::swap(swapped[0], swapped[1]);

  const sonare::Audio right = render_masked_notes(spec, masks, forward, length);
  const sonare::Audio wrong = render_masked_notes(spec, masks, swapped, length);
  const Agreement how = agreement(wrong, right);
  INFO("the swapped pairing moves the render by " << how.worst << " at sample " << how.at
                                                  << ", peak " << how.scale);
  REQUIRE(how.scale > kFixturePeakFloor);
  REQUIRE(how.worst > kGuardMargin * kTelescopeRelative * how.scale);
}

// --- What the returned note can take --------------------------------------

TEST_CASE("every returned note takes a vibrato or drift edit", "[polyphony_notes]") {
  // The one state validate_note_for_render rejects a note for being unable to
  // take, and the contract says a converted note cannot be in it.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 0, 28), alternating_ridge(kHighHz, 520.0f, 4, 20)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length);
  require_usable_notes(got);
  REQUIRE(got.size() == 2);

  std::vector<NoteObject> wobbled = got;
  std::vector<NoteObject> drifted = got;
  for (size_t i = 0; i < got.size(); ++i) {
    INFO("ridge " << i);
    REQUIRE(got[i].edit.is_identity());
    REQUIRE_NOTHROW(note_model::validate_note_for_render(got[i]));
    REQUIRE(got[i].median_hz > 0.0f);

    wobbled[i].edit.vibrato_depth_change = 0.5f;
    drifted[i].edit.drift_change = -0.5f;
    REQUIRE_NOTHROW(note_model::validate_note_for_render(wobbled[i]));
    REQUIRE_NOTHROW(note_model::validate_note_for_render(drifted[i]));
  }

  // The validator really refuses a note with no usable curve, so the passes
  // above are a property of these notes rather than of a check that accepts
  // everything.
  NoteObject stripped = got[0];
  stripped.f0_hz.values.clear();
  stripped.edit.vibrato_depth_change = 0.5f;
  REQUIRE(code_of([&] { note_model::validate_note_for_render(stripped); }) == kInvalid);
  NoteObject unvoiced = got[0];
  unvoiced.f0_hz.values.assign(unvoiced.f0_hz.values.size(), 0.0f);
  unvoiced.median_hz = 0.0f;
  unvoiced.edit.drift_change = 0.5f;
  REQUIRE(code_of([&] { note_model::validate_note_for_render(unvoiced); }) == kInvalid);

  // And the edit applies through the chain it was converted for, which is the
  // whole point of the curve being usable.
  REQUIRE_NOTHROW(render_masked_notes(spec, masks, wobbled, length));
  REQUIRE_NOTHROW(render_masked_notes(spec, masks, drifted, length));
}

// --- What is derived rather than copied ------------------------------------

TEST_CASE("the spans are derived against length and not copied from the ridge",
          "[polyphony_notes]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const int natural = static_cast<int>(spec.to_audio(0).size());
  // A ridge reaching the last frame, which centre padding makes the normal case:
  // its own span ends at frame_end * hop, past the audio the frames describe.
  const MultiF0Track track = track_over(spec, {steady_ridge(kLowHz, 0, spec.n_frames())});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const int64_t ridge_offset = track.ridges[0].offset_sample;
  REQUIRE(ridge_offset == static_cast<int64_t>(spec.n_frames()) * kHopLength);
  // The last length is four hops under the ridge's own end whatever this
  // framing's natural length turns out to be, so the clamp is reached by
  // construction rather than by the trim happening to fall short.
  const int shorter = static_cast<int>(ridge_offset) - 4 * kHopLength;
  REQUIRE(shorter > 0);

  size_t clamped = 0;
  for (const int length : {0, natural, static_cast<int>(kSourceSamples), shorter}) {
    INFO("length " << length);
    const int reach = length == 0 ? natural : length;
    const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length);
    require_usable_notes(got);
    REQUIRE(got.size() == 1);
    // Sliceable over the audio it will be rendered against, at every length.
    REQUIRE(got[0].onset_sample >= 0);
    REQUIRE(got[0].onset_sample < got[0].offset_sample);
    REQUIRE(got[0].offset_sample <= static_cast<int64_t>(reach));
    // Where the ridge's own offset is outside that audio, a copy is visible.
    if (ridge_offset > static_cast<int64_t>(reach)) {
      REQUIRE(got[0].offset_sample < ridge_offset);
      ++clamped;
    }
    // The frames are still the ridge's: it is the sample span that is clamped.
    REQUIRE(got[0].frame_start == track.ridges[0].frame_start);
    REQUIRE(got[0].frame_end == track.ridges[0].frame_end());
    require_same_note(got[0], measured_by_make_note(spec, track, masks, 0, length),
                      "the clamped span at length " + std::to_string(length));
  }
  // At least one length really asked for the clamp, or the loop asserted the
  // derivation against lengths the ridge already fitted in.
  REQUIRE(clamped > 0);

  // Stated the other way round: the ridge's sample span is not read at all, so
  // moving it moves nothing.
  const int length = static_cast<int>(kSourceSamples);
  const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length);
  require_usable_notes(got);
  MultiF0Track moved = track;
  moved.ridges[0].onset_sample = 0;
  moved.ridges[0].offset_sample = 1;
  const std::vector<NoteObject> from_moved = make_masked_notes(spec, moved, masks, length);
  require_usable_notes(from_moved);
  REQUIRE(from_moved.size() == 1);
  require_same_note(from_moved[0], got[0], "a ridge whose own span was moved");
}

TEST_CASE("the measured pitch is not the ridge's median", "[polyphony_notes]") {
  // A ridge's median_hz is the median of its Hz values; a note's is the median of
  // the span's cents converted back. Over an odd frame count the two coincide,
  // the conversion being monotone, so this is an even count over two pitches an
  // octave apart: the arithmetic middle of 300 and 600 is 450 and the geometric
  // one is 424.3.
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track = track_over(spec, {alternating_ridge(300.0f, 600.0f, 4, 20)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE(track.ridges[0].f0_hz.size() % 2 == 0);
  REQUIRE(track.ridges[0].median_hz == 450.0f);

  const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length);
  require_usable_notes(got);
  REQUIRE(got.size() == 1);
  const NoteObject want = measured_by_make_note(spec, track, masks, 0, length);
  // The two numbers are far apart, so a copy cannot be read as a measurement
  // that happened to agree.
  INFO("the ridge's median " << track.ridges[0].median_hz << ", the note's " << want.median_hz);
  REQUIRE(std::abs(want.median_hz - track.ridges[0].median_hz) > 10.0f);
  REQUIRE(got[0].median_hz == want.median_hz);
  REQUIRE(got[0].median_hz != track.ridges[0].median_hz);

  // And the ridge's own field is read for nothing, so a wrong one changes no
  // part of the note.
  MultiF0Track lying = track;
  lying.ridges[0].median_hz = 1.0f;
  const std::vector<NoteObject> from_lying = make_masked_notes(spec, lying, masks, length);
  require_usable_notes(from_lying);
  REQUIRE(from_lying.size() == 1);
  require_same_note(from_lying[0], got[0], "a ridge carrying a wrong median");
}

TEST_CASE("only the two segmenter fields are read and the segmenter is not consulted",
          "[polyphony_notes]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  // A ridge that moves, so the stability figure is not saturated and the
  // threshold below has something to change.
  const MultiF0Track track =
      track_over(spec, {alternating_ridge(kLowHz, 350.0f, 4, 20), steady_ridge(kHighHz, 2, 24)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> base = make_masked_notes(spec, track, masks, length);
  require_usable_notes(base);
  REQUIRE(base.size() == 2);

  SECTION("a minimum note length no ridge here would survive changes nothing") {
    // 100 s against spans of 320 and 384 ms, so a segmenter consulted with this
    // would emit no note at all.
    NoteExtractorConfig config;
    config.segmenter.min_note_ms = 100000.0f;
    const double span_ms = 1000.0 * 20.0 * kHopLength / kSampleRate;
    REQUIRE(span_ms < config.segmenter.min_note_ms);

    const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length, config);
    require_usable_notes(got);
    REQUIRE(got.size() == base.size());
    for (size_t i = 0; i < got.size(); ++i) {
      require_same_note(got[i], base[i], "ridge " + std::to_string(i) + " under a long minimum");
    }
  }

  SECTION("the reference pitch moves the cents and not the measured Hz") {
    constexpr float kOtherReference = 100.0f;
    NoteExtractorConfig config;
    config.segmenter.reference_hz = kOtherReference;
    const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length, config);
    require_usable_notes(got);
    REQUIRE(got.size() == base.size());
    // Moving the reference adds one constant to every frame's cents, so it
    // survives the median and the expected shift is known rather than bounded:
    // 1200 * log2(440 / 100) is 2565.4 cents.
    const double expected_shift = static_cast<double>(sonare::constants::kCentsPerOctave) *
                                  std::log2(static_cast<double>(sonare::constants::kA4Hz) /
                                            static_cast<double>(kOtherReference));
    for (size_t i = 0; i < got.size(); ++i) {
      const double shift =
          static_cast<double>(got[i].median_cents) - static_cast<double>(base[i].median_cents);
      const double moved_hz =
          std::abs(static_cast<double>(got[i].median_hz) - static_cast<double>(base[i].median_hz));
      INFO("ridge " << i << ": cents " << base[i].median_cents << " against " << got[i].median_cents
                    << ", shift " << shift << " against " << expected_shift << "; Hz "
                    << base[i].median_hz << " against " << got[i].median_hz << ", moved "
                    << moved_hz << " relative " << (moved_hz / base[i].median_hz));
      // The field is read, and by the stated formula: the cents move by that one
      // offset and not merely by something. A tenth of a cent is three hundred
      // times the float rounding of a 2565-cent quantity and far under any other
      // reading of the field.
      REQUIRE(std::abs(shift - expected_shift) <= 0.1);
      // And the Hz do not move, the conversion back using the same reference.
      // Inexact, alone in this file, for the reason its constant carries: the two
      // sides have different inputs, so this is an invariance across a changed
      // input rather than one function over one buffer, and it carries the
      // arithmetic between them.
      REQUIRE(base[i].median_hz > 0.0f);
      REQUIRE(moved_hz <= kReferenceInvariantRelative * static_cast<double>(base[i].median_hz));
    }
    require_measured_by_make_note(spec, track, masks, length, config);
  }

  SECTION("the segmentation threshold moves the stability figure") {
    NoteExtractorConfig config;
    config.segmenter.segmentation_threshold_cents = 400.0f;
    const std::vector<NoteObject> got = make_masked_notes(spec, track, masks, length, config);
    require_usable_notes(got);
    REQUIRE(got.size() == base.size());
    // The moving ridge is the one that can show it; the steady one is at the
    // figure's ceiling whatever the threshold.
    INFO("stability " << base[0].f0_stability << " against " << got[0].f0_stability);
    REQUIRE(std::abs(got[0].f0_stability - base[0].f0_stability) > 0.1f);
    require_measured_by_make_note(spec, track, masks, length, config);
  }
}

// --- Rejections ------------------------------------------------------------

TEST_CASE("a set that does not describe the track is rejected", "[polyphony_notes]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track many =
      track_over(spec, {steady_ridge(kLowHz, 2, 20), steady_ridge(kHighHz, 6, 16)});
  const NoteMaskSet many_masks = build_note_masks(spec, many);
  const MultiF0Track one = track_over(spec, {steady_ridge(kLowHz, 2, 20)});
  const NoteMaskSet one_mask = build_note_masks(spec, one);
  const std::vector<NoteObject> many_notes = make_masked_notes(spec, many, many_masks, length);
  const std::vector<NoteObject> one_note = make_masked_notes(spec, one, one_mask, length);
  require_usable_notes(many_notes);
  require_usable_notes(one_note);

  // The same list against a set of two masks and against a set of one, so a
  // check that only runs once it has a second mask to compare fails the second.
  for (const NamedSetBreak& entry : set_breaks(spec)) {
    INFO(entry.what);
    MultiF0Track track = many;
    NoteMaskSet masks = many_masks;
    entry.apply(track, masks);
    REQUIRE(code_of([&] { return make_masked_notes(spec, track, masks, length); }) == kInvalid);
    // And the renderer refuses the same set. The pair has to agree on what a
    // describable set is, or a set this call accepts reaches a call that rejects
    // it and the notes cannot be passed on after all -- which is the composition
    // contract rather than a second opinion about the break.
    REQUIRE(code_of([&] { return render_masked_notes(spec, masks, many_notes, length); }) ==
            kInvalid);

    MultiF0Track single = one;
    NoteMaskSet single_mask = one_mask;
    entry.apply(single, single_mask);
    REQUIRE(code_of([&] { return make_masked_notes(spec, single, single_mask, length); }) ==
            kInvalid);
    REQUIRE(code_of([&] { return render_masked_notes(spec, single_mask, one_note, length); }) ==
            kInvalid);
  }

  // A set carrying no shape at all, which is the default-constructed one.
  const NoteMaskSet unset;
  REQUIRE(unset.n_frames == 0);
  REQUIRE(code_of([&] { return make_masked_notes(spec, many, unset, length); }) == kInvalid);
}

TEST_CASE("a mask count that is not the ridge count is rejected", "[polyphony_notes]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const std::vector<F0Ridge> ridges = {steady_ridge(kLowHz, 2, 20), steady_ridge(kHighHz, 6, 16),
                                       steady_ridge(2.0f * kLowHz, 10, 12)};

  // Both directions, because a check written as one inequality passes half of
  // them: masks for fewer ridges than the track has, and for more.
  for (const size_t masked : {size_t{0}, size_t{1}, size_t{3}}) {
    for (const size_t tracked : {size_t{0}, size_t{1}, size_t{2}, size_t{3}}) {
      if (masked == tracked) continue;
      INFO(masked << " masks and " << tracked << " ridges");
      const MultiF0Track for_masks = track_over(
          spec,
          std::vector<F0Ridge>(ridges.begin(), ridges.begin() + static_cast<ptrdiff_t>(masked)));
      const MultiF0Track track = track_over(
          spec,
          std::vector<F0Ridge>(ridges.begin(), ridges.begin() + static_cast<ptrdiff_t>(tracked)));
      const NoteMaskSet masks = build_note_masks(spec, for_masks);
      REQUIRE(masks.notes.size() == masked);
      REQUIRE(code_of([&] { return make_masked_notes(spec, track, masks, length); }) == kInvalid);
    }
  }
}

TEST_CASE("a mask whose frames are not its ridge's is rejected", "[polyphony_notes]") {
  // The amplitude is measured over the frames the mask claims and the pitch
  // curve describes the frames the ridge holds, so a disagreement measures one
  // note's level against another's pitch.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const std::vector<F0Ridge> ridges = {steady_ridge(kLowHz, 4, 16), steady_ridge(kHighHz, 8, 12)};
  const MultiF0Track track = track_over(spec, ridges);
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE_NOTHROW(make_masked_notes(spec, track, masks, length));

  // Each break at the first mask and at the last, so a check reading one of them
  // is not covered by the other.
  for (const size_t at : {size_t{0}, size_t{1}}) {
    for (const int shift : {1, -1, 4}) {
      INFO("mask " << at << " moved by " << shift << " frames");
      NoteMaskSet shifted = masks;
      shifted.notes[at].frame_start += shift;
      REQUIRE(shifted.notes[at].frame_start >= 0);
      REQUIRE(shifted.notes[at].frame_end() <= spec.n_frames());
      REQUIRE(code_of([&] { return make_masked_notes(spec, track, shifted, length); }) == kInvalid);
    }

    // A frame count that disagrees, built by masking a different set of ridges
    // so each mask stays internally well formed.
    for (const int delta : {-3, 3}) {
      INFO("mask " << at << " spanning " << delta << " frames more than its ridge");
      std::vector<F0Ridge> other = ridges;
      other[at] = steady_ridge(other[at].f0_hz.front(), other[at].frame_start,
                               static_cast<int>(other[at].f0_hz.size()) + delta);
      const NoteMaskSet mismatched = build_note_masks(spec, track_over(spec, other));
      REQUIRE(mismatched.notes[at].n_frames != masks.notes[at].n_frames);
      REQUIRE(code_of([&] { return make_masked_notes(spec, track, mismatched, length); }) ==
              kInvalid);
    }
  }
}

TEST_CASE("a negative length and an empty spectrogram are rejected", "[polyphony_notes]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 4, 16), steady_ridge(kHighHz, 8, 12)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const MultiF0Track nothing_tracked = track_over(spec, {});
  const NoteMaskSet no_masks = build_note_masks(spec, nothing_tracked);

  for (const int bad : {-1, -kHopLength, -static_cast<int>(kSourceSamples)}) {
    INFO("length " << bad);
    REQUIRE(code_of([&] { return make_masked_notes(spec, track, masks, bad); }) == kInvalid);
    // And with no ridge to measure, so the bound is the parameter's rather than
    // a by-product of laying out a note's span.
    REQUIRE(code_of([&] { return make_masked_notes(spec, nothing_tracked, no_masks, bad); }) ==
            kInvalid);
  }

  const sonare::Spectrogram empty;
  REQUIRE(empty.empty());
  REQUIRE(code_of([&] { return make_masked_notes(empty, track, masks, 0); }) == kInvalid);
  REQUIRE(code_of([&] { return make_masked_notes(empty, nothing_tracked, no_masks, 0); }) ==
          kInvalid);
}

TEST_CASE("a spec and length whose inverse carries no samples is rejected per mask",
          "[polyphony_notes]") {
  // A single centred frame at length 0: the whole reconstruction is the padding
  // the trim removes, so there is no audio to measure a note over. The contract
  // calls that a framing error where a mask is inverted -- and with no mask to
  // invert, no notes rather than an error.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  std::vector<float> samples(static_cast<size_t>(kHopLength / 2), 0.0f);
  add_tone(samples, kLowHz, 0.3f, 10, kSampleRate);
  const sonare::Spectrogram spec = sonare::Spectrogram::compute(
      sonare::Audio::from_vector(std::move(samples), kSampleRate), stft_config());
  REQUIRE(spec.n_frames() == 1);
  // Not the empty-spectrogram rejection: this one has a frame and bins.
  REQUIRE(!spec.empty());
  REQUIRE(spec.n_bins() > 0);
  REQUIRE(spec.to_audio(0).empty());

  const MultiF0Track track = track_over(spec, {steady_ridge(kLowHz, 0, 1)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const MultiF0Track nothing_tracked = track_over(spec, {});
  const NoteMaskSet no_masks = build_note_masks(spec, nothing_tracked);

  REQUIRE(code_of([&] { return make_masked_notes(spec, track, masks, 0); }) == kInvalid);
  // With no ridge the same pair is not an error at all: there is nothing to
  // invert, so no notes is the answer. Deliberately unlike the masked renderer,
  // which has a residual to invert whatever the notes are and so rejects the pair
  // up front -- asserted as the difference it is, from both sides, rather than
  // read off one of them.
  REQUIRE(make_masked_notes(spec, nothing_tracked, no_masks, 0).empty());
  REQUIRE(code_of([&] { return render_masked_notes(spec, no_masks, {}, 0); }) == kInvalid);

  // The same spectrogram at a length that does carry samples is measurable, so
  // the rejection is the pair and not the framing on its own.
  REQUIRE_NOTHROW(make_masked_notes(spec, track, masks, kHopLength));
}

TEST_CASE("a config is refused where make_note refuses it, and for a reference pitch it does not",
          "[polyphony_notes]") {
  // Two rules at once, and the split is the contract's. The delegated one: for
  // every field but the reference pitch the two verdicts agree, because the
  // conversion reads only two of the four and the rest are validated by the
  // oracle rather than by this chain having anything to do with them. The one of
  // its own: a reference pitch that is not positive is refused here whatever
  // make_note makes of it, because the note it would otherwise return cannot
  // take a curve edit.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 4, 16), steady_ridge(kHighHz, 8, 12)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const MultiF0Track nothing_tracked = track_over(spec, {});
  const NoteMaskSet no_masks = build_note_masks(spec, nothing_tracked);
  const F0Ridge& ridge = track.ridges[0];
  const pitch_editor::F0Track mono = track_for_ridge(track, ridge);
  const sonare::Audio isolated = masked_audio(spec, masks.notes[0], length);

  size_t refused = 0;
  size_t accepted = 0;
  for (const NamedConfig& entry : delegated_configs()) {
    const sonare::ErrorCode oracle = code_of([&] {
      return note_model::make_note(isolated, mono, ridge.frame_start, ridge.frame_end(),
                                   entry.config);
    });
    const sonare::ErrorCode got =
        code_of([&] { return make_masked_notes(spec, track, masks, length, entry.config); });
    INFO(entry.what << ": make_note " << static_cast<int>(oracle) << ", make_masked_notes "
                    << static_cast<int>(got));
    REQUIRE(got == oracle);
    if (got == kInvalid) {
      ++refused;
    } else {
      ++accepted;
    }
  }
  // Both verdicts occurred, or the loop asserted agreement on one of them and
  // could not have seen a conversion that answers the other way round.
  INFO("refused " << refused << ", accepted " << accepted << " of " << delegated_configs().size());
  REQUIRE(refused > 0);
  REQUIRE(accepted > 0);

  for (const float bad : refused_reference_pitches()) {
    INFO("a reference pitch of " << bad);
    REQUIRE(code_of([&] {
              NoteExtractorConfig config;
              config.segmenter.reference_hz = bad;
              return make_masked_notes(spec, track, masks, length, config);
            }) == kInvalid);
    // Refused with no ridge to apply it to as well. Unlike the degenerate
    // spec-and-length pair, which is decided per mask because deciding it up
    // front costs a whole inverse, a config costs nothing to read -- and a field
    // that succeeds on an empty track and throws on a populated one is a wiring
    // bug that passed quietly.
    REQUIRE(code_of([&] {
              NoteExtractorConfig config;
              config.segmenter.reference_hz = bad;
              return make_masked_notes(spec, nothing_tracked, no_masks, length, config);
            }) == kInvalid);
  }
}

TEST_CASE("a framing the set and the track agree on but the spectrogram does not is rejected",
          "[polyphony_notes]") {
  // The set is checked against the spectrogram and against the track, and the
  // contract says both explicitly for this arrangement: a set and a track
  // agreeing with each other while disagreeing with the spectrogram would
  // otherwise pass, and a hop that is not the spectrogram's puts every span
  // somewhere else at the cadence the track was believed to have.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  // Ridges well inside the frame count, so a smaller n_frames below is still a
  // framing every ridge fits in and the rejection is the disagreement itself.
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 2, 10), steady_ridge(kHighHz, 4, 8)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> notes = make_masked_notes(spec, track, masks, length);
  require_usable_notes(notes);

  struct Named {
    std::string what;
    std::function<void(MultiF0Track&, NoteMaskSet&)> apply;
  };
  const std::vector<Named> broken = {
      {"half the spectrogram's hop",
       [](MultiF0Track& t, NoteMaskSet& s) {
         t.hop_length = kHopLength / 2;
         s.hop_length = kHopLength / 2;
       }},
      {"twice the spectrogram's hop",
       [](MultiF0Track& t, NoteMaskSet& s) {
         t.hop_length = 2 * kHopLength;
         s.hop_length = 2 * kHopLength;
       }},
      {"another sample rate",
       [](MultiF0Track& t, NoteMaskSet& s) {
         t.sample_rate = 44100;
         s.sample_rate = 44100;
       }},
      {"one frame fewer than the spectrogram", [](MultiF0Track& t, NoteMaskSet& s) {
         t.n_frames -= 1;
         s.n_frames -= 1;
       }}};

  for (const Named& entry : broken) {
    INFO(entry.what);
    MultiF0Track moved = track;
    NoteMaskSet set = masks;
    entry.apply(moved, set);
    // The break really is the spectrogram disagreeing and not the set
    // disagreeing with the track, which the case above already covers.
    REQUIRE(moved.hop_length == set.hop_length);
    REQUIRE(moved.sample_rate == set.sample_rate);
    REQUIRE(moved.n_frames == set.n_frames);
    REQUIRE(code_of([&] { return make_masked_notes(spec, moved, set, length); }) == kInvalid);
    // The renderer refuses the same set, so the two ends of the pair agree about
    // a framing that is not the spectrogram's.
    REQUIRE(code_of([&] { return render_masked_notes(spec, set, notes, length); }) == kInvalid);
  }
}

// --- Degenerate tracks -----------------------------------------------------

TEST_CASE("a track with no ridges measures nothing", "[polyphony_notes]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const MultiF0Track track = track_over(spec, {});
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE(masks.notes.empty());
  REQUIRE(masks.n_frames == spec.n_frames());

  for (const int length : {0, static_cast<int>(kSourceSamples)}) {
    INFO("length " << length);
    REQUIRE(make_masked_notes(spec, track, masks, length).empty());
  }
}

TEST_CASE("a ridge carrying a pitch that is not positive and finite is rejected",
          "[polyphony_notes]") {
  // A ridge from track_f0_ridges always carries a positive finite F0, so this is
  // a hand-built track -- which is what every caller past the core hands in. The
  // rejection is what holds the usable-curve clause: such a ridge would measure a
  // median_hz of 0, which is how a track spells no pitch, and the note would come
  // back looking measured while the next call refuses the curve edit.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);

  // One frame of the ridge and then all of them, because a check reading the
  // first value alone passes the first of those.
  for (const float bad : {0.0f, -kLowHz, kNaN, kInf, -kInf}) {
    MultiF0Track whole = track_over(spec, {steady_ridge(kLowHz, 4, 16)});
    const NoteMaskSet masks = build_note_masks(spec, whole);
    REQUIRE_NOTHROW(make_masked_notes(spec, whole, masks, length));

    MultiF0Track one_frame = whole;
    one_frame.ridges[0].f0_hz[7] = bad;
    whole.ridges[0].f0_hz.assign(whole.ridges[0].f0_hz.size(), bad);

    INFO("an f0 of " << bad);
    REQUIRE(code_of([&] { return make_masked_notes(spec, whole, masks, length); }) == kInvalid);
    REQUIRE(code_of([&] { return make_masked_notes(spec, one_frame, masks, length); }) == kInvalid);
  }

  // And at both ends of a set of three, so a check reading one ridge is not
  // covered by another.
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 2, 14), steady_ridge(kHighHz, 6, 12),
                        steady_ridge(2.0f * kLowHz, 10, 10)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE_NOTHROW(make_masked_notes(spec, track, masks, length));
  for (const size_t at : {size_t{0}, size_t{2}}) {
    INFO("ridge " << at);
    MultiF0Track broken = track;
    broken.ridges[at].f0_hz[3] = 0.0f;
    REQUIRE(code_of([&] { return make_masked_notes(spec, broken, masks, length); }) == kInvalid);
  }
}

TEST_CASE("a ridge whose frames fall outside the track is rejected", "[polyphony_notes]") {
  // Not implied by the set agreeing with the track and the track with the
  // spectrogram: the track's own frame count is what the F0 values are indexed
  // against, so a ridge reaching past it is a read off the end of the track this
  // builds rather than an odd input. The mask is moved with the ridge, so what is
  // caught is the two of them leaving the framing and not the set-level
  // disagreement the case above covers.
  //
  // What this case fixes is the rejection. Where the check sits relative to the
  // inverse is fixed by a sanitizer run and not here: the write into the
  // synthesized track happens before the mask bound is read, so an ordering the
  // other way round is a heap write before the refusal -- one exception from here
  // either way, a heap-buffer-overflow under ASan.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const int frames = spec.n_frames();
  const MultiF0Track track = track_over(spec, {steady_ridge(kLowHz, frames - 12, 8)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE_NOTHROW(make_masked_notes(spec, track, masks, length));

  for (const int shift : {-(frames - 12) - 1, 8, 12}) {
    INFO("the ridge and its mask moved by " << shift << " frames");
    MultiF0Track moved = track;
    moved.ridges[0].frame_start += shift;
    moved.ridges[0].onset_sample = static_cast<int64_t>(moved.ridges[0].frame_start) * kHopLength;
    moved.ridges[0].offset_sample = static_cast<int64_t>(moved.ridges[0].frame_end()) * kHopLength;
    NoteMaskSet set = masks;
    set.notes[0].frame_start += shift;
    // Outside the track on one side or the other, and the mask says the same.
    REQUIRE((moved.ridges[0].frame_start < 0 || moved.ridges[0].frame_end() > moved.n_frames));
    REQUIRE(set.notes[0].frame_start == moved.ridges[0].frame_start);
    REQUIRE(set.notes[0].n_frames == static_cast<int>(moved.ridges[0].f0_hz.size()));
    REQUIRE(code_of([&] { return make_masked_notes(spec, moved, set, length); }) == kInvalid);
  }
}

TEST_CASE("a length ending before a ridge's span is a framing error", "[polyphony_notes]") {
  // Both ends of the span clamp to the length, so the note would be measured over
  // nothing -- which the frame bound make_note checks does not catch, the span
  // being inside the track's frames either way. A note measured over nothing is
  // a framing error like an inverse carrying no samples, not a note to hand back.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const MultiF0Track track = track_over(spec, {steady_ridge(kLowHz, 20, 8)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const int64_t onset = track.ridges[0].onset_sample;
  REQUIRE(onset == 20 * kHopLength);
  // The ridge has to start after frame 0: the frame-to-sample rule returns 0 for
  // frame 0 whatever the length, so a ridge starting there keeps its onset while
  // only the offset clamps, leaving a span and nothing for this case to reject.
  REQUIRE(track.ridges[0].frame_start > 0);

  // Up to and including the length the span starts at, which is the last one that
  // leaves nothing: the clamp puts both ends on it.
  for (const int length : {1, kHopLength, static_cast<int>(onset) - 1, static_cast<int>(onset)}) {
    INFO("length " << length);
    REQUIRE(static_cast<int64_t>(length) <= onset);
    REQUIRE(code_of([&] { return make_masked_notes(spec, track, masks, length); }) == kInvalid);
  }

  // One sample further is the first length that leaves a span, so the rejection
  // is the empty measurement and not a bound on how short the audio may be. The
  // note it returns is still one the next call accepts, which is what the
  // returned-note validation is there for.
  const std::vector<NoteObject> got =
      make_masked_notes(spec, track, masks, static_cast<int>(onset) + 1);
  REQUIRE(got.size() == 1);
  INFO("span [" << got[0].onset_sample << ", " << got[0].offset_sample << ")");
  REQUIRE(got[0].onset_sample == onset);
  REQUIRE(got[0].offset_sample == onset + 1);
  require_usable_notes(got);
}
