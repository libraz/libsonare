#include <cstddef>
#include <string>
#include <vector>

#include "mastering/api/named_processor.h"
#include "sonare_wrap.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

// Splits a '\n'-joined, caller-borrowed C string (program- or thread-lifetime,
// never freed) into a JS string[]. An empty string yields an empty array.
Napi::Array JoinedNamesToArray(Napi::Env env, const char* joined) {
  Napi::Array out = Napi::Array::New(env);
  if (joined == nullptr || joined[0] == '\0') {
    return out;
  }
  const std::string names(joined);
  uint32_t index = 0;
  size_t start = 0;
  while (start <= names.size()) {
    const size_t end = names.find('\n', start);
    if (end == std::string::npos) {
      out.Set(index++, Napi::String::New(env, names.substr(start)));
      break;
    }
    out.Set(index++, Napi::String::New(env, names.substr(start, end - start)));
    start = end + 1;
  }
  return out;
}

// Planar per-track input marshalled from the JS arrays, in the parallel-array
// shape the assistant entry points take. The Float32Array handles and the id /
// name strings are held here for the duration of the native call because the
// pointer arrays alias their storage.
struct TrackArrays {
  std::vector<Napi::Float32Array> left_handles;
  std::vector<Napi::Float32Array> right_handles;
  std::vector<std::string> ids;
  std::vector<std::string> names;
  std::vector<const float*> left;
  std::vector<const float*> right;
  std::vector<const char*> id_pointers;
  std::vector<const char*> name_pointers;
  std::vector<size_t> lengths;
  // Tracks may differ in length, so the count is per track rather than shared.
  std::vector<bool> has_name;
  bool any_right = false;
  bool any_name = false;
};

// Reads the five leading positional arguments into @p out. Returns false with a
// pending JS exception on the first bad argument, so the caller must bail out
// before touching native state (a second throw on a pending exception aborts).
bool ReadTrackArrays(Napi::Env env, const Napi::CallbackInfo& info, TrackArrays* out) {
  if (info.Length() < 5 || !info[0].IsArray() || !info[2].IsArray() || !info[4].IsNumber()) {
    Napi::TypeError::New(
        env, "Expected (leftChannels, rightChannels, trackIds, trackNames, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return false;
  }

  Napi::Array left_input = info[0].As<Napi::Array>();
  Napi::Array id_input = info[2].As<Napi::Array>();
  const uint32_t count = left_input.Length();
  if (id_input.Length() != count) {
    Napi::TypeError::New(env, "trackIds must have one entry per track")
        .ThrowAsJavaScriptException();
    return false;
  }

  const bool has_right_array = info[1].IsArray();
  Napi::Array right_input = has_right_array ? info[1].As<Napi::Array>() : Napi::Array::New(env, 0);
  if (has_right_array && right_input.Length() != count) {
    Napi::TypeError::New(env, "rightChannels must have one entry per track")
        .ThrowAsJavaScriptException();
    return false;
  }
  const bool has_name_array = info[3].IsArray();
  Napi::Array name_input = has_name_array ? info[3].As<Napi::Array>() : Napi::Array::New(env, 0);
  if (has_name_array && name_input.Length() != count) {
    Napi::TypeError::New(env, "trackNames must have one entry per track")
        .ThrowAsJavaScriptException();
    return false;
  }

  out->left_handles.reserve(count);
  out->right_handles.resize(count);
  out->ids.reserve(count);
  out->names.resize(count);
  out->has_name.assign(count, false);
  out->lengths.reserve(count);

  for (uint32_t index = 0; index < count; ++index) {
    Napi::Value left_value = left_input.Get(index);
    if (!IsFloat32Array(left_value)) {
      Napi::TypeError::New(env, "every track's left channel must be a Float32Array")
          .ThrowAsJavaScriptException();
      return false;
    }
    out->left_handles.push_back(left_value.As<Napi::Float32Array>());
    out->lengths.push_back(out->left_handles.back().ElementLength());

    Napi::Value id_value = id_input.Get(index);
    if (!id_value.IsString()) {
      Napi::TypeError::New(env, "every trackIds entry must be a string")
          .ThrowAsJavaScriptException();
      return false;
    }
    out->ids.push_back(id_value.As<Napi::String>().Utf8Value());

    if (has_right_array) {
      Napi::Value right_value = right_input.Get(index);
      if (IsFloat32Array(right_value)) {
        Napi::Float32Array right = right_value.As<Napi::Float32Array>();
        if (right.ElementLength() != out->lengths.back()) {
          Napi::TypeError::New(env, "a track's left and right channel lengths must match")
              .ThrowAsJavaScriptException();
          return false;
        }
        out->right_handles[index] = right;
        out->any_right = true;
      } else if (!right_value.IsUndefined() && !right_value.IsNull()) {
        Napi::TypeError::New(env, "every rightChannels entry must be a Float32Array or null")
            .ThrowAsJavaScriptException();
        return false;
      }
    }

    if (has_name_array) {
      Napi::Value name_value = name_input.Get(index);
      if (name_value.IsString()) {
        out->names[index] = name_value.As<Napi::String>().Utf8Value();
        out->has_name[index] = true;
        out->any_name = true;
      } else if (!name_value.IsUndefined() && !name_value.IsNull()) {
        Napi::TypeError::New(env, "every trackNames entry must be a string or null")
            .ThrowAsJavaScriptException();
        return false;
      }
    }
  }

  // Pointer arrays are filled only once every string and handle is in place, so
  // no vector reallocation can invalidate a pointer already taken.
  out->left.reserve(count);
  out->right.reserve(count);
  out->id_pointers.reserve(count);
  out->name_pointers.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    out->left.push_back(out->left_handles[index].Data());
    out->right.push_back(out->right_handles[index].IsEmpty() ? nullptr
                                                             : out->right_handles[index].Data());
    out->id_pointers.push_back(out->ids[index].c_str());
    out->name_pointers.push_back(out->has_name[index] ? out->names[index].c_str() : nullptr);
  }
  return true;
}

