#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "mastering/api/presets.h"
#include "mastering/assistant/audio_profile.h"
#include "mastering/assistant/config_from_params.h"
#include "mastering/assistant/platform_targets.h"
#include "mastering/assistant/suggester.h"
#include "mixing/assistant/mix_eval.h"
#include "support/rate_material.h"
#include "util/constants.h"
#include "util/db.h"

namespace assistant = sonare::mastering::assistant;

TEST_CASE("Assistant does not suggest denoise for measured silence", "[mastering][assistant]") {
  constexpr int sample_rate = 48000;
  const std::vector<float> silence(sample_rate, 0.0f);
  assistant::AssistantConfig config;
  config.enable_repair = true;
  const auto mono = assistant::suggest_chain(silence.data(), silence.size(), sample_rate, config);
  const std::vector<float> stereo_silence(silence.size() * 2, 0.0f);
  const auto stereo = assistant::suggest_chain_interleaved(stereo_silence.data(), silence.size(), 2,
                                                           sample_rate, config);
  for (const auto* result : {&mono, &stereo}) {
    REQUIRE(result->profile.defects.measured);
    REQUIRE(std::isinf(result->profile.loudness.integrated_lufs));
    REQUIRE(result->profile.loudness.integrated_lufs < 0.0f);
    CHECK_FALSE(result->config.repair.denoise.enabled);
    CHECK(std::none_of(result->explanation.begin(), result->explanation.end(),
                       [](const std::string& text) { return text.find("denoise:") == 0; }));
  }
}

TEST_CASE("Assistant refuses a shared declip threshold marked unsafe", "[mastering][assistant]") {
  assistant::AudioProfile profile;
  profile.defects.measured = true;
  profile.defects.clip_flat_run_count = 10;
  profile.defects.clip_flat_level = 0.25f;
  profile.defects.declip_threshold_safe = false;
  assistant::AssistantConfig config;
  config.enable_repair = true;
  const auto unsafe = assistant::suggest_chain(profile, config);
  CHECK_FALSE(unsafe.config.repair.declip.enabled);
  CHECK(std::any_of(unsafe.explanation.begin(), unsafe.explanation.end(),
                    [](const std::string& text) { return text.find("declip withheld:") == 0; }));
  profile.defects.declip_threshold_safe = true;
  const auto safe = assistant::suggest_chain(profile, config);
  CHECK(safe.config.repair.declip.enabled);
  CHECK(safe.config.repair.declip.config.clip_threshold == 0.25f);
}

namespace {

std::vector<float> declip_material(float ceiling, float drive, float burst_peak) {
  constexpr int kRate = 48000;
  std::vector<float> samples(4 * kRate);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    samples[i] =
        std::clamp(drive * std::sin(sonare::constants::kTwoPi * 220.0f * t), -ceiling, ceiling);
  }
  if (burst_peak > 0.0f) {
    const auto burst = static_cast<std::size_t>(0.05f * kRate);
    for (std::size_t i = 0; i < burst; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(kRate);
      const float window =
          std::sin(sonare::constants::kPi * static_cast<float>(i) / static_cast<float>(burst));
      samples[samples.size() / 2 + i] =
          burst_peak * window * std::sin(sonare::constants::kTwoPi * 1000.0f * t);
    }
  }
  return samples;
}

}  // namespace

TEST_CASE("Assistant withholds declip when unclipped audio is louder than the pinned level",
          "[mastering][assistant]") {
  constexpr int kRate = 48000;
  const std::vector<float> mono = declip_material(0.1f, 0.3f, 0.56f);
  std::vector<float> stereo(mono.size() * 2);
  for (std::size_t i = 0; i < mono.size(); ++i) {
    stereo[2 * i] = mono[i];
    stereo[2 * i + 1] = mono[i];
  }
  assistant::AssistantConfig config;
  config.enable_repair = true;

  const auto mono_result = assistant::suggest_chain(mono.data(), mono.size(), kRate, config);
  const auto stereo_result =
      assistant::suggest_chain_interleaved(stereo.data(), mono.size(), 2, kRate, config);
  for (const auto* result : {&mono_result, &stereo_result}) {
    CHECK(result->profile.defects.clip_flat_run_count > 0);
    CHECK_FALSE(result->profile.defects.declip_threshold_safe);
    CHECK_FALSE(result->config.repair.declip.enabled);
    CHECK(std::any_of(result->explanation.begin(), result->explanation.end(),
                      [](const std::string& text) { return text.find("declip withheld:") == 0; }));
  }
}

TEST_CASE("Assistant proposes declip for an over-unity plateau and an asymmetric clip",
          "[mastering][assistant]") {
  constexpr int kRate = 48000;
  assistant::AssistantConfig config;
  config.enable_repair = true;

  const std::vector<float> over_unity = declip_material(1.5f, 4.5f, 0.0f);
  const auto over = assistant::suggest_chain(over_unity.data(), over_unity.size(), kRate, config);
  CHECK(over.profile.defects.declip_threshold_safe);
  CHECK(over.config.repair.declip.enabled);
  CHECK(over.config.repair.declip.config.clip_threshold == 1.0f);

  std::vector<float> asymmetric(4 * kRate);
  for (std::size_t i = 0; i < asymmetric.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    asymmetric[i] =
        std::clamp(0.9f * std::sin(sonare::constants::kTwoPi * 220.0f * t), -0.4f, 0.5f);
  }
  const auto skewed = assistant::suggest_chain(asymmetric.data(), asymmetric.size(), kRate, config);
  CHECK(skewed.profile.defects.declip_threshold_safe);
  CHECK(skewed.config.repair.declip.enabled);
  CHECK(skewed.config.repair.declip.config.clip_threshold == 0.5f);
}

namespace {

using sonare::constants::kTwoPi;

std::vector<float> tone(int sr, float seconds, float frequency, float amplitude = 0.4f) {
  std::vector<float> samples(static_cast<size_t>(seconds * static_cast<float>(sr)));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = amplitude * std::sin(kTwoPi * frequency * t);
  }
  return samples;
}

