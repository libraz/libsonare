/// @file voice_f0_tracker_test.cpp
/// @brief Accuracy, voicing, emission timing, rate independence and allocation freedom of the
///        streaming f0 tracker, measured against synthetic voices with a known f0 per sample.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "editing/voice_changer/streaming_f0.h"
#include "support/alloc_guard.h"
#include "util/constants.h"
#include "voicelab/synthetic_voice.h"

namespace {

using sonare::editing::voice_changer::F0Estimate;
using sonare::editing::voice_changer::StreamingF0Tracker;
using sonare::voicelab::VoiceSegment;
namespace vl = sonare::voicelab;

constexpr float kF0MinHz = 70.0f;
constexpr float kLevelDb = -20.0f;
constexpr float kRampMs = 10.0f;
// Decimation factor and the group delay (input samples) of the 49-tap decimation FIR.
constexpr int kDecimation = 3;
constexpr int kGroupDelay = 24;

/// Later half of the analysis window in input samples: the longest searched lag, decimated.
int window_half(double sr, float f0_min) {
  return kDecimation * static_cast<int>(std::ceil(sr / kDecimation / f0_min));
}

double cents(double a, double b) { return sonare::constants::kCentsPerOctave * std::log2(a / b); }

double median(std::vector<double> v) {
  REQUIRE_FALSE(v.empty());
  const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
  std::nth_element(v.begin(), mid, v.end());
  return *mid;
}

struct Emitted {
  F0Estimate estimate;
  std::int64_t block_start;  ///< First input index of the process() call that returned it.
  int block_size;
};

/// Streams `x` through a fresh tracker in blocks cycling through `blocks`.
std::vector<Emitted> track(const std::vector<float>& x, double sr, const std::vector<int>& blocks,
                           int* max_per_call_excess = nullptr) {
  StreamingF0Tracker tracker;
  const int max_block = *std::max_element(blocks.begin(), blocks.end());
  tracker.prepare(sr, max_block, kF0MinHz);
  const int hop = tracker.hop_input_samples();
  std::vector<F0Estimate> scratch(static_cast<size_t>(max_block / hop + 2));
  std::vector<Emitted> result;
  std::size_t pos = 0;
  std::size_t b = 0;
  int excess = 0;
  while (pos < x.size()) {
    const int n = static_cast<int>(std::min<std::size_t>(
        static_cast<std::size_t>(blocks[b++ % blocks.size()]), x.size() - pos));
    const int got =
        tracker.process(x.data() + pos, n, scratch.data(), static_cast<int>(scratch.size()));
    excess = std::max(excess, got - ((n + hop - 1) / hop + 1));
    for (int i = 0; i < got; ++i) {
      result.push_back({scratch[static_cast<size_t>(i)], static_cast<std::int64_t>(pos), n});
    }
    pos += static_cast<std::size_t>(n);
  }
  if (max_per_call_excess != nullptr) *max_per_call_excess = excess;
  return result;
}

VoiceSegment steady(double seconds, float f0, int vowel, float vibrato_cents = 0.0f,
                    float vibrato_hz = 0.0f) {
  return {seconds, f0, f0, vibrato_cents, vibrato_hz, true, vowel, kLevelDb, kRampMs};
}

VoiceSegment noise(double seconds, float level_db = kLevelDb) {
  return {seconds, 0.0f, 0.0f, 0.0f, 0.0f, false, 0, level_db, kRampMs};
}

/// Signed cent errors of the estimates whose whole window lies inside the voiced span
/// [begin, end) of the input, away from the segment ramps.
std::vector<double> interior_errors(const std::vector<Emitted>& run,
                                    const std::vector<float>& truth, std::int64_t begin,
                                    std::int64_t end, int guard) {
  std::vector<double> errors;
  for (const Emitted& e : run) {
    const std::int64_t c = e.estimate.centre_sample;
    if (c < begin + guard || c >= end - guard) continue;
    const float f = truth[static_cast<size_t>(c)];
    if (f <= 0.0f) continue;
    errors.push_back(cents(e.estimate.f0_hz, f));
  }
  return errors;
}

}  // namespace

