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
#include "editing/note_model/note_object.h"
#include "editing/note_model/note_renderer.h"
#include "editing/polyphony/masked_renderer.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "util/constants.h"
#include "util/exception.h"

namespace note_model = sonare::editing::note_model;

using sonare::editing::note_model::NoteObject;
using sonare::editing::note_model::NoteRenderConfig;
using namespace sonare::editing::polyphony;

namespace {

/// The framing every fixture, every span and every window below is reasoned in.
/// A quarter of the mask tests' transform, so half a window is 512 samples and a
/// leakage window is a sample range that can be written down rather than a
/// fraction of something.
constexpr int kSampleRate = 16000;
constexpr int kNfft = 1024;
constexpr int kHopLength = 256;
constexpr int kHalfWindow = kNfft / 2;
constexpr size_t kSourceSamples = 8000;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// The two pitches every fixture is built from, a fifth apart so that some
/// partials collide and most do not: the shared bins are where the equal split
/// divides, and a share other than one is what the telescoping has to carry.
constexpr float kLowHz = 330.0f;
constexpr float kHighHz = 495.0f;

/// @brief How far the sum of the per-note inverses may sit from one inverse of
///        the whole, relative to the reconstruction's own peak.
/// @details The header attributes the deviation to the order of the additions.
///          That is not where it comes from: the call runs one inverse transform
///          per note plus one for the residual, and an inverse FFT carries its own
///          rounding, so summing @c k of them deviates from one inverse of the sum
///          by the transforms' error and not by a reassociation of the additions.
///
///          Measured at a peak of 0.886: two notes deviate by 1.788e-07 and three
///          by 2.384e-07, which is 2.02e-07 and 2.69e-07 of the peak -- 1.5 and 2
///          float ulps of it. The same three-note figure comes back at the
///          polyphony default framing, 4096 and 512 at 44.1 kHz, at 2.51e-07 of a
///          0.950 peak, so the deviation is a couple of ulps of the peak rather
///          than a function of the transform size. This is four times the worst of
///          those, which leaves room for a wider set without admitting an error of
///          a different kind: a division error is a fraction of the value it
///          divides -- a half where a third belongs is 0.17 of it, a dropped note
///          all of it -- and sits five orders above this.
///
///          Every comparison that is not the identity measured exactly zero: the
///          residual plus the per-note renders, the empty note set, and all three
///          muted readings. Those are bit-for-bit and are asserted through this
///          bound anyway, since nothing in the contract promises them.
constexpr double kTelescopeRelative = 1e-6;

/// @brief Peak a fixture reconstructs to, below which a comparison is vacuous.
/// @details Asserted next to every relative comparison rather than once in a
///          fixture, so no case can pass on a buffer of zeros.
constexpr double kFixturePeakFloor = 0.2;

/// @brief How far above the tolerance a quantity has to sit to be worth
///        comparing against it.
/// @details Used for the non-vacuity guards: a difference this far above the bound
///          cannot be confused with the bound. The tightest quantity any guard
///          below measures is the leakage in the window above a muted note's span,
///          at 0.0112 against a peak of 0.886 -- 250 times this margin.
constexpr double kGuardMargin = 50.0;

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
/// @details Held throughout rather than gated to the ridges' spans, so a mask
///          over a sub-span still has signal to claim and the material outside a
///          ridge's frames is the residual's rather than silence.
sonare::Audio source_audio() {
  std::vector<float> samples(kSourceSamples, 0.0f);
  add_tone(samples, kLowHz, 0.3f, 10, kSampleRate);
  add_tone(samples, kHighHz, 0.25f, 10, kSampleRate);
  return sonare::Audio::from_vector(std::move(samples), kSampleRate);
}

sonare::Spectrogram source_spectrogram() {
  return sonare::Spectrogram::compute(source_audio(), stft_config());
}

// --- Hand-built tracks, masks and notes ------------------------------------

/// @brief A ridge holding one pitch over [frame_start, frame_start + n_frames).
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

/// @brief A note over [frame_start, frame_end) of a framing.
/// @details The span is those frames in samples and the curves cover the same
///          frames at the framing's cadence, so the note is renderable on its own
///          terms -- including the vibrato and drift edits, which are rejected on
///          a note carrying no usable pitch curve.
NoteObject note_for(int frame_start, int frame_end, float f0_hz, int hop_length, int sample_rate) {
  NoteObject note;
  note.onset_sample = static_cast<int64_t>(frame_start) * hop_length;
  note.offset_sample = static_cast<int64_t>(frame_end) * hop_length;
  note.frame_start = frame_start;
  note.frame_end = frame_end;
  note.median_hz = f0_hz;
  note.median_cents =
      sonare::constants::kCentsPerOctave * std::log2(f0_hz / sonare::constants::kA4Hz);
  const float cadence = static_cast<float>(sample_rate) / static_cast<float>(hop_length);
  const size_t frames = static_cast<size_t>(std::max(0, frame_end - frame_start));
  note.f0_hz.values.assign(frames, f0_hz);
  note.f0_hz.frame_rate_hz = cadence;
  note.f0_hz.frame_offset = frame_start;
  note.amplitude.values.assign(frames, 0.2f);
  note.amplitude.frame_rate_hz = cadence;
  note.amplitude.frame_offset = frame_start;
  note.f0_stability = 1.0f;
  return note;
}

NoteObject note_over(int frame_start, int frame_end, float f0_hz) {
  return note_for(frame_start, frame_end, f0_hz, kHopLength, kSampleRate);
}

/// @brief One note per ridge of @p track, spanning that ridge's frames.
std::vector<NoteObject> notes_for(const MultiF0Track& track) {
  std::vector<NoteObject> notes;
  notes.reserve(track.ridges.size());
  for (const F0Ridge& ridge : track.ridges) {
    notes.push_back(note_for(ridge.frame_start, ridge.frame_end(), ridge.median_hz,
                             track.hop_length, track.sample_rate));
  }
  return notes;
}

// --- Reading a buffer ------------------------------------------------------

/// @brief One note's share of @p spec, inverted at @p length.
/// @details The material the header says the per-note chain is handed, built here
///          out of the two note_mask entry points rather than taken from the
///          renderer, so a case reading it is not reading the renderer back.
sonare::Audio masked_audio(const sonare::Spectrogram& spec, const NoteMask& mask, int length) {
  return apply_note_mask(spec, mask).to_audio(length);
}

sonare::Audio residual_audio(const sonare::Spectrogram& spec, const NoteMaskSet& masks,
                             int length) {
  return residual_spectrum(spec, masks).to_audio(length);
}

/// @brief Elementwise sum of buffers that already share a length and a rate.
sonare::Audio sum_of(const std::vector<sonare::Audio>& parts) {
  REQUIRE(!parts.empty());
  std::vector<float> total(parts.front().size(), 0.0f);
  for (const sonare::Audio& part : parts) {
    REQUIRE(part.size() == total.size());
    REQUIRE(part.sample_rate() == parts.front().sample_rate());
    for (size_t i = 0; i < total.size(); ++i) total[i] += part[i];
  }
  return sonare::Audio::from_vector(std::move(total), parts.front().sample_rate());
}

/// @brief Worst absolute deviation of @p got from @p want over [lo, hi), and the
///        largest magnitude of @p want there.
struct Agreement {
  double worst = 0.0;
  double scale = 0.0;
  size_t at = 0;
};

Agreement agreement(const sonare::Audio& got, const sonare::Audio& want, size_t lo, size_t hi) {
  REQUIRE(got.size() == want.size());
  REQUIRE(lo < hi);
  REQUIRE(hi <= want.size());
  Agreement result;
  for (size_t i = lo; i < hi; ++i) {
    const double difference = std::abs(static_cast<double>(got[i]) - static_cast<double>(want[i]));
    if (difference > result.worst) {
      result.worst = difference;
      result.at = i;
    }
    result.scale = std::max(result.scale, std::abs(static_cast<double>(want[i])));
  }
  return result;
}

Agreement agreement(const sonare::Audio& got, const sonare::Audio& want) {
  return agreement(got, want, 0, want.size());
}

double peak_in(const sonare::Audio& audio, size_t lo, size_t hi) {
  REQUIRE(lo < hi);
  REQUIRE(hi <= audio.size());
  double highest = 0.0;
  for (size_t i = lo; i < hi; ++i) {
    highest = std::max(highest, std::abs(static_cast<double>(audio[i])));
  }
  return highest;
}

/// @brief Asserts the telescoping identity over the whole buffer.
/// @details The non-vacuity guard sits here, at the assertion, and not in a
///          fixture: a buffer of zeros agrees with a buffer of zeros perfectly,
///          so the scale the tolerance is relative to is asserted first.
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

/// @brief The note every rejected variant below is one broken field away from.
NoteObject valid_note() { return note_over(4, 16, kLowHz); }

struct NamedNote {
  std::string what;
  NoteObject note;
};

/// @brief Every note @ref note_model::validate_note_for_render rejects.
/// @details One list, so a renderer covering only the variants a case happened to
///          spell out separately cannot pass. Each entry is the valid note with
///          exactly one field broken, so a rejection is attributable to it.
///
///          The monophonic contract's "non-finite or non-positive edit field" is
///          read as non-finiteness for every float field and non-positivity for
///          @c time_stretch_ratio alone: every other field has documented
///          non-positive values -- a gain of 0 is the default, -1 flattens the
///          vibrato, a negative @c time_offset_samples moves the note earlier.
std::vector<NamedNote> rejected_notes() {
  std::vector<NamedNote> broken;
  {
    NoteObject note = valid_note();
    note.offset_sample = note.onset_sample;
    broken.push_back({"an empty span", note});
  }
  {
    NoteObject note = valid_note();
    std::swap(note.onset_sample, note.offset_sample);
    broken.push_back({"a reversed span", note});
  }
  {
    NoteObject note = valid_note();
    note.onset_sample = -1;
    broken.push_back({"a span starting one sample before zero", note});
  }
  {
    NoteObject note = valid_note();
    note.onset_sample = -kHopLength;
    note.offset_sample = kHopLength;
    broken.push_back({"a span reaching well before zero", note});
  }
  const std::vector<std::pair<const char*, float>> unusable = {
      {"nan", kNaN}, {"inf", kInf}, {"-inf", -kInf}};
  for (const auto& bad : unusable) {
    NoteObject note = valid_note();
    note.edit.pitch_shift_semitones = bad.second;
    broken.push_back({std::string("a pitch shift of ") + bad.first, note});
  }
  for (const auto& bad : unusable) {
    NoteObject note = valid_note();
    note.edit.gain_db = bad.second;
    broken.push_back({std::string("a gain of ") + bad.first, note});
  }
  for (const auto& bad : unusable) {
    NoteObject note = valid_note();
    note.edit.formant_shift_semitones = bad.second;
    broken.push_back({std::string("a formant shift of ") + bad.first, note});
  }
  for (const auto& bad : unusable) {
    NoteObject note = valid_note();
    note.edit.vibrato_depth_change = bad.second;
    broken.push_back({std::string("a vibrato change of ") + bad.first, note});
  }
  for (const auto& bad : unusable) {
    NoteObject note = valid_note();
    note.edit.drift_change = bad.second;
    broken.push_back({std::string("a drift change of ") + bad.first, note});
  }
  for (const auto& bad : unusable) {
    NoteObject note = valid_note();
    note.edit.time_stretch_ratio = bad.second;
    broken.push_back({std::string("a time stretch ratio of ") + bad.first, note});
  }
  for (const float bad : {0.0f, -1.0f, -0.5f}) {
    NoteObject note = valid_note();
    note.edit.time_stretch_ratio = bad;
    broken.push_back({"a non-positive time stretch ratio " + std::to_string(bad), note});
  }
  for (const auto& bad : unusable) {
    NoteObject note = valid_note();
    note.edit.amplitude_envelope = {1.0f, bad.second, 1.0f};
    broken.push_back({std::string("an envelope value of ") + bad.first, note});
  }
  for (const float bad : {-1e-6f, -0.5f, -100.0f}) {
    NoteObject note = valid_note();
    note.edit.amplitude_envelope = {1.0f, bad, 1.0f};
    broken.push_back({"a negative envelope value " + std::to_string(bad), note});
  }
  {
    NoteObject note = valid_note();
    note.f0_hz.values.clear();
    note.edit.vibrato_depth_change = 0.5f;
    broken.push_back({"a vibrato edit with no pitch curve", note});
  }
  {
    NoteObject note = valid_note();
    note.f0_hz.values.clear();
    note.edit.drift_change = 0.5f;
    broken.push_back({"a drift edit with no pitch curve", note});
  }
  {
    // Unvoiced frames are how a track spells no measurement, so a curve of zeros
    // carries no usable pitch either.
    NoteObject note = valid_note();
    note.f0_hz.values.assign(note.f0_hz.values.size(), 0.0f);
    note.median_hz = 0.0f;
    note.edit.vibrato_depth_change = 0.5f;
    broken.push_back({"a vibrato edit on an unvoiced curve", note});
  }
  return broken;
}

struct NamedConfig {
  std::string what;
  NoteRenderConfig config;
};

/// @brief Configs whose fields sit outside what the render chain accepts.
/// @details The two fields are checked at different times by contract, so the list
///          carries both and the case reading it asserts which is which rather
///          than assuming they behave alike. @c fade_ms is what
///          @ref note_model::validate_render_config covers -- not finite or
///          negative -- and @c decomposition is validated where it is read, which
///          is a note carrying a curve edit.
///
///          A @c fade_ms of 0 is deliberately absent: it is a hard cut and is
///          accepted here, and only the percussive event chain's identically
///          shaped check refuses one.
std::vector<NamedConfig> rejected_configs() {
  std::vector<NamedConfig> broken;
  const std::vector<std::pair<const char*, float>> unusable = {
      {"nan", kNaN}, {"inf", kInf}, {"-inf", -kInf}};
  for (const auto& bad : unusable) {
    NoteRenderConfig config;
    config.fade_ms = bad.second;
    broken.push_back({std::string("a fade of ") + bad.first, config});
  }
  for (const float bad : {-1e-6f, -5.0f, -1000.0f}) {
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

struct NamedSetBreak {
  std::string what;
  std::function<void(NoteMaskSet&)> apply;
};

/// @brief Every way a set stops describing the spectrogram it is used with.
/// @details Written as mutations rather than as finished sets, so the same list
///          runs against a set of several masks and against a set of none.
std::vector<NamedSetBreak> set_breaks(const sonare::Spectrogram& spec) {
  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  std::vector<NamedSetBreak> broken;
  for (const int bad : {bins + 1, bins - 1, 0, -1}) {
    broken.push_back(
        {"n_bins " + std::to_string(bad), [bad](NoteMaskSet& set) { set.n_bins = bad; }});
  }
  for (const int bad : {frames + 1, frames - 1, 0, -1}) {
    broken.push_back(
        {"n_frames " + std::to_string(bad), [bad](NoteMaskSet& set) { set.n_frames = bad; }});
  }
  for (const int bad : {kHopLength + 1, kHopLength / 2, 0, -1}) {
    broken.push_back(
        {"hop_length " + std::to_string(bad), [bad](NoteMaskSet& set) { set.hop_length = bad; }});
  }
  for (const int bad : {kSampleRate + 1, 44100, 0, -1}) {
    broken.push_back(
        {"sample_rate " + std::to_string(bad), [bad](NoteMaskSet& set) { set.sample_rate = bad; }});
  }
  return broken;
}

struct NamedMaskBreak {
  std::string what;
  std::function<void(NoteMask&)> apply;
};

/// @brief Every way one mask's own shape stops being readable.
/// @details Each of these is a read outside the mask's own arrays, or outside the
///          spectrogram, rather than a merely odd input -- which is why
///          note_mask.h has every consumer check the shape before allocating
///          against it, and the renderer is one of those consumers.
std::vector<NamedMaskBreak> mask_breaks(const sonare::Spectrogram& spec) {
  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  std::vector<NamedMaskBreak> broken;
  broken.push_back({"frame_offset one short of n_frames + 1",
                    [](NoteMask& mask) { mask.frame_offset.pop_back(); }});
  broken.push_back({"frame_offset one long",
                    [](NoteMask& mask) { mask.frame_offset.push_back(mask.frame_offset.back()); }});
  broken.push_back({"frame_offset does not start at zero",
                    [](NoteMask& mask) { mask.frame_offset.front() = 1; }});
  broken.push_back({"frame_offset descends",
                    [](NoteMask& mask) { mask.frame_offset[1] = mask.frame_offset[2] + 1; }});
  broken.push_back({"the last offset reaches past the arrays", [](NoteMask& mask) {
                      mask.frame_offset.back() = static_cast<int32_t>(mask.bins.size()) + 1;
                    }});
  broken.push_back({"bins shorter than weights", [](NoteMask& mask) { mask.bins.pop_back(); }});
  broken.push_back({"weights shorter than bins", [](NoteMask& mask) { mask.weights.pop_back(); }});
  broken.push_back({"a negative frame count", [](NoteMask& mask) { mask.n_frames = -1; }});
  broken.push_back({"a bin past the spectrogram", [bins](NoteMask& mask) {
                      mask.bins[mask.bins.size() / 2] = static_cast<int32_t>(bins);
                    }});
  broken.push_back(
      {"a negative bin", [](NoteMask& mask) { mask.bins[mask.bins.size() / 2] = -1; }});
  broken.push_back({"a span starting before zero", [](NoteMask& mask) { mask.frame_start = -1; }});
  broken.push_back({"a span reaching past the last frame",
                    [frames](NoteMask& mask) { mask.frame_start = frames - mask.n_frames + 1; }});
  // A weight of zero, and one that is not finite, break the total and the
  // residual without any shape being wrong, which note_mask.h states is the
  // worse outcome of the two and so a rejection of its own.
  broken.push_back(
      {"a zero weight", [](NoteMask& mask) { mask.weights[mask.weights.size() / 2] = 0.0f; }});
  broken.push_back({"a nan weight", [](NoteMask& mask) {
                      mask.weights[mask.weights.size() / 2] = std::complex<float>(kNaN, 0.0f);
                    }});
  broken.push_back({"an infinite weight", [](NoteMask& mask) {
                      mask.weights[mask.weights.size() / 2] = std::complex<float>(kInf, 0.0f);
                    }});
  broken.push_back({"a default-constructed mask", [](NoteMask& mask) { mask = NoteMask{}; }});
  return broken;
}

}  // namespace

// --- The telescoping -------------------------------------------------------

TEST_CASE("with every edit identity the result is the spectrogram's own round trip",
          "[polyphony_render]") {
  const sonare::Spectrogram spec = source_spectrogram();
  REQUIRE(spec.n_frames() > 28);
  const int natural = static_cast<int>(spec.to_audio(0).size());
  REQUIRE(natural > 0);
  // Two distinct lengths to run the identity at, which is also what says the
  // parameter is not being ignored in favour of the framing's own count.
  REQUIRE(natural != static_cast<int>(kSourceSamples));

  SECTION("two notes a fifth apart, so some bins are shared and most are not") {
    const MultiF0Track track =
        track_over(spec, {steady_ridge(kLowHz, 0, 28), steady_ridge(kHighHz, 4, 20)});
    const NoteMaskSet masks = build_note_masks(spec, track);
    REQUIRE(masks.notes.size() == 2);
    const std::vector<NoteObject> notes = notes_for(track);
    for (const NoteObject& note : notes) REQUIRE(note.edit.is_identity());

    // The shared bins are real, or the section only exercises a whole claim.
    size_t shared = 0;
    for (const NoteMask& mask : masks.notes) {
      for (const std::complex<float>& weight : mask.weights) {
        if (weight != 1.0f) ++shared;
      }
    }
    REQUIRE(shared > 0);

    for (const int length : {0, natural, static_cast<int>(kSourceSamples)}) {
      INFO("length " << length);
      require_telescopes(render_masked_notes(spec, masks, notes, length), spec.to_audio(length),
                         "two notes");
    }
  }

  SECTION("three notes at one pitch, so every share is a third") {
    // A half is exact in binary and a third is not, so this is the division that
    // makes the spectral partition itself carry rounding rather than only the
    // additions the header names.
    const MultiF0Track track = track_over(
        spec,
        {steady_ridge(kLowHz, 0, 28), steady_ridge(kLowHz, 0, 28), steady_ridge(kLowHz, 0, 28)});
    const NoteMaskSet masks = build_note_masks(spec, track);
    REQUIRE(masks.notes.size() == 3);
    for (const NoteMask& mask : masks.notes) {
      REQUIRE(!mask.weights.empty());
      for (const std::complex<float>& weight : mask.weights) REQUIRE(weight != 1.0f);
    }

    require_telescopes(render_masked_notes(spec, masks, notes_for(track), natural),
                       spec.to_audio(natural), "three at one pitch");
  }

  SECTION("the order of the notes moves the result by no more than the identity's own slack") {
    // The header states the identity "up to the order of the additions", so both
    // orderings sit inside that slack and therefore within twice it of each
    // other. A renderer writing rather than accumulating would not.
    const MultiF0Track track =
        track_over(spec, {steady_ridge(kLowHz, 0, 28), steady_ridge(kHighHz, 4, 20),
                          steady_ridge(2.0f * kLowHz, 8, 16)});
    const NoteMaskSet masks = build_note_masks(spec, track);
    const std::vector<NoteObject> notes = notes_for(track);
    REQUIRE(masks.notes.size() == 3);

    NoteMaskSet reversed = masks;
    std::reverse(reversed.notes.begin(), reversed.notes.end());
    std::vector<NoteObject> reversed_notes = notes;
    std::reverse(reversed_notes.begin(), reversed_notes.end());

    const sonare::Audio forward = render_masked_notes(spec, masks, notes, natural);
    const sonare::Audio backward = render_masked_notes(spec, reversed, reversed_notes, natural);
    require_telescopes(forward, spec.to_audio(natural), "forward order");
    require_telescopes(backward, spec.to_audio(natural), "reversed order");

    const Agreement how = agreement(backward, forward);
    INFO("order difference " << how.worst << " at sample " << how.at << ", peak " << how.scale);
    REQUIRE(how.scale > kFixturePeakFloor);
    REQUIRE(how.worst <= 2.0 * kTelescopeRelative * how.scale);
  }

  SECTION("the identity is not an artefact of this framing") {
    // The polyphony defaults, at the rate the mask tests are written in.
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
    const int wide_length = static_cast<int>(other.to_audio(0).size());
    // Every span stays inside the buffer the notes are rendered over, which is
    // the only arrangement the contract speaks about.
    for (const NoteObject& note : notes_for(track)) {
      REQUIRE(note.offset_sample <= wide_length);
    }

    require_telescopes(render_masked_notes(other, masks, notes_for(track), wide_length),
                       other.to_audio(wide_length), "polyphony defaults");
  }
}

TEST_CASE("the result is the residual plus each note rendered over its own masked resynthesis",
          "[polyphony_render]") {
  // The mechanism the header states, assembled here out of the note_mask entry
  // points and the monophonic renderer: one inverse per mask, render_notes once
  // per note over that inverse, and the residual added to the sum. The identity
  // above cannot tell a renderer that edits the mixture from one that edits each
  // note's own audio, because with an identity edit the two agree.
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 2, 24), steady_ridge(kHighHz, 6, 18)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE(masks.notes.size() == 2);

  std::vector<NoteObject> notes = notes_for(track);
  // Non-identity, so the per-note chain really runs and the decomposition is not
  // being read off two untouched copies of the same material.
  notes[0].edit.pitch_shift_semitones = 2.0f;
  notes[1].edit.gain_db = -4.0f;
  REQUIRE(!notes[0].edit.is_identity());
  REQUIRE(!notes[1].edit.is_identity());

  const NoteRenderConfig config;
  std::vector<sonare::Audio> parts;
  parts.push_back(residual_audio(spec, masks, length));
  for (size_t i = 0; i < masks.notes.size(); ++i) {
    parts.push_back(
        note_model::render_notes(masked_audio(spec, masks.notes[i], length), {notes[i]}, config));
  }
  const sonare::Audio expected = sum_of(parts);
  const sonare::Audio got = render_masked_notes(spec, masks, notes, length, config);

  REQUIRE(got.size() == expected.size());
  const Agreement how = agreement(got, expected);
  INFO("decomposition: worst " << how.worst << " at sample " << how.at << ", peak " << how.scale);
  REQUIRE(how.scale > kFixturePeakFloor);
  REQUIRE(how.worst <= kTelescopeRelative * how.scale);

  // And the edits changed something, so the agreement is not two copies of the
  // untouched round trip agreeing with each other.
  const Agreement edited = agreement(got, spec.to_audio(length));
  INFO("the edit moved the output by " << edited.worst << ", peak " << edited.scale);
  REQUIRE(edited.scale > kFixturePeakFloor);
  REQUIRE(edited.worst > kGuardMargin * kTelescopeRelative * edited.scale);
}

TEST_CASE("an empty note set leaves the spectrogram's own round trip", "[polyphony_render]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const NoteMaskSet masks = build_note_masks(spec, track_over(spec, {}));
  REQUIRE(masks.notes.empty());
  REQUIRE(masks.n_bins == spec.n_bins());
  REQUIRE(masks.n_frames == spec.n_frames());

  // Nothing is claimed, so the residual is the whole spectrum and the output is
  // the residual alone -- which is what the contract's "residual plus every note
  // rendered" comes to when there are no notes to add.
  for (const int length : {0, static_cast<int>(kSourceSamples)}) {
    INFO("length " << length);
    require_telescopes(render_masked_notes(spec, masks, {}, length), spec.to_audio(length),
                       "no notes");
  }
}

// --- Overlap ---------------------------------------------------------------

TEST_CASE("overlapping spans are accepted where the monophonic chain rejects them",
          "[polyphony_render]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 4, 12), steady_ridge(kHighHz, 8, 12)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> notes = notes_for(track);
  REQUIRE(notes.size() == 2);

  // The overlap is real, or the case checks the disjoint arrangement twice.
  REQUIRE(notes[1].onset_sample < notes[0].offset_sample);
  REQUIRE(notes[0].onset_sample < notes[1].offset_sample);

  // The monophonic chain checks source spans for disjointness and these are not
  // disjoint, which is the one check the header says does not carry over.
  REQUIRE(code_of([&] { return note_model::render_notes(source_audio(), notes); }) ==
          sonare::ErrorCode::InvalidParameter);

  REQUIRE_NOTHROW(render_masked_notes(spec, masks, notes, length));
  // And the overlapping set still telescopes, so accepting the overlap is not
  // accepting a different sum.
  require_telescopes(render_masked_notes(spec, masks, notes, length), spec.to_audio(length),
                     "overlapping spans");
}

TEST_CASE("two notes moved onto each other add rather than overwrite", "[polyphony_render]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  // Disjoint source spans, so the arrangement is one the monophonic chain would
  // accept too and the only thing under test is what happens where the two land.
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 4, 8), steady_ridge(kHighHz, 16, 8)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> identity = notes_for(track);
  REQUIRE(identity.size() == 2);
  REQUIRE(identity[0].offset_sample < identity[1].onset_sample);

