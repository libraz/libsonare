#include "editing/polyphony/polyphonic_edit.h"

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
#include "editing/polyphony/masked_notes.h"
#include "editing/polyphony/masked_renderer.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "editing/polyphony/shared_bins.h"
#include "util/constants.h"
#include "util/exception.h"

namespace note_model = sonare::editing::note_model;

using sonare::editing::note_model::NoteCurve;
using sonare::editing::note_model::NoteObject;
using sonare::editing::note_model::NoteRenderConfig;
using namespace sonare::editing::polyphony;

namespace {

/// The framing every fixture below is reasoned in, which is the one
/// @ref polyphony_stft_defaults recommends and the one the extraction is
/// calibrated for. The two tones are a fifth apart at 44.1 kHz, where the mask
/// and extraction suites already read ridges off the same material.
constexpr int kSampleRate = 44100;
constexpr int kNfft = 4096;
constexpr int kHopLength = 512;
constexpr size_t kSourceSamples = 22050;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// E4 and B4: a fifth, so the lower tone's every third partial lands on the
/// upper one's and the equal split has something to divide.
constexpr float kLowHz = 329.6276f;
constexpr float kHighHz = 493.8833f;

/// @brief How far a sum of per-note inverses may sit from one inverse of the
///        whole, relative to the reconstruction's peak.
/// @details Inherited rather than measured here: the quantity is the masked
///          renderer's telescoping error, measured in polyphonic_render_test.cpp
///          at this very framing -- three notes plus a residual at 4096 and 512
///          over 44.1 kHz deviate by 2.51e-07 of a 0.950 peak, two notes by
///          1.79e-07 of 0.886. This fixture's synthesis peaks at 0.887, so the
///          bound is four times the worst of those at about the same scale.
///
///          It does not cover a set of any size. The deviation carries one
///          transform rounding per summand, and 1e-06 of 0.887 is some fifteen
///          float ulps of the peak, so the cases below state the note count they
///          ran at. A division error sits five orders above it: a half where a
///          third belongs is 0.17 of the value, a dropped note all of it.
constexpr double kTelescopeRelative = 1e-6;

/// @brief Peak a fixture reconstructs to, below which a comparison is vacuous.
/// @details The two tones sum to 0.887 measured on the synthesis itself, so this
///          is four times under it; asserted next to every relative comparison
///          rather than in a fixture, because zeros agree with zeros exactly.
constexpr double kFixturePeakFloor = 0.2;

/// @brief How far above the tolerance a quantity has to sit to be worth
///        comparing against it.
constexpr double kGuardMargin = 50.0;

/// @brief Per-frame RMS below which an isolated note carries nothing to measure.
/// @details Algebraic rather than measured: one tone of ten partials at 1/h and
///          0.25 has RMS 0.25 * sqrt(0.5 * sum 1/h^2) = 0.22, and a masked note
///          is at most that. This is four times under it and far over the zero a
///          silent or misaddressed buffer reads.
constexpr float kNoteAmplitudeFloor = 0.05f;

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
void add_tone(std::vector<float>& into, float f0_hz, float amplitude, int n_partials) {
  const double nyquist = 0.5 * static_cast<double>(kSampleRate);
  for (int h = 1; h <= n_partials; ++h) {
    const double hz = static_cast<double>(h) * static_cast<double>(f0_hz);
    if (hz >= nyquist) break;
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
sonare::Audio source_audio() {
  std::vector<float> samples(kSourceSamples, 0.0f);
  add_tone(samples, kLowHz, 0.25f, 10);
  add_tone(samples, kHighHz, 0.25f, 10);
  return audio_of(std::move(samples));
}

sonare::Audio silent_audio() { return audio_of(std::vector<float>(kSourceSamples, 0.0f)); }

sonare::Spectrogram spectrogram_of(const sonare::Audio& audio,
                                   const sonare::StftConfig& stft = polyphony_stft_defaults()) {
  return sonare::Spectrogram::compute(audio, stft);
}

// --- The chain this file composes ------------------------------------------

/// @brief The committed calls the contract runs, in the order it states.
/// @details The oracle for everything @ref analyze_polyphonic returns. Built from
///          the public entry points rather than from a second spelling of what
///          they do, so a case reading it is comparing one composition against
///          another and not against a remembered number. The spectrogram form of
///          the extraction is the one used, since that is the call that takes an
///          STFT the caller already holds.
///
///          A composition is only an oracle for the stages it runs: a stage
///          missing from both sides agrees with itself. @ref masks_differ is what
///          keeps that from passing silently for the apportionment.
struct Chain {
  sonare::Spectrogram spectrum;
  MultiF0Track track;
  NoteMaskSet masks;
  std::vector<NoteObject> notes;
  int length = 0;
};

Chain chain_by_hand(const sonare::Audio& audio, const PolyphonicEditConfig& config = {}) {
  Chain built;
  built.length = static_cast<int>(audio.size());
  built.spectrum = sonare::Spectrogram::compute(audio, config.extraction.stft);
  built.track = extract_multi_f0(audio, built.spectrum, config.extraction);
  built.masks = build_note_masks(built.spectrum, built.track, config.masks);
  built.masks = solve_shared_bins(built.spectrum, built.masks, built.track, config.shared_bins);
  built.notes =
      make_masked_notes(built.spectrum, built.track, built.masks, built.length, config.notes);
  return built;
}

/// @brief Whether two mask sets divide any shared bin differently.
/// @details The geometry is not compared: the apportionment is defined to leave
///          the bins and the note order alone, so the weights are the only place
///          its presence shows. A caller asserting a difference over equal
///          geometry is asserting that one stage ran.
bool masks_differ(const NoteMaskSet& a, const NoteMaskSet& b) {
  if (a.notes.size() != b.notes.size()) return true;
  for (size_t i = 0; i < a.notes.size(); ++i) {
    if (a.notes[i].bins != b.notes[i].bins) return true;
    if (a.notes[i].weights != b.notes[i].weights) return true;
  }
  return false;
}

// --- Hand-built analyses ---------------------------------------------------

/// @brief A ridge holding one pitch over [frame_start, frame_start + n_frames).
F0Ridge steady_ridge(float f0_hz, int frame_start, int n_frames) {
  F0Ridge ridge;
  ridge.frame_start = frame_start;
  ridge.f0_hz.assign(static_cast<size_t>(n_frames), f0_hz);
  ridge.salience.assign(static_cast<size_t>(n_frames), 1.0f);
  ridge.onset_sample = static_cast<int64_t>(frame_start) * kHopLength;
  ridge.offset_sample = static_cast<int64_t>(frame_start + n_frames) * kHopLength;
  ridge.median_hz = f0_hz;
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

/// @brief An analysis over hand-placed ridges, assembled by the calls below it.
/// @details Every member comes from the committed stage that produces it at the
///          one @p length, so the result is internally consistent the way a
///          returned analysis is, without the case depending on what the
///          extraction found in the material. The cases about a host's edits and
///          about a broken analysis use this; the cases about what
///          @ref analyze_polyphonic measures use real material.
PolyphonicAnalysis analysis_over(const sonare::Spectrogram& spec, std::vector<F0Ridge> ridges,
                                 int length) {
  PolyphonicAnalysis analysis;
  analysis.spectrum = spec;
  analysis.track = track_over(spec, std::move(ridges));
  analysis.masks = build_note_masks(spec, analysis.track);
  analysis.notes = make_masked_notes(spec, analysis.track, analysis.masks, length);
  analysis.length = length;
  return analysis;
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

double peak_of(const sonare::Audio& audio) {
  double highest = 0.0;
  for (size_t i = 0; i < audio.size(); ++i) {
    highest = std::max(highest, std::abs(static_cast<double>(audio[i])));
  }
  return highest;
}

/// @brief Asserts @p got is @p want up to the telescoping bound, with the
///        non-vacuity guard at the assertion.
void require_telescopes(const sonare::Audio& got, const sonare::Audio& want,
                        const std::string& what, size_t summands) {
  REQUIRE(got.size() == want.size());
  REQUIRE(got.sample_rate() == want.sample_rate());
  const Agreement how = agreement(got, want);
  INFO(what << ": worst |difference| " << how.worst << " at sample " << how.at << ", peak "
            << how.scale << ", ratio " << (how.worst / how.scale) << ", over " << summands
            << " note inverses plus the residual");
  REQUIRE(how.scale > kFixturePeakFloor);
  // The bound covers about fifteen summands; a set past that is outside what the
  // figure it was taken from says.
  REQUIRE(summands <= 12);
  REQUIRE(how.worst <= kTelescopeRelative * how.scale);
}

/// @brief Asserts two buffers are the same buffer, sample for sample.
/// @details Exact, and not a tolerance: the two sides are one function over one
///          set of arguments, so a difference of any size is a different call
///          rather than a rounding of the same one.
void require_same_audio(const sonare::Audio& got, const sonare::Audio& want,
                        const std::string& what) {
  REQUIRE(got.size() == want.size());
  REQUIRE(got.sample_rate() == want.sample_rate());
  const Agreement how = agreement(got, want);
  INFO(what << ": worst |difference| " << how.worst << " at sample " << how.at << ", peak "
            << how.scale);
  REQUIRE(how.scale > kFixturePeakFloor);
  REQUIRE(how.worst == 0.0);
}

// --- Reading the analysis's members ---------------------------------------

float median_of(const std::vector<float>& values) {
  REQUIRE(!values.empty());
  std::vector<float> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  const size_t half = sorted.size() / 2;
  return sorted.size() % 2 == 1 ? sorted[half] : 0.5f * (sorted[half - 1] + sorted[half]);
}

/// @brief Asserts two spectrograms are the same STFT, geometry and data.
/// @details Every geometry field is named rather than compared through the frame
///          count alone, so a framing that merely shares today's defaults cannot
///          pass for the one asked for. The data comparison is exact for the
///          reason @ref require_same_audio is, and its guard is that the
///          reference carries a non-zero bin -- two empty spectra agree.
void require_same_spectrum(const sonare::Spectrogram& got, const sonare::Spectrogram& want) {
  REQUIRE(got.n_bins() == want.n_bins());
  REQUIRE(got.n_frames() == want.n_frames());
  REQUIRE(got.n_fft() == want.n_fft());
  REQUIRE(got.hop_length() == want.hop_length());
  REQUIRE(got.win_length() == want.win_length());
  REQUIRE(got.center() == want.center());
  REQUIRE(got.window() == want.window());
  REQUIRE(got.pad_mode() == want.pad_mode());
  REQUIRE(got.sample_rate() == want.sample_rate());

  const size_t values = static_cast<size_t>(want.n_bins()) * static_cast<size_t>(want.n_frames());
  REQUIRE(values > 0);
  double largest = 0.0;
  double worst = 0.0;
  for (size_t i = 0; i < values; ++i) {
    largest = std::max(largest, static_cast<double>(std::abs(want.complex_data()[i])));
    worst = std::max(worst,
                     static_cast<double>(std::abs(got.complex_data()[i] - want.complex_data()[i])));
  }
  INFO("the spectrum: worst |difference| " << worst << ", largest |bin| " << largest);
  // Zero is the only value that makes the exact comparison vacuous, so that is
  // the bound stated -- no level of this fixture's STFT is measured here.
  REQUIRE(largest > 0.0);
  REQUIRE(worst == 0.0);
}

void require_same_track(const MultiF0Track& got, const MultiF0Track& want) {
  REQUIRE(got.n_frames == want.n_frames);
  REQUIRE(got.hop_length == want.hop_length);
  REQUIRE(got.sample_rate == want.sample_rate);
  REQUIRE(got.polyphony == want.polyphony);
  REQUIRE(got.ridges.size() == want.ridges.size());
  for (size_t i = 0; i < want.ridges.size(); ++i) {
    INFO("ridge " << i);
    REQUIRE(got.ridges[i].frame_start == want.ridges[i].frame_start);
    REQUIRE(got.ridges[i].f0_hz == want.ridges[i].f0_hz);
    REQUIRE(got.ridges[i].salience == want.ridges[i].salience);
    REQUIRE(got.ridges[i].onset_sample == want.ridges[i].onset_sample);
    REQUIRE(got.ridges[i].offset_sample == want.ridges[i].offset_sample);
    REQUIRE(got.ridges[i].median_hz == want.ridges[i].median_hz);
  }
}

void require_same_masks(const NoteMaskSet& got, const NoteMaskSet& want) {
  REQUIRE(got.n_bins == want.n_bins);
  REQUIRE(got.n_frames == want.n_frames);
  REQUIRE(got.hop_length == want.hop_length);
  REQUIRE(got.sample_rate == want.sample_rate);
  REQUIRE(got.config.n_harmonics == want.config.n_harmonics);
  REQUIRE(got.config.claim_lobes == want.config.claim_lobes);
  REQUIRE(got.config.inharmonicity == want.config.inharmonicity);
  REQUIRE(got.notes.size() == want.notes.size());
  for (size_t i = 0; i < want.notes.size(); ++i) {
    INFO("mask " << i);
    REQUIRE(got.notes[i].ridge_index == want.notes[i].ridge_index);
    REQUIRE(got.notes[i].frame_start == want.notes[i].frame_start);
    REQUIRE(got.notes[i].n_frames == want.notes[i].n_frames);
    REQUIRE(got.notes[i].frame_offset == want.notes[i].frame_offset);
    REQUIRE(got.notes[i].bins == want.notes[i].bins);
    REQUIRE(got.notes[i].weights == want.notes[i].weights);
  }
}

void require_same_curve(const NoteCurve& got, const NoteCurve& want, const char* which) {
  INFO(which);
  REQUIRE(got.values == want.values);
  REQUIRE(got.frame_rate_hz == want.frame_rate_hz);
  REQUIRE(got.frame_offset == want.frame_offset);
}

/// @brief Asserts two notes are the same note, field by field.
/// @details Every field is named rather than compared through a whole-struct
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

void require_same_notes(const std::vector<NoteObject>& got, const std::vector<NoteObject>& want) {
  REQUIRE(got.size() == want.size());
  for (size_t i = 0; i < want.size(); ++i) {
    require_same_note(got[i], want[i], "note " + std::to_string(i));
  }
}

/// @brief Asserts every note of @p analysis is one the render chain accepts and
///        one that measured something.
void require_usable_notes(const PolyphonicAnalysis& analysis) {
  for (size_t i = 0; i < analysis.notes.size(); ++i) {
    INFO("note " << i);
    const NoteObject& note = analysis.notes[i];
    REQUIRE(note.edit.is_identity());
    REQUIRE_NOTHROW(note_model::validate_note_for_render(note));
    REQUIRE(note.onset_sample >= 0);
    REQUIRE(note.onset_sample < note.offset_sample);
    REQUIRE(note.offset_sample <= static_cast<int64_t>(analysis.length));
    REQUIRE(note.median_hz > 0.0f);
    // The note measured a buffer with a note in it: a curve of zeros agrees with
    // a measurement taken over the wrong audio.
    REQUIRE(median_of(note.amplitude.values) > kNoteAmplitudeFloor);
    // The curve edits the pitch curve has to carry are carried, which is the one
    // state validate_note_for_render refuses a note for being unable to take.
    NoteObject wobbled = note;
    wobbled.edit.vibrato_depth_change = 0.5f;
    REQUIRE_NOTHROW(note_model::validate_note_for_render(wobbled));
    NoteObject drifted = note;
    drifted.edit.drift_change = -0.5f;
    REQUIRE_NOTHROW(note_model::validate_note_for_render(drifted));
  }
}

/// @brief Weights the equal split put below one, which is where it divided.
size_t shared_weights(const NoteMaskSet& masks) {
  size_t shared = 0;
  for (const NoteMask& mask : masks.notes) {
    for (const std::complex<float>& weight : mask.weights) {
      if (weight != 1.0f) ++shared;
    }
  }
  return shared;
}

// --- Shared rejection inputs ----------------------------------------------

struct NamedNoteBreak {
  std::string what;
  std::function<void(NoteObject&)> apply;
};

/// @brief Every way one note stops being renderable.
/// @details One list, applied at both ends of a set, so a check reading one note
///          or stopping once it has something to render covers neither. Each
///          entry breaks exactly one field of a note the call already accepted,
///          so a rejection is attributable to it, and the case reading this
///          asserts the exposed per-note validator agrees.
std::vector<NamedNoteBreak> note_breaks() {
  std::vector<NamedNoteBreak> broken;
  broken.push_back(
      {"an empty span", [](NoteObject& note) { note.offset_sample = note.onset_sample; }});
  broken.push_back({"a reversed span",
                    [](NoteObject& note) { std::swap(note.onset_sample, note.offset_sample); }});
  broken.push_back({"a span starting before zero", [](NoteObject& note) {
                      note.onset_sample = -1;
                      note.offset_sample = std::max<int64_t>(note.offset_sample, 1);
                    }});
  const std::vector<std::pair<const char*, float>> unusable = {
      {"nan", kNaN}, {"inf", kInf}, {"-inf", -kInf}};
  for (const auto& bad : unusable) {
    const float value = bad.second;
    broken.push_back({std::string("a pitch shift of ") + bad.first,
                      [value](NoteObject& note) { note.edit.pitch_shift_semitones = value; }});
    broken.push_back({std::string("a gain of ") + bad.first,
                      [value](NoteObject& note) { note.edit.gain_db = value; }});
    broken.push_back({std::string("a formant shift of ") + bad.first,
                      [value](NoteObject& note) { note.edit.formant_shift_semitones = value; }});
    broken.push_back({std::string("a time stretch ratio of ") + bad.first,
                      [value](NoteObject& note) { note.edit.time_stretch_ratio = value; }});
  }
  for (const float bad : {0.0f, -1.0f}) {
    broken.push_back({"a time stretch ratio of " + std::to_string(bad),
                      [bad](NoteObject& note) { note.edit.time_stretch_ratio = bad; }});
  }
  broken.push_back({"a non-finite envelope value",
                    [](NoteObject& note) { note.edit.amplitude_envelope = {1.0f, kNaN, 1.0f}; }});
  broken.push_back({"a negative envelope value",
                    [](NoteObject& note) { note.edit.amplitude_envelope = {1.0f, -0.5f, 1.0f}; }});
  broken.push_back({"a vibrato edit with no pitch curve", [](NoteObject& note) {
                      note.f0_hz.values.clear();
                      note.edit.vibrato_depth_change = 0.5f;
                    }});
  broken.push_back({"a drift edit on an unvoiced curve", [](NoteObject& note) {
                      note.f0_hz.values.assign(note.f0_hz.values.size(), 0.0f);
                      note.median_hz = 0.0f;
                      note.edit.drift_change = 0.5f;
                    }});
  return broken;
}

struct NamedConfig {
  std::string what;
  NoteRenderConfig config;
};

/// @brief Configs the render chain refuses, on both sides of its own split.
/// @details @c fade_ms is what @ref note_model::validate_render_config covers and
///          @c decomposition is validated where it is read. Which is which is the
///          monophonic chain's contract and is asserted there; this file asserts
///          that @ref render_polyphonic answers whatever its delegate answers.
std::vector<NamedConfig> rejected_configs() {
  std::vector<NamedConfig> broken;
  const std::vector<std::pair<const char*, float>> unusable = {
      {"nan", kNaN}, {"inf", kInf}, {"-inf", -kInf}};
  for (const auto& bad : unusable) {
    NoteRenderConfig config;
    config.fade_ms = bad.second;
    broken.push_back({std::string("a fade of ") + bad.first, config});
  }
  for (const float bad : {-1e-6f, -5.0f}) {
    NoteRenderConfig config;
    config.fade_ms = bad;
    broken.push_back({"a negative fade " + std::to_string(bad), config});
  }
  for (const auto& bad : unusable) {
    NoteRenderConfig config;
    config.decomposition.vibrato_cutoff_hz = bad.second;
    broken.push_back({std::string("a vibrato cutoff of ") + bad.first, config});
  }
  return broken;
}

struct NamedAnalysisBreak {
  std::string what;
  std::function<void(PolyphonicAnalysis&)> apply;
};

/// @brief Every way an analysis's members stop describing each other, where what
///        they have to describe is the spectrum, the note-to-mask pairing, or the
///        output length.
/// @details Written as mutations of an accepted analysis, so each rejection is
///          attributable to one field, and every one of them is a disagreement
///          the contract names rather than a value that is merely odd.
std::vector<NamedAnalysisBreak> analysis_breaks(const sonare::Spectrogram& spec) {
  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  std::vector<NamedAnalysisBreak> broken;
  for (const int bad : {bins + 1, bins - 1, 0, -1}) {
    broken.push_back({"the masks' n_bins " + std::to_string(bad),
                      [bad](PolyphonicAnalysis& analysis) { analysis.masks.n_bins = bad; }});
  }
  for (const int bad : {frames + 1, frames - 1, 0, -1}) {
    broken.push_back({"the masks' n_frames " + std::to_string(bad),
                      [bad](PolyphonicAnalysis& analysis) { analysis.masks.n_frames = bad; }});
  }
  for (const int bad : {kHopLength + 1, kHopLength / 2, 0, -1}) {
    broken.push_back({"the masks' hop_length " + std::to_string(bad),
                      [bad](PolyphonicAnalysis& analysis) { analysis.masks.hop_length = bad; }});
  }
  for (const int bad : {kSampleRate + 1, 16000, 0, -1}) {
    broken.push_back({"the masks' sample_rate " + std::to_string(bad),
                      [bad](PolyphonicAnalysis& analysis) { analysis.masks.sample_rate = bad; }});
  }
  // The pairing, in both directions: a check written as one inequality passes
  // half of these.
  broken.push_back(
      {"one note dropped", [](PolyphonicAnalysis& analysis) { analysis.notes.pop_back(); }});
  broken.push_back(
      {"every note dropped", [](PolyphonicAnalysis& analysis) { analysis.notes.clear(); }});
  broken.push_back({"one note too many", [](PolyphonicAnalysis& analysis) {
                      analysis.notes.push_back(analysis.notes.back());
                    }});
  broken.push_back(
      {"one mask dropped", [](PolyphonicAnalysis& analysis) { analysis.masks.notes.pop_back(); }});
  for (const int bad : {-1, -kHopLength, -static_cast<int>(kSourceSamples)}) {
    broken.push_back({"a length of " + std::to_string(bad),
                      [bad](PolyphonicAnalysis& analysis) { analysis.length = bad; }});
  }
  // A mask's own shape, which the renderer checks where it applies the mask. Here
  // because an analysis carrying one is still an analysis that cannot be rendered.
  broken.push_back({"a mask's frame_offset one short", [](PolyphonicAnalysis& analysis) {
                      analysis.masks.notes.front().frame_offset.pop_back();
                    }});
  broken.push_back({"a mask bin past the spectrum", [bins](PolyphonicAnalysis& analysis) {
                      analysis.masks.notes.back().bins.front() = static_cast<int32_t>(bins);
                    }});
  broken.push_back({"a zero mask weight", [](PolyphonicAnalysis& analysis) {
                      analysis.masks.notes.back().weights.front() = 0.0f;
                    }});
  return broken;
}

}  // namespace

// --- What the analysis is --------------------------------------------------

TEST_CASE("the analysis is the chain below it, measured at the source's own length",
          "[polyphony_edit]") {
  // The contract's core claim, and the only case that can tell a call that runs
  // the chain from one that spells any part of it again: every member is compared
  // against the committed stage that produces it, over the same audio and the
  // same config, so nothing here states what a ridge, a mask or a measured note
  // is.
  //
  // That the one STFT is computed once rather than twice is not asserted. The
  // call returns one spectrum and reports no work, so a second transform has no
  // observable but time, which is not an instrument this suite carries.
  const sonare::StftConfig defaults = polyphony_stft_defaults();
  REQUIRE(defaults.n_fft == kNfft);
  REQUIRE(defaults.hop_length == kHopLength);

  const sonare::Audio audio = source_audio();

  SECTION("the default framing") {
    const PolyphonicAnalysis analysis = analyze_polyphonic(audio);
    const Chain want = chain_by_hand(audio);

    // The material tracks ridges, or every comparison below holds over empty
    // vectors. The mask suite reads ridges off this same half second.
    INFO("the extraction found " << want.track.ridges.size() << " ridges");
    REQUIRE(!analysis.notes.empty());

    // The oracle's last mask stage is not a no-op on this material, which is what
    // makes the mask comparison below able to see it at all. Without this the
    // chain and the call could both be missing the apportionment and agree.
    const NoteMaskSet equally_split =
        build_note_masks(want.spectrum, want.track, PolyphonicEditConfig{}.masks);
    REQUIRE(masks_differ(equally_split, want.masks));

    require_same_spectrum(analysis.spectrum, want.spectrum);
    require_same_track(analysis.track, want.track);
    require_same_masks(analysis.masks, want.masks);
    require_same_notes(analysis.notes, want.notes);
    // The source's own count, which is what makes the spans and the render agree
    // without a caller passing one number twice.
    REQUIRE(analysis.length == static_cast<int>(audio.size()));
    REQUIRE(analysis.length == want.length);
    require_usable_notes(analysis);
  }

  SECTION("a framing the configuration asks for, and not this file's default") {
    PolyphonicEditConfig config;
    config.extraction.stft = sonare::make_stft_config(2 * kNfft, 2 * kHopLength);
    const PolyphonicAnalysis analysis = analyze_polyphonic(audio, config);
    const Chain want = chain_by_hand(audio, config);

    // The framing reached the STFT, which is the one member a different config
    // changes whatever the material turns out to hold.
    REQUIRE(analysis.spectrum.n_fft() == 2 * kNfft);
    REQUIRE(analysis.spectrum.hop_length() == 2 * kHopLength);
    REQUIRE(analysis.spectrum.n_frames() != spectrogram_of(audio).n_frames());
    INFO("the longer window found " << analysis.track.ridges.size() << " ridges");

    require_same_spectrum(analysis.spectrum, want.spectrum);
    require_same_track(analysis.track, want.track);
    require_same_masks(analysis.masks, want.masks);
    require_same_notes(analysis.notes, want.notes);
    REQUIRE(analysis.length == static_cast<int>(audio.size()));
  }
}

TEST_CASE("every member of the analysis describes the others", "[polyphony_edit]") {
  // The pairing is what a host edits against: notes[i] is the note of
  // masks.notes[i] of ridge i, and the framing is one framing. A member that
  // described a different analysis would leave the host editing a note measured
  // somewhere else.
  const sonare::Audio audio = source_audio();
  const PolyphonicAnalysis analysis = analyze_polyphonic(audio);
  REQUIRE(!analysis.notes.empty());

  REQUIRE(analysis.notes.size() == analysis.masks.notes.size());
  REQUIRE(analysis.notes.size() == analysis.track.ridges.size());
  REQUIRE(analysis.masks.n_frames == analysis.spectrum.n_frames());
  REQUIRE(analysis.masks.n_bins == analysis.spectrum.n_bins());
  REQUIRE(analysis.masks.hop_length == analysis.spectrum.hop_length());
  REQUIRE(analysis.masks.sample_rate == analysis.spectrum.sample_rate());
  REQUIRE(analysis.track.n_frames == analysis.spectrum.n_frames());
  REQUIRE(analysis.track.hop_length == analysis.spectrum.hop_length());
  REQUIRE(analysis.track.sample_rate == analysis.spectrum.sample_rate());
  REQUIRE(analysis.spectrum.sample_rate() == kSampleRate);
  REQUIRE(analysis.length == static_cast<int>(kSourceSamples));
  REQUIRE(analysis.masks.config.n_harmonics == PolyphonicEditConfig{}.masks.n_harmonics);

  for (size_t i = 0; i < analysis.notes.size(); ++i) {
    INFO("note " << i << " at " << analysis.notes[i].median_hz << " Hz");
    const F0Ridge& ridge = analysis.track.ridges[i];
    const NoteMask& mask = analysis.masks.notes[i];
    REQUIRE(mask.ridge_index == static_cast<int>(i));
    REQUIRE(mask.frame_start == ridge.frame_start);
    REQUIRE(mask.n_frames == static_cast<int>(ridge.f0_hz.size()));
    REQUIRE(analysis.notes[i].frame_start == ridge.frame_start);
    REQUIRE(analysis.notes[i].frame_end == ridge.frame_end());
    // The pitch curve is the ridge's own values, which is the one measured field
    // that carries over directly.
    REQUIRE(analysis.notes[i].f0_hz.values == ridge.f0_hz);
    REQUIRE(analysis.notes[i].f0_hz.frame_offset == ridge.frame_start);
    REQUIRE(analysis.notes[i].amplitude.frame_offset == ridge.frame_start);
    REQUIRE(ridge.frame_start >= 0);
    REQUIRE(ridge.frame_end() <= analysis.spectrum.n_frames());
  }
  require_usable_notes(analysis);
}

TEST_CASE("every stage configuration reaches its stage", "[polyphony_edit]") {
  // One config per stage, read off a value rather than off the absence of a
  // throw. A config silently dropped would return the default analysis, which is
  // a correct-looking result measured against something the caller did not ask
  // for.
  const sonare::Audio audio = source_audio();
  const PolyphonicAnalysis base = analyze_polyphonic(audio);
  REQUIRE(!base.notes.empty());

  SECTION("the extraction's own cap on voices per frame") {
    PolyphonicEditConfig config;
    config.extraction.estimation.max_polyphony = 1;
    const PolyphonicAnalysis analysis = analyze_polyphonic(audio, config);
    require_same_track(analysis.track, chain_by_hand(audio, config).track);

    // Two sides: the default resolves more than one voice somewhere, and the cap
    // leaves none. One of those alone would pass on a config nothing read.
    const int default_most =
        *std::max_element(base.track.polyphony.begin(), base.track.polyphony.end());
    const int capped_most =
        *std::max_element(analysis.track.polyphony.begin(), analysis.track.polyphony.end());
    INFO("voices in the busiest frame: " << default_most << " by default, " << capped_most
                                         << " capped at one");
    REQUIRE(default_most >= 2);
    REQUIRE(capped_most <= 1);
  }

  SECTION("the mask geometry") {
    PolyphonicEditConfig config;
    config.masks.n_harmonics = 3;
    const PolyphonicAnalysis analysis = analyze_polyphonic(audio, config);
    require_same_masks(analysis.masks, chain_by_hand(audio, config).masks);
    REQUIRE(analysis.masks.config.n_harmonics == 3);
    REQUIRE(analysis.masks.notes.size() == base.masks.notes.size());
    // Three partials claim fewer bins than twenty, so the narrower geometry is
    // visible in the masks rather than only in the carried config.
    for (size_t i = 0; i < analysis.masks.notes.size(); ++i) {
      INFO("mask " << i);
      REQUIRE(analysis.masks.notes[i].bins.size() < base.masks.notes[i].bins.size());
    }
    // And the render still telescopes: what the notes no longer claim the
    // residual carries.
    require_telescopes(render_polyphonic(analysis), analysis.spectrum.to_audio(analysis.length),
                       "a three-partial claim", analysis.notes.size());
  }

  SECTION("the thresholds the apportionment refuses on") {
    // Read off both ends of one threshold rather than off a carried value: the
    // mask set does not carry SharedBinConfig, so the only evidence the config
    // arrived is the division it produced.
    PolyphonicEditConfig config;
    config.shared_bins.window_frames = 64;
    const PolyphonicAnalysis analysis = analyze_polyphonic(audio, config);
    require_same_masks(analysis.masks, chain_by_hand(audio, config).masks);

    // A window longer than the material's own frame count leaves every span too
    // short to fit, so every shared bin refuses and the equal split stands.
    REQUIRE(config.shared_bins.window_frames > analysis.spectrum.n_frames());
    const NoteMaskSet equally_split =
        build_note_masks(analysis.spectrum, analysis.track, config.masks);
    REQUIRE(!masks_differ(equally_split, analysis.masks));
    // And the default does divide this material, so the section above is not
    // reporting a stage that never moves anything.
    REQUIRE(masks_differ(equally_split, base.masks));
  }

  SECTION("the reference pitch the measurement is stated against") {
    constexpr float kOtherReference = 100.0f;
    PolyphonicEditConfig config;
    config.notes.segmenter.reference_hz = kOtherReference;
    const PolyphonicAnalysis analysis = analyze_polyphonic(audio, config);
    require_same_notes(analysis.notes, chain_by_hand(audio, config).notes);
    // The notes config does not touch the tracking, so the two analyses pair up
    // note by note and the shift below is the same note read twice.
    REQUIRE(analysis.notes.size() == base.notes.size());

    // Moving the reference adds one constant to every frame's cents, so it
    // survives the median: 1200 * log2(440 / 100) is 2565.4 cents.
    const double expected_shift = static_cast<double>(sonare::constants::kCentsPerOctave) *
                                  std::log2(static_cast<double>(sonare::constants::kA4Hz) /
                                            static_cast<double>(kOtherReference));
    for (size_t i = 0; i < analysis.notes.size(); ++i) {
      const double shift = static_cast<double>(analysis.notes[i].median_cents) -
                           static_cast<double>(base.notes[i].median_cents);
      INFO("note " << i << ": cents " << base.notes[i].median_cents << " against "
                   << analysis.notes[i].median_cents << ", shift " << shift << " against "
                   << expected_shift);
      // A tenth of a cent is three hundred times the float rounding of a
      // 2565-cent quantity and far under any other reading of the field.
      REQUIRE(std::abs(shift - expected_shift) <= 0.1);
    }
  }
}

// --- The length the analysis carries ---------------------------------------

TEST_CASE("the length the spans were derived against is the length the render is given",
          "[polyphony_edit]") {
  // The reason this layer exists. Assembled by hand the chain takes the length
  // twice, and the two calls disagreeing is a render whose edits land on the
  // wrong samples -- which nothing below can detect, the notes being well formed
  // for either length. A hand-built analysis is used so the measurement does not
  // depend on where the extraction put its ridges.
  const sonare::Audio audio = source_audio();
  const sonare::Spectrogram spec = spectrogram_of(audio);
  REQUIRE(spec.n_frames() >= 44);
  const int full = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 2, 40), steady_ridge(kHighHz, 6, 34)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE(masks.notes.size() == 2);

  const std::vector<NoteObject> at_full = make_masked_notes(spec, track, masks, full);
  // A length inside every span and under every span's end, so the spans derived
  // against it are non-empty and clamped rather than unchanged.
  int64_t latest_onset = 0;
  int64_t earliest_offset = at_full.front().offset_sample;
  for (const NoteObject& note : at_full) {
    latest_onset = std::max(latest_onset, note.onset_sample);
    earliest_offset = std::min(earliest_offset, note.offset_sample);
  }
  REQUIRE(latest_onset + 1 < earliest_offset);
  const int shorter = static_cast<int>(0.5 * static_cast<double>(latest_onset + earliest_offset));
  const std::vector<NoteObject> at_shorter = make_masked_notes(spec, track, masks, shorter);
  REQUIRE(at_shorter.size() == at_full.size());

  // The two derivations really differ, or the comparison below is one note set
  // against itself.
  for (size_t i = 0; i < at_full.size(); ++i) {
    INFO("note " << i << ": [" << at_full[i].onset_sample << ", " << at_full[i].offset_sample
                 << ") at " << full << ", [" << at_shorter[i].onset_sample << ", "
                 << at_shorter[i].offset_sample << ") at " << shorter);
    REQUIRE(at_shorter[i].onset_sample == at_full[i].onset_sample);
    REQUIRE(at_shorter[i].offset_sample < at_full[i].offset_sample);
    REQUIRE(at_shorter[i].offset_sample == static_cast<int64_t>(shorter));
  }

  // An identity edit resynthesizes nothing, so both note sets telescope and the
  // mismatch is invisible. That is why the falsifier below carries a gain.
  const sonare::Audio want = spec.to_audio(full);
  require_telescopes(render_masked_notes(spec, masks, at_full, full), want,
                     "spans derived at the render's length", at_full.size());
  require_telescopes(render_masked_notes(spec, masks, at_shorter, full), want,
                     "spans derived at another length, unedited", at_shorter.size());

  const auto with_gain = [](std::vector<NoteObject> notes) {
    notes.front().edit.gain_db = -9.0f;
    notes.back().edit.gain_db = 6.0f;
    return notes;
  };
  const sonare::Audio matched = render_masked_notes(spec, masks, with_gain(at_full), full);
  const sonare::Audio mismatched = render_masked_notes(spec, masks, with_gain(at_shorter), full);
  const Agreement how = agreement(mismatched, matched);
  INFO("two lengths move the render by " << how.worst << " at sample " << how.at << ", peak "
                                         << how.scale);
  REQUIRE(how.scale > kFixturePeakFloor);
  REQUIRE(how.worst > kGuardMargin * kTelescopeRelative * how.scale);

  // And the composition delivers the matched pair. It holds one length, so the
  // mismatched render above is not a call this entry point can make -- which is a
  // property of the signature and is asserted here only as the agreement with the
  // matched hand-built render.
  PolyphonicAnalysis analysis;
  analysis.spectrum = spec;
  analysis.track = track;
  analysis.masks = masks;
  analysis.notes = with_gain(at_full);
  analysis.length = full;
  require_same_audio(render_polyphonic(analysis), matched, "the carried length");
}

// --- The render ------------------------------------------------------------

TEST_CASE("an unedited analysis renders to its own spectrum's round trip", "[polyphony_edit]") {
  // Not the source bit for bit: the STFT round trip's error is neither added to
  // nor removed by this call, so the target is the spectrum the analysis carries
  // inverted once.
  const sonare::Audio audio = source_audio();

  SECTION("an analysis the extraction produced") {
    const PolyphonicAnalysis analysis = analyze_polyphonic(audio);
    REQUIRE(!analysis.notes.empty());
    // The equal split divided something, or every mask is a whole claim and the
    // sum telescopes for a reason the partition never exercised.
    REQUIRE(shared_weights(analysis.masks) > 0);

    const sonare::Audio got = render_polyphonic(analysis);
    REQUIRE(got.size() == static_cast<size_t>(analysis.length));
    REQUIRE(got.sample_rate() == kSampleRate);
    require_telescopes(got, analysis.spectrum.to_audio(analysis.length), "a tracked analysis",
                       analysis.notes.size());
    // The call is its delegate over the members it carries, so the two are the
    // same buffer and not two buffers within a tolerance of each other.
    require_same_audio(
        got,
        render_masked_notes(analysis.spectrum, analysis.masks, analysis.notes, analysis.length),
        "the masked renderer over the same members");
  }

  SECTION("two notes a fifth apart, so some bins are shared and most are not") {
    const sonare::Spectrogram spec = spectrogram_of(audio);
    const PolyphonicAnalysis analysis =
        analysis_over(spec, {steady_ridge(kLowHz, 0, 40), steady_ridge(kHighHz, 4, 32)},
                      static_cast<int>(kSourceSamples));
    REQUIRE(analysis.notes.size() == 2);
    REQUIRE(shared_weights(analysis.masks) > 0);
    require_telescopes(render_polyphonic(analysis), spec.to_audio(analysis.length), "two notes",
                       analysis.notes.size());
  }

  SECTION("three notes at one pitch, so every share is a third") {
    // A third is not exact in binary, so the partition itself carries rounding
    // rather than only the additions.
    const sonare::Spectrogram spec = spectrogram_of(audio);
    const PolyphonicAnalysis analysis = analysis_over(
        spec,
        {steady_ridge(kLowHz, 0, 40), steady_ridge(kLowHz, 0, 40), steady_ridge(kLowHz, 0, 40)},
        static_cast<int>(kSourceSamples));
    REQUIRE(analysis.notes.size() == 3);
    for (const NoteMask& mask : analysis.masks.notes) {
      REQUIRE(!mask.weights.empty());
      for (const std::complex<float>& weight : mask.weights) REQUIRE(weight != 1.0f);
    }
    require_telescopes(render_polyphonic(analysis), spec.to_audio(analysis.length),
                       "three at one pitch", analysis.notes.size());
  }

  SECTION("the length the framing implies, asked for and left unnamed") {
    const sonare::Spectrogram spec = spectrogram_of(audio);
    const int natural = static_cast<int>(spec.to_audio(0).size());
    REQUIRE(natural > 0);
    REQUIRE(natural != static_cast<int>(kSourceSamples));
    const PolyphonicAnalysis analysis =
        analysis_over(spec, {steady_ridge(kLowHz, 0, 36), steady_ridge(kHighHz, 4, 28)}, natural);
    const sonare::Audio got = render_polyphonic(analysis);
    REQUIRE(got.size() == static_cast<size_t>(natural));
    require_telescopes(got, spec.to_audio(natural), "the framing's own length",
                       analysis.notes.size());

    // A carried length of 0 is the same request, which is what the member
    // documents: an analysis a host half-filled renders at the framing's own
    // count rather than being refused, because that is what the call below
    // documents 0 to mean.
    const PolyphonicAnalysis unnamed =
        analysis_over(spec, {steady_ridge(kLowHz, 0, 36), steady_ridge(kHighHz, 4, 28)}, 0);
    REQUIRE(unnamed.length == 0);
    require_same_notes(unnamed.notes, analysis.notes);
    require_same_audio(render_polyphonic(unnamed), got, "a carried length of zero");
  }
}

TEST_CASE("an edit moves the render, and which note carries it matters", "[polyphony_edit]") {
  // What holding the spectrum buys: an edit is re-rendered without analysing the
  // audio again. The identity round trip cannot see whether a note's edit reached
  // that note's own material, so each edit here is measured against the identity
  // and against the same edit on the other note.
  const sonare::Audio audio = source_audio();
  const sonare::Spectrogram spec = spectrogram_of(audio);
  const PolyphonicAnalysis analysis =
      analysis_over(spec, {steady_ridge(kLowHz, 2, 30), steady_ridge(kHighHz, 8, 24)},
                    static_cast<int>(kSourceSamples));
  REQUIRE(analysis.notes.size() == 2);
  // Distinct spans and distinct pitches, or a swap below is the identity.
  REQUIRE(analysis.notes[0].onset_sample != analysis.notes[1].onset_sample);
  REQUIRE(analysis.notes[0].median_hz != analysis.notes[1].median_hz);

  const sonare::Audio identity = render_polyphonic(analysis);
  require_telescopes(identity, spec.to_audio(analysis.length), "every edit identity",
                     analysis.notes.size());
  const double tolerance = kTelescopeRelative * peak_of(identity);
  REQUIRE(peak_of(identity) > kFixturePeakFloor);

  /// @brief How far the render moves when @p at carries @p edit.
  const auto moved_by = [&](size_t at, const std::function<void(NoteObject&)>& edit) {
    PolyphonicAnalysis edited = analysis;
    edit(edited.notes[at]);
    REQUIRE(!edited.notes[at].edit.is_identity());
    return agreement(render_polyphonic(edited), identity);
  };

  // Each edit at both ends of the set, so an edit read off one note is not
  // covered by the other.
  for (const size_t at : {size_t{0}, size_t{1}}) {
    for (const auto& edit : std::vector<std::pair<const char*, std::function<void(NoteObject&)>>>{
             {"a gain of -9 dB", [](NoteObject& note) { note.edit.gain_db = -9.0f; }},
             {"a pitch shift of two semitones",
              [](NoteObject& note) { note.edit.pitch_shift_semitones = 2.0f; }},
             {"a mute", [](NoteObject& note) { note.edit.muted = true; }}}) {
      const Agreement how = moved_by(at, edit.second);
      INFO("note " << at << " with " << edit.first << " moves the render by " << how.worst
                   << " at sample " << how.at << ", tolerance " << tolerance);
      REQUIRE(how.worst > kGuardMargin * tolerance);
    }
  }

  // And the pairing means something. With every edit identity any pairing
  // telescopes, so the falsifier needs an edit: one gain applied through the
  // other note's mask lands on the other note's material.
  PolyphonicAnalysis forward = analysis;
  forward.notes[0].edit.gain_db = -9.0f;
  PolyphonicAnalysis swapped = forward;
  std::swap(swapped.notes[0], swapped.notes[1]);
  const Agreement how = agreement(render_polyphonic(swapped), render_polyphonic(forward));
  INFO("the swapped pairing moves the render by " << how.worst << " at sample " << how.at
                                                  << ", peak " << how.scale);
  REQUIRE(how.scale > kFixturePeakFloor);
  REQUIRE(how.worst > kGuardMargin * kTelescopeRelative * how.scale);
}

// --- No notes --------------------------------------------------------------

TEST_CASE("an analysis with no notes is not an error", "[polyphony_edit]") {
  // Silence, or material whose ridges the tracking drops, leaves the render the
  // residual alone -- which is the whole round trip, every bin being unclaimed.
  const sonare::Audio audio = source_audio();

  SECTION("silence tracks no ridge") {
    const sonare::Audio quiet = silent_audio();
    const PolyphonicAnalysis analysis = analyze_polyphonic(quiet);
    REQUIRE(analysis.track.ridges.empty());
    REQUIRE(analysis.masks.notes.empty());
    REQUIRE(analysis.notes.empty());
    REQUIRE(analysis.length == static_cast<int>(quiet.size()));
    REQUIRE(analysis.spectrum.n_frames() > 1);
    REQUIRE(analysis.masks.n_frames == analysis.spectrum.n_frames());
    REQUIRE(analysis.masks.n_bins == analysis.spectrum.n_bins());

    const sonare::Audio got = render_polyphonic(analysis);
    REQUIRE(got.size() == static_cast<size_t>(analysis.length));
    REQUIRE(got.sample_rate() == kSampleRate);
    // Both buffers are zero, so their agreement carries nothing about the sum.
    // What this section asserts is that no notes is not a rejection and the
    // result still describes the framing; the non-vacuous reading of the same
    // round trip is the section below.
    const sonare::Audio want = analysis.spectrum.to_audio(analysis.length);
    REQUIRE(peak_of(want) == 0.0);
    REQUIRE(agreement(got, want).worst == 0.0);
  }

  SECTION("a minimum duration no ridge of this material survives") {
    // A hundred seconds against half a second of audio, so every ridge the
    // estimation found is dropped while the material itself is untouched -- which
    // is what makes the round trip here a comparison at the fixture's own peak
    // rather than between two silent buffers.
    PolyphonicEditConfig config;
    config.extraction.ridges.min_duration_ms = 100000.0f;
    const PolyphonicAnalysis analysis = analyze_polyphonic(audio, config);
    REQUIRE(analysis.track.ridges.empty());
    REQUIRE(analysis.masks.notes.empty());
    REQUIRE(analysis.notes.empty());
    REQUIRE(analysis.masks.n_frames == analysis.spectrum.n_frames());
    // The estimation still ran: the frames hold voices, and only the tracking
    // dropped them.
    REQUIRE(*std::max_element(analysis.track.polyphony.begin(), analysis.track.polyphony.end()) >
            0);

    require_telescopes(render_polyphonic(analysis), analysis.spectrum.to_audio(analysis.length),
                       "no notes over tonal material", analysis.notes.size());
    // And the same material under the default does track ridges, so the empty
    // result is the configuration rather than the fixture.
    REQUIRE(!analyze_polyphonic(audio).notes.empty());
  }
}

// --- Rejections ------------------------------------------------------------

TEST_CASE("analyze_polyphonic rejects the audio and the configurations its stages reject",
          "[polyphony_edit]") {
  // The contract delegates: every reason the extraction, the mask builder and the
  // measurement throw is a reason this throws. One entry per stage's own ground,
  // so a call that validated only the first of the three is visible.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Audio audio = source_audio();
  // The control the list needs: the same audio under the default config is
  // accepted, so a rejection below is the entry and not the fixture.
  REQUIRE_NOTHROW(analyze_polyphonic(audio));

  const sonare::Audio empty;
  REQUIRE(empty.empty());
  REQUIRE(code_of([&] { return analyze_polyphonic(empty); }) == kInvalid);

  // One frame is the correct framing of a signal this short and still too little
  // to read an instantaneous frequency from.
  const sonare::Audio too_short = audio_of(std::vector<float>(100, 0.01f));
  REQUIRE(sonare::stft_frame_count(too_short.size(), polyphony_stft_defaults()) == 1);
  REQUIRE(code_of([&] { return analyze_polyphonic(too_short); }) == kInvalid);

  // The contract also refuses an audio longer than the carried length can hold.
  // Not written: an int reaches 2 GSamples, so standing at that bound means
  // holding 8 GiB of float, and a case that cannot run asserts nothing.

  struct Named {
    std::string what;
    std::function<void(PolyphonicEditConfig&)> apply;
  };
  const std::vector<Named> broken = {
      {"an STFT of no size",
       [](PolyphonicEditConfig& config) { config.extraction.stft.n_fft = 0; }},
      {"an STFT with no hop",
       [](PolyphonicEditConfig& config) { config.extraction.stft.hop_length = 0; }},
      {"a cent axis with no span",
       [](PolyphonicEditConfig& config) {
         config.extraction.spectrum.max_hz = config.extraction.spectrum.ref_hz;
       }},
      {"no voices per frame",
       [](PolyphonicEditConfig& config) { config.extraction.estimation.max_polyphony = 0; }},
      {"a jump bound of zero",
       [](PolyphonicEditConfig& config) { config.extraction.ridges.max_jump_cents = 0.0f; }},
      {"a negative minimum duration",
       [](PolyphonicEditConfig& config) { config.extraction.ridges.min_duration_ms = -1.0f; }},
      {"no partials to claim", [](PolyphonicEditConfig& config) { config.masks.n_harmonics = 0; }},
      {"more partials than the tables hold",
       [](PolyphonicEditConfig& config) { config.masks.n_harmonics = 129; }},
      {"a claim of no width",
       [](PolyphonicEditConfig& config) { config.masks.claim_lobes = 0.0f; }},
      {"a negative inharmonicity",
       [](PolyphonicEditConfig& config) { config.masks.inharmonicity = -1.0f; }},
      {"a reference pitch of zero",
       [](PolyphonicEditConfig& config) { config.notes.segmenter.reference_hz = 0.0f; }},
      {"a negative reference pitch",
       [](PolyphonicEditConfig& config) { config.notes.segmenter.reference_hz = -440.0f; }},
      {"a voiced threshold of nan",
       [](PolyphonicEditConfig& config) { config.notes.voiced_threshold = kNaN; }},
      {"a segmentation threshold of nan",
       [](PolyphonicEditConfig& config) {
         config.notes.segmenter.segmentation_threshold_cents = kNaN;
       }},
      // Read by nothing in this chain and still refused for being non-finite,
      // which is the difference between a field that is not consulted and one
      // that is harmless to set.
      {"a minimum note length of nan",
       [](PolyphonicEditConfig& config) { config.notes.segmenter.min_note_ms = kNaN; }},
      {"a minimum note length of inf",
       [](PolyphonicEditConfig& config) { config.notes.segmenter.min_note_ms = kInf; }}};

  for (const Named& entry : broken) {
    INFO(entry.what);
    PolyphonicEditConfig config;
    entry.apply(config);
    REQUIRE(code_of([&] { return analyze_polyphonic(audio, config); }) == kInvalid);
  }
}

TEST_CASE("render_polyphonic rejects an analysis whose members stop describing each other",
          "[polyphony_edit]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Audio audio = source_audio();
  const sonare::Spectrogram spec = spectrogram_of(audio);
  const PolyphonicAnalysis analysis =
      analysis_over(spec, {steady_ridge(kLowHz, 2, 30), steady_ridge(kHighHz, 8, 24)},
                    static_cast<int>(kSourceSamples));
  REQUIRE(analysis.notes.size() == 2);
  REQUIRE_NOTHROW(render_polyphonic(analysis));

  for (const NamedAnalysisBreak& entry : analysis_breaks(spec)) {
    INFO(entry.what);
    PolyphonicAnalysis broken = analysis;
    entry.apply(broken);
    REQUIRE(code_of([&] { return render_polyphonic(broken); }) == kInvalid);
  }

  // An empty spectrum, which no accepted analysis carries and which a host can
  // still hand over.
  PolyphonicAnalysis no_spectrum = analysis;
  no_spectrum.spectrum = sonare::Spectrogram();
  REQUIRE(no_spectrum.spectrum.empty());
  REQUIRE(code_of([&] { return render_polyphonic(no_spectrum); }) == kInvalid);

  // A default-constructed analysis carries no shape at all.
  REQUIRE(code_of([&] { return render_polyphonic(PolyphonicAnalysis{}); }) == kInvalid);
}

TEST_CASE("the track is carried and not read on the way out", "[polyphony_edit]") {
  // The asymmetry the contract states: the measurement reads the track and the
  // render does not, so one break is a rejection on the way in and nothing at all
  // on the way out. "Nothing" is a claim about the output and not only about the
  // absence of a throw, so every break here is rendered and compared sample for
  // sample against the untouched render -- a silent acceptance no rejection list
  // can see.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Audio audio = source_audio();
  const sonare::Spectrogram spec = spectrogram_of(audio);
  const int length = static_cast<int>(kSourceSamples);
  const PolyphonicAnalysis analysis =
      analysis_over(spec, {steady_ridge(kLowHz, 2, 30), steady_ridge(kHighHz, 8, 24)}, length);
  REQUIRE(analysis.notes.size() == 2);

  // Edited, so the per-note chain really runs: an identity set resynthesizes
  // nothing and would agree with itself whatever the renderer read.
  PolyphonicAnalysis edited = analysis;
  edited.notes[0].edit.gain_db = -9.0f;
  edited.notes[1].edit.pitch_shift_semitones = 2.0f;
  const sonare::Audio untouched = render_polyphonic(edited);
  REQUIRE(peak_of(untouched) > kFixturePeakFloor);

  struct Named {
    std::string what;
    std::function<void(MultiF0Track&)> apply;
    /// Whether @ref make_masked_notes names this break as one of its own.
    bool measurement_rejects;
  };
  const std::vector<Named> broken = {
      {"no ridges at all", [](MultiF0Track& track) { track.ridges.clear(); }, true},
      {"one ridge dropped", [](MultiF0Track& track) { track.ridges.pop_back(); }, true},
      {"one ridge too many",
       [](MultiF0Track& track) { track.ridges.push_back(track.ridges.back()); }, true},
      {"the first ridge's frames moved",
       [](MultiF0Track& track) { track.ridges.front().frame_start += 3; }, true},
      {"the last ridge carrying no pitch",
       [](MultiF0Track& track) {
         track.ridges.back().f0_hz.assign(track.ridges.back().f0_hz.size(), 0.0f);
       },
       true},
      {"a ridge median that is not its values'",
       [](MultiF0Track& track) { track.ridges.front().median_hz = 1.0f; }, false},
      {"the per-frame polyphony cleared",
       [](MultiF0Track& track) { std::fill(track.polyphony.begin(), track.polyphony.end(), 0); },
       false},
      {"another framing in the track alone",
       [](MultiF0Track& track) {
         track.hop_length = kHopLength / 2;
         track.sample_rate = 16000;
         track.n_frames -= 1;
       },
       false}};

  size_t rejected_in = 0;
  for (const Named& entry : broken) {
    INFO(entry.what);
    PolyphonicAnalysis moved = edited;
    entry.apply(moved.track);
    // Ignored, and ignoring it means the same samples.
    require_same_audio(render_polyphonic(moved), untouched, entry.what + ", rendered");
    // The measurement is the other side: where the contract gives it a ground to
    // stand on, the same analysis does not get past it.
    const sonare::ErrorCode on_the_way_in = code_of(
        [&] { return make_masked_notes(moved.spectrum, moved.track, moved.masks, moved.length); });
    INFO("make_masked_notes answered " << static_cast<int>(on_the_way_in));
    if (entry.measurement_rejects) {
      REQUIRE(on_the_way_in == kInvalid);
      ++rejected_in;
    }
  }
  // Both sides of the asymmetry occurred, or the loop only ever saw one of them.
  REQUIRE(rejected_in > 0);
  REQUIRE(broken.size() > rejected_in);
}

TEST_CASE("a mask whose frames are no longer its ridge's is the measurement's rule",
          "[polyphony_edit]") {
  // Pairing a mask with its ridge is a rule of the measurement and not of the
  // render: one reads the track and the other does not. So one break is refused on
  // the way in and rendered on the way out -- the mask decides which audio the
  // note is, and the span, untouched here, decides where the edit applies to it.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Audio audio = source_audio();
  const sonare::Spectrogram spec = spectrogram_of(audio);
  const int length = static_cast<int>(kSourceSamples);
  const PolyphonicAnalysis analysis =
      analysis_over(spec, {steady_ridge(kLowHz, 2, 30), steady_ridge(kHighHz, 8, 24)}, length);
  REQUIRE(analysis.notes.size() == 2);
  REQUIRE_NOTHROW(render_polyphonic(analysis));
  REQUIRE_NOTHROW(make_masked_notes(spec, analysis.track, analysis.masks, length));

  // Both ends of the set and both directions, so a rule reading one mask is not
  // covered by the other.
  for (const size_t at : {size_t{0}, size_t{1}}) {
    for (const int shift : {1, -1}) {
      INFO("mask " << at << " moved by " << shift << " frames");
      PolyphonicAnalysis broken = analysis;
      broken.masks.notes[at].frame_start += shift;
      REQUIRE(broken.masks.notes[at].frame_start >= 0);
      REQUIRE(broken.masks.notes[at].frame_end() <= spec.n_frames());

      // The measurement refuses it: the amplitude would be measured over frames
      // the pitch curve does not describe.
      REQUIRE(code_of([&] {
                return make_masked_notes(spec, analysis.track, broken.masks, length);
              }) == kInvalid);

      // The render does not, and with every edit identity it still telescopes --
      // the residual being one minus whatever the masks took, however they sit.
      // Which is why the falsifier below needs an edit.
      require_telescopes(render_polyphonic(broken), spec.to_audio(length),
                         "a shifted mask, unedited", broken.notes.size());

      // And the edit lands on other material, so the shift is rendered rather
      // than ignored. Stated as the difference it makes, because "not refused"
      // alone does not separate a mask that was read from one that was not.
      PolyphonicAnalysis edited = analysis;
      edited.notes[at].edit.gain_db = -9.0f;
      PolyphonicAnalysis edited_broken = broken;
      edited_broken.notes[at].edit.gain_db = -9.0f;
      const Agreement how = agreement(render_polyphonic(edited_broken), render_polyphonic(edited));
      INFO("the same gain through the shifted mask moves the render by "
           << how.worst << " at sample " << how.at << ", peak " << how.scale);
      REQUIRE(how.scale > kFixturePeakFloor);
      REQUIRE(how.worst > kGuardMargin * kTelescopeRelative * how.scale);
    }
  }
}

TEST_CASE("an analysis whose inverse carries no samples is a framing error", "[polyphony_edit]") {
  // A single centred frame at length 0: the whole reconstruction is the padding
  // the trim removes, so there is nothing to render into. The contract calls that
  // a framing error rather than an empty result.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  std::vector<float> samples(static_cast<size_t>(kHopLength / 2), 0.0f);
  add_tone(samples, kLowHz, 0.25f, 10);
  const sonare::Spectrogram spec = spectrogram_of(audio_of(std::move(samples)));
  REQUIRE(spec.n_frames() == 1);
  // Not the empty-spectrum rejection: this one has a frame and bins.
  REQUIRE(!spec.empty());
  REQUIRE(spec.n_bins() > 0);
  REQUIRE(spec.to_audio(0).empty());

  const PolyphonicAnalysis nothing = analysis_over(spec, {}, 0);
  REQUIRE(nothing.notes.empty());
  REQUIRE(code_of([&] { return render_polyphonic(nothing); }) == kInvalid);

  // The same spectrum at a length that does carry samples renders, so the
  // rejection is the pair and not the framing on its own.
  PolyphonicAnalysis longer = nothing;
  longer.length = kHopLength;
  const sonare::Audio got = render_polyphonic(longer);
  REQUIRE(got.size() == static_cast<size_t>(kHopLength));
  REQUIRE(got.sample_rate() == kSampleRate);
}

TEST_CASE("render_polyphonic rejects every note the render chain rejects, at either end",
          "[polyphony_edit]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Audio audio = source_audio();
  const sonare::Spectrogram spec = spectrogram_of(audio);
  const PolyphonicAnalysis analysis =
      analysis_over(spec,
                    {steady_ridge(kLowHz, 2, 20), steady_ridge(kHighHz, 10, 16),
                     steady_ridge(2.0f * kLowHz, 20, 14)},
                    static_cast<int>(kSourceSamples));
  REQUIRE(analysis.notes.size() == 3);
  REQUIRE_NOTHROW(render_polyphonic(analysis));

  for (const NamedNoteBreak& entry : note_breaks()) {
    for (const size_t at : {size_t{0}, size_t{2}}) {
      INFO(entry.what << " at note " << at);
      PolyphonicAnalysis broken = analysis;
      entry.apply(broken.notes[at]);
      // The exposed per-note validator and this call are the same contract, so a
      // note reaches both or neither.
      REQUIRE(code_of([&] { note_model::validate_note_for_render(broken.notes[at]); }) == kInvalid);
      REQUIRE(code_of([&] { return render_polyphonic(broken); }) == kInvalid);
    }
  }

  // A note that asks to be resynthesized for nothing is still validated: a
  // reversed span under an identity edit is rejected.
  PolyphonicAnalysis reversed = analysis;
  std::swap(reversed.notes[1].onset_sample, reversed.notes[1].offset_sample);
  REQUIRE(reversed.notes[1].edit.is_identity());
  REQUIRE(code_of([&] { return render_polyphonic(reversed); }) == kInvalid);

  // And the span checks end there. A span moved off the frames its mask spans is
  // not a rejection -- the mask decides which audio the note is and the span
  // decides where the edit applies to it, so the two may disagree. Observable
  // because the gain then lands on other samples, which is asserted as a rendered
  // difference rather than as the absence of a throw.
  PolyphonicAnalysis moved = analysis;
  moved.notes[0].edit.gain_db = -9.0f;
  const sonare::Audio in_place = render_polyphonic(moved);
  moved.notes[0].onset_sample += 6 * kHopLength;
  moved.notes[0].offset_sample += 6 * kHopLength;
  REQUIRE(moved.notes[0].offset_sample <= static_cast<int64_t>(moved.length));
  REQUIRE_NOTHROW(note_model::validate_note_for_render(moved.notes[0]));
  const sonare::Audio elsewhere = render_polyphonic(moved);
  const Agreement how = agreement(elsewhere, in_place);
  INFO("the same gain six hops along moves the render by " << how.worst << " at sample " << how.at
                                                           << ", peak " << how.scale);
  REQUIRE(how.scale > kFixturePeakFloor);
  REQUIRE(how.worst > kGuardMargin * kTelescopeRelative * how.scale);
}

TEST_CASE("a render config is refused where the render chain refuses it", "[polyphony_edit]") {
  // The relative claim: this call answers whatever its delegate answers for a
  // config field, so a layer stricter than the chain it composes is as wrong as
  // one more permissive. Run over four note sets because a config field is read
  // only on the path that needs it -- an identity set resynthesizes nothing, a
  // gain edit resynthesizes without decomposing the pitch curve, only a curve
  // edit reaches the decomposition, and a set with no notes reaches none of
  // them.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Audio audio = source_audio();
  const sonare::Spectrogram spec = spectrogram_of(audio);
  const PolyphonicAnalysis identity =
      analysis_over(spec, {steady_ridge(kLowHz, 2, 30), steady_ridge(kHighHz, 8, 24)},
                    static_cast<int>(kSourceSamples));
  REQUIRE(identity.notes.size() == 2);

  PolyphonicAnalysis gained = identity;
  gained.notes[0].edit.gain_db = 3.0f;
  PolyphonicAnalysis wobbled = identity;
  wobbled.notes[0].edit.vibrato_depth_change = 0.5f;
  REQUIRE(!wobbled.notes[0].f0_hz.values.empty());
  // An analysis with nothing to render, where a config checked only on the
  // rendering path is never looked at.
  const PolyphonicAnalysis nothing = analysis_over(spec, {}, static_cast<int>(kSourceSamples));
  REQUIRE(nothing.notes.empty());
  REQUIRE_NOTHROW(render_polyphonic(nothing));

  size_t refused = 0;
  size_t hoisted = 0;
  for (const NamedConfig& entry : rejected_configs()) {
    const sonare::ErrorCode up_front =
        code_of([&] { note_model::validate_render_config(entry.config); });
    for (const auto& set : std::vector<std::pair<const char*, const PolyphonicAnalysis*>>{
             {"identity notes", &identity},
             {"a gain edit", &gained},
             {"a vibrato edit", &wobbled},
             {"no notes", &nothing}}) {
      const sonare::ErrorCode delegate = code_of([&] {
        return render_masked_notes(set.second->spectrum, set.second->masks, set.second->notes,
                                   set.second->length, entry.config);
      });
      const sonare::ErrorCode got =
          code_of([&] { return render_polyphonic(*set.second, entry.config); });
      INFO(entry.what << ", " << set.first << ": render_masked_notes " << static_cast<int>(delegate)
                      << ", render_polyphonic " << static_cast<int>(got));
      REQUIRE(got == delegate);
      if (got == kInvalid) ++refused;
      // A field the up-front validator refuses is refused whatever the notes
      // are, including the set that would render nothing at all.
      if (up_front == kInvalid) REQUIRE(got == kInvalid);
    }
    if (up_front == kInvalid) ++hoisted;
  }
  // Both the agreement and the up-front arm ran, or the loop asserted nothing.
  INFO("refused " << refused << " of " << 4 * rejected_configs().size() << "; " << hoisted
                  << " fields the up-front validator covers");
  REQUIRE(refused > 0);
  REQUIRE(hoisted > 0);
}
