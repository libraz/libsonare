#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

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

}  // namespace

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