TEST_CASE("f0 tracker hop and decision lag follow the decimated-window emission rule",
          "[voice_f0]") {
  struct Expected {
    double sr;
    int hop;
    int lag;
  };
  for (const Expected& expected : {Expected{48000.0, 96, 768}, Expected{44100.0, 87, 696}}) {
    StreamingF0Tracker tracker;
    tracker.prepare(expected.sr, 512, kF0MinHz);
    const int hop = tracker.hop_input_samples();
    const int needed = window_half(expected.sr, kF0MinHz) + kGroupDelay;
    INFO("sr " << expected.sr << " lag " << tracker.decision_lag_samples());
    CHECK(hop == expected.hop);
    CHECK(tracker.decision_lag_samples() == (needed + hop - 1) / hop * hop);
    CHECK(tracker.decision_lag_samples() == expected.lag);
  }
}

TEST_CASE("f0 tracker emits each estimate in the block that reaches centre + T_max + delay",
          "[voice_f0]") {
  for (const double sr : {48000.0, 44100.0}) {
    const std::vector<float> x = vl::synthesize_voice({steady(0.25, 180.0f, 1)}, sr);
    StreamingF0Tracker reference;
    reference.prepare(sr, 1, kF0MinHz);
    const int hop = reference.hop_input_samples();
    const int lag = reference.decision_lag_samples();
    const int needed = window_half(sr, kF0MinHz) + kGroupDelay;
    for (const std::vector<int>& blocks :
         {std::vector<int>{1}, std::vector<int>{7, 128, 513, 3}, std::vector<int>{96}}) {
      int excess = 0;
      const std::vector<Emitted> run = track(x, sr, blocks, &excess);
      REQUIRE_FALSE(run.empty());
      CHECK(excess <= 0);
      // One estimate per hop, centred on the hop grid from the start of the stream.
      CHECK(static_cast<std::int64_t>(run.size()) ==
            (static_cast<std::int64_t>(x.size()) - lag) / hop + 1);
      int misplaced = 0;
      for (std::size_t i = 0; i < run.size(); ++i) {
        const Emitted& e = run[i];
        const std::int64_t centre = e.estimate.centre_sample;
        const std::int64_t due = (centre + needed + hop - 1) / hop * hop;
        const bool placed = centre == static_cast<std::int64_t>(i) * hop && due - centre == lag &&
                            e.block_start < due && due <= e.block_start + e.block_size;
        if (!placed) ++misplaced;
      }
      INFO("sr " << sr << " first block " << blocks.front());
      CHECK(misplaced == 0);
    }
  }
}

TEST_CASE("f0 tracker centre_sample is the centre of the analysed window", "[voice_f0]") {
  // A step from 150 to 220 Hz: windows wholly before the step read 150, wholly after read 220.
  const double sr = 48000.0;
  const VoiceSegment low{0.3, 150.0f, 150.0f, 0.0f, 0.0f, true, 0, kLevelDb, 0.0f};
  const VoiceSegment high{0.3, 220.0f, 220.0f, 0.0f, 0.0f, true, 0, kLevelDb, 0.0f};
  const std::vector<float> x = vl::synthesize_voice({low, high}, sr);
  const auto step = static_cast<std::int64_t>(0.3 * sr);
  const int margin = window_half(sr, kF0MinHz) + kGroupDelay;
  int before = 0;
  int after = 0;
  int wrong = 0;
  for (const Emitted& e : track(x, sr, {128})) {
    const std::int64_t c = e.estimate.centre_sample;
    if (c < margin) continue;
    if (c <= step - margin) {
      ++before;
      if (std::abs(cents(e.estimate.f0_hz, 150.0)) > 20.0) ++wrong;
    } else if (c >= step + margin) {
      ++after;
      if (std::abs(cents(e.estimate.f0_hz, 220.0)) > 20.0) ++wrong;
    }
  }
  CHECK(before > 20);
  CHECK(after > 20);
  CHECK(wrong == 0);
}

