/// @file project_vocal_edit.cpp
/// @brief Embind adapter for applying and rehydrating vocal Project sidecars.

#ifdef __EMSCRIPTEN__

#include <emscripten/emscripten.h>
#include <emscripten/val.h>
#include <sonare/sonare_c_vocal_project.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

// clang-format off
EM_JS(EM_VAL, sonare_project_invoke_cancel, (EM_VAL callback), {
  try {
    const value = Emval.toValue(callback)();
    return Emval.toHandle({
      failed: false,
      cancelled: typeof value === 'boolean' && value,
    });
  } catch (error) {
    return Emval.toHandle({ failed: true, cancelled: true, error });
  }
});
// clang-format on

namespace {

using emscripten::val;

bool absent(const val& value) { return value.isUndefined() || value.isNull(); }

val property(const val& object, const char* key) { return object[key]; }

[[noreturn]] void invalid(const std::string& message) {
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, message);
}

uint32_t projectId(const val& value, const char* field, bool allow_zero = true) {
  const uint32_t result = checkedUintFromVal(value, field);
  if (!allow_zero && result == 0) invalid(std::string(field) + " must be non-zero");
  if (result == std::numeric_limits<uint32_t>::max()) {
    invalid(std::string(field) + " must be less than UINT32_MAX");
  }
  return result;
}

uint32_t positiveUint32(const val& value, const char* field) {
  const uint32_t result = checkedUintFromVal(value, field);
  if (result == 0) invalid(std::string(field) + " must be non-zero");
  return result;
}

uint32_t optionalProjectId(const val& object, const char* field, uint32_t fallback,
                           bool allow_zero = true) {
  const val value = property(object, field);
  return absent(value) ? fallback : projectId(value, field, allow_zero);
}

uint64_t decimalUint64(const val& value, const char* field) {
  if (value.typeOf().as<std::string>() != "string") {
    invalid(std::string(field) + " must be a decimal uint64 string");
  }
  const std::string text = value.as<std::string>();
  if (text.empty()) invalid(std::string(field) + " must be a decimal uint64 string");
  uint64_t result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    invalid(std::string(field) + " must be a decimal uint64 string");
  }
  return result;
}

std::string decimalString(uint64_t value) { return std::to_string(value); }

