/// @file pyin_voicing_contract_test.cpp
/// @brief The contract between pYIN's voicing outputs and their two consumers:
///        voiced_flag is the voicing decision, voiced_prob is an F0-dependent
///        observation mass that is neither a confidence nor a correction weight.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "util/constants.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kHopLength = 512;
constexpr int kFrameLength = 2048;

// Three harmonics at a constant amplitude: identical signal quality at every
// pitch, so anything that varies with the note is a property of the estimator,
// not of the material.
std::vector<float> harmonic_tone(float midi, float seconds = 2.0f) {
  const int n = static_cast<int>(seconds * kSampleRate);
  const double hz = sonare::constants::kA4Hz *
                    std::pow(2.0, (static_cast<double>(midi) - sonare::constants::kMidiA4) / 12.0);
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  double phase = 0.0;
  for (int i = 0; i < n; ++i) {
    phase += 2.0 * M_PI * hz / kSampleRate;
    out[static_cast<size_t>(i)] = static_cast<float>(
        0.3 * (std::sin(phase) + 0.5 * std::sin(2.0 * phase) + 0.25 * std::sin(3.0 * phase)));
  }
  return out;
}

struct Pyin {
  std::vector<float> f0;
  std::vector<float> voiced_prob;
  std::vector<float> voiced_flag;  // 0.0 / 1.0, the form note segmentation wants
  int n_frames = 0;
  double mean_prob = 0.0;
  bool all_voiced = true;
};

Pyin run_pyin(float midi) {
  const std::vector<float> samples = harmonic_tone(midi);
  SonarePitchResult raw{};
  REQUIRE(sonare_pitch_pyin(samples.data(), samples.size(), kSampleRate, kFrameLength, kHopLength,
                            65.0f, 2093.0f, 0.1f, /*fill_na=*/1, &raw) == SONARE_OK);
  Pyin out;
  out.n_frames = raw.n_frames;
  out.f0.assign(raw.f0, raw.f0 + raw.n_frames);
  out.voiced_prob.assign(raw.voiced_prob, raw.voiced_prob + raw.n_frames);
  out.voiced_flag.reserve(static_cast<size_t>(raw.n_frames));
  double sum = 0.0;
  for (int i = 0; i < raw.n_frames; ++i) {
    out.voiced_flag.push_back(raw.voiced_flag[i] != 0 ? 1.0f : 0.0f);
    out.all_voiced = out.all_voiced && raw.voiced_flag[i] != 0;
    sum += raw.voiced_prob[i];
  }
  out.mean_prob = raw.n_frames > 0 ? sum / raw.n_frames : 0.0;
  sonare_free_pitch_result(&raw);
  return out;
}

size_t segment_count(const std::vector<float>& f0, const std::vector<float>& voicing,
                     float voiced_threshold) {
  SonareNoteSegmenterConfig config{};
  config.struct_version = 2;
  config.voiced_threshold = voiced_threshold;
  SonareNoteSegmentsResult result{};
  REQUIRE(sonare_note_segments(f0.data(), f0.size(), voicing.data(), voicing.size(),
                               static_cast<float>(kSampleRate) / kHopLength, &config,
                               &result) == SONARE_OK);
  const size_t count = result.count;
  sonare_free_note_segments(&result);
  return count;
}

}  // namespace

TEST_CASE("pYIN voiced_prob tracks F0, not signal quality", "[feature][pitch][pyin]") {
  // The same three-harmonic tone at C3 and C5. voiced_flag is true throughout
  // both, so nothing about the voicing DECISION differs -- yet the probability
  // is far apart. This is librosa-faithful (the mass is the summed voiced
  // observation, and a long period leaves shallower CMNDF troughs inside a
  // fixed frame_length), and it is exactly why voiced_prob must not be read as
  // a confidence.
  const Pyin low = run_pyin(48.0f);   // C3
  const Pyin high = run_pyin(72.0f);  // C5
  REQUIRE(low.all_voiced);
  REQUIRE(high.all_voiced);
  REQUIRE(low.mean_prob < high.mean_prob);
  REQUIRE(low.mean_prob < 0.5);
  REQUIRE(high.mean_prob > 0.5);
}

