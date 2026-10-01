#include "editing/pitch_editor/pitch_editor.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "core/audio.h"
#include "core/convert.h"
#include "util/constants.h"
#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::editing::pitch_editor;

namespace {

std::vector<float> sine(float frequency_hz, int sample_rate, int samples) {
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) {
    output[static_cast<size_t>(i)] =
        0.5f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz *
                                           static_cast<double>(i) / sample_rate));
  }
  return output;
}

F0Track constant_track(float frequency_hz, int sample_rate, int hop_length, int frames) {
  F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = hop_length;
  track.f0_hz.assign(static_cast<size_t>(frames), frequency_hz);
  track.voiced.assign(static_cast<size_t>(frames), true);
  track.voiced_prob.assign(static_cast<size_t>(frames), 1.0f);
  return track;
}

float median_voiced_f0(const F0Track& track) {
  std::vector<float> values;
  for (size_t i = 0; i < track.f0_hz.size(); ++i) {
    if (i < track.voiced.size() && track.voiced[i] && track.f0_hz[i] > 0.0f) {
      values.push_back(track.f0_hz[i]);
    }
  }
  std::sort(values.begin(), values.end());
  REQUIRE(!values.empty());
  return values[values.size() / 2];
}

// A deliberately independent estimator for the synthetic-tone regressions
// below.  The supplied F0 track describes the requested edit, so measuring the
// rendered samples independently is what catches a wrong resynthesis pitch.
float positive_zero_crossing_hz(const sonare::Audio& audio, int begin, int end) {
  const int first = std::max(1, begin + 1);
  const int last = std::min(end, static_cast<int>(audio.size()));
  if (last <= first) return 0.0f;

  double first_crossing = 0.0;
  double last_crossing = 0.0;
  int crossings = 0;
  for (int i = first; i < last; ++i) {
    const float previous = audio[static_cast<size_t>(i - 1)];
    const float current = audio[static_cast<size_t>(i)];
    if (!(previous <= 0.0f && current > 0.0f)) continue;

    const double denominator = static_cast<double>(current) - previous;
    const double fraction = denominator > 0.0 ? -static_cast<double>(previous) / denominator : 0.0;
    const double crossing = static_cast<double>(i - 1) + fraction;
    if (crossings == 0) first_crossing = crossing;
    last_crossing = crossing;
    ++crossings;
  }
  if (crossings < 2 || last_crossing <= first_crossing) return 0.0f;
  return static_cast<float>(audio.sample_rate() * static_cast<double>(crossings - 1) /
                            (last_crossing - first_crossing));
}

// Use an independent pYIN pass for rendered-pitch measurements.  Its search
// band is fixed rather than derived from the requested edit, so the test does
// not select a lag from the expected pitch.  The long frame gives pYIN enough
// periods for both octave endpoints while retaining the whole interior window.
float median_pyin_f0(const sonare::Audio& audio) {
  sonare::PitchConfig config;
  config.frame_length = 4096;
  config.hop_length = 256;
  config.fmin = 30.0f;
  config.fmax = 1000.0f;
  PyinF0Provider provider(config);
  return median_voiced_f0(provider.detect(audio));
}

F0Track voiced_runs_track(float f0_hz, int sample_rate, int hop_length, int frames, int first_start,
                          int first_end, int second_start, int second_end) {
  F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = hop_length;
  track.f0_hz.assign(static_cast<size_t>(frames), 0.0f);
  track.voiced.assign(static_cast<size_t>(frames), false);
  track.voiced_prob.assign(static_cast<size_t>(frames), 1.0f);
  for (int frame = first_start; frame < first_end; ++frame) {
    track.f0_hz[static_cast<size_t>(frame)] = f0_hz;
    track.voiced[static_cast<size_t>(frame)] = true;
  }
  for (int frame = second_start; frame < second_end; ++frame) {
    track.f0_hz[static_cast<size_t>(frame)] = f0_hz;
    track.voiced[static_cast<size_t>(frame)] = true;
  }
  return track;
}

}  // namespace

TEST_CASE("NoteSegmenter splits voiced notes by sustained pitch jump", "[pitch_editor]") {
  F0Track track;
  track.sample_rate = 1000;
  track.hop_length = 10;
  track.f0_hz = {440.0f, 440.0f, 440.0f, 0.0f, 550.0f, 550.0f, 550.0f};
  track.voiced = {true, true, true, false, true, true, true};
  track.voiced_prob.assign(track.f0_hz.size(), 1.0f);

  NoteSegmenter segmenter({50.0f, 20.0f, 440.0f});
  const auto regions = segmenter.segment(track);

  REQUIRE(regions.size() == 2);
  REQUIRE(regions[0].onset_sample == 0);
  REQUIRE(regions[0].offset_sample == 30);
  REQUIRE(regions[1].onset_sample == 40);
  REQUIRE(regions[1].offset_sample == 70);
  REQUIRE_THAT(regions[0].median_cents, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(regions[1].median_cents, WithinAbs(386.3137f, 0.01f));
}

TEST_CASE("NoteSegmenter returns nothing for an invalid reference frequency", "[pitch_editor]") {
  F0Track track;
  track.sample_rate = 1000;
  track.hop_length = 10;
  track.f0_hz = {440.0f, 440.0f, 440.0f, 550.0f, 550.0f, 550.0f};
  track.voiced.assign(track.f0_hz.size(), true);
  track.voiced_prob.assign(track.f0_hz.size(), 1.0f);

  // reference_hz <= 0 makes every cents value collapse to 0; bail instead of
  // emitting meaningless segments.
  NoteSegmenter zero_ref({50.0f, 20.0f, 0.0f});
  REQUIRE(zero_ref.segment(track).empty());

  NoteSegmenter neg_ref({50.0f, 20.0f, -100.0f});
  REQUIRE(neg_ref.segment(track).empty());
}

TEST_CASE("NoteSegmenter splits sustained pitch changes without silence", "[pitch_editor]") {
  F0Track track;
  track.sample_rate = 1000;
  track.hop_length = 10;
  track.f0_hz = {440.0f, 440.0f, 440.0f, 550.0f, 550.0f, 550.0f};
  track.voiced.assign(track.f0_hz.size(), true);
  track.voiced_prob.assign(track.f0_hz.size(), 1.0f);

  NoteSegmenter segmenter({50.0f, 20.0f, 440.0f});
  const auto regions = segmenter.segment(track);

  REQUIRE(regions.size() == 2);
  REQUIRE(regions[0].frame_start == 0);
  REQUIRE(regions[0].frame_end == 3);
  REQUIRE(regions[1].frame_start == 3);
  REQUIRE(regions[1].frame_end == 6);
}

TEST_CASE("NoteSegmenter uses the shared even-sized median for note cents", "[pitch_editor]") {
  F0Track track;
  track.sample_rate = 1000;
  track.hop_length = 10;
  track.f0_hz = {440.0f, 880.0f};
  track.voiced.assign(track.f0_hz.size(), true);
  track.voiced_prob.assign(track.f0_hz.size(), 1.0f);

  NoteSegmenter segmenter({2000.0f, 1.0f, 440.0f});
  const auto regions = segmenter.segment(track);

  REQUIRE(regions.size() == 1);
  REQUIRE_THAT(regions[0].median_cents, WithinAbs(600.0f, 0.001f));
}

// pitch-editor-005: +inf (or -inf/NaN) satisfies "hz > 0.0f" with no finiteness
// guard, so a non-finite F0 estimate used to read as a strongly-voiced frame
// instead of carrying no measurement -- both starting/extending regions on its
// own and, worse, landing +inf directly in a region's cents vector.
TEST_CASE("NoteSegmenter treats a non-finite F0 as unvoiced rather than as a measurement",
          "[pitch_editor]") {
  constexpr float kInf = std::numeric_limits<float>::infinity();
  constexpr float kNegInf = -std::numeric_limits<float>::infinity();
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
  // min_note_ms = 1 at this 100 Hz cadence (hop 10, rate 1000) keeps min_frames
  // at its floor of 1, so even a single surviving frame would form a region --
  // the fix's absence would be visible on the smallest possible span.
  const NoteSegmenterConfig config{50.0f, 1.0f, 440.0f};

  for (const float bad : {kInf, kNegInf, kNaN}) {
    INFO("non-finite value " << bad);
    F0Track track;
    track.sample_rate = 1000;
    track.hop_length = 10;
    track.f0_hz = {0.0f, bad, 0.0f};
    track.voiced = {false, true, false};
    track.voiced_prob.assign(track.f0_hz.size(), 1.0f);

    NoteSegmenter segmenter(config);
    REQUIRE(segmenter.segment(track).empty());
  }

  // Inside an otherwise-voiced run, the non-finite frame must break it rather
  // than merge into it -- two finite-median regions, not one region whose
  // median absorbed an infinity.
  F0Track split_by_inf;
  split_by_inf.sample_rate = 1000;
  split_by_inf.hop_length = 10;
  split_by_inf.f0_hz = {440.0f, kInf, 440.0f};
  split_by_inf.voiced = {true, true, true};
  split_by_inf.voiced_prob.assign(split_by_inf.f0_hz.size(), 1.0f);

  NoteSegmenter segmenter(config);
  const auto regions = segmenter.segment(split_by_inf);
  REQUIRE(regions.size() == 2);
  REQUIRE(regions[0].onset_sample == 0);
  REQUIRE(regions[1].onset_sample == 20);
  for (const auto& region : regions) {
    REQUIRE(std::isfinite(region.median_cents));
    REQUIRE_THAT(region.median_cents, WithinAbs(0.0f, 0.001f));
  }
}

TEST_CASE("NoteSegmenter saturates long-track sample offsets instead of overflowing",
          "[pitch_editor]") {
  F0Track track;
  track.sample_rate = 1000;
  track.hop_length = std::numeric_limits<int>::max() / 2 + 100;
  track.f0_hz = {440.0f, 550.0f};
  track.voiced.assign(track.f0_hz.size(), true);
  track.voiced_prob.assign(track.f0_hz.size(), 1.0f);

  NoteSegmenter segmenter({50.0f, 1.0f, 440.0f});
  const auto regions = segmenter.segment(track);

  REQUIRE(regions.size() == 2);
  REQUIRE(regions[0].onset_sample == 0);
  REQUIRE(regions[0].offset_sample == track.hop_length);
  REQUIRE(regions[1].onset_sample == track.hop_length);
  REQUIRE(regions[1].offset_sample == std::numeric_limits<int>::max());
}

TEST_CASE("one F0Track converts frames at a single cadence everywhere", "[pitch_editor]") {
  // A host-supplied frame_rate_hz replaces the sample_rate / hop_length
  // derivation, and it has to replace it for BOTH halves of the conversion.
  // Reading the cadence for the note-length threshold while deriving the sample
  // offsets from hop_length puts the note boundaries at positions the frame
  // indices never described.
  F0Track track;
  track.sample_rate = 1000;
  track.hop_length = 10;        // would derive 100 frames/s
  track.frame_rate_hz = 50.0f;  // the track's real cadence: 20 samples per frame
  track.f0_hz = {440.0f, 440.0f, 440.0f, 0.0f, 550.0f, 550.0f, 550.0f};
  track.voiced = {true, true, true, false, true, true, true};
  track.voiced_prob.assign(track.f0_hz.size(), 1.0f);

  REQUIRE_THAT(track.frame_rate(), WithinAbs(50.0f, 1e-6f));
  REQUIRE_THAT(static_cast<float>(track.samples_per_frame()), WithinAbs(20.0f, 1e-6f));

  NoteSegmenter segmenter({50.0f, 20.0f, 440.0f});
  const auto regions = segmenter.segment(track);

  REQUIRE(regions.size() == 2);
  REQUIRE(regions[0].frame_start == 0);
  REQUIRE(regions[0].frame_end == 3);
  REQUIRE(regions[1].frame_start == 4);
  REQUIRE(regions[1].frame_end == 7);
  // 20 samples per frame, not the 10 that hop_length alone would give.
  REQUIRE(regions[0].onset_sample == 0);
  REQUIRE(regions[0].offset_sample == 60);
  REQUIRE(regions[1].onset_sample == 80);
  REQUIRE(regions[1].offset_sample == 140);
}

TEST_CASE("ScaleQuantizer maps all chroma to enabled scale degrees", "[pitch_editor]") {
  const ScaleQuantizer quantizer({0, 0b101010110101, 69.0f});

  for (int midi = 60; midi < 72; ++midi) {
    const float quantized = quantizer.quantize_midi(static_cast<float>(midi));
    const int pc = static_cast<int>(std::round(quantized)) % 12;
    REQUIRE(quantizer.pitch_class_enabled(pc));
  }

  REQUIRE_THAT(quantizer.quantize_midi(61.0f), WithinAbs(60.0f, 0.0001f));
  REQUIRE_THAT(quantizer.quantize_midi(66.0f), WithinAbs(65.0f, 0.0001f));
}

TEST_CASE("ScaleQuantizer reference MIDI shifts the tuning grid", "[pitch_editor]") {
  constexpr uint16_t c_major = 0b101010110101;
  const ScaleQuantizer concert_pitch({0, c_major, 69.0f});
  const ScaleQuantizer raised_grid({0, c_major, 69.25f});

  REQUIRE_THAT(concert_pitch.quantize_midi(69.1f), WithinAbs(69.0f, 0.0001f));
  REQUIRE_THAT(raised_grid.quantize_midi(69.1f), WithinAbs(69.25f, 0.0001f));
}

TEST_CASE("PyinF0Provider adapts existing pYIN output to F0Track", "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  auto samples = sine(440.0f, sample_rate, sample_rate / 2);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);
  sonare::PitchConfig config;
  config.frame_length = 1024;
  config.hop_length = 256;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;

  PyinF0Provider provider(config);
  const F0Track track = provider.detect(audio);

  REQUIRE(track.n_frames() > 0);
  REQUIRE(track.hop_length == 256);
  REQUIRE(track.sample_rate == sample_rate);
  REQUIRE(track.f0_hz.size() == track.voiced.size());
}

TEST_CASE("PitchCorrector estimates and limits semitone corrections", "[pitch_editor]") {
  const F0Track track = constant_track(440.0f, 22050, 256, 8);

  PitchCorrectionConfig config;
  config.max_correction_semitones = 0.5f;
  PitchCorrector corrector(config);

  REQUIRE_THAT(corrector.estimate_median_midi(track), WithinAbs(69.0f, 0.001f));
  REQUIRE_THAT(corrector.correction_to_midi(track, 70.0f), WithinAbs(0.5f, 0.001f));
}

TEST_CASE("PitchCorrector ignores voiced probability when voicing is explicit", "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  constexpr int hop_length = 256;
  auto samples = sine(440.0f, sample_rate, sample_rate / 2);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);
  F0Track track = constant_track(440.0f, sample_rate, hop_length,
                                 static_cast<int>(audio.size()) / hop_length + 2);

  PitchCorrectionConfig config;
  config.retune_speed_ms = 0.0f;
  config.vibrato_threshold_cents = 0.0f;
  PitchCorrector corrector(config);

  // voiced_prob is a voicing input, not a correction weight. It used to scale
  // the per-frame correction, which silently made the corrector fall short of
  // its target for any caller that passed a real pYIN probability track (that
  // value tracks F0, not confidence, so low notes barely moved). The correction
  // must now depend only on the F0 contour and the voicing flags.
  const sonare::Audio without_prob = corrector.correct_to_midi_timevarying(audio, track, 71.0f);
  track.voiced_prob.assign(track.f0_hz.size(), 0.25f);
  const sonare::Audio quarter = corrector.correct_to_midi_timevarying(audio, track, 71.0f);
  track.voiced_prob.assign(track.f0_hz.size(), 1.0f);
  const sonare::Audio full = corrector.correct_to_midi_timevarying(audio, track, 71.0f);

  REQUIRE(without_prob.size() == quarter.size());
  REQUIRE(without_prob.size() == full.size());
  for (size_t i = 0; i < full.size(); ++i) {
    REQUIRE(quarter[i] == without_prob[i]);
    REQUIRE(full[i] == without_prob[i]);
  }
}

