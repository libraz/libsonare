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
///          (left / right / ids / names / track rates / sample rate / params).
///          The per-track frame counts and the track count that entry point
///          takes as separate arguments are derived from the JS array lengths
///          instead. A track at another rate is resampled onto the sample rate
///          here, as the C layer does, before the core sees it.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
#include "c_api/mixing_input_domains.h"
#include "core/resample.h"
#include "mixing/assistant/config_from_params.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"
#endif

namespace {

#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT

namespace assistant = sonare::mixing::assistant;

bool isJsArray(const val& value) { return val::global("Array").call<bool>("isArray", value); }

bool isAbsent(const val& value) { return value.isUndefined() || value.isNull(); }

// Rejects a non-array where an array of per-track entries is expected. Absent
// optional arrays are the caller's business and are handled at the call site.
void requireJsArray(const val& value, const char* subject) {
  if (!isJsArray(value)) {
    throw WasmTypeError(std::string(subject) + " must be an array");
  }
}

std::string requireTrackId(const val& ids, std::size_t index) {
  const val value = ids[index];
  if (isAbsent(value) || value.typeOf().as<std::string>() != "string") {
    throw WasmTypeError("tracks[" + std::to_string(index) + "].id must be a non-empty string");
  }
  std::string id = value.as<std::string>();
  if (id.empty()) {
    throw WasmTypeError("tracks[" + std::to_string(index) + "].id must be a non-empty string");
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
    throw WasmTypeError("tracks[" + std::to_string(index) + "].name must be a string");
  }
  return value.as<std::string>();
}

// Native input shape captured before any numeric domain is admitted. Keeping the handles and
// names here also means the data copied after admission is the data whose shape was checked.
struct AssistantInputShape {
  std::size_t count = 0;
  std::vector<val> left_planes;
  std::vector<val> right_planes;
  std::vector<bool> has_right;
  std::vector<std::size_t> left_lengths;
  std::vector<std::size_t> right_lengths;
  std::vector<std::string> ids;
  std::vector<std::string> names;
  std::vector<val> track_sample_rates;
  std::vector<bool> has_track_sample_rate;
};

struct AssistantRates {
  int sample_rate = 0;
  std::vector<int> track_sample_rates;
};

struct AssistantInput {
  std::vector<std::vector<float>> left;
  // One entry per track; empty for a mono track (no right plane).
  std::vector<std::vector<float>> right;
  std::vector<assistant::TrackInput> tracks;
};

AssistantInputShape inspectAssistantInput(val left_channels, val right_channels, val track_ids,
                                          val track_names, val track_sample_rates,
                                          const val& sample_rate_value) {
  AssistantInputShape shape;
  shape.count = wasmPlaneListLength(left_channels, "leftChannels");
  requireJsArray(track_ids, "trackIds");
  if (!isAbsent(track_names)) requireJsArray(track_names, "trackNames");
  if (!isAbsent(track_sample_rates)) requireJsArray(track_sample_rates, "trackSampleRates");
  (void)integerNumberFromVal(sample_rate_value, "sampleRate");

  if (wasmArrayLength(track_ids, "trackIds") != shape.count) {
    throw WasmRangeError("trackIds must have one entry per track");
  }
  if (!isAbsent(right_channels) &&
      wasmPlaneListLength(right_channels, "rightChannels") != shape.count) {
    throw WasmRangeError("rightChannels must have one entry per track");
  }
  if (!isAbsent(track_names) && wasmArrayLength(track_names, "trackNames") != shape.count) {
    throw WasmRangeError("trackNames must have one entry per track");
  }
  if (!isAbsent(track_sample_rates) &&
      wasmArrayLength(track_sample_rates, "trackSampleRates") != shape.count) {
    throw WasmRangeError("trackSampleRates must have one entry per track");
  }

  shape.left_planes.reserve(shape.count);
  shape.right_planes.resize(shape.count, val::undefined());
  shape.has_right.assign(shape.count, false);
  shape.left_lengths.reserve(shape.count);
  shape.right_lengths.assign(shape.count, 0);
  shape.ids.reserve(shape.count);
  shape.names.reserve(shape.count);
  std::size_t plain_count = 0;

  // Read every track's channel, identity and name shapes before considering any rate.
  for (std::size_t index = 0; index < shape.count; ++index) {
    shape.ids.push_back(requireTrackId(track_ids, index));
    shape.names.push_back(optionalTrackName(track_names, index));

    const std::string at = "[" + std::to_string(index) + "]";
    const val left = left_channels[index];
    const std::string left_path = "leftChannels" + at;
    const char* const left_subject = wasmCString(left_path, "leftChannels");
    shape.left_lengths.push_back(
        accumulateWasmFloat32ArrayLength(left, left_subject, &plain_count));
    shape.left_planes.push_back(left);

    const val right = isAbsent(right_channels) ? val::undefined() : right_channels[index];
    if (!isAbsent(right)) {
      const std::string right_path = "rightChannels" + at;
      const char* const right_subject = wasmCString(right_path, "rightChannels");
      shape.right_lengths[index] =
          accumulateWasmFloat32ArrayLength(right, right_subject, &plain_count);
      shape.right_planes[index] = right;
      shape.has_right[index] = true;
    }
  }

  // A per-track rate's type is shape; integrality and its range are its domain.
  shape.track_sample_rates.resize(shape.count, val::undefined());
  shape.has_track_sample_rate.assign(shape.count, false);
  if (!isAbsent(track_sample_rates)) {
    for (std::size_t index = 0; index < shape.count; ++index) {
      const val rate = track_sample_rates[index];
      if (!isAbsent(rate) && !rate.isNumber()) {
        throw WasmTypeError("tracks[" + std::to_string(index) + "].sampleRate must be an integer");
      }
      shape.track_sample_rates[index] = rate;
      shape.has_track_sample_rate[index] = rate.isNumber();
    }
  }
  return shape;
}

void validateAssistantTrackLengths(const AssistantInputShape& shape) {
  for (std::size_t index = 0; index < shape.count; ++index) {
    if (shape.has_right[index] && shape.left_lengths[index] != shape.right_lengths[index]) {
      const std::string track = "tracks[" + std::to_string(index) + "]";
      throw WasmRangeError(track + ".left and " + track + ".right must have the same length");
    }
  }
}

AssistantRates validateAssistantRates(const AssistantInputShape& shape,
                                      const val& sample_rate_value) {
  AssistantRates rates;
  rates.track_sample_rates.resize(shape.count);
  for (std::size_t index = 0; index < shape.count; ++index) {
    if (!shape.has_track_sample_rate[index]) continue;
    const std::string subject = "tracks[" + std::to_string(index) + "].sampleRate";
    rates.track_sample_rates[index] =
        domainNumberFromVal<int>(shape.track_sample_rates[index], subject.c_str(),
                                 sonare_c_detail::kMixingAssistantTrackSampleRateDomain);
  }
  rates.sample_rate = domainNumberFromVal<int>(sample_rate_value, "sampleRate",
                                               sonare_c_detail::kMixingAssistantSampleRateDomain);
  for (std::size_t index = 0; index < shape.count; ++index) {
    if (!shape.has_track_sample_rate[index]) {
      rates.track_sample_rates[index] = rates.sample_rate;
    }
  }
  return rates;
}

std::vector<mastering::api::Param> assistantParamsFromObject(val params_obj) {
  if (!isAbsent(params_obj) && params_obj.typeOf().as<std::string>() != "object") {
    throw WasmTypeError("params must be an object");
  }
  return masteringParamsFromObject(params_obj);
}

assistant::MixAssistantConfig assistantConfigFromParams(
    const std::vector<mastering::api::Param>& params) {
  for (const auto& param : params) {
    if (param.key == "tempoBpm" && param.value == 0.0) {
      throw WasmRangeError("tempoBpm must be in [20, 400] BPM");
    }
  }
  return assistant::mix_assistant_config_from_params(params.data(), params.size());
}

AssistantInput buildAssistantInput(const AssistantInputShape& shape, const AssistantRates& rates) {
  AssistantInput input;
  input.left.resize(shape.count);
  input.right.resize(shape.count);
  for (std::size_t index = 0; index < shape.count; ++index) {
    const std::string left_path = "leftChannels[" + std::to_string(index) + "]";
    const char* const left_subject = wasmCString(left_path, "leftChannels");
    input.left[index] = float32ArrayToVector(shape.left_planes[index], left_subject);
    if (shape.has_right[index]) {
      const std::string right_path = "rightChannels[" + std::to_string(index) + "]";
      const char* const right_subject = wasmCString(right_path, "rightChannels");
      input.right[index] = float32ArrayToVector(shape.right_planes[index], right_subject);
    }
    const int track_rate = rates.track_sample_rates[index];
    if (track_rate != rates.sample_rate && !input.left[index].empty()) {
      input.left[index] = sonare::resample(input.left[index].data(), input.left[index].size(),
                                           track_rate, rates.sample_rate);
      if (!input.right[index].empty()) {
        input.right[index] = sonare::resample(input.right[index].data(), input.right[index].size(),
                                              track_rate, rates.sample_rate);
      }
    }
  }

  // Pointers are taken only after every buffer is in place, so no later resize
  // can invalidate one.
  input.tracks.reserve(shape.count);
  for (std::size_t index = 0; index < shape.count; ++index) {
    assistant::TrackInput track;
    track.id = shape.ids[index];
    track.name = shape.names[index];
    track.left = input.left[index].empty() ? nullptr : input.left[index].data();
    track.right = input.right[index].empty() ? nullptr : input.right[index].data();
    track.frame_count = input.left[index].size();
    track.sample_rate = rates.sample_rate;
    input.tracks.push_back(std::move(track));
  }
  return input;
}

#else

// The entry points stay registered when the assistant is compiled out, so the
// JS surface does not change shape with the build configuration; a caller gets
// a runtime answer instead of a missing function.
[[noreturn]] void throwUnavailable() {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "this build does not include the mixing assistant");
}