TEST_CASE("f0 tracker is within 5 cent median from 82 to 800 Hz with rare octave errors",
          "[voice_f0]") {
  const double sr = 48000.0;
  const double seconds = 0.22;
  constexpr int kVowelI = 2;  // /i/: its strong upper formants put ripple dips beside the period.
  const int guard =
      window_half(sr, kF0MinHz) + kGroupDelay + static_cast<int>(std::lround(kRampMs * 1e-3 * sr));
  int octave_errors = 0;
  int gross_errors = 0;
  std::size_t frames = 0;
  int vowel = 0;
  for (const float f0 : {82.0f, 110.0f, 165.0f, 220.0f, 330.0f, 440.0f, 660.0f, 800.0f}) {
    for (const VoiceSegment& segment : {steady(seconds, f0, vowel), steady(seconds, f0, kVowelI),
                                        steady(seconds, f0, kVowelI, 50.0f, 5.5f)}) {
      std::vector<float> truth;
      const std::vector<float> x = vl::synthesize_voice({segment}, sr, &truth);
      const std::vector<double> errors = interior_errors(
          track(x, sr, {128}), truth, 0, static_cast<std::int64_t>(x.size()), guard);
      REQUIRE(errors.size() > 50);
      std::vector<double> magnitude;
      for (const double e : errors) {
        magnitude.push_back(std::abs(e));
        if (std::abs(e) > 600.0) ++octave_errors;
        if (std::abs(e) > 50.0) ++gross_errors;
      }
      frames += errors.size();
      INFO("f0 " << f0 << " vowel " << segment.vowel << " vibrato " << segment.vibrato_cents
                 << " median " << median(magnitude) << " cent");
      CHECK(median(magnitude) <= 5.0);
    }
    vowel = (vowel + 1) % 5;
  }
  INFO("octave errors " << octave_errors << ", gross errors " << gross_errors << " of " << frames);
  CHECK(static_cast<double>(octave_errors) <= 0.005 * static_cast<double>(frames));
  // The gross-error bound the shifter's output is held to; a tracker over it cannot meet it.
  CHECK(static_cast<double>(gross_errors) <= 0.01 * static_cast<double>(frames));
}

TEST_CASE("f0 tracker voicing agrees with alternating voiced and unvoiced input", "[voice_f0]") {
  const double sr = 48000.0;
  const std::vector<VoiceSegment> segments = {
      steady(0.2, 110.0f, 0),
      noise(0.2),
      steady(0.2, 330.0f, 2, 30.0f, 5.0f),
      noise(0.2, -40.0f),
      steady(0.2, 200.0f, 4),
      noise(0.2, -90.0f),
      steady(0.2, 600.0f, 1),
      noise(0.2),
  };
  std::vector<float> truth;
  const std::vector<float> x = vl::synthesize_voice(segments, sr, &truth);
  StreamingF0Tracker probe;
  probe.prepare(sr, 128, kF0MinHz);
  // Transitions are excluded for a window half, the two-estimate hysteresis and the ramp.
  const int guard = window_half(sr, kF0MinHz) + kGroupDelay + 2 * probe.hop_input_samples() +
                    static_cast<int>(std::lround(kRampMs * 1e-3 * sr));
  std::vector<std::int64_t> boundaries;
  double t = 0.0;
  for (const VoiceSegment& s : segments) {
    t += s.duration_s;
    boundaries.push_back(static_cast<std::int64_t>(std::llround(t * sr)));
  }
  int agree = 0;
  int total = 0;
  int voiced_frames = 0;
  for (const Emitted& e : track(x, sr, {128})) {
    const std::int64_t c = e.estimate.centre_sample;
    if (c < guard || c >= static_cast<std::int64_t>(x.size())) continue;
    bool near_boundary = false;
    for (const std::int64_t b : boundaries) near_boundary |= std::llabs(c - b) < guard;
    if (near_boundary) continue;
    const bool truly_voiced = truth[static_cast<size_t>(c)] > 0.0f;
    ++total;
    if (truly_voiced) ++voiced_frames;
    if (e.estimate.voiced == truly_voiced) ++agree;
  }
  REQUIRE(voiced_frames > total / 4);
  REQUIRE(voiced_frames < 3 * total / 4);
  INFO("agree " << agree << " of " << total);
  CHECK(static_cast<double>(agree) >= 0.97 * static_cast<double>(total));
}

