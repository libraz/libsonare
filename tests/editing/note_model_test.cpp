#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_object.h"
#include "editing/note_model/note_renderer.h"
#include "editing/pitch_editor/f0_provider.h"
#include "util/constants.h"
#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using sonare::editing::pitch_editor::F0Track;
using namespace sonare::editing::note_model;

namespace {

constexpr int kSampleRate = 16000;
constexpr int kHopLength = 160;  // 100 frames/s, 10 ms per frame
constexpr float kFrameRateHz = 100.0f;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr size_t kNoMismatch = static_cast<size_t>(-1);

std::vector<float> sine(float frequency_hz, float amplitude, int samples) {
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) {
    output[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz *
                                                static_cast<double>(i) / kSampleRate));
  }
  return output;
}

sonare::Audio tone(float frequency_hz, float amplitude, int samples) {
  return sonare::Audio::from_vector(sine(frequency_hz, amplitude, samples), kSampleRate);
}

F0Track voiced_track(float frequency_hz, int frames) {
  F0Track track;
  track.sample_rate = kSampleRate;
  track.hop_length = kHopLength;
  track.f0_hz.assign(static_cast<size_t>(frames), frequency_hz);
  track.voiced.assign(static_cast<size_t>(frames), true);
  track.voiced_prob.assign(static_cast<size_t>(frames), 1.0f);
  return track;
}

/// @brief A well-formed note with constant curves, independent of the extractor.
NoteObject make_note(int64_t onset_sample, int64_t offset_sample, float frequency_hz) {
  NoteObject note;
  note.onset_sample = onset_sample;
  note.offset_sample = offset_sample;
  note.frame_start = static_cast<int>(onset_sample / kHopLength);
  note.frame_end = static_cast<int>(offset_sample / kHopLength);
  note.median_hz = frequency_hz;
  note.median_cents =
      sonare::constants::kCentsPerOctave * std::log2(frequency_hz / sonare::constants::kA4Hz);
  // Reversed spans are a validation case, so the curve length is clamped rather
  // than wrapping around.
  const size_t frames = static_cast<size_t>(std::max(0, note.frame_end - note.frame_start));
  note.f0_hz.values.assign(frames, frequency_hz);
  note.f0_hz.frame_rate_hz = kFrameRateHz;
  note.f0_hz.frame_offset = note.frame_start;
  note.amplitude.values.assign(frames, 0.25f);
  note.amplitude.frame_rate_hz = kFrameRateHz;
  note.amplitude.frame_offset = note.frame_start;
  note.f0_stability = 1.0f;
  return note;
}

/// @brief Index of the first differing sample in [lo, hi), or kNoMismatch.
size_t first_mismatch(const sonare::Audio& a, const sonare::Audio& b, size_t lo, size_t hi) {
  const size_t end = std::min(hi, std::min(a.size(), b.size()));
  for (size_t i = lo; i < end; ++i) {
    if (a[i] != b[i]) return i;
  }
  return kNoMismatch;
}

size_t first_mismatch(const sonare::Audio& a, const sonare::Audio& b) {
  if (a.size() != b.size()) return 0;
  return first_mismatch(a, b, 0, a.size());
}

double rms(const sonare::Audio& audio, size_t lo, size_t hi) {
  const size_t end = std::min(hi, audio.size());
  double acc = 0.0;
  for (size_t i = lo; i < end; ++i) {
    acc += static_cast<double>(audio[i]) * static_cast<double>(audio[i]);
  }
  return std::sqrt(acc / static_cast<double>(std::max<size_t>(1, end - lo)));
}

float peak(const sonare::Audio& audio, size_t lo, size_t hi) {
  const size_t end = std::min(hi, audio.size());
  float highest = 0.0f;
  for (size_t i = lo; i < end; ++i) {
    highest = std::max(highest, std::abs(audio[i]));
  }
  return highest;
}

float median_of(std::vector<float> values) {
  REQUIRE(!values.empty());
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

}  // namespace

// --- Acceptance: an identity edit set is a bit-exact pass-through ---------

