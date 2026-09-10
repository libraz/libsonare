#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/fft.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_object.h"
#include "editing/note_model/note_renderer.h"
#include "editing/note_model/note_split_merge.h"
#include "editing/pitch_editor/f0_provider.h"
#include "effects/formant_warp.h"
#include "effects/pitch_shift.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
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
NoteObject synthetic_note(int64_t onset_sample, int64_t offset_sample, float frequency_hz) {
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

/// @brief Harmonics of @p f0_hz under a resonance envelope.
/// @details A bare sine has no spectral envelope to warp, so the formant cases
///          need a source whose envelope an LPC round can model and move.
sonare::Audio vowel_tone(float f0_hz, float formant_hz, float amplitude, int samples) {
  constexpr float kBandwidthHz = 500.0f;
  const float nyquist = 0.5f * static_cast<float>(kSampleRate);
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);
  for (int h = 1; static_cast<float>(h) * f0_hz < nyquist; ++h) {
    const float harmonic_hz = static_cast<float>(h) * f0_hz;
    const float weight = 1.0f / (1.0f + std::pow((harmonic_hz - formant_hz) / kBandwidthHz, 2.0f));
    const std::vector<float> partial = sine(harmonic_hz, amplitude * weight, samples);
    for (int i = 0; i < samples; ++i) {
      output[static_cast<size_t>(i)] += partial[static_cast<size_t>(i)];
    }
  }
  return sonare::Audio::from_vector(std::move(output), kSampleRate);
}

/// @brief 440 Hz for the first half, 587.33 Hz (a fifth up) for the second.
sonare::Audio two_pitch_tone(int samples) {
  std::vector<float> values = sine(440.0f, 0.5f, samples / 2);
  const std::vector<float> upper = sine(587.33f, 0.25f, samples - samples / 2);
  values.insert(values.end(), upper.begin(), upper.end());
  return sonare::Audio::from_vector(std::move(values), kSampleRate);
}

/// @brief The track matching @ref two_pitch_tone: the jump lands halfway.
F0Track two_pitch_track(int frames) {
  F0Track track = voiced_track(440.0f, frames);
  for (size_t i = static_cast<size_t>(frames) / 2; i < static_cast<size_t>(frames); ++i) {
    track.f0_hz[i] = 587.33f;
  }
  return track;
}

/// @brief Marks [@p first, @p last) unvoiced and drops their pitch, the way an
///        unvoiced gap between two sung notes reads.
void silence_frames(F0Track& track, int first, int last) {
  for (int frame = first; frame < last; ++frame) {
    const size_t i = static_cast<size_t>(frame);
    track.voiced[i] = false;
    track.voiced_prob[i] = 0.0f;
    track.f0_hz[i] = 0.0f;
  }
}

float max_abs_difference(const sonare::Audio& a, const sonare::Audio& b) {
  REQUIRE(a.size() == b.size());
  float worst = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    worst = std::max(worst, std::abs(a[i] - b[i]));
  }
  return worst;
}

/// @brief Magnitude-weighted spectral centroid (Hz) of one Hann-windowed frame
///        starting at @p lo. Tracks where the spectral envelope sits.
float centroid_hz(const sonare::Audio& audio, size_t lo) {
  constexpr int kNfft = 4096;
  const size_t available = audio.size() > lo ? audio.size() - lo : 0;
  const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(kNfft), available));
  std::vector<float> frame(static_cast<size_t>(kNfft), 0.0f);
  for (int i = 0; i < n; ++i) {
    const float window = 0.5f - 0.5f * std::cos(sonare::constants::kTwoPi * static_cast<float>(i) /
                                                static_cast<float>(kNfft - 1));
    frame[static_cast<size_t>(i)] = audio[lo + static_cast<size_t>(i)] * window;
  }

  sonare::FFT fft(kNfft);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(frame.data(), spectrum.data());

  double weighted = 0.0;
  double total = 0.0;
  const double bin_hz = static_cast<double>(kSampleRate) / kNfft;
  for (int bin = 0; bin < fft.n_bins(); ++bin) {
    const double magnitude = std::abs(spectrum[static_cast<size_t>(bin)]);
    weighted += magnitude * (static_cast<double>(bin) * bin_hz);
    total += magnitude;
  }
  return total > 0.0 ? static_cast<float>(weighted / total) : 0.0f;
}

/// @brief Autocorrelation-peak fundamental (Hz) over [@p lo, @p hi), searched
///        inside [@p min_hz, @p max_hz] so an octave error cannot pass.
float fundamental_hz(const sonare::Audio& audio, size_t lo, size_t hi, float min_hz, float max_hz) {
  const size_t end = std::min(hi, audio.size());
  REQUIRE(end > lo);
  const int n = static_cast<int>(end - lo);
  const int min_lag = static_cast<int>(static_cast<float>(kSampleRate) / max_hz);
  const int max_lag = static_cast<int>(static_cast<float>(kSampleRate) / min_hz);
  double best = -1.0;
  int best_lag = min_lag;
  for (int lag = min_lag; lag <= max_lag && lag < n; ++lag) {
    double acc = 0.0;
    for (int i = 0; i + lag < n; ++i) {
      acc += static_cast<double>(audio[lo + static_cast<size_t>(i)]) *
             static_cast<double>(audio[lo + static_cast<size_t>(i + lag)]);
    }
    if (acc > best) {
      best = acc;
      best_lag = lag;
    }
  }
  return static_cast<float>(kSampleRate) / static_cast<float>(best_lag);
}

/// @brief A linear 0 -> 1 ramp of @p entries values.
std::vector<float> ramp_envelope(size_t entries) {
  REQUIRE(entries > 1);
  std::vector<float> envelope(entries, 0.0f);
  for (size_t i = 0; i < entries; ++i) {
    envelope[i] = static_cast<float>(i) / static_cast<float>(entries - 1);
  }
  return envelope;
}

