/// @file source_classifier_test.cpp
/// @brief Source classification tests for the mixing assistant.

#include "mixing/assistant/source_classifier.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "mixing/assistant/track_profile.h"
#include "util/constants.h"

namespace assistant = sonare::mixing::assistant;
using assistant::SourceClass;
using sonare::constants::kTwoPi;

namespace {

constexpr int kSampleRate = 48000;

// Feature values a profile carries into the classifier. Occupancy is given
// unnormalised and normalised by the builder, so a row reads as a balance
// between bands rather than as numbers that must add up.
struct ProfileSpec {
  std::array<float, assistant::kBandCount> occupancy{};
  float flatness = 0.0f;
  float rolloff_hz = 0.0f;
  float sustain_ratio = 0.0f;
  float attack_density = 0.0f;
  float crest_factor_db = 0.0f;
};

assistant::TrackProfile make_profile(const ProfileSpec& spec, const std::string& name = "") {
  assistant::TrackProfile profile;
  profile.strip_id = "strip";
  profile.name = name;
  profile.usable = true;
  profile.duration_sec = 1.0f;

  const float total = std::accumulate(spec.occupancy.begin(), spec.occupancy.end(), 0.0f);
  for (std::size_t band = 0; band < spec.occupancy.size(); ++band) {
    profile.band_occupancy[band] = total > 0.0f ? spec.occupancy[band] / total : 0.0f;
  }
  profile.base.spectral.flatness = spec.flatness;
  profile.base.spectral.rolloff_hz = spec.rolloff_hz;
  profile.base.dynamics.sustain_ratio = spec.sustain_ratio;
  profile.base.dynamics.attack_density = spec.attack_density;
  profile.base.loudness.crest_factor_db = spec.crest_factor_db;
  return profile;
}

// Each spec below carries only what makes its class that class: register,
// how much of the note survives the attack, and how tonal it is.

ProfileSpec kick_spec() {
  return {{{0.45f, 0.40f, 0.08f, 0.05f, 0.02f, 0.0f, 0.0f}}, 0.05f, 500.0f, 0.12f, 3.0f, 16.0f};
}

// Same register as the kick, but held and tonal.
ProfileSpec bass_spec() {
  return {{{0.25f, 0.55f, 0.12f, 0.06f, 0.02f, 0.0f, 0.0f}}, 0.06f, 800.0f, 0.75f, 1.5f, 6.0f};
}

ProfileSpec vocal_spec() {
  return {{{0.0f, 0.10f, 0.25f, 0.45f, 0.15f, 0.05f, 0.0f}}, 0.06f, 4000.0f, 0.70f, 2.0f, 12.0f};
}

// A register above the voice, with its weight in the presence band.
ProfileSpec lead_spec() {
  return {{{0.0f, 0.0f, 0.08f, 0.32f, 0.42f, 0.18f, 0.0f}}, 0.12f, 9000.0f, 0.70f, 2.0f, 10.0f};
}

ProfileSpec hihat_spec() {
  return {{{0.0f, 0.0f, 0.0f, 0.0f, 0.15f, 0.60f, 0.25f}}, 0.55f, 15000.0f, 0.15f, 8.0f, 14.0f};
}

// The same bright noise as the hi-hat, left to ring and struck rarely.
ProfileSpec cymbal_spec() {
  return {{{0.0f, 0.0f, 0.0f, 0.0f, 0.20f, 0.45f, 0.35f}}, 0.50f, 16000.0f, 0.65f, 0.6f, 11.0f};
}

// Energy spread evenly over every band, noisy, half sustained: a wash that is
// not defined by any register. No row claims it, which is the point.
ProfileSpec unclassifiable_spec() {
  return {{{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}}, 0.70f, 15000.0f, 0.50f, 0.5f, 5.0f};
}

// Inside the kick row on every bound, but only just: enough sub-low energy,
// barely short enough, barely peaky enough.
ProfileSpec marginal_kick_spec() {
  return {{{0.60f, 0.06f, 0.20f, 0.14f, 0.0f, 0.0f, 0.0f}}, 0.05f, 600.0f, 0.34f, 3.0f, 8.2f};
}

// The same shape with real headroom on each of those bounds, but still less
// than the definitive kick above.
ProfileSpec modest_kick_spec() {
  return {{{0.62f, 0.10f, 0.18f, 0.10f, 0.0f, 0.0f, 0.0f}}, 0.05f, 600.0f, 0.28f, 3.0f, 10.0f};
}

void require_self_consistent(const assistant::SourceClassification& classification) {
  REQUIRE(classification.confidence >= 0.0f);
  REQUIRE(classification.confidence <= 1.0f);
  if (classification.source == SourceClass::Unknown) {
    REQUIRE(classification.confidence == 0.0f);
  } else {
    REQUIRE(classification.confidence > 0.0f);
  }
}

// --- Synthetic signals for the end-to-end path ------------------------------
//
// Each one carries the defining trait of its class and nothing else. They are
// not imitations of instruments: a kick here is a low decaying sine with an
// edge on it, which is exactly what the kick row claims to recognise.

constexpr float kSignalSeconds = 1.5f;
constexpr float kKickToneHz = 60.0f;    // the register that defines a kick
constexpr int kKickHitsPerSecond = 4;   // sparse enough that the hits never overlap
constexpr float kKickDecaySec = 0.03f;  // the hit is over long before the next one
constexpr float kKickAmplitude = 0.8f;
constexpr float kClickAmplitude = 0.5f;
constexpr int kClickSamples = 24;  // the transient edge, roughly half a millisecond

constexpr std::array<float, 4> kBassNoteHz = {55.0f, 82.41f, 110.0f, 65.41f};
constexpr float kBassNoteSeconds = 0.375f;
constexpr float kBassAmplitude = 0.5f;
constexpr float kBassHarmonicLevel = 0.3f;  // enough for a pitched timbre, not a bright one

// White noise shaped into the register a hi-hat and a cymbal share. One-pole
// slopes on each side are gentle enough that the result is still recognisably
// noise rather than a narrow band that would read as tonal.
constexpr float kNoiseHighpassHz = 5000.0f;
constexpr float kNoiseLowpassHz = 9000.0f;
constexpr unsigned kNoiseSeed = 20250821u;

constexpr int kHatHitsPerSecond = 8;  // dense, as a hi-hat pattern is
constexpr float kHatBurstSeconds = 0.025f;
constexpr float kHatDecaySec = 0.008f;  // choked
constexpr float kHatAmplitude = 0.6f;
constexpr float kCymbalDecaySec = 0.7f;  // rings for most of the signal
constexpr float kCymbalAmplitude = 0.7f;

std::size_t sample_count(float seconds) {
  return static_cast<std::size_t>(seconds * static_cast<float>(kSampleRate));
}

float seconds_at(std::size_t index) {
  return static_cast<float>(index) / static_cast<float>(kSampleRate);
}

std::vector<float> kick_signal() {
  std::vector<float> out(sample_count(kSignalSeconds), 0.0f);
  const std::size_t period = static_cast<std::size_t>(kSampleRate / kKickHitsPerSecond);
  for (std::size_t start = 0; start < out.size(); start += period) {
    for (std::size_t n = 0; n < period && start + n < out.size(); ++n) {
      const float t = seconds_at(n);
      float sample =
          kKickAmplitude * std::exp(-t / kKickDecaySec) * std::sin(kTwoPi * kKickToneHz * t);
      if (n < static_cast<std::size_t>(kClickSamples)) {
        sample +=
            kClickAmplitude * (1.0f - static_cast<float>(n) / static_cast<float>(kClickSamples));
      }
      out[start + n] += sample;
    }
  }
  return out;
}

std::vector<float> bass_signal() {
  std::vector<float> out(sample_count(kSignalSeconds), 0.0f);
  // A continuous phase accumulator: the note changes without an amplitude step,
  // so the track stays sustained instead of reading as a string of onsets.
  float phase = 0.0f;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::size_t note =
        static_cast<std::size_t>(seconds_at(i) / kBassNoteSeconds) % kBassNoteHz.size();
    phase += kTwoPi * kBassNoteHz[note] / static_cast<float>(kSampleRate);
    out[i] = kBassAmplitude * (std::sin(phase) + kBassHarmonicLevel * std::sin(2.0f * phase));
  }
  return out;
}

