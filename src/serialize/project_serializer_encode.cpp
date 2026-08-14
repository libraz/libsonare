// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <utility>
#include <variant>

#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer_internal.h"
#include "transport/tempo_map.h"

namespace sonare::serialize {
namespace detail {

// ===========================================================================
// Encode: arrangement model -> util::json::Value
// ===========================================================================

Value tempo_segment_to_json(const transport::TempoSegment& s) {
  Object o;
  o["start_ppq"] = s.start_ppq;
  o["bpm"] = s.bpm;
  o["start_sample"] = s.start_sample;
  o["end_bpm"] = s.end_bpm;
  // end_ppq is internal (populated during TempoMap normalization), not user
  // input — intentionally not serialized.
  return o;
}

Value time_signature_to_json(const transport::TimeSignatureSegment& s) {
  Object o;
  o["start_ppq"] = s.start_ppq;
  o["numerator"] = s.time_sig.numerator;
  o["denominator"] = s.time_sig.denominator;
  return o;
}

Value automation_lane_to_json(const automation::AutomationLane& lane) {
  Object o;
  o["target_param_id"] = static_cast<double>(lane.target_param_id());
  if (lane.target_kind() != automation::AutomationTargetKind::kOpaque) {
    o["target_kind"] = static_cast<int>(lane.target_kind());
  }
  Array points;
  for (const auto& p : lane.points()) {
    Object po;
    po["ppq"] = p.ppq;
    po["value"] = p.value;
    po["curve_to_next"] = static_cast<int>(p.curve_to_next);
    points.push_back(std::move(po));
  }
  o["points"] = std::move(points);
  return o;
}

Value track_to_json(const arrangement::Track& t) {
  Object o;
  o["id"] = static_cast<double>(t.id);
  o["name"] = t.name;
  o["kind"] = static_cast<int>(t.kind);
  o["gain"] = static_cast<double>(t.gain);
  o["mute"] = t.mute;
  o["solo"] = t.solo;
  o["pan"] = static_cast<double>(t.pan);
  o["channel_strip_ref"] = t.channel_strip_ref;
  o["output_target"] = t.output_target;
  o["midi_destination_id"] = static_cast<double>(t.midi_destination_id);
  Array lanes;
  for (const auto& lane : t.automation_lanes) {
    lanes.push_back(automation_lane_to_json(lane));
  }
  o["automation_lanes"] = std::move(lanes);
  return o;
}

Value fade_to_json(const arrangement::ClipFade& f) {
  Object o;
  o["length_ppq"] = f.length_ppq;
  o["curve"] = static_cast<int>(f.curve);
  return o;
}

Value take_to_json(const arrangement::ClipTake& take) {
  Object o;
  o["id"] = static_cast<double>(take.id);
  o["source_id"] = static_cast<double>(take.source_id);
  o["source_offset_ppq"] = take.source_offset_ppq;
  o["name"] = take.name;
  return o;
}

Value comp_segment_to_json(const arrangement::ClipCompSegment& segment) {
  Object o;
  o["start_ppq"] = segment.start_ppq;
  o["end_ppq"] = segment.end_ppq;
  o["take_id"] = static_cast<double>(segment.take_id);
  return o;
}

Value clip_to_json(const arrangement::EditClip& c) {
  Object o;
  o["id"] = static_cast<double>(c.id);
  o["track_id"] = static_cast<double>(c.track_id);
  o["source_id"] = static_cast<double>(c.source_id);
  o["start_ppq"] = c.start_ppq;
  o["length_ppq"] = c.length_ppq;
  o["source_offset_ppq"] = c.source_offset_ppq;
  o["gain"] = c.gain;
  o["fade_in"] = fade_to_json(c.fade_in);
  o["fade_out"] = fade_to_json(c.fade_out);
  o["loop_mode"] = static_cast<int>(c.loop_mode);
  o["loop_length_ppq"] = c.loop_length_ppq;
  // Optional; only emitted when set so existing projects round-trip byte-for-byte.
  if (c.loop_crossfade_ppq != 0.0) o["loop_crossfade_ppq"] = c.loop_crossfade_ppq;
  o["warp_ref_id"] = static_cast<double>(c.warp_ref_id);
  o["warp_mode"] = static_cast<int>(c.warp_mode);
  if (!c.takes.empty()) {
    Array takes;
    for (const auto& take : c.takes) {
      takes.push_back(take_to_json(take));
    }
    o["takes"] = std::move(takes);
  }
  if (c.active_take_id != 0) {
    o["active_take_id"] = static_cast<double>(c.active_take_id);
  }
  if (!c.comp_segments.empty()) {
    Array segments;
    for (const auto& segment : c.comp_segments) {
      segments.push_back(comp_segment_to_json(segment));
    }
    o["comp_segments"] = std::move(segments);
  }
  return o;
}

Value warp_map_to_json(const arrangement::WarpMapRef& map) {
  Object o;
  o["id"] = static_cast<double>(map.id);
  o["name"] = map.name;
  Array anchors;
  for (const auto& anchor : map.anchors) {
    Object ao;
    ao["warp_sample"] = anchor.warp_sample;
    ao["source_sample"] = anchor.source_sample;
    anchors.push_back(std::move(ao));
  }
  o["anchors"] = std::move(anchors);
  return o;
}

Value source_to_json(const arrangement::ClipSource& src) {
  Object o;
  if (const auto* audio = std::get_if<arrangement::AudioSourceRef>(&src)) {
    o["kind"] = static_cast<int>(arrangement::SourceKind::kAudio);
    o["id"] = static_cast<double>(audio->id);
    // Host-local reference only; the core never opens the URI. Recording /
    // generated audio with no file defaults to this ref (URI/asset id) and/or a
    // storage handle id — the core does not embed or interpret blobs.
    //
    // LIMITATION (audio-sample serialization): decoded interleaved PCM is NEVER
    // embedded in the project JSON — that is deliberate, since the document is
    // byte-stable / deterministic by design and PCM blobs would bloat it and
    // break golden stability. The ONLY render-time link to the underlying audio
    // is this @c uri / @c storage_handle_id / @c content_hash triple. A host
    // that saves a project, drops the in-memory content store, then loads + bounces
    // gets SILENT audio clips unless it re-supplies samples for these references
    // (re-decode the URI, or re-bind via the content store). We therefore make
    // sure ALL of those references survive the round-trip so the host can detect
    // an unresolved source and re-decode it, rather than silently bouncing
    // silence. We do NOT base64-embed PCM here.
    o["uri"] = audio->uri;
    o["channel_count"] = static_cast<double>(audio->channel_count);
    o["sample_rate_hint"] = audio->sample_rate_hint;
    o["storage_handle_id"] = static_cast<double>(audio->storage_handle_id);
    // Additive + optional: only emit when set so existing projects without a
    // content hash serialize byte-for-byte identically (no golden churn).
    if (!audio->content_hash.empty()) {
      o["content_hash"] = audio->content_hash;
    }
    if (!audio->external_stem_role.empty()) {
      o["external_stem_role"] = audio->external_stem_role;
    }
  } else {
    const auto& m = std::get<arrangement::MidiSourceRef>(src);
    o["kind"] = static_cast<int>(arrangement::SourceKind::kMidi);
    o["id"] = static_cast<double>(m.id);
    o["name"] = m.name;
    o["channel_hint"] = static_cast<double>(m.channel_hint);
  }
  return o;
}

Value marker_to_json(const arrangement::ProjectMarker& m) {
  Object o;
  o["id"] = static_cast<double>(m.id);
  o["ppq"] = m.ppq;
  o["name"] = m.name;
  // `kind` mirrors SonareMarkerKind. Omitted for the default marker kind (0) to
  // keep existing marker JSON unchanged. The key fields are written for the
  // key-signature kind (4) and, independently, whenever they carry a non-zero
  // value: the C ABI range-checks key_fifths only for kind 4 but accepts the
  // fields on any kind, so keying the write on the kind alone would drop state
  // the edit API can legitimately produce. A marker with zeroed key fields still
  // emits nothing, so ordinary marker JSON is byte-identical.
  if (m.kind != 0) o["kind"] = static_cast<double>(m.kind);
  if (m.kind == 4 || m.key_fifths != 0 || m.key_minor) {
    o["key_fifths"] = static_cast<double>(m.key_fifths);
    o["key_minor"] = m.key_minor;
  }
  return o;
}

Value chord_to_json(const arrangement::ChordSymbol& c) {
  Object o;
  o["start_ppq"] = c.start_ppq;
  o["end_ppq"] = c.end_ppq;
  o["root_pc"] = static_cast<double>(c.root_pc);
  o["quality"] = static_cast<int>(c.quality);
  Array ext;
  for (uint8_t e : c.extensions) ext.push_back(static_cast<double>(e));
  o["extensions"] = std::move(ext);
  o["slash_bass_pc"] = static_cast<double>(c.slash_bass_pc);
  o["roman_numeral"] = c.roman_numeral;
  o["modulation_boundary"] = c.modulation_boundary;
  return o;
}

Value key_segment_to_json(const arrangement::KeySegment& k) {
  Object o;
  o["start_ppq"] = k.start_ppq;
  o["end_ppq"] = k.end_ppq;
  o["tonic_pc"] = static_cast<double>(k.tonic_pc);
  o["mode"] = static_cast<int>(k.mode);
  return o;
}

Value annotation_to_json(const arrangement::ProjectAnnotation& a) {
  Object o;
  o["tempo_confidence"] = a.tempo_confidence;
  Array keys;
  for (const auto& k : a.keys) keys.push_back(key_segment_to_json(k));
  o["keys"] = std::move(keys);
  Array chords;
  for (const auto& c : a.chords) chords.push_back(chord_to_json(c));
  o["chords"] = std::move(chords);
  Array sections;
  for (const auto& s : a.sections) {
    Object so;
    so["start_ppq"] = s.start_ppq;
    so["end_ppq"] = s.end_ppq;
    so["label"] = s.label;
    sections.push_back(std::move(so));
  }
  o["sections"] = std::move(sections);
  Array onsets;
  for (const auto& on : a.onsets) {
    Object oo;
    oo["ppq"] = on.ppq;
    oo["confidence"] = on.confidence;
    onsets.push_back(std::move(oo));
  }
  o["onsets"] = std::move(onsets);
  return o;
}

Value sidecar_to_json(const arrangement::AssistSidecar& s) {
  Object o;
  o["module_id"] = s.module_id;
  o["schema_version"] = static_cast<double>(s.schema_version);
  // Opaque payload: base64 so arbitrary bytes round-trip verbatim regardless of
  // whether the owning module is registered or the schema_version is known.
  o["payload_b64"] = base64_encode(s.payload);
  o["target_track_id"] = static_cast<double>(s.target_track_id);
  o["region_start_ppq"] = s.region_start_ppq;
  o["region_end_ppq"] = s.region_end_ppq;
  return o;
}

Value midi_content_to_json(const arrangement::MidiContentStore& midi) {
  // Keyed by clip id (string) so the std::map dump order is stable and the
  // mapping survives round-trip. Events carry UMP words in POD fields.
  Object o;
  for (const auto& [clip_id, events] : midi.events) {
    Array arr;
    for (const auto& e : events) {
      Object eo;
      eo["ppq"] = e.ppq;
      eo["data0"] = static_cast<double>(e.data0);
      eo["data1"] = static_cast<double>(e.data1);
      if (e.sysex_handle != 0) {
        eo["sysex_handle"] = static_cast<double>(e.sysex_handle);
      }
      arr.push_back(std::move(eo));
    }
    o[std::to_string(clip_id)] = std::move(arr);
  }
  if (!midi.sysex_payloads.empty()) {
    Object payloads;
    for (const auto& [handle, payload] : midi.sysex_payloads) {
      payloads[std::to_string(handle)] = base64_encode(payload);
    }
    o["__sysex_payloads"] = std::move(payloads);
  }
  return o;
}

Value scene_to_value(const mixing::api::Scene& scene) {
  return json::parse(mixing::api::scene_to_json(scene));
}

}  // namespace detail
}  // namespace sonare::serialize