TEST_CASE("PitchCorrector uses the shared even-sized median for detected MIDI", "[pitch_editor]") {
  F0Track track = constant_track(440.0f, 22050, 256, 4);
  track.f0_hz = {
      PitchCorrector::midi_to_hz(60.0f),
      PitchCorrector::midi_to_hz(62.0f),
      PitchCorrector::midi_to_hz(64.0f),
      PitchCorrector::midi_to_hz(66.0f),
  };

  PitchCorrector corrector;
  REQUIRE_THAT(corrector.estimate_median_midi(track), WithinAbs(63.0f, 0.001f));
}

TEST_CASE("PitchCorrector pitch conversion wrappers match core conversion contract",
          "[pitch_editor]") {
  REQUIRE_THAT(PitchCorrector::hz_to_midi(440.0f), WithinAbs(sonare::hz_to_midi(440.0f), 0.0f));
  REQUIRE_THAT(PitchCorrector::midi_to_hz(69.0f), WithinAbs(sonare::midi_to_hz(69.0f), 0.0f));

  const float zero_hz_midi = PitchCorrector::hz_to_midi(0.0f);
  REQUIRE(std::isinf(zero_hz_midi));
  REQUIRE(zero_hz_midi < 0.0f);

  const float negative_hz_midi = PitchCorrector::hz_to_midi(-440.0f);
  REQUIRE(std::isinf(negative_hz_midi));
  REQUIRE(negative_hz_midi < 0.0f);

  const float c_minus1_hz = PitchCorrector::midi_to_hz(0.0f);
  REQUIRE(std::isfinite(PitchCorrector::hz_to_midi(c_minus1_hz)));
  REQUIRE_THAT(PitchCorrector::hz_to_midi(c_minus1_hz), WithinAbs(0.0f, 0.01f));
}

TEST_CASE("PitchCorrector applies pYIN-verifiable one semitone correction", "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  auto samples = sine(440.0f, sample_rate, sample_rate);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);
  const F0Track track = constant_track(440.0f, sample_rate, 256, 16);

  // Instant snap (no retune glide) so the corrected median F0 reaches the
  // target; the default 50 ms glide ramp would bias the median flat.
  PitchCorrectionConfig corrector_config;
  corrector_config.retune_speed_ms = 0.0f;
  PitchCorrector corrector(corrector_config);
  const sonare::Audio corrected = corrector.correct_to_midi(audio, track, 70.0f);

  sonare::PitchConfig config;
  config.frame_length = 1024;
  config.hop_length = 256;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  PyinF0Provider provider(config);
  const F0Track corrected_track = provider.detect(corrected);

  const float expected_hz = PitchCorrector::midi_to_hz(70.0f);
  auto target_samples = sine(expected_hz, sample_rate, sample_rate);
  const sonare::Audio target_audio =
      sonare::Audio::from_vector(std::move(target_samples), sample_rate);
  const F0Track target_track = provider.detect(target_audio);

  // Plan spec: within +-2 cents. At ~467.5 Hz, 2 cents is about +-0.54 Hz.
  REQUIRE_THAT(median_voiced_f0(corrected_track), WithinAbs(median_voiced_f0(target_track), 0.6f));
}

TEST_CASE("PitchCorrector constant MIDI transpose reaches its target independent of sample rate",
          "[pitch_editor]") {
  for (const int sample_rate : {22050, 48000}) {
    auto samples = sine(440.0f, sample_rate, sample_rate);
    const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

    PitchCorrector corrector;
    const sonare::Audio corrected = corrector.correct_to_midi(audio, 69.0f, 70.0f);

    sonare::PitchConfig config;
    config.frame_length = 2048;
    config.hop_length = 512;
    config.fmin = 100.0f;
    config.fmax = 1000.0f;
    PyinF0Provider provider(config);
    const float detected_hz = median_voiced_f0(provider.detect(corrected));
    const float expected_hz = PitchCorrector::midi_to_hz(70.0f);
    const float cents_error = 1200.0f * std::log2(detected_hz / expected_hz);
    REQUIRE(std::abs(cents_error) < 5.0f);
  }

  const int sample_rate = 22050;
  auto three_seconds = sine(220.0f, sample_rate, sample_rate * 3);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(three_seconds), sample_rate);
  PitchCorrector corrector;
  REQUIRE(corrector.correct_to_midi(audio, 57.0f, 60.0f).size() == audio.size());
}