std::array<uint8_t, 32> hexSha256(const val& value, const char* field) {
  if (value.typeOf().as<std::string>() != "string") {
    invalid(std::string(field) + " must be a 64-character hexadecimal string");
  }
  const std::string text = value.as<std::string>();
  if (text.size() != 64) {
    invalid(std::string(field) + " must be a 64-character hexadecimal string");
  }
  std::array<uint8_t, 32> result{};
  for (size_t i = 0; i < result.size(); ++i) {
    const auto nibble = [](char character) -> int {
      if (character >= '0' && character <= '9') return character - '0';
      if (character >= 'a' && character <= 'f') return character - 'a' + 10;
      if (character >= 'A' && character <= 'F') return character - 'A' + 10;
      return -1;
    };
    const int high = nibble(text[i * 2]);
    const int low = nibble(text[i * 2 + 1]);
    if (high < 0 || low < 0) {
      invalid(std::string(field) + " must be a 64-character hexadecimal string");
    }
    result[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return result;
}

std::string hexSha256(const uint8_t* value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    result[i * 2] = kHex[value[i] >> 4];
    result[i * 2 + 1] = kHex[value[i] & 0x0f];
  }
  return result;
}

std::string fixedText(const char* value, size_t capacity) {
  if (value == nullptr) return {};
  size_t length = 0;
  while (length < capacity && value[length] != '\0') ++length;
  return std::string(value, length);
}

std::vector<float> copyMono(const val& value, const char* field) {
  const size_t count = wasmFloat32ArrayLength(value, field);
  if (count == 0) invalid(std::string(field) + " must not be empty");
  std::vector<float> result = float32ArrayToVector(value);
  for (float sample : result) {
    if (!std::isfinite(sample)) invalid(std::string(field) + " must contain finite samples");
  }
  return result;
}

std::vector<uint8_t> copySVE1(const val& value) {
  const size_t count = wasmArrayLikeLength(value, "sve1", "byteLength");
  if (count == 0) invalid("sve1 must not be empty");
  return uint8ArrayToVector(value);
}

struct TokenStorage {
  SonareVocalStateToken token{};
};

SonareVocalStateToken tokenFromVal(const val& value) {
  if (value.isNull() || value.isUndefined() || value.typeOf().as<std::string>() != "object") {
    invalid("renderToken must be an object");
  }
  SonareVocalStateToken result{};
  result.session_epoch = decimalUint64(property(value, "sessionEpoch"), "renderToken.sessionEpoch");
  result.revision = decimalUint64(property(value, "revision"), "renderToken.revision");
  result.draft_id = decimalUint64(property(value, "draftId"), "renderToken.draftId");
  result.generation = decimalUint64(property(value, "generation"), "renderToken.generation");
  result.request_id = decimalUint64(property(value, "requestId"), "renderToken.requestId");
  result.profile_id = checkedUintFromVal(property(value, "profileId"), "renderToken.profileId");
  if (result.draft_id != 0 || result.generation != 0) {
    invalid("renderToken must be a committed session token");
  }
  return result;
}

struct ApplyStorage {
  SonareProjectVocalEditApplyDesc desc{};
  std::vector<float> rendered;
  std::vector<uint8_t> state;
  std::array<uint8_t, 32> source_sha{};
};

SonareProjectVocalEditApplyDesc applyDescFromVal(const val& request, ApplyStorage* storage) {
  if (request.isNull() || request.isUndefined() || request.typeOf().as<std::string>() != "object") {
    invalid("request must be an object");
  }
  sonare_project_vocal_edit_apply_desc_init(&storage->desc);
  storage->desc.clip_id = projectId(property(request, "clipId"), "clipId", false);
  storage->desc.take_id = optionalProjectId(request, "takeId", 0);
  storage->desc.expected_source_id =
      projectId(property(request, "expectedSourceId"), "expectedSourceId", false);
  storage->desc.expected_source_sample_rate =
      positiveUint32(property(request, "expectedSourceSampleRate"), "expectedSourceSampleRate");
  storage->desc.expected_source_sample_count = checkedInt64FromVal(
      property(request, "expectedSourceSampleCount"), "expectedSourceSampleCount");
  if (storage->desc.expected_source_sample_count <= 0) {
    invalid("expectedSourceSampleCount must be positive");
  }
  if (static_cast<uint64_t>(storage->desc.expected_source_sample_count) >
      std::numeric_limits<size_t>::max()) {
    invalid("expectedSourceSampleCount is too large for this WASM build");
  }
  storage->source_sha =
      hexSha256(property(request, "expectedSourceSha256"), "expectedSourceSha256");
  std::copy(storage->source_sha.begin(), storage->source_sha.end(),
            storage->desc.expected_source_sha256);
  storage->desc.expected_clip_length_ppq =
      checkedDoubleFromVal(property(request, "expectedClipLengthPpq"), "expectedClipLengthPpq");
  storage->desc.expected_source_offset_ppq =
      checkedDoubleFromVal(property(request, "expectedSourceOffsetPpq"), "expectedSourceOffsetPpq");
  storage->rendered = copyMono(property(request, "renderedMono"), "renderedMono");
  if (storage->rendered.size() != static_cast<size_t>(storage->desc.expected_source_sample_count)) {
    invalid("renderedMono length must equal expectedSourceSampleCount");
  }
  storage->desc.rendered_mono = storage->rendered.data();
  storage->desc.rendered_sample_count = static_cast<int64_t>(storage->rendered.size());
  storage->desc.rendered_sample_rate =
      positiveUint32(property(request, "renderedSampleRate"), "renderedSampleRate");
  const val rendered_start = property(request, "renderedStartSample");
  storage->desc.rendered_start_sample =
      absent(rendered_start) ? 0 : checkedInt64FromVal(rendered_start, "renderedStartSample");
  if (storage->desc.rendered_start_sample != 0) {
    invalid("renderedStartSample must be zero for the v1 Project vocal API");
  }
  storage->desc.render_token = tokenFromVal(property(request, "renderToken"));
  storage->state = copySVE1(property(request, "sve1"));
  storage->desc.sve1 = storage->state.data();
  storage->desc.sve1_size = storage->state.size();
  return storage->desc;
}

val applyResultToVal(const SonareProjectVocalEditApplyResult& result) {
  val out = val::object();
  out.set("clipId", result.clip_id);
  out.set("takeId", result.take_id);
  out.set("originalSourceId", result.original_source_id);
  out.set("derivedSourceId", result.derived_source_id);
  out.set("committedRevision", decimalString(result.committed_revision));
  out.set("profileId", result.profile_id);
  out.set("derivedSourceSha256", hexSha256(result.derived_source_sha256));
  out.set("sidecarKey", fixedText(result.sidecar_key, SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY));
  return out;
}

val dependencyToVal(const SonareProjectVocalEditDependency& dependency) {
  val out = val::object();
  out.set("clipId", dependency.clip_id);
  out.set("takeId", dependency.take_id);
  out.set("originalSourceId", dependency.original_source_id);
  out.set("derivedSourceId", dependency.derived_source_id);
  out.set("sourceSampleRate", dependency.source_sample_rate);
  out.set("profileId", dependency.profile_id);
  out.set("sourceSampleCount", static_cast<double>(dependency.source_sample_count));
  out.set("committedRevision", decimalString(dependency.committed_revision));
  out.set("originalSourceSha256", hexSha256(dependency.original_source_sha256));
  out.set("derivedSourceSha256", hexSha256(dependency.derived_source_sha256));
  out.set("originalPcmAvailable", dependency.original_pcm_available != 0);
  out.set("derivedPcmAvailable", dependency.derived_pcm_available != 0);
  out.set("reason", dependency.reason);
  out.set("sidecarKey",
          fixedText(dependency.sidecar_key, SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY));
  return out;
}

val dependenciesToVal(const SonareProjectVocalEditDependenciesResult& result) {
  if (result.dependency_count > 0 && result.dependencies == nullptr) {
    invalid("native vocal dependency result has a null array");
  }
  if (result.dependency_count > std::numeric_limits<unsigned>::max()) {
    invalid("native vocal dependency result is too large");
  }
  val out = val::array();
  for (uint64_t index = 0; index < result.dependency_count; ++index) {
    out.set(static_cast<unsigned>(index), dependencyToVal(result.dependencies[index]));
  }
  return out;
}

val rehydrateItemToVal(const SonareProjectVocalRehydrateItem& item) {
  val out = val::object();
  out.set("clipId", item.clip_id);
  out.set("takeId", item.take_id);
  out.set("derivedSourceId", item.derived_source_id);
  out.set("status", item.status);
  out.set("reason", item.reason);
  return out;
}

val rehydrateItemsToVal(const SonareProjectVocalRehydrateResult& result) {
  if (result.item_count > 0 && result.items == nullptr) {
    invalid("native vocal rehydrate result has a null array");
  }
  if (result.item_count > std::numeric_limits<unsigned>::max()) {
    invalid("native vocal rehydrate result is too large");
  }
  val out = val::array();
  for (uint64_t index = 0; index < result.item_count; ++index) {
    out.set(static_cast<unsigned>(index), rehydrateItemToVal(result.items[index]));
  }
  return out;
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

OriginalStorage originalsFromVal(const val& originals) {
  if (!val::global("Array").call<bool>("isArray", originals)) {
    invalid("originals must be an array");
  }
  const size_t count = wasmArrayLikeLength(originals, "originals");
  OriginalStorage storage;
  storage.rows.resize(count);
  storage.buffers.reserve(count);
  std::vector<uint32_t> source_ids;
  source_ids.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const val source = originals[static_cast<unsigned>(index)];
    if (source.isNull() || source.isUndefined() || source.typeOf().as<std::string>() != "object") {
      invalid("originals entries must be objects");
    }
    auto& row = storage.rows[index];
    sonare_project_vocal_original_source_init(&row);
    row.source_id = projectId(property(source, "sourceId"), "sourceId", false);
    if (std::find(source_ids.begin(), source_ids.end(), row.source_id) != source_ids.end()) {
      invalid("originals contains duplicate sourceId");
    }
    source_ids.push_back(row.source_id);
    row.sample_rate = positiveUint32(property(source, "sampleRate"), "sampleRate");
    storage.buffers.push_back(copyMono(property(source, "mono"), "original.mono"));
    row.mono = storage.buffers.back().data();
    row.sample_count = static_cast<int64_t>(storage.buffers.back().size());
  }
  return storage;
}

struct CancelStorage {
  val callback;
  bool callback_failed = false;
  val callback_failure;
};

int vocalProjectCancel(void* user_data) {
  auto* storage = static_cast<CancelStorage*>(user_data);
  if (storage == nullptr) return 0;
  const val outcome =
      val::take_ownership(sonare_project_invoke_cancel(storage->callback.as_handle()));
  if (typedBoolProperty(outcome, "failed", false)) {
    // The JS helper catches the exception because C++ cannot catch a JS throw
    // escaping emscripten::val::operator(). Keep the original value so it can
    // be returned once the C ABI has unwound, while reporting cancellation.
    storage->callback_failed = true;
    storage->callback_failure = outcome["error"];
    return 1;
  }
  return typedBoolProperty(outcome, "cancelled", false) ? 1 : 0;
}

}  // namespace

