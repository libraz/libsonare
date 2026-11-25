#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "mir/tempo_estimator_bridge.h"
#include "transport/musical_time.h"
#include "transport/tempo_map.h"
#include "util/constants.h"

namespace {

struct OwnedTempoSegments {
  SonareProjectTempoSegmentList value{};
  ~OwnedTempoSegments() { sonare_free_project_tempo_segment_list(&value); }
};

struct OwnedTimeSignatureSegments {
  SonareProjectTimeSignatureList value{};
  ~OwnedTimeSignatureSegments() { sonare_free_project_time_signature_list(&value); }
};

using ProjectPtr = std::unique_ptr<SonareProject, decltype(&sonare_project_destroy)>;

constexpr int kSampleRate = 22050;
constexpr int kBeatCount = 48;
constexpr double kBpm = 120.0;
constexpr double kFirstClickSeconds = 0.13;
constexpr double kBeatSeconds = 60.0 / kBpm;

constexpr double kSecondsPerMinuteOf(double bpm) {
  return sonare::constants::kSecondsPerMinute / bpm;
}

std::vector<float> make_click_train() {
  const double end_seconds =
      kFirstClickSeconds + static_cast<double>(kBeatCount - 1) * kBeatSeconds + 1.0;
  std::vector<float> samples(
      static_cast<size_t>(std::ceil(end_seconds * static_cast<double>(kSampleRate))), 0.0f);
  const size_t click_frames = static_cast<size_t>(kSampleRate / 100);

  for (int beat = 0; beat < kBeatCount; ++beat) {
    const double click_time = kFirstClickSeconds + static_cast<double>(beat) * kBeatSeconds;
    const size_t start = static_cast<size_t>(std::lround(click_time * kSampleRate));
    const float gain = beat % 4 == 0 ? 1.0f : 0.5f;
    for (size_t frame = 0; frame < click_frames && start + frame < samples.size(); ++frame) {
      samples[start + frame] =
          gain * static_cast<float>(std::sin(sonare::constants::kPiD * static_cast<double>(frame) /
                                             static_cast<double>(click_frames)));
    }
  }
  return samples;
}

// Exact beats at first_s + k * 60 / bpm, as the analyzer would hand them to the bridge.
sonare::mir::BeatAnalysisInput exact_beats(double first_s, double bpm, int count) {
  sonare::mir::BeatAnalysisInput input;
  input.bpm = static_cast<float>(bpm);
  input.sample_rate = kSampleRate;
  input.hop_length = 512;
  input.time_signature = {4, 4, 1.0f};
  for (int k = 0; k < count; ++k) {
    const double t = first_s + static_cast<double>(k) * kSecondsPerMinuteOf(bpm);
    input.beats.push_back({static_cast<float>(t), static_cast<int>(t * kSampleRate / 512), 1.0f});
  }
  input.downbeat_indices = {0};
  return input;
}

}  // namespace

TEST_CASE("a first event nearer sample 0 than one lead-in beat is placed on sample 0",
          "[mir][tempo]") {
  using sonare::Beat;
  const float sub = static_cast<float>(0.5 * sonare::transport::kMinLeadInSec);
  const float at = static_cast<float>(sonare::transport::kMinLeadInSec);
  std::vector<Beat> events{{sub, 0, 1.0f}, {sub + 0.5f, 22, 1.0f}};
  sonare::place_first_event_within_lead_in_on_zero(events);
  CHECK(events[0].time == 0.0f);
  CHECK(events[1].time == sub + 0.5f);

  std::vector<Beat> kept{{at, 0, 1.0f}};
  sonare::place_first_event_within_lead_in_on_zero(kept);
  CHECK(kept[0].time == at);
}

TEST_CASE("auto tempo maps put every detected beat on a beat line with and without the meter",
          "[mir][tempo]") {
  constexpr double kBeatBpm = 117.3;
  const double firsts[] = {sonare::transport::kMinLeadInSec, 0.004, 0.13, 0.371, 1.9};
  for (const double first : firsts) {
    CAPTURE(first);
    const sonare::mir::BeatAnalysisInput input = exact_beats(first, kBeatBpm, 32);
    const sonare::mir::TempoEstimate est = sonare::mir::estimate_tempo(input).front();
    const auto to_map = [](std::vector<sonare::transport::TempoSegment> segments) {
      auto map = std::make_unique<sonare::transport::TempoMap>();
      map->prepare(static_cast<double>(kSampleRate));
      map->set_segments(std::move(segments));
      return map;
    };
    // One sample at the analysis rate, in beats.
    const double tolerance_ppq = kBeatBpm / 60.0 / static_cast<double>(kSampleRate);

    const auto unmetered = to_map(est.unmetered_segments);
    REQUIRE(est.unmetered_segments.front().start_ppq == 0.0);
    CHECK(est.unmetered_segments.front().bpm <= sonare::transport::kMaxPublicTempoBpm);
    const auto metered = to_map(est.segments);
    const double bar_phase = est.time_sigs.front().start_ppq;
    for (const sonare::Beat& beat : input.beats) {
      const double t = static_cast<double>(beat.time);
      const double plain = unmetered->seconds_to_ppq(t);
      CHECK(std::abs(plain - std::round(plain)) <= tolerance_ppq);
      const double phased = metered->seconds_to_ppq(t) - bar_phase;
      CHECK(std::abs(phased - std::round(phased)) <= tolerance_ppq);
    }
  }
}