  // Both moved to start at frame 10, so the two rendered spans coincide exactly.
  constexpr int64_t kLanding = 10 * kHopLength;
  const int64_t first_offset = kLanding - identity[0].onset_sample;
  const int64_t second_offset = kLanding - identity[1].onset_sample;
  std::vector<NoteObject> first_moved = identity;
  first_moved[0].edit.time_offset_samples = first_offset;
  std::vector<NoteObject> second_moved = identity;
  second_moved[1].edit.time_offset_samples = second_offset;
  std::vector<NoteObject> both_moved = identity;
  both_moved[0].edit.time_offset_samples = first_offset;
  both_moved[1].edit.time_offset_samples = second_offset;

  const sonare::Audio none = render_masked_notes(spec, masks, identity, length);
  const sonare::Audio one = render_masked_notes(spec, masks, first_moved, length);
  const sonare::Audio two = render_masked_notes(spec, masks, second_moved, length);
  const sonare::Audio both = render_masked_notes(spec, masks, both_moved, length);

  // Each note's contribution depends on that note alone, so moving both is
  // moving each: the prediction is the two single moves less the baseline they
  // share. Overwriting in the landing window drops one of the two terms, which
  // is the size of the guards below and not of the tolerance.
  std::vector<float> predicted(none.size(), 0.0f);
  for (size_t i = 0; i < predicted.size(); ++i) predicted[i] = one[i] + two[i] - none[i];
  const sonare::Audio prediction = sonare::Audio::from_vector(std::move(predicted), kSampleRate);

