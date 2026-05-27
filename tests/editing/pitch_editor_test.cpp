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