using SuggestEntry = SonareError (*)(const float* const*, const float* const*, const char* const*,
                                     const char* const*, const size_t*, size_t, int,
                                     const SonareMasteringParam*, size_t, char**);

// Shared body of the two suggest entry points, so they cannot drift into
// accepting different inputs. @p entry selects the full result document or the
// scene-only document.
Napi::Value SuggestMixScene(const Napi::CallbackInfo& info, SuggestEntry entry) {
  Napi::Env env = info.Env();
  TrackArrays tracks;
  if (!ReadTrackArrays(env, info, &tracks)) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 6 && info[5].IsObject()) {
    params = ParamsFromObject(info[5].As<Napi::Object>());
  }
  if (env.IsExceptionPending()) return env.Undefined();
  std::vector<SonareMasteringParam> c_params;
  c_params.reserve(params.size());
  for (const auto& param : params) {
    c_params.push_back({param.key.c_str(), param.value});
  }

  char* json = nullptr;
  const SonareError err =
      entry(tracks.left.data(), tracks.any_right ? tracks.right.data() : nullptr,
            tracks.id_pointers.data(), tracks.any_name ? tracks.name_pointers.data() : nullptr,
            tracks.lengths.data(), tracks.left.size(), info[4].As<Napi::Number>().Int32Value(),
            c_params.empty() ? nullptr : c_params.data(), c_params.size(), &json);
  if (err != SONARE_OK) {
    sonare_free_string(json);
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  std::string result = json != nullptr ? json : "";
  sonare_free_string(json);
  return Napi::String::New(env, result);
  SONARE_NODE_CATCH(env)
}

}  // namespace

Napi::Value SonareWrap::MixingAssistantSuggest(const Napi::CallbackInfo& info) {
  return SuggestMixScene(info, &sonare_mixing_assistant_suggest);
}

Napi::Value SonareWrap::MixingAssistantSuggestSceneJson(const Napi::CallbackInfo& info) {
  return SuggestMixScene(info, &sonare_mixing_assistant_suggest_scene_json);
}

Napi::Value SonareWrap::MixingAssistantSourceClassNames(const Napi::CallbackInfo& info) {
  // sonare_mixing_assistant_source_class_names() returns a thread-local
  // '\n'-joined const char* (NOT to be freed); split it into a JS string[] like
  // the other *_names getters.
  return JoinedNamesToArray(info.Env(), sonare_mixing_assistant_source_class_names());
}

Napi::Value SonareWrap::MixingAssistantSourceClassFromName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (name: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string name = info[0].As<Napi::String>().Utf8Value();
  // An unknown name resolves to -1 rather than an error; the caller decides
  // whether that is a failure.
  return Napi::Number::New(env, sonare_mixing_assistant_source_class_from_name(name.c_str()));
}
