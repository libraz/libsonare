// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arrangement/edit_command_internal.h"
#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer_internal.h"
#include "serialize/project_serializer_private.h"
#include "serialize/serialized_enum_bounds.h"
#include "transport/tempo_map.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::arrangement::detail {

// The serializer is the only code that needs to normalize a loaded track's
// private lane vector. Keeping this access in the decoder TU avoids adding a
// mutable range to the installed Project API or changing Project's layout.
struct ProjectSerializerAccess {
  static std::vector<Track>& tracks(Project& project) noexcept { return project.tracks_; }
};

}  // namespace sonare::arrangement::detail

namespace sonare::serialize {

using constants::kDefaultBpm;

namespace detail {

namespace {

// The accepted ordinal range comes from the enum's own last enumerator (see
// serialized_enum_bounds.h), never from a literal repeated at the call site: a
// literal that lags the enum makes the decoder reject a document the encoder
// just produced, which discards the whole project.
template <typename Enum>
Enum enum_or(const Value& value, const char* key, Enum fallback) {
  const uint32_t ordinal = uint_or(value, key, static_cast<uint32_t>(fallback));
  if (ordinal > kMaxSerializedOrdinal<Enum>) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string("enum field is out of range: ") + key);
  }
  return static_cast<Enum>(ordinal);
}

uint8_t pitch_class_or(const Value& value, const char* key) {
  const uint32_t pitch_class = uint_or(value, key, arrangement::kUnknownPitchClass);
  if (pitch_class > 11 && pitch_class != arrangement::kUnknownPitchClass) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string("pitch-class field is out of range: ") + key);
  }
  return static_cast<uint8_t>(pitch_class);
}

float float_or(const Value& value, const char* key, float fallback, const char* field_path) {
  float converted = 0.0f;
  if (!numeric::checked_float_cast(num_or(value, key, fallback), &converted)) {
    throw SonareException(
        ErrorCode::InvalidFormat,
        std::string("floating-point field is non-finite or out of float range: ") + field_path);
  }
  return converted;
}

}  // namespace

// ===========================================================================
// Decode helpers: util::json::Value -> arrangement model
// ===========================================================================

transport::TempoSegment tempo_segment_from_json(const Value& v) {
  transport::TempoSegment s;
  s.start_ppq = num_or(v, "start_ppq", 0.0);
  // The absent-field fallback is the model's own default, so a document with no
  // tempo decodes to the same segment a default-constructed one produces.
  s.bpm = num_or(v, "bpm", kDefaultBpm);
  s.start_sample = num_or(v, "start_sample", 0.0);
  s.end_bpm = num_or(v, "end_bpm", 0.0);
  return s;
}

transport::TimeSignatureSegment time_signature_from_json(const Value& v) {
  transport::TimeSignatureSegment s;
  s.start_ppq = num_or(v, "start_ppq", 0.0);
  s.time_sig.numerator = int_or(v, "numerator", 4);
  s.time_sig.denominator = int_or(v, "denominator", 4);
  return s;
}

automation::AutomationLane automation_lane_from_json(const Value& v, uint32_t schema_version) {
  automation::AutomationTargetKind target_kind = automation::AutomationTargetKind::kOpaque;
  if (schema_version >= 2u) {
    const uint32_t ordinal = uint_or(v, "target_kind", 0);
    if (ordinal > kMaxSerializedOrdinal<automation::AutomationTargetKind>) {
      throw SonareException(ErrorCode::InvalidFormat,
                            "automation target kind ordinal is out of range");
    }
    target_kind = static_cast<automation::AutomationTargetKind>(ordinal);
  }
  automation::AutomationLane lane(uint_or(v, "target_param_id", 0), target_kind);
  std::vector<automation::Breakpoint> points;
  if (const auto* arr = array_at(v, "points")) {
    for (const auto& element : *arr) {
      const Value& pv = object_element(element, "points");
      automation::Breakpoint bp;
      bp.ppq = num_or(pv, "ppq", 0.0);
      bp.value = float_or(pv, "value", 0.0f, "tracks[].automation_lanes[].points[].value");
      bp.curve_to_next = enum_or(pv, "curve_to_next", automation::CurveType::Linear);
      points.push_back(bp);
    }
  }
  lane.set_points(std::move(points));
  return lane;
}

