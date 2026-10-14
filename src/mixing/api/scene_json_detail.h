#pragma once

// Private scene serialization bridge. This header intentionally lives below
// src/ rather than include/ and is not part of the installed API. The project
// serializer embeds the canonical scene Value directly, avoiding a dump/parse
// round trip while keeping scene_json.cpp as the sole writer of scene fields.

#include "mixing/api/scene.h"
#include "util/json.h"

namespace sonare::mixing::api::detail {

#if defined(__GNUC__) || defined(__clang__)
#define SONARE_SCENE_JSON_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define SONARE_SCENE_JSON_DETAIL_HIDDEN
#endif

SONARE_SCENE_JSON_DETAIL_HIDDEN sonare::util::json::Value scene_to_value(const Scene& scene);

}  // namespace sonare::mixing::api::detail

#undef SONARE_SCENE_JSON_DETAIL_HIDDEN