TEST_CASE("render_notes reproduces the input bit for bit when every edit is identity",
          "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);

  // Hand-built spans: adjacent, half-open, covering the whole buffer.
  std::vector<NoteObject> notes = {make_note(0, 3200, 440.0f), make_note(3200, 6400, 440.0f)};
  for (const NoteObject& note : notes) REQUIRE(note.edit.is_identity());
  REQUIRE(first_mismatch(render_notes(audio, notes), audio) == kNoMismatch);

  // The same property for extractor-produced spans, which is the shape a caller
  // actually holds between an extract and a render.
  const std::vector<NoteObject> extracted = extract_notes(audio, voiced_track(440.0f, 40));
  REQUIRE(!extracted.empty());
  REQUIRE(first_mismatch(render_notes(audio, extracted), audio) == kNoMismatch);
}

TEST_CASE("render_notes reproduces the input bit for bit for an empty note set", "[note_model]") {
  const sonare::Audio audio = tone(330.0f, 0.4f, 4800);
  const sonare::Audio rendered = render_notes(audio, {});

  REQUIRE(rendered.size() == audio.size());
  REQUIRE(rendered.sample_rate() == audio.sample_rate());
  REQUIRE(first_mismatch(rendered, audio) == kNoMismatch);
}

// --- NoteEdit::is_identity ------------------------------------------------

TEST_CASE("NoteEdit is identity only when every field is at its neutral value", "[note_model]") {
  REQUIRE(NoteEdit{}.is_identity());

  NoteEdit pitch;
  pitch.pitch_shift_semitones = 0.01f;
  REQUIRE_FALSE(pitch.is_identity());

  NoteEdit gain;
  gain.gain_db = -0.5f;
  REQUIRE_FALSE(gain.is_identity());

  NoteEdit offset;
  offset.time_offset_samples = 1;
  REQUIRE_FALSE(offset.is_identity());
  offset.time_offset_samples = -1;
  REQUIRE_FALSE(offset.is_identity());

  NoteEdit stretch;
  stretch.time_stretch_ratio = 1.0001f;
  REQUIRE_FALSE(stretch.is_identity());

  NoteEdit muted;
  muted.muted = true;
  REQUIRE(muted.pitch_shift_semitones == 0.0f);
  REQUIRE_FALSE(muted.is_identity());
}

TEST_CASE("NoteEdit reports a non-finite field as non-identity", "[note_model]") {
  // A non-finite field must reach the renderer's validation rather than being
  // waved through as "changes nothing".
  for (const float value : {kNaN, kInf, -kInf}) {
    NoteEdit pitch;
    pitch.pitch_shift_semitones = value;
    REQUIRE_FALSE(pitch.is_identity());

    NoteEdit gain;
    gain.gain_db = value;
    REQUIRE_FALSE(gain.is_identity());

    NoteEdit stretch;
    stretch.time_stretch_ratio = value;
    REQUIRE_FALSE(stretch.is_identity());
  }
}

// --- extract_notes --------------------------------------------------------

