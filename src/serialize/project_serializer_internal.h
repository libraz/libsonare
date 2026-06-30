#pragma once

/// @file project_serializer_internal.h
/// @brief Internal (non-public) declarations shared across the project
///        serializer translation units. NOT a public header: it is not under
///        include/sonare/ and is included by bare/relative quote only from the
///        sibling project_serializer*.cpp TUs that compile into sonare_serialize.
///
/// The encode/decode walker bodies are split across sibling TUs; this header
/// declares the small read helpers and the per-struct walkers once so every TU
/// (including the public-entry driver) sees the same declarations. The helpers
/// have external linkage within sonare::serialize::detail (a static library, no
/// public symbol export) instead of the prior anonymous-namespace internal
/// linkage; behaviour is unchanged.

#include <cstdint>
#include <string>
#include <vector>

#include "arrangement/edit_model.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer.h"
#include "transport/tempo_map.h"
#include "util/json.h"

namespace sonare::serialize {
namespace detail {

namespace json = sonare::util::json;
using json::Array;
using json::Object;
using json::Value;

// ===========================================================================
// Deterministic base64 (for opaque AssistSidecar binary payloads).
// ===========================================================================

std::string base64_encode(const std::vector<uint8_t>& bytes);
uint8_t base64_value(char c);
bool base64_decode(const std::string& text, std::vector<uint8_t>* out);

// ===========================================================================
// Small read helpers (forward-compatible: missing / wrong-typed fields fall
// back to the default).
// ===========================================================================

double num_or(const Value& obj, const char* key, double fallback);
uint32_t uint_or(const Value& obj, const char* key, uint32_t fallback);
bool parse_uint32_key(const std::string& key, uint32_t* out);
std::string str_or(const Value& obj, const char* key, const std::string& fallback);
bool bool_or(const Value& obj, const char* key, bool fallback);
uint32_t midi_word_or_warn(const Value& obj, const char* key, uint32_t clip_id,
                           std::vector<Diagnostic>* diagnostics);
const Array* array_at(const Value& obj, const char* key);
const Object* object_at(const Value& obj, const char* key);
double num_or_any(const Value& obj, const char* primary, const char* legacy, double fallback);
std::string str_or_any(const Value& obj, const char* primary, const char* legacy,
                       const std::string& fallback);
bool bool_or_any(const Value& obj, const char* primary, const char* legacy, bool fallback);

bool schema_version_supported(uint32_t version);

// ===========================================================================
// Encode: arrangement model -> util::json::Value
// ===========================================================================

Value tempo_segment_to_json(const transport::TempoSegment& s);
Value time_signature_to_json(const transport::TimeSignatureSegment& s);
Value automation_lane_to_json(const automation::AutomationLane& lane);
Value track_to_json(const arrangement::Track& t);
Value fade_to_json(const arrangement::ClipFade& f);
Value take_to_json(const arrangement::ClipTake& take);
Value comp_segment_to_json(const arrangement::ClipCompSegment& segment);
Value clip_to_json(const arrangement::EditClip& c);
Value warp_map_to_json(const arrangement::WarpMapRef& map);
Value source_to_json(const arrangement::ClipSource& src);
Value marker_to_json(const arrangement::ProjectMarker& m);
Value chord_to_json(const arrangement::ChordSymbol& c);
Value key_segment_to_json(const arrangement::KeySegment& k);
Value annotation_to_json(const arrangement::ProjectAnnotation& a);
Value sidecar_to_json(const arrangement::AssistSidecar& s);
Value midi_content_to_json(const arrangement::MidiContentStore& midi);
Value insert_to_json(const mixing::api::Insert& ins);
Value scene_to_value(const mixing::api::Scene& scene);

// ===========================================================================
// Decode: util::json::Value -> arrangement model
// ===========================================================================

transport::TempoSegment tempo_segment_from_json(const Value& v);
transport::TimeSignatureSegment time_signature_from_json(const Value& v);
automation::AutomationLane automation_lane_from_json(const Value& v);
arrangement::Track track_from_json(const Value& v);
arrangement::ClipFade fade_from_json(const Value& v);
arrangement::ClipTake take_from_json(const Value& v);
arrangement::ClipCompSegment comp_segment_from_json(const Value& v);
arrangement::EditClip clip_from_json(const Value& v);
arrangement::WarpMapRef warp_map_from_json(const Value& v);
arrangement::ClipSource source_from_json(const Value& v);
arrangement::ChordSymbol chord_from_json(const Value& v);
arrangement::KeySegment key_segment_from_json(const Value& v);
void annotation_from_json(const Value& v, arrangement::ProjectAnnotation* a);
bool sidecar_from_json(const Value& v, arrangement::AssistSidecar* out);
mixing::api::Insert insert_from_json(const Value& v);
mixing::api::Scene scene_from_value(const Value& v);

}  // namespace detail
}  // namespace sonare::serialize