  const size_t landing_lo = static_cast<size_t>(kLanding);
  const size_t landing_hi = landing_lo + static_cast<size_t>(8 * kHopLength);
  REQUIRE(landing_hi <= none.size());

  const Agreement how = agreement(both, prediction);
  const double moved_first = agreement(one, none, landing_lo, landing_hi).worst;
  const double moved_second = agreement(two, none, landing_lo, landing_hi).worst;
  INFO("worst " << how.worst << " at sample " << how.at << ", peak " << how.scale
                << "; the first note moved by " << moved_first << ", the second by "
                << moved_second);
  REQUIRE(how.scale > kFixturePeakFloor);
  const double tolerance = kTelescopeRelative * how.scale;
  // Both notes really arrive in the landing window, so dropping either would
  // show in the comparison that follows.
  REQUIRE(moved_first > kGuardMargin * tolerance);
  REQUIRE(moved_second > kGuardMargin * tolerance);
  REQUIRE(how.worst <= tolerance);
}

// --- A muted note ----------------------------------------------------------

TEST_CASE("a muted note drops its span and keeps what its mask reaches outside it",
          "[polyphony_render]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  // One mask over frames [8, 20), whose inverse reaches half a window either
  // side of the frames it spans: samples [8 * hop - n_fft/2, 19 * hop + n_fft/2),
  // which is [1536, 5376) at this framing.
  constexpr int kMaskFirstFrame = 8;
  constexpr int kMaskLastFrame = 20;
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, kMaskFirstFrame, kMaskLastFrame - kMaskFirstFrame)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE(masks.notes.size() == 1);
  REQUIRE(masks.notes[0].frame_start == kMaskFirstFrame);
  REQUIRE(masks.notes[0].frame_end() == kMaskLastFrame);

  const std::vector<NoteObject> notes = {note_over(kMaskFirstFrame, kMaskLastFrame, kLowHz)};
  const size_t span_lo = static_cast<size_t>(notes[0].onset_sample);
  const size_t span_hi = static_cast<size_t>(notes[0].offset_sample);
  const size_t reach_lo = static_cast<size_t>(kMaskFirstFrame * kHopLength - kHalfWindow);
  const size_t reach_hi = static_cast<size_t>((kMaskLastFrame - 1) * kHopLength + kHalfWindow);
  REQUIRE(reach_lo < span_lo);
  REQUIRE(span_hi < reach_hi);
  REQUIRE(reach_hi < static_cast<size_t>(length));

  // The cross-fade at an edited note's edges is 5 ms, which is 80 samples here,
  // so every window below stands clear of a span edge by more than that.
  constexpr size_t kEdgeMargin = 112;
  const size_t mid_lo = span_lo + kEdgeMargin;
  const size_t mid_hi = span_hi - kEdgeMargin;
  const size_t leak_low_hi = span_lo - kEdgeMargin;
  const size_t leak_high_lo = span_hi + kEdgeMargin;

  const sonare::Audio note_share = masked_audio(spec, masks.notes[0], length);
  const sonare::Audio residual = residual_audio(spec, masks, length);
  const sonare::Audio all = render_masked_notes(spec, masks, notes, length);

  // One scale for the whole case: the deviation between a sum of inverses and an
  // inverse of a sum is set by the transform's own input scale, so it does not
  // shrink in a window where the signal happens to be quiet.
  const double scale = peak_in(all, 0, all.size());
  const double tolerance = kTelescopeRelative * scale;
  REQUIRE(scale > kFixturePeakFloor);

  // The mask reaches at most half a window either side of the frames it spans,
  // so outside that its inverse is not small but absent.
  REQUIRE(peak_in(note_share, 0, reach_lo) == 0.0);
  REQUIRE(peak_in(note_share, reach_hi, note_share.size()) == 0.0);

  std::vector<NoteObject> muted = notes;
  muted[0].edit.muted = true;
  const sonare::Audio dropped = render_masked_notes(spec, masks, muted, length);

  SECTION("the span is dropped") {
    const Agreement how = agreement(dropped, residual, mid_lo, mid_hi);
    const double was_there = peak_in(note_share, mid_lo, mid_hi);
    INFO("worst " << how.worst << " at sample " << how.at << ", the note's own share there "
                  << was_there << ", tolerance " << tolerance);
    // There was something to drop: the mask's share of these samples is far
    // larger than the tolerance, so agreeing with the residual alone is a
    // statement about the note having gone rather than about silence.
    REQUIRE(was_there > kGuardMargin * tolerance);
    REQUIRE(how.worst <= tolerance);
  }

  SECTION("the mask's leakage outside the span is kept") {
    for (const std::pair<size_t, size_t>& window :
         {std::make_pair(reach_lo, leak_low_hi), std::make_pair(leak_high_lo, reach_hi)}) {
      INFO("window [" << window.first << ", " << window.second << ")");
      const Agreement kept = agreement(dropped, all, window.first, window.second);
      const double leakage = peak_in(note_share, window.first, window.second);
      const double from_residual = agreement(dropped, residual, window.first, window.second).worst;
      INFO("worst against the full render " << kept.worst << ", leakage " << leakage
                                            << ", distance from the residual alone "
                                            << from_residual << ", tolerance " << tolerance);
      // The leakage is large enough that keeping it and dropping it are
      // different answers, which is what makes the comparison mean anything.
      REQUIRE(leakage > kGuardMargin * tolerance);
      REQUIRE(kept.worst <= tolerance);
      // Stated the other way round as well: this is not the residual alone.
      REQUIRE(from_residual > 0.5 * leakage);
    }
  }

  SECTION("a muted note's other edit fields do not apply") {
    std::vector<NoteObject> loud = muted;
    loud[0].edit.gain_db = 12.0f;
    loud[0].edit.pitch_shift_semitones = 7.0f;
    const sonare::Audio still_dropped = render_masked_notes(spec, masks, loud, length);

    std::vector<NoteObject> audible = loud;
    audible[0].edit.muted = false;
    const sonare::Audio unmuted = render_masked_notes(spec, masks, audible, length);

    const Agreement how = agreement(still_dropped, dropped, mid_lo, mid_hi);
    const double unmuted_moves = agreement(unmuted, dropped, mid_lo, mid_hi).worst;
    INFO("worst " << how.worst << ", the same edit unmuted moves " << unmuted_moves
                  << ", tolerance " << tolerance);
    // The same fields on an unmuted note change the output, so their having no
    // effect here is the mute rather than the fields being inert.
    REQUIRE(unmuted_moves > kGuardMargin * tolerance);
    REQUIRE(how.worst <= tolerance);
  }
}