void add_tone(std::vector<float>& samples, int sr, float frequency, float amplitude) {
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] += amplitude * std::sin(kTwoPi * frequency * t);
  }
}

void add_clicks(std::vector<float>& samples, int sr, float bpm, float amplitude) {
  const int period = static_cast<int>((60.0f / bpm) * static_cast<float>(sr));
  if (period <= 0) return;
  for (size_t pos = 0; pos < samples.size(); pos += static_cast<size_t>(period)) {
    for (size_t n = 0; n < 64 && pos + n < samples.size(); ++n) {
      const float env = 1.0f - static_cast<float>(n) / 64.0f;
      samples[pos + n] += amplitude * env;
    }
  }
}

std::vector<float> rhythmic_track(int sr, float seconds, float bpm, float bass_hz,
                                  float click_amp) {
  auto samples = tone(sr, seconds, bass_hz, 0.35f);
  add_tone(samples, sr, bass_hz * 2.0f, 0.12f);
  add_clicks(samples, sr, bpm, click_amp);
  return samples;
}

std::vector<float> ambient_track(int sr, float seconds) {
  auto samples = tone(sr, seconds, 180.0f, 0.24f);
  add_tone(samples, sr, 360.0f, 0.12f);
  add_tone(samples, sr, 540.0f, 0.08f);
  return samples;
}

std::vector<float> speech_like(int sr, float seconds) {
  std::vector<float> samples(static_cast<size_t>(seconds * static_cast<float>(sr)));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    const float syllable = 0.55f + 0.45f * std::max(0.0f, std::sin(kTwoPi * 4.0f * t));
    samples[i] =
        syllable * (0.20f * std::sin(kTwoPi * 170.0f * t) + 0.16f * std::sin(kTwoPi * 900.0f * t) +
                    0.08f * std::sin(kTwoPi * 1800.0f * t));
  }
  return samples;
}

}  // namespace

TEST_CASE("Assistant AudioProfile captures spectral and dynamics differences",
          "[mastering][assistant]") {
  constexpr int sr = 22050;
  assistant::AudioProfileConfig cfg;
  cfg.n_fft = 1024;
  cfg.hop_length = 256;

  const auto low_samples = tone(sr, 2.0f, 220.0f);
  const auto low =
      assistant::analyze_audio_profile(low_samples.data(), low_samples.size(), sr, cfg);
  const auto high_samples = tone(sr, 2.0f, 4000.0f);
  const auto high =
      assistant::analyze_audio_profile(high_samples.data(), high_samples.size(), sr, cfg);
  REQUIRE(high.spectral.centroid_hz > low.spectral.centroid_hz * 4.0f);

  auto transient_samples = tone(sr, 2.0f, 220.0f, 0.12f);
  add_clicks(transient_samples, sr, 120.0f, 0.8f);
  const auto steady =
      assistant::analyze_audio_profile(low_samples.data(), low_samples.size(), sr, cfg);
  const auto transient =
      assistant::analyze_audio_profile(transient_samples.data(), transient_samples.size(), sr, cfg);
  REQUIRE(transient.dynamics.attack_density > steady.dynamics.attack_density);
  REQUIRE(transient.loudness.true_peak_db > steady.loudness.true_peak_db - 12.0f);
}

TEST_CASE("Assistant AudioProfile measures a stereo pair with channel summing",
          "[mastering][assistant]") {
  constexpr int sr = 22050;
  assistant::AudioProfileConfig cfg;
  cfg.n_fft = 1024;
  cfg.hop_length = 256;

  const auto left = tone(sr, 4.0f, 440.0f, 0.3f);
  const auto right = tone(sr, 4.0f, 761.0f, 0.3f);
  std::vector<float> interleaved(left.size() * 2);
  std::vector<float> downmix(left.size());
  for (size_t index = 0; index < left.size(); ++index) {
    interleaved[2 * index] = left[index];
    interleaved[2 * index + 1] = right[index];
    downmix[index] = 0.5f * (left[index] + right[index]);
  }

  const auto stereo =
      assistant::analyze_audio_profile_interleaved(interleaved.data(), left.size(), 2, sr, cfg);
  const auto mono = assistant::analyze_audio_profile(downmix.data(), downmix.size(), sr, cfg);

  // The loudness block follows BS.1770 channel summing, so a decorrelated pair
  // reads 6.02 dB above the downmix a mono caller would have to build.
  REQUIRE_THAT(stereo.loudness.integrated_lufs - mono.loudness.integrated_lufs,
               Catch::Matchers::WithinAbs(6.02f, 0.3f));

  // Everything else describes spectral shape and timing rather than absolute
  // level, so it is measured on the same downmix and must match exactly.
  REQUIRE(stereo.duration_sec == mono.duration_sec);
  REQUIRE(stereo.spectral.centroid_hz == mono.spectral.centroid_hz);
  REQUIRE(stereo.spectral.flatness == mono.spectral.flatness);
  REQUIRE(stereo.spectral.rolloff_hz == mono.spectral.rolloff_hz);
  REQUIRE(stereo.dynamics.attack_density == mono.dynamics.attack_density);
  REQUIRE(stereo.dynamics.sustain_ratio == mono.dynamics.sustain_ratio);
  REQUIRE(stereo.bpm == mono.bpm);

  // The short-term spread is one of those, and the two entry points reach it by
  // different routes: the mono profile reduces the series its own loudness pass
  // produced, the stereo one measures the downmix separately because its loudness
  // describes the channel-summed program. Equal to the bit or one of them is
  // reading a series that does not belong to the signal it describes.
  REQUIRE(stereo.dynamics.short_term_lufs_std == mono.dynamics.short_term_lufs_std);
}

TEST_CASE("Assistant stereo entry points reject degenerate input", "[mastering][assistant]") {
  const std::vector<float> samples(256, 0.1f);
  REQUIRE(assistant::analyze_audio_profile_interleaved(nullptr, 64, 2, 22050).duration_sec == 0.0f);
  REQUIRE(assistant::analyze_audio_profile_interleaved(samples.data(), 0, 2, 22050).duration_sec ==
          0.0f);
  REQUIRE(assistant::analyze_audio_profile_interleaved(samples.data(), 64, 0, 22050).duration_sec ==
          0.0f);
  REQUIRE(assistant::analyze_audio_profile_interleaved(samples.data(), 64, 2, 0).duration_sec ==
          0.0f);
  REQUIRE(assistant::suggest_chain_interleaved(nullptr, 64, 2, 22050).profile.duration_sec == 0.0f);
}