std::vector<float> bright_noise(std::size_t count) {
  std::mt19937 rng(kNoiseSeed);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float> out(count, 0.0f);
  for (float& sample : out) sample = dist(rng);

  const float rate = static_cast<float>(kSampleRate);
  const float highpass_pole = std::exp(-kTwoPi * kNoiseHighpassHz / rate);
  float previous_in = 0.0f;
  float previous_out = 0.0f;
  for (float& sample : out) {
    const float input = sample;
    previous_out = highpass_pole * (previous_out + input - previous_in);
    previous_in = input;
    sample = previous_out;
  }

  const float lowpass_coefficient = 1.0f - std::exp(-kTwoPi * kNoiseLowpassHz / rate);
  float state = 0.0f;
  for (float& sample : out) {
    state += lowpass_coefficient * (sample - state);
    sample = state;
  }

  float peak = 0.0f;
  for (const float sample : out) peak = std::max(peak, std::abs(sample));
  if (peak > 0.0f) {
    for (float& sample : out) sample /= peak;
  }
  return out;
}

std::vector<float> hihat_signal() {
  const std::size_t count = sample_count(kSignalSeconds);
  const std::vector<float> noise = bright_noise(count);
  std::vector<float> out(count, 0.0f);
  const std::size_t period = static_cast<std::size_t>(kSampleRate / kHatHitsPerSecond);
  const std::size_t burst = sample_count(kHatBurstSeconds);
  for (std::size_t start = 0; start < count; start += period) {
    for (std::size_t n = 0; n < burst && start + n < count; ++n) {
      out[start + n] = kHatAmplitude * std::exp(-seconds_at(n) / kHatDecaySec) * noise[start + n];
    }
  }
  return out;
}