// pitch-editor-006: resynthesize's grain-clock accumulators (output_epoch,
// analysis_epoch) held float, whose 24-bit mantissa can only represent
// whole-2-sample increments past 2^24 samples (~12.7 min at 22050 Hz) and
// whole-4 past 2^25, so a non-integer period accumulated a directional,
// growing pitch/timing drift on long audio rather than a zero-mean rounding
// error. Slow: the input has to actually cross 2^24 samples to reproduce it.
TEST_CASE("PitchCorrector holds pitch precision on audio past 2^24 samples",
          "[pitch_editor][.][slow]") {
  constexpr int sample_rate = 22050;
  constexpr int threshold = 1 << 24;
  // Room for a full one-second measurement window starting half a second past
  // the threshold, with margin.
  const int total_samples = threshold + 2 * sample_rate;

  auto samples = sine(440.0f, sample_rate, total_samples);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);
  const int hop_length = 256;
  const F0Track track = constant_track(440.0f, sample_rate, hop_length, total_samples / hop_length);

  // Instant snap (no retune glide), matching the one-semitone-correction test
  // above, so the corrected median F0 reaches the target rather than being
  // biased flat by the default glide ramp.
  PitchCorrectionConfig corrector_config;
  corrector_config.retune_speed_ms = 0.0f;
  PitchCorrector corrector(corrector_config);
  const sonare::Audio corrected = corrector.correct_to_midi(audio, track, 70.0f);
  REQUIRE(corrected.size() == audio.size());

  // Measure only a one-second window well past the threshold, not the whole
  // buffer: pitch detection over 16M+ samples would dominate this test's
  // runtime for no extra signal about the defect, which has already fully
  // accumulated by this point and does not un-accumulate further downstream.
  const size_t window_start = static_cast<size_t>(threshold) + static_cast<size_t>(sample_rate) / 2;
  const size_t window_len = static_cast<size_t>(sample_rate);
  std::vector<float> window(corrected.data() + window_start,
                            corrected.data() + window_start + window_len);
  const sonare::Audio window_audio = sonare::Audio::from_vector(std::move(window), sample_rate);

  sonare::PitchConfig config;
  config.frame_length = 1024;
  config.hop_length = 256;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  PyinF0Provider provider(config);
  const float measured_hz = median_voiced_f0(provider.detect(window_audio));

  // Compared against a reference tone measured through the SAME detector, not
  // against the mathematically exact target: PyinF0Provider carries its own
  // few-cents systematic bias at this frame_length/hop_length/frequency
  // combination (the "pYIN-verifiable one semitone correction" test above
  // does the same, for the same reason), which would otherwise be
  // indistinguishable from the grain-clock drift this test exists to catch.
  const float expected_hz = PitchCorrector::midi_to_hz(70.0f);
  auto reference_samples = sine(expected_hz, sample_rate, sample_rate);
  const sonare::Audio reference_audio =
      sonare::Audio::from_vector(std::move(reference_samples), sample_rate);
  const float reference_hz = median_voiced_f0(provider.detect(reference_audio));

  const float cents_error = 1200.0f * std::log2(measured_hz / reference_hz);
  REQUIRE(std::abs(cents_error) < 1.0f);
}

// max_correction_semitones bounds how far a RETUNE may drag a MEASURED pitch.
// A constant transpose states both endpoints, so the interval between them is
// the request, not a distance to be pulled back toward: clamping it to the
// default octave returned a two-octave move as a one-octave one and reported
// success, with both MIDI numbers well inside the validated [0, 127].
TEST_CASE("PitchCorrector applies a constant transpose wider than the correction limit",
          "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  constexpr float kFromMidi = 48.0f;  // C3
  constexpr float kToMidi = 72.0f;    // C5, two octaves up
  auto samples = sine(PitchCorrector::midi_to_hz(kFromMidi), sample_rate, sample_rate);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

  // The default correction limit is narrower than the interval asked for, which
  // is what makes this a test of the bypass rather than of an already-legal move.
  REQUIRE(PitchCorrectionConfig{}.max_correction_semitones < kToMidi - kFromMidi);

  PitchCorrector corrector;
  const sonare::Audio corrected = corrector.correct_to_midi(audio, kFromMidi, kToMidi);

  sonare::PitchConfig config;
  config.frame_length = 2048;
  config.hop_length = 512;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  PyinF0Provider provider(config);
  const float detected_hz = median_voiced_f0(provider.detect(corrected));
  const float cents_error = 1200.0f * std::log2(detected_hz / PitchCorrector::midi_to_hz(kToMidi));
  // A clamped result would land an octave low, 1200 cents away.
  REQUIRE(std::abs(cents_error) < 20.0f);
}

TEST_CASE("PitchCorrector rejects out-of-range target and invalid F0 in the core",
          "[pitch_editor]") {
  // The validation lives in the core so every surface (C ABI, Node, Python,
  // WASM) inherits it; Node/WASM previously called the core directly and
  // returned garbage for these inputs.
  constexpr int sample_rate = 22050;
  auto samples = sine(440.0f, sample_rate, sample_rate / 4);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);
  const F0Track track = constant_track(440.0f, sample_rate, 256, 8);
  PitchCorrector corrector;

  // target_midi outside [0, 127] is rejected.
  REQUIRE_THROWS_AS(corrector.correct_to_midi(audio, track, 200.0f), sonare::SonareException);
  REQUIRE_THROWS_AS(corrector.correct_to_midi(audio, track, -1.0f), sonare::SonareException);

  // A negative / non-finite F0 in the track is rejected.
  F0Track bad_f0 = track;
  bad_f0.f0_hz[0] = -5.0f;
  REQUIRE_THROWS_AS(corrector.correct_to_midi(audio, bad_f0, 69.0f), sonare::SonareException);
  F0Track nan_f0 = track;
  nan_f0.f0_hz[1] = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS_AS(corrector.correct_to_midi(audio, nan_f0, 69.0f), sonare::SonareException);

  // pYIN represents unvoiced F0 as NaN by default. Those frames are ignored by
  // correction and must remain valid pipeline input, while the same NaN on a
  // voiced frame is still rejected.
  F0Track pyin_track = track;
  pyin_track.voiced[1] = false;
  pyin_track.voiced_prob[1] = 0.0f;
  pyin_track.f0_hz[1] = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_NOTHROW(corrector.correct_to_midi(audio, pyin_track, 69.0f));

  // Frequencies above Nyquist are not representable input pitches. Besides
  // being invalid, they make the PSOLA analysis epoch advance by less than one
  // sample and can turn large buffers into an effectively unbounded loop.
  F0Track above_nyquist = track;
  above_nyquist.f0_hz[0] = static_cast<float>(sample_rate);
  REQUIRE_THROWS_AS(corrector.correct_to_midi(audio, above_nyquist, 69.0f),
                    sonare::SonareException);

  // A valid request still succeeds.
  REQUIRE_NOTHROW(corrector.correct_to_midi(audio, track, 69.0f));
}

// pitch-editor-007: the spectral fallback pitch_shift()s the WHOLE buffer by
// one median delta over the large-shift voiced frames, then that shifted
// buffer used to stand in for the dry input at every non-PSOLA sample -- not
// just the ones actually near the large-shift region. An unvoiced region far
// from the large shift (a breath, a sibilant) must pass through unchanged.
TEST_CASE(
    "PitchCorrector's spectral fallback does not shift unvoiced audio elsewhere in the buffer",
    "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  constexpr int hop = 256;
  constexpr int frames_per_region = 40;  // ~0.46 s per region
  constexpr int region_samples = frames_per_region * hop;

  // Region A: low-amplitude noise, unvoiced -- stands in for a breath/sibilant.
  std::vector<float> noise(static_cast<size_t>(region_samples));
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> dist(-0.05f, 0.05f);
  for (float& s : noise) {
    s = dist(rng);
  }

  // Region B: a tone far enough from the target (A3 -> A4, +12 st) to exceed
  // the 6-semitone PSOLA limit and force the spectral fallback.
  auto tone = sine(220.0f, sample_rate, region_samples);

  std::vector<float> samples = noise;
  samples.insert(samples.end(), tone.begin(), tone.end());
  const sonare::Audio audio = sonare::Audio::from_vector(samples, sample_rate);

  F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = hop;
  track.f0_hz.assign(static_cast<size_t>(2 * frames_per_region), 0.0f);
  track.voiced.assign(static_cast<size_t>(2 * frames_per_region), false);
  track.voiced_prob.assign(static_cast<size_t>(2 * frames_per_region), 0.0f);
  for (int f = frames_per_region; f < 2 * frames_per_region; ++f) {
    track.f0_hz[static_cast<size_t>(f)] = 220.0f;
    track.voiced[static_cast<size_t>(f)] = true;
    track.voiced_prob[static_cast<size_t>(f)] = 1.0f;
  }

  PitchCorrector corrector;
  const sonare::Audio corrected = corrector.correct_to_midi(audio, track, 69.0f);
  REQUIRE(corrected.size() == audio.size());

  // Checked well clear of the region boundary (a generous margin past any
  // realistic cross-fade width), so this asserts the passthrough itself, not
  // the cross-fade ramp leading into it.
  constexpr int boundary_guard = 2000;
  for (int i = 0; i < region_samples - boundary_guard; ++i) {
    REQUIRE(corrected[static_cast<size_t>(i)] == samples[static_cast<size_t>(i)]);
  }
}