void require_same_curve(const NoteCurve& actual, const NoteCurve& expected) {
  REQUIRE(actual.values.size() == expected.values.size());
  REQUIRE(actual.frame_offset == expected.frame_offset);
  REQUIRE_THAT(actual.frame_rate_hz, WithinAbs(expected.frame_rate_hz, 1.0e-4f));
  float worst = 0.0f;
  for (size_t i = 0; i < expected.values.size(); ++i) {
    worst = std::max(worst, std::abs(actual.values[i] - expected.values[i]));
  }
  REQUIRE(worst < 1.0e-6f);
}

/// @brief Every field of a note, so "the same note" is not asserted field by
///        field at each call site.
void require_same_note(const NoteObject& actual, const NoteObject& expected) {
  REQUIRE(actual.onset_sample == expected.onset_sample);
  REQUIRE(actual.offset_sample == expected.offset_sample);
  REQUIRE(actual.frame_start == expected.frame_start);
  REQUIRE(actual.frame_end == expected.frame_end);
  REQUIRE_THAT(actual.median_hz, WithinAbs(expected.median_hz, 1.0e-3f));
  REQUIRE_THAT(actual.median_cents, WithinAbs(expected.median_cents, 1.0e-3f));
  REQUIRE_THAT(actual.f0_stability, WithinAbs(expected.f0_stability, 1.0e-5f));
  require_same_curve(actual.f0_hz, expected.f0_hz);
  require_same_curve(actual.amplitude, expected.amplitude);
  REQUIRE(actual.edit.is_identity() == expected.edit.is_identity());
  REQUIRE(actual.edit.gain_db == expected.edit.gain_db);
  REQUIRE(actual.edit.pitch_shift_semitones == expected.edit.pitch_shift_semitones);
  REQUIRE(actual.edit.formant_shift_semitones == expected.edit.formant_shift_semitones);
  REQUIRE(actual.edit.time_stretch_ratio == expected.edit.time_stretch_ratio);
  REQUIRE(actual.edit.time_offset_samples == expected.edit.time_offset_samples);
  REQUIRE(actual.edit.muted == expected.edit.muted);
  REQUIRE(actual.edit.amplitude_envelope == expected.edit.amplitude_envelope);
}

}  // namespace

// --- Acceptance: an identity edit set is a bit-exact pass-through ---------

TEST_CASE("render_notes reproduces the input bit for bit when every edit is identity",
          "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);

  // Hand-built spans: adjacent, half-open, covering the whole buffer.
  std::vector<NoteObject> notes = {synthetic_note(0, 3200, 440.0f),
                                   synthetic_note(3200, 6400, 440.0f)};
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

  NoteEdit formant;
  formant.formant_shift_semitones = 0.01f;
  REQUIRE_FALSE(formant.is_identity());

  // Emptiness is what makes an envelope neutral, not the values in it: a unity
  // envelope still asks for the per-frame path.
  NoteEdit envelope;
  envelope.amplitude_envelope = {1.0f};
  REQUIRE_FALSE(envelope.is_identity());
  envelope.amplitude_envelope.clear();
  REQUIRE(envelope.is_identity());
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

    NoteEdit formant;
    formant.formant_shift_semitones = value;
    REQUIRE_FALSE(formant.is_identity());
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
    NoteObject note = synthetic_note(kOnset, kOffset, 440.0f);
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

  NoteObject note = synthetic_note(kOnset, kOffset, 440.0f);
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
    NoteObject note = synthetic_note(1920, 6080, 440.0f);
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

  NoteObject note = synthetic_note(kOnset, kOffset, 440.0f);
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
    NoteObject note = synthetic_note(onset, offset, 440.0f);
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

  NoteObject note = synthetic_note(kOnset, kOffset, 440.0f);
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

  NoteObject first = synthetic_note(0, 3200, 440.0f);
  first.edit.time_offset_samples = 1600;
  NoteObject second = synthetic_note(3200, 6400, 440.0f);
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

// --- NoteEdit::formant_shift_semitones ------------------------------------

TEST_CASE("render_notes runs no formant warp when the shift is 0", "[note_model]") {
  // A whole-buffer note with no cross-fade makes the render exactly the segment
  // the per-note chain produced, so a pitch-only edit can be compared against
  // pitch_shift itself. An LPC analysis-resynthesis round inserted at 0
  // semitones is lossy, so it would show up here as a per-sample difference --
  // this is the "a zero costs nothing" contract.
  const sonare::Audio audio = vowel_tone(200.0f, 1200.0f, 0.1f, 6400);

  NoteObject note = synthetic_note(0, 6400, 200.0f);
  note.edit.pitch_shift_semitones = 4.0f;
  REQUIRE(note.edit.formant_shift_semitones == 0.0f);

  NoteRenderConfig config;
  config.fade_ms = 0.0f;
  const sonare::Audio rendered = render_notes(audio, {note}, config);

  sonare::PitchShiftConfig shift_config;
  shift_config.backend = config.stretch_backend;
  const sonare::Audio expected = sonare::pitch_shift(audio, 4.0f, shift_config);

  REQUIRE(rendered.size() == audio.size());
  REQUIRE(expected.size() == audio.size());
  REQUIRE(first_mismatch(rendered, expected) == kNoMismatch);

  // The comparison above is only worth something if the field can move the
  // render at all.
  NoteObject warped = note;
  warped.edit.formant_shift_semitones = 5.0f;
  REQUIRE(first_mismatch(render_notes(audio, {warped}, config), rendered) != kNoMismatch);
}

TEST_CASE("render_notes moves the spectral envelope with the formant shift", "[note_model]") {
  const sonare::Audio audio = vowel_tone(200.0f, 1200.0f, 0.1f, 6400);

  NoteRenderConfig config;
  config.fade_ms = 0.0f;
  auto rendered_with = [&](float formant_semitones) {
    NoteObject note = synthetic_note(0, 6400, 200.0f);
    note.edit.pitch_shift_semitones = 4.0f;
    note.edit.formant_shift_semitones = formant_semitones;
    return render_notes(audio, {note}, config);
  };

  const sonare::Audio down = rendered_with(-6.0f);
  const sonare::Audio flat = rendered_with(0.0f);
  const sonare::Audio up = rendered_with(6.0f);

  // One analysis frame past the phase vocoder's edge transient.
  constexpr size_t kProbeStart = 1600;
  constexpr size_t kProbeEnd = 5696;
  const float centroid_down = centroid_hz(down, kProbeStart);
  const float centroid_flat = centroid_hz(flat, kProbeStart);
  const float centroid_up = centroid_hz(up, kProbeStart);

  REQUIRE(centroid_flat > 0.0f);
  REQUIRE(centroid_down < centroid_flat);
  REQUIRE(centroid_up > centroid_flat);
  // +-6 semitones scales the envelope by 1.41 / 0.71, so the two ends must be
  // far apart rather than merely ordered: a wrong sign fails the two ordering
  // checks, a warp that barely moves fails this one.
  REQUIRE(centroid_up / centroid_down > 1.1f);

  // The pitch edit still decides the fundamental -- the warp moves the envelope
  // over the same harmonic comb, it does not retune it.
  const float expected_f0 = 200.0f * std::pow(2.0f, 4.0f / 12.0f);
  for (const sonare::Audio* rendered : {&down, &flat, &up}) {
    REQUIRE_THAT(fundamental_hz(*rendered, kProbeStart, kProbeEnd, 150.0f, 400.0f),
                 WithinRel(expected_f0, 0.05f));
  }
}

TEST_CASE("render_notes rejects a non-finite formant shift", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.4f, 8000);

  for (const float bad : {kNaN, kInf, -kInf}) {
    NoteObject note = synthetic_note(1920, 4800, 440.0f);
    note.edit.formant_shift_semitones = bad;
    REQUIRE_FALSE(note.edit.is_identity());
    REQUIRE_THROWS_AS(render_notes(audio, {note}), sonare::SonareException);
  }

  // A wide but finite shift is an edit, not an error.
  for (const float wide : {-12.0f, 12.0f}) {
    NoteObject note = synthetic_note(1920, 4800, 440.0f);
    note.edit.formant_shift_semitones = wide;
    REQUIRE_NOTHROW(render_notes(audio, {note}));
  }
}