TEST_CASE("extract_notes segments a synthetic signal into the expected notes", "[note_model]") {
  // One sustained note.
  const sonare::Audio single = tone(440.0f, 0.5f, 6400);
  const std::vector<NoteObject> one = extract_notes(single, voiced_track(440.0f, 40));
  REQUIRE(one.size() == 1);
  REQUIRE(one[0].onset_sample == 0);
  REQUIRE(one[0].offset_sample == 6400);
  REQUIRE(one[0].edit.is_identity());

  // Two notes: 440 Hz then 587.33 Hz (a fifth up, far past the 50-cent
  // segmentation threshold), splitting at frame 20 / sample 3200.
  std::vector<float> samples = sine(440.0f, 0.5f, 3200);
  const std::vector<float> upper = sine(587.33f, 0.25f, 3200);
  samples.insert(samples.end(), upper.begin(), upper.end());
  const sonare::Audio pair = sonare::Audio::from_vector(std::move(samples), kSampleRate);

  F0Track two_note_track = voiced_track(440.0f, 40);
  for (size_t i = 20; i < 40; ++i) two_note_track.f0_hz[i] = 587.33f;
  const std::vector<NoteObject> two = extract_notes(pair, two_note_track);
  REQUIRE(two.size() == 2);
  REQUIRE(two[0].frame_start == 0);
  REQUIRE(two[0].frame_end == 20);
  REQUIRE(two[1].frame_start == 20);
  REQUIRE(two[1].frame_end == 40);
  REQUIRE(two[0].onset_sample == 0);
  REQUIRE(two[0].offset_sample == 3200);
  REQUIRE(two[1].onset_sample == 3200);
  REQUIRE(two[1].offset_sample == 6400);

  // The amplitude curve is linear, so the second note (half the amplitude of the
  // first) reads half as loud. A ratio keeps this independent of the analysis
  // window the RMS is taken over.
  const float loud = median_of(two[0].amplitude.values);
  const float quiet = median_of(two[1].amplitude.values);
  REQUIRE(quiet > 0.0f);
  REQUIRE_THAT(loud / quiet, WithinAbs(2.0f, 0.2f));

  // Nothing voiced, nothing to extract.
  const sonare::Audio silence =
      sonare::Audio::from_vector(std::vector<float>(6400, 0.0f), kSampleRate);
  F0Track unvoiced = voiced_track(0.0f, 40);
  unvoiced.voiced.assign(40, false);
  unvoiced.voiced_prob.assign(40, 0.0f);
  REQUIRE(extract_notes(silence, unvoiced).empty());
}

TEST_CASE("extract_notes gives both curves one value per frame of the note's span",
          "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  F0Track track = voiced_track(440.0f, 40);
  for (size_t i = 20; i < 40; ++i) track.f0_hz[i] = 587.33f;

  const std::vector<NoteObject> notes = extract_notes(audio, track);
  REQUIRE(notes.size() == 2);
  for (const NoteObject& note : notes) {
    const size_t span_frames = static_cast<size_t>(note.frame_end - note.frame_start);
    REQUIRE(span_frames > 0);
    REQUIRE(note.f0_hz.values.size() == span_frames);
    REQUIRE(note.amplitude.values.size() == span_frames);
    // frame_offset places values[0] in the source track.
    REQUIRE(note.f0_hz.frame_offset == note.frame_start);
    REQUIRE(note.amplitude.frame_offset == note.frame_start);
    REQUIRE_THAT(note.f0_hz.frame_rate_hz, WithinAbs(kFrameRateHz, 0.001f));
    REQUIRE_THAT(note.amplitude.frame_rate_hz, WithinAbs(kFrameRateHz, 0.001f));
    for (const float amplitude : note.amplitude.values) {
      REQUIRE(std::isfinite(amplitude));
      REQUIRE(amplitude >= 0.0f);
    }
  }
}

TEST_CASE("extract_notes measures the note's median pitch and RMS level", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  const std::vector<NoteObject> notes = extract_notes(audio, voiced_track(440.0f, 40));

  REQUIRE(notes.size() == 1);
  REQUIRE_THAT(notes[0].median_hz, WithinAbs(440.0f, 1.0f));
  // Cents are measured against the extractor's reference, which defaults to A4.
  REQUIRE_THAT(notes[0].median_cents, WithinAbs(0.0f, 5.0f));
  // Linear RMS of a 0.5-amplitude sine, not its peak and not a dB value.
  REQUIRE_THAT(median_of(notes[0].amplitude.values), WithinAbs(0.354f, 0.1f));

  const sonare::Audio lower = tone(220.0f, 0.5f, 6400);
  const std::vector<NoteObject> low = extract_notes(lower, voiced_track(220.0f, 40));
  REQUIRE(low.size() == 1);
  REQUIRE_THAT(low[0].median_hz, WithinAbs(220.0f, 1.0f));
  REQUIRE_THAT(low[0].median_cents, WithinAbs(-1200.0f, 5.0f));
}

