/// @file live_input_track_test.cpp
/// @brief Live input routed into one track lane: its strip and fader, the dry monitor beside it,
///        capture sources, channel mapping, ordering against prepare, lane removal, the switch
///        ramp, and the input monitor reader.

#include <catch2/catch_test_macros.hpp>

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_ARRANGEMENT)

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "mixing/api/scene.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

using sonare::engine::RealtimeEngine;
using sonare::engine::TrackLaneConfig;
using sonare::mixing::api::Insert;
using sonare::mixing::api::InsertSlot;
using sonare::mixing::api::Strip;

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr uint32_t kTrackT = 10;
constexpr uint32_t kTrackU = 20;
// 5 ms at 48 kHz.
constexpr int kRampSamples = 240;
// Long enough for the switch ramp and the fader smoother to settle.
constexpr int kSettleFrames = 4800;
constexpr int kFftSize = 4096;

using Planes = std::vector<std::vector<float>>;

Planes sines(const std::vector<float>& freqs, int frames, float amplitude = 0.5f) {
  Planes planes(freqs.size(), std::vector<float>(static_cast<size_t>(frames)));
  for (size_t ch = 0; ch < freqs.size(); ++ch) {
    for (int i = 0; i < frames; ++i) {
      planes[ch][static_cast<size_t>(i)] =
          amplitude * std::sin(sonare::constants::kTwoPi * freqs[ch] * static_cast<float>(i) /
                               static_cast<float>(kRate));
    }
  }
  return planes;
}

/// Processes @p input in blocks of @p block in place and returns the engine's output.
Planes run(RealtimeEngine& engine, Planes input, int block = kBlock) {
  const int frames = static_cast<int>(input.front().size());
  std::vector<float*> io(input.size());
  for (int start = 0; start < frames; start += block) {
    const int n = std::min(block, frames - start);
    for (size_t ch = 0; ch < input.size(); ++ch) io[ch] = input[ch].data() + start;
    engine.process(io.data(), static_cast<int>(io.size()), n);
  }
  return input;
}

void prepare(RealtimeEngine& engine, int channels = 2) {
  engine.prepare(kRate, kBlock, RealtimeEngine::kDefaultCommandCapacity,
                 RealtimeEngine::kDefaultTelemetryCapacity, channels);
}

Strip strip_with(float fader_db, float pan = 0.0f) {
  Strip strip;
  strip.id = "s";
  strip.fader_db = fader_db;
  strip.pan = pan;
  return strip;
}

Strip octave_up_strip() {
  Strip strip = strip_with(0.0f);
  strip.inserts.push_back(
      Insert{InsertSlot::PreFader, "voice.changer", R"({"retunePitchSemitones":12})"});
  return strip;
}

void route(RealtimeEngine& engine, const std::vector<uint32_t>& tracks) {
  std::vector<TrackLaneConfig> lanes;
  for (uint32_t track : tracks) lanes.emplace_back(track);
  REQUIRE(engine.set_track_lanes(lanes));
}

float max_abs(const std::vector<float>& plane, int from = 0) {
  float peak = 0.0f;
  for (size_t i = static_cast<size_t>(from); i < plane.size(); ++i) {
    peak = std::max(peak, std::abs(plane[i]));
  }
  return peak;
}

float max_error(const std::vector<float>& got, const std::vector<float>& expected, float scale,
                int from) {
  float error = 0.0f;
  for (size_t i = static_cast<size_t>(from); i < got.size(); ++i) {
    error = std::max(error, std::abs(got[i] - expected[i] * scale));
  }
  return error;
}

/// Magnitude of the DFT of the last kFftSize samples of @p plane at @p bin.
float bin_magnitude(const std::vector<float>& plane, int bin) {
  const size_t start = plane.size() - static_cast<size_t>(kFftSize);
  std::complex<double> acc{};
  for (int i = 0; i < kFftSize; ++i) {
    const double phase = -sonare::constants::kTwoPiD * bin * i / kFftSize;
    acc += static_cast<double>(plane[start + static_cast<size_t>(i)]) *
           std::complex<double>(std::cos(phase), std::sin(phase));
  }
  return static_cast<float>(std::abs(acc));
}

/// The bin holding the most energy among bins 1 .. kFftSize / 8 (0 .. 6 kHz).
int dominant_bin(const std::vector<float>& plane) {
  int best = 1;
  float best_magnitude = 0.0f;
  for (int bin = 1; bin < kFftSize / 8; ++bin) {
    const float magnitude = bin_magnitude(plane, bin);
    if (magnitude > best_magnitude) {
      best_magnitude = magnitude;
      best = bin;
    }
  }
  return best;
}