TEST_CASE("render_notes saturates a formant shift past the warp's factor bounds", "[note_model]") {
  // The warp clamps its own factor to [kFormantFactorMin, kFormantFactorMax], so
  // a shift past either end saturates rather than being rejected. These are the
  // semitone equivalents note_object.h documents, to the one decimal it states.
  const float max_shift = 12.0f * std::log2(sonare::kFormantFactorMax);
  const float min_shift = 12.0f * std::log2(sonare::kFormantFactorMin);
  REQUIRE_THAT(max_shift, WithinAbs(8.7f, 0.1f));
  REQUIRE_THAT(min_shift, WithinAbs(-10.3f, 0.1f));

  const sonare::Audio audio = vowel_tone(200.0f, 1200.0f, 0.1f, 6400);
  NoteRenderConfig config;
  config.fade_ms = 0.0f;
  auto rendered_with = [&](float formant_semitones) {
    NoteObject note = synthetic_note(0, 6400, 200.0f);
    note.edit.formant_shift_semitones = formant_semitones;
    return render_notes(audio, {note}, config);
  };

  // Two shifts past the ceiling resolve to the same clamped factor and render
  // identically; one inside it does not.
  const sonare::Audio at_ceiling = rendered_with(12.0f);
  REQUIRE(first_mismatch(rendered_with(18.0f), at_ceiling) == kNoMismatch);
  REQUIRE(first_mismatch(rendered_with(6.0f), at_ceiling) != kNoMismatch);

  const sonare::Audio at_floor = rendered_with(-12.0f);
  REQUIRE(first_mismatch(rendered_with(-18.0f), at_floor) == kNoMismatch);
  REQUIRE(first_mismatch(rendered_with(-6.0f), at_floor) != kNoMismatch);

  // Where the saturation starts, bracketed to half a semitone: a clamp widened
  // or narrowed in either direction fails one of these four.
  REQUIRE(first_mismatch(rendered_with(max_shift + 0.5f), at_ceiling) == kNoMismatch);
  REQUIRE(first_mismatch(rendered_with(max_shift - 0.5f), at_ceiling) != kNoMismatch);
  REQUIRE(first_mismatch(rendered_with(min_shift - 0.5f), at_floor) == kNoMismatch);
  REQUIRE(first_mismatch(rendered_with(min_shift + 0.5f), at_floor) != kNoMismatch);
}

// --- NoteEdit::amplitude_envelope -----------------------------------------

TEST_CASE("render_notes treats an empty amplitude envelope as the identity", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);

  std::vector<NoteObject> notes = {synthetic_note(0, 3200, 440.0f),
                                   synthetic_note(3200, 6400, 440.0f)};
  for (NoteObject& note : notes) {
    note.edit.amplitude_envelope = {0.5f};
    REQUIRE_FALSE(note.edit.is_identity());
  }
  REQUIRE(first_mismatch(render_notes(audio, notes), audio) != kNoMismatch);

  for (NoteObject& note : notes) {
    note.edit.amplitude_envelope.clear();
    REQUIRE(note.edit.is_identity());
  }
  REQUIRE(first_mismatch(render_notes(audio, notes), audio) == kNoMismatch);
}