// --- Length ----------------------------------------------------------------

TEST_CASE("length is the output's own count and zero takes the framing's", "[polyphony_render]") {
  const sonare::Spectrogram spec = source_spectrogram();
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 0, 28), steady_ridge(kHighHz, 4, 20)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> notes = notes_for(track);

  const size_t natural = spec.to_audio(0).size();
  REQUIRE(natural > 0);
  REQUIRE(natural != kSourceSamples);

  const sonare::Audio automatic = render_masked_notes(spec, masks, notes, 0);
  REQUIRE(automatic.size() == natural);
  REQUIRE(automatic.sample_rate() == spec.sample_rate());

  const sonare::Audio explicit_source =
      render_masked_notes(spec, masks, notes, static_cast<int>(kSourceSamples));
  REQUIRE(explicit_source.size() == kSourceSamples);
  REQUIRE(explicit_source.sample_rate() == spec.sample_rate());

  // Naming the framing's own count is the same request as naming none, so the
  // two routes to one length agree.
  const sonare::Audio explicit_natural =
      render_masked_notes(spec, masks, notes, static_cast<int>(natural));
  REQUIRE(explicit_natural.size() == natural);
  const Agreement how = agreement(explicit_natural, automatic);
  INFO("worst " << how.worst << " at sample " << how.at << ", peak " << how.scale);
  REQUIRE(how.scale > kFixturePeakFloor);
  REQUIRE(how.worst <= kTelescopeRelative * how.scale);
}