TEST_CASE("Assistant suggest_chain_interleaved builds on the stereo profile",
          "[mastering][assistant]") {
  constexpr int sr = 22050;
  const auto left = tone(sr, 4.0f, 440.0f, 0.3f);
  const auto right = tone(sr, 4.0f, 761.0f, 0.3f);
  std::vector<float> interleaved(left.size() * 2);
  std::vector<float> downmix(left.size());
  for (size_t index = 0; index < left.size(); ++index) {
    interleaved[2 * index] = left[index];
    interleaved[2 * index + 1] = right[index];
    downmix[index] = 0.5f * (left[index] + right[index]);
  }

  const auto stereo = assistant::suggest_chain_interleaved(interleaved.data(), left.size(), 2, sr);
  const auto mono = assistant::suggest_chain(downmix.data(), downmix.size(), sr);
  REQUIRE_THAT(stereo.profile.loudness.integrated_lufs,
               Catch::Matchers::WithinAbs(assistant::analyze_audio_profile_interleaved(
                                              interleaved.data(), left.size(), 2, sr)
                                              .loudness.integrated_lufs,
                                          0.001f));
  REQUIRE(stereo.profile.loudness.integrated_lufs > mono.profile.loudness.integrated_lufs);
  REQUIRE(!stereo.explanation.empty());
}

TEST_CASE("Assistant chain does not follow the material outside repair", "[mastering][assistant]") {
  // Dark, dynamic, bass-heavy, fast and transient-dense all at once: none of it
  // may move the chain, because no measured rule maps those readings to stages.
  assistant::AudioProfile extreme;
  extreme.bpm = 128.0f;
  extreme.loudness.lra_lu = 14.0f;
  extreme.spectral.centroid_hz = 1200.0f;
  extreme.spectral.low_rms_db = -18.0f;
  extreme.spectral.mid_rms_db = -20.0f;
  extreme.spectral.air_rms_db = -42.0f;
  extreme.dynamics.attack_density = 2.8f;
  extreme.dynamics.short_term_lufs_std = 5.0f;

  assistant::AssistantConfig cfg;
  cfg.target_lufs = -13.0f;
  cfg.ceiling_db = -0.8f;
  const auto result = assistant::suggest_chain(extreme, cfg);

  REQUIRE(result.config.loudness.enabled);
  REQUIRE(result.config.loudness.target_lufs == -13.0f);
  REQUIRE(result.config.loudness.ceiling_db == -0.8f);
  REQUIRE(sonare::mastering::api::chain_config_to_json(result.config) ==
          sonare::mastering::api::chain_config_to_json(
              assistant::suggest_chain(assistant::AudioProfile{}, cfg).config));
  REQUIRE(result.explanation ==
          std::vector<std::string>{"base preset: streaming",
                                   "target loudness and ceiling applied from AssistantConfig"});
}

TEST_CASE("Assistant exposes speech mono-maker amount", "[mastering][assistant]") {
  assistant::AudioProfile profile;

  assistant::AssistantConfig cfg;
  cfg.preset = sonare::mastering::api::Preset::Speech;
  cfg.speech_mono_amount = 0.35f;
  auto result = assistant::suggest_chain(profile, cfg);

  REQUIRE(result.config.dynamics.deesser.enabled);
  REQUIRE(result.config.stereo.mono_maker.enabled);
  REQUIRE(result.config.stereo.mono_maker.config.amount == 0.35f);
}

namespace {

template <typename Callable>
void require_invalid_parameter(Callable call) {
  try {
    call();
    FAIL("expected InvalidParameter");
  } catch (const sonare::SonareException& error) {
    REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
  }
}

}  // namespace

TEST_CASE("Assistant rejects non-finite suggestion controls at the profile boundary",
          "[mastering][assistant][validation]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (const float value : {nan, inf, -inf}) {
    assistant::AssistantConfig target;
    target.target_lufs = value;
    require_invalid_parameter(
        [&] { (void)assistant::suggest_chain(assistant::AudioProfile{}, target); });

    assistant::AssistantConfig ceiling;
    ceiling.ceiling_db = value;
    require_invalid_parameter(
        [&] { (void)assistant::suggest_chain(assistant::AudioProfile{}, ceiling); });

    assistant::AssistantConfig speech;
    speech.preset = sonare::mastering::api::Preset::Speech;
    speech.speech_mono_amount = value;
    require_invalid_parameter(
        [&] { (void)assistant::suggest_chain(assistant::AudioProfile{}, speech); });
  }
}

TEST_CASE("Assistant keeps finite loudness values unrestricted and clamps speech amount",
          "[mastering][assistant][validation]") {
  assistant::AssistantConfig finite;
  finite.target_lufs = -100.0f;
  finite.ceiling_db = -100.0f;
  const auto finite_result = assistant::suggest_chain(assistant::AudioProfile{}, finite);
  REQUIRE(finite_result.config.loudness.target_lufs == -100.0f);
  REQUIRE(finite_result.config.loudness.ceiling_db == -100.0f);
  REQUIRE_NOTHROW(sonare::mastering::api::MasteringChain{finite_result.config});

  for (const auto& [amount, expected] : {std::pair{-1.0f, 0.0f}, std::pair{2.0f, 1.0f}}) {
    assistant::AssistantConfig speech;
    speech.preset = sonare::mastering::api::Preset::Speech;
    speech.speech_mono_amount = amount;
    const auto result = assistant::suggest_chain(assistant::AudioProfile{}, speech);
    REQUIRE(result.config.stereo.mono_maker.config.amount == expected);
    REQUIRE_NOTHROW(sonare::mastering::api::MasteringChain{result.config});
  }
}