arrangement::Track track_from_json(const Value& v, uint32_t schema_version) {
  arrangement::Track t;
  t.id = uint_or(v, "id", 0);
  t.name = str_or(v, "name", "");
  t.kind = enum_or(v, "kind", arrangement::Track::Kind::kAudio);
  t.gain = std::max(0.0f, float_or(v, "gain", 1.0f, "tracks[].gain"));
  t.mute = bool_or(v, "mute", false);
  t.solo = bool_or(v, "solo", false);
  t.pan = std::clamp(float_or(v, "pan", 0.0f, "tracks[].pan"), -1.0f, 1.0f);
  t.channel_strip_ref = str_or(v, "channel_strip_ref", "");
  t.output_target = str_or(v, "output_target", "");
  t.midi_destination_id = uint_or(v, "midi_destination_id", 0);
  if (const auto* arr = array_at(v, "automation_lanes")) {
    // Decoded verbatim; the one-lane-per-target invariant is enforced for the
    // whole model by enforce_edit_api_invariants().
    for (const auto& lv : *arr) {
      t.automation_lanes.push_back(
          automation_lane_from_json(object_element(lv, "automation_lanes"), schema_version));
    }
  }
  return t;
}

arrangement::ClipFade fade_from_json(const Value& v) {
  arrangement::ClipFade f;
  f.length_ppq = num_or(v, "length_ppq", 0.0);
  f.curve = enum_or(v, "curve", arrangement::FadeCurve::kLinear);
  return f;
}

arrangement::ClipTake take_from_json(const Value& v, uint32_t schema_version) {
  arrangement::ClipTake take;
  take.id = uint_or(v, "id", 0);
  take.source_id = uint_or(v, "source_id", 0);
  take.source_offset_ppq = num_or(v, "source_offset_ppq", 0.0);
  take.name = str_or(v, "name", "");
  if (schema_version >= SONARE_PROJECT_SCHEMA_VERSION_PHYSICAL_CLIP &&
      v.contains("source_offset_seconds")) {
    take.source_offset_seconds = num_or(v, "source_offset_seconds", 0.0);
  }
  take.partial = bool_or(v, "partial", false);
  return take;
}

arrangement::ClipCompSegment comp_segment_from_json(const Value& v) {
  arrangement::ClipCompSegment segment;
  segment.start_ppq = num_or(v, "start_ppq", 0.0);
  segment.end_ppq = num_or(v, "end_ppq", 0.0);
  segment.take_id = uint_or(v, "take_id", 0);
  // Gated read: a document written before comp seams could crossfade has no
  // key here and loads as the butt join it was rendered with.
  segment.crossfade_ppq = num_or(v, "crossfade_ppq", 0.0);
  return segment;
}

arrangement::ClipCompRenderPart comp_render_part_from_json(const Value& v) {
  arrangement::ClipCompRenderPart part;
  part.visible_start_ppq = num_or(v, "visible_start_ppq", 0.0);
  part.visible_end_ppq = num_or(v, "visible_end_ppq", 0.0);
  part.reference_start_ppq = num_or(v, "reference_start_ppq", 0.0);
  part.reference_end_ppq = num_or(v, "reference_end_ppq", 0.0);
  part.take_id = uint_or(v, "take_id", 0);
  part.seam_fade_in_ppq = num_or(v, "seam_fade_in_ppq", 0.0);
  part.seam_fade_out_ppq = num_or(v, "seam_fade_out_ppq", 0.0);
  part.retained_only = bool_or(v, "retained_only", false);
  return part;
}

arrangement::EditClip clip_from_json(const Value& v, uint32_t schema_version) {
  // Physical fields are read from schema 3 on, as a schema-2 reader would ignore them.
  const bool physical = schema_version >= SONARE_PROJECT_SCHEMA_VERSION_PHYSICAL_CLIP;
  arrangement::EditClip c;
  c.id = uint_or(v, "id", 0);
  c.track_id = uint_or(v, "track_id", 0);
  c.source_id = uint_or(v, "source_id", 0);
  c.start_ppq = num_or(v, "start_ppq", 0.0);
  c.length_ppq = num_or(v, "length_ppq", 0.0);
  c.source_offset_ppq = num_or(v, "source_offset_ppq", 0.0);
  if (physical && v.contains("source_offset_seconds")) {
    c.source_offset_seconds = num_or(v, "source_offset_seconds", 0.0);
  }
  c.gain = float_or(v, "gain", 1.0f, "clips[].gain");
  if (const auto* fi = object_at(v, "fade_in")) c.fade_in = fade_from_json(Value(*fi));
  if (const auto* fo = object_at(v, "fade_out")) c.fade_out = fade_from_json(Value(*fo));
  c.loop_mode = enum_or(v, "loop_mode", arrangement::LoopMode::kOff);
  c.loop_length_ppq = num_or(v, "loop_length_ppq", 0.0);
  c.loop_crossfade_ppq = num_or(v, "loop_crossfade_ppq", 0.0);
  const bool has_loop_period = physical && v.contains("loop_period_seconds");
  const bool has_loop_phase = physical && v.contains("loop_phase_seconds");
  if (has_loop_period != has_loop_phase) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "loop_period_seconds and loop_phase_seconds must be supplied together");
  }
  if (has_loop_period) {
    c.loop_anchor = arrangement::LoopAnchor{
        num_or(v, "loop_period_seconds", 0.0),
        num_or(v, "loop_phase_seconds", 0.0),
    };
  }
  c.warp_ref_id = uint_or(v, "warp_ref_id", 0);
  c.warp_mode = enum_or(v, "warp_mode", arrangement::WarpMode::kOff);
  if (const auto* arr = array_at(v, "takes")) {
    for (const auto& tv : *arr) {
      c.takes.push_back(take_from_json(object_element(tv, "takes"), schema_version));
    }
  }
  c.active_take_id = uint_or(v, "active_take_id", 0);
  if (const auto* arr = array_at(v, "comp_segments")) {
    for (const auto& sv : *arr) {
      c.comp_segments.push_back(comp_segment_from_json(object_element(sv, "comp_segments")));
    }
  }
  if (schema_version >= SONARE_PROJECT_SCHEMA_VERSION_COMP_RENDER_PARTS) {
    if (const auto* arr = array_at(v, "comp_render_parts")) {
      for (const auto& pv : *arr) {
        c.comp_render_parts.push_back(
            comp_render_part_from_json(object_element(pv, "comp_render_parts")));
      }
    }
  }
  return c;
}