TEST_CASE("extract_notes cuts a note at an unvoiced gap rather than spanning it", "[note_model]") {
  // Why a note carries no voiced fraction: an unvoiced frame ends the run, so
  // every span the segmenter emits is fully voiced and the figure would be 1
  // for every note it can produce.
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  F0Track gapped = voiced_track(440.0f, 40);
  for (size_t i = 15; i < 20; ++i) {
    gapped.voiced[i] = false;
  }

  const std::vector<NoteObject> notes = extract_notes(audio, gapped);
  REQUIRE(notes.size() == 2);
  REQUIRE(notes[0].frame_start == 0);
  REQUIRE(notes[0].frame_end == 15);
  REQUIRE(notes[1].frame_start == 20);
  REQUIRE(notes[1].frame_end == 40);
}

TEST_CASE("extract_notes scores a steady pitch as more stable than a vibrato", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);

  const std::vector<NoteObject> steady = extract_notes(audio, voiced_track(440.0f, 40));
  REQUIRE(steady.size() == 1);

  // 5 Hz vibrato of +-40 cents: wide enough to move the deviation statistic,
  // narrow enough to stay inside the 50-cent segmentation threshold so the span
  // remains one note. Phase starts at zero so the first frame (the segmenter's
  // reference) sits at the centre pitch.
  F0Track vibrato_track = voiced_track(440.0f, 40);
  for (size_t i = 0; i < vibrato_track.f0_hz.size(); ++i) {
    const double phase = sonare::constants::kTwoPiD * 5.0 * static_cast<double>(i) / kFrameRateHz;
    const double cents = 40.0 * std::sin(phase);
    vibrato_track.f0_hz[i] = 440.0f * static_cast<float>(std::pow(2.0, cents / 1200.0));
  }
  const std::vector<NoteObject> vibrato = extract_notes(audio, vibrato_track);
  REQUIRE(vibrato.size() == 1);

  for (const float stability : {steady[0].f0_stability, vibrato[0].f0_stability}) {
    REQUIRE(std::isfinite(stability));
    REQUIRE(stability >= 0.0f);
    REQUIRE(stability <= 1.0f);
  }
  // A constant F0 has zero deviation, which is the top of the scale.
  REQUIRE(steady[0].f0_stability > 0.99f);
  REQUIRE(vibrato[0].f0_stability < steady[0].f0_stability);
  // Discriminating, not merely ordered: the vibrato must be visibly less steady.
  REQUIRE(vibrato[0].f0_stability < 0.9f);
}

TEST_CASE("extract_notes derives voicing from voiced_prob when the track has no flags",
          "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);

  F0Track prob_only = voiced_track(440.0f, 40);
  prob_only.voiced.clear();
  prob_only.voiced_prob.assign(40, 0.9f);

  NoteExtractorConfig config;
  config.voiced_threshold = 0.5f;
  const std::vector<NoteObject> notes = extract_notes(audio, prob_only, config);
  REQUIRE(notes.size() == 1);
  REQUIRE(notes[0].onset_sample == 0);
  REQUIRE(notes[0].offset_sample == 6400);

  // The same probabilities under a threshold above them leave nothing voiced.
  config.voiced_threshold = 0.95f;
  REQUIRE(extract_notes(audio, prob_only, config).empty());

  // Probabilities straddling a mid threshold: the low half drops out, so the
  // extracted span covers the high half only.
  F0Track split = prob_only;
  for (size_t i = 0; i < 20; ++i) split.voiced_prob[i] = 0.1f;
  config.voiced_threshold = 0.5f;
  const std::vector<NoteObject> tail = extract_notes(audio, split, config);
  REQUIRE(tail.size() == 1);
  REQUIRE(tail[0].frame_start == 20);
  REQUIRE(tail[0].frame_end == 40);
}