TEST_CASE("Assistant validates controls through raw Audio and interleaved entry points",
          "[mastering][assistant][validation]") {
  constexpr int sample_rate = 48000;
  const auto samples = tone(sample_rate, 0.1f, 440.0f);
  const sonare::Audio audio =
      sonare::Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  std::vector<float> interleaved(samples.size() * 2);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    interleaved[2 * index] = samples[index];
    interleaved[2 * index + 1] = samples[index];
  }

  assistant::AssistantConfig raw;
  raw.target_lufs = std::numeric_limits<float>::quiet_NaN();
  require_invalid_parameter(
      [&] { (void)assistant::suggest_chain(samples.data(), samples.size(), sample_rate, raw); });

  assistant::AssistantConfig from_audio;
  from_audio.ceiling_db = std::numeric_limits<float>::infinity();
  require_invalid_parameter([&] { (void)assistant::suggest_chain(audio, from_audio); });

  assistant::AssistantConfig from_interleaved;
  from_interleaved.speech_mono_amount = std::numeric_limits<float>::quiet_NaN();
  require_invalid_parameter([&] {
    (void)assistant::suggest_chain_interleaved(interleaved.data(), samples.size(), 2, sample_rate,
                                               from_interleaved);
  });
}

TEST_CASE("Assistant rejects unknown target platforms at the profile boundary",
          "[mastering][assistant][validation]") {
  assistant::AudioProfile profile;
  for (const bool explicit_values : {false, true}) {
    assistant::AssistantConfig config;
    config.target_platform = "brodcast";
    if (explicit_values) {
      config.target_lufs = -13.0f;
      config.ceiling_db = -0.8f;
      config.target_lufs_explicit = true;
      config.ceiling_db_explicit = true;
    }
    require_invalid_parameter([&] { (void)assistant::suggest_chain(profile, config); });
  }

  assistant::AssistantConfig setter;
  require_invalid_parameter([&] { assistant::set_target_platform(setter, "brodcast"); });

  for (const std::string& name : assistant::platform_names()) {
    assistant::AssistantConfig known;
    known.target_platform = name;
    REQUIRE_NOTHROW(assistant::suggest_chain(profile, known));
  }
}

TEST_CASE("Assistant rejects noncanonical preset enum values",
          "[mastering][assistant][validation]") {
  for (const int raw : {-1, 9999}) {
    assistant::AssistantConfig config;
    config.preset = static_cast<sonare::mastering::api::Preset>(raw);
    require_invalid_parameter(
        [&] { (void)assistant::suggest_chain(assistant::AudioProfile{}, config); });
  }
}

TEST_CASE("Assistant target platform and streaming-safe preference affect suggestions",
          "[mastering][assistant]") {
  assistant::AudioProfile profile;
  profile.spectral.flatness = 0.6f;

  assistant::AssistantConfig broadcast;
  broadcast.target_platform = "broadcast";
  auto broadcast_result = assistant::suggest_chain(profile, broadcast);
  REQUIRE(broadcast_result.config.loudness.target_lufs == -23.0f);

  // A target that moves the ceiling as well as the loudness: broadcast and
  // podcast ask for the same ceiling the default already has, so only the loud
  // delivery formats make the ceiling observable.
  assistant::AssistantConfig club;
  club.target_platform = "club";
  auto club_result = assistant::suggest_chain(profile, club);
  REQUIRE(club_result.config.loudness.target_lufs == -9.0f);
  REQUIRE(club_result.config.loudness.ceiling_db == -0.3f);

  // A target that deliberately adds nothing keeps the caller's request.
  assistant::AssistantConfig cinema;
  cinema.target_platform = "cinema";
  auto cinema_result = assistant::suggest_chain(profile, cinema);
  REQUIRE(cinema_result.config.loudness.target_lufs == -14.0f);
  REQUIRE(cinema_result.config.loudness.ceiling_db == -1.0f);

  // Repair is selected from the defect measurement, so the profile has to carry
  // one. A floor 6 dB under the programme clears the rule; the click count is a
  // count the detector either found or did not.
  assistant::AudioProfile damaged = profile;
  damaged.loudness.integrated_lufs = -14.0f;
  damaged.defects.measured = true;
  damaged.defects.click_count = 5;
  damaged.defects.noise_floor_dbfs = -20.0f;

  assistant::AssistantConfig streaming_safe;
  streaming_safe.enable_repair = true;
  streaming_safe.prefer_streaming_safe = true;
  // Both preferences select denoise; what the preference decides is the noise
  // estimator, because the default one ranks every frame of the whole signal and
  // a stream has no whole signal. Asserting only that the stage is on would pass
  // whichever estimator came out, which is the part that has to differ.
  auto safe_result = assistant::suggest_chain(damaged, streaming_safe);
  REQUIRE(safe_result.config.repair.declick.enabled);
  REQUIRE(safe_result.config.repair.denoise.enabled);
  REQUIRE(safe_result.config.repair.denoise.config.noise_estimator ==
          sonare::mastering::repair::DenoiseNoiseEstimator::Spp);

  assistant::AssistantConfig offline_repair;
  offline_repair.enable_repair = true;
  offline_repair.prefer_streaming_safe = false;
  auto repair_result = assistant::suggest_chain(damaged, offline_repair);
  REQUIRE(repair_result.config.repair.declick.enabled);
  REQUIRE(repair_result.config.repair.denoise.enabled);
  REQUIRE(repair_result.config.repair.denoise.config.noise_estimator ==
          sonare::mastering::repair::DenoiseNoiseEstimator::Quantile);

  // Declip is selected on flat tops, not on samples reaching the ceiling, and it
  // carries the measured level with it. Leaving the threshold at its default
  // would select the stage and then hand it nothing above that default to
  // reconstruct, so the explanation would claim a repair that never happened.
  assistant::AudioProfile attenuated = damaged;
  attenuated.defects.clip_sample_count = 0;
  attenuated.defects.clip_run_count = 0;
  attenuated.defects.clip_flat_run_count = 40;
  attenuated.defects.clip_flat_level = 0.25f;
  auto attenuated_result = assistant::suggest_chain(attenuated, offline_repair);
  REQUIRE(attenuated_result.config.repair.declip.enabled);
  REQUIRE(attenuated_result.config.repair.declip.config.clip_threshold == 0.25f);

  // The other direction: a peak that reaches the ceiling with no flat top is an
  // unclipped full-scale waveform, and selecting declip on it would reconstruct
  // undamaged samples.
  assistant::AudioProfile at_ceiling = damaged;
  at_ceiling.defects.clip_sample_count = 8460;
  at_ceiling.defects.clip_run_count = 660;
  at_ceiling.defects.clip_flat_run_count = 0;
  auto at_ceiling_result = assistant::suggest_chain(at_ceiling, offline_repair);
  REQUIRE_FALSE(at_ceiling_result.config.repair.declip.enabled);

  // An unmeasured profile selects nothing, whatever enable_repair says: "nobody
  // looked" must not read as "nothing is wrong".
  auto unmeasured = assistant::suggest_chain(profile, offline_repair);
  REQUIRE_FALSE(unmeasured.config.repair.declick.enabled);
  REQUIRE_FALSE(unmeasured.config.repair.denoise.enabled);
}

