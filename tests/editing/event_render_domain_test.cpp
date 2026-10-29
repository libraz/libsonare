#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/event_model/event_renderer.h"
#include "editing/event_model/percussive_event.h"
#include "effects/hpss.h"
#include "util/db.h"
#include "util/exception.h"

namespace {

constexpr int kSampleRate = 22050;
constexpr std::size_t kFixtureSamples = 64;

sonare::Audio transient_audio(float amplitude = 0.5f) {
  std::vector<float> samples(kFixtureSamples, 0.0f);
  samples[1] = -0.0f;
  samples[kFixtureSamples / 2] = amplitude;
  return sonare::Audio::from_vector(std::move(samples), kSampleRate);
}

sonare::editing::event_model::PercussiveEvent event_span(int64_t onset, int64_t offset) {
  sonare::editing::event_model::PercussiveEvent event;
  event.onset_sample = onset;
  event.offset_sample = offset;
  return event;
}

sonare::editing::event_model::PercussiveEventRenderConfig compact_config() {
  sonare::editing::event_model::PercussiveEventRenderConfig config;
  config.separation.n_fft = 16;
  config.separation.hop_length = 8;
  config.separation.hpss.kernel_size_harmonic = 3;
  config.separation.hpss.kernel_size_percussive = 3;
  // The positive fade rounds to zero samples, so the compact transient is not
  // tapered before the overflow precondition checks the separated signal.
  config.fade_ms = 0.001f;
  return config;
}

template <typename Fn>
sonare::ErrorCode code_of(Fn&& fn) {
  try {
    fn();
  } catch (const sonare::SonareException& error) {
    return error.code();
  }
  return sonare::ErrorCode::Ok;
}

bool all_finite(const sonare::Audio& audio) {
  return std::all_of(audio.begin(), audio.end(),
                     [](float sample) { return std::isfinite(sample); });
}

}  // namespace

TEST_CASE("event renderer rejects extreme source spans before deriving their length",
          "[event_render_domain]") {
  const sonare::Audio audio = transient_audio();
  const int64_t length = static_cast<int64_t>(audio.size());
  const sonare::ErrorCode invalid = sonare::ErrorCode::InvalidParameter;

  // This identity span wraps length_samples() to a positive value on common
  // two's-complement builds. It must be rejected before that subtraction.
  const auto underflowing = event_span(1, std::numeric_limits<int64_t>::min());
  REQUIRE(underflowing.edit.is_identity());
  REQUIRE(code_of([&] {
            sonare::editing::event_model::render_percussive_events(audio, {underflowing});
          }) == invalid);

  const auto beyond_audio =
      event_span(std::numeric_limits<int64_t>::max() - 1, std::numeric_limits<int64_t>::max());
  REQUIRE(code_of([&] {
            sonare::editing::event_model::render_percussive_events(audio, {beyond_audio});
          }) == invalid);
  REQUIRE(length > 0);
}

TEST_CASE("event renderer validates converted gain on muted and audible events",
          "[event_render_domain]") {
  const sonare::Audio audio = transient_audio();
  const auto config = compact_config();
  const sonare::ErrorCode invalid = sonare::ErrorCode::InvalidParameter;

  for (const float gain_db : {10000.0f, std::numeric_limits<float>::max()}) {
    INFO("gain_db " << gain_db);
    REQUIRE(std::isfinite(gain_db));
    REQUIRE_FALSE(std::isfinite(sonare::db_to_linear(gain_db)));
    for (const bool muted : {false, true}) {
      INFO("muted " << muted);
      auto event = event_span(0, static_cast<int64_t>(audio.size()));
      event.edit.gain_db = gain_db;
      event.edit.muted = muted;
      REQUIRE(code_of([&] {
                sonare::editing::event_model::render_percussive_events(audio, {event}, config);
              }) == invalid);
    }
  }
}