/// The frequency at the centre of @p bin, so a rectangular DFT sees no leakage from it.
float freq_of_bin(int bin) {
  return static_cast<float>(bin) * static_cast<float>(kRate) / static_cast<float>(kFftSize);
}

int bin_of(float freq) {
  return static_cast<int>(std::lround(freq * kFftSize / static_cast<float>(kRate)));
}

/// A two-channel engine routing the input into track T with @p strip, the dry monitor off.
void wet_only(RealtimeEngine& engine, const Strip& strip) {
  prepare(engine);
  route(engine, {kTrackT});
  REQUIRE(engine.set_track_strip(kTrackT, strip));
  engine.set_input_monitor(false, 1.0f);
  engine.set_input_track(kTrackT);
}

}  // namespace

TEST_CASE("live input through a track lane takes the track's fader", "[engine][live_input]") {
  const float fader = std::pow(10.0f, -6.0f / 20.0f);
  const Planes input = sines({440.0f, 440.0f}, 3 * kSettleFrames);

  RealtimeEngine engine;
  wet_only(engine, strip_with(-6.0f));
  REQUIRE(engine.input_track() == std::optional<uint32_t>(kTrackT));
  const Planes out = run(engine, input);
  for (size_t ch = 0; ch < 2; ++ch) {
    INFO("channel " << ch);
    REQUIRE(max_error(out[ch], input[ch], fader, kSettleFrames) < 1e-4f);
  }

  SECTION("without an input track the dry-less output is silent") {
    RealtimeEngine control;
    prepare(control);
    route(control, {kTrackT});
    control.set_input_monitor(false, 1.0f);
    const Planes silent = run(control, input);
    REQUIRE(max_abs(silent[0]) == 0.0f);
    REQUIRE(max_abs(silent[1]) == 0.0f);
  }
}

TEST_CASE("the dry monitor and the input track sound together", "[engine][live_input]") {
  const Planes input = sines({440.0f, 440.0f}, 3 * kSettleFrames);
  RealtimeEngine engine;
  wet_only(engine, strip_with(0.0f));
  engine.set_input_monitor(true, 1.0f);
  const Planes out = run(engine, input);
  REQUIRE(max_error(out[0], input[0], 2.0f, kSettleFrames) < 1e-4f);
  REQUIRE(max_error(out[1], input[1], 2.0f, kSettleFrames) < 1e-4f);
}

TEST_CASE("the input track's strip inserts process the live input", "[engine][live_input]") {
  const float freq = 220.0f;
  const Planes input = sines({freq, freq}, 48000);
  RealtimeEngine engine;
  wet_only(engine, octave_up_strip());
  const Planes out = run(engine, input);
  REQUIRE(std::abs(dominant_bin(out[0]) - bin_of(2.0f * freq)) <= 1);
}

TEST_CASE("clearing, refusing and losing the input track", "[engine][live_input]") {
  const Planes input = sines({440.0f, 440.0f}, 2 * kSettleFrames);
  RealtimeEngine engine;
  wet_only(engine, strip_with(0.0f));
  (void)run(engine, input);

  SECTION("an id no lane carries is refused and changes nothing") {
    try {
      engine.set_input_track(99);
      FAIL("track 99 was accepted");
    } catch (const sonare::SonareParameterRefusal& e) {
      REQUIRE(e.code() == sonare::ErrorCode::InvalidParameter);
      REQUIRE(e.subject() == "track_id");
    }
    REQUIRE(engine.input_track() == std::optional<uint32_t>(kTrackT));
  }

  SECTION("clear returns the output to silence after the ramp") {
    engine.clear_input_track();
    REQUIRE_FALSE(engine.input_track().has_value());
    const Planes out = run(engine, input);
    REQUIRE(max_abs(out[0], kRampSamples) == 0.0f);
    REQUIRE(max_abs(out[1], kRampSamples) == 0.0f);
  }

  SECTION("set_track_lanes dropping the track clears the input track") {
    route(engine, {kTrackU});
    REQUIRE_FALSE(engine.input_track().has_value());
    const Planes out = run(engine, input);
    REQUIRE(max_abs(out[0]) == 0.0f);
    REQUIRE(max_abs(out[1]) == 0.0f);
  }
}