TEST_CASE("Assistant suggester starts from the preset the caller names", "[mastering][assistant]") {
  namespace api = sonare::mastering::api;

  for (const std::string& name : api::preset_names()) {
    const api::Preset preset = api::preset_from_string(name);
    if (api::preset_kind(preset) != api::PresetKind::Mastering) continue;
    CAPTURE(name);
    assistant::AssistantConfig cfg;
    cfg.preset = preset;
    const auto result = assistant::suggest_chain(assistant::AudioProfile{}, cfg);
    const auto preset_base = api::preset_config(preset);

    REQUIRE(result.config.eq.tilt.tilt_db == preset_base.eq.tilt.tilt_db);
    REQUIRE(result.config.stereo.imager.config.width == preset_base.stereo.imager.config.width);
    REQUIRE(result.config.saturation.tape.enabled == preset_base.saturation.tape.enabled);
    REQUIRE(std::find(result.explanation.begin(), result.explanation.end(),
                      "base preset: " + name) != result.explanation.end());
  }
}

TEST_CASE("Assistant base preset does not follow the material", "[mastering][assistant]") {
  namespace api = sonare::mastering::api;
  constexpr int sr = 22050;
  assistant::AudioProfileConfig profile_cfg;
  profile_cfg.n_fft = 1024;
  profile_cfg.hop_length = 256;

  // Speech-like, fast rhythmic and ambient material: with no preset named, every
  // one starts from the streaming default and none picks up the speech stages.
  const std::vector<std::vector<float>> signals = {speech_like(sr, 3.0f),
                                                   rhythmic_track(sr, 3.0f, 174.0f, 55.0f, 0.9f),
                                                   ambient_track(sr, 3.0f)};
  const auto streaming = api::preset_config(api::Preset::Streaming);
  for (const auto& samples : signals) {
    const auto profile =
        assistant::analyze_audio_profile(samples.data(), samples.size(), sr, profile_cfg);
    const auto result = assistant::suggest_chain(profile, assistant::AssistantConfig{});
    REQUIRE(result.explanation.front() == "base preset: streaming");
    REQUIRE(result.config.stereo.imager.config.width == streaming.stereo.imager.config.width);
    REQUIRE_FALSE(result.config.dynamics.deesser.enabled);
    REQUIRE_FALSE(result.config.stereo.mono_maker.enabled);
  }
}

TEST_CASE("Assistant refuses a restoration preset as its base", "[mastering][assistant]") {
  assistant::AssistantConfig cfg;
  cfg.preset = sonare::mastering::api::Preset::Vinyl;
  REQUIRE_THROWS_AS(assistant::suggest_chain(assistant::AudioProfile{}, cfg),
                    sonare::SonareException);
}

TEST_CASE("Assistant preset param carries a preset index", "[mastering][assistant]") {
  namespace api = sonare::mastering::api;
  const auto names = api::preset_names();
  const auto jazz = std::find(names.begin(), names.end(), "jazz");
  REQUIRE(jazz != names.end());
  const api::Param by_index[] = {{"preset", static_cast<double>(jazz - names.begin())}};
  REQUIRE(assistant::assistant_config_from_params(by_index, 1).preset == api::Preset::Jazz);

  const api::Param out_of_range[] = {{"preset", static_cast<double>(names.size())}};
  REQUIRE_THROWS_AS(assistant::assistant_config_from_params(out_of_range, 1),
                    sonare::SonareException);
  const api::Param fractional[] = {{"preset", 0.5}};
  REQUIRE_THROWS_AS(assistant::assistant_config_from_params(fractional, 1),
                    sonare::SonareException);
  REQUIRE(assistant::AssistantConfig{}.preset == api::Preset::Streaming);
}

TEST_CASE("Assistant param builders refuse an unknown key", "[mastering][assistant]") {
  // A misspelt key would otherwise leave its setting at the default unannounced,
  // which a chain override already refuses.
  const sonare::mastering::api::Param misspelt[] = {{"targetLufz", -12.0}};
  REQUIRE_THROWS_AS(assistant::assistant_config_from_params(misspelt, 1), sonare::SonareException);
  REQUIRE_THROWS_AS(assistant::audio_profile_config_from_params(misspelt, 1),
                    sonare::SonareException);
}

TEST_CASE("Assistant suggester loudness config matches preset true-peak oversampling",
          "[mastering][assistant]") {
  // The suggester's set_loudness() must produce the same true_peak_oversample as
  // api::enable_loudness() (presets.cpp). Both are expected to use 4x oversampling;
  // asserting equality guards against the two paths drifting apart in future.
  assistant::AudioProfile profile;

  assistant::AssistantConfig cfg;
  auto suggested = assistant::suggest_chain(profile, cfg);

  const auto preset = sonare::mastering::api::preset_config(sonare::mastering::api::Preset::Pop);
  REQUIRE(suggested.config.loudness.enabled);
  REQUIRE(suggested.config.loudness.true_peak_oversample == preset.loudness.true_peak_oversample);
  REQUIRE(suggested.config.loudness.true_peak_oversample == 4);
}

