#include <sonare/sonare_c_vocal_project.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "project/common.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::project;

namespace {

constexpr double kMaxSafeInteger = 9007199254740991.0;
constexpr uint32_t kMaxProjectId = std::numeric_limits<uint32_t>::max() - 1u;
constexpr uint64_t kMaxResultItems = 1ull << 28;

bool ReadProjectId(Napi::Env env, const Napi::Value& value, const char* field, bool allow_zero,
                   uint32_t* out) {
  if (!RequiredUint32Value(env, value, field, out)) return false;
  if (*out > kMaxProjectId || (!allow_zero && *out == 0)) {
    Napi::RangeError::New(env, std::string(field) + " must be within the project id range")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

bool ReadDigest(Napi::Env env, const Napi::Value& value, const char* field, uint8_t* out) {
  std::string text;
  if (!RequiredStringValue(env, value, field, &text)) return false;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  bool valid = text.size() == 64;
  for (size_t index = 0; valid && index < 32; ++index) {
    const int high = nibble(text[index * 2]);
    const int low = nibble(text[index * 2 + 1]);
    valid = high >= 0 && low >= 0;
    if (valid) out[index] = static_cast<uint8_t>((high << 4) | low);
  }
  if (!valid) {
    Napi::RangeError::New(env, std::string(field) + " must be 64 hexadecimal characters")
        .ThrowAsJavaScriptException();
  }
  return valid;
}

std::string DigestString(const uint8_t* value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(64, '0');
  for (size_t index = 0; index < 32; ++index) {
    result[index * 2] = kHex[value[index] >> 4];
    result[index * 2 + 1] = kHex[value[index] & 0x0f];
  }
  return result;
}

std::string FixedText(const char* value, size_t capacity) {
  if (value == nullptr) return {};
  size_t length = 0;
  while (length < capacity && value[length] != '\0') ++length;
  return std::string(value, length);
}

bool CopyFiniteSamples(Napi::Env env, const Napi::Value& value, const char* field,
                       std::vector<float>* out) {
  Napi::Float32Array array;
  if (!RequiredFloat32ArrayValue(env, value, field, &array)) return false;
  if (array.ElementLength() == 0) {
    Napi::RangeError::New(env, std::string(field) + " must not be empty")
        .ThrowAsJavaScriptException();
    return false;
  }
  out->assign(array.Data(), array.Data() + array.ElementLength());
  for (size_t index = 0; index < out->size(); ++index) {
    if (!std::isfinite((*out)[index])) {
      Napi::RangeError::New(env, std::string(field) + " must contain finite samples")
          .ThrowAsJavaScriptException();
      return false;
    }
  }
  return true;
}

bool CopyBytes(Napi::Env env, const Napi::Value& value, const char* field,
               std::vector<uint8_t>* out) {
  Napi::Uint8Array array;
  if (!RequiredUint8ArrayValue(env, value, field, &array)) return false;
  if (array.ElementLength() == 0) {
    Napi::RangeError::New(env, std::string(field) + " must not be empty")
        .ThrowAsJavaScriptException();
    return false;
  }
  out->assign(array.Data(), array.Data() + array.ElementLength());
  return true;
}

bool ReadToken(Napi::Env env, const Napi::Value& value, const char* field,
               SonareVocalStateToken* out) {
  Napi::Object object;
  if (!RequiredObjectValue(env, value, field, &object)) return false;
  *out = SonareVocalStateToken{};
  if (!RequiredUint64Value(env, object.Get("sessionEpoch"), "renderToken.sessionEpoch",
                           &out->session_epoch) ||
      !RequiredUint64Value(env, object.Get("revision"), "renderToken.revision", &out->revision) ||
      !RequiredUint64Value(env, object.Get("draftId"), "renderToken.draftId", &out->draft_id) ||
      !RequiredUint64Value(env, object.Get("generation"), "renderToken.generation",
                           &out->generation) ||
      !RequiredUint64Value(env, object.Get("requestId"), "renderToken.requestId",
                           &out->request_id) ||
      !RequiredUint32Value(env, object.Get("profileId"), "renderToken.profileId",
                           &out->profile_id)) {
    return false;
  }
  if (out->draft_id != 0 || out->generation != 0) {
    Napi::RangeError::New(env, "renderToken must be a committed session token")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

struct ApplyStorage {
  SonareProjectVocalEditApplyDesc desc{};
  std::vector<float> rendered;
  std::vector<uint8_t> sve1;
};

bool ReadApplyRequest(Napi::Env env, const Napi::Value& value, ApplyStorage* storage) {
  Napi::Object request;
  if (!RequiredObjectValue(env, value, "request", &request)) return false;
  sonare_project_vocal_edit_apply_desc_init(&storage->desc);
  if (!ReadProjectId(env, request.Get("clipId"), "clipId", false, &storage->desc.clip_id) ||
      !ReadProjectId(env, request.Get("expectedSourceId"), "expectedSourceId", false,
                     &storage->desc.expected_source_id) ||
      !ReadProjectId(env, request.Get("expectedSourceSampleRate"), "expectedSourceSampleRate",
                     false, &storage->desc.expected_source_sample_rate)) {
    return false;
  }
  storage->desc.take_id = Uint32Property(request, "takeId", 0);
  if (storage->desc.take_id > kMaxProjectId) {
    Napi::RangeError::New(env, "takeId must be within the project id range")
        .ThrowAsJavaScriptException();
    return false;
  }
  if (!RequiredSafeIntegerValue(env, request.Get("expectedSourceSampleCount"),
                                "expectedSourceSampleCount",
                                &storage->desc.expected_source_sample_count, 1)) {
    return false;
  }
  if (!ReadDigest(env, request.Get("expectedSourceSha256"), "expectedSourceSha256",
                  storage->desc.expected_source_sha256) ||
      !RequiredFiniteDoubleValue(env, request.Get("expectedClipLengthPpq"), "expectedClipLengthPpq",
                                 &storage->desc.expected_clip_length_ppq) ||
      !RequiredFiniteDoubleValue(env, request.Get("expectedSourceOffsetPpq"),
                                 "expectedSourceOffsetPpq",
                                 &storage->desc.expected_source_offset_ppq) ||
      !CopyFiniteSamples(env, request.Get("renderedMono"), "renderedMono", &storage->rendered)) {
    return false;
  }
  if (storage->rendered.size() != static_cast<size_t>(storage->desc.expected_source_sample_count)) {
    Napi::RangeError::New(env, "renderedMono length must equal expectedSourceSampleCount")
        .ThrowAsJavaScriptException();
    return false;
  }
  if (!ReadProjectId(env, request.Get("renderedSampleRate"), "renderedSampleRate", false,
                     &storage->desc.rendered_sample_rate)) {
    return false;
  }
  if (storage->desc.rendered_sample_rate != storage->desc.expected_source_sample_rate) {
    Napi::RangeError::New(env, "renderedSampleRate must equal expectedSourceSampleRate")
        .ThrowAsJavaScriptException();
    return false;
  }
  const Napi::Value rendered_start = request.Get("renderedStartSample");
  if (rendered_start.IsUndefined() || rendered_start.IsNull()) {
    storage->desc.rendered_start_sample = 0;
  } else if (!RequiredSafeIntegerValue(env, rendered_start, "renderedStartSample",
                                       &storage->desc.rendered_start_sample)) {
    return false;
  }
  if (storage->desc.rendered_start_sample != 0) {
    Napi::RangeError::New(env, "renderedStartSample must be zero for the v1 Project vocal API")
        .ThrowAsJavaScriptException();
    return false;
  }
  if (!ReadToken(env, request.Get("renderToken"), "renderToken", &storage->desc.render_token) ||
      !CopyBytes(env, request.Get("sve1"), "sve1", &storage->sve1)) {
    return false;
  }
  storage->desc.rendered_mono = storage->rendered.data();
  storage->desc.rendered_sample_count = static_cast<int64_t>(storage->rendered.size());
  storage->desc.sve1 = storage->sve1.data();
  storage->desc.sve1_size = storage->sve1.size();
  return true;
}

struct DependenciesGuard {
  SonareProjectVocalEditDependenciesResult value{};
  DependenciesGuard() { sonare_project_vocal_edit_dependencies_result_init(&value); }
  ~DependenciesGuard() { sonare_project_free_vocal_edit_dependencies(&value); }
};

struct RehydrateGuard {
  SonareProjectVocalRehydrateResult value{};
  RehydrateGuard() { sonare_project_vocal_rehydrate_result_init(&value); }
  ~RehydrateGuard() { sonare_project_free_vocal_rehydrate_result(&value); }
};

struct OriginalStorage {
  std::vector<SonareProjectVocalOriginalSource> rows;
  std::vector<std::vector<float>> buffers;
};

bool ReadOriginals(Napi::Env env, const Napi::Value& value, OriginalStorage* storage) {
  Napi::Array originals;
  if (!RequiredArrayValue(env, value, "originals", &originals)) return false;
  storage->rows.resize(originals.Length());
  storage->buffers.reserve(originals.Length());
  std::vector<uint32_t> ids;
  ids.reserve(originals.Length());
  for (uint32_t index = 0; index < originals.Length(); ++index) {
    Napi::Object source;
    if (!RequiredObjectValue(env, originals.Get(index), "originals entry", &source)) return false;
    auto& row = storage->rows[index];
    sonare_project_vocal_original_source_init(&row);
    if (!ReadProjectId(env, source.Get("sourceId"), "sourceId", false, &row.source_id) ||
        !ReadProjectId(env, source.Get("sampleRate"), "sampleRate", false, &row.sample_rate)) {
      return false;
    }
    if (std::find(ids.begin(), ids.end(), row.source_id) != ids.end()) {
      Napi::RangeError::New(env, "originals contains duplicate sourceId")
          .ThrowAsJavaScriptException();
      return false;
    }
    ids.push_back(row.source_id);
    storage->buffers.emplace_back();
    if (!CopyFiniteSamples(env, source.Get("mono"), "original.mono", &storage->buffers.back())) {
      return false;
    }
    row.mono = storage->buffers.back().data();
    row.sample_count = static_cast<int64_t>(storage->buffers.back().size());
  }
  return true;
}

struct CancelContext {
  Napi::Env env;
  std::optional<Napi::Function> callback;
};

int VocalProjectCancel(void* user_data) {
  auto* context = static_cast<CancelContext*>(user_data);
  if (context == nullptr || !context->callback.has_value() || context->env.IsExceptionPending()) {
    return 1;
  }
  const Napi::Value value = context->callback->Call({});
  if (context->env.IsExceptionPending()) return 1;
  return value.IsBoolean() && value.As<Napi::Boolean>().Value() ? 1 : 0;
}

bool ResultCountValid(Napi::Env env, uint64_t count, const char* field) {
  if (count > kMaxResultItems || count > std::numeric_limits<uint32_t>::max()) {
    Napi::RangeError::New(env, std::string(field) + " count is unreasonable")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

bool ResultPointerValid(Napi::Env env, const void* pointer, uint64_t count, const char* field) {
  if (count != 0 && pointer == nullptr) {
    Napi::Error::New(env, std::string(field) + " result contains a null array")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

Napi::Object ApplyResultToObject(Napi::Env env, const SonareProjectVocalEditApplyResult& result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("clipId", result.clip_id);
  out.Set("takeId", result.take_id);
  out.Set("originalSourceId", result.original_source_id);
  out.Set("derivedSourceId", result.derived_source_id);
  out.Set("committedRevision", std::to_string(result.committed_revision));
  out.Set("profileId", result.profile_id);
  out.Set("derivedSourceSha256", DigestString(result.derived_source_sha256));
  out.Set("sidecarKey", FixedText(result.sidecar_key, SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY));
  return out;
}

Napi::Object DependencyToObject(Napi::Env env, const SonareProjectVocalEditDependency& dependency) {
  if (dependency.source_sample_count < 0 ||
      static_cast<double>(dependency.source_sample_count) > kMaxSafeInteger) {
    Napi::RangeError::New(env, "dependency.sourceSampleCount is outside the safe integer range")
        .ThrowAsJavaScriptException();
    return Napi::Object::New(env);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("clipId", dependency.clip_id);
  out.Set("takeId", dependency.take_id);
  out.Set("originalSourceId", dependency.original_source_id);
  out.Set("derivedSourceId", dependency.derived_source_id);
  out.Set("sourceSampleRate", dependency.source_sample_rate);
  out.Set("profileId", dependency.profile_id);
  out.Set("sourceSampleCount", static_cast<double>(dependency.source_sample_count));
  out.Set("committedRevision", std::to_string(dependency.committed_revision));
  out.Set("originalSourceSha256", DigestString(dependency.original_source_sha256));
  out.Set("derivedSourceSha256", DigestString(dependency.derived_source_sha256));
  out.Set("originalPcmAvailable", dependency.original_pcm_available != 0);
  out.Set("derivedPcmAvailable", dependency.derived_pcm_available != 0);
  out.Set("reason", dependency.reason);
  out.Set("sidecarKey",
          FixedText(dependency.sidecar_key, SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY));
  return out;
}

Napi::Object RehydrateItemToObject(Napi::Env env, const SonareProjectVocalRehydrateItem& item) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("clipId", item.clip_id);
  out.Set("takeId", item.take_id);
  out.Set("derivedSourceId", item.derived_source_id);
  out.Set("status", item.status);
  out.Set("reason", item.reason);
  return out;
}

}  // namespace

Napi::Value ProjectWrap::ApplyVocalEdit(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (project_ == nullptr) {
    Napi::Error::New(env, "Project is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() != 1) {
    Napi::TypeError::New(env, "applyVocalEdit expects one request object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ApplyStorage storage;
  if (!ReadApplyRequest(env, info[0], &storage)) return env.Undefined();
  SonareProjectVocalEditApplyResult result{};
  sonare_project_vocal_edit_apply_result_init(&result);
  const SonareError error = sonare_project_apply_vocal_edit(project_, &storage.desc, &result);
  ThrowIfError(env, error);
  if (env.IsExceptionPending()) return env.Undefined();
  return ApplyResultToObject(env, result);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::GetVocalEditDependencies(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (project_ == nullptr) {
    Napi::Error::New(env, "Project is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() != 0) {
    Napi::TypeError::New(env, "getVocalEditDependencies expects no arguments")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  DependenciesGuard result;
  const SonareError error = sonare_project_get_vocal_edit_dependencies(project_, &result.value);
  ThrowIfError(env, error);
  if (env.IsExceptionPending()) return env.Undefined();
  if (!ResultCountValid(env, result.value.dependency_count, "dependency") ||
      !ResultPointerValid(env, result.value.dependencies, result.value.dependency_count,
                          "dependency")) {
    return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env, static_cast<uint32_t>(result.value.dependency_count));
  for (uint64_t index = 0; index < result.value.dependency_count; ++index) {
    out.Set(static_cast<uint32_t>(index),
            DependencyToObject(env, result.value.dependencies[index]));
    if (env.IsExceptionPending()) return env.Undefined();
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::RehydrateVocalEdits(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (project_ == nullptr) {
    Napi::Error::New(env, "Project is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || info.Length() > 2) {
    Napi::TypeError::New(env,
                         "rehydrateVocalEdits expects originals and an optional cancel callback")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  OriginalStorage storage;
  if (!ReadOriginals(env, info[0], &storage)) return env.Undefined();
  bool has_cancel = false;
  if (info.Length() == 2 && !info[1].IsUndefined() && !info[1].IsNull()) {
    if (!info[1].IsFunction()) {
      Napi::TypeError::New(env, "cancel must be a function").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    has_cancel = true;
  }
  std::optional<Napi::Function> callback;
  if (has_cancel) callback.emplace(info[1].As<Napi::Function>());
  CancelContext cancel{env, std::move(callback)};
  auto busy_call = BeginBusyCall();
  RehydrateGuard result;
  const SonareError error = sonare_project_rehydrate_vocal_edits(
      project_, storage.rows.empty() ? nullptr : storage.rows.data(), storage.rows.size(),
      has_cancel ? &VocalProjectCancel : nullptr, has_cancel ? &cancel : nullptr, &result.value);
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfError(env, error);
  if (env.IsExceptionPending()) return env.Undefined();
  if (!ResultCountValid(env, result.value.item_count, "rehydrate") ||
      !ResultPointerValid(env, result.value.items, result.value.item_count, "rehydrate")) {
    return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env, static_cast<uint32_t>(result.value.item_count));
  for (uint64_t index = 0; index < result.value.item_count; ++index) {
    out.Set(static_cast<uint32_t>(index), RehydrateItemToObject(env, result.value.items[index]));
    if (env.IsExceptionPending()) return env.Undefined();
  }
  return out;
  SONARE_NODE_CATCH(env)
}
