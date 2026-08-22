/// @file mixing_assistant.cpp
/// @brief Embind bindings for the offline mixing assistant.
///
/// @details The WASM target does not link the aggregate C-ABI translation unit,
///          so none of the argument checking that layer performs is inherited
///          here. Every precondition the C entry point enforces before it calls
///          the core is written out again in this file, plus one WASM-only
///          check: a right-channel buffer shorter than its left plane would be
///          read past its end, because the core carries a single frame count per
///          track.
///
/// @details Two preconditions are deliberately *not* restated: unique track ids
///          and finite samples are checked inside the core, so both surfaces
///          inherit one implementation of each rather than keeping two that can
///          drift. Anything the C layer checks before reaching the core has to
///          be here; anything the core checks itself must not be.
///
/// @details Argument order mirrors the C entry point
///          (left / right / ids / names / sample rate / params). The per-track
///          frame counts and the track count that entry point takes as separate
///          arguments are derived from the JS array lengths instead.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
#include "mixing/api/scene.h"
#include "mixing/assistant/config_from_params.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"
#endif

namespace {

#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT

namespace assistant = sonare::mixing::assistant;

// Planar per-track input copied out of JS. The core reads the sample pointers
// during the analysis call, so the copies have to outlive it; they are held here
// alongside the TrackInput views that point into them.
struct AssistantInput {
  std::vector<std::vector<float>> left;
  // One entry per track; empty for a mono track (no right plane).
  std::vector<std::vector<float>> right;
  std::vector<assistant::TrackInput> tracks;
};

bool isJsArray(const val& value) { return val::global("Array").call<bool>("isArray", value); }

bool isAbsent(const val& value) { return value.isUndefined() || value.isNull(); }

// Rejects a non-array where an array of per-track entries is expected. Absent
// optional arrays are the caller's business and are handled at the call site.
void requireJsArray(const val& value, const char* subject) {
  if (!isJsArray(value)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  std::string(subject) + " must be an array");
  }
}

std::string requireTrackId(const val& ids, std::size_t index) {
  const val value = ids[index];
  if (isAbsent(value) || value.typeOf().as<std::string>() != "string") {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "trackIds[" + std::to_string(index) + "] must be a non-empty string");
  }
  std::string id = value.as<std::string>();
  if (id.empty()) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "trackIds[" + std::to_string(index) + "] must be a non-empty string");
  }
  return id;
}

// Optional display name; used only as a classifier hint, so an absent entry is
// an empty name rather than an error. A present entry must still be a string.
std::string optionalTrackName(const val& names, std::size_t index) {
  if (isAbsent(names)) return {};
  const val value = names[index];
  if (isAbsent(value)) return {};
  if (value.typeOf().as<std::string>() != "string") {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "trackNames[" + std::to_string(index) + "] must be a string");
  }
  return value.as<std::string>();
}

AssistantInput buildAssistantInput(val left_channels, val right_channels, val track_ids,
                                   val track_names, int sample_rate) {
  requireJsArray(left_channels, "leftChannels");
  requireJsArray(track_ids, "trackIds");
  if (!isAbsent(right_channels)) requireJsArray(right_channels, "rightChannels");
  if (!isAbsent(track_names)) requireJsArray(track_names, "trackNames");

  const std::size_t count = wasmArrayLikeLength(left_channels, "leftChannels");
  if (wasmArrayLikeLength(track_ids, "trackIds") != count) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "trackIds must have one entry per track");
  }
  if (!isAbsent(right_channels) && wasmArrayLikeLength(right_channels, "rightChannels") != count) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "rightChannels must have one entry per track");
  }
  if (!isAbsent(track_names) && wasmArrayLikeLength(track_names, "trackNames") != count) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "trackNames must have one entry per track");
  }

  AssistantInput input;
  // Zero tracks is a valid degenerate call that yields an empty suggestion, and
  // the sample rate is never read on that path, so it is checked only once there
  // is audio for it to describe.
  if (count == 0) return input;
  if (sample_rate <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "sampleRate must be positive");
  }

  input.left.resize(count);
  input.right.resize(count);
  std::vector<std::string> ids(count);
  std::vector<std::string> names(count);
  std::size_t budget = 0;
  for (std::size_t index = 0; index < count; ++index) {
    ids[index] = requireTrackId(track_ids, index);
    names[index] = optionalTrackName(track_names, index);

    const val left = left_channels[index];
    if (isAbsent(left)) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "leftChannels[" + std::to_string(index) + "] must be a Float32Array");
    }
    const std::size_t frames = accumulateWasmFloat32ArrayLength(
        left, "leftChannels entry", "mixingAssistantSuggest input", &budget);
    input.left[index] = float32ArrayToVector(left);

    const val right = isAbsent(right_channels) ? val::undefined() : right_channels[index];
    if (!isAbsent(right)) {
      // The core carries one frame count per track and reads both planes with
      // it, so a shorter right plane would be read past its end.
      const std::size_t right_frames = accumulateWasmFloat32ArrayLength(
          right, "rightChannels entry", "mixingAssistantSuggest input", &budget);
      if (right_frames != frames) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "track " + std::to_string(index) + " left and right lengths must match");
      }
      input.right[index] = float32ArrayToVector(right);
    }
  }

  // Pointers are taken only after every buffer is in place, so no later resize
  // can invalidate one.
  input.tracks.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    assistant::TrackInput track;
    track.id = std::move(ids[index]);
    track.name = std::move(names[index]);
    track.left = input.left[index].empty() ? nullptr : input.left[index].data();
    track.right = input.right[index].empty() ? nullptr : input.right[index].data();
    track.frame_count = input.left[index].size();
    track.sample_rate = sample_rate;
    input.tracks.push_back(std::move(track));
  }
  return input;
}