TEST_CASE("render_notes reads a single-entry amplitude envelope as a constant gain",
          "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  NoteRenderConfig config;
  config.fade_ms = 0.0f;

  NoteObject enveloped = synthetic_note(0, 6400, 440.0f);
  enveloped.edit.amplitude_envelope = {0.5f};
  NoteObject gained = synthetic_note(0, 6400, 440.0f);
  gained.edit.gain_db = sonare::linear_to_db(0.5f);

  const sonare::Audio by_envelope = render_notes(audio, {enveloped}, config);
  const sonare::Audio by_gain = render_notes(audio, {gained}, config);

  const double source_rms = rms(audio, 0, audio.size());
  REQUIRE(source_rms > 0.0);
  REQUIRE_THAT(rms(by_envelope, 0, by_envelope.size()) / source_rms, WithinAbs(0.5, 0.005));
  // The two paths differ only by the dB round trip, whose float error is a few
  // parts in 1e7 of the 0.5 factor; a wrong factor would be off by 0.1 or more.
  REQUIRE(max_abs_difference(by_envelope, by_gain) < 1.0e-4f);
}

TEST_CASE("render_notes follows an amplitude envelope's ramp in the rendered RMS", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  NoteRenderConfig config;
  config.fade_ms = 0.0f;

  NoteObject note = synthetic_note(0, 6400, 440.0f);
  note.edit.amplitude_envelope = ramp_envelope(40);
  const sonare::Audio rendered = render_notes(audio, {note}, config);

  const double source_rms = rms(audio, 0, audio.size());
  REQUIRE(source_rms > 0.0);

  // The RMS of a linear 0 -> 1 ramp over the sub-range [a, b] of the span is
  // sqrt((a^2 + ab + b^2) / 3), so the whole shape is asserted rather than
  // "something changed". The tolerance covers a nearest-neighbour resampling of
  // the 40-entry envelope, whose worst step is 1/40.
  constexpr int kBlocks = 8;
  const size_t block = audio.size() / kBlocks;
  for (int i = 0; i < kBlocks; ++i) {
    const double a = static_cast<double>(i) / kBlocks;
    const double b = static_cast<double>(i + 1) / kBlocks;
    const double expected = std::sqrt((a * a + a * b + b * b) / 3.0);
    INFO("block " << i);
    REQUIRE_THAT(rms(rendered, static_cast<size_t>(i) * block, static_cast<size_t>(i + 1) * block) /
                     source_rms,
                 WithinAbs(expected, 0.04));
  }
}

TEST_CASE("render_notes resamples the amplitude envelope to the stretched length", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  NoteRenderConfig config;
  config.fade_ms = 0.0f;

  auto rendered_with = [&](bool with_envelope) {
    NoteObject note = synthetic_note(0, 6400, 440.0f);
    note.edit.time_stretch_ratio = 0.5f;
    if (with_envelope) note.edit.amplitude_envelope = ramp_envelope(40);
    return render_notes(audio, {note}, config);
  };

  const sonare::Audio plain = rendered_with(false);
  const sonare::Audio shaped = rendered_with(true);

  // The envelope is the only difference between the two renders, so their
  // per-block RMS ratio is the envelope itself -- independent of what the
  // stretcher did to the level. The note is half as long, so the ramp has to
  // complete inside the first 3200 samples: an envelope indexed by source frames
  // instead of resampled would only reach half way by then.
  constexpr size_t kStretched = 3200;
  constexpr size_t kBlock = 400;
  double previous = -1.0;
  for (size_t begin = 0; begin < kStretched; begin += kBlock) {
    const double reference = rms(plain, begin, begin + kBlock);
    REQUIRE(reference > 0.0);
    const double ratio = rms(shaped, begin, begin + kBlock) / reference;
    INFO("block at " << begin << " ratio " << ratio);
    REQUIRE(ratio > previous);
    previous = ratio;
  }
  REQUIRE(rms(shaped, 0, kBlock) / rms(plain, 0, kBlock) < 0.25);
  // 0.938 if the ramp completes, 0.47 if only its first half was used.
  REQUIRE(rms(shaped, kStretched - kBlock, kStretched) /
              rms(plain, kStretched - kBlock, kStretched) >
          0.75);
}

TEST_CASE("render_notes multiplies the amplitude envelope with gain_db", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  NoteRenderConfig config;
  config.fade_ms = 0.0f;

  NoteObject both = synthetic_note(0, 6400, 440.0f);
  both.edit.amplitude_envelope = {0.5f};
  both.edit.gain_db = -6.0f;

  NoteObject folded = synthetic_note(0, 6400, 440.0f);
  folded.edit.gain_db = -6.0f + sonare::linear_to_db(0.5f);

  const sonare::Audio composed = render_notes(audio, {both}, config);
  const double source_rms = rms(audio, 0, audio.size());
  REQUIRE(source_rms > 0.0);
  // 0.5 x -6 dB is -12.02 dB. An envelope that replaced the gain would land on
  // 0.5 and a gain that replaced the envelope on 0.501 -- both twice this.
  REQUIRE_THAT(rms(composed, 0, composed.size()) / source_rms,
               WithinAbs(0.5 * sonare::db_to_linear(-6.0f), 0.005));
  REQUIRE(max_abs_difference(composed, render_notes(audio, {folded}, config)) < 1.0e-4f);
}