TEST_CASE("extract_notes rejects malformed audio, track and config", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  const F0Track track = voiced_track(440.0f, 40);

  const sonare::Audio empty_audio;
  REQUIRE(empty_audio.empty());
  REQUIRE_THROWS_AS(extract_notes(empty_audio, track), sonare::SonareException);

  F0Track empty_track;
  empty_track.sample_rate = kSampleRate;
  empty_track.hop_length = kHopLength;
  REQUIRE_THROWS_AS(extract_notes(audio, empty_track), sonare::SonareException);

  // frame_rate() is zero for either half of the cadence rule being unusable.
  F0Track no_hop = voiced_track(440.0f, 40);
  no_hop.hop_length = 0;
  REQUIRE_THAT(no_hop.frame_rate(), WithinAbs(0.0f, 0.0f));
  REQUIRE_THROWS_AS(extract_notes(audio, no_hop), sonare::SonareException);

  F0Track no_rate = voiced_track(440.0f, 40);
  no_rate.sample_rate = 0;
  no_rate.hop_length = 0;
  REQUIRE_THROWS_AS(extract_notes(audio, no_rate), sonare::SonareException);

  F0Track negative_rate = voiced_track(440.0f, 40);
  negative_rate.hop_length = -160;
  REQUIRE_THROWS_AS(extract_notes(audio, negative_rate), sonare::SonareException);

  // Neither voicing source present: there is nothing to segment on.
  F0Track no_voicing = voiced_track(440.0f, 40);
  no_voicing.voiced.clear();
  no_voicing.voiced_prob.clear();
  REQUIRE_THROWS_AS(extract_notes(audio, no_voicing), sonare::SonareException);

  for (const float bad : {kNaN, kInf}) {
    NoteExtractorConfig threshold;
    threshold.segmenter.segmentation_threshold_cents = bad;
    REQUIRE_THROWS_AS(extract_notes(audio, track, threshold), sonare::SonareException);

    NoteExtractorConfig min_note;
    min_note.segmenter.min_note_ms = bad;
    REQUIRE_THROWS_AS(extract_notes(audio, track, min_note), sonare::SonareException);

    NoteExtractorConfig reference;
    reference.segmenter.reference_hz = bad;
    REQUIRE_THROWS_AS(extract_notes(audio, track, reference), sonare::SonareException);

    NoteExtractorConfig voiced;
    voiced.voiced_threshold = bad;
    REQUIRE_THROWS_AS(extract_notes(audio, track, voiced), sonare::SonareException);
  }

  REQUIRE_NOTHROW(extract_notes(audio, track));
}

// --- render_notes ---------------------------------------------------------

TEST_CASE("render_notes applies a gain edit inside the note and leaves the rest untouched",
          "[note_model]") {
  // 0.25 amplitude leaves room for the +6 dB case to stay below full scale.
  const sonare::Audio audio = tone(440.0f, 0.25f, 8000);
  constexpr size_t kOnset = 1920;
  constexpr size_t kOffset = 6080;
  // Interior windows and outside windows both stay clear of the edge
  // cross-fades, which are 5 ms (80 samples at this rate) by default.
  constexpr size_t kMargin = 320;

  const double source_rms = rms(audio, kOnset + kMargin, kOffset - kMargin);
  REQUIRE(source_rms > 0.0);

  auto render_with_gain = [&](float gain_db) {
    NoteObject note = make_note(kOnset, kOffset, 440.0f);
    note.edit.gain_db = gain_db;
    return render_notes(audio, {note});
  };

  const sonare::Audio quieter = render_with_gain(-6.0206f);
  REQUIRE(quieter.size() == audio.size());
  REQUIRE_THAT(rms(quieter, kOnset + kMargin, kOffset - kMargin) / source_rms,
               WithinAbs(0.5, 0.03));

  const sonare::Audio louder = render_with_gain(6.0206f);
  REQUIRE_THAT(rms(louder, kOnset + kMargin, kOffset - kMargin) / source_rms, WithinAbs(2.0, 0.12));

  // Everything outside the edited span passes through untouched.
  for (const sonare::Audio* rendered : {&quieter, &louder}) {
    REQUIRE(first_mismatch(audio, *rendered, 0, kOnset - kMargin) == kNoMismatch);
    REQUIRE(first_mismatch(audio, *rendered, kOffset + kMargin, audio.size()) == kNoMismatch);
  }
}

