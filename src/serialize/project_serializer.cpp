// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include "serialize/project_serializer.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "transport/tempo_map.h"
#include "util/json.h"

#if defined(SONARE_WITH_MIXING)
// scene_to_json / scene_from_json live in sonare_mixing (BUILD_MIXING). When the
// mixer is built, embed the Scene through the tested serializer (single source
// of truth). Otherwise serialize the always-present Scene struct fields directly.
#endif

namespace sonare::serialize {
namespace {

namespace json = sonare::util::json;
using json::Array;
using json::Object;
using json::Value;

// ===========================================================================
// Deterministic base64 (for opaque AssistSidecar binary payloads). The core
// never interprets sidecar bytes; base64 keeps arbitrary bytes (including NUL /
// non-UTF-8) round-trippable inside a JSON string. Standard alphabet + padding.
// ===========================================================================

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<uint8_t>& bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16) |
                            (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                            static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
    out.push_back(kBase64Alphabet[triple & 0x3F]);
    i += 3;
  }
  const size_t remaining = bytes.size() - i;
  if (remaining == 1) {
    const uint32_t triple = static_cast<uint32_t>(bytes[i]) << 16;
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (remaining == 2) {
    const uint32_t triple =
        (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

// Returns 0..63 for a valid base64 character, or 0xFF for invalid (so a malformed
// payload decodes to a defined value rather than reading OOB; the surrounding
// decode reports failure via the bool result).
uint8_t base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(c - 'A');
  if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 'a' + 26);
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0' + 52);
  if (c == '+') return 62;
  if (c == '/') return 63;
  return 0xFF;
}

// Decodes a standard-alphabet base64 string. Returns false (and leaves *out
// empty) on any malformed length / character so deserialize fails gracefully.
bool base64_decode(const std::string& text, std::vector<uint8_t>* out) {
  out->clear();
  if (text.size() % 4 != 0) return false;
  out->reserve((text.size() / 4) * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    const char c0 = text[i];
    const char c1 = text[i + 1];
    const char c2 = text[i + 2];
    const char c3 = text[i + 3];
    const uint8_t v0 = base64_value(c0);
    const uint8_t v1 = base64_value(c1);
    if (v0 == 0xFF || v1 == 0xFF) return false;
    const bool pad2 = (c2 == '=');
    const bool pad3 = (c3 == '=');
    // Padding may only appear in the final quad, c3 alone or c2+c3 together.
    if ((pad2 || pad3) && i + 4 != text.size()) return false;
    if (pad2 && !pad3) return false;
    uint32_t triple = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12);
    out->push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
    if (!pad2) {
      const uint8_t v2 = base64_value(c2);
      if (v2 == 0xFF) return false;
      triple |= static_cast<uint32_t>(v2) << 6;
      out->push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
      if (!pad3) {
        const uint8_t v3 = base64_value(c3);
        if (v3 == 0xFF) return false;
        triple |= static_cast<uint32_t>(v3);
        out->push_back(static_cast<uint8_t>(triple & 0xFF));
      }
    }
  }
  return true;
}

// ===========================================================================
// Small read helpers (forward-compatible: missing / wrong-typed fields fall back
// to the default, matching the "unknown fields safely ignored" contract).
// ===========================================================================

double num_or(const Value& obj, const char* key, double fallback) {
  const auto* v = obj.find(key);
  return (v && v->is_number()) ? v->as_number() : fallback;
}

uint32_t uint_or(const Value& obj, const char* key, uint32_t fallback) {
  const auto* v = obj.find(key);
  if (!v || !v->is_number()) return fallback;
  const double d = v->as_number();
  return d < 0.0 ? fallback : static_cast<uint32_t>(d);
}

std::string str_or(const Value& obj, const char* key, const std::string& fallback) {
  const auto* v = obj.find(key);
  return (v && v->is_string()) ? v->as_string() : fallback;
}

bool bool_or(const Value& obj, const char* key, bool fallback) {
  const auto* v = obj.find(key);
  return (v && v->is_bool()) ? v->as_bool() : fallback;
}

const Array* array_at(const Value& obj, const char* key) {
  const auto* v = obj.find(key);
  return (v && v->is_array()) ? &v->as_array() : nullptr;
}

const Object* object_at(const Value& obj, const char* key) {
  const auto* v = obj.find(key);
  return (v && v->is_object()) ? &v->as_object() : nullptr;
}

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
  o["channel_strip_ref"] = t.channel_strip_ref;
  o["output_target"] = t.output_target;
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
  o["warp_ref_id"] = static_cast<double>(c.warp_ref_id);
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
    o["uri"] = audio->uri;
    o["channel_count"] = static_cast<double>(audio->channel_count);
    o["sample_rate_hint"] = audio->sample_rate_hint;
    o["storage_handle_id"] = static_cast<double>(audio->storage_handle_id);
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

#if defined(SONARE_WITH_MIXING)
// Embed the mixer Scene through the tested mixer serializer, then re-parse so it
// nests as structured JSON (rather than an escaped string) for stable key order.
Value scene_to_value(const mixing::api::Scene& scene) {
  return json::parse(mixing::api::scene_to_json(scene));
}
#else
Value insert_to_json(const mixing::api::Insert& ins) {
  Object o;
  o["slot"] = ins.slot == mixing::api::InsertSlot::PreFader ? "pre" : "post";
  o["processor_name"] = ins.processor_name;
  o["params_json"] = ins.params_json;
  o["sidechain_key"] = ins.sidechain_key;
  return o;
}

// Mixing-OFF fallback: serialize the always-present Scene struct fields directly
// with util::json, mirroring scene_json.cpp's key names so the structure stays
// stable in both build paths.
Value scene_to_value(const mixing::api::Scene& scene) {
  Object o;
  o["version"] = scene.version;
  Array strips;
  for (const auto& s : scene.strips) {
    Object so;
    so["id"] = s.id;
    so["input_trim_db"] = s.input_trim_db;
    so["fader_db"] = s.fader_db;
    so["pan"] = s.pan;
    so["width"] = s.width;
    so["muted"] = s.muted;
    so["soloed"] = s.soloed;
    so["solo_safe"] = s.solo_safe;
    so["pan_mode"] = s.pan_mode;
    so["dual_pan_left"] = s.dual_pan_left;
    so["dual_pan_right"] = s.dual_pan_right;
    so["polarity_invert_left"] = s.polarity_invert_left;
    so["polarity_invert_right"] = s.polarity_invert_right;
    so["pan_law"] = s.pan_law;
    so["channel_delay_samples"] = s.channel_delay_samples;
    Array inserts;
    for (const auto& ins : s.inserts) inserts.push_back(insert_to_json(ins));
    so["inserts"] = std::move(inserts);
    Array sends;
    for (const auto& sd : s.sends) {
      Object sdo;
      sdo["id"] = sd.id;
      sdo["destination_bus_id"] = sd.destination_bus_id;
      sdo["send_db"] = sd.send_db;
      sdo["timing"] = sd.timing == mixing::api::SendTiming::PreFader ? "pre" : "post";
      sends.push_back(std::move(sdo));
    }
    so["sends"] = std::move(sends);
    strips.push_back(std::move(so));
  }
  o["strips"] = std::move(strips);
  Array buses;
  for (const auto& b : scene.buses) {
    Object bo;
    bo["id"] = b.id;
    bo["role"] = b.role;
    Array inserts;
    for (const auto& ins : b.inserts) inserts.push_back(insert_to_json(ins));
    bo["inserts"] = std::move(inserts);
    buses.push_back(std::move(bo));
  }
  o["buses"] = std::move(buses);
  Array vcas;
  for (const auto& v : scene.vca_groups) {
    Object vo;
    vo["id"] = v.id;
    vo["gain_db"] = v.gain_db;
    Array members;
    for (const auto& m : v.members) members.push_back(m);
    vo["members"] = std::move(members);
    vcas.push_back(std::move(vo));
  }
  o["vca_groups"] = std::move(vcas);
  Array connections;
  for (const auto& c : scene.connections) {
    Object co;
    co["source"] = c.source;
    co["destination"] = c.destination;
    connections.push_back(std::move(co));
  }
  o["connections"] = std::move(connections);
  return o;
}
#endif

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
  t.channel_strip_ref = str_or(v, "channel_strip_ref", "");
  t.output_target = str_or(v, "output_target", "");
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
  c.warp_ref_id = uint_or(v, "warp_ref_id", 0);
  return c;
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

#if defined(SONARE_WITH_MIXING)
mixing::api::Scene scene_from_value(const Value& v) {
  // Re-serialize the embedded subtree and hand it to the tested mixer parser.
  return mixing::api::scene_from_json(json::dump(v));
}
#else
mixing::api::Insert insert_from_json(const Value& v) {
  mixing::api::Insert ins;
  ins.slot = str_or(v, "slot", "pre") == "post" ? mixing::api::InsertSlot::PostFader
                                                : mixing::api::InsertSlot::PreFader;
  ins.processor_name = str_or(v, "processor_name", "");
  ins.params_json = str_or(v, "params_json", "");
  ins.sidechain_key = str_or(v, "sidechain_key", "");
  return ins;
}

mixing::api::Scene scene_from_value(const Value& v) {
  mixing::api::Scene scene;
  scene.version = static_cast<int>(num_or(v, "version", 1.0));
  if (const auto* arr = array_at(v, "strips")) {
    for (const auto& sv : *arr) {
      if (!sv.is_object()) continue;
      mixing::api::Strip s;
      s.id = str_or(sv, "id", "");
      s.input_trim_db = static_cast<float>(num_or(sv, "input_trim_db", 0.0));
      s.fader_db = static_cast<float>(num_or(sv, "fader_db", 0.0));
      s.pan = static_cast<float>(num_or(sv, "pan", 0.0));
      s.width = static_cast<float>(num_or(sv, "width", 1.0));
      s.muted = bool_or(sv, "muted", false);
      s.soloed = bool_or(sv, "soloed", false);
      s.solo_safe = bool_or(sv, "solo_safe", false);
      s.pan_mode = static_cast<int>(num_or(sv, "pan_mode", 0.0));
      s.dual_pan_left = static_cast<float>(num_or(sv, "dual_pan_left", -1.0));
      s.dual_pan_right = static_cast<float>(num_or(sv, "dual_pan_right", 1.0));
      s.polarity_invert_left = bool_or(sv, "polarity_invert_left", false);
      s.polarity_invert_right = bool_or(sv, "polarity_invert_right", false);
      s.pan_law = static_cast<int>(num_or(sv, "pan_law", 0.0));
      s.channel_delay_samples = static_cast<int>(num_or(sv, "channel_delay_samples", 0.0));
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
          sd.destination_bus_id = str_or(dv, "destination_bus_id", "");
          sd.send_db = static_cast<float>(num_or(dv, "send_db", 0.0));
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
      if (const auto* iarr = array_at(bv, "inserts")) {
        for (const auto& iv : *iarr) {
          if (iv.is_object()) b.inserts.push_back(insert_from_json(iv));
        }
      }
      scene.buses.push_back(std::move(b));
    }
  }
  if (const auto* arr = array_at(v, "vca_groups")) {
    for (const auto& vv : *arr) {
      if (!vv.is_object()) continue;
      mixing::api::VcaGroup g;
      g.id = str_or(vv, "id", "");
      g.gain_db = static_cast<float>(num_or(vv, "gain_db", 0.0));
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
#endif

// ===========================================================================
// Migration hook. The current serializer knows schema version 1. A document with the same
// MAJOR version is accepted (forward-compatible field handling above); an
// unknown future major is rejected with a diagnostic rather than misread.
// ===========================================================================

bool schema_version_supported(uint32_t version) { return version <= SONARE_PROJECT_SCHEMA_VERSION; }

}  // namespace

// ===========================================================================
// Public: serialize
// ===========================================================================

std::string project_to_json(const arrangement::Project& project,
                            const arrangement::MidiContentStore& midi) {
  Object root;
  root["version"] = static_cast<double>(SONARE_PROJECT_SCHEMA_VERSION);
  root["sample_rate"] = project.sample_rate();
  root["overlap_policy"] = static_cast<int>(project.overlap_policy());

  Array tempo;
  for (const auto& s : project.tempo_segments()) tempo.push_back(tempo_segment_to_json(s));
  root["tempo_segments"] = std::move(tempo);

  Array sigs;
  for (const auto& s : project.time_signatures()) sigs.push_back(time_signature_to_json(s));
  root["time_signatures"] = std::move(sigs);

  Array sources;
  for (const auto& src : project.sources()) sources.push_back(source_to_json(src));
  root["sources"] = std::move(sources);

  Array tracks;
  for (const auto& t : project.tracks()) tracks.push_back(track_to_json(t));
  root["tracks"] = std::move(tracks);

  Array clips;
  for (const auto& c : project.clips()) clips.push_back(clip_to_json(c));
  root["clips"] = std::move(clips);

  Array markers;
  for (const auto& m : project.markers()) markers.push_back(marker_to_json(m));
  root["markers"] = std::move(markers);

  root["annotation"] = annotation_to_json(project.annotation());
  root["midi_content"] = midi_content_to_json(midi);
  root["scene"] = scene_to_value(project.scene());

  Array sidecars;
  for (const auto& s : project.assist_sidecars()) sidecars.push_back(sidecar_to_json(s));
  root["assist_sidecars"] = std::move(sidecars);

  return json::dump(Value(std::move(root)));
}

// ===========================================================================
// Public: deserialize
// ===========================================================================

DeserializeResult project_from_json(const std::string& json_text) {
  DeserializeResult result;

  Value root;
  try {
    root = json::parse(json_text);
  } catch (const json::JsonError& e) {
    result.diagnostics.push_back({DiagnosticSeverity::kError, "malformed_json", e.what()});
    return result;
  } catch (...) {
    // Defensive: parse() only throws JsonError, but never let anything escape.
    result.diagnostics.push_back(
        {DiagnosticSeverity::kError, "malformed_json", "unknown parse failure"});
    return result;
  }

  try {
    if (!root.is_object()) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, "not_an_object", "top-level JSON value is not an object"});
      return result;
    }

    const auto* version = root.find("version");
    if (!version || !version->is_number()) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, "missing_version", "missing mandatory \"version\" field"});
      return result;
    }
    const double version_d = version->as_number();
    if (version_d < 0.0) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, "invalid_version", "negative schema version"});
      return result;
    }
    const uint32_t schema_version = static_cast<uint32_t>(version_d);
    if (!schema_version_supported(schema_version)) {
      result.diagnostics.push_back({DiagnosticSeverity::kError, "unsupported_schema_version",
                                    "schema version " + std::to_string(schema_version) +
                                        " is newer than supported " +
                                        std::to_string(SONARE_PROJECT_SCHEMA_VERSION)});
      return result;
    }

    arrangement::Project project;
    project.set_sample_rate(num_or(root, "sample_rate", 22050.0));
    project.set_overlap_policy(
        static_cast<arrangement::OverlapPolicy>(uint_or(root, "overlap_policy", 0)));

    // Tempo / time signature.
    std::vector<transport::TempoSegment> tempo;
    if (const auto* arr = array_at(root, "tempo_segments")) {
      for (const auto& sv : *arr) {
        if (sv.is_object()) tempo.push_back(tempo_segment_from_json(sv));
      }
    }
    project.set_tempo_segments(std::move(tempo));

    std::vector<transport::TimeSignatureSegment> sigs;
    if (const auto* arr = array_at(root, "time_signatures")) {
      for (const auto& sv : *arr) {
        if (sv.is_object()) sigs.push_back(time_signature_from_json(sv));
      }
    }
    project.set_time_signatures(std::move(sigs));

    // Sources (insert verbatim, preserving ids, then bump the id counters so a
    // later add_* allocates a fresh non-colliding id).
    uint32_t max_source_id = 0;
    if (const auto* arr = array_at(root, "sources")) {
      for (const auto& sv : *arr) {
        if (!sv.is_object()) continue;
        arrangement::ClipSource src = source_from_json(sv);
        const arrangement::SourceId sid = arrangement::source_id(src);
        if (sid > max_source_id) max_source_id = sid;
        project.insert_source_raw(std::move(src));
      }
    }
    if (max_source_id > 0) project.ensure_next_source_id(max_source_id);

    // Tracks.
    uint32_t max_track_id = 0;
    if (const auto* arr = array_at(root, "tracks")) {
      for (const auto& tv : *arr) {
        if (!tv.is_object()) continue;
        arrangement::Track t = track_from_json(tv);
        if (t.id > max_track_id) max_track_id = t.id;
        project.insert_track_raw(std::move(t));
      }
    }
    if (max_track_id > 0) project.ensure_next_track_id(max_track_id);

    // Clips (insert verbatim; bypasses overlap validation to preserve the saved
    // arrangement exactly, including comp lanes that intentionally overlap).
    uint32_t max_clip_id = 0;
    if (const auto* arr = array_at(root, "clips")) {
      for (const auto& cv : *arr) {
        if (!cv.is_object()) continue;
        arrangement::EditClip c = clip_from_json(cv);
        if (c.id > max_clip_id) max_clip_id = c.id;
        project.insert_clip_raw(std::move(c));
      }
    }
    if (max_clip_id > 0) project.ensure_next_clip_id(max_clip_id);

    // Markers.
    uint32_t max_marker_id = 0;
    if (const auto* arr = array_at(root, "markers")) {
      for (const auto& mv : *arr) {
        if (!mv.is_object()) continue;
        arrangement::ProjectMarker m;
        m.id = uint_or(mv, "id", 0);
        m.ppq = num_or(mv, "ppq", 0.0);
        m.name = str_or(mv, "name", "");
        if (m.id > max_marker_id) max_marker_id = m.id;
        project.markers_mutable().push_back(std::move(m));
      }
    }
    if (max_marker_id > 0) project.ensure_next_marker_id(max_marker_id);

    // Annotation.
    if (const auto* av = object_at(root, "annotation")) {
      annotation_from_json(Value(*av), &project.annotation());
    }

    // Mixer scene.
    if (const auto* sv = object_at(root, "scene")) {
      project.scene() = scene_from_value(Value(*sv));
    }

    // Assist sidecars (lossless, even for unregistered modules).
    if (const auto* arr = array_at(root, "assist_sidecars")) {
      for (const auto& sv : *arr) {
        if (!sv.is_object()) continue;
        arrangement::AssistSidecar sidecar;
        if (!sidecar_from_json(sv, &sidecar)) {
          result.diagnostics.push_back({DiagnosticSeverity::kError, "invalid_sidecar_payload",
                                        "assist sidecar payload base64 is malformed"});
          return result;
        }
        project.add_assist_sidecar(std::move(sidecar));
      }
    }

    // MIDI content store (keyed by clip id string).
    if (const auto* mc = object_at(root, "midi_content")) {
      for (const auto& [key, value] : *mc) {
        if (key == "__sysex_payloads") {
          if (!value.is_object()) continue;
          for (const auto& [handle_key, payload_value] : value.as_object()) {
            if (!payload_value.is_string()) continue;
            arrangement::ClipId handle = 0;
            try {
              handle = static_cast<arrangement::ClipId>(std::stoul(handle_key));
            } catch (...) {
              continue;
            }
            std::vector<uint8_t> payload;
            if (!base64_decode(payload_value.as_string(), &payload)) {
              result.diagnostics.push_back({DiagnosticSeverity::kError, "invalid_sysex_payload",
                                            "MIDI SysEx payload base64 is malformed"});
              return result;
            }
            result.midi.sysex_payloads[handle] = std::move(payload);
          }
          continue;
        }
        if (!value.is_array()) continue;
        arrangement::ClipId clip_id = 0;
        try {
          clip_id = static_cast<arrangement::ClipId>(std::stoul(key));
        } catch (...) {
          continue;  // Non-numeric key: ignore (forward-compat).
        }
        arrangement::MidiClipEventList events;
        for (const auto& ev : value.as_array()) {
          if (!ev.is_object()) continue;
          arrangement::MidiClipEvent e;
          e.ppq = num_or(ev, "ppq", 0.0);
          e.data0 = uint_or(ev, "data0", 0);
          e.data1 = uint_or(ev, "data1", 0);
          e.sysex_handle = uint_or(ev, "sysex_handle", 0);
          events.push_back(e);
        }
        result.midi.events[clip_id] = std::move(events);
      }
    }

    result.project = std::move(project);
    return result;
  } catch (const std::exception& e) {
    // Any structural surprise (bad get<>, etc.) becomes a diagnostic, never a
    // crash. `result.project` stays empty.
    result.project.reset();
    result.diagnostics.push_back({DiagnosticSeverity::kError, "deserialize_failed", e.what()});
    return result;
  } catch (...) {
    result.project.reset();
    result.diagnostics.push_back(
        {DiagnosticSeverity::kError, "deserialize_failed", "unknown deserialize failure"});
    return result;
  }
}

}  // namespace sonare::serialize