TEST_CASE("render_notes rejects a non-finite or negative amplitude envelope value",
          "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.4f, 8000);

  auto with_envelope = [](std::vector<float> envelope) {
    NoteObject note = synthetic_note(1920, 4800, 440.0f);
    note.edit.amplitude_envelope = std::move(envelope);
    return note;
  };

  for (const float bad : {kNaN, kInf, -kInf, -0.1f}) {
    REQUIRE_THROWS_AS(render_notes(audio, {with_envelope({bad})}), sonare::SonareException);
    // Not only the first entry.
    REQUIRE_THROWS_AS(render_notes(audio, {with_envelope({1.0f, 0.5f, bad})}),
                      sonare::SonareException);
  }

  // Zero is a legal envelope value: silencing part of a note is an edit.
  REQUIRE_NOTHROW(render_notes(audio, {with_envelope({0.0f, 1.0f})}));
  REQUIRE_NOTHROW(render_notes(audio, {with_envelope({4.0f})}));
}

// --- make_note ------------------------------------------------------------

TEST_CASE("make_note reproduces the note extract_notes builds for the same span", "[note_model]") {
  const sonare::Audio audio = two_pitch_tone(6400);
  const F0Track track = two_pitch_track(40);

  const std::vector<NoteObject> extracted = extract_notes(audio, track);
  REQUIRE(extracted.size() == 2);
  for (const NoteObject& expected : extracted) {
    INFO("frames [" << expected.frame_start << ", " << expected.frame_end << ")");
    const NoteObject built = make_note(audio, track, expected.frame_start, expected.frame_end);
    require_same_note(built, expected);
    REQUIRE(built.edit.is_identity());
  }
}

TEST_CASE("make_note measures a span the segmenter would not have emitted", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  F0Track gapped = voiced_track(440.0f, 40);
  silence_frames(gapped, 15, 20);
  REQUIRE(extract_notes(audio, gapped).size() == 2);

  const NoteObject spanning = make_note(audio, gapped, 0, 40);
  REQUIRE(spanning.frame_start == 0);
  REQUIRE(spanning.frame_end == 40);
  REQUIRE(spanning.onset_sample == 0);
  REQUIRE(spanning.offset_sample == 6400);
  REQUIRE(spanning.f0_hz.values.size() == 40);
  REQUIRE(spanning.amplitude.values.size() == 40);
  REQUIRE(spanning.f0_hz.frame_offset == 0);
  REQUIRE(spanning.amplitude.frame_offset == 0);
  REQUIRE(spanning.edit.is_identity());

  // The gap's own frames are carried through rather than interpolated over: the
  // pitch is the track's 0 and the level is measured from the audio, which is a
  // tone throughout.
  for (size_t i = 15; i < 20; ++i) {
    REQUIRE(spanning.f0_hz.values[i] == 0.0f);
    REQUIRE(spanning.amplitude.values[i] > 0.0f);
  }
  for (const float value : spanning.amplitude.values) {
    REQUIRE(std::isfinite(value));
    REQUIRE(value >= 0.0f);
  }

  // Every voiced frame sits at the same pitch, so the hole moves neither the
  // measured pitch nor the deviation statistic.
  REQUIRE_THAT(spanning.median_hz, WithinAbs(440.0f, 1.0f));
  REQUIRE(spanning.f0_stability > 0.99f);
  REQUIRE(spanning.f0_stability <= 1.0f);
}

TEST_CASE("make_note scores a span that crosses a pitch jump as unstable", "[note_model]") {
  const sonare::Audio audio = two_pitch_tone(6400);
  const F0Track track = two_pitch_track(40);

  const std::vector<NoteObject> extracted = extract_notes(audio, track);
  REQUIRE(extracted.size() == 2);
  const NoteObject spanning = make_note(audio, track, 0, 40);

  REQUIRE(spanning.frame_start == 0);
  REQUIRE(spanning.frame_end == 40);
  REQUIRE(spanning.f0_hz.values.size() == 40);

  // Half the span sits 500 cents above the other half, so the median absolute
  // deviation is ~250 cents against the 50-cent segmentation threshold and the
  // stability saturates at its floor.
  REQUIRE(extracted[0].f0_stability > 0.99f);
  REQUIRE(extracted[1].f0_stability > 0.99f);
  REQUIRE(spanning.f0_stability < 0.05f);

  // The measured pitch is the whole span's, so it lands between the two notes'.
  REQUIRE(spanning.median_hz > extracted[0].median_hz);
  REQUIRE(spanning.median_hz < extracted[1].median_hz);
}

TEST_CASE("make_note reports no pitch for a span that carries none", "[note_model]") {
  // An empty set of voiced cents has no median. Reporting reference_hz there
  // would read as a measured A4; 0 Hz is how an F0 track spells "no pitch".
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  F0Track unvoiced = voiced_track(440.0f, 40);
  silence_frames(unvoiced, 0, 40);
  REQUIRE(extract_notes(audio, unvoiced).empty());

  const NoteObject note = make_note(audio, unvoiced, 0, 40);
  REQUIRE(note.median_hz == 0.0f);
  REQUIRE(note.median_cents == 0.0f);
  REQUIRE(note.f0_stability == 0.0f);

  // The span is still measured: both curves cover it and the level comes from
  // the audio, which is a tone throughout.
  REQUIRE(note.frame_start == 0);
  REQUIRE(note.frame_end == 40);
  REQUIRE(note.onset_sample == 0);
  REQUIRE(note.offset_sample == 6400);
  REQUIRE(note.f0_hz.values.size() == 40);
  REQUIRE(note.amplitude.values.size() == 40);
  REQUIRE(note.f0_hz.frame_offset == 0);
  REQUIRE(note.amplitude.frame_offset == 0);
  REQUIRE(note.edit.is_identity());
  for (const float value : note.amplitude.values) {
    REQUIRE(value > 0.0f);
  }

  // A voiced flag with no usable pitch behind it reads the same way: the
  // measurement follows the pitch, not the flag.
  F0Track zero_pitch = voiced_track(440.0f, 40);
  zero_pitch.f0_hz.assign(40, 0.0f);
  const NoteObject from_zero = make_note(audio, zero_pitch, 0, 40);
  REQUIRE(from_zero.median_hz == 0.0f);
  REQUIRE(from_zero.median_cents == 0.0f);
  REQUIRE(from_zero.f0_stability == 0.0f);
  REQUIRE(from_zero.amplitude.values.size() == 40);

  F0Track nan_pitch = voiced_track(440.0f, 40);
  nan_pitch.f0_hz.assign(40, kNaN);
  const NoteObject from_nan = make_note(audio, nan_pitch, 0, 40);
  REQUIRE(from_nan.median_hz == 0.0f);
  REQUIRE(from_nan.median_cents == 0.0f);
  REQUIRE(from_nan.f0_stability == 0.0f);
  REQUIRE(from_nan.amplitude.values.size() == 40);
}