TEST_CASE("render_notes silences a muted note and leaves the rest untouched", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 8000);
  constexpr size_t kOnset = 1920;
  constexpr size_t kOffset = 6080;
  constexpr size_t kMargin = 320;

  NoteObject note = make_note(kOnset, kOffset, 440.0f);
  note.edit.muted = true;
  REQUIRE_FALSE(note.edit.is_identity());
  const sonare::Audio rendered = render_notes(audio, {note});

  REQUIRE(rendered.size() == audio.size());
  REQUIRE(peak(rendered, kOnset + kMargin, kOffset - kMargin) < 1e-6f);
  REQUIRE(peak(audio, kOnset + kMargin, kOffset - kMargin) > 0.4f);
  REQUIRE(first_mismatch(audio, rendered, 0, kOnset - kMargin) == kNoMismatch);
  REQUIRE(first_mismatch(audio, rendered, kOffset + kMargin, audio.size()) == kNoMismatch);
}

TEST_CASE("render_notes keeps the input's length and sample rate for every edit", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.4f, 8000);

  auto rendered_with = [&](const NoteEdit& edit) {
    NoteObject note = make_note(1920, 6080, 440.0f);
    note.edit = edit;
    return render_notes(audio, {note});
  };

  std::vector<NoteEdit> edits;
  NoteEdit later;
  later.time_offset_samples = 1200;
  edits.push_back(later);
  NoteEdit earlier;
  earlier.time_offset_samples = -1200;
  edits.push_back(earlier);
  // Pushed clean past both ends: truncated, not resized and not an error.
  NoteEdit past_end;
  past_end.time_offset_samples = 100000;
  edits.push_back(past_end);
  NoteEdit before_start;
  before_start.time_offset_samples = -100000;
  edits.push_back(before_start);
  NoteEdit lengthened;
  lengthened.time_stretch_ratio = 2.0f;
  edits.push_back(lengthened);
  NoteEdit shortened;
  shortened.time_stretch_ratio = 0.5f;
  edits.push_back(shortened);
  NoteEdit shifted;
  shifted.pitch_shift_semitones = 3.0f;
  edits.push_back(shifted);

  for (const NoteEdit& edit : edits) {
    const sonare::Audio rendered = rendered_with(edit);
    REQUIRE(rendered.size() == audio.size());
    REQUIRE(rendered.sample_rate() == audio.sample_rate());
    REQUIRE(peak(rendered, 0, rendered.size()) < 10.0f);
    for (size_t i = 0; i < rendered.size(); i += 97) {
      REQUIRE(std::isfinite(rendered[i]));
    }
  }
}

TEST_CASE("render_notes moves an edited note to its new position", "[note_model]") {
  // A single burst inside silence: an offset edit must vacate the source span
  // and put the energy at the destination.
  constexpr size_t kOnset = 1920;
  constexpr size_t kOffset = 4480;
  constexpr int64_t kShift = 2560;
  std::vector<float> samples(8000, 0.0f);
  const std::vector<float> burst = sine(440.0f, 0.5f, static_cast<int>(kOffset - kOnset));
  std::copy(burst.begin(), burst.end(), samples.begin() + static_cast<ptrdiff_t>(kOnset));
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), kSampleRate);

  NoteObject note = make_note(kOnset, kOffset, 440.0f);
  note.edit.time_offset_samples = kShift;
  const sonare::Audio rendered = render_notes(audio, {note});

  REQUIRE(rendered.size() == audio.size());
  const size_t moved_onset = kOnset + static_cast<size_t>(kShift);
  const size_t moved_offset = kOffset + static_cast<size_t>(kShift);
  REQUIRE(rms(rendered, moved_onset + 320, moved_offset - 320) > 0.2);
  REQUIRE(peak(rendered, kOnset + 320, moved_onset - 320) < 0.05f);
}

