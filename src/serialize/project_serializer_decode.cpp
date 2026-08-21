// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer_internal.h"
#include "serialize/serialized_enum_bounds.h"
#include "transport/tempo_map.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

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
    for (const auto& pv : *arr) {
      if (!pv.is_object()) continue;
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
      if (lv.is_object()) {
        t.automation_lanes.push_back(automation_lane_from_json(lv, schema_version));
      }
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

arrangement::ClipTake take_from_json(const Value& v) {
  arrangement::ClipTake take;
  take.id = uint_or(v, "id", 0);
  take.source_id = uint_or(v, "source_id", 0);
  take.source_offset_ppq = num_or(v, "source_offset_ppq", 0.0);
  take.name = str_or(v, "name", "");
  return take;
}

arrangement::ClipCompSegment comp_segment_from_json(const Value& v) {
  arrangement::ClipCompSegment segment;
  segment.start_ppq = num_or(v, "start_ppq", 0.0);
  segment.end_ppq = num_or(v, "end_ppq", 0.0);
  segment.take_id = uint_or(v, "take_id", 0);
  return segment;
}

arrangement::EditClip clip_from_json(const Value& v) {
  arrangement::EditClip c;
  c.id = uint_or(v, "id", 0);
  c.track_id = uint_or(v, "track_id", 0);
  c.source_id = uint_or(v, "source_id", 0);
  c.start_ppq = num_or(v, "start_ppq", 0.0);
  c.length_ppq = num_or(v, "length_ppq", 0.0);
  c.source_offset_ppq = num_or(v, "source_offset_ppq", 0.0);
  c.gain = float_or(v, "gain", 1.0f, "clips[].gain");
  if (const auto* fi = object_at(v, "fade_in")) c.fade_in = fade_from_json(Value(*fi));
  if (const auto* fo = object_at(v, "fade_out")) c.fade_out = fade_from_json(Value(*fo));
  c.loop_mode = enum_or(v, "loop_mode", arrangement::LoopMode::kOff);
  c.loop_length_ppq = num_or(v, "loop_length_ppq", 0.0);
  c.loop_crossfade_ppq = num_or(v, "loop_crossfade_ppq", 0.0);
  c.warp_ref_id = uint_or(v, "warp_ref_id", 0);
  c.warp_mode = enum_or(v, "warp_mode", arrangement::WarpMode::kOff);
  if (const auto* arr = array_at(v, "takes")) {
    for (const auto& tv : *arr) {
      if (tv.is_object()) c.takes.push_back(take_from_json(tv));
    }
  }
  c.active_take_id = uint_or(v, "active_take_id", 0);
  if (const auto* arr = array_at(v, "comp_segments")) {
    for (const auto& sv : *arr) {
      if (sv.is_object()) c.comp_segments.push_back(comp_segment_from_json(sv));
    }
  }
  return c;
}

arrangement::WarpMapRef warp_map_from_json(const Value& v) {
  arrangement::WarpMapRef map;
  map.id = uint_or(v, "id", 0);
  map.name = str_or(v, "name", "");
  if (const auto* arr = array_at(v, "anchors")) {
    for (const auto& av : *arr) {
      if (!av.is_object()) continue;
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
      if (!ev.is_number()) continue;
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
      if (kv.is_object()) a->keys.push_back(key_segment_from_json(kv));
    }
  }
  if (const auto* arr = array_at(v, "chords")) {
    for (const auto& cv : *arr) {
      if (cv.is_object()) a->chords.push_back(chord_from_json(cv));
    }
  }
  if (const auto* arr = array_at(v, "sections")) {
    for (const auto& sv : *arr) {
      if (!sv.is_object()) continue;
      arrangement::SectionSegment s;
      s.start_ppq = num_or(sv, "start_ppq", 0.0);
      s.end_ppq = num_or(sv, "end_ppq", 0.0);
      s.label = str_or(sv, "label", "");
      a->sections.push_back(std::move(s));
    }
  }
  if (const auto* arr = array_at(v, "onsets")) {
    for (const auto& ov : *arr) {
      if (!ov.is_object()) continue;
      arrangement::OnsetMarker on;
      on.ppq = num_or(ov, "ppq", 0.0);
      on.confidence = float_or(ov, "confidence", 0.0f, "annotation.onsets[].confidence");
      a->onsets.push_back(on);
    }
  }
}

// Returns false (with the sidecar left untouched) when the payload base64 is
// malformed; the caller records a diagnostic. module_id / schema_version are
// preserved verbatim even for unregistered modules / unknown schema versions.
bool sidecar_from_json(const Value& v, arrangement::AssistSidecar* out, size_t max_payload_bytes) {
  out->module_id = str_or(v, "module_id", "");
  out->schema_version = uint_or(v, "schema_version", 0);
  out->target_track_id = uint_or(v, "target_track_id", 0);
  out->region_start_ppq = num_or(v, "region_start_ppq", 0.0);
  out->region_end_ppq = num_or(v, "region_end_ppq", 0.0);
  const std::string b64 = str_or(v, "payload_b64", "");
  return base64_decode(b64, &out->payload, max_payload_bytes);
}

mixing::api::Scene scene_from_value(const Value& v) {
  try {
    // Walk the already-parsed sub-tree. Dumping it back to text and re-parsing
    // would cost a second full parse of the scene and would put that parse under
    // a different (unlimited) resource regime than the one the document as a
    // whole was admitted under.
    return mixing::api::scene_from_value(v);
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
// therefore enumerated ONCE here and applied to the finished model, so the value
// set the load path accepts stays a subset of the value set the edit path
// accepts no matter which decoder produced a field.
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
using SidecarKey = std::tuple<std::string, arrangement::TrackId, double, double>;

SidecarKey sidecar_key(const arrangement::AssistSidecar& s) {
  return {s.module_id, s.target_track_id, s.region_start_ppq, s.region_end_ppq};
}

}  // namespace

std::optional<InvariantViolation> enforce_edit_api_invariants(arrangement::Project* project,
                                                              BoundedDiagnostics* diagnostics) {
  if (project == nullptr) return std::nullopt;

  // ---- Positions are non-negative ------------------------------------------
  for (const transport::TempoSegment& s : project->tempo_segments()) {
    if (!valid_position_ppq(s.start_ppq)) {
      return InvariantViolation{"invalid_tempo_start_ppq",
                                "tempo segment start_ppq must be finite and non-negative"};
    }
  }
  for (const transport::TimeSignatureSegment& s : project->time_signatures()) {
    if (!valid_position_ppq(s.start_ppq)) {
      return InvariantViolation{"invalid_time_signature_start_ppq",
                                "time signature segment start_ppq must be finite and non-negative"};
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
    if (!valid_position_ppq(c.fade_in.length_ppq) || !valid_position_ppq(c.fade_out.length_ppq)) {
      return InvariantViolation{"invalid_clip_fade_ppq",
                                label + "fade lengths must be finite and non-negative"};
    }
    // A loop length of 0 is the documented "loop the whole clip" request, so
    // only negatives and non-finite values are out of contract.
    if (!valid_position_ppq(c.loop_length_ppq) || !valid_position_ppq(c.loop_crossfade_ppq)) {
      return InvariantViolation{
          "invalid_clip_loop_ppq",
          label + "loop_length_ppq and loop_crossfade_ppq must be finite and non-negative"};
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
    }
    for (const arrangement::ClipCompSegment& segment : c.comp_segments) {
      if (!valid_position_ppq(segment.start_ppq) || !valid_length_ppq(segment.end_ppq) ||
          !(segment.end_ppq > segment.start_ppq)) {
        return InvariantViolation{
            "invalid_clip_comp_segment_ppq",
            label + "comp segment bounds must be finite with 0 <= start_ppq < end_ppq"};
      }
    }
  }

  // ---- Automation lane identity / target invariants ------------------------
  // AddAutomationLane refuses a second lane for a target, and remove/edit
  // address a lane by its target id alone, so a duplicate is unaddressable.
  // Typed mixer targets have one slot per kind on a track; opaque lanes do not
  // participate in that typed-target uniqueness rule.
  std::vector<arrangement::TrackId> track_ids;
  track_ids.reserve(project->tracks().size());
  for (const arrangement::Track& track : project->tracks()) track_ids.push_back(track.id);
  for (const arrangement::TrackId track_id : track_ids) {
    arrangement::Track* track = project->find_track_mutable(track_id);
    if (track == nullptr) continue;
    std::vector<automation::AutomationLane> kept;
    std::vector<size_t> kept_order;
    kept.reserve(track->automation_lanes.size());
    kept_order.reserve(track->automation_lanes.size());
    for (size_t source_index = 0; source_index < track->automation_lanes.size(); ++source_index) {
      automation::AutomationLane& lane = track->automation_lanes[source_index];
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
      const auto existing = std::find_if(kept.begin(), kept.end(),
                                         [target](const automation::AutomationLane& candidate) {
                                           return candidate.target_param_id() == target;
                                         });
      if (existing != kept.end()) {
        const size_t existing_index = static_cast<size_t>(std::distance(kept.begin(), existing));
        *existing = std::move(lane);  // Last writer wins, in the first slot.
        kept_order[existing_index] = source_index;
        if (diagnostics != nullptr) {
          diagnostics->warn("duplicate_automation_lane",
                            "track " + std::to_string(track_id) +
                                " carries more than one automation lane for target " +
                                std::to_string(target) + "; kept the last one");
        }
        continue;
      }
      kept.push_back(std::move(lane));
      kept_order.push_back(source_index);
    }

    std::vector<automation::AutomationLane> normalized;
    std::vector<size_t> normalized_order;
    normalized.reserve(kept.size());
    normalized_order.reserve(kept.size());
    for (size_t kept_index = 0; kept_index < kept.size(); ++kept_index) {
      automation::AutomationLane& lane = kept[kept_index];
      if (lane.target_kind() == automation::AutomationTargetKind::kOpaque) {
        normalized.push_back(std::move(lane));
        normalized_order.push_back(kept_order[kept_index]);
        continue;
      }
      const auto existing = std::find_if(normalized.begin(), normalized.end(),
                                         [&lane](const automation::AutomationLane& candidate) {
                                           return candidate.target_kind() == lane.target_kind();
                                         });
      if (existing != normalized.end()) {
        const size_t existing_index =
            static_cast<size_t>(std::distance(normalized.begin(), existing));
        if (kept_order[kept_index] > normalized_order[existing_index]) {
          *existing = std::move(lane);  // Last writer wins, in the first slot.
          normalized_order[existing_index] = kept_order[kept_index];
        }
        if (diagnostics != nullptr) {
          diagnostics->warn("duplicate_automation_target_kind",
                            "track " + std::to_string(track_id) +
                                " carries more than one automation lane for target kind " +
                                std::to_string(static_cast<uint32_t>(existing->target_kind())) +
                                "; kept the last one");
        }
        continue;
      }
      normalized.push_back(std::move(lane));
      normalized_order.push_back(kept_order[kept_index]);
    }
    track->automation_lanes = std::move(normalized);
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
  for (arrangement::AssistSidecar& sidecar : sidecars) {
    const SidecarKey key = sidecar_key(sidecar);
    const auto existing = std::find_if(kept_sidecars.begin(), kept_sidecars.end(),
                                       [&key](const arrangement::AssistSidecar& candidate) {
                                         return sidecar_key(candidate) == key;
                                       });
    if (existing != kept_sidecars.end()) {
      *existing = std::move(sidecar);
      if (diagnostics != nullptr) {
        diagnostics->warn("duplicate_assist_sidecar",
                          "assist sidecar \"" + std::get<0>(key) +
                              "\" is present more than once "
                              "for the same target scope; kept the last one");
      }
      continue;
    }
    kept_sidecars.push_back(std::move(sidecar));
  }
  sidecars = std::move(kept_sidecars);

  return std::nullopt;
}

}  // namespace detail
}  // namespace sonare::serialize