std::vector<float> cymbal_signal() {
  const std::size_t count = sample_count(kSignalSeconds);
  const std::vector<float> noise = bright_noise(count);
  std::vector<float> out(count, 0.0f);
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = kCymbalAmplitude * std::exp(-seconds_at(i) / kCymbalDecaySec) * noise[i];
  }
  return out;
}

assistant::TrackProfile profile_of(const std::vector<float>& samples) {
  assistant::TrackInput input;
  input.id = "track";
  input.left = samples.data();
  input.frame_count = samples.size();
  input.sample_rate = kSampleRate;
  return assistant::analyze_track_profile(input);
}

}  // namespace

TEST_CASE("kick and bass profiles are not taken for each other", "[mixing][assistant]") {
  const auto kick = assistant::classify_source(make_profile(kick_spec()));
  const auto bass = assistant::classify_source(make_profile(bass_spec()));

  REQUIRE(kick.source == SourceClass::Kick);
  REQUIRE(bass.source == SourceClass::Bass);
  require_self_consistent(kick);
  require_self_consistent(bass);
}

TEST_CASE("hi-hat and cymbal profiles are not taken for each other", "[mixing][assistant]") {
  const auto hihat = assistant::classify_source(make_profile(hihat_spec()));
  const auto cymbal = assistant::classify_source(make_profile(cymbal_spec()));

  REQUIRE(hihat.source == SourceClass::HiHat);
  REQUIRE(cymbal.source == SourceClass::Cymbal);
  require_self_consistent(hihat);
  require_self_consistent(cymbal);
}

TEST_CASE("vocal and lead profiles are not taken for each other", "[mixing][assistant]") {
  const auto vocal = assistant::classify_source(make_profile(vocal_spec()));
  const auto lead = assistant::classify_source(make_profile(lead_spec()));

  REQUIRE(vocal.source == SourceClass::Vocal);
  REQUIRE(lead.source == SourceClass::Lead);
  require_self_consistent(vocal);
  require_self_consistent(lead);
}

TEST_CASE("a track matching no rule stays Unknown", "[mixing][assistant]") {
  const auto classification = assistant::classify_source(make_profile(unclassifiable_spec()));

  REQUIRE(classification.source == SourceClass::Unknown);
  REQUIRE(classification.confidence == 0.0f);
}

TEST_CASE("a rule matched on its bounds is reported as Unknown", "[mixing][assistant]") {
  // The marginal profile satisfies every bound of the kick rule, so the table
  // does match it -- and the confidence that match earns is too low to act on.
  // Reporting Unknown here is the whole reason confidence is a margin rather
  // than a constant per class.
  const auto marginal = assistant::classify_source(make_profile(marginal_kick_spec()));
  REQUIRE(marginal.source == SourceClass::Unknown);
  REQUIRE(marginal.confidence == 0.0f);
}

TEST_CASE("confidence grows with the margin the rule was matched by", "[mixing][assistant]") {
  const auto definitive = assistant::classify_source(make_profile(kick_spec()));
  const auto modest = assistant::classify_source(make_profile(modest_kick_spec()));

  REQUIRE(definitive.source == SourceClass::Kick);
  REQUIRE(modest.source == SourceClass::Kick);
  REQUIRE(definitive.confidence > modest.confidence);
}

TEST_CASE("an agreeing track name raises confidence", "[mixing][assistant]") {
  const auto anonymous = assistant::classify_source(make_profile(vocal_spec()));
  const auto named = assistant::classify_source(make_profile(vocal_spec(), "VOX 01"));

  REQUIRE(named.source == anonymous.source);
  REQUIRE(named.confidence > anonymous.confidence);
}

TEST_CASE("a contradicting track name lowers confidence without changing the class",
          "[mixing][assistant]") {
  const auto anonymous = assistant::classify_source(make_profile(vocal_spec()));
  const auto misnamed = assistant::classify_source(make_profile(vocal_spec(), "Kick In"));

  // The measurement still decides what the track is; the disagreement is
  // expressed as less certainty, not as a different answer.
  REQUIRE(misnamed.source == SourceClass::Vocal);
  REQUIRE(misnamed.confidence < anonymous.confidence);
  REQUIRE(misnamed.confidence > 0.0f);
}