assistant::MixAssistantConfig assistantConfigFromParams(val params_obj) {
  // The same flat param list the C and Python paths build, so a JS options bag
  // and a C param array resolve to one config parser.
  const std::vector<mastering::api::Param> params = masteringParamsFromObject(params_obj);
  return assistant::mix_assistant_config_from_params(params.data(), params.size());
}

#else

// The entry points stay registered when the assistant is compiled out, so the
// JS surface does not change shape with the build configuration; a caller gets
// a runtime answer instead of a missing function.
[[noreturn]] void throwUnavailable() {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "this build does not include the mixing assistant");
}

#endif  // SONARE_WITH_MIXING_ASSISTANT

}  // namespace

// Analyses a set of planar tracks and returns the suggested scene, the
// measurements behind it and the explanation as one JSON document. The
// assistant only suggests: applying the scene is the caller's separate step
// through Mixer.fromSceneJson.
std::string js_mixing_assistant_suggest(val left_channels, val right_channels, val track_ids,
                                        val track_names, int sample_rate, val params_obj) {
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  const assistant::MixAssistantConfig config = assistantConfigFromParams(params_obj);
  const AssistantInput input =
      buildAssistantInput(left_channels, right_channels, track_ids, track_names, sample_rate);
  return assistant::mix_assistant_result_to_json(assistant::suggest_scene(input.tracks, config));
#else
  (void)left_channels;
  (void)right_channels;
  (void)track_ids;
  (void)track_names;
  (void)sample_rate;
  (void)params_obj;
  throwUnavailable();
#endif
}

// As js_mixing_assistant_suggest, but returns only the suggested scene, in the
// schema Mixer.fromSceneJson reads.
std::string js_mixing_assistant_suggest_scene_json(val left_channels, val right_channels,
                                                   val track_ids, val track_names, int sample_rate,
                                                   val params_obj) {
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  const assistant::MixAssistantConfig config = assistantConfigFromParams(params_obj);
  const AssistantInput input =
      buildAssistantInput(left_channels, right_channels, track_ids, track_names, sample_rate);
  const auto result = assistant::suggest_scene(input.tracks, config);
  return mixing::api::scene_to_json(result.scene);
#else
  (void)left_channels;
  (void)right_channels;
  (void)track_ids;
  (void)track_names;
  (void)sample_rate;
  (void)params_obj;
  throwUnavailable();
#endif
}

// Source-class identifiers the assistant can report, in enum order.
val js_mixing_assistant_source_class_names() {
  val out = val::array();
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  for (const std::string& name : assistant::source_class_names()) {
    out.call<void>("push", name);
  }
#endif
  return out;
}

// Resolves a source-class identifier to its index in the names list, or -1 when
// unknown. Deliberately index-based rather than the core's enum lookup, which
// folds an unknown name into Unknown (0) and so cannot report a miss.
int js_mixing_assistant_source_class_from_name(std::string name) {
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  const std::vector<std::string> names = assistant::source_class_names();
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (names[index] == name) return static_cast<int>(index);
  }
#else
  (void)name;
#endif
  return -1;
}

void registerMixingAssistantBindings() {
  function("mixingAssistantSuggest", &js_mixing_assistant_suggest);
  function("mixingAssistantSuggestSceneJson", &js_mixing_assistant_suggest_scene_json);
  function("mixingAssistantSourceClassNames", &js_mixing_assistant_source_class_names);
  function("mixingAssistantSourceClassFromName", &js_mixing_assistant_source_class_from_name);
}

#endif  // __EMSCRIPTEN__