arrangement::WarpMapRef warp_map_from_json(const Value& v) {
  arrangement::WarpMapRef map;
  map.id = uint_or(v, "id", 0);
  map.name = str_or(v, "name", "");
  if (const auto* arr = array_at(v, "anchors")) {
    for (const auto& element : *arr) {
      const Value& av = object_element(element, "anchors");
      arrangement::WarpAnchorRef anchor;
      anchor.warp_sample = num_or(av, "warp_sample", 0.0);
      anchor.source_sample = num_or(av, "source_sample", 0.0);
      map.anchors.push_back(anchor);
    }
  }
  return map;
}

arrangement::ClipSource source_from_json(const Value& v) {
  const auto kind = enum_or(v, "kind", arrangement::SourceKind::kAudio);
  if (kind == arrangement::SourceKind::kMidi) {
    arrangement::MidiSourceRef m;
    m.id = uint_or(v, "id", 0);
    m.name = str_or(v, "name", "");
    m.channel_hint = uint_or(v, "channel_hint", 0);
    return m;
  }
  arrangement::AudioSourceRef a;
  a.id = uint_or(v, "id", 0);
  a.uri = str_or(v, "uri", "");
  a.channel_count = uint_or(v, "channel_count", 0);
  a.sample_rate_hint = num_or(v, "sample_rate_hint", 0.0);
  a.storage_handle_id = uint_or(v, "storage_handle_id", 0);
  // Gated read: absent content_hash (older documents) loads as empty.
  a.content_hash = str_or(v, "content_hash", "");
  a.external_stem_role = str_or(v, "external_stem_role", "");
  return a;
}

arrangement::ChordSymbol chord_from_json(const Value& v) {
  arrangement::ChordSymbol c;
  c.start_ppq = num_or(v, "start_ppq", 0.0);
  c.end_ppq = num_or(v, "end_ppq", 0.0);
  c.root_pc = pitch_class_or(v, "root_pc");
  c.quality = enum_or(v, "quality", arrangement::ChordQuality::kUnknown);
  if (const auto* arr = array_at(v, "extensions")) {
    for (const auto& ev : *arr) {
      if (!ev.is_number()) {
        throw SonareException(ErrorCode::InvalidFormat, "chord extension must be a number");
      }
      uint8_t extension = 0;
      if (!numeric::checked_integral_cast(ev.as_number(), &extension)) {
        throw SonareException(ErrorCode::InvalidFormat,
                              "chord extension is fractional or out of uint8 range");
      }
      c.extensions.push_back(extension);
    }
  }
  c.slash_bass_pc = pitch_class_or(v, "slash_bass_pc");
  c.roman_numeral = str_or(v, "roman_numeral", "");
  c.modulation_boundary = bool_or(v, "modulation_boundary", false);
  return c;
}

arrangement::KeySegment key_segment_from_json(const Value& v) {
  arrangement::KeySegment k;
  k.start_ppq = num_or(v, "start_ppq", 0.0);
  k.end_ppq = num_or(v, "end_ppq", 0.0);
  k.tonic_pc = pitch_class_or(v, "tonic_pc");
  k.mode = enum_or(v, "mode", arrangement::KeyMode::kUnknown);
  return k;
}