TEST_CASE("capture records the dry input or the voiced output by its source",
          "[engine][live_input]") {
  const float freq = 220.0f;
  const int frames = 48000;
  const Planes input = sines({freq, freq}, frames);
  for (const auto source :
       {sonare::engine::CaptureSource::kInput, sonare::engine::CaptureSource::kOutput}) {
    const bool from_input = source == sonare::engine::CaptureSource::kInput;
    INFO("source " << (from_input ? "input" : "output"));
    RealtimeEngine engine;
    wet_only(engine, octave_up_strip());
    Planes captured(2, std::vector<float>(static_cast<size_t>(frames), 0.0f));
    float* capture_channels[] = {captured[0].data(), captured[1].data()};
    engine.set_capture_segment({capture_channels, 2, frames});
    engine.set_capture_source(source);
    engine.set_capture_armed(true);
    const Planes out = run(engine, input);
    REQUIRE(engine.captured_frames() == frames);
    const int expected = bin_of(from_input ? freq : 2.0f * freq);
    REQUIRE(std::abs(dominant_bin(captured[0]) - expected) <= 1);
    if (!from_input) {
      REQUIRE(captured[0] == out[0]);
      REQUIRE(captured[1] == out[1]);
    }
  }
}

TEST_CASE("the input track takes the first two input channels", "[engine][live_input]") {
  SECTION("four channels: channels 2 and 3 stay out of the lane") {
    const std::vector<float> freqs = {freq_of_bin(26), freq_of_bin(43), freq_of_bin(60),
                                      freq_of_bin(77)};
    const Planes input = sines(freqs, 2 * kSettleFrames);
    RealtimeEngine engine;
    prepare(engine, 4);
    route(engine, {kTrackT});
    engine.set_input_monitor(false, 1.0f);
    engine.set_input_track(kTrackT);
    const Planes out = run(engine, input);
    const float left_own = bin_magnitude(out[0], bin_of(freqs[0]));
    const float right_own = bin_magnitude(out[1], bin_of(freqs[1]));
    REQUIRE(left_own > 100.0f);
    REQUIRE(right_own > 100.0f);
    for (size_t ch = 0; ch < 2; ++ch) {
      for (size_t other = 2; other < freqs.size(); ++other) {
        INFO("output " << ch << " at " << freqs[other] << " Hz");
        REQUIRE(bin_magnitude(out[ch], bin_of(freqs[other])) < 1e-3f * left_own);
      }
    }
  }

  SECTION("one channel fills a mono engine's one-plane lane") {
    const Planes input = sines({440.0f}, 2 * kSettleFrames);
    RealtimeEngine engine;
    prepare(engine, 1);
    route(engine, {kTrackT});
    engine.set_input_monitor(false, 1.0f);
    engine.set_input_track(kTrackT);
    (void)run(engine, input);
    const float input_peak_db = 20.0f * std::log10(max_abs(input[0]));
    bool found = false;
    sonare::engine::MeterTelemetryRecord record{};
    sonare::engine::MeterTelemetryRecord lane{};
    while (engine.pop_meter_telemetry(record)) {
      if (record.target_id == 1) {
        lane = record;
        found = true;
      }
    }
    REQUIRE(found);
    INFO("lane peaks " << lane.peak_db[0] << " / " << lane.peak_db[1] << " dB");
    REQUIRE(std::abs(lane.peak_db[0] - input_peak_db) < 0.05f);
    // A mono engine renders one lane plane for every source; the second is not measured.
    REQUIRE(std::isnan(lane.peak_db[1]));
  }
}

TEST_CASE("an input track set before prepare survives it", "[engine][live_input]") {
  const Planes input = sines({440.0f, 440.0f}, 2 * kSettleFrames);
  RealtimeEngine reference;
  wet_only(reference, strip_with(-6.0f));
  const Planes expected = run(reference, input);

  RealtimeEngine engine;
  route(engine, {kTrackT});
  REQUIRE(engine.set_track_strip(kTrackT, strip_with(-6.0f)));
  engine.set_input_monitor(false, 1.0f);
  engine.set_input_track(kTrackT);
  prepare(engine);
  const Planes out = run(engine, input);
  REQUIRE(max_error(out[0], expected[0], 1.0f, 0) < 1e-6f);
  REQUIRE(max_error(out[1], expected[1], 1.0f, 0) < 1e-6f);

  prepare(engine);
  REQUIRE(engine.input_track() == std::optional<uint32_t>(kTrackT));
}