TEST_CASE("Assistant delivery targets resolve through the shared table", "[mastering][assistant]") {
  // Every accepted name resolves to an index, and the index resolves back to the
  // same name: the two directions bindings rely on must agree.
  const std::vector<std::string> names = assistant::platform_names();
  REQUIRE(names.size() == assistant::kPlatformTargets.size());
  for (const std::string& name : names) {
    const int index = assistant::platform_index_from_name(name.c_str());
    REQUIRE(index >= 0);
    REQUIRE(assistant::platform_name_at(index) != nullptr);
    REQUIRE(std::string(assistant::platform_name_at(index)) == name);
  }
  // The default the AssistantConfig ships with is one of the accepted names, not
  // a value that only survives because unknown names are tolerated.
  REQUIRE(assistant::platform_index_from_name(
              assistant::AssistantConfig{}.target_platform.c_str()) >= 0);

  REQUIRE(assistant::platform_index_from_name("not-a-platform") == -1);
  REQUIRE(assistant::platform_index_from_name(nullptr) == -1);
  REQUIRE(assistant::platform_name_at(-1) == nullptr);
  REQUIRE(assistant::platform_name_at(static_cast<int>(assistant::kPlatformTargets.size())) ==
          nullptr);
}

TEST_CASE("Assistant params carry the delivery target as its index", "[mastering][assistant]") {
  namespace api = sonare::mastering::api;
  const int broadcast = assistant::platform_index_from_name("broadcast");
  REQUIRE(broadcast >= 0);

  const api::Param params[] = {{"targetPlatform", static_cast<double>(broadcast)}};
  const assistant::AssistantConfig config = assistant::assistant_config_from_params(params, 1);
  REQUIRE(config.target_platform == "broadcast");

  // An index that names no target is rejected rather than truncated toward a
  // neighbouring one, and so is a fractional index.
  const api::Param out_of_range[] = {
      {"targetPlatform", static_cast<double>(assistant::kPlatformTargets.size())}};
  REQUIRE_THROWS_AS(assistant::assistant_config_from_params(out_of_range, 1),
                    sonare::SonareException);
  const api::Param negative[] = {{"targetPlatform", -1.0}};
  REQUIRE_THROWS_AS(assistant::assistant_config_from_params(negative, 1), sonare::SonareException);
  const api::Param fractional[] = {{"targetPlatform", static_cast<double>(broadcast) + 0.5}};
  REQUIRE_THROWS_AS(assistant::assistant_config_from_params(fractional, 1),
                    sonare::SonareException);

  // The by-name setter every binding funnels through rejects an unknown target.
  assistant::AssistantConfig by_name;
  assistant::set_target_platform(by_name, "club");
  REQUIRE(by_name.target_platform == "club");
  REQUIRE_THROWS_AS(assistant::set_target_platform(by_name, "vinyl"), sonare::SonareException);
}

TEST_CASE("Profile params refuse a fractional analysis size rather than truncating it",
          "[mastering][assistant]") {
  namespace api = sonare::mastering::api;
  constexpr int kSr = 22050;
  auto signal = tone(kSr, 2.0f, 220.0f);
  add_tone(signal, kSr, 5000.0f, 0.15f);
  add_clicks(signal, kSr, 480.0f, 0.5f);

  const auto profile_with = [&](const char* key, double value) {
    const api::Param params[] = {{key, value}};
    return assistant::analyze_audio_profile(signal.data(), signal.size(), kSr,
                                            assistant::audio_profile_config_from_params(params, 1));
  };

  // Each field gets two valid settings that move the measurement, so the
  // refusals below are known to be refusing a value that would have been read.
  // A refusal-only case passes against a bare throw and proves nothing.
  // Both sides are required finite first: `a != b` is true for free once either
  // can be NaN, so the separation would otherwise pass on a broken measurement.
  const auto differs = [](float lhs, float rhs) {
    REQUIRE(std::isfinite(lhs));
    REQUIRE(std::isfinite(rhs));
    REQUIRE(lhs != rhs);
  };

  const auto coarse = profile_with("nFft", 512.0);
  const auto fine = profile_with("nFft", 2048.0);
  CAPTURE(coarse.spectral.centroid_hz, fine.spectral.centroid_hz);
  differs(coarse.spectral.centroid_hz, fine.spectral.centroid_hz);

  const auto dense = profile_with("hopLength", 128.0);
  const auto sparse = profile_with("hopLength", 512.0);
  // The spectral profile, not the onset density: the hop reaches the STFT that
  // feeds the band measurements, while attack_density comes out of the onset
  // detector's own framing and measures identically at 128 and at 512.
  CAPTURE(dense.spectral.flatness, sparse.spectral.flatness);
  differs(dense.spectral.flatness, sparse.spectral.flatness);

  const auto oversampled = profile_with("truePeakOversample", 8.0);
  const auto plain = profile_with("truePeakOversample", 1.0);
  CAPTURE(oversampled.loudness.true_peak_db, plain.loudness.true_peak_db);
  differs(oversampled.loudness.true_peak_db, plain.loudness.true_peak_db);

  // A count cannot be fractional, and rounding one would answer with a window
  // the caller never asked for. Both spellings of every key are accepted, so
  // both have to refuse.
  for (const char* key :
       {"nFft", "n_fft", "hopLength", "hop_length", "truePeakOversample", "true_peak_oversample"}) {
    const api::Param fractional[] = {{key, 512.5}};
    CAPTURE(key);
    REQUIRE_THROWS_AS(assistant::audio_profile_config_from_params(fractional, 1),
                      sonare::SonareException);
  }

  const api::Param too_large[] = {{"nFft", 1.0e18}};
  REQUIRE_THROWS_AS(assistant::audio_profile_config_from_params(too_large, 1),
                    sonare::SonareException);
}