TEST_CASE("event renderer rejects non-finite identity audio", "[event_render_domain]") {
  const sonare::ErrorCode invalid = sonare::ErrorCode::InvalidParameter;

  for (const float bad_sample :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    std::vector<float> samples(kFixtureSamples, 0.0f);
    samples[kFixtureSamples / 2] = bad_sample;
    const sonare::Audio audio = sonare::Audio::from_vector(std::move(samples), kSampleRate);
    const auto identity = event_span(0, static_cast<int64_t>(audio.size()));
    REQUIRE(identity.edit.is_identity());
    REQUIRE(code_of([&] {
              sonare::editing::event_model::render_percussive_events(audio, {identity});
            }) == invalid);
  }
}

TEST_CASE("event renderer rejects a finite gain whose processed sample overflows",
          "[event_render_domain]") {
  const sonare::Audio audio = transient_audio(4.0f);
  const auto config = compact_config();
  const float gain_db = 770.0f;
  const float gain = sonare::db_to_linear(gain_db);
  REQUIRE(all_finite(audio));
  REQUIRE(std::isfinite(gain_db));
  REQUIRE(std::isfinite(gain));

  sonare::StftConfig stft;
  stft.n_fft = config.separation.n_fft;
  stft.hop_length = config.separation.hop_length;
  const sonare::Audio separated = sonare::percussive(audio, config.separation.hpss, stft);
  REQUIRE(all_finite(separated));

  float lifted_peak = 0.0f;
  for (const float sample : separated) lifted_peak = std::max(lifted_peak, std::abs(sample));
  REQUIRE(lifted_peak > 1.0f);
  // The exact multiply used by the old render loop is non-finite even though
  // both the source samples and converted gain passed input validation.
  REQUIRE_FALSE(std::isfinite(gain * lifted_peak));

  auto event = event_span(0, static_cast<int64_t>(audio.size()));
  event.edit.gain_db = gain_db;
  REQUIRE(code_of([&] {
            sonare::editing::event_model::render_percussive_events(audio, {event}, config);
          }) == sonare::ErrorCode::InvalidParameter);
}

TEST_CASE("event renderer preserves ordinary identity, gain, mute and truncated-shift behavior",
          "[event_render_domain]") {
  const sonare::Audio audio = transient_audio();
  const auto config = compact_config();
  const int64_t length = static_cast<int64_t>(audio.size());

  const auto identity = event_span(0, length);
  const sonare::Audio passed =
      sonare::editing::event_model::render_percussive_events(audio, {identity}, config);
  REQUIRE(passed.size() == audio.size());
  REQUIRE(std::memcmp(passed.data(), audio.data(), audio.size() * sizeof(float)) == 0);

  auto muted_event = event_span(0, length);
  muted_event.edit.muted = true;
  const sonare::Audio muted =
      sonare::editing::event_model::render_percussive_events(audio, {muted_event}, config);
  REQUIRE(all_finite(muted));

  auto gained_event = event_span(0, length);
  gained_event.edit.gain_db = 6.0206f;
  const float gain = sonare::db_to_linear(gained_event.edit.gain_db);
  const sonare::Audio gained =
      sonare::editing::event_model::render_percussive_events(audio, {gained_event}, config);
  REQUIRE(all_finite(gained));
  float source_component_peak = 0.0f;
  float gain_residual = 0.0f;
  for (std::size_t i = 0; i < audio.size(); ++i) {
    const float source_component = audio[i] - muted[i];
    const float gained_component = gained[i] - muted[i];
    source_component_peak = std::max(source_component_peak, std::abs(source_component));
    gain_residual = std::max(gain_residual, std::abs(gained_component - gain * source_component));
  }
  REQUIRE(source_component_peak > 0.1f);
  REQUIRE(gain_residual < 1.0e-5f);

  auto shifted_event = event_span(0, length);
  shifted_event.edit.time_offset_samples = length;
  const sonare::Audio truncated =
      sonare::editing::event_model::render_percussive_events(audio, {shifted_event}, config);
  REQUIRE(std::memcmp(truncated.data(), muted.data(), audio.size() * sizeof(float)) == 0);
}