TEST_CASE("make_note rejects a span that is empty, reversed or outside the track", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  const F0Track track = voiced_track(440.0f, 40);

  REQUIRE_THROWS_AS(make_note(audio, track, 10, 10), sonare::SonareException);
  REQUIRE_THROWS_AS(make_note(audio, track, 40, 40), sonare::SonareException);
  REQUIRE_THROWS_AS(make_note(audio, track, 20, 10), sonare::SonareException);
  REQUIRE_THROWS_AS(make_note(audio, track, -1, 20), sonare::SonareException);
  REQUIRE_THROWS_AS(make_note(audio, track, 0, 41), sonare::SonareException);
  REQUIRE_THROWS_AS(make_note(audio, track, 41, 42), sonare::SonareException);

  // The inputs extract_notes rejects are rejected here too.
  const sonare::Audio empty_audio;
  REQUIRE_THROWS_AS(make_note(empty_audio, track, 0, 20), sonare::SonareException);
  F0Track no_voicing = voiced_track(440.0f, 40);
  no_voicing.voiced.clear();
  no_voicing.voiced_prob.clear();
  REQUIRE_THROWS_AS(make_note(audio, no_voicing, 0, 20), sonare::SonareException);
  NoteExtractorConfig bad_config;
  bad_config.voiced_threshold = kNaN;
  REQUIRE_THROWS_AS(make_note(audio, track, 0, 20, bad_config), sonare::SonareException);

  // Both extremes of a legal span, including a single frame.
  REQUIRE_NOTHROW(make_note(audio, track, 0, 40));
  REQUIRE_NOTHROW(make_note(audio, track, 39, 40));
}

// --- split_note -----------------------------------------------------------

TEST_CASE("split_note leaves an identity-edit render bit for bit unchanged", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  const F0Track track = voiced_track(440.0f, 40);
  const std::vector<NoteObject> notes = extract_notes(audio, track);
  REQUIRE(notes.size() == 1);

  const std::vector<NoteObject> split = split_note(audio, track, notes, 0, 20);
  REQUIRE(split.size() == 2);
  for (const NoteObject& half : split) {
    REQUIRE(half.edit.is_identity());
  }
  REQUIRE(first_mismatch(render_notes(audio, split), audio) == kNoMismatch);

  // The input list is left alone, like every other edit in this model.
  REQUIRE(notes.size() == 1);
  REQUIRE(notes[0].frame_end == 40);
}

TEST_CASE("split_note partitions the source note's span and cuts its curves", "[note_model]") {
  const sonare::Audio audio = two_pitch_tone(6400);
  const F0Track track = two_pitch_track(40);
  const std::vector<NoteObject> notes = extract_notes(audio, track);
  REQUIRE(notes.size() == 2);

  const NoteObject source = notes[0];
  constexpr int kCut = 8;
  const std::vector<NoteObject> split = split_note(audio, track, notes, 0, kCut);
  REQUIRE(split.size() == 3);

  const NoteObject& first = split[0];
  const NoteObject& second = split[1];
  REQUIRE(first.frame_start == source.frame_start);
  REQUIRE(first.frame_end == kCut);
  REQUIRE(second.frame_start == kCut);
  REQUIRE(second.frame_end == source.frame_end);

  // No gap and no overlap in samples either.
  REQUIRE(first.onset_sample == source.onset_sample);
  REQUIRE(first.offset_sample == second.onset_sample);
  REQUIRE(second.offset_sample == source.offset_sample);
  REQUIRE(first.length_samples() + second.length_samples() == source.length_samples());

  // Both halves' curves are the source's, cut at the same frame.
  const size_t cut = static_cast<size_t>(kCut - source.frame_start);
  REQUIRE(first.f0_hz.values.size() == cut);
  REQUIRE(second.f0_hz.values.size() == source.f0_hz.values.size() - cut);
  float worst_f0 = 0.0f;
  float worst_amplitude = 0.0f;
  for (size_t i = 0; i < source.f0_hz.values.size(); ++i) {
    const NoteObject& half = i < cut ? first : second;
    const size_t j = i < cut ? i : i - cut;
    worst_f0 = std::max(worst_f0, std::abs(half.f0_hz.values[j] - source.f0_hz.values[i]));
    worst_amplitude =
        std::max(worst_amplitude, std::abs(half.amplitude.values[j] - source.amplitude.values[i]));
  }
  REQUIRE(worst_f0 < 1.0e-6f);
  REQUIRE(worst_amplitude < 1.0e-6f);
  REQUIRE(first.f0_hz.frame_offset == first.frame_start);
  REQUIRE(second.f0_hz.frame_offset == second.frame_start);
  REQUIRE(second.amplitude.frame_offset == second.frame_start);

  // The other note is untouched and the list stays in time order.
  require_same_note(split[2], notes[1]);
  REQUIRE(split[0].offset_sample <= split[1].onset_sample);
  REQUIRE(split[1].offset_sample <= split[2].onset_sample);
}