#endif  // SONARE_WITH_MIXING_ASSISTANT

}  // namespace

// Analyses a set of planar tracks and returns the suggested scene, the
// measurements behind it and the explanation as one JSON document. The
// assistant only suggests: applying the scene is the caller's separate step
// through Mixer.fromSceneJson.
std::string js_mixing_assistant_suggest(val left_channels, val right_channels, val track_ids,
                                        val track_names, val track_sample_rates,
                                        const val& sample_rate, val params_obj) {
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  const AssistantInputShape shape = inspectAssistantInput(
      left_channels, right_channels, track_ids, track_names, track_sample_rates, sample_rate);
  const std::vector<mastering::api::Param> params = assistantParamsFromObject(params_obj);
  validateAssistantTrackLengths(shape);
  const AssistantRates rates = validateAssistantRates(shape, sample_rate);
  const assistant::MixAssistantConfig config = assistantConfigFromParams(params);
  const AssistantInput input = buildAssistantInput(shape, rates);
  return assistant::mix_assistant_result_to_json(assistant::suggest_scene(input.tracks, config));
#else
  (void)left_channels;
  (void)right_channels;
  (void)track_ids;
  (void)track_names;
  (void)track_sample_rates;
  (void)sample_rate;
  (void)params_obj;
  throwUnavailable();
#endif
}

// Source-class identifiers the assistant can report, in enum order.
val js_mixing_assistant_source_class_names() {
#if defined(SONARE_WITH_MIXING_ASSISTANT) && SONARE_WITH_MIXING_ASSISTANT
  return stringVectorToVal(assistant::source_class_names());
#else
  return val::array();
#endif
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
  function("mixingAssistantSourceClassNames", &js_mixing_assistant_source_class_names);
  function("mixingAssistantSourceClassFromName", &js_mixing_assistant_source_class_from_name);
}

#endif  // __EMSCRIPTEN__