void annotation_from_json(const Value& v, arrangement::ProjectAnnotation* a) {
  a->tempo_confidence = float_or(v, "tempo_confidence", 0.0f, "annotation.tempo_confidence");
  if (const auto* arr = array_at(v, "keys")) {
    for (const auto& kv : *arr) {
      a->keys.push_back(key_segment_from_json(object_element(kv, "keys")));
    }
  }
  if (const auto* arr = array_at(v, "chords")) {
    for (const auto& cv : *arr) {
      a->chords.push_back(chord_from_json(object_element(cv, "chords")));
    }
  }
  if (const auto* arr = array_at(v, "sections")) {
    for (const auto& element : *arr) {
      const Value& sv = object_element(element, "sections");
      arrangement::SectionSegment s;
      s.start_ppq = num_or(sv, "start_ppq", 0.0);
      s.end_ppq = num_or(sv, "end_ppq", 0.0);
      s.label = str_or(sv, "label", "");
      a->sections.push_back(std::move(s));
    }
  }
  if (const auto* arr = array_at(v, "onsets")) {
    for (const auto& element : *arr) {
      const Value& ov = object_element(element, "onsets");
      arrangement::OnsetMarker on;
      on.ppq = num_or(ov, "ppq", 0.0);
      on.confidence = float_or(ov, "confidence", 0.0f, "annotation.onsets[].confidence");
      a->onsets.push_back(on);
    }
  }
}

void sidecar_fields_from_json(const Value& v, arrangement::AssistSidecar* out) {
  out->module_id = str_or(v, "module_id", "");
  out->schema_version = uint_or(v, "schema_version", 0);
  out->target_track_id = uint_or(v, "target_track_id", 0);
  out->region_start_ppq = num_or(v, "region_start_ppq", 0.0);
  out->region_end_ppq = num_or(v, "region_end_ppq", 0.0);
}

bool sidecar_from_borrowed_payload(const std::string& encoded_payload,
                                   arrangement::AssistSidecar* out, size_t max_payload_bytes) {
  return base64_decode(encoded_payload, &out->payload, max_payload_bytes);
}

// Metadata is assigned before payload decoding, preserving the installed
// helper's field-by-field mutation and metadata-first error precedence. A
// malformed payload therefore leaves the decoded metadata in place while the
// caller records the failed decode.
bool sidecar_from_json(const Value& v, arrangement::AssistSidecar* out, size_t max_payload_bytes) {
  sidecar_fields_from_json(v, out);
  static const std::string kEmptyPayload;
  const std::string* payload = strict_string_field(v, "payload_b64");
  return sidecar_from_borrowed_payload(payload != nullptr ? *payload : kEmptyPayload, out,
                                       max_payload_bytes);
}

mixing::api::Scene scene_from_value(const Value& v, BoundedDiagnostics* diagnostics) {
  try {
    // Walk the already-parsed sub-tree. Dumping it back to text and re-parsing
    // would cost a second full parse of the scene and would put that parse under
    // a different (unlimited) resource regime than the one the document as a
    // whole was admitted under.
    std::vector<std::string> unknown_keys;
    mixing::api::Scene scene = mixing::api::scene_from_value(v, &unknown_keys);
    // The scene sits under the document's `scene` key, so its paths start there.
    if (diagnostics != nullptr) {
      for (const auto& warning : unknown_keys) {
        diagnostics->warn(
            "unknown_scene_key",
            "unknown scene key 'scene." + mixing::api::unknown_scene_key_path(warning) + "'");
      }
    }
    return scene;
  } catch (const SonareException& error) {
    // A scene nested in a project is malformed persisted input, while the
    // standalone control-thread API reports InvalidParameter. Keep the project
    // serializer's stable InvalidFormat diagnostic channel at this boundary.
    throw SonareException(ErrorCode::InvalidFormat, error.what());
  }
}

// ===========================================================================
// Edit-API invariants on the load path
// ===========================================================================
//
// A document can express model states that no sequence of C ABI edit calls can
// reach. Loading one produces a project the edit API can then neither address
// nor repair -- an undeletable automation lane, a marker at a negative PPQ that
// every setter would have rejected. The invariants the setters enforce are
// therefore enumerated ONCE here and applied to the finished model -- and to the
// MIDI content store decoded beside it, which holds clip event positions the
// project itself does not carry -- so the value set the load path accepts stays a
// subset of the value set the edit path accepts no matter which decoder produced
// a field.
//
// Two outcomes. A violation with a well-defined normalization (a duplicate that
// the setter would have overwritten in place) is repaired last-writer-wins and
// reported as a warning. A violation with no meaningful repair (a negative
// position -- clamping it would silently move musical content) is fatal, and the
// caller turns it into an error diagnostic and drops the project, matching how
// the tempo decoder already rejects a non-finite start.