TEST_CASE("Assistant delivery target yields to a named loudness, default-valued or not",
          "[mastering][assistant]") {
  namespace api = sonare::mastering::api;
  assistant::AudioProfile profile;
  profile.spectral.flatness = 0.6f;

  const int broadcast = assistant::platform_index_from_name("broadcast");
  REQUIRE(broadcast >= 0);
  const int club = assistant::platform_index_from_name("club");
  REQUIRE(club >= 0);

  // Precedence used to be decided by comparing the requested value against the
  // default, so the one slider position that equals the default -- where a host
  // UI sits before the user touches anything -- silently lost to the delivery
  // target while every other position won.
  const api::Param default_valued[] = {{"targetPlatform", static_cast<double>(broadcast)},
                                       {"targetLufs", -14.0},
                                       {"ceilingDb", -1.0}};
  const auto explicit_default =
      assistant::suggest_chain(profile, assistant::assistant_config_from_params(default_valued, 3));
  REQUIRE(explicit_default.config.loudness.target_lufs == -14.0f);
  REQUIRE(explicit_default.config.loudness.ceiling_db == -1.0f);

  // Presence, not value: naming one field must not suppress the target on the
  // other.
  const api::Param loudness_only[] = {{"targetPlatform", static_cast<double>(club)},
                                      {"targetLufs", -14.0}};
  const auto partial =
      assistant::suggest_chain(profile, assistant::assistant_config_from_params(loudness_only, 2));
  REQUIRE(partial.config.loudness.target_lufs == -14.0f);
  REQUIRE(partial.config.loudness.ceiling_db == -0.3f);

  // Naming nothing still takes the whole target.
  const api::Param platform_only[] = {{"targetPlatform", static_cast<double>(club)}};
  const auto full =
      assistant::suggest_chain(profile, assistant::assistant_config_from_params(platform_only, 1));
  REQUIRE(full.config.loudness.target_lufs == -9.0f);
  REQUIRE(full.config.loudness.ceiling_db == -0.3f);
}

TEST_CASE("Assistant AudioProfile dynamics and band levels do not depend on the input rate",
          "[mastering][assistant][analysis_rate]") {
  using sonare::test::make_rate_material;
  using sonare::test::RateMaterial;
  constexpr int kReferenceRate = assistant::kProfileReferenceRate;
  // Attack density from the click train (C); sustain and band levels from the triads (T).
  // All content is below 10 kHz.
  const auto material = [](RateMaterial m, int sr) {
    const sonare::Audio audio = make_rate_material(m, sr);
    return assistant::analyze_audio_profile(audio.data(), audio.size(), sr);
  };
  // The bands at or below 10 kHz (sub to high-mid); the rest reach past the material.
  const auto bands = [](const assistant::AudioProfile& p) {
    return std::vector<float>{p.spectral.sub_rms_db, p.spectral.low_rms_db,
                              p.spectral.low_mid_rms_db, p.spectral.mid_rms_db,
                              p.spectral.high_mid_rms_db};
  };

  const auto reference_clicks = material(RateMaterial::Clicks, kReferenceRate);
  const auto reference = material(RateMaterial::TriadTurnaround, kReferenceRate);
  REQUIRE(reference_clicks.dynamics.attack_density > 0.0f);
  REQUIRE(reference.dynamics.sustain_ratio > 0.0f);
  const std::vector<float> reference_bands = bands(reference);
  // T has nothing in the sub and high-mid bands; their 40-70 dB lower floor is window
  // leakage, so only bands within 30 dB of the loudest one are compared.
  const float loudest = *std::max_element(reference_bands.begin(), reference_bands.end());
  std::vector<bool> carries_content;
  for (float level : reference_bands) carries_content.push_back(level >= loudest - 30.0f);
  REQUIRE(std::count(carries_content.begin(), carries_content.end(), true) >= 3);

  for (int sr : {22050, 32000, 44100}) {
    const auto clicks = material(RateMaterial::Clicks, sr);
    const auto profile = material(RateMaterial::TriadTurnaround, sr);
    const float attack_deviation =
        std::abs(clicks.dynamics.attack_density / reference_clicks.dynamics.attack_density - 1.0f);
    const float sustain_deviation =
        std::abs(profile.dynamics.sustain_ratio / reference.dynamics.sustain_ratio - 1.0f);
    const std::vector<float> rate_bands = bands(profile);
    float band_deviation_db = 0.0f;
    size_t worst_band = 0;
    for (size_t band = 0; band < rate_bands.size(); ++band) {
      if (!carries_content[band]) continue;
      const float deviation = std::abs(rate_bands[band] - reference_bands[band]);
      if (deviation > band_deviation_db) {
        band_deviation_db = deviation;
        worst_band = band;
      }
    }
    CAPTURE(sr, attack_deviation, sustain_deviation, band_deviation_db, worst_band);
    CHECK(attack_deviation <= 0.10f);
    CHECK(sustain_deviation <= 0.10f);
    // A partial beside a band edge moves across it with the bin grid (0.56 dB at 250 Hz).
    CHECK(band_deviation_db <= 1.0f);
  }
}

TEST_CASE("Assistant AudioProfile band levels are dBFS on the noise fields' mean-square scale",
          "[mastering][assistant]") {
  // 10*log10(1/2): a full-scale sine's mean square.
  const float full_scale_sine_dbfs = 10.0f * std::log10(0.5f);
  for (int sr : {44100, 48000}) {
    for (float amplitude : {1.0f, 0.25f}) {
      std::vector<float> samples(static_cast<size_t>(sr));
      for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = amplitude *
                     static_cast<float>(std::sin(sonare::constants::kTwoPiD * 1000.0 *
                                                 static_cast<double>(i) / static_cast<double>(sr)));
      }
      const auto profile = assistant::analyze_audio_profile(samples.data(), samples.size(), sr);
      const float expected = full_scale_sine_dbfs + sonare::linear_to_db(amplitude);
      CAPTURE(sr, amplitude, profile.spectral.mid_rms_db);
      CHECK(std::abs(profile.spectral.mid_rms_db - expected) < 0.1f);
      // The neighbours hold only window leakage.
      CHECK(profile.spectral.low_mid_rms_db < expected - 40.0f);
      CHECK(profile.spectral.high_mid_rms_db < expected - 40.0f);
    }
  }
}

