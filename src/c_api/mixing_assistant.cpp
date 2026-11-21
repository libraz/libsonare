/// @file mixing_assistant.cpp
/// @brief C ABI for the mixing assistant.
/// @details The entry points exist unconditionally so the exported surface does
///          not change with the build configuration; when the assistant is
///          compiled out they report SONARE_ERROR_NOT_SUPPORTED rather than
///          disappearing. A caller linking against a stripped build then gets a
///          runtime answer instead of a link error, and the cross-surface parity
///          check sees one stable set of symbols.

#include <sonare/sonare_c.h>

#include <string>
#include <vector>

#include "sonare_c_internal.h"

#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
#include "c_api/mixing_input_domains.h"
#include "mixing/assistant/config_from_params.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"
#include "sonare_c_mastering_helpers.h"
#include "util/parameter_checks.h"
#endif

using namespace sonare_c_detail;

namespace {

#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT

namespace assistant = sonare::mixing::assistant;

// Shared argument checking for the two suggest entry points. Kept as one
// function so the two cannot drift into accepting different inputs.
SonareError check_track_arrays(const float* const* input_left, const char* const* track_ids,
                               const size_t* track_lengths, const int* track_sample_rates,
                               size_t input_count, int sample_rate,
                               const SonareMasteringParam* params, size_t param_count) {
  if (input_count != 0) {
    SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(input_left, input_count);
    SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(track_ids, input_count);
    SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(track_lengths, input_count);
    for (size_t index = 0; index < input_count; ++index) {
      const auto at = [index] { return "[" + std::to_string(index) + "]"; };
      if (input_left[index] == nullptr && track_lengths[index] > 0) {
        return refuse_null_when_count(("input_left" + at()).c_str(),
                                      ("track_lengths" + at()).c_str());
      }
      if (track_ids[index] == nullptr) return refuse_null(("track_ids" + at()).c_str());
      if (track_sample_rates != nullptr && track_sample_rates[index] <= 0) {
        const std::string name = "track_sample_rates" + at();
        return run_validation([&] {
          sonare::util::check::integer_within(track_sample_rates[index],
                                              kMixingAssistantTrackSampleRateDomain, name.c_str());
        });
      }
    }
    if (const SonareError err = run_validation([&] {
          sonare::util::check::integer_within(sample_rate, kMixingAssistantSampleRateDomain,
                                              "sample_rate");
        });
        err != SONARE_OK) {
      return err;
    }
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(params, param_count);
  return SONARE_OK;
}

std::vector<assistant::TrackInput> build_tracks(
    const float* const* input_left, const float* const* input_right, const char* const* track_ids,
    const char* const* track_names, const size_t* track_lengths, const int* track_sample_rates,
    size_t input_count, int sample_rate, std::vector<std::vector<float>>& resampled) {
  std::vector<assistant::TrackInput> tracks;
  tracks.reserve(input_count);
  for (size_t index = 0; index < input_count; ++index) {
    assistant::TrackInput track;
    track.id = track_ids[index];
    if (track_names != nullptr && track_names[index] != nullptr) {
      track.name = track_names[index];
    }
    track.left = input_left[index];
    track.right = input_right != nullptr ? input_right[index] : nullptr;
    track.frame_count = track_lengths[index];
    track.sample_rate = track_sample_rates != nullptr ? track_sample_rates[index] : sample_rate;
    tracks.push_back(std::move(track));
  }
  return assistant::tracks_at_rate(std::move(tracks), sample_rate, resampled);
}

#endif  // SONARE_WITH_MIXING_ASSISTANT

}  // namespace

SonareError sonare_mixing_assistant_suggest(
    const float* const* input_left, const float* const* input_right, const char* const* track_ids,
    const char* const* track_names, const size_t* track_lengths, const int* track_sample_rates,
    size_t input_count, int sample_rate, const SonareMasteringParam* params, size_t param_count,
    char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  if (const SonareError err =
          check_track_arrays(input_left, track_ids, track_lengths, track_sample_rates, input_count,
                             sample_rate, params, param_count);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(json_out);

  SONARE_C_TRY
  const auto parsed = sonare_c_mastering_detail::to_params(params, param_count);
  const auto config = assistant::mix_assistant_config_from_params(parsed.data(), parsed.size());
  std::vector<std::vector<float>> resampled;
  const auto tracks = build_tracks(input_left, input_right, track_ids, track_names, track_lengths,
                                   track_sample_rates, input_count, sample_rate, resampled);
  const auto result = assistant::suggest_scene(tracks, config);
  *json_out = copy_string(assistant::mix_assistant_result_to_json(result));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_REQUIRE_NONNULL(json_out);
  (void)input_left;
  (void)input_right;
  (void)track_ids;
  (void)track_names;
  (void)track_lengths;
  (void)track_sample_rates;
  (void)input_count;
  (void)sample_rate;
  (void)params;
  (void)param_count;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

const char* sonare_mixing_assistant_source_class_names(void) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  // thread_local rather than plain static: join_names writes into the storage
  // and returns a borrowed pointer into it, so two threads sharing one buffer
  // would hand each other a dangling view mid-rewrite.
  static thread_local std::string storage;
  return join_names(assistant::source_class_names(), storage);
#else
  return "";
#endif
  SONARE_C_CATCH_RETURN("")
}

int sonare_mixing_assistant_source_class_from_name(const char* name) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  if (name == nullptr) {
    sonare_c_detail::refuse_null("name");
    return -1;
  }
  const std::string key(name);
  const auto names = assistant::source_class_names();
  for (size_t index = 0; index < names.size(); ++index) {
    if (names[index] == key) return static_cast<int>(index);
  }
  return -1;
#else
  static_cast<void>(name);
  return -1;
#endif
  SONARE_C_CATCH_RETURN(-1)
}