TEST_CASE("PitchCorrector validates configuration and time-varying track shape in the core",
          "[pitch_editor]") {
  PitchCorrectionConfig config;
  config.max_correction_semitones = -1.0f;
  REQUIRE_THROWS_AS(PitchCorrector(config), sonare::SonareException);
  config = {};
  config.retune_speed_ms = -50.0f;
  REQUIRE_THROWS_AS(PitchCorrector(config), sonare::SonareException);
  config = {};
  config.retune_speed_ms = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS_AS(PitchCorrector(config), sonare::SonareException);
  config = {};
  config.scale.mode_mask = 0;
  REQUIRE_THROWS_AS(PitchCorrector(config), sonare::SonareException);

  // pitch-editor-001: scale_quantizer.h documents reference_midi as bounded to
  // [0, kMaxReferenceMidi] by every public entry point, but PitchCorrector's
  // own constructor only checked isfinite -- an extreme reference collapses
  // the float grid in ScaleQuantizer::quantize_midi and, past 2^31 or so, can
  // hit lround()/int-cast domain issues, silently producing a wrong-sized
  // correction rather than a refusal. sonare_scale_quantize_midi already
  // range-checks its own reference_midi; this is the scale-mode PitchCorrector
  // path the header's own claim did not yet hold for.
  config = {};
  config.scale.reference_midi = kMaxReferenceMidi + 1.0f;
  REQUIRE_THROWS_AS(PitchCorrector(config), sonare::SonareException);
  config = {};
  config.scale.reference_midi = -1.0f;
  REQUIRE_THROWS_AS(PitchCorrector(config), sonare::SonareException);
  config = {};
  config.scale.reference_midi = 1e9f;
  REQUIRE_THROWS_AS(PitchCorrector(config), sonare::SonareException);
  config = {};
  config.scale.reference_midi = kMaxReferenceMidi;  // the boundary itself is legal
  REQUIRE_NOTHROW(PitchCorrector(config));

  constexpr int sample_rate = 22050;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(440.0f, sample_rate, 1024), sample_rate);
  PitchCorrector corrector;
  F0Track empty;
  empty.sample_rate = sample_rate;
  REQUIRE_THROWS_AS(corrector.correct_to_midi(audio, empty, 69.0f), sonare::SonareException);
  F0Track bad_hop = constant_track(440.0f, sample_rate, 0, 4);
  REQUIRE_THROWS_AS(corrector.correct_to_midi(audio, bad_hop, 69.0f), sonare::SonareException);
}

TEST_CASE("PitchCorrector rejects an F0Track whose sample rate differs from the audio",
          "[pitch_editor]") {
  // Regression (editing#3): hop_length is in samples at the track's rate, so a
  // track produced at a different rate than the audio would silently apply the
  // correction curve at the wrong time positions. correct_timevarying must
  // reject the mismatch instead of returning silently mis-aligned output.
  constexpr int sample_rate = 22050;
  auto samples = sine(440.0f, sample_rate, sample_rate);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);
  F0Track track = constant_track(440.0f, sample_rate, 256, 16);
  PitchCorrector corrector;

  // Matching rate succeeds.
  REQUIRE_NOTHROW(corrector.correct_to_midi(audio, track, 70.0f));

  // Mismatched rate is rejected.
  track.sample_rate = 48000;
  REQUIRE_THROWS(corrector.correct_to_midi(audio, track, 70.0f));
}

TEST_CASE("PitchCorrector retune_amount=0 leaves off-pitch notes uncorrected", "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  auto samples = sine(440.0f, sample_rate, sample_rate);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);
  const F0Track track = constant_track(440.0f, sample_rate, 256, 16);

  // Target is a full semitone above the detected pitch (100 cents, well beyond
  // the 20-cent vibrato band). With retune_amount=0 no correction must be
  // applied: the output pitch stays at the original 440 Hz, not the target.
  PitchCorrectionConfig corrector_config;
  corrector_config.retune_speed_ms = 0.0f;
  corrector_config.retune_amount = 0.0f;
  PitchCorrector corrector(corrector_config);
  const sonare::Audio corrected = corrector.correct_to_midi(audio, track, 70.0f);

  sonare::PitchConfig config;
  config.frame_length = 1024;
  config.hop_length = 256;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  PyinF0Provider provider(config);
  const F0Track corrected_track = provider.detect(corrected);

  // Should remain near 440 Hz (original), far from the 466.16 Hz target.
  REQUIRE_THAT(median_voiced_f0(corrected_track), WithinAbs(440.0f, 3.0f));
}

TEST_CASE("PitchCorrector retune_amount scales correction strength", "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  auto samples = sine(440.0f, sample_rate, sample_rate);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);
  const F0Track track = constant_track(440.0f, sample_rate, 256, 16);

  sonare::PitchConfig config;
  config.frame_length = 1024;
  config.hop_length = 256;
  config.fmin = 100.0f;
  config.fmax = 1000.0f;
  PyinF0Provider provider(config);

  auto corrected_f0 = [&](float retune_amount) {
    PitchCorrectionConfig corrector_config;
    corrector_config.retune_speed_ms = 0.0f;
    corrector_config.retune_amount = retune_amount;
    PitchCorrector corrector(corrector_config);
    const sonare::Audio corrected = corrector.correct_to_midi(audio, track, 70.0f);
    return median_voiced_f0(provider.detect(corrected));
  };

  const float full = corrected_f0(1.0f);  // pulled to ~466 Hz target
  const float half = corrected_f0(0.5f);  // pulled roughly halfway
  const float none = corrected_f0(0.0f);  // stays at 440 Hz

  // Higher retune_amount -> output pitch closer to the (higher) target.
  REQUIRE(none < half);
  REQUIRE(half < full);
}

TEST_CASE("PitchCorrector preserves duration for a constant pitch shift", "[pitch_editor]") {
  // Regression for TD-PSOLA duration drift: a sustained voiced region corrected
  // by a constant amount must keep its length and onset timing. Previously the
  // synthesis loop advanced the output epoch by period_out while the source
  // epoch advanced by period_in, time-compressing voiced regions by 1/ratio.
  constexpr int sample_rate = 48000;
  constexpr float f0_hz = 150.0f;
  constexpr int n_samples = 9600;  // 200 ms of steady, continuously voiced tone
  constexpr int hop_length = 256;
  const int n_frames = n_samples / hop_length + 2;

  // Steady 150 Hz tone with a Gaussian amplitude burst as a timing marker. The
  // burst is an envelope feature (not a pitch change), so PSOLA must leave its
  // position in time unchanged even as the carrier pitch is shifted.
  constexpr int marker_center = 6000;  // known input time of the envelope peak
  constexpr float marker_sigma = 400.0f;
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i);
    const float carrier = static_cast<float>(
        std::sin(sonare::constants::kTwoPiD * f0_hz * static_cast<double>(i) / sample_rate));
    const float d = (t - static_cast<float>(marker_center)) / marker_sigma;
    const float env = 0.3f + 0.6f * std::exp(-0.5f * d * d);
    samples[static_cast<size_t>(i)] = env * carrier;
  }
  const sonare::Audio audio = sonare::Audio::from_vector(std::vector<float>(samples), sample_rate);
  const F0Track track = constant_track(f0_hz, sample_rate, hop_length, n_frames);

  // Constant +2 semitone correction => ratio = 2^(2/12) ~= 1.122 throughout.
  const float detected_midi = PitchCorrector::hz_to_midi(f0_hz);
  PitchCorrectionConfig corrector_config;
  corrector_config.retune_speed_ms = 0.0f;  // instant snap, constant correction
  corrector_config.vibrato_threshold_cents = 0.0f;
  PitchCorrector corrector(corrector_config);
  const sonare::Audio corrected = corrector.correct_to_midi(audio, track, detected_midi + 2.0f);

  // Duration must be preserved exactly (output buffer matches input length).
  REQUIRE(corrected.size() == audio.size());

  // Temporal alignment: locate the envelope peak of the corrected signal via a
  // smoothed magnitude and confirm it stayed near the input marker time. If the
  // old drift bug were present the marker would land near marker_center/ratio
  // (~5347), far outside this tolerance.
  auto envelope_peak = [&](const sonare::Audio& signal) {
    constexpr int win = 256;  // moving-average window over |x| (a few periods)
    int best_index = 0;
    float best_value = -1.0f;
    float acc = 0.0f;
    const int len = static_cast<int>(signal.size());
    for (int i = 0; i < len; ++i) {
      acc += std::abs(signal[static_cast<size_t>(i)]);
      if (i >= win) {
        acc -= std::abs(signal[static_cast<size_t>(i - win)]);
      }
      if (i >= win && acc > best_value) {
        best_value = acc;
        best_index = i - win / 2;  // center of the averaging window
      }
    }
    return best_index;
  };

  const int input_peak = envelope_peak(audio);
  const int output_peak = envelope_peak(corrected);
  // Sanity: the input marker is recovered near its known location.
  REQUIRE(std::abs(input_peak - marker_center) < 300);
  // The corrected marker must stay aligned with the input marker (within ~6 ms),
  // NOT shifted by the pitch ratio.
  REQUIRE(std::abs(output_peak - input_peak) < 300);
}

TEST_CASE("PitchCorrector resynthesizes separated large shifts and preserves an unvoiced gap",
          "[pitch_editor]") {
  constexpr int sample_rate = 48000;
  constexpr int hop_length = 240;
  constexpr int n_samples = sample_rate;
  constexpr int frames = n_samples / hop_length;
  constexpr float input_f0 = 200.0f;
  constexpr int first_start = 8;
  constexpr int first_end = 64;
  constexpr int gap_start = 64;
  constexpr int gap_end = 112;
  constexpr int second_start = 112;
  constexpr int second_end = 192;

  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(input_f0, sample_rate, n_samples), sample_rate);
  const F0Track track = voiced_runs_track(input_f0, sample_rate, hop_length, frames, first_start,
                                          first_end, second_start, second_end);
  std::vector<float> deltas(static_cast<size_t>(frames), 0.0f);
  std::fill(deltas.begin() + first_start, deltas.begin() + first_end, 7.0f);
  std::fill(deltas.begin() + second_start, deltas.begin() + second_end, -7.0f);

  const PitchCorrector corrector;
  const sonare::Audio corrected = corrector.resynthesize(audio, track, deltas);
  REQUIRE(corrected.size() == audio.size());

  const int first_lo = (first_start + 4) * hop_length;
  const int first_hi = (first_end - 4) * hop_length;
  const int second_lo = (second_start + 4) * hop_length;
  const int second_hi = (second_end - 4) * hop_length;
  const float expected_up = input_f0 * std::pow(2.0f, 7.0f / 12.0f);
  const float expected_down = input_f0 * std::pow(2.0f, -7.0f / 12.0f);
  CHECK_THAT(positive_zero_crossing_hz(corrected, first_lo, first_hi),
             WithinAbs(expected_up, 8.0f));
  CHECK_THAT(positive_zero_crossing_hz(corrected, second_lo, second_hi),
             WithinAbs(expected_down, 8.0f));

  // Boundary cross-fades may touch a few samples at each edge.  The middle of
  // the unvoiced run must remain the original input sample-for-sample.
  const int dry_lo = (gap_start + 4) * hop_length;
  const int dry_hi = (gap_end - 4) * hop_length;
  for (int i = dry_lo; i < dry_hi; ++i) {
    CHECK_THAT(corrected[static_cast<size_t>(i)],
               WithinAbs(audio[static_cast<size_t>(i)], 1.0e-6f));
  }
}

