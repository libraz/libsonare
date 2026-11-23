// Every null a result writer emits sits at a path whose infinite end is declared.
//
// A JSON result writes a NaN member as an absent key and an infinite one as null, so a
// reader cannot tell which infinity a null stands for without the writer saying so. Each
// family's `<family>_null_means()` list is that statement. This case renders each family
// from inputs that drive its levels to their ends -- silence, one silent side, an
// out-of-phase pair -- and holds the nulls it finds to the list in both directions.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "analysis/analysis_json.h"
#include "mastering/api/named_processor.h"
#include "mastering/assistant/audio_profile.h"
#include "mastering/assistant/repair_session.h"
#include "mastering/assistant/suggester.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mixing/assistant/suggester.h"
#include "quick.h"
#include "util/constants.h"
#include "util/json.h"

namespace {

namespace json = sonare::util::json;

constexpr int kSampleRate = 48000;
constexpr std::size_t kLength = kSampleRate;

std::vector<float> tones(float low_hz, float high_hz, float amplitude) {
  std::vector<float> samples(kLength);
  for (std::size_t i = 0; i < kLength; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[i] = amplitude * (0.6f * std::sin(sonare::constants::kTwoPi * low_hz * t) +
                              0.4f * std::sin(sonare::constants::kTwoPi * high_hz * t));
  }
  return samples;
}

void collect_null_paths(const json::Value& value, const std::string& prefix,
                        std::set<std::string>& out) {
  if (value.is_null()) {
    out.insert(prefix);
    return;
  }
  if (value.is_object()) {
    for (const auto& [key, child] : value.as_object()) {
      collect_null_paths(child, prefix.empty() ? key : prefix + "." + key, out);
    }
    return;
  }
  if (value.is_array()) {
    for (std::size_t i = 0; i < value.size(); ++i) collect_null_paths(value[i], prefix + "[]", out);
  }
}

std::set<std::string> null_paths_of(const std::string& text) {
  std::set<std::string> out;
  collect_null_paths(json::parse(text), "", out);
  return out;
}

// The first half of @p samples followed by exact zeros.
std::vector<float> tone_then_silence(std::vector<float> samples) {
  std::fill(samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2), samples.end(), 0.0f);
  return samples;
}

using NullMeans = std::vector<std::pair<std::string, std::string>>;

struct Family {
  std::string name;
  const NullMeans& declared;
  const std::vector<std::string>& paths;
  std::vector<std::function<std::string()>> renders;
};

}  // namespace

