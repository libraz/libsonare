#pragma once

// Private serializer bridge. This header stays below src/ and is not part of
// the installed serializer API. It exposes only the borrowed decode seams used
// by the sibling serializer translation units; the public-facing declarations
// remain in project_serializer_internal.h with their existing ABI.

#include <cstddef>
#include <string>

#include "arrangement/edit_model.h"
#include "util/json.h"

namespace sonare::serialize::detail {

#if defined(__GNUC__) || defined(__clang__)
#define SONARE_PROJECT_SERIALIZER_PRIVATE_HIDDEN __attribute__((visibility("hidden")))
#else
#define SONARE_PROJECT_SERIALIZER_PRIVATE_HIDDEN
#endif

SONARE_PROJECT_SERIALIZER_PRIVATE_HIDDEN const std::string* strict_string_field(
    const sonare::util::json::Value& value, const char* key);

SONARE_PROJECT_SERIALIZER_PRIVATE_HIDDEN void sidecar_fields_from_json(
    const sonare::util::json::Value& value, sonare::arrangement::AssistSidecar* out);

SONARE_PROJECT_SERIALIZER_PRIVATE_HIDDEN bool sidecar_from_borrowed_payload(
    const std::string& encoded_payload, sonare::arrangement::AssistSidecar* out,
    std::size_t max_payload_bytes);

}  // namespace sonare::serialize::detail

#undef SONARE_PROJECT_SERIALIZER_PRIVATE_HIDDEN