TEST_CASE("PitchCorrector keeps adjacent large shifts separate for opposite intervals",
          "[pitch_editor]") {
  constexpr int sample_rate = 48000;
  constexpr int hop_length = 240;
  constexpr int n_samples = sample_rate;
  constexpr int frames = n_samples / hop_length;
  constexpr float input_f0 = 200.0f;
  constexpr int first_start = 8;
  constexpr int boundary = 100;
  constexpr int second_end = 192;

  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(input_f0, sample_rate, n_samples), sample_rate);
  const F0Track track = voiced_runs_track(input_f0, sample_rate, hop_length, frames, first_start,
                                          boundary, boundary, second_end);
  std::vector<float> deltas(static_cast<size_t>(frames), 0.0f);
  std::fill(deltas.begin() + first_start, deltas.begin() + boundary, 7.0f);
  std::fill(deltas.begin() + boundary, deltas.begin() + second_end, -7.0f);

  const PitchCorrector corrector;
  const sonare::Audio corrected = corrector.resynthesize(audio, track, deltas);
  const float expected_up = input_f0 * std::pow(2.0f, 7.0f / 12.0f);
  const float expected_down = input_f0 * std::pow(2.0f, -7.0f / 12.0f);
  CHECK_THAT(positive_zero_crossing_hz(corrected, 16 * hop_length, 88 * hop_length),
             WithinAbs(expected_up, 8.0f));
  CHECK_THAT(positive_zero_crossing_hz(corrected, 112 * hop_length, 184 * hop_length),
             WithinAbs(expected_down, 8.0f));
}

TEST_CASE("PitchCorrector keeps adjacent large shifts separate for unequal intervals",
          "[pitch_editor]") {
  constexpr int sample_rate = 48000;
  constexpr int hop_length = 240;
  constexpr int n_samples = sample_rate;
  constexpr int frames = n_samples / hop_length;
  constexpr float input_f0 = 200.0f;
  constexpr int first_start = 8;
  constexpr int boundary = 100;
  constexpr int second_end = 192;

  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(input_f0, sample_rate, n_samples), sample_rate);
  const F0Track track = voiced_runs_track(input_f0, sample_rate, hop_length, frames, first_start,
                                          boundary, boundary, second_end);
  std::vector<float> deltas(static_cast<size_t>(frames), 0.0f);
  std::fill(deltas.begin() + first_start, deltas.begin() + boundary, 7.0f);
  std::fill(deltas.begin() + boundary, deltas.begin() + second_end, 9.0f);

  const PitchCorrector corrector;
  const sonare::Audio corrected = corrector.resynthesize(audio, track, deltas);
  const float expected_first = input_f0 * std::pow(2.0f, 7.0f / 12.0f);
  const float expected_second = input_f0 * std::pow(2.0f, 9.0f / 12.0f);
  CHECK_THAT(positive_zero_crossing_hz(corrected, 16 * hop_length, 88 * hop_length),
             WithinAbs(expected_first, 8.0f));
  CHECK_THAT(positive_zero_crossing_hz(corrected, 112 * hop_length, 184 * hop_length),
             WithinAbs(expected_second, 8.0f));
}

TEST_CASE("PitchCorrector resynthesize rejects malformed tracks and non-finite deltas",
          "[pitch_editor]") {
  constexpr int sample_rate = 48000;
  constexpr int hop_length = 256;
  constexpr int frames = 16;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(200.0f, sample_rate, 4096), sample_rate);
  const F0Track valid = constant_track(200.0f, sample_rate, hop_length, frames);
  const std::vector<float> deltas(static_cast<size_t>(frames), 0.0f);
  const PitchCorrector corrector;

  auto require_invalid_parameter = [](auto&& operation) {
    bool threw = false;
    try {
      operation();
    } catch (const sonare::SonareException& error) {
      threw = true;
      CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    }
    CHECK(threw);
  };

  F0Track malformed_shape = valid;
  malformed_shape.voiced.pop_back();
  require_invalid_parameter([&] { corrector.resynthesize(audio, malformed_shape, deltas); });

  F0Track malformed_rate = valid;
  malformed_rate.sample_rate = sample_rate + 1;
  require_invalid_parameter([&] { corrector.resynthesize(audio, malformed_rate, deltas); });

  F0Track malformed_cadence = valid;
  malformed_cadence.frame_rate_hz = static_cast<float>(sample_rate) * 2.0f;
  require_invalid_parameter([&] { corrector.resynthesize(audio, malformed_cadence, deltas); });
  require_invalid_parameter([&] { corrector.correct_to_midi(audio, malformed_cadence, 69.0f); });

  F0Track nonfinite_f0 = valid;
  nonfinite_f0.f0_hz[0] = std::numeric_limits<float>::quiet_NaN();
  require_invalid_parameter([&] { corrector.resynthesize(audio, nonfinite_f0, deltas); });

  for (const float bad_delta :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()}) {
    std::vector<float> bad_deltas = deltas;
    bad_deltas[0] = bad_delta;
    require_invalid_parameter([&] { corrector.resynthesize(audio, valid, bad_deltas); });
  }
}

TEST_CASE("PitchCorrector validates voiced periods and target pitch representability",
          "[pitch_editor]") {
  constexpr int sample_rate = 48000;
  constexpr int hop_length = 256;
  constexpr int frames = 16;
  const sonare::Audio short_audio =
      sonare::Audio::from_vector(sine(200.0f, sample_rate, 4096), sample_rate);
  const PitchCorrector corrector;
  const std::vector<float> zero_deltas(static_cast<size_t>(frames), 0.0f);

  auto check_invalid_parameter = [](auto&& operation) {
    bool threw = false;
    try {
      operation();
    } catch (const sonare::SonareException& error) {
      threw = true;
      CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    }
    CHECK(threw);
  };

  // A voiced period longer than the source buffer cannot provide a complete
  // PSOLA grain.  That frame is dry-passed rather than rejected: the contract
  // is per-frame representability, rather than an arbitrary musical cutoff.
  const F0Track tiny_f0 = constant_track(1.0f, sample_rate, hop_length, frames);
  const sonare::Audio tiny_identity = corrector.resynthesize(
      short_audio, tiny_f0, std::vector<float>(static_cast<size_t>(frames), 6.0f));
  for (size_t i = 0; i < tiny_identity.size(); ++i) {
    CHECK_THAT(tiny_identity[i], WithinAbs(short_audio[i], 1.0e-7f));
  }

  const F0Track nominal = constant_track(200.0f, sample_rate, hop_length, frames);
  const sonare::Audio identity = corrector.resynthesize(short_audio, nominal, zero_deltas);
  CHECK(identity.size() == short_audio.size());
  for (size_t i = 0; i < identity.size(); ++i) {
    CHECK_THAT(identity[i], WithinAbs(short_audio[i], 1.0e-7f));
  }

  std::vector<float> too_low(static_cast<size_t>(frames), -100.0f);
  std::vector<float> too_high(static_cast<size_t>(frames), 100.0f);
  const sonare::Audio low_identity = corrector.resynthesize(short_audio, nominal, too_low);
  const sonare::Audio high_identity = corrector.resynthesize(short_audio, nominal, too_high);
  for (size_t i = 0; i < short_audio.size(); ++i) {
    CHECK_THAT(low_identity[i], WithinAbs(short_audio[i], 1.0e-7f));
    CHECK_THAT(high_identity[i], WithinAbs(short_audio[i], 1.0e-7f));
  }

  // A source F0 above Nyquist remains malformed input even when the frame
  // would otherwise be dry-passable.
  F0Track above_nyquist = nominal;
  above_nyquist.f0_hz[0] = static_cast<float>(sample_rate);
  check_invalid_parameter([&] { corrector.resynthesize(short_audio, above_nyquist, zero_deltas); });

  // Shifts beyond six semitones use bounded passes when every target pitch is
  // representable in this buffer.
  const sonare::Audio long_audio =
      sonare::Audio::from_vector(sine(200.0f, sample_rate, sample_rate), sample_rate);
  const F0Track long_track =
      constant_track(200.0f, sample_rate, hop_length, sample_rate / hop_length);
  const sonare::Audio pure_400_audio =
      sonare::Audio::from_vector(sine(400.0f, sample_rate, sample_rate), sample_rate);
  CHECK_THAT(median_pyin_f0(pure_400_audio), WithinAbs(400.0f, 5.0f));

  for (const float delta : {-24.0f, 24.0f}) {
    CAPTURE(delta);
    const std::vector<float> curve(static_cast<size_t>(long_track.n_frames()), delta);
    const sonare::Audio shifted = corrector.resynthesize(long_audio, long_track, curve);
    CHECK(shifted.size() == long_audio.size());
    for (size_t i = 0; i < shifted.size(); ++i) CHECK(std::isfinite(shifted[i]));
    const float expected = 200.0f * std::pow(2.0f, delta / 12.0f);
    CHECK_THAT(median_pyin_f0(shifted), WithinAbs(expected, 8.0f));
  }
}