TEST_CASE("f0 tracker reads the same f0 at 44.1 and 48 kHz", "[voice_f0]") {
  for (const float f0 : {110.0f, 330.0f, 700.0f}) {
    std::vector<double> medians;
    std::vector<std::vector<std::pair<double, double>>> tracks;
    for (const double sr : {48000.0, 44100.0}) {
      std::vector<float> truth;
      const std::vector<float> x =
          vl::synthesize_voice({steady(0.3, f0, 3, 40.0f, 5.0f)}, sr, &truth);
      const int guard = window_half(sr, kF0MinHz) + kGroupDelay +
                        static_cast<int>(std::lround(kRampMs * 1e-3 * sr));
      std::vector<double> signed_errors;
      std::vector<std::pair<double, double>> timed;
      for (const Emitted& e : track(x, sr, {128})) {
        const std::int64_t c = e.estimate.centre_sample;
        if (c < guard || c >= static_cast<std::int64_t>(x.size()) - guard) continue;
        signed_errors.push_back(cents(e.estimate.f0_hz, truth[static_cast<size_t>(c)]));
        timed.emplace_back(static_cast<double>(c) / sr, e.estimate.f0_hz);
      }
      medians.push_back(median(signed_errors));
      tracks.push_back(timed);
    }
    // Pair each 48 kHz estimate with the 44.1 kHz estimate nearest in time (hops differ).
    std::vector<double> differences;
    for (const auto& [time, hz] : tracks[0]) {
      const auto nearest = std::min_element(
          tracks[1].begin(), tracks[1].end(), [time = time](const auto& a, const auto& b) {
            return std::abs(a.first - time) < std::abs(b.first - time);
          });
      if (nearest != tracks[1].end() && std::abs(nearest->first - time) < 0.001) {
        differences.push_back(std::abs(cents(hz, nearest->second)));
      }
    }
    INFO("f0 " << f0 << " median bias 48k " << medians[0] << " 44.1k " << medians[1]);
    CHECK(std::abs(medians[0] - medians[1]) <= 2.0);
    REQUIRE(differences.size() > 50);
    INFO("median paired difference " << median(differences) << " cent");
    CHECK(median(differences) <= 3.0);
  }
}

TEST_CASE("f0 tracker process allocates nothing after prepare", "[voice_f0][rt]") {
  constexpr int kBlock = 128;
  const double sr = 48000.0;
  const std::vector<float> x = vl::synthesize_voice({steady(0.2, 220.0f, 0), noise(0.1)}, sr);
  StreamingF0Tracker tracker;
  tracker.prepare(sr, kBlock, kF0MinHz);
  std::vector<F0Estimate> out(static_cast<size_t>(kBlock / tracker.hop_input_samples() + 2));
  const int capacity = static_cast<int>(out.size());
  tracker.process(x.data(), kBlock, out.data(), capacity);
  int emitted = 0;
  std::size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    for (std::size_t pos = 0; pos + kBlock <= x.size(); pos += kBlock) {
      emitted += tracker.process(x.data() + pos, kBlock, out.data(), capacity);
      if (pos == 40 * kBlock) tracker.reset();
    }
    allocations = guard.count();
  }
  CHECK(emitted > 0);
  CHECK(allocations == 0);
}