// --- Rejections ------------------------------------------------------------

TEST_CASE("render_masked_notes rejects every note the monophonic chain rejects",
          "[polyphony_render]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track = track_over(spec, {steady_ridge(kLowHz, 4, 12)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  REQUIRE_NOTHROW(render_masked_notes(spec, masks, {valid_note()}, length));
  REQUIRE_NOTHROW(note_model::validate_note_for_render(valid_note()));

  for (const NamedNote& entry : rejected_notes()) {
    INFO(entry.what);
    // The exposed per-note validator and the renderer are the same contract, so
    // a note either reaches both or neither.
    REQUIRE(code_of([&] { note_model::validate_note_for_render(entry.note); }) == kInvalid);
    REQUIRE(code_of([&] { return render_masked_notes(spec, masks, {entry.note}, length); }) ==
            kInvalid);
  }
}

TEST_CASE("a rejected note anywhere in the set is rejected", "[polyphony_render]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track = track_over(
      spec,
      {steady_ridge(kLowHz, 2, 10), steady_ridge(kHighHz, 12, 8), steady_ridge(kLowHz, 22, 6)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> good = notes_for(track);
  REQUIRE(good.size() == 3);
  REQUIRE_NOTHROW(render_masked_notes(spec, masks, good, length));

  // The same list at the first and the last position: a renderer validating one
  // note, or stopping once it has something to render, covers neither.
  for (const NamedNote& entry : rejected_notes()) {
    for (const size_t at : {size_t{0}, size_t{2}}) {
      INFO(entry.what << " at index " << at);
      std::vector<NoteObject> notes = good;
      notes[at] = entry.note;
      REQUIRE(code_of([&] { return render_masked_notes(spec, masks, notes, length); }) == kInvalid);
    }
  }
}

TEST_CASE("a config is refused up front or where it is read, and never only here",
          "[polyphony_render]") {
  // Two contracts at once. The relative one: the masked renderer throws on
  // "anything render_notes rejects about a config field", so the two agree
  // whatever the field -- a renderer stricter than its own oracle is as wrong as
  // one more permissive. The absolute one: whatever
  // validate_render_config refuses is refused before any inverse transform, so it
  // is refused for every note set including one that would render nothing.
  //
  // The two fields sit on opposite sides of that line by contract. fade_ms is what
  // the up-front validator covers; decomposition is validated where it is read,
  // which is a note carrying a curve edit, so a set with no such note never
  // reaches it. Asserted as the documented split rather than as one rule, because
  // a renderer hoisting the cutoff too would pass a single-rule case and break the
  // monophonic agreement.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 4, 12), steady_ridge(kHighHz, 8, 10)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> notes = notes_for(track);
  const sonare::Audio audio = source_audio();

  size_t refused = 0;
  size_t hoisted = 0;
  size_t late = 0;
  for (const NamedConfig& entry : rejected_configs()) {
    const sonare::ErrorCode up_front =
        code_of([&] { note_model::validate_render_config(entry.config); });

    // Three note sets, because a config field is only read on the path that needs
    // it: an identity set resynthesizes nothing, a gain edit resynthesizes without
    // decomposing the pitch curve, and only a vibrato edit reaches the
    // decomposition the cutoff belongs to.
    std::vector<NoteObject> gained = notes;
    gained[0].edit.gain_db = 3.0f;
    std::vector<NoteObject> wobbled = notes;
    wobbled[0].edit.vibrato_depth_change = 0.5f;
    REQUIRE(!wobbled[0].f0_hz.values.empty());

    const std::vector<std::pair<const char*, const std::vector<NoteObject>*>> sets = {
        {"identity notes", &notes}, {"a gain edit", &gained}, {"a vibrato edit", &wobbled}};
    size_t refused_here = 0;
    for (const auto& set : sets) {
      const sonare::ErrorCode monophonic = code_of(
          [&] { return note_model::render_notes(audio, {(*set.second)[0]}, entry.config); });
      const sonare::ErrorCode polyphonic = code_of(
          [&] { return render_masked_notes(spec, masks, *set.second, length, entry.config); });
      INFO(entry.what << ", " << set.first << ": validate_render_config "
                      << static_cast<int>(up_front) << ", render_notes "
                      << static_cast<int>(monophonic) << ", render_masked_notes "
                      << static_cast<int>(polyphonic));
      REQUIRE(polyphonic == monophonic);
      // A config the up-front validator refuses is refused whatever the notes
      // are, which is what validating it with no note to apply it to comes to.
      if (up_front == kInvalid) REQUIRE(polyphonic == kInvalid);
      if (polyphonic == kInvalid) {
        ++refused;
        ++refused_here;
      }
    }
    if (up_front == kInvalid) {
      // Refused for all three sets, so the field really is checked before a note
      // is reached rather than happening to be read by each of them.
      REQUIRE(refused_here == sets.size());
      ++hoisted;
    } else if (refused_here > 0) {
      // Refused by the set that reads it and not by the two that do not, which is
      // the documented late check and not a gap.
      REQUIRE(refused_here == 1);
      ++late;
    }
  }
  INFO("refused " << refused << " of " << 3 * rejected_configs().size() << "; " << hoisted
                  << " fields hoisted, " << late << " checked late");
  // Both sides of the split are populated, or one of the two branches above is an
  // assertion nothing ever ran.
  REQUIRE(hoisted > 0);
  REQUIRE(late > 0);
}

TEST_CASE("the config is validated with no note to apply it to", "[polyphony_render]") {
  // The case that would have passed for the wrong reason while the only thing
  // reading the config was the per-note render: nothing here has a note, so a
  // config checked on the rendering path alone is never looked at.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const NoteMaskSet empty_set = build_note_masks(spec, track_over(spec, {}));
  REQUIRE(empty_set.notes.empty());
  // The same call with a good config renders, so a rejection below is the config
  // and not the empty set.
  REQUIRE_NOTHROW(render_masked_notes(spec, empty_set, {}, length));

  const MultiF0Track track = track_over(spec, {steady_ridge(kLowHz, 4, 12)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> notes = notes_for(track);
  REQUIRE_NOTHROW(render_masked_notes(spec, masks, notes, length));

  size_t checked = 0;
  for (const NamedConfig& entry : rejected_configs()) {
    if (code_of([&] { note_model::validate_render_config(entry.config); }) != kInvalid) continue;
    ++checked;
    INFO(entry.what);
    // Both arms, because the defect this replaces was the two disagreeing: a
    // fade_ms of NaN that succeeded on an empty set and threw on a populated one.
    REQUIRE(code_of([&] {
              return render_masked_notes(spec, empty_set, {}, length, entry.config);
            }) == kInvalid);
    REQUIRE(code_of([&] {
              return render_masked_notes(spec, masks, notes, length, entry.config);
            }) == kInvalid);
  }
  // The list held something the up-front validator refuses, or the loop ran no
  // assertions at all.
  INFO("checked " << checked << " of " << rejected_configs().size());
  REQUIRE(checked > 0);
}

TEST_CASE("a spec and length whose inverse carries no samples is a framing error",
          "[polyphony_render]") {
  // A single centred frame at length 0: the whole reconstruction is the padding
  // the trim removes, so there is nothing to render into. The contract calls that
  // a framing error rather than an empty result.
  //
  // It says the rejection happens once against the residual rather than per note.
  // That has no observable outside the throw -- one exception is one exception
  // however many times the call declined to invert -- so the assertion here is the
  // throw, and the "once" is not asserted at all rather than through something
  // weaker wearing its name.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  // Shorter than one hop, so the framing holds exactly one frame.
  std::vector<float> samples(static_cast<size_t>(kHopLength / 2), 0.0f);
  add_tone(samples, kLowHz, 0.3f, 10, kSampleRate);
  const sonare::Spectrogram spec = sonare::Spectrogram::compute(
      sonare::Audio::from_vector(std::move(samples), kSampleRate), stft_config());
  REQUIRE(spec.n_frames() == 1);
  // Not the empty-spectrogram rejection: this one has a frame and bins.
  REQUIRE(!spec.empty());
  REQUIRE(spec.n_bins() > 0);
  // The framing really does invert to nothing, which is what makes the rejection
  // about this pair rather than about a length the test picked.
  REQUIRE(spec.to_audio(0).empty());

  const NoteMaskSet empty_set = build_note_masks(spec, track_over(spec, {}));
  const MultiF0Track track = track_over(spec, {steady_ridge(kLowHz, 0, 1)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> notes = notes_for(track);
  REQUIRE(notes.size() == 1);

  // With nothing to render and with a note to render, because the contract puts
  // the check on the residual and a renderer that only noticed while laying out a
  // note's buffer would pass the first.
  REQUIRE(code_of([&] { return render_masked_notes(spec, empty_set, {}, 0); }) == kInvalid);
  REQUIRE(code_of([&] { return render_masked_notes(spec, masks, notes, 0); }) == kInvalid);

  // The same spectrogram at a length that does carry samples renders, so the
  // rejection is the pair and not the framing on its own.
  const sonare::Audio rendered = render_masked_notes(spec, empty_set, {}, kHopLength);
  REQUIRE(rendered.size() == static_cast<size_t>(kHopLength));
  REQUIRE(rendered.sample_rate() == kSampleRate);
}

TEST_CASE("render_masked_notes rejects a mask set that does not describe the spectrogram",
          "[polyphony_render]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 4, 12), steady_ridge(kHighHz, 8, 10)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> notes = notes_for(track);
  const NoteMaskSet empty_set = build_note_masks(spec, track_over(spec, {}));
  REQUIRE_NOTHROW(render_masked_notes(spec, masks, notes, length));
  REQUIRE_NOTHROW(render_masked_notes(spec, empty_set, {}, length));

  // The same breaks against a set of two masks and against a set of none, so a
  // shape check that only runs when there is a note to render fails the second.
  for (const NamedSetBreak& entry : set_breaks(spec)) {
    INFO(entry.what);
    NoteMaskSet described = masks;
    entry.apply(described);
    REQUIRE(code_of([&] { return render_masked_notes(spec, described, notes, length); }) ==
            kInvalid);

    NoteMaskSet nothing = empty_set;
    entry.apply(nothing);
    REQUIRE(code_of([&] { return render_masked_notes(spec, nothing, {}, length); }) == kInvalid);
  }

  // A set carrying no shape at all, which is the default-constructed one.
  const NoteMaskSet unset;
  REQUIRE(unset.n_bins == 0);
  REQUIRE(unset.n_frames == 0);
  REQUIRE(code_of([&] { return render_masked_notes(spec, unset, {}, length); }) == kInvalid);
}

TEST_CASE("render_masked_notes rejects a mask whose own shape is broken", "[polyphony_render]") {
  // Only that it throws. The contract puts this check late on purpose -- a mask
  // reaching outside the spectrogram is found where it is applied, so a set
  // unrenderable because of its second mask pays the first note's inverse before
  // it throws -- so nothing here asserts an ordering, and the second position
  // below is about a check reading one mask and not the other rather than about
  // when it reads either.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 4, 12), steady_ridge(kHighHz, 8, 10)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> notes = notes_for(track);
  REQUIRE(masks.notes.size() == 2);
  REQUIRE(masks.notes[0].n_frames > 4);
  REQUIRE(masks.notes[0].bins.size() > 4);
  REQUIRE(masks.notes[1].bins.size() > 4);

  // Each break applied to the first mask and to the second, so a check reading
  // only one of them is not covered by the other.
  for (const NamedMaskBreak& entry : mask_breaks(spec)) {
    for (const size_t at : {size_t{0}, size_t{1}}) {
      INFO(entry.what << " in mask " << at);
      NoteMaskSet broken = masks;
      entry.apply(broken.notes[at]);
      REQUIRE(code_of([&] { return render_masked_notes(spec, broken, notes, length); }) ==
              kInvalid);
    }
  }
}

TEST_CASE("render_masked_notes rejects a note count that is not the mask count",
          "[polyphony_render]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 4, 12), steady_ridge(kHighHz, 8, 10)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const NoteMaskSet empty_set = build_note_masks(spec, track_over(spec, {}));
  REQUIRE(notes_for(track).size() == 2);

  // Both directions and both sides of the pair, because a check written as one
  // inequality passes half of them.
  for (const size_t count : {size_t{0}, size_t{1}, size_t{3}, size_t{8}}) {
    INFO("two masks and " << count << " notes");
    const std::vector<NoteObject> wrong(count, valid_note());
    REQUIRE(code_of([&] { return render_masked_notes(spec, masks, wrong, length); }) == kInvalid);
  }
  for (const size_t count : {size_t{1}, size_t{2}}) {
    INFO("no masks and " << count << " notes");
    const std::vector<NoteObject> wrong(count, valid_note());
    REQUIRE(code_of([&] { return render_masked_notes(spec, empty_set, wrong, length); }) ==
            kInvalid);
  }
}

TEST_CASE("render_masked_notes rejects a negative length and an empty spectrogram",
          "[polyphony_render]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 4, 12), steady_ridge(kHighHz, 8, 10)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const NoteMaskSet empty_set = build_note_masks(spec, track_over(spec, {}));
  const std::vector<NoteObject> notes = notes_for(track);

  for (const int bad : {-1, -kHopLength, -static_cast<int>(kSourceSamples)}) {
    INFO("length " << bad);
    REQUIRE(code_of([&] { return render_masked_notes(spec, masks, notes, bad); }) == kInvalid);
    // And with nothing to render, so the bound is the parameter's rather than a
    // by-product of allocating a note's buffer.
    REQUIRE(code_of([&] { return render_masked_notes(spec, empty_set, {}, bad); }) == kInvalid);
  }

  const sonare::Spectrogram nothing;
  REQUIRE(nothing.empty());
  REQUIRE(code_of([&] { return render_masked_notes(nothing, masks, notes, 0); }) == kInvalid);
  REQUIRE(code_of([&] { return render_masked_notes(nothing, empty_set, {}, 0); }) == kInvalid);
}

TEST_CASE("every note is validated, identity or not", "[polyphony_render]") {
  // The header also promises the validation runs before any inverse transform.
  // That ordering has no observable outside the throw: the call returns one
  // buffer or none, holds no state and reports no progress, so a renderer
  // inverting first and rejecting after is indistinguishable from one rejecting
  // first. What is observable is the other half of the same clause -- a note the
  // renderer would otherwise never touch is still rejected.
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = source_spectrogram();
  const int length = static_cast<int>(kSourceSamples);
  const MultiF0Track track =
      track_over(spec, {steady_ridge(kLowHz, 2, 12), steady_ridge(kHighHz, 16, 10)});
  const NoteMaskSet masks = build_note_masks(spec, track);
  const std::vector<NoteObject> good = notes_for(track);
  REQUIRE_NOTHROW(render_masked_notes(spec, masks, good, length));

  // A reversed span under an identity edit: nothing about this note asks to be
  // resynthesized, and it is still rejected.
  std::vector<NoteObject> reversed = good;
  std::swap(reversed[1].onset_sample, reversed[1].offset_sample);
  REQUIRE(reversed[1].edit.is_identity());
  REQUIRE(code_of([&] { return render_masked_notes(spec, masks, reversed, length); }) == kInvalid);

  // And an empty span under an identity edit, which is the other span defect a
  // renderer skipping identity notes would pass.
  std::vector<NoteObject> empty_span = good;
  empty_span[0].offset_sample = empty_span[0].onset_sample;
  REQUIRE(empty_span[0].edit.is_identity());
  REQUIRE(code_of([&] { return render_masked_notes(spec, masks, empty_span, length); }) ==
          kInvalid);
}
