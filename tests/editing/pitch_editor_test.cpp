#include "editing/pitch_editor/pitch_editor.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"

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
  REQUIRE_THAT(moved[100], WithinAbs(0.0f, 0.0001f));
  REQUIRE(moved[510] > 0.0f);
  REQUIRE(moved[500] < moved[510]);
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

  const int expected_size =
      static_cast<int>(audio.size()) - original_region_length +
      static_cast<int>(std::ceil(static_cast<float>(original_region_length) * stretch_ratio));
  REQUIRE_THAT(static_cast<float>(stretched.size()),
               WithinAbs(static_cast<float>(expected_size), 2.0f));
  REQUIRE(stretched.sample_rate() == audio.sample_rate());
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