TEST_CASE("project auto tempo preserves a delayed 4/4 click grid", "[.][slow][mir][tempo]") {
  const std::vector<float> samples = make_click_train();
  SonareProject* raw_project = nullptr;
  REQUIRE(sonare_project_create(&raw_project) == SONARE_OK);
  const ProjectPtr project(raw_project, &sonare_project_destroy);

  SonareProjectAutoTempoResult auto_tempo{};
  REQUIRE(sonare_project_auto_tempo(project.get(), samples.data(), samples.size(), kSampleRate,
                                    nullptr, 0, 1, &auto_tempo) == SONARE_OK);
  CHECK(auto_tempo.bpm == Catch::Approx(kBpm).epsilon(0.03));

  OwnedTempoSegments tempo_list;
  REQUIRE(sonare_project_tempo_segments(project.get(), &tempo_list.value) == SONARE_OK);
  REQUIRE(tempo_list.value.count > 0);
  std::vector<sonare::transport::TempoSegment> tempo_segments;
  tempo_segments.reserve(tempo_list.value.count);
  for (size_t i = 0; i < tempo_list.value.count; ++i) {
    const SonareProjectTempoSegment& stored = tempo_list.value.items[i];
    tempo_segments.push_back({stored.start_ppq, stored.bpm, 0.0, stored.end_bpm});
  }

  OwnedTimeSignatureSegments signature_list;
  REQUIRE(sonare_project_time_signatures(project.get(), &signature_list.value) == SONARE_OK);
  REQUIRE(signature_list.value.count > 0);
  std::vector<sonare::transport::TimeSignatureSegment> signatures;
  signatures.reserve(signature_list.value.count);
  for (size_t i = 0; i < signature_list.value.count; ++i) {
    const SonareProjectTimeSignatureSegment& stored = signature_list.value.items[i];
    signatures.push_back({stored.start_ppq, {stored.numerator, stored.denominator}});
  }
  REQUIRE(signatures.front().time_sig.numerator == 4);
  REQUIRE(signatures.front().time_sig.denominator == 4);
  const double first_signature_start = signatures.front().start_ppq;

  sonare::transport::TempoMap map;
  map.prepare(static_cast<double>(kSampleRate));
  map.set_segments(std::move(tempo_segments));
  map.set_time_signatures(std::move(signatures));

  const int64_t first_bar = map.ppq_to_bar_beat(first_signature_start).bar;
  std::vector<double> errors;
  errors.reserve(kBeatCount);
  for (int beat = 0; beat < kBeatCount; ++beat) {
    const int64_t bar = first_bar + static_cast<int64_t>(beat / 4);
    const int beat_in_bar = beat % 4;
    const double ppq = map.bar_index_start_ppq(bar) +
                       static_cast<double>(beat_in_bar) * sonare::transport::note_length_ppq(4);
    const double estimated_seconds = map.ppq_to_seconds(ppq);
    const double true_seconds = kFirstClickSeconds + static_cast<double>(beat) * kBeatSeconds;
    errors.push_back(std::abs(estimated_seconds - true_seconds));
  }

  std::sort(errors.begin(), errors.end());
  const size_t middle = errors.size() / 2;
  const double median_error = (errors[middle - 1] + errors[middle]) / 2.0;
  CHECK(median_error <= 0.030);
}

TEST_CASE("project auto tempo without the meter keeps a delayed click grid on beat lines",
          "[.][slow][mir][tempo]") {
  const std::vector<float> samples = make_click_train();
  SonareProject* raw_project = nullptr;
  REQUIRE(sonare_project_create(&raw_project) == SONARE_OK);
  const ProjectPtr project(raw_project, &sonare_project_destroy);

  SonareProjectAutoTempoResult auto_tempo{};
  REQUIRE(sonare_project_auto_tempo(project.get(), samples.data(), samples.size(), kSampleRate,
                                    nullptr, 0, 0, &auto_tempo) == SONARE_OK);

  OwnedTempoSegments tempo_list;
  REQUIRE(sonare_project_tempo_segments(project.get(), &tempo_list.value) == SONARE_OK);
  REQUIRE(tempo_list.value.count >= 2);
  CHECK(tempo_list.value.items[0].start_ppq == 0.0);
  std::vector<sonare::transport::TempoSegment> tempo_segments;
  for (size_t i = 0; i < tempo_list.value.count; ++i) {
    const SonareProjectTempoSegment& stored = tempo_list.value.items[i];
    tempo_segments.push_back({stored.start_ppq, stored.bpm, 0.0, stored.end_bpm});
  }
  sonare::transport::TempoMap map;
  map.prepare(static_cast<double>(kSampleRate));
  map.set_segments(std::move(tempo_segments));

  std::vector<double> errors;
  for (int beat = 0; beat < kBeatCount; ++beat) {
    const double true_seconds = kFirstClickSeconds + static_cast<double>(beat) * kBeatSeconds;
    const double ppq = map.seconds_to_ppq(true_seconds);
    errors.push_back(std::abs(ppq - std::round(ppq)) * kBeatSeconds);
  }
  std::sort(errors.begin(), errors.end());
  const size_t middle = errors.size() / 2;
  CHECK((errors[middle - 1] + errors[middle]) / 2.0 <= 0.030);
}