TEST_CASE("Note segmentation follows voiced_flag across the whole register",
          "[feature][pitch][pyin]") {
  for (const float midi : {36.0f, 48.0f, 60.0f, 69.0f, 72.0f, 84.0f}) {
    const Pyin result = run_pyin(midi);
    REQUIRE(result.n_frames > 0);
    // The voicing decision segments every register.
    REQUIRE(segment_count(result.f0, result.voiced_flag, 0.5f) > 0);
  }
  // Feeding voiced_prob instead is the trap the docs warn about: below C5 the
  // default 0.5 threshold discards every frame and the call returns nothing at
  // all, with no error. A caller who must pass a probability track can lower
  // the threshold instead.
  const Pyin low = run_pyin(48.0f);
  REQUIRE(segment_count(low.f0, low.voiced_prob, 0.5f) == 0);
  REQUIRE(segment_count(low.f0, low.voiced_prob, 0.01f) > 0);
}

TEST_CASE("Time-varying pitch correction ignores voiced_prob", "[feature][pitch][pyin]") {
  const Pyin source = run_pyin(60.0f);  // C4
  const std::vector<float> samples = harmonic_tone(60.0f);
  std::vector<int32_t> voiced(source.voiced_flag.size());
  for (size_t i = 0; i < voiced.size(); ++i) {
    voiced[i] = source.voiced_flag[i] > 0.5f ? 1 : 0;
  }

  const auto correct = [&](const float* prob) {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_pitch_correct_to_midi_timevarying(
                samples.data(), samples.size(), kSampleRate, source.f0.data(), prob, voiced.data(),
                voiced.size(), kHopLength, 69.0f, &out, &out_length) == SONARE_OK);
    std::vector<float> result(out, out + out_length);
    sonare_free_floats(out);
    return result;
  };

  // Passing pYIN's voiced_prob straight through used to scale the correction by
  // that value, so the result fell short of the target by an amount that
  // depended on the source pitch. The two calls must now be identical.
  const std::vector<float> omitted = correct(nullptr);
  const std::vector<float> with_prob = correct(source.voiced_prob.data());
  REQUIRE(omitted.size() == with_prob.size());
  REQUIRE_FALSE(omitted.empty());
  for (size_t i = 0; i < omitted.size(); ++i) {
    REQUIRE(omitted[i] == with_prob[i]);
  }
}

TEST_CASE("Explicit voiced flags ignore invalid voiced_prob on both correction APIs",
          "[feature][pitch][pyin]") {
  const std::vector<float> samples = harmonic_tone(60.0f, 0.5f);
  const size_t n_frames = samples.size() / static_cast<size_t>(kHopLength) + 1;
  const std::vector<float> f0(n_frames, 220.0f);
  const std::vector<int32_t> voiced(n_frames, 1);

  const auto fixed = [&](const float* prob, const int32_t* flags) {
    float* out = nullptr;
    size_t out_length = 0;
    const SonareError error = sonare_pitch_correct_to_midi_timevarying(
        samples.data(), samples.size(), kSampleRate, f0.data(), prob, flags, n_frames, kHopLength,
        69.0f, &out, &out_length);
    std::vector<float> result;
    if (error == SONARE_OK) {
      result.assign(out, out + out_length);
      sonare_free_floats(out);
    }
    return std::pair<SonareError, std::vector<float>>{error, std::move(result)};
  };

  const auto general = [&](const float* prob, const int32_t* flags) {
    float* out = nullptr;
    size_t out_length = 0;
    const SonareError error = sonare_pitch_correct_timevarying(
        samples.data(), samples.size(), kSampleRate, f0.data(), prob, flags, n_frames, kHopLength,
        nullptr, &out, &out_length);
    std::vector<float> result;
    if (error == SONARE_OK) {
      result.assign(out, out + out_length);
      sonare_free_floats(out);
    }
    return std::pair<SonareError, std::vector<float>>{error, std::move(result)};
  };

  const auto fixed_without_prob = fixed(nullptr, voiced.data());
  const auto general_without_prob = general(nullptr, voiced.data());
  REQUIRE(fixed_without_prob.first == SONARE_OK);
  REQUIRE(general_without_prob.first == SONARE_OK);

  for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), -0.1f, 1.1f}) {
    const std::vector<float> invalid_prob(n_frames, invalid);
    const auto fixed_with_invalid_prob = fixed(invalid_prob.data(), voiced.data());
    const auto general_with_invalid_prob = general(invalid_prob.data(), voiced.data());
    REQUIRE(fixed_with_invalid_prob.first == SONARE_OK);
    REQUIRE(general_with_invalid_prob.first == SONARE_OK);
    REQUIRE(fixed_with_invalid_prob.second == fixed_without_prob.second);
    REQUIRE(general_with_invalid_prob.second == general_without_prob.second);
  }

  // The probability-only path still owns the [0,1] and finite-value contract.
  for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), -0.1f, 1.1f}) {
    const std::vector<float> invalid_prob(n_frames, invalid);
    REQUIRE(fixed(invalid_prob.data(), nullptr).first == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(general(invalid_prob.data(), nullptr).first == SONARE_ERROR_INVALID_PARAMETER);
  }
}