namespace {

// Positions the C ABI setters admit: finite and non-negative
// (`finite_non_negative` in the project edit bridge).
bool valid_position_ppq(double ppq) noexcept { return std::isfinite(ppq) && ppq >= 0.0; }

// Durations the C ABI setters admit: finite and strictly positive
// (`finite_positive` in the project edit bridge).
bool valid_length_ppq(double ppq) noexcept { return std::isfinite(ppq) && ppq > 0.0; }

// Entity ids the model admits. 0 is the failure/sentinel value and UINT32_MAX
// is the exhausted-counter sentinel; the id allocator hands out neither, so a
// document carrying one describes a project no edit sequence could build.
bool valid_entity_id(uint32_t id) noexcept {
  return id != 0 && id != std::numeric_limits<uint32_t>::max();
}

// Identity a sidecar is addressed by. SetAssistSidecar overwrites an existing
// entry with this key rather than appending, and RemoveAssistSidecarInternal
// erases the first match, so two entries sharing it make the second unreachable.
struct SidecarKey {
  std::string module_id;
  arrangement::TrackId target_track_id = 0;
  double region_start_ppq = 0.0;
  double region_end_ppq = 0.0;

  bool operator==(const SidecarKey& other) const noexcept {
    return module_id == other.module_id && target_track_id == other.target_track_id &&
           region_start_ppq == other.region_start_ppq && region_end_ppq == other.region_end_ppq;
  }
};

double canonical_hash_zero(double value) noexcept { return value == 0.0 ? 0.0 : value; }

struct SidecarKeyHash {
  static void combine(size_t* seed, size_t value) noexcept {
    // The constant is the conventional boost-style mixer; the exact value is
    // irrelevant, but combining all fields keeps unrelated identities apart.
    *seed ^= value + static_cast<size_t>(0x9e3779b9U) + (*seed << 6) + (*seed >> 2);
  }

  size_t operator()(const SidecarKey& key) const noexcept {
    size_t seed = std::hash<std::string>{}(key.module_id);
    combine(&seed, std::hash<arrangement::TrackId>{}(key.target_track_id));
    combine(&seed, std::hash<double>{}(canonical_hash_zero(key.region_start_ppq)));
    combine(&seed, std::hash<double>{}(canonical_hash_zero(key.region_end_ppq)));
    return seed;
  }
};

SidecarKey sidecar_key(const arrangement::AssistSidecar& sidecar) {
  return {sidecar.module_id, sidecar.target_track_id, sidecar.region_start_ppq,
          sidecar.region_end_ppq};
}

}  // namespace

