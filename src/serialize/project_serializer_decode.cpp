// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer_internal.h"
#include "transport/tempo_map.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::serialize {
namespace detail {

namespace {

template <typename Enum>
Enum enum_or(const Value& value, const char* key, Enum fallback, uint32_t maximum) {
  const uint32_t ordinal = uint_or(value, key, static_cast<uint32_t>(fallback));
  if (ordinal > maximum) {
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
  s.bpm = num_or(v, "bpm", 120.0);
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

automation::AutomationLane automation_lane_from_json(const Value& v) {
  automation::AutomationLane lane(uint_or(v, "target_param_id", 0));
  std::vector<automation::Breakpoint> points;
  if (const auto* arr = array_at(v, "points")) {
    for (const auto& pv : *arr) {
      if (!pv.is_object()) continue;
      automation::Breakpoint bp;
      bp.ppq = num_or(pv, "ppq", 0.0);
      bp.value = float_or(pv, "value", 0.0f, "tracks[].automation_lanes[].points[].value");
      bp.curve_to_next = enum_or(pv, "curve_to_next", automation::CurveType::Linear, 3);
      points.push_back(bp);
    }
  }
  lane.set_points(std::move(points));
  return lane;
}

arrangement::Track track_from_json(const Value& v) {
  arrangement::Track t;
  t.id = uint_or(v, "id", 0);
  t.name = str_or(v, "name", "");
  t.kind = enum_or(v, "kind", arrangement::Track::Kind::kAudio, 2);
  t.gain = std::max(0.0f, float_or(v, "gain", 1.0f, "tracks[].gain"));
  t.mute = bool_or(v, "mute", false);
  t.solo = bool_or(v, "solo", false);
  t.pan = std::clamp(float_or(v, "pan", 0.0f, "tracks[].pan"), -1.0f, 1.0f);
  t.channel_strip_ref = str_or(v, "channel_strip_ref", "");
  t.output_target = str_or(v, "output_target", "");
  t.midi_destination_id = uint_or(v, "midi_destination_id", 0);
  if (const auto* arr = array_at(v, "automation_lanes")) {
    for (const auto& lv : *arr) {
      if (lv.is_object()) t.automation_lanes.push_back(automation_lane_from_json(lv));
    }
  }
  return t;
}

arrangement::ClipFade fade_from_json(const Value& v) {
  arrangement::ClipFade f;
  f.length_ppq = num_or(v, "length_ppq", 0.0);
  f.curve = enum_or(v, "curve", arrangement::FadeCurve::kLinear, 3);
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
  c.loop_mode = enum_or(v, "loop_mode", arrangement::LoopMode::kOff, 1);
  c.loop_length_ppq = num_or(v, "loop_length_ppq", 0.0);
  c.loop_crossfade_ppq = num_or(v, "loop_crossfade_ppq", 0.0);
  c.warp_ref_id = uint_or(v, "warp_ref_id", 0);
  c.warp_mode = enum_or(v, "warp_mode", arrangement::WarpMode::kOff, 2);
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
  const auto kind = enum_or(v, "kind", arrangement::SourceKind::kAudio, 1);
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
  c.quality = enum_or(v, "quality", arrangement::ChordQuality::kUnknown, 7);
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
  k.mode = enum_or(v, "mode", arrangement::KeyMode::kUnknown, 7);
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
    return mixing::api::scene_from_json(json::dump(v));
  } catch (const SonareException& error) {
    // A scene nested in a project is malformed persisted input, while the
    // standalone control-thread API reports InvalidParameter. Keep the project
    // serializer's stable InvalidFormat diagnostic channel at this boundary.
    throw SonareException(ErrorCode::InvalidFormat, error.what());
  }
}

}  // namespace detail
}  // namespace sonare::serialize
