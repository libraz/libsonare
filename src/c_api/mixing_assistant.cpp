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
#include "mixing/api/scene.h"
#include "mixing/assistant/config_from_params.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"
#include "sonare_c_mastering_helpers.h"
#endif

using namespace sonare_c_detail;

namespace {

#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT

namespace assistant = sonare::mixing::assistant;

// Shared argument checking for the two suggest entry points. Kept as one
// function so the two cannot drift into accepting different inputs.
SonareError check_track_arrays(const float* const* input_left, const char* const* track_ids,
                               const size_t* track_lengths, size_t input_count, int sample_rate,
                               const SonareMasteringParam* params, size_t param_count) {
  if (params == nullptr && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (input_count == 0) return SONARE_OK;
  if (input_left == nullptr || track_ids == nullptr || track_lengths == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sample_rate <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  for (size_t index = 0; index < input_count; ++index) {
    if (track_ids[index] == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
    if (track_lengths[index] > 0 && input_left[index] == nullptr) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  return SONARE_OK;
}

std::vector<assistant::TrackInput> build_tracks(const float* const* input_left,
                                                const float* const* input_right,
                                                const char* const* track_ids,
                                                const char* const* track_names,
                                                const size_t* track_lengths, size_t input_count,
                                                int sample_rate) {
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
    track.sample_rate = sample_rate;
    tracks.push_back(std::move(track));
  }
  return tracks;
}

#endif  // SONARE_WITH_MIXING_ASSISTANT

}  // namespace

SonareError sonare_mixing_assistant_suggest(
    const float* const* input_left, const float* const* input_right, const char* const* track_ids,
    const char* const* track_names, const size_t* track_lengths, size_t input_count,
    int sample_rate, const SonareMasteringParam* params, size_t param_count, char** json_out) {
  SONARE_C_API_ENTRY;
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  const SonareError err = check_track_arrays(input_left, track_ids, track_lengths, input_count,
                                             sample_rate, params, param_count);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  const auto parsed = sonare_c_mastering_detail::to_params(params, param_count);
  const auto config = assistant::mix_assistant_config_from_params(parsed.data(), parsed.size());
  const auto tracks = build_tracks(input_left, input_right, track_ids, track_names, track_lengths,
                                   input_count, sample_rate);
  const auto result = assistant::suggest_scene(tracks, config);
  *json_out = copy_string(assistant::mix_assistant_result_to_json(result));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  (void)input_left;
  (void)input_right;
  (void)track_ids;
  (void)track_names;
  (void)track_lengths;
  (void)input_count;
  (void)sample_rate;
  (void)params;
  (void)param_count;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_mixing_assistant_suggest_scene_json(
    const float* const* input_left, const float* const* input_right, const char* const* track_ids,
    const char* const* track_names, const size_t* track_lengths, size_t input_count,
    int sample_rate, const SonareMasteringParam* params, size_t param_count, char** json_out) {
  SONARE_C_API_ENTRY;
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  const SonareError err = check_track_arrays(input_left, track_ids, track_lengths, input_count,
                                             sample_rate, params, param_count);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  const auto parsed = sonare_c_mastering_detail::to_params(params, param_count);
  const auto config = assistant::mix_assistant_config_from_params(parsed.data(), parsed.size());
  const auto tracks = build_tracks(input_left, input_right, track_ids, track_names, track_lengths,
                                   input_count, sample_rate);
  const auto result = assistant::suggest_scene(tracks, config);
  *json_out = copy_string(sonare::mixing::api::scene_to_json(result.scene));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  (void)input_left;
  (void)input_right;
  (void)track_ids;
  (void)track_names;
  (void)track_lengths;
  (void)input_count;
  (void)sample_rate;
  (void)params;
  (void)param_count;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

const char* sonare_mixing_assistant_source_class_names(void) {
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  // thread_local rather than plain static: join_names writes into the storage
  // and returns a borrowed pointer into it, so two threads sharing one buffer
  // would hand each other a dangling view mid-rewrite.
  static thread_local std::string storage;
  return join_names(assistant::source_class_names(), storage);
#else
  return "";
#endif
}

int sonare_mixing_assistant_source_class_from_name(const char* name) {
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  if (name == nullptr) return -1;
  const std::string key(name);
  const auto names = assistant::source_class_names();
  for (size_t index = 0; index < names.size(); ++index) {
    if (names[index] == key) return static_cast<int>(index);
  }
  return -1;
#else
  (void)name;
  return -1;
#endif
}