TEST_CASE("render_notes rejects malformed spans, edits and config", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.4f, 8000);

  // A non-identity edit throughout, so the note is one the renderer must act on.
  auto edited = [](int64_t onset, int64_t offset) {
    NoteObject note = make_note(onset, offset, 440.0f);
    note.edit.gain_db = -3.0f;
    return note;
  };

  const sonare::Audio empty_audio;
  REQUIRE_THROWS_AS(render_notes(empty_audio, {edited(0, 1600)}), sonare::SonareException);

  // Empty and reversed spans.
  REQUIRE_THROWS_AS(render_notes(audio, {edited(1920, 1920)}), sonare::SonareException);
  REQUIRE_THROWS_AS(render_notes(audio, {edited(4000, 1920)}), sonare::SonareException);

  // A negative onset has no position in the source buffer.
  REQUIRE_THROWS_AS(render_notes(audio, {edited(-160, 1600)}), sonare::SonareException);

  // Overlapping spans: the model is monophonic, so this is not a renderable set.
  const std::vector<NoteObject> overlapping = {edited(0, 3200), edited(1600, 4800)};
  REQUIRE_THROWS_AS(render_notes(audio, overlapping), sonare::SonareException);
  // Adjacent spans share a boundary without overlapping and stay legal.
  const std::vector<NoteObject> adjacent = {edited(0, 3200), edited(3200, 4800)};
  REQUIRE_NOTHROW(render_notes(audio, adjacent));

  for (const float bad : {kNaN, kInf, -kInf}) {
    NoteObject pitch = edited(1920, 4800);
    pitch.edit.gain_db = 0.0f;
    pitch.edit.pitch_shift_semitones = bad;
    REQUIRE_THROWS_AS(render_notes(audio, {pitch}), sonare::SonareException);

    NoteObject gain = edited(1920, 4800);
    gain.edit.gain_db = bad;
    REQUIRE_THROWS_AS(render_notes(audio, {gain}), sonare::SonareException);

    NoteObject stretch = edited(1920, 4800);
    stretch.edit.time_stretch_ratio = bad;
    REQUIRE_THROWS_AS(render_notes(audio, {stretch}), sonare::SonareException);
  }

  // A non-positive stretch ratio has no length to render into.
  for (const float ratio : {0.0f, -1.0f}) {
    NoteObject stretch = edited(1920, 4800);
    stretch.edit.time_stretch_ratio = ratio;
    REQUIRE_THROWS_AS(render_notes(audio, {stretch}), sonare::SonareException);
  }

  for (const float fade_ms : {kNaN, kInf, -1.0f}) {
    NoteRenderConfig config;
    config.fade_ms = fade_ms;
    REQUIRE_THROWS_AS(render_notes(audio, {edited(1920, 4800)}, config), sonare::SonareException);
  }

  NoteRenderConfig valid;
  valid.fade_ms = 5.0f;
  valid.stretch_backend = sonare::StretchBackend::NativeSpectral;
  REQUIRE_NOTHROW(render_notes(audio, {edited(1920, 4800)}, valid));
}

TEST_CASE("extract_notes rejects a voicing array whose length is not the track's frames",
          "[note_model]") {
  // Padding a short array out with unvoiced frames would turn a malformed track
  // into a plausible-looking short note, so the mismatch is an error instead.
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);

  F0Track short_voiced = voiced_track(440.0f, 40);
  short_voiced.voiced.resize(39);
  REQUIRE(short_voiced.n_frames() == 40);
  REQUIRE_THROWS_AS(extract_notes(audio, short_voiced), sonare::SonareException);

  F0Track long_voiced = voiced_track(440.0f, 40);
  long_voiced.voiced.push_back(true);
  REQUIRE_THROWS_AS(extract_notes(audio, long_voiced), sonare::SonareException);

  // With no explicit flags the probabilities carry the decision, so their length
  // is the one that has to match.
  F0Track short_prob = voiced_track(440.0f, 40);
  short_prob.voiced.clear();
  short_prob.voiced_prob.resize(39);
  REQUIRE_THROWS_AS(extract_notes(audio, short_prob), sonare::SonareException);

  F0Track long_prob = voiced_track(440.0f, 40);
  long_prob.voiced.clear();
  long_prob.voiced_prob.push_back(1.0f);
  REQUIRE_THROWS_AS(extract_notes(audio, long_prob), sonare::SonareException);

  // voiced_prob is optional wherever voiced is populated, so an absent one is
  // not a mismatch.
  F0Track flags_only = voiced_track(440.0f, 40);
  flags_only.voiced_prob.clear();
  REQUIRE_NOTHROW(extract_notes(audio, flags_only));
}