TEST_CASE("a track name alone cannot select a class", "[mixing][assistant]") {
  const auto named = assistant::classify_source(make_profile(unclassifiable_spec(), "Kick In"));

  REQUIRE(named.source == SourceClass::Unknown);
  REQUIRE(named.confidence == 0.0f);
}

TEST_CASE("name hints match case-insensitively on a substring", "[mixing][assistant]") {
  const auto spaced = assistant::classify_source(make_profile(vocal_spec(), "Kick In"));
  const auto upper = assistant::classify_source(make_profile(vocal_spec(), "KICK_01"));
  const auto bare = assistant::classify_source(make_profile(vocal_spec(), "kick"));

  REQUIRE(spaced.confidence == upper.confidence);
  REQUIRE(spaced.confidence == bare.confidence);
}

TEST_CASE("a name naming its own class outweighs the other class it mentions",
          "[mixing][assistant]") {
  const auto anonymous = assistant::classify_source(make_profile(bass_spec()));
  const auto named = assistant::classify_source(make_profile(bass_spec(), "bass gtr"));

  REQUIRE(named.source == SourceClass::Bass);
  REQUIRE(named.confidence > anonymous.confidence);
}

TEST_CASE("an unusable track is never classified", "[mixing][assistant]") {
  auto profile = make_profile(kick_spec(), "Kick In");
  profile.usable = false;
  profile.exclusion_reason = "silent";

  const auto classification = assistant::classify_source(profile);
  REQUIRE(classification.source == SourceClass::Unknown);
  REQUIRE(classification.confidence == 0.0f);
}

TEST_CASE("classify_sources fills every profile in place", "[mixing][assistant]") {
  std::vector<assistant::TrackProfile> profiles = {
      make_profile(kick_spec()),
      make_profile(vocal_spec()),
      make_profile(unclassifiable_spec()),
  };

  assistant::classify_sources(profiles);

  REQUIRE(profiles[0].source == SourceClass::Kick);
  REQUIRE(profiles[0].source_confidence > 0.0f);
  REQUIRE(profiles[1].source == SourceClass::Vocal);
  REQUIRE(profiles[1].source_confidence > 0.0f);
  REQUIRE(profiles[2].source == SourceClass::Unknown);
  REQUIRE(profiles[2].source_confidence == 0.0f);
}

TEST_CASE("a synthesised kick is never handed to a neighbouring class", "[mixing][assistant]") {
  const std::vector<float> samples = kick_signal();
  const assistant::TrackProfile profile = profile_of(samples);
  REQUIRE(profile.usable);

  const auto classification = assistant::classify_source(profile);
  require_self_consistent(classification);
  REQUIRE(classification.source != SourceClass::Bass);
  REQUIRE(classification.source != SourceClass::HiHat);
  REQUIRE(classification.source != SourceClass::Cymbal);
  REQUIRE(classification.source != SourceClass::Vocal);
}

TEST_CASE("a synthesised bass line is never handed to a neighbouring class",
          "[mixing][assistant]") {
  const std::vector<float> samples = bass_signal();
  const assistant::TrackProfile profile = profile_of(samples);
  REQUIRE(profile.usable);

  const auto classification = assistant::classify_source(profile);
  require_self_consistent(classification);
  REQUIRE(classification.source != SourceClass::Kick);
  REQUIRE(classification.source != SourceClass::HiHat);
  REQUIRE(classification.source != SourceClass::Cymbal);
}

TEST_CASE("a synthesised hi-hat is never handed to a neighbouring class", "[mixing][assistant]") {
  const std::vector<float> samples = hihat_signal();
  const assistant::TrackProfile profile = profile_of(samples);
  REQUIRE(profile.usable);

  const auto classification = assistant::classify_source(profile);
  require_self_consistent(classification);
  REQUIRE(classification.source != SourceClass::Cymbal);
  REQUIRE(classification.source != SourceClass::Kick);
  REQUIRE(classification.source != SourceClass::Bass);
}

TEST_CASE("a synthesised cymbal is never handed to a neighbouring class", "[mixing][assistant]") {
  const std::vector<float> samples = cymbal_signal();
  const assistant::TrackProfile profile = profile_of(samples);
  REQUIRE(profile.usable);

  const auto classification = assistant::classify_source(profile);
  require_self_consistent(classification);
  REQUIRE(classification.source != SourceClass::HiHat);
  REQUIRE(classification.source != SourceClass::Kick);
  REQUIRE(classification.source != SourceClass::Bass);
}

TEST_CASE("a silent track is excluded before classification", "[mixing][assistant]") {
  const std::vector<float> samples(sample_count(kSignalSeconds), 0.0f);
  const assistant::TrackProfile profile = profile_of(samples);
  REQUIRE_FALSE(profile.usable);

  const auto classification = assistant::classify_source(profile);
  REQUIRE(classification.source == SourceClass::Unknown);
  REQUIRE(classification.confidence == 0.0f);
}