namespace {

using sonare::mixing::assistant::test::make_demo_tracks;

constexpr int kSummaryRate = assistant::kProfileReferenceRate;
constexpr float kBurstGainDb = 24.0f;
constexpr float kBurstSeconds = 0.05f;
constexpr std::size_t kDemoKick = 0;
constexpr std::size_t kDemoHats = 4;
constexpr float kClickAmplitude = 0.2f;

// Deterministic white noise in [-1, 1].
std::vector<float> white_noise(std::size_t length, std::uint32_t seed) {
  std::vector<float> out(length);
  for (float& value : out) {
    seed = seed * 1664525u + 1013904223u;
    value = static_cast<float>(static_cast<double>(seed) / 4294967295.0) * 2.0f - 1.0f;
  }
  return out;
}

float abs_peak(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float value : samples) peak = std::max(peak, std::abs(value));
  return peak;
}

// A 50 ms white-noise burst kBurstGainDb above the material's own peak, 30 ms past its middle.
std::vector<float> with_burst(std::vector<float> samples) {
  const float amplitude = abs_peak(samples) * sonare::db_to_linear(kBurstGainDb);
  const std::size_t start = samples.size() / 2 + static_cast<std::size_t>(0.03f * kSummaryRate);
  const auto noise =
      white_noise(static_cast<std::size_t>(kBurstSeconds * kSummaryRate), 0x2468ACE1u);
  for (std::size_t i = 0; i < noise.size() && start + i < samples.size(); ++i) {
    samples[start + i] += amplitude * noise[i];
  }
  return samples;
}

std::vector<float> demo_track(std::size_t index, float seconds, const char* id) {
  auto fixture = make_demo_tracks(kSummaryRate, seconds);
  REQUIRE(fixture.ids[index] == id);
  return std::move(fixture.left[index]);
}

assistant::DynamicsProfile dynamics_of(const std::vector<float>& samples) {
  return assistant::analyze_audio_profile(samples.data(), samples.size(), kSummaryRate).dynamics;
}

float relative_change(float after, float before) { return std::abs(after / before - 1.0f); }

}  // namespace

TEST_CASE("Assistant attack density and sustain ratio ignore one dominant burst",
          "[mastering][assistant][.][slow]") {
  constexpr float kSeconds = 24.0f;
  for (const auto& [index, id] : {std::pair{kDemoKick, "kick"}, std::pair{kDemoHats, "hats"}}) {
    const auto samples = demo_track(index, kSeconds, id);
    const auto plain = dynamics_of(samples);
    const auto burst = dynamics_of(with_burst(samples));
    CAPTURE(id, plain.attack_density, burst.attack_density, plain.sustain_ratio,
            burst.sustain_ratio);
    REQUIRE(plain.attack_density > 1.0f);
    REQUIRE(plain.sustain_ratio > 0.1f);
    // The burst's own onset peak is one count in 24 s (2.1 %).
    CHECK(relative_change(burst.attack_density, plain.attack_density) <= 0.10f);
    CHECK(relative_change(burst.sustain_ratio, plain.sustain_ratio) <= 0.10f);
  }
}

TEST_CASE("Assistant attack density of a steady tone is zero", "[mastering][assistant]") {
  // Phase in double: a float phase at 24 s jitters by several ulps and adds onsets of its own.
  std::vector<float> samples(static_cast<std::size_t>(24.0f * kSummaryRate));
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double t = static_cast<double>(i) / kSummaryRate;
    samples[i] = 0.4f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 220.0 * t));
  }
  const auto dynamics = dynamics_of(samples);
  CAPTURE(dynamics.attack_density);
  CHECK(dynamics.attack_density == 0.0f);
}

TEST_CASE("Assistant attack density counts one click over -40 dBFS noise and not the noise",
          "[mastering][assistant]") {
  constexpr float kSeconds = 24.0f;
  auto samples = white_noise(static_cast<std::size_t>(kSeconds * kSummaryRate), 0x13572468u);
  const float noise_amplitude = sonare::db_to_linear(-40.0f);
  for (float& value : samples) value *= noise_amplitude;
  // A 5 ms broadband click, decaying linearly.
  const auto click = white_noise(static_cast<std::size_t>(0.005f * kSummaryRate), 0x0BADF00Du);
  const std::size_t at = samples.size() / 2;
  for (std::size_t i = 0; i < click.size(); ++i) {
    const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(click.size());
    samples[at + i] += kClickAmplitude * envelope * click[i];
  }
  const auto dynamics = dynamics_of(samples);
  CAPTURE(dynamics.attack_density);
  CHECK_THAT(dynamics.attack_density, Catch::Matchers::WithinRel(1.0f / kSeconds, 1.0e-4f));
}

TEST_CASE("Assistant summary references set nothing aside below ten seconds",
          "[mastering][assistant][.][slow]") {
  // 12 s sets one event aside, so the burst stops setting the level; 6 s sets none aside,
  // so the burst still does -- both sides, or the duration rule could vanish unnoticed.
  const auto at_12 = demo_track(kDemoKick, 12.0f, "kick");
  const auto plain_12 = dynamics_of(at_12);
  const auto burst_12 = dynamics_of(with_burst(at_12));
  const auto at_6 = demo_track(kDemoKick, 6.0f, "kick");
  const auto plain_6 = dynamics_of(at_6);
  const auto burst_6 = dynamics_of(with_burst(at_6));
  CAPTURE(plain_12.attack_density, burst_12.attack_density, plain_12.sustain_ratio,
          burst_12.sustain_ratio, plain_6.attack_density, burst_6.attack_density,
          plain_6.sustain_ratio, burst_6.sustain_ratio);
  CHECK(relative_change(burst_12.attack_density, plain_12.attack_density) <= 0.10f);
  CHECK(relative_change(burst_12.sustain_ratio, plain_12.sustain_ratio) <= 0.10f);
  // At 6 s the burst is the reference: only its own frames and onset clear the threshold.
  CHECK(burst_6.sustain_ratio < 0.2f * plain_6.sustain_ratio);
  CHECK(burst_6.attack_density < 0.2f * plain_6.attack_density);
}