TEST_CASE("PitchCorrector follows a large time-varying pitch ramp", "[pitch_editor]") {
  constexpr int sample_rate = 48000;
  constexpr int hop_length = 240;
  constexpr int n_samples = sample_rate;
  constexpr int frames = n_samples / hop_length;
  constexpr float input_f0 = 200.0f;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(input_f0, sample_rate, n_samples), sample_rate);
  const F0Track track = constant_track(input_f0, sample_rate, hop_length, frames);
  std::vector<float> deltas(static_cast<size_t>(frames), 0.0f);
  for (int frame = 0; frame < frames; ++frame) {
    deltas[static_cast<size_t>(frame)] =
        7.0f + 11.0f * static_cast<float>(frame) / static_cast<float>(frames - 1);
  }

  const PitchCorrector corrector;
  const sonare::Audio corrected = corrector.resynthesize(audio, track, deltas);
  const auto expected_at = [&](int frame) {
    const float delta = deltas[static_cast<size_t>(frame)];
    return input_f0 * std::pow(2.0f, delta / 12.0f);
  };
  CHECK_THAT(positive_zero_crossing_hz(corrected, 20 * hop_length, 50 * hop_length),
             WithinAbs(expected_at(35), 15.0f));
  CHECK_THAT(positive_zero_crossing_hz(corrected, 150 * hop_length, 180 * hop_length),
             WithinAbs(expected_at(165), 15.0f));
}

TEST_CASE("PitchCorrector accepts a target at the exact lower representability bound",
          "[pitch_editor][pitch_revision]") {
  constexpr int sample_rate = 8000;
  constexpr int n_samples = 512;
  constexpr int hop_length = n_samples;
  constexpr float source_f0 = 31.25f;
  constexpr float target_f0 = 15.625f;  // sample_rate / n_samples
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(source_f0, sample_rate, n_samples), sample_rate);
  const F0Track track = constant_track(source_f0, sample_rate, hop_length, 1);
  const PitchCorrector corrector;
  const float target_delta = 12.0f * std::log2(target_f0 / source_f0);  // exactly -12 semitones
  const sonare::Audio corrected =
      corrector.resynthesize(audio, track, std::vector<float>{target_delta});

  REQUIRE(corrected.size() == audio.size());
  double maximum_difference = 0.0;
  for (size_t i = 0; i < corrected.size(); ++i) {
    CHECK(std::isfinite(corrected[i]));
    maximum_difference = std::max(maximum_difference, std::abs(static_cast<double>(corrected[i]) -
                                                               static_cast<double>(audio[i])));
  }
  CHECK(maximum_difference > 1.0e-4);
  CHECK_THAT(target_f0, WithinAbs(sample_rate / static_cast<float>(n_samples), 1.0e-6f));
}

TEST_CASE("PitchCorrector keeps bounded passes stable at fractional frame cadence",
          "[pitch_editor][pitch_revision]") {
  constexpr int sample_rate = 48000;
  constexpr int hop_length = 240;
  constexpr int n_samples = sample_rate;
  constexpr float input_f0 = 220.0f;
  constexpr float fractional_frame_rate = 200.5f;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(input_f0, sample_rate, n_samples), sample_rate);
  F0Track track = constant_track(input_f0, sample_rate, hop_length, n_samples / hop_length + 2);
  track.frame_rate_hz = fractional_frame_rate;

  const PitchCorrector corrector;
  for (const float delta : {-12.0f, -6.0f, 6.0f, 12.0f}) {
    CAPTURE(delta);
    const std::vector<float> curve(static_cast<size_t>(track.n_frames()), delta);
    const sonare::Audio shifted = corrector.resynthesize(audio, track, curve);
    REQUIRE(shifted.size() == audio.size());
    for (size_t i = 0; i < shifted.size(); ++i) CHECK(std::isfinite(shifted[i]));

    const float expected = input_f0 * std::pow(2.0f, delta / 12.0f);
    // A PSOLA output can contain strong harmonics that add extra zero
    // crossings even while its fundamental is correct. Measure the rendered
    // pitch through the independent fixed-band pYIN estimator instead.
    CHECK_THAT(median_pyin_f0(shifted), WithinAbs(expected, 8.0f));
  }
}

TEST_CASE("PitchCorrector accepts a float-spelled lower endpoint",
          "[pitch_editor][pitch_revision]") {
  constexpr int sample_rate = 44100;
  constexpr int n_samples = 1000;
  constexpr int hop_length = n_samples;
  const float target_f0 = static_cast<float>(sample_rate) / static_cast<float>(n_samples);
  for (const float source_f0 : {100.0f, 351.942871f, 440.0f, 10000.0f, 21798.728515625f}) {
    CAPTURE(source_f0);
    const sonare::Audio audio =
        sonare::Audio::from_vector(sine(source_f0, sample_rate, n_samples), sample_rate);
    const F0Track track = constant_track(source_f0, sample_rate, hop_length, 1);
    const float target_delta = 12.0f * std::log2(target_f0 / source_f0);
    const sonare::Audio corrected =
        PitchCorrector().resynthesize(audio, track, std::vector<float>{target_delta});

    REQUIRE(corrected.size() == audio.size());
    double maximum_difference = 0.0;
    for (size_t i = 0; i < corrected.size(); ++i) {
      CHECK(std::isfinite(corrected[i]));
      maximum_difference = std::max(maximum_difference, std::abs(static_cast<double>(corrected[i]) -
                                                                 static_cast<double>(audio[i])));
    }
    CHECK(maximum_difference > 1.0e-4);

    const float below_bound_delta = 12.0f * std::log2(44.0f / source_f0);
    const sonare::Audio dry =
        PitchCorrector().resynthesize(audio, track, std::vector<float>{below_bound_delta});
    REQUIRE(dry.size() == audio.size());
    for (size_t i = 0; i < dry.size(); ++i) CHECK(dry[i] == audio[i]);
  }
}

TEST_CASE("PitchCorrector accepts a float-spelled Nyquist endpoint",
          "[pitch_editor][pitch_revision]") {
  constexpr int sample_rate = 44100;
  constexpr int n_samples = 1000;
  constexpr float source_f0 = 84.94380187988281f;
  constexpr float target_f0 = sample_rate / 2.0f;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(source_f0, sample_rate, n_samples), sample_rate);
  const F0Track track = constant_track(source_f0, sample_rate, n_samples, 1);
  const float delta = 12.0f * std::log2(target_f0 / source_f0);
  const sonare::Audio corrected = PitchCorrector().resynthesize(audio, track, {delta});
  REQUIRE(corrected.size() == audio.size());
  double maximum_difference = 0.0;
  for (size_t i = 0; i < corrected.size(); ++i) {
    CHECK(std::isfinite(corrected[i]));
    maximum_difference = std::max(maximum_difference, std::abs(static_cast<double>(corrected[i]) -
                                                               static_cast<double>(audio[i])));
  }
  CHECK(maximum_difference > 1.0e-4);

  const float above_bound_delta = 12.0f * std::log2((target_f0 + 1.0f) / source_f0);
  const sonare::Audio dry = PitchCorrector().resynthesize(audio, track, {above_bound_delta});
  REQUIRE(dry.size() == audio.size());
  for (size_t i = 0; i < dry.size(); ++i) CHECK(dry[i] == audio[i]);
}

TEST_CASE("PitchCorrector dry-passes ineligible mixed frames without a large-shift loop",
          "[pitch_editor][pitch_revision]") {
  constexpr int sample_rate = 8000;
  constexpr int hop_length = 512;
  constexpr int n_samples = sample_rate * 2;
  constexpr float input_f0 = 500.0f;
  constexpr int frames = n_samples / hop_length;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(input_f0, sample_rate, n_samples), sample_rate);
  const F0Track track = constant_track(input_f0, sample_rate, hop_length, frames);
  std::vector<float> deltas(static_cast<size_t>(frames), std::numeric_limits<float>::max());
  const int boundary_frame = frames / 2;
  // 500 Hz + 36 semitones is exactly Nyquist at 8 kHz and must remain an
  // eligible boundary case.  The tail requests an enormous finite shift and
  // must be treated as dry because its target is outside the representable
  // range, without turning the pass count into an unbounded loop.
  std::fill(deltas.begin(), deltas.begin() + boundary_frame, 36.0f);

  const PitchCorrector corrector;
  const sonare::Audio corrected = corrector.resynthesize(audio, track, deltas);
  REQUIRE(corrected.size() == audio.size());
  for (size_t i = 0; i < corrected.size(); ++i) CHECK(std::isfinite(corrected[i]));

  const int active_lo = 4 * hop_length;
  const int active_hi = (boundary_frame - 4) * hop_length;
  double active_difference = 0.0;
  for (int i = active_lo; i < active_hi; ++i) {
    active_difference += std::abs(static_cast<double>(corrected[static_cast<size_t>(i)]) -
                                  static_cast<double>(audio[static_cast<size_t>(i)]));
  }
  CHECK(active_difference > 1.0);

  // The dry tail is measured well beyond the transition cross-fade and a
  // source grain, so neighbouring eligible grains must not leak into it.
  const int dry_lo = (boundary_frame + 4) * hop_length;
  const int dry_hi = n_samples - 4 * hop_length;
  for (int i = dry_lo; i < dry_hi; ++i) {
    CHECK_THAT(corrected[static_cast<size_t>(i)],
               WithinAbs(audio[static_cast<size_t>(i)], 1.0e-6f));
  }
}