val ProjectWasm::applyVocalEdit(val request) {
  ApplyStorage storage;
  const SonareProjectVocalEditApplyDesc desc = applyDescFromVal(request, &storage);
  SonareProjectVocalEditApplyResult result{};
  sonare_project_vocal_edit_apply_result_init(&result);
  const SonareError error = sonare_project_apply_vocal_edit(project_.get(), &desc, &result);
  if (error != SONARE_OK) throwCError(error, "failed to apply vocal edit");
  return applyResultToVal(result);
}

val ProjectWasm::getVocalEditDependencies() {
  DependenciesGuard result;
  const SonareError error =
      sonare_project_get_vocal_edit_dependencies(project_.get(), &result.value);
  if (error != SONARE_OK) throwCError(error, "failed to enumerate vocal edit dependencies");
  return dependenciesToVal(result.value);
}

val ProjectWasm::rehydrateVocalEdits(val originals, val cancel) {
  // A raw embind `native.delete()` may run from the JS cancellation callback,
  // after the C API has entered this method. Keep the opaque project alive
  // until every C call and result conversion has finished; accessing only the
  // local copy below also avoids touching the deleted wrapper's member.
  const auto project_keepalive = project_;
  OriginalStorage storage = originalsFromVal(originals);
  if (!absent(cancel) && cancel.typeOf().as<std::string>() != "function") {
    invalid("cancel must be a function");
  }
  CancelStorage cancel_storage{cancel, false, val::undefined()};
  RehydrateGuard result;
  const SonareError error = sonare_project_rehydrate_vocal_edits(
      project_keepalive.get(), storage.rows.empty() ? nullptr : storage.rows.data(),
      storage.rows.size(), absent(cancel) ? nullptr : &vocalProjectCancel,
      absent(cancel) ? nullptr : &cancel_storage, &result.value);
  // A JS throw from here would skip every destructor in this frame (the
  // keepalive, the buffers, the result guard), so a throwing callback is
  // returned as data and the TypeScript facade rethrows it.
  if (cancel_storage.callback_failed) {
    val failure = val::object();
    failure.set("cancelFailure", cancel_storage.callback_failure);
    return failure;
  }
  if (error != SONARE_OK) throwCError(error, "failed to rehydrate vocal edits");
  return rehydrateItemsToVal(result.value);
}

void registerProjectVocalEdit(class_<ProjectWasm>& cls) {
  cls.function("applyVocalEdit", &ProjectWasm::applyVocalEdit)
      .function("getVocalEditDependencies", &ProjectWasm::getVocalEditDependencies)
      .function("rehydrateVocalEdits", &ProjectWasm::rehydrateVocalEdits);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
