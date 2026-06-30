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

namespace sonare::serialize {
namespace detail {

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
  s.time_sig.numerator = static_cast<int>(num_or(v, "numerator", 4.0));
  s.time_sig.denominator = static_cast<int>(num_or(v, "denominator", 4.0));
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
      bp.value = static_cast<float>(num_or(pv, "value", 0.0));
      bp.curve_to_next =
          static_cast<automation::CurveType>(static_cast<int>(num_or(pv, "curve_to_next", 0.0)));
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
  t.kind = static_cast<arrangement::Track::Kind>(uint_or(v, "kind", 0));
  t.gain = std::max(0.0f, static_cast<float>(num_or(v, "gain", 1.0)));
  t.mute = bool_or(v, "mute", false);
  t.solo = bool_or(v, "solo", false);
  t.pan = std::clamp(static_cast<float>(num_or(v, "pan", 0.0)), -1.0f, 1.0f);
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
  f.curve = static_cast<arrangement::FadeCurve>(uint_or(v, "curve", 0));
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
  c.gain = static_cast<float>(num_or(v, "gain", 1.0));
  if (const auto* fi = object_at(v, "fade_in")) c.fade_in = fade_from_json(Value(*fi));
  if (const auto* fo = object_at(v, "fade_out")) c.fade_out = fade_from_json(Value(*fo));
  c.loop_mode = static_cast<arrangement::LoopMode>(uint_or(v, "loop_mode", 0));
  c.loop_length_ppq = num_or(v, "loop_length_ppq", 0.0);
  c.loop_crossfade_ppq = num_or(v, "loop_crossfade_ppq", 0.0);
  c.warp_ref_id = uint_or(v, "warp_ref_id", 0);
  c.warp_mode = static_cast<arrangement::WarpMode>(uint_or(v, "warp_mode", 0));
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
  const auto kind = static_cast<arrangement::SourceKind>(uint_or(v, "kind", 0));
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
  return a;
}

arrangement::ChordSymbol chord_from_json(const Value& v) {
  arrangement::ChordSymbol c;
  c.start_ppq = num_or(v, "start_ppq", 0.0);
  c.end_ppq = num_or(v, "end_ppq", 0.0);
  c.root_pc = static_cast<uint8_t>(uint_or(v, "root_pc", arrangement::kUnknownPitchClass));
  c.quality = static_cast<arrangement::ChordQuality>(uint_or(v, "quality", 0));
  if (const auto* arr = array_at(v, "extensions")) {
    for (const auto& ev : *arr) {
      if (ev.is_number()) c.extensions.push_back(static_cast<uint8_t>(ev.as_int()));
    }
  }
  c.slash_bass_pc =
      static_cast<uint8_t>(uint_or(v, "slash_bass_pc", arrangement::kUnknownPitchClass));
  c.roman_numeral = str_or(v, "roman_numeral", "");
  c.modulation_boundary = bool_or(v, "modulation_boundary", false);
  return c;
}

arrangement::KeySegment key_segment_from_json(const Value& v) {
  arrangement::KeySegment k;
  k.start_ppq = num_or(v, "start_ppq", 0.0);
  k.end_ppq = num_or(v, "end_ppq", 0.0);
  k.tonic_pc = static_cast<uint8_t>(uint_or(v, "tonic_pc", arrangement::kUnknownPitchClass));
  k.mode = static_cast<arrangement::KeyMode>(uint_or(v, "mode", 0));
  return k;
}

void annotation_from_json(const Value& v, arrangement::ProjectAnnotation* a) {
  a->tempo_confidence = static_cast<float>(num_or(v, "tempo_confidence", 0.0));
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
      on.confidence = static_cast<float>(num_or(ov, "confidence", 0.0));
      a->onsets.push_back(on);
    }
  }
}

// Returns false (with the sidecar left untouched) when the payload base64 is
// malformed; the caller records a diagnostic. module_id / schema_version are
// preserved verbatim even for unregistered modules / unknown schema versions.
bool sidecar_from_json(const Value& v, arrangement::AssistSidecar* out) {
  out->module_id = str_or(v, "module_id", "");
  out->schema_version = uint_or(v, "schema_version", 0);
  out->target_track_id = uint_or(v, "target_track_id", 0);
  out->region_start_ppq = num_or(v, "region_start_ppq", 0.0);
  out->region_end_ppq = num_or(v, "region_end_ppq", 0.0);
  const std::string b64 = str_or(v, "payload_b64", "");
  return base64_decode(b64, &out->payload);
}

mixing::api::Insert insert_from_json(const Value& v) {
  mixing::api::Insert ins;
  ins.slot = str_or(v, "slot", "pre") == "post" ? mixing::api::InsertSlot::PostFader
                                                : mixing::api::InsertSlot::PreFader;
  ins.processor_name = str_or_any(v, "processor", "processor_name", "");
  ins.params_json = str_or_any(v, "params", "params_json", "");
  ins.sidechain_key = str_or_any(v, "sidechainKey", "sidechain_key", "");
  return ins;
}

