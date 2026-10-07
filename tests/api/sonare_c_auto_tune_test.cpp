/// @file sonare_c_auto_tune_test.cpp
/// @brief Tests for sonare_scale_mask_for_mode and sonare_auto_tune.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "util/constants.h"

TEST_CASE("sonare_scale_mask_for_mode returns the mode's scale, root-independent", "[c_api]") {
  const struct {
    SonareMode mode;
    uint16_t mask;
  } kRows[] = {
      {SONARE_MODE_MAJOR, 0b101010110101},   {SONARE_MODE_MINOR, 0b010110101101},
      {SONARE_MODE_DORIAN, 0b011010101101},  {SONARE_MODE_PHRYGIAN, 0b010110101011},
      {SONARE_MODE_LYDIAN, 0b101011010101},  {SONARE_MODE_MIXOLYDIAN, 0b011010110101},
      {SONARE_MODE_LOCRIAN, 0b010101101011},
  };
  for (const auto& row : kRows) {
    for (int root : {0, 5, 11}) {
      uint16_t mask = 0xFFFF;
      REQUIRE(sonare_scale_mask_for_mode(root, row.mode, &mask) == SONARE_OK);
      REQUIRE(mask == row.mask);
    }
  }
}

TEST_CASE("sonare_scale_mask_for_mode refuses out-of-domain arguments", "[c_api]") {
  uint16_t mask = 0xFFFF;
  REQUIRE(sonare_scale_mask_for_mode(-1, SONARE_MODE_MAJOR, &mask) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(mask == 0);
  REQUIRE(sonare_scale_mask_for_mode(12, SONARE_MODE_MAJOR, &mask) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_scale_mask_for_mode(0, static_cast<SonareMode>(7), &mask) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_scale_mask_for_mode(0, static_cast<SonareMode>(-1), &mask) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_scale_mask_for_mode(0, SONARE_MODE_MAJOR, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
}

#ifdef SONARE_WITH_PITCH_EDITOR

namespace {

constexpr int kSampleRate = 22050;
constexpr float kNoteSeconds = 0.5f;
// pYIN quantizes pitch to 10-cent bins; the corrected pitch is judged to within one bin.
constexpr float kToleranceCents = 10.0f;
constexpr float kDetuneCents = 35.0f;

// C major melody (MIDI numbers), every note in the scale.
const std::vector<int> kMelody = {60, 64, 67, 64, 62, 65, 69, 67, 60, 64, 67, 72};

std::vector<float> detuned_melody(float detune_cents) {
  std::vector<float> out;
  const int note_samples = static_cast<int>(kNoteSeconds * kSampleRate);
  for (int midi : kMelody) {
    const double hz = 440.0 * std::pow(2.0, (midi - 69 + detune_cents / 100.0) / 12.0);
    for (int i = 0; i < note_samples; ++i) {
      const double t = static_cast<double>(i) / kSampleRate;
      double v = 0.0;
      for (int h = 1; h <= 3; ++h) v += std::sin(sonare::constants::kTwoPiD * hz * h * t) / h;
      // Short fades keep the note joins from reading as pitch.
      const double fade = std::min({1.0, i / 200.0, (note_samples - i) / 200.0});
      out.push_back(static_cast<float>(0.3 * fade * v));
    }
  }
  return out;
}

float median_midi_of_note(const std::vector<float>& audio, size_t note) {
  const size_t note_samples = static_cast<size_t>(kNoteSeconds * kSampleRate);
  const size_t begin = note * note_samples + note_samples / 2;
  const size_t end = (note + 1) * note_samples - note_samples / 8;
  SonarePitchResult pitch{};
  REQUIRE(sonare_pitch_pyin(audio.data() + begin, end - begin, kSampleRate, 2048, 512, 65.0f,
                            2093.0f, 0.1f, 0, &pitch) == SONARE_OK);
  std::vector<float> midi;
  for (int i = 0; i < pitch.n_frames; ++i) {
    if (pitch.voiced_flag[i] != 0 && std::isfinite(pitch.f0[i])) {
      midi.push_back(69.0f + 12.0f * std::log2(pitch.f0[i] / 440.0f));
    }
  }
  sonare_free_pitch_result(&pitch);
  REQUIRE(!midi.empty());
  std::sort(midi.begin(), midi.end());
  return midi[midi.size() / 2];
}

}  // namespace

TEST_CASE("sonare_auto_tune lands a detuned melody on the key's scale", "[c_api]") {
  const std::vector<float> input = detuned_melody(kDetuneCents);
  const SonareKey key{SONARE_PITCH_C, SONARE_MODE_MAJOR, 0.0f};

  float* out = nullptr;
  size_t out_length = 0;
  SonareKey used{};
  REQUIRE(sonare_auto_tune(input.data(), input.size(), kSampleRate, &key, nullptr, &out,
                           &out_length, &used) == SONARE_OK);
  REQUIRE(out_length == input.size());
  REQUIRE(used.root == SONARE_PITCH_C);
  REQUIRE(used.mode == SONARE_MODE_MAJOR);
  REQUIRE(used.confidence == 1.0f);
  const std::vector<float> corrected(out, out + out_length);
  sonare_free_floats(out);

  float worst_before = 0.0f;
  float worst_after = 0.0f;
  for (size_t note = 0; note < kMelody.size(); ++note) {
    const float target = static_cast<float>(kMelody[note]);
    worst_before = std::max(worst_before, std::abs(median_midi_of_note(input, note) - target));
    worst_after = std::max(worst_after, std::abs(median_midi_of_note(corrected, note) - target));
  }
  INFO("worst error before " << worst_before * 100.0f << " cents, after " << worst_after * 100.0f);
  REQUIRE(worst_before * 100.0f > kDetuneCents - 10.0f);
  REQUIRE(worst_after * 100.0f < kToleranceCents);
}

TEST_CASE("sonare_auto_tune with no key equals passing the detected key", "[c_api]") {
  const std::vector<float> input = detuned_melody(kDetuneCents);

  float* detected_out = nullptr;
  size_t detected_length = 0;
  SonareKey detected{};
  REQUIRE(sonare_auto_tune(input.data(), input.size(), kSampleRate, nullptr, nullptr, &detected_out,
                           &detected_length, &detected) == SONARE_OK);

  SonareKey reference{};
  REQUIRE(sonare_detect_key(input.data(), input.size(), kSampleRate, &reference) == SONARE_OK);
  REQUIRE(detected.root == reference.root);
  REQUIRE(detected.mode == reference.mode);
  REQUIRE(detected.confidence == reference.confidence);

  float* named_out = nullptr;
  size_t named_length = 0;
  SonareKey named{};
  REQUIRE(sonare_auto_tune(input.data(), input.size(), kSampleRate, &reference, nullptr, &named_out,
                           &named_length, &named) == SONARE_OK);
  REQUIRE(named_length == detected_length);
  REQUIRE(std::equal(detected_out, detected_out + detected_length, named_out));
  sonare_free_floats(detected_out);
  sonare_free_floats(named_out);
}

TEST_CASE("sonare_auto_tune strength 0 leaves the audio unchanged", "[c_api]") {
  const std::vector<float> input = detuned_melody(kDetuneCents);
  SonarePitchCorrectionConfig config{};
  REQUIRE(sonare_pitch_correction_config_default(&config) == SONARE_OK);
  config.retune_amount = 0.0f;
  const SonareKey key{SONARE_PITCH_C, SONARE_MODE_MAJOR, 0.0f};
  float* out = nullptr;
  size_t out_length = 0;
  SonareKey used{};
  REQUIRE(sonare_auto_tune(input.data(), input.size(), kSampleRate, &key, &config, &out,
                           &out_length, &used) == SONARE_OK);
  REQUIRE(out_length == input.size());
  float worst = 0.0f;
  for (size_t i = 0; i < input.size(); ++i) worst = std::max(worst, std::abs(out[i] - input[i]));
  sonare_free_floats(out);
  REQUIRE(worst < 1.0e-3f);
}

TEST_CASE("sonare_auto_tune refuses bad arguments and defines its outputs", "[c_api]") {
  const std::vector<float> input = detuned_melody(0.0f);
  float poison = 0.0f;
  float* out = &poison;
  size_t out_length = 7;
  SonareKey used{SONARE_PITCH_B, SONARE_MODE_LOCRIAN, 0.5f};
  const SonareKey bad_root{static_cast<SonarePitchClass>(12), SONARE_MODE_MAJOR, 1.0f};
  const SonareKey bad_mode{SONARE_PITCH_C, static_cast<SonareMode>(7), 1.0f};
  SonarePitchCorrectionConfig config{};
  REQUIRE(sonare_pitch_correction_config_default(&config) == SONARE_OK);
  config.retune_amount = 1.5f;
  const SonareKey good{SONARE_PITCH_C, SONARE_MODE_MAJOR, 1.0f};

  REQUIRE(sonare_auto_tune(input.data(), input.size(), kSampleRate, &bad_root, nullptr, &out,
                           &out_length, &used) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_length == 0);
  REQUIRE(sonare_auto_tune(input.data(), input.size(), kSampleRate, &bad_mode, nullptr, &out,
                           &out_length, &used) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_auto_tune(input.data(), input.size(), kSampleRate, &good, &config, &out,
                           &out_length, &used) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_auto_tune(input.data(), input.size(), kSampleRate, &good, nullptr, &out,
                           &out_length, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_auto_tune(nullptr, 0, kSampleRate, &good, nullptr, &out, &out_length, &used) !=
          SONARE_OK);
}

#endif