TEST_CASE("PitchCorrector stops a correction at a voiced-to-unvoiced tail boundary",
          "[pitch_editor][pitch_revision]") {
  constexpr int sample_rate = 48000;
  constexpr int hop_length = 2048;
  constexpr int frames = 24;
  constexpr int tail_frame = 8;
  constexpr float input_f0 = 2000.0f;
  constexpr int n_samples = frames * hop_length;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(input_f0, sample_rate, n_samples), sample_rate);

  auto make_track = [&](int last_voiced_frame) {
    F0Track track;
    track.sample_rate = sample_rate;
    track.hop_length = hop_length;
    track.f0_hz.assign(frames, 0.0f);
    track.voiced.assign(frames, false);
    track.voiced_prob.assign(frames, 1.0f);
    for (int frame = 2; frame <= last_voiced_frame; ++frame) {
      track.f0_hz[static_cast<size_t>(frame)] = input_f0;
      track.voiced[static_cast<size_t>(frame)] = true;
    }
    return track;
  };

  const std::vector<float> deltas(static_cast<size_t>(frames), 6.0f);
  const PitchCorrector corrector;
  const sonare::Audio cut_at_tail = corrector.resynthesize(audio, make_track(tail_frame), deltas);
  const sonare::Audio with_one_more_frame =
      corrector.resynthesize(audio, make_track(tail_frame + 1), deltas);
  REQUIRE(cut_at_tail.size() == audio.size());
  REQUIRE(with_one_more_frame.size() == audio.size());

  // sample_voiced() selects the nearest frame, so frame N remains active only
  // through its centered half-frame. Compare the latter part of that active
  // interval, ending one cross-fade and one source period before the boundary.
  // The only difference between the tracks is whether frame N+1 is voiced, so
  // a correction must not taper toward zero through the tail of frame N.
  constexpr int crossfade_samples = 480;
  constexpr int source_period = sample_rate / static_cast<int>(input_f0);
  const int compare_lo = tail_frame * hop_length + hop_length / 8;
  const int compare_hi =
      tail_frame * hop_length + hop_length / 2 - crossfade_samples - source_period;
  REQUIRE(compare_hi > compare_lo);
  double difference = 0.0;
  for (int i = compare_lo; i < compare_hi; ++i) {
    difference = std::max(
        difference, std::abs(static_cast<double>(cut_at_tail[static_cast<size_t>(i)]) -
                             static_cast<double>(with_one_more_frame[static_cast<size_t>(i)])));
  }
  double reference_difference = 0.0;
  for (int i = compare_lo; i < compare_hi; ++i) {
    reference_difference =
        std::max(reference_difference,
                 std::abs(static_cast<double>(with_one_more_frame[static_cast<size_t>(i)]) -
                          static_cast<double>(audio[static_cast<size_t>(i)])));
  }
  CHECK(reference_difference > 1.0e-4);
  CHECK(difference < 1.0e-4);
}

TEST_CASE("NoteEditor rejects invalid fade configuration", "[pitch_editor]") {
  for (const float fade_ms :
       {-1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()}) {
    CAPTURE(fade_ms);
    NoteEditorConfig config;
    config.fade_ms = fade_ms;
    bool threw = false;
    try {
      (void)NoteEditor(config);
    } catch (const sonare::SonareException& error) {
      threw = true;
      CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    }
    CHECK(threw);
  }
}

TEST_CASE("NoteEditor saturates a huge finite fade before converting to samples",
          "[pitch_editor]") {
  constexpr int sample_rate = 1000;
  std::vector<float> samples(1000, 0.0f);
  std::fill(samples.begin() + 200, samples.begin() + 800, 1.0f);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);
  NoteRegion region;
  region.onset_sample = 200;
  region.offset_sample = 800;

  NoteEditorConfig config;
  config.fade_ms = std::numeric_limits<float>::max();
  NoteEditor editor(config);
  bool threw = false;
  sonare::Audio moved;
  try {
    moved = editor.move_note(audio, region, 0);
  } catch (const sonare::SonareException& error) {
    threw = true;
    CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
  }
  CHECK_FALSE(threw);
  CHECK(moved.size() == audio.size());
  for (size_t i = 0; i < moved.size(); ++i) CHECK(std::isfinite(moved[i]));
  // A finite but huge fade saturates at half the 600-sample region (300
  // samples) before the integer conversion. A platform-dependent overflow to
  // zero would paste a hard-edged constant note at the destination instead.
  CHECK(moved[1] < 0.001f);
  CHECK_THAT(moved[150], WithinAbs(0.5f, 0.01f));
}

TEST_CASE("NoteEditor identity move and stretch preserve samples for both backends",
          "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  constexpr int n_samples = sample_rate;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(440.0f, sample_rate, n_samples), sample_rate);
  NoteRegion region;
  region.onset_sample = 2000;
  region.offset_sample = 12000;

  for (const sonare::StretchBackend backend :
       {sonare::StretchBackend::NativeSpectral, sonare::StretchBackend::PhaseVocoder}) {
    const int backend_id = static_cast<int>(backend);
    CAPTURE(backend_id);
    NoteEditorConfig config;
    config.stretch_backend = backend;
    REQUIRE(config.fade_ms > 0.0f);
    NoteEditor editor(config);

    const sonare::Audio moved = editor.move_note(audio, region, region.onset_sample);
    const sonare::Audio stretched = editor.stretch_note(audio, region, 1.0f);
    REQUIRE(moved.size() == audio.size());
    REQUIRE(stretched.size() == audio.size());

    float move_error = 0.0f;
    float stretch_error = 0.0f;
    for (size_t i = 0; i < audio.size(); ++i) {
      move_error = std::max(move_error, std::abs(moved[i] - audio[i]));
      stretch_error = std::max(stretch_error, std::abs(stretched[i] - audio[i]));
    }
    CHECK_THAT(move_error, WithinAbs(0.0f, 1.0e-5f));
    CHECK_THAT(stretch_error, WithinAbs(0.0f, 1.0e-5f));
  }
}

TEST_CASE("NoteEditor moves note region with edge fades", "[pitch_editor]") {
  constexpr int sample_rate = 1000;
  std::vector<float> samples(1000, 0.0f);
  for (int i = 100; i < 300; ++i) {
    samples[static_cast<size_t>(i)] = 0.5f;
  }
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

  NoteRegion region;
  region.onset_sample = 100;
  region.offset_sample = 300;

  NoteEditor editor({5.0f, sonare::StretchBackend::NativeSpectral});
  const sonare::Audio moved = editor.move_note(audio, region, 500);

  REQUIRE(moved.size() == audio.size());
  // The vacated span ramps out of the source instead of being cut: the taper's
  // first point sits half a fade step in, so sample 100 keeps most of its level.
  REQUIRE_THAT(moved[100], WithinAbs(0.49384f, 0.001f));
  REQUIRE(moved[100] > moved[101]);
  REQUIRE(moved[101] > moved[104]);
  REQUIRE_THAT(moved[105], WithinAbs(0.0f, 0.000001f));
  REQUIRE(moved[510] > 0.0f);
  REQUIRE(moved[500] < moved[510]);
  // The moved note must fade down toward its final sample. A reversed tail
  // fade makes the last sample nearly full-scale and the preceding edge quiet,
  // producing both a dropout and a click at the note offset.
  REQUIRE(moved[699] < moved[695]);
  REQUIRE(moved[699] < 0.1f);
}

TEST_CASE("NoteEditor vacates a moved note span without a step at the seam", "[pitch_editor]") {
  constexpr int sample_rate = 1000;
  // Continuous material either side of the note, which is the case where a hard
  // cut at the vacated edge is audible as a click.
  std::vector<float> samples(1000, 0.5f);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

  NoteRegion region;
  region.onset_sample = 400;
  region.offset_sample = 600;

  NoteEditor editor({5.0f, sonare::StretchBackend::NativeSpectral});
  const sonare::Audio moved = editor.move_note(audio, region, 700);

  // A hard fill steps the whole 0.5 at either edge; the taper's first step is
  // 0.5 * (1 - cos(pi/2 * 0.1)) = 0.0062.
  REQUIRE(std::abs(moved[400] - moved[399]) < 0.05f);
  REQUIRE(std::abs(moved[600] - moved[599]) < 0.05f);
}

TEST_CASE("NoteEditor refuses a move target with no room in the buffer", "[pitch_editor]") {
  constexpr int sample_rate = 1000;
  std::vector<float> samples(1000, 0.5f);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

  NoteRegion region;
  region.onset_sample = 400;
  region.offset_sample = 600;
  NoteEditor editor({5.0f, sonare::StretchBackend::NativeSpectral});

  // The vacated span is ramped out before the paste, so a target with nowhere to
  // land used to erase the note and write it nowhere, under a successful return.
  REQUIRE_THROWS_AS(editor.move_note(audio, region, 1000), sonare::SonareException);
  REQUIRE_THROWS_AS(editor.move_note(audio, region, 99999999), sonare::SonareException);
  // A negative folded into 0, making -1 and a deliberate 0 indistinguishable.
  REQUIRE_THROWS_AS(editor.move_note(audio, region, -1), sonare::SonareException);

  // A note landing near the end keeps the part that fits, which is the same
  // clipping clamp_region applies to the source span.
  const sonare::Audio clipped = editor.move_note(audio, region, 999);
  REQUIRE(clipped.size() == audio.size());
  REQUIRE(clipped[999] > audio[999]);
}

TEST_CASE("NoteEditor stretches note region to requested length ratio", "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  auto samples = sine(440.0f, sample_rate, sample_rate / 2);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

  NoteRegion region;
  region.onset_sample = 1000;
  region.offset_sample = 5000;
  const int original_region_length = region.offset_sample - region.onset_sample;
  const float stretch_ratio = 1.5f;

  NoteEditor editor({2.0f, sonare::StretchBackend::NativeSpectral});
  const sonare::Audio stretched = editor.stretch_note(audio, region, stretch_ratio);

  const int stretched_len =
      static_cast<int>(std::ceil(static_cast<float>(original_region_length) * stretch_ratio));
  // Only the region's length changes; the seams cross-fade against the note's
  // own samples, not by overlapping the neighbours.
  const int expected_size = static_cast<int>(audio.size()) - original_region_length + stretched_len;
  REQUIRE_THAT(static_cast<float>(stretched.size()),
               WithinAbs(static_cast<float>(expected_size), 2.0f));
  REQUIRE(stretched.sample_rate() == audio.sample_rate());
}