TEST_CASE("split_note cuts an amplitude envelope at the same proportion", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  const F0Track track = voiced_track(440.0f, 40);
  std::vector<NoteObject> notes = extract_notes(audio, track);
  REQUIRE(notes.size() == 1);

  // 200 entries over the note's 40 frames: an envelope does not have to match
  // the frame count, and the finer grid keeps the seam error near 1/200 rather
  // than 1/40.
  notes[0].edit.amplitude_envelope = ramp_envelope(200);
  notes[0].edit.gain_db = -3.0f;

  NoteRenderConfig config;
  config.fade_ms = 0.0f;
  const sonare::Audio before = render_notes(audio, notes, config);

  const std::vector<NoteObject> split = split_note(audio, track, notes, 0, 20);
  REQUIRE(split.size() == 2);
  REQUIRE(split[0].edit.gain_db == notes[0].edit.gain_db);
  REQUIRE(split[1].edit.gain_db == notes[0].edit.gain_db);
  const sonare::Audio after = render_notes(audio, split, config);

  // Not bit-exact: each half's envelope is resampled onto its own endpoints, so
  // the two disagree by about one envelope step (1/200 = 0.5%) near the seam.
  // A wrong proportion -- either half carrying the whole ramp -- moves a block
  // by ~0.5, which 2% catches with room to spare.
  constexpr int kBlocks = 8;
  const size_t block = audio.size() / kBlocks;
  for (int i = 0; i < kBlocks; ++i) {
    const size_t begin = static_cast<size_t>(i) * block;
    INFO("block " << i);
    REQUIRE(rms(before, begin, begin + block) > 0.0);
    REQUIRE_THAT(rms(after, begin, begin + block),
                 WithinRel(rms(before, begin, begin + block), 0.02));
  }

  // A one-entry envelope is a constant over the span, so both halves get that
  // same entry and the constant survives the split exactly.
  std::vector<NoteObject> constant = extract_notes(audio, track);
  REQUIRE(constant.size() == 1);
  constant[0].edit.amplitude_envelope = {0.5f};
  const sonare::Audio flat_before = render_notes(audio, constant, config);

  const std::vector<NoteObject> flat_split = split_note(audio, track, constant, 0, 20);
  REQUIRE(flat_split.size() == 2);
  for (const NoteObject& half : flat_split) {
    REQUIRE(half.edit.amplitude_envelope.size() == 1);
    REQUIRE(half.edit.amplitude_envelope[0] == 0.5f);
  }
  REQUIRE(first_mismatch(render_notes(audio, flat_split, config), flat_before) == kNoMismatch);
}

TEST_CASE("split_note rejects an out-of-range index and a frame outside the note", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  const F0Track track = voiced_track(440.0f, 40);
  const std::vector<NoteObject> notes = extract_notes(audio, track);
  REQUIRE(notes.size() == 1);
  REQUIRE(notes[0].frame_start == 0);
  REQUIRE(notes[0].frame_end == 40);

  const std::vector<NoteObject> none;
  REQUIRE_THROWS_AS(split_note(audio, track, none, 0, 20), sonare::SonareException);
  REQUIRE_THROWS_AS(split_note(audio, track, notes, 1, 20), sonare::SonareException);

  // Strictly inside the span, so neither end is a legal cut.
  REQUIRE_THROWS_AS(split_note(audio, track, notes, 0, 0), sonare::SonareException);
  REQUIRE_THROWS_AS(split_note(audio, track, notes, 0, 40), sonare::SonareException);
  REQUIRE_THROWS_AS(split_note(audio, track, notes, 0, -1), sonare::SonareException);
  REQUIRE_THROWS_AS(split_note(audio, track, notes, 0, 41), sonare::SonareException);

  // One frame in from either end is legal, and both halves keep a span.
  for (const int frame : {1, 39}) {
    const std::vector<NoteObject> split = split_note(audio, track, notes, 0, frame);
    REQUIRE(split.size() == 2);
    REQUIRE(split[0].length_samples() > 0);
    REQUIRE(split[1].length_samples() > 0);
  }
}

// --- merge_notes ----------------------------------------------------------

TEST_CASE("merge_notes spans the gap the segmenter cut at and re-measures it", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  F0Track gapped = voiced_track(440.0f, 40);
  silence_frames(gapped, 15, 20);

  const std::vector<NoteObject> notes = extract_notes(audio, gapped);
  REQUIRE(notes.size() == 2);
  REQUIRE(notes[0].frame_end == 15);
  REQUIRE(notes[1].frame_start == 20);
  // Neither note carries the gap's five frames, so a merge that concatenated
  // what the two held would come back short.
  REQUIRE(notes[0].f0_hz.values.size() + notes[1].f0_hz.values.size() == 35);

  const std::vector<NoteObject> merged = merge_notes(audio, gapped, notes, 0, 1);
  REQUIRE(merged.size() == 1);
  const NoteObject& note = merged[0];
  REQUIRE(note.frame_start == 0);
  REQUIRE(note.frame_end == 40);
  REQUIRE(note.onset_sample == notes[0].onset_sample);
  REQUIRE(note.offset_sample == notes[1].offset_sample);
  REQUIRE(note.f0_hz.values.size() == 40);
  REQUIRE(note.amplitude.values.size() == 40);
  for (size_t i = 15; i < 20; ++i) {
    REQUIRE(note.f0_hz.values[i] == 0.0f);
    REQUIRE(note.amplitude.values[i] > 0.0f);
  }

  // Re-derived from the track and the audio, so it is the note the same span
  // would have been built as from scratch.
  require_same_note(note, make_note(audio, gapped, 0, 40));
}