TEST_CASE("render_notes ignores the other edit fields of a muted note", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 8000);
  constexpr size_t kOnset = 1920;
  constexpr size_t kOffset = 6080;
  constexpr size_t kMargin = 320;

  NoteObject note = make_note(kOnset, kOffset, 440.0f);
  note.edit.muted = true;
  note.edit.gain_db = 12.0f;
  note.edit.pitch_shift_semitones = 5.0f;
  note.edit.time_stretch_ratio = 2.0f;
  note.edit.time_offset_samples = 1600;
  const sonare::Audio rendered = render_notes(audio, {note});

  REQUIRE(rendered.size() == audio.size());
  REQUIRE(rendered.sample_rate() == audio.sample_rate());
  // Muting silences the span and nothing else happens: no gain, no shift, and
  // the note does not move, so the source is intact on both sides.
  REQUIRE(peak(rendered, kOnset + kMargin, kOffset - kMargin) < 1e-6f);
  REQUIRE(first_mismatch(audio, rendered, 0, kOnset - kMargin) == kNoMismatch);
  REQUIRE(first_mismatch(audio, rendered, kOffset + kMargin, audio.size()) == kNoMismatch);
}

TEST_CASE("render_notes accepts moved notes that land on top of each other", "[note_model]") {
  // Overlap is a property of the source spans. Two notes moved onto the same
  // stretch of the timeline are a legal set; the later write wins.
  const sonare::Audio audio = tone(440.0f, 0.4f, 8000);

  NoteObject first = make_note(0, 3200, 440.0f);
  first.edit.time_offset_samples = 1600;
  NoteObject second = make_note(3200, 6400, 440.0f);
  second.edit.time_offset_samples = -1600;
  const std::vector<NoteObject> moved_onto_each_other = {first, second};

  // The two destinations genuinely overlap, which is the case that must not be
  // rejected.
  REQUIRE(first.onset_sample + first.edit.time_offset_samples <
          second.offset_sample + second.edit.time_offset_samples);
  REQUIRE(second.onset_sample + second.edit.time_offset_samples <
          first.offset_sample + first.edit.time_offset_samples);

  const sonare::Audio rendered = render_notes(audio, moved_onto_each_other);
  REQUIRE(rendered.size() == audio.size());
  REQUIRE(rendered.sample_rate() == audio.sample_rate());
  for (size_t i = 0; i < rendered.size(); i += 97) {
    REQUIRE(std::isfinite(rendered[i]));
  }
}

TEST_CASE("extract_notes serves a track that states only its frame cadence", "[note_model]") {
  // A host-supplied track sets frame_rate_hz and leaves the hop unset. Under the
  // cadence rule that is a complete track, so it has to segment -- returning an
  // empty set here would look like "no notes found" rather than like a track the
  // extractor could not read.
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);

  F0Track cadence_only = voiced_track(440.0f, 40);
  cadence_only.hop_length = 0;
  cadence_only.frame_rate_hz = kFrameRateHz;
  REQUIRE_THAT(cadence_only.frame_rate(), WithinAbs(kFrameRateHz, 0.001f));

  const std::vector<NoteObject> notes = extract_notes(audio, cadence_only);
  REQUIRE(notes.size() == 1);
  REQUIRE(notes[0].frame_start == 0);
  REQUIRE(notes[0].frame_end == 40);
  REQUIRE(notes[0].onset_sample == 0);
  REQUIRE(notes[0].offset_sample == 6400);

  // The same track with no sample rate either: the cadence still resolves, and
  // the audio supplies the rate the frame-to-sample half of the rule needs.
  F0Track no_sample_rate = cadence_only;
  no_sample_rate.sample_rate = 0;
  const std::vector<NoteObject> from_audio_rate = extract_notes(audio, no_sample_rate);
  REQUIRE(from_audio_rate.size() == 1);
  REQUIRE(from_audio_rate[0].onset_sample == 0);
  REQUIRE(from_audio_rate[0].offset_sample == 6400);
}