std::optional<InvariantViolation> enforce_edit_api_invariants(arrangement::Project* project,
                                                              arrangement::MidiContentStore* midi,
                                                              BoundedDiagnostics* diagnostics) {
  if (project == nullptr) return std::nullopt;

  // ---- Positions are non-negative ------------------------------------------
  // The C-ABI setters bound position AND tempo, not merely their sign
  // (sonare_project_set_tempo_segments / _set_time_signatures both run the
  // valid_public_* predicate). A document that skipped the upper bounds here
  // loaded fine and then could not be re-saved through the edit path it claims
  // to round-trip with, while TempoMap consumers read silently clamped
  // positions from it.
  for (const transport::TempoSegment& s : project->tempo_segments()) {
    if (!valid_position_ppq(s.start_ppq)) {
      return InvariantViolation{"invalid_tempo_start_ppq",
                                "tempo segment start_ppq must be finite and non-negative"};
    }
    if (!transport::valid_public_tempo_segment(s)) {
      return InvariantViolation{
          "invalid_tempo_segment",
          "tempo segment start_ppq or bpm is outside the range the edit API accepts"};
    }
  }
  for (const transport::TimeSignatureSegment& s : project->time_signatures()) {
    if (!valid_position_ppq(s.start_ppq)) {
      return InvariantViolation{"invalid_time_signature_start_ppq",
                                "time signature segment start_ppq must be finite and non-negative"};
    }
    if (!transport::valid_public_time_signature_segment(s)) {
      return InvariantViolation{
          "invalid_time_signature_segment",
          "time signature segment start_ppq, numerator or denominator is outside the range the "
          "edit API accepts"};
    }
  }
  for (const arrangement::ProjectMarker& m : project->markers()) {
    if (!valid_position_ppq(m.ppq)) {
      return InvariantViolation{"invalid_marker_ppq", "marker " + std::to_string(m.id) +
                                                          " ppq must be finite and non-negative"};
    }
    if (!valid_entity_id(m.id)) {
      return InvariantViolation{
          "invalid_marker_id",
          "marker id " + std::to_string(m.id) + " is reserved and cannot be addressed or reloaded"};
    }
  }

  // ---- Clip field invariants -----------------------------------------------
  // sonare_project_add_clip and the clip setters (fade / loop / takes / comp
  // segments) enforce these; the load path inserts clips verbatim to preserve
  // the saved arrangement, so without this pass a hand-edited document could
  // carry a clip no edit call could have produced -- one that loads here and
  // then suppresses the whole timeline at compile time. None of these has a
  // repair that preserves musical intent: clamping a negative offset or a
  // non-positive length would silently move or resize content. All fatal.
  for (const arrangement::EditClip& c : project->clips()) {
    const std::string label = "clip " + std::to_string(c.id) + " ";
    if (!valid_position_ppq(c.start_ppq) || !valid_length_ppq(c.length_ppq) ||
        !valid_position_ppq(c.source_offset_ppq)) {
      return InvariantViolation{"invalid_clip_ppq",
                                label +
                                    "must have a finite non-negative start_ppq and "
                                    "source_offset_ppq and a finite positive length_ppq"};
    }
    if (c.source_offset_seconds.has_value() &&
        !(std::isfinite(*c.source_offset_seconds) && *c.source_offset_seconds >= 0.0)) {
      return InvariantViolation{"invalid_clip_source_offset_seconds",
                                label + "source_offset_seconds must be finite and non-negative"};
    }
    if (!valid_position_ppq(c.fade_in.length_ppq) || !valid_position_ppq(c.fade_out.length_ppq)) {
      return InvariantViolation{"invalid_clip_fade_ppq",
                                label + "fade lengths must be finite and non-negative"};
    }
    // Loop length is finite; under LOOP it is non-negative and 0 loops the whole clip.
    if (!std::isfinite(c.loop_length_ppq) ||
        (c.loop_mode == arrangement::LoopMode::kLoop && !valid_position_ppq(c.loop_length_ppq)) ||
        !valid_position_ppq(c.loop_crossfade_ppq)) {
      return InvariantViolation{"invalid_clip_loop_ppq",
                                label +
                                    "loop_length_ppq must be finite (and non-negative when "
                                    "looping) and loop_crossfade_ppq finite and non-negative"};
    }
    if (c.loop_anchor.has_value() &&
        !(std::isfinite(c.loop_anchor->period_seconds) && c.loop_anchor->period_seconds > 0.0 &&
          std::isfinite(c.loop_anchor->phase_seconds))) {
      return InvariantViolation{
          "invalid_clip_loop_anchor",
          label + "loop_period_seconds must be positive and phase_seconds must be finite"};
    }
    for (const arrangement::ClipTake& take : c.takes) {
      if (take.id == 0) {
        return InvariantViolation{"invalid_clip_take_id",
                                  label + "carries a take with the reserved id 0"};
      }
      if (!valid_position_ppq(take.source_offset_ppq)) {
        return InvariantViolation{"invalid_clip_take_ppq",
                                  label + "take " + std::to_string(take.id) +
                                      " source_offset_ppq must be finite and non-negative"};
      }
      if (take.source_offset_seconds.has_value() &&
          !(std::isfinite(*take.source_offset_seconds) && *take.source_offset_seconds >= 0.0)) {
        return InvariantViolation{"invalid_clip_take_source_offset_seconds",
                                  label + "take " + std::to_string(take.id) +
                                      " source_offset_seconds must be finite and non-negative"};
      }
    }
    if (!arrangement::detail::valid_clip_take_structure(*project, c, c.takes, c.active_take_id)) {
      return InvariantViolation{
          "invalid_clip_take_structure",
          label +
              "takes must have unique non-zero ids, a valid active_take_id, and no takes on a "
              "MIDI track"};
    }
    for (const arrangement::ClipCompSegment& segment : c.comp_segments) {
      if (!valid_position_ppq(segment.start_ppq) || !valid_length_ppq(segment.end_ppq) ||
          !(segment.end_ppq > segment.start_ppq)) {
        return InvariantViolation{
            "invalid_clip_comp_segment_ppq",
            label + "comp segment bounds must be finite with 0 <= start_ppq < end_ppq"};
      }
    }
    if (!arrangement::detail::valid_comp_segments(c.takes, c.comp_segments, c.length_ppq) ||
        (arrangement::detail::clip_is_on_midi_track(*project, c) && !c.comp_segments.empty()) ||
        (c.loop_mode == arrangement::LoopMode::kLoop &&
         arrangement::detail::comp_segments_split_clip(c.comp_segments, c.length_ppq))) {
      return InvariantViolation{
          "invalid_clip_comp_segments",
          label +
              "comp segments must reference known takes, be ordered within the clip, have "
              "valid crossfades, and be compatible with MIDI tracks and looping"};
    }
    if (!arrangement::valid_clip_render_parts(c)) {
      return InvariantViolation{
          "invalid_clip_comp_render_part",
          label + "comp render geometry is invalid or conflicts with the clip's loop"};
    }
  }

  // ---- Automation breakpoint positions -------------------------------------
  // AutomationLane::set_points already rejects a non-finite position, so what a
  // document can still smuggle past it is a negative one; the C ABI breakpoint
  // setter requires finite_non_negative like every other position. A negative
  // breakpoint is not dropped downstream but silently pulled to sample 0 by the
  // bounce scheduler, which is a quieter corruption than a diagnostic. Validate
  // before the identity pass below, so a malformed lane cannot vanish behind a
  // last-writer-wins survivor.
  for (const arrangement::Track& track : project->tracks()) {
    for (const automation::AutomationLane& lane : track.automation_lanes) {
      for (const automation::Breakpoint& point : lane.points()) {
        if (!valid_position_ppq(point.ppq)) {
          return InvariantViolation{
              "invalid_automation_breakpoint_ppq",
              "track " + std::to_string(track.id) + " automation lane " +
                  std::to_string(lane.target_param_id()) +
                  " carries a breakpoint whose ppq is not finite and non-negative"};
        }
      }
    }
  }

  // ---- MIDI clip event positions -------------------------------------------
  // Clip events live in the MIDI content store rather than on the project, so
  // they are the one position set this pass has to be handed explicitly. The
  // setter (sonare_project_set_midi_events) admits 0 <= ppq <= kMaxPublicPpq;
  // an event below the clip's source offset is dropped without a diagnostic at
  // compile time, and the encoder writes the value back verbatim, so a bad
  // position survives every save/load round trip that follows.
  if (midi != nullptr) {
    for (const auto& [clip_id, events] : midi->events) {
      for (const arrangement::MidiClipEvent& event : events) {
        // valid_public_ppq is a two-sided comparison, so NaN and both infinities
        // fail it without a separate finiteness test.
        if (!transport::valid_public_ppq(event.ppq)) {
          return InvariantViolation{
              "invalid_midi_event_ppq",
              "MIDI clip " + std::to_string(clip_id) +
                  " carries an event whose ppq is outside the range the edit API accepts"};
        }
      }
    }
    // The edit API keys events only by a live clip and erases them with it, so an
    // orphan list would otherwise become the content of the clip next given its id.
    for (auto it = midi->events.begin(); it != midi->events.end();) {
      if (project->has_clip(it->first)) {
        ++it;
        continue;
      }
      if (diagnostics != nullptr) {
        diagnostics->warn("dropped_orphan_midi_events",
                          "MIDI content for clip " + std::to_string(it->first) +
                              " names no clip in the project; event list dropped");
      }
      it = midi->events.erase(it);
    }
    for (auto& entry : midi->events) {
      const arrangement::ClipId clip_id = entry.first;
      arrangement::MidiClipEventList& events = entry.second;
      const auto dangling = [&](const arrangement::MidiClipEvent& event) {
        if (event.sysex_handle == 0 || midi->sysex_payloads.count(event.sysex_handle) != 0) {
          return false;
        }
        if (diagnostics != nullptr) {
          diagnostics->warn(
              "dropped_dangling_sysex_event",
              "MIDI clip " + std::to_string(clip_id) + " carries an event whose SysEx handle " +
                  std::to_string(event.sysex_handle) + " has no payload; event dropped");
        }
        return true;
      };
      events.erase(std::remove_if(events.begin(), events.end(), dangling), events.end());
    }
  }

  // ---- Automation lane identity / target invariants ------------------------
  // AddAutomationLane refuses a second lane for a target, and remove/edit
  // address a lane by its target id alone, so a duplicate is unaddressable.
  // Typed mixer targets have one slot per kind on a track; opaque lanes do not
  // participate in that typed-target uniqueness rule.
  for (arrangement::Track& track : arrangement::detail::ProjectSerializerAccess::tracks(*project)) {
    const arrangement::TrackId track_id = track.id;
    std::vector<automation::AutomationLane> kept;
    std::vector<size_t> kept_order;
    kept.reserve(track.automation_lanes.size());
    kept_order.reserve(track.automation_lanes.size());
    std::unordered_map<uint32_t, size_t> first_lane_by_target;
    first_lane_by_target.reserve(track.automation_lanes.size());
    for (size_t source_index = 0; source_index < track.automation_lanes.size(); ++source_index) {
      automation::AutomationLane& lane = track.automation_lanes[source_index];
      const uint32_t target = lane.target_param_id();
      if (target == 0) {
        // Target id 0 predates the non-zero requirement, so documents carrying
        // such a lane exist. It is unaddressable now — edit and remove look a
        // lane up by target id, and the compiler rejects it — so the lane is
        // dropped rather than failing the whole document, which would strand
        // every other edit the project holds.
        if (diagnostics != nullptr) {
          diagnostics->warn("dropped_automation_lane_target_id",
                            "track " + std::to_string(track_id) +
                                " carried an automation lane with target id 0; lane dropped");
        }
        continue;
      }
      if (!automation::valid_automation_target_kind(lane.target_kind())) {
        return InvariantViolation{"invalid_automation_target_kind",
                                  "track " + std::to_string(track_id) +
                                      " contains an automation lane with an invalid target kind"};
      }
      const auto existing = first_lane_by_target.find(target);
      if (existing != first_lane_by_target.end()) {
        const size_t existing_index = existing->second;
        kept[existing_index] = std::move(lane);  // Last writer wins, in the first slot.
        kept_order[existing_index] = source_index;
        if (diagnostics != nullptr) {
          diagnostics->warn("duplicate_automation_lane",
                            "track " + std::to_string(track_id) +
                                " carries more than one automation lane for target " +
                                std::to_string(target) + "; kept the last one");
        }
        continue;
      }
      first_lane_by_target.emplace(target, kept.size());
      kept.push_back(std::move(lane));
      kept_order.push_back(source_index);
    }

    std::vector<automation::AutomationLane> normalized;
    std::vector<size_t> normalized_order;
    normalized.reserve(kept.size());
    normalized_order.reserve(kept.size());
    constexpr size_t kAutomationTargetKindSlotCount =
        static_cast<size_t>(kMaxSerializedOrdinal<automation::AutomationTargetKind>) + 1;
    constexpr size_t kNoAutomationTargetKindSlot = std::numeric_limits<size_t>::max();
    std::array<size_t, kAutomationTargetKindSlotCount> typed_lane_slots;
    typed_lane_slots.fill(kNoAutomationTargetKindSlot);
    for (size_t kept_index = 0; kept_index < kept.size(); ++kept_index) {
      automation::AutomationLane& lane = kept[kept_index];
      if (lane.target_kind() == automation::AutomationTargetKind::kOpaque) {
        normalized.push_back(std::move(lane));
        normalized_order.push_back(kept_order[kept_index]);
        continue;
      }
      const automation::AutomationTargetKind kind = lane.target_kind();
      const size_t kind_slot = static_cast<size_t>(kind);
      const size_t existing_index = typed_lane_slots[kind_slot];
      if (existing_index != kNoAutomationTargetKindSlot) {
        if (kept_order[kept_index] > normalized_order[existing_index]) {
          normalized[existing_index] = std::move(lane);  // Last writer wins, in the first slot.
          normalized_order[existing_index] = kept_order[kept_index];
        }
        if (diagnostics != nullptr) {
          diagnostics->warn("duplicate_automation_target_kind",
                            "track " + std::to_string(track_id) +
                                " carries more than one automation lane for target kind " +
                                std::to_string(static_cast<uint32_t>(kind)) +
                                "; kept the last one");
        }
        continue;
      }
      typed_lane_slots[kind_slot] = normalized.size();
      normalized.push_back(std::move(lane));
      normalized_order.push_back(kept_order[kept_index]);
    }
    track.automation_lanes = std::move(normalized);
  }

  // ---- Assist sidecar field invariants -------------------------------------
  // SetAssistSidecar requires a namespaced, C-string-safe module id and finite
  // non-negative PPQ bounds. The model deliberately permits end <= start: the
  // edit API treats that pair as an unset region, so validation must not turn
  // that documented representation into a rejection. Validate every entry
  // before deduplicating so malformed shadowed entries cannot disappear behind
  // a last-writer-wins survivor.
  for (const arrangement::AssistSidecar& sidecar : project->assist_sidecars()) {
    if (sidecar.module_id.empty() || sidecar.module_id.find('\0') != std::string::npos) {
      return InvariantViolation{
          "invalid_assist_sidecar_module_id",
          "assist sidecar module_id must be non-empty and must not contain an embedded NUL"};
    }
    if (!valid_position_ppq(sidecar.region_start_ppq)) {
      return InvariantViolation{"invalid_assist_sidecar_region_start_ppq",
                                "assist sidecar region_start_ppq must be finite and non-negative"};
    }
    if (!valid_position_ppq(sidecar.region_end_ppq)) {
      return InvariantViolation{"invalid_assist_sidecar_region_end_ppq",
                                "assist sidecar region_end_ppq must be finite and non-negative"};
    }
  }

  // ---- One assist sidecar per identity -------------------------------------
  std::vector<arrangement::AssistSidecar>& sidecars = project->assist_sidecars_mutable();
  std::vector<arrangement::AssistSidecar> kept_sidecars;
  kept_sidecars.reserve(sidecars.size());
  std::unordered_map<SidecarKey, size_t, SidecarKeyHash> first_sidecar_by_identity;
  first_sidecar_by_identity.reserve(sidecars.size());
  for (arrangement::AssistSidecar& sidecar : sidecars) {
    SidecarKey key = sidecar_key(sidecar);
    const auto existing = first_sidecar_by_identity.find(key);
    if (existing != first_sidecar_by_identity.end()) {
      kept_sidecars[existing->second] = std::move(sidecar);
      if (diagnostics != nullptr) {
        diagnostics->warn("duplicate_assist_sidecar",
                          "assist sidecar \"" + key.module_id +
                              "\" is present more than once "
                              "for the same target scope; kept the last one");
      }
      continue;
    }
    first_sidecar_by_identity.emplace(std::move(key), kept_sidecars.size());
    kept_sidecars.push_back(std::move(sidecar));
  }
  sidecars = std::move(kept_sidecars);

  return std::nullopt;
}

}  // namespace detail
}  // namespace sonare::serialize