TEST_CASE("every null a result writes sits at a path declaring its infinite end",
          "[conformance][schema]") {
  namespace api = sonare::mastering::api;
  namespace assistant = sonare::mastering::assistant;
  namespace mixing = sonare::mixing::assistant;
  const std::vector<float> silence(kLength, 0.0f);
  const auto music = tones(220.0f, 1760.0f, 0.2f);
  std::vector<float> inverted(music);
  for (float& sample : inverted) sample = -sample;

  const auto pair_renders = [&](const char* name) {
    std::vector<std::function<std::string()>> renders;
    for (const auto& [source, reference] :
         {std::pair{&silence, &music}, std::pair{&music, &silence},
          std::pair{&silence, &silence}}) {
      renders.emplace_back([&, name, source = source, reference = reference] {
        return api::analyze_named_pair(name, source->data(), reference->data(), kLength,
                                       kSampleRate);
      });
    }
    return renders;
  };
  const auto stereo_renders = [&](const char* name, const std::vector<api::Param>& params) {
    std::vector<std::function<std::string()>> renders;
    const std::vector<const std::vector<float>*> rights = {&inverted, &silence, &music};
    for (const std::vector<float>* right : rights) {
      renders.emplace_back([&, name, right, params] {
        return api::analyze_named_stereo(name, music.data(), right->data(), kLength, kSampleRate,
                                         params);
      });
    }
    renders.emplace_back([&, name, params] {
      return api::analyze_named_stereo(name, silence.data(), silence.data(), kLength, kSampleRate,
                                       params);
    });
    return renders;
  };
  assistant::AudioProfileConfig detecting;
  detecting.detect_defects = true;
  const auto repair = [&](const std::vector<float>& left) {
    return [&] {
      const float* planes[] = {left.data(), silence.data()};
      return assistant::repair_analyze_json(planes, 2, kLength, kSampleRate, "");
    };
  };
  mixing::TrackInput silent_track;
  silent_track.id = "silent";
  silent_track.left = silence.data();
  silent_track.right = silence.data();
  silent_track.frame_count = kLength;
  silent_track.sample_rate = kSampleRate;
  mixing::TrackInput wide_track = silent_track;
  wide_track.id = "wide";
  wide_track.left = music.data();
  wide_track.right = inverted.data();

  const std::vector<Family> families = {
      {"streaming_preview",
       sonare::mastering::maximizer::streaming_preview_null_means(),
       sonare::mastering::maximizer::streaming_preview_schema_paths(),
       {[&] {
         return sonare::mastering::maximizer::streaming_preview_to_json(
             sonare::mastering::maximizer::streaming_preview(
                 sonare::Audio::from_buffer(silence.data(), silence.size(), kSampleRate)));
       }}},
      {"match_reference_loudness", api::match_reference_loudness_null_means(),
       api::match_reference_loudness_schema_paths(), pair_renders("match.referenceLoudness")},
      {"match_tonal_balance", api::match_tonal_balance_null_means(),
       api::match_tonal_balance_schema_paths(), pair_renders("match.tonalBalance")},
      {"match_tonal_balance (log bands)", api::match_tonal_balance_null_means(),
       api::match_tonal_balance_schema_paths(), pair_renders("match.tonalBalanceLogBands")},
      // A silent side is refused, and a band empty on either side is decided as a finite
      // gain, so the curve writes no null from any pair it accepts.
      {"match_eq_curve",
       api::match_eq_curve_null_means(),
       api::match_eq_curve_schema_paths(),
       {[&] {
          return api::analyze_named_pair("match.matchEqCurve", music.data(), inverted.data(),
                                         kLength, kSampleRate);
        },
        [&, low = tones(110.0f, 220.0f, 0.2f), high = tones(3520.0f, 7040.0f, 0.2f)] {
          return api::analyze_named_pair("match.matchEqCurve", low.data(), high.data(), kLength,
                                         kSampleRate);
        }}},
      {"stereo_mono_compat", api::stereo_mono_compat_null_means(),
       api::stereo_mono_compat_schema_paths(), stereo_renders("stereo.monoCompatCheck", {})},
      {"match_reference_delay", api::match_reference_delay_null_means(),
       api::match_reference_delay_schema_paths(),
       pair_renders("match.estimateReferenceDelaySamples")},
      {"stereo_mono_compat_log_bands", api::stereo_mono_compat_log_bands_null_means(),
       api::stereo_mono_compat_log_bands_schema_paths(),
       stereo_renders("stereo.monoCompatCheckLogBands", {{"highHz", 10000.0}})},
      {"audio_profile",
       assistant::audio_profile_null_means(),
       assistant::audio_profile_schema_paths(),
       {[&] {
         return assistant::audio_profile_to_json(assistant::analyze_audio_profile(
             silence.data(), silence.size(), kSampleRate, detecting));
       }}},
      {"assistant_result",
       assistant::assistant_result_null_means(),
       assistant::assistant_result_schema_paths(),
       {[&] {
         return assistant::assistant_result_to_json(
             assistant::suggest_chain(silence.data(), silence.size(), kSampleRate));
       }}},
      {"repair_analysis",
       assistant::repair_analysis_null_means(),
       assistant::repair_analysis_schema_paths(),
       {repair(silence), repair(music)}},
      {"analysis_result",
       sonare::analysis_result_null_means(),
       sonare::analysis_result_schema_paths(),
       {[&] {
          return sonare::analysis_result_to_json(
              sonare::quick::analyze(silence.data(), silence.size(), kSampleRate));
        },
        // Tone then zeros, read at the analysis rate so no resampler tail fills the zeros: the
        // range over the silent low percentile is +inf.
        [&, gapped = tone_then_silence(music)] {
          return sonare::analysis_result_to_json(
              sonare::quick::analyze(gapped.data(), gapped.size(), 22050));
        }}},
      {"mix_assistant_result",
       mixing::mix_assistant_result_null_means(),
       mixing::mix_assistant_result_schema_paths(),
       {[&] {
         return mixing::mix_assistant_result_to_json(
             mixing::suggest_scene({silent_track, wide_track}));
       }}},
  };

  for (const Family& family : families) {
    INFO(family.name);
    const std::set<std::string> listed(family.paths.begin(), family.paths.end());
    std::set<std::string> declared;
    for (const auto& [path, end] : family.declared) {
      INFO(path);
      CHECK((end == "-Infinity" || end == "Infinity"));
      // A null array element is declared as "<array>[]"; the path list names the array.
      const bool element = path.size() > 2 && path.compare(path.size() - 2, 2, "[]") == 0;
      CHECK(listed.count(element ? path.substr(0, path.size() - 2) : path) == 1);
      declared.insert(path);
    }
    std::set<std::string> found;
    for (const auto& render : family.renders) {
      std::string text;
      REQUIRE_NOTHROW(text = render());
      for (const auto& path : null_paths_of(text)) found.insert(path);
    }
    // Both directions: an undeclared null has no stated end, and a declared path no render
    // reaches is a statement nothing here checks.
    CHECK(found == declared);
  }
}