TEST_CASE("a block after the input track's lane is gone routes the input nowhere",
          "[engine][live_input]") {
  const Planes input = sines({440.0f, 440.0f}, kBlock);
  RealtimeEngine engine;
  prepare(engine);
  route(engine, {kTrackT, kTrackU});
  engine.set_input_monitor(false, 1.0f);
  engine.set_input_track(kTrackT);
  (void)run(engine, input);
  sonare::engine::MeterTelemetryRecord record{};
  while (engine.pop_meter_telemetry(record)) {
  }

  route(engine, {kTrackU});
  const Planes out = run(engine, input);
  REQUIRE(max_abs(out[0]) == 0.0f);
  REQUIRE(max_abs(out[1]) == 0.0f);
  while (engine.pop_meter_telemetry(record)) {
    if (record.target_id == 0) continue;
    INFO("meter target " << record.target_id);
    for (size_t ch = 0; ch < 2; ++ch) {
      REQUIRE_FALSE(record.peak_db[ch] > sonare::constants::kFloorDb);
    }
  }
}

TEST_CASE("setting, clearing and moving the input track ramp over 5 ms", "[engine][live_input]") {
  constexpr int kSmallBlock = 128;
  const int frames = 4 * kRampSamples;
  const Planes dc(2, std::vector<float>(static_cast<size_t>(frames), 1.0f));
  RealtimeEngine engine;
  engine.prepare(kRate, kSmallBlock);
  route(engine, {kTrackT, kTrackU});
  // T hard left, U hard right: each output channel carries one lane's share of the input.
  REQUIRE(engine.set_track_strip(kTrackT, strip_with(0.0f, -1.0f)));
  REQUIRE(engine.set_track_strip(kTrackU, strip_with(0.0f, 1.0f)));
  engine.set_input_monitor(false, 1.0f);
  (void)run(engine, dc, kSmallBlock);

  engine.set_input_track(kTrackT);
  const Planes in = run(engine, dc, kSmallBlock);
  for (int n = 0; n < frames; ++n) {
    INFO("sample " << n << " after set");
    const float expected = n < kRampSamples ? static_cast<float>(n) / kRampSamples : 1.0f;
    REQUIRE(std::abs(in[0][static_cast<size_t>(n)] - expected) < 1e-5f);
  }

  engine.set_input_track(kTrackU);
  const Planes moved = run(engine, dc, kSmallBlock);
  for (int n = 0; n < frames; ++n) {
    INFO("sample " << n << " after the move");
    const float t = moved[0][static_cast<size_t>(n)];
    const float u = moved[1][static_cast<size_t>(n)];
    REQUIRE(std::abs(t + u - 1.0f) < 1e-5f);
    const float expected_u = n < kRampSamples ? static_cast<float>(n) / kRampSamples : 1.0f;
    REQUIRE(std::abs(u - expected_u) < 1e-5f);
  }

  engine.clear_input_track();
  const Planes out = run(engine, dc, kSmallBlock);
  for (int n = 0; n < frames; ++n) {
    INFO("sample " << n << " after clear");
    const float expected = n < kRampSamples ? 1.0f - static_cast<float>(n) / kRampSamples : 0.0f;
    REQUIRE(std::abs(out[1][static_cast<size_t>(n)] - expected) < 1e-5f);
  }
}

TEST_CASE("configuring capture leaves the input monitor as it was set", "[engine][live_input]") {
  const Planes input = sines({440.0f, 440.0f}, kSettleFrames);
  RealtimeEngine engine;
  prepare(engine);
  engine.set_input_monitor(false, 0.25f);
  std::vector<float> left(static_cast<size_t>(kSettleFrames));
  std::vector<float> right(static_cast<size_t>(kSettleFrames));
  float* capture_channels[] = {left.data(), right.data()};
  engine.set_capture_segment({capture_channels, 2, kSettleFrames});
  engine.set_capture_source(sonare::engine::CaptureSource::kInput);
  engine.set_capture_armed(true);
  const RealtimeEngine::InputMonitorState monitor = engine.input_monitor();
  REQUIRE_FALSE(monitor.enabled);
  REQUIRE(monitor.gain == 0.25f);
  const Planes out = run(engine, input);
  REQUIRE(max_abs(out[0]) == 0.0f);
  REQUIRE(max_abs(out[1]) == 0.0f);
}

#endif