mixing::api::Scene scene_from_value(const Value& v) {
  mixing::api::Scene scene;
  scene.version = static_cast<int>(num_or(v, "version", 1.0));
  // Mirror the standalone scene_from_json guard: reject an embedded mixer scene
  // whose version exceeds what this build understands instead of silently
  // mis-reading a future schema. Version 1 is the only supported value today.
  if (scene.version != 1) {
    throw SonareException(ErrorCode::InvalidParameter, "unsupported embedded mixer scene version");
  }
  if (const auto* arr = array_at(v, "strips")) {
    for (const auto& sv : *arr) {
      if (!sv.is_object()) continue;
      mixing::api::Strip s;
      s.id = str_or(sv, "id", "");
      s.input_trim_db = static_cast<float>(num_or_any(sv, "inputTrimDb", "input_trim_db", 0.0));
      s.fader_db = static_cast<float>(num_or_any(sv, "faderDb", "fader_db", 0.0));
      s.vca_offset_db = static_cast<float>(num_or_any(sv, "vcaOffsetDb", "vca_offset_db", 0.0));
      s.pan = static_cast<float>(num_or(sv, "pan", 0.0));
      s.width = static_cast<float>(num_or(sv, "width", 1.0));
      s.muted = bool_or(sv, "muted", false);
      s.soloed = bool_or(sv, "soloed", false);
      s.solo_safe = bool_or_any(sv, "soloSafe", "solo_safe", false);
      s.pan_mode = static_cast<int>(num_or_any(sv, "panMode", "pan_mode", 0.0));
      s.dual_pan_left = static_cast<float>(num_or_any(sv, "dualPanLeft", "dual_pan_left", -1.0));
      s.dual_pan_right = static_cast<float>(num_or_any(sv, "dualPanRight", "dual_pan_right", 1.0));
      s.polarity_invert_left = bool_or_any(sv, "polarityInvertLeft", "polarity_invert_left", false);
      s.polarity_invert_right =
          bool_or_any(sv, "polarityInvertRight", "polarity_invert_right", false);
      s.pan_law = static_cast<int>(num_or_any(sv, "panLaw", "pan_law", 0.0));
      s.channel_delay_samples =
          static_cast<int>(num_or_any(sv, "channelDelaySamples", "channel_delay_samples", 0.0));
      if (const std::string layout = str_or(sv, "sourceLayout", ""); !layout.empty()) {
        ChannelLayout parsed = ChannelLayout::Stereo;
        if (channel_layout_from_string(layout, parsed)) s.source_layout = parsed;
      }
      if (const auto* sp = object_at(sv, "surroundPan")) {
        const Value spv(*sp);
        s.surround_pan.azimuth = static_cast<float>(num_or(spv, "azimuth", 0.0));
        s.surround_pan.elevation = static_cast<float>(num_or(spv, "elevation", 0.0));
        s.surround_pan.divergence = static_cast<float>(num_or(spv, "divergence", 0.0));
        s.surround_pan.lfe = static_cast<float>(num_or(spv, "lfe", 0.0));
        s.surround_pan.distance = static_cast<float>(num_or(spv, "distance", 1.0));
      }
      if (const auto* iarr = array_at(sv, "inserts")) {
        for (const auto& iv : *iarr) {
          if (iv.is_object()) s.inserts.push_back(insert_from_json(iv));
        }
      }
      if (const auto* sarr = array_at(sv, "sends")) {
        for (const auto& dv : *sarr) {
          if (!dv.is_object()) continue;
          mixing::api::Send sd;
          sd.id = str_or(dv, "id", "");
          sd.destination_bus_id = str_or_any(dv, "destinationBusId", "destination_bus_id", "");
          sd.send_db = static_cast<float>(num_or_any(dv, "sendDb", "send_db", 0.0));
          sd.timing = str_or(dv, "timing", "post") == "pre" ? mixing::api::SendTiming::PreFader
                                                            : mixing::api::SendTiming::PostFader;
          s.sends.push_back(std::move(sd));
        }
      }
      scene.strips.push_back(std::move(s));
    }
  }
  if (const auto* arr = array_at(v, "buses")) {
    for (const auto& bv : *arr) {
      if (!bv.is_object()) continue;
      mixing::api::Bus b;
      b.id = str_or(bv, "id", "");
      b.role = str_or(bv, "role", "aux");
      if (const std::string layout = str_or(bv, "layout", ""); !layout.empty()) {
        ChannelLayout parsed = ChannelLayout::Stereo;
        if (channel_layout_from_string(layout, parsed)) b.layout = parsed;
      }
      if (const auto* iarr = array_at(bv, "inserts")) {
        for (const auto& iv : *iarr) {
          if (iv.is_object()) b.inserts.push_back(insert_from_json(iv));
        }
      }
      scene.buses.push_back(std::move(b));
    }
  }
  const Array* vca_groups = array_at(v, "vcaGroups");
  if (vca_groups == nullptr) vca_groups = array_at(v, "vca_groups");
  if (vca_groups != nullptr) {
    for (const auto& vv : *vca_groups) {
      if (!vv.is_object()) continue;
      mixing::api::VcaGroup g;
      g.id = str_or(vv, "id", "");
      g.gain_db = static_cast<float>(num_or_any(vv, "gainDb", "gain_db", 0.0));
      if (const auto* marr = array_at(vv, "members")) {
        for (const auto& mv : *marr) {
          if (mv.is_string()) g.members.push_back(mv.as_string());
        }
      }
      scene.vca_groups.push_back(std::move(g));
    }
  }
  if (const auto* arr = array_at(v, "connections")) {
    for (const auto& cv : *arr) {
      if (!cv.is_object()) continue;
      mixing::api::Connection c;
      c.source = str_or(cv, "source", "");
      c.destination = str_or(cv, "destination", "");
      scene.connections.push_back(std::move(c));
    }
  }
  return scene;
}

}  // namespace detail
}  // namespace sonare::serialize