TEST_CASE("merge_notes measures a long gap's pitch from the voiced frames only", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  F0Track gapped = voiced_track(440.0f, 40);
  silence_frames(gapped, 5, 35);

  const std::vector<NoteObject> notes = extract_notes(audio, gapped);
  REQUIRE(notes.size() == 2);
  REQUIRE(notes[0].frame_end == 5);
  REQUIRE(notes[1].frame_start == 35);

  const std::vector<NoteObject> merged = merge_notes(audio, gapped, notes, 0, 1);
  REQUIRE(merged.size() == 1);
  REQUIRE(merged[0].frame_start == 0);
  REQUIRE(merged[0].frame_end == 40);

  // Three quarters of the span carries no pitch, but the ten frames that do are
  // all at 440: the unvoiced frames are left out of the statistic rather than
  // averaged in as 0, which would have read as 110 Hz.
  REQUIRE_THAT(merged[0].median_hz, WithinAbs(440.0f, 1.0f));
  REQUIRE(merged[0].f0_stability > 0.99f);
  REQUIRE(merged[0].f0_hz.values.size() == 40);
  for (size_t i = 5; i < 35; ++i) {
    REQUIRE(merged[0].f0_hz.values[i] == 0.0f);
  }
  // Still measured over the whole span, gap included.
  for (const float value : merged[0].amplitude.values) {
    REQUIRE(value > 0.0f);
  }
}

TEST_CASE("merge_notes takes the first note's edit", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  F0Track gapped = voiced_track(440.0f, 40);
  silence_frames(gapped, 15, 20);

  std::vector<NoteObject> notes = extract_notes(audio, gapped);
  REQUIRE(notes.size() == 2);
  notes[0].edit.gain_db = -3.0f;
  notes[0].edit.pitch_shift_semitones = 2.0f;
  notes[0].edit.formant_shift_semitones = -1.5f;
  notes[0].edit.time_offset_samples = 160;
  notes[0].edit.time_stretch_ratio = 1.5f;
  notes[0].edit.amplitude_envelope = {0.25f, 0.75f};
  notes[1].edit.gain_db = 9.0f;
  notes[1].edit.muted = true;

  const std::vector<NoteObject> merged = merge_notes(audio, gapped, notes, 0, 1);
  REQUIRE(merged.size() == 1);
  const NoteEdit& edit = merged[0].edit;
  REQUIRE(edit.gain_db == notes[0].edit.gain_db);
  REQUIRE(edit.pitch_shift_semitones == notes[0].edit.pitch_shift_semitones);
  REQUIRE(edit.formant_shift_semitones == notes[0].edit.formant_shift_semitones);
  REQUIRE(edit.time_offset_samples == notes[0].edit.time_offset_samples);
  REQUIRE(edit.time_stretch_ratio == notes[0].edit.time_stretch_ratio);
  REQUIRE(edit.amplitude_envelope == notes[0].edit.amplitude_envelope);
  // The second note's mute does not survive: the rule is the first note's edit,
  // not a merge of the two.
  REQUIRE_FALSE(edit.muted);
}

TEST_CASE("merge_notes leaves the other notes untouched and in time order", "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  F0Track track = voiced_track(440.0f, 40);
  silence_frames(track, 10, 12);
  silence_frames(track, 25, 27);

  const std::vector<NoteObject> notes = extract_notes(audio, track);
  REQUIRE(notes.size() == 3);

  const std::vector<NoteObject> merged = merge_notes(audio, track, notes, 1, 2);
  REQUIRE(merged.size() == 2);
  require_same_note(merged[0], notes[0]);
  REQUIRE(merged[1].frame_start == notes[1].frame_start);
  REQUIRE(merged[1].frame_end == notes[2].frame_end);
  REQUIRE(merged[1].onset_sample == notes[1].onset_sample);
  REQUIRE(merged[1].offset_sample == notes[2].offset_sample);
  REQUIRE(merged[0].offset_sample <= merged[1].onset_sample);

  // Merging the whole run collapses the list to one note over the whole span.
  const std::vector<NoteObject> all = merge_notes(audio, track, notes, 0, 2);
  REQUIRE(all.size() == 1);
  REQUIRE(all[0].frame_start == notes[0].frame_start);
  REQUIRE(all[0].frame_end == notes[2].frame_end);
}

TEST_CASE("merge_notes rejects a first/last pair that is not an ascending in-range run",
          "[note_model]") {
  const sonare::Audio audio = tone(440.0f, 0.5f, 6400);
  F0Track track = voiced_track(440.0f, 40);
  silence_frames(track, 10, 12);
  silence_frames(track, 25, 27);
  const std::vector<NoteObject> notes = extract_notes(audio, track);
  REQUIRE(notes.size() == 3);

  // A run of one is not a merge, and a run cannot run backwards.
  REQUIRE_THROWS_AS(merge_notes(audio, track, notes, 1, 1), sonare::SonareException);
  REQUIRE_THROWS_AS(merge_notes(audio, track, notes, 2, 1), sonare::SonareException);

  // last indexes past the end, with and without first inside the list.
  REQUIRE_THROWS_AS(merge_notes(audio, track, notes, 0, 3), sonare::SonareException);
  REQUIRE_THROWS_AS(merge_notes(audio, track, notes, 3, 4), sonare::SonareException);

  const std::vector<NoteObject> none;
  REQUIRE_THROWS_AS(merge_notes(audio, track, none, 0, 1), sonare::SonareException);

  REQUIRE_NOTHROW(merge_notes(audio, track, notes, 0, 1));
  REQUIRE_NOTHROW(merge_notes(audio, track, notes, 0, 2));
}