// When `voiced` is NULL, voiced_prob has to take over deriving the per-frame
// voicing decision (>= 0.5 is voiced) -- the same rule sonare_note_segments
// applies -- rather than every frame defaulting to voiced regardless of what
// voiced_prob says. Driven through acceptance of pYIN's own NaN-for-unvoiced
// convention: valid_pitch_track_f0 accepts a NaN frame only when that frame is
// NOT treated as voiced, so whether a low- or high-probability NaN frame is
// accepted is a direct, deterministic readout of the voicing decision the
// implementation actually made -- no need to inspect internal state.
TEST_CASE("Pitch correction derives voicing from voiced_prob when voiced is NULL",
          "[feature][pitch][pyin]") {
  constexpr size_t kFrames = 8;
  const std::vector<float> samples = harmonic_tone(60.0f, 1.0f);
  const std::vector<float> nan_f0(kFrames, std::numeric_limits<float>::quiet_NaN());

  SECTION("sonare_pitch_correct_to_midi_timevarying") {
    const auto correct = [&](const std::vector<float>& prob) {
      float* out = nullptr;
      size_t out_length = 0;
      const SonareError err = sonare_pitch_correct_to_midi_timevarying(
          samples.data(), samples.size(), kSampleRate, nan_f0.data(), prob.data(),
          /*voiced=*/nullptr, kFrames, kHopLength, 69.0f, &out, &out_length);
      if (err == SONARE_OK) sonare_free_floats(out);
      return err;
    };
    // Below 0.5: every frame reads as unvoiced, so the canonical unvoiced NaN
    // representation is accepted -- this used to fail with voiced forced true.
    REQUIRE(correct(std::vector<float>(kFrames, 0.1f)) == SONARE_OK);
    // At/above 0.5: every frame reads as voiced, so a voiced frame carrying
    // NaN (no measured pitch) is rejected, the same as an explicit voiced=1.
    REQUIRE(correct(std::vector<float>(kFrames, 0.9f)) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(correct(std::vector<float>(kFrames, 0.5f)) == SONARE_ERROR_INVALID_PARAMETER);
    // voiced_prob absent too: every frame defaults to voiced, so NaN is
    // rejected exactly like the all-voiced case above.
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_pitch_correct_to_midi_timevarying(
                samples.data(), samples.size(), kSampleRate, nan_f0.data(), /*voiced_prob=*/nullptr,
                /*voiced=*/nullptr, kFrames, kHopLength, 69.0f, &out,
                &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("sonare_pitch_correct_timevarying") {
    const auto correct = [&](const std::vector<float>& prob) {
      float* out = nullptr;
      size_t out_length = 0;
      const SonareError err = sonare_pitch_correct_timevarying(
          samples.data(), samples.size(), kSampleRate, nan_f0.data(), prob.data(),
          /*voiced=*/nullptr, kFrames, kHopLength, /*config=*/nullptr, &out, &out_length);
      if (err == SONARE_OK) sonare_free_floats(out);
      return err;
    };
    REQUIRE(correct(std::vector<float>(kFrames, 0.1f)) == SONARE_OK);
    REQUIRE(correct(std::vector<float>(kFrames, 0.9f)) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(correct(std::vector<float>(kFrames, 0.5f)) == SONARE_ERROR_INVALID_PARAMETER);
  }
}