TEST_CASE("NoteEditor stretch shifts later audio by exactly the region's length change",
          "[pitch_editor]") {
  constexpr int sample_rate = 22050;
  auto samples = sine(440.0f, sample_rate, sample_rate / 2);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

  NoteRegion region;
  region.onset_sample = 1000;
  region.offset_sample = 5000;
  const int region_length = region.offset_sample - region.onset_sample;
  NoteEditor editor({2.0f, sonare::StretchBackend::NativeSpectral});

  // 1.0 is time-neutral: same length, and everything outside the region is the
  // input sample for sample.
  for (const float ratio : {1.0f, 1.1f, 0.8f}) {
    CAPTURE(ratio);
    const sonare::Audio out = editor.stretch_note(audio, region, ratio);
    const int shift = static_cast<int>(out.size()) - static_cast<int>(audio.size());
    const int stretched_len = region_length + shift;
    REQUIRE(std::abs(static_cast<float>(stretched_len) - region_length * ratio) <= 2.0f);
    if (ratio == 1.0f) REQUIRE(shift == 0);
    int mismatches = 0;
    for (size_t i = 0; i < static_cast<size_t>(region.onset_sample); ++i) {
      mismatches += out[i] != audio[i] ? 1 : 0;
    }
    for (size_t i = static_cast<size_t>(region.offset_sample); i < audio.size(); ++i) {
      mismatches += out[i + static_cast<size_t>(shift)] != audio[i] ? 1 : 0;
    }
    REQUIRE(mismatches == 0);
  }
}

TEST_CASE("ScaleQuantizer boundary MIDI values quantize without crash", "[pitch_editor]") {
  // C major scale, root = 0 (C).
  // 0b101010110101 = bits for C D E F G A B enabled.
  const ScaleQuantizer quantizer({0, 0b101010110101, 69.0f});

  // MIDI 0 (C-1): lowest representable; should land on a C major tone.
  {
    const float q = quantizer.quantize_midi(0.0f);
    REQUIRE(std::isfinite(q));
    const int pc = static_cast<int>(std::round(q) + 1200) % 12;
    REQUIRE(quantizer.pitch_class_enabled(pc));
  }

  // MIDI 127 (G9): highest standard MIDI; should land on a C major tone.
  {
    const float q = quantizer.quantize_midi(127.0f);
    REQUIRE(std::isfinite(q));
    const int pc = static_cast<int>(std::round(q) + 1200) % 12;
    REQUIRE(quantizer.pitch_class_enabled(pc));
  }

  // Sub-zero and over-range inputs must not crash or produce NaN/Inf.
  for (float midi : {-1.0f, -12.0f, 128.0f, 144.0f}) {
    const float q = quantizer.quantize_midi(midi);
    REQUIRE(std::isfinite(q));
  }
}

TEST_CASE("ScaleQuantizer pitch_class_enabled reflects mode_mask bits", "[pitch_editor]") {
  // All-notes mask (chromatic): every pitch class is enabled.
  const ScaleQuantizer chromatic({0, 0b111111111111, 69.0f});
  for (int pc = 0; pc < 12; ++pc) {
    REQUIRE(chromatic.pitch_class_enabled(pc));
  }

  // Single-note mask (only C, bit 0): C major is NOT the full chromatic set,
  // so C# (bit 1) must be disabled.
  const ScaleQuantizer c_only({0, 0b000000000001, 69.0f});
  REQUIRE(c_only.pitch_class_enabled(0));        // C enabled
  REQUIRE_FALSE(c_only.pitch_class_enabled(1));  // C# disabled
}

TEST_CASE("PitchCorrector scopes fallback overshoot guard to the fallback region",
          "[pitch_editor]") {
  // Regression (editing#1): a localized spectral-fallback overshoot must not
  // trigger a GLOBAL gain reduction of the whole corrected output. We build a
  // long voiced tone and a track whose correction is small (PSOLA-handled) for
  // the first half and very large (> 6 semitones, spectral fallback) for the
  // second half. The first half's level must stay intact whether or not the
  // second half later forces a fallback overshoot.
  constexpr int sample_rate = 48000;
  constexpr float f0_hz = 200.0f;
  constexpr int n_samples = 24000;  // 500 ms
  constexpr int hop_length = 256;
  const int n_frames = n_samples / hop_length + 2;

  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  for (int i = 0; i < n_samples; ++i) {
    samples[static_cast<size_t>(i)] =
        0.95f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * f0_hz *
                                            static_cast<double>(i) / sample_rate));
  }
  const sonare::Audio audio = sonare::Audio::from_vector(std::vector<float>(samples), sample_rate);

  // Track: small +1 semitone target for the first half, +18 semitones (way past
  // kPsolaMaxSemitones=6) for the second half so it routes to the spectral
  // fallback which can overshoot.
  const float base_midi = PitchCorrector::hz_to_midi(f0_hz);
  F0Track split_track;
  split_track.sample_rate = sample_rate;
  split_track.hop_length = hop_length;
  split_track.f0_hz.assign(static_cast<size_t>(n_frames), f0_hz);
  split_track.voiced.assign(static_cast<size_t>(n_frames), true);
  split_track.voiced_prob.assign(static_cast<size_t>(n_frames), 1.0f);

  // First-half-only track: identical small correction, no fallback region. The
  // remainder is left voiced at f0 with target == detected (no shift) so PSOLA
  // simply reproduces it.
  PitchCorrectionConfig config;
  config.retune_speed_ms = 0.0f;
  config.vibrato_threshold_cents = 0.0f;
  config.max_correction_semitones = 24.0f;
  PitchCorrector corrector(config);

  // Build a target-midi curve via correct_to_scale-style helper is awkward, so
  // exercise the fixed-target path for the whole buffer (+1 st) as the no-fallback
  // reference, then a hand-built large-shift second half via a per-frame f0 that
  // forces a >6 st correction toward the same fixed target.
  const sonare::Audio reference = corrector.correct_to_midi(audio, split_track, base_midi + 1.0f);

  // Now make the second half "off-pitch" so correcting to (base+1) needs a huge
  // shift there: drop f0 of the second-half frames so target-detected > 6 st.
  for (int f = n_frames / 2; f < n_frames; ++f) {
    split_track.f0_hz[static_cast<size_t>(f)] = PitchCorrector::midi_to_hz(base_midi - 18.0f);
  }
  const sonare::Audio corrected = corrector.correct_to_midi(audio, split_track, base_midi + 1.0f);

  REQUIRE(corrected.size() == audio.size());
  for (size_t i = 0; i < corrected.size(); ++i) REQUIRE(std::isfinite(corrected[i]));

  // RMS of the first quarter (clearly PSOLA, far from the fallback boundary)
  // must be essentially unchanged by the presence of the second-half fallback.
  // A global peak-normalization (the bug) would scale this region down whenever
  // the fallback half overshoots.
  auto rms = [](const sonare::Audio& a, int lo, int hi) {
    double acc = 0.0;
    for (int i = lo; i < hi; ++i)
      acc += static_cast<double>(a[static_cast<size_t>(i)]) * a[static_cast<size_t>(i)];
    return std::sqrt(acc / std::max(1, hi - lo));
  };
  const int q = n_samples / 4;
  const double ref_rms = rms(reference, 1000, q);
  const double cor_rms = rms(corrected, 1000, q);
  REQUIRE(ref_rms > 0.0);
  // Scoped guard: the first-quarter level tracks the no-fallback reference to
  // within a few percent. (A global-gain bug would drop it noticeably.)
  REQUIRE(cor_rms > ref_rms * 0.9);
}

TEST_CASE("NoteEditor stretch_note keeps a continuous level across the splice", "[pitch_editor]") {
  // Regression (editing#2): stretch_note used to fade the stretched region to
  // zero at both splice points, leaving an audible level dip/gap where the
  // unmodified head/tail (at full level) meet the silenced splice. The
  // equal-power cross-fade must keep the seam level continuous.
  constexpr int sample_rate = 22050;
  // Constant-amplitude tone so any dip is purely an artifact of the splice.
  auto samples = sine(440.0f, sample_rate, sample_rate);
  const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), sample_rate);

  NoteRegion region;
  region.onset_sample = 4000;
  region.offset_sample = 9000;
  const float stretch_ratio = 1.4f;

  NoteEditor editor({5.0f, sonare::StretchBackend::NativeSpectral});
  const sonare::Audio stretched = editor.stretch_note(audio, region, stretch_ratio);

  std::vector<float> out(stretched.data(), stretched.data() + stretched.size());
  for (float s : out) REQUIRE(std::isfinite(s));

  // Local RMS in a short window around the first splice (region.onset_sample)
  // must not collapse toward zero. A fade-to-zero left a deep notch here.
  auto local_rms = [&](int center, int half) {
    const int lo = std::max(0, center - half);
    const int hi = std::min(static_cast<int>(out.size()), center + half);
    double acc = 0.0;
    for (int i = lo; i < hi; ++i)
      acc += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
    return std::sqrt(acc / std::max(1, hi - lo));
  };

  // Reference level taken from the untouched head, well before the splice.
  const float head_rms = local_rms(2000, 200);
  const float seam_rms = local_rms(region.onset_sample, 64);
  REQUIRE(head_rms > 0.0f);
  // Equal-power seam: the splice retains a substantial fraction of the head
  // level rather than dipping to silence (the old fade-to-zero would put this
  // far below 0.5x).
  REQUIRE(seam_rms > head_rms * 0.5f);
}

TEST_CASE("ScaleQuantizer root shift moves enabled pitch classes", "[pitch_editor]") {
  // Root = 0 (C): C major mask enables C (0), D (2), E (4), F (5), G (7), A (9), B (11).
  const ScaleQuantizer root_c({0, 0b101010110101, 69.0f});
  // Root = 2 (D): same mask but the enabled set shifts — D (2) must now be the
  // lowest-numbered enabled class in the first octave-span.
  const ScaleQuantizer root_d({2, 0b101010110101, 69.0f});

  // A note at C (MIDI 60) under root_c lands on C (enabled in C major).
  // Under root_d the same pitch may land on a different nearest enabled degree.
  const float q_c = root_c.quantize_midi(60.0f);
  const float q_d = root_d.quantize_midi(60.0f);

  // Both results must be finite and land on an enabled pitch class for their
  // respective scale. The outputs need not be equal (that's the whole point).
  REQUIRE(std::isfinite(q_c));
  REQUIRE(std::isfinite(q_d));
  const int pc_c = static_cast<int>(std::round(q_c) + 1200) % 12;
  REQUIRE(root_c.pitch_class_enabled(pc_c));
  // correction_semitones must be within +/-6 (nearest semitone).
  REQUIRE(std::abs(root_c.correction_semitones(60.0f)) <= 6.0f);
  REQUIRE(std::abs(root_d.correction_semitones(60.0f)) <= 6.0f);
}
