// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include "serialize/project_serializer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

namespace sonare::serialize {
namespace {

namespace json = sonare::util::json;
using json::Array;
using json::Object;
using json::Value;

constexpr double kMinProjectSampleRate = 8000.0;
constexpr double kMaxProjectSampleRate = 384000.0;

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
  constexpr double kMaxUint32 = 4294967295.0;  // 2^32 - 1
  return (!std::isfinite(d) || d < 0.0 || d > kMaxUint32) ? fallback : static_cast<uint32_t>(d);
}

bool parse_uint32_key(const std::string& key, uint32_t* out) {
  if (out == nullptr || key.empty()) return false;
  uint64_t value = 0;
  for (const char c : key) {
    if (c < '0' || c > '9') return false;
    value = value * 10u + static_cast<uint64_t>(c - '0');
    if (value > std::numeric_limits<uint32_t>::max()) return false;
  }
  *out = static_cast<uint32_t>(value);
  return true;
}

std::string str_or(const Value& obj, const char* key, const std::string& fallback) {
  const auto* v = obj.find(key);
  return (v && v->is_string()) ? v->as_string() : fallback;
}

bool bool_or(const Value& obj, const char* key, bool fallback) {
  const auto* v = obj.find(key);
  return (v && v->is_bool()) ? v->as_bool() : fallback;
}

// Reads a UMP data word (full uint32_t range). A present-but-out-of-range value
// (negative, non-finite, or above the uint32_t maximum) is clamped deterministically
// to 0 / 0xFFFFFFFF and recorded as a warning, instead of being silently zeroed.
// Absent / non-numeric (the forward-compatible default) stays a silent 0.
uint32_t midi_word_or_warn(const Value& obj, const char* key, uint32_t clip_id,
                           std::vector<Diagnostic>* diagnostics) {
  const auto* v = obj.find(key);
  if (!v || !v->is_number()) return 0;
  const double d = v->as_number();
  constexpr double kMaxWord = 4294967295.0;  // 2^32 - 1
  if (!std::isfinite(d) || d < 0.0 || d > kMaxWord) {
    const uint32_t clamped = (!std::isfinite(d) || d < 0.0) ? 0u : static_cast<uint32_t>(kMaxWord);
    diagnostics->push_back({DiagnosticSeverity::kWarning, "midi_word_out_of_range",
                            "MIDI event field \"" + std::string(key) + "\" on clip " +
                                std::to_string(clip_id) + " is out of range; clamped to " +
                                std::to_string(clamped)});
    return clamped;
  }
  return static_cast<uint32_t>(d);
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
  // keep existing marker JSON unchanged; key fields are written only for the
  // key-signature kind (4).
  if (m.kind != 0) o["kind"] = static_cast<double>(m.kind);
  if (m.kind == 4) {
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

Value insert_to_json(const mixing::api::Insert& ins) {
  Object o;
  o["slot"] = ins.slot == mixing::api::InsertSlot::PreFader ? "pre" : "post";
  o["processor"] = ins.processor_name;
  o["params"] = ins.params_json;
  if (!ins.sidechain_key.empty()) {
    o["sidechainKey"] = ins.sidechain_key;
  }
  return o;
}

// Serialize the always-present Scene struct fields directly with the same
// camelCase keys used by mixing::api::scene_to_json. Keeping this walker local
// avoids BUILD_MIXING ON/OFF changing project JSON shape.
Value scene_to_value(const mixing::api::Scene& scene) {
  Object o;
  o["version"] = scene.version;
  Array strips;
  for (const auto& s : scene.strips) {
    Object so;
    so["id"] = s.id;
    so["inputTrimDb"] = s.input_trim_db;
    so["faderDb"] = s.fader_db;
    so["vcaOffsetDb"] = s.vca_offset_db;
    so["pan"] = s.pan;
    so["width"] = s.width;
    so["muted"] = s.muted;
    so["soloed"] = s.soloed;
    so["soloSafe"] = s.solo_safe;
    so["panMode"] = s.pan_mode;
    so["dualPanLeft"] = s.dual_pan_left;
    so["dualPanRight"] = s.dual_pan_right;
    so["polarityInvertLeft"] = s.polarity_invert_left;
    so["polarityInvertRight"] = s.polarity_invert_right;
    so["panLaw"] = s.pan_law;
    so["channelDelaySamples"] = s.channel_delay_samples;
    // Surround source layout + pan: omit at the stereo / centered-point default
    // so existing stereo scenes serialize unchanged (mirrors scene_json.cpp).
    if (s.source_layout != ChannelLayout::Stereo) {
      so["sourceLayout"] = channel_layout_to_string(s.source_layout);
    }
    const mixing::api::SurroundPan& sp = s.surround_pan;
    if (sp.azimuth != 0.0f || sp.elevation != 0.0f || sp.divergence != 0.0f || sp.lfe != 0.0f ||
        sp.distance != 1.0f) {
      Object pan;
      pan["azimuth"] = sp.azimuth;
      pan["elevation"] = sp.elevation;
      pan["divergence"] = sp.divergence;
      pan["lfe"] = sp.lfe;
      pan["distance"] = sp.distance;
      so["surroundPan"] = std::move(pan);
    }
    Array inserts;
    for (const auto& ins : s.inserts) inserts.push_back(insert_to_json(ins));
    so["inserts"] = std::move(inserts);
    Array sends;
    for (const auto& sd : s.sends) {
      Object sdo;
      sdo["id"] = sd.id;
      sdo["destinationBusId"] = sd.destination_bus_id;
      sdo["sendDb"] = sd.send_db;
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
    // Omit at the stereo default so existing stereo scenes are unchanged; only
    // surround buses carry the field (mirrors scene_json.cpp).
    if (b.layout != ChannelLayout::Stereo) {
      bo["layout"] = channel_layout_to_string(b.layout);
    }
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
    vo["gainDb"] = v.gain_db;
    Array members;
    for (const auto& m : v.members) members.push_back(m);
    vo["members"] = std::move(members);
    vcas.push_back(std::move(vo));
  }
  o["vcaGroups"] = std::move(vcas);
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

double num_or_any(const Value& obj, const char* primary, const char* legacy, double fallback);
std::string str_or_any(const Value& obj, const char* primary, const char* legacy,
                       const std::string& fallback);

mixing::api::Insert insert_from_json(const Value& v) {
  mixing::api::Insert ins;
  ins.slot = str_or(v, "slot", "pre") == "post" ? mixing::api::InsertSlot::PostFader
                                                : mixing::api::InsertSlot::PreFader;
  ins.processor_name = str_or_any(v, "processor", "processor_name", "");
  ins.params_json = str_or_any(v, "params", "params_json", "");
  ins.sidechain_key = str_or_any(v, "sidechainKey", "sidechain_key", "");
  return ins;
}

double num_or_any(const Value& obj, const char* primary, const char* legacy, double fallback) {
  const auto* v = obj.find(primary);
  if (v && v->is_number()) return v->as_number();
  return num_or(obj, legacy, fallback);
}

std::string str_or_any(const Value& obj, const char* primary, const char* legacy,
                       const std::string& fallback) {
  const auto* v = obj.find(primary);
  if (v && v->is_string()) return v->as_string();
  return str_or(obj, legacy, fallback);
}

bool bool_or_any(const Value& obj, const char* primary, const char* legacy, bool fallback) {
  const auto* v = obj.find(primary);
  if (v && v->is_bool()) return v->as_bool();
  return bool_or(obj, legacy, fallback);
}

mixing::api::Scene scene_from_value(const Value& v) {
  mixing::api::Scene scene;
  scene.version = static_cast<int>(num_or(v, "version", 1.0));
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

  Array warp_maps;
  for (const auto& map : project.warp_maps()) warp_maps.push_back(warp_map_to_json(map));
  root["warp_maps"] = std::move(warp_maps);

  Array markers;
  for (const auto& m : project.markers()) markers.push_back(marker_to_json(m));
  root["markers"] = std::move(markers);

  // The next_*_id counters are deliberately NOT serialized: serialization must
  // be a pure function of the user-visible arrangement so that an edit + undo
  // round-trip restores the exact bytes (the cross-binding undo contract,
  // pinned by the binding parity tests) even though the counters stay bumped.
  // Consequence (documented trade-off): a reload derives the counters from the
  // live max-id scan below, so an id that was allocated and deleted before the
  // save can be re-allocated after a load. Hosts must not key external state
  // by project ids across save/load boundaries.
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
    // Default matches arrangement::Project's constructor default (48 kHz, the
    // conventional DAW render rate) so a document that omits "sample_rate"
    // round-trips to the same rate an in-memory project would have.
    const double sample_rate = num_or(root, "sample_rate", 48000.0);
    if (!std::isfinite(sample_rate) || sample_rate < kMinProjectSampleRate ||
        sample_rate > kMaxProjectSampleRate) {
      result.diagnostics.push_back({DiagnosticSeverity::kError, "invalid_sample_rate",
                                    "sample_rate must be finite and within the supported range"});
      return result;
    }
    project.set_sample_rate(sample_rate);
    project.set_overlap_policy(
        static_cast<arrangement::OverlapPolicy>(uint_or(root, "overlap_policy", 0)));

    // Tempo / time signature. Segments are validated (finite, positive BPM and
    // start_ppq) and normalized on load: a segment with NaN/Inf or non-positive
    // BPM, or a non-finite start_ppq, is rejected with a diagnostic rather than
    // propagated; the surviving segments are sorted by start_ppq and de-duped on
    // start_ppq (last writer wins) so the in-memory map is well-ordered.
    std::vector<transport::TempoSegment> tempo;
    if (const auto* arr = array_at(root, "tempo_segments")) {
      size_t index = 0;
      for (const auto& sv : *arr) {
        if (!sv.is_object()) {
          ++index;
          continue;
        }
        transport::TempoSegment s = tempo_segment_from_json(sv);
        if (!std::isfinite(s.start_ppq)) {
          result.diagnostics.push_back(
              {DiagnosticSeverity::kError, "invalid_tempo_start_ppq",
               "tempo segment " + std::to_string(index) + " has non-finite start_ppq"});
          return result;
        }
        if (!std::isfinite(s.bpm) || s.bpm <= 0.0) {
          result.diagnostics.push_back(
              {DiagnosticSeverity::kError, "invalid_tempo_bpm",
               "tempo segment " + std::to_string(index) + " has non-finite or non-positive bpm"});
          return result;
        }
        // end_bpm == 0 means "constant tempo"; any explicit end_bpm must be a
        // finite positive ramp target.
        if (s.end_bpm != 0.0 && (!std::isfinite(s.end_bpm) || s.end_bpm < 0.0)) {
          result.diagnostics.push_back(
              {DiagnosticSeverity::kError, "invalid_tempo_end_bpm",
               "tempo segment " + std::to_string(index) + " has invalid end_bpm"});
          return result;
        }
        if (!std::isfinite(s.start_sample)) s.start_sample = 0.0;
        tempo.push_back(s);
        ++index;
      }
    }
    // Stable sort by start_ppq, then drop earlier duplicates sharing a start_ppq
    // (last segment for a tick wins) so the map has a single segment per tick.
    std::stable_sort(tempo.begin(), tempo.end(),
                     [](const transport::TempoSegment& a, const transport::TempoSegment& b) {
                       return a.start_ppq < b.start_ppq;
                     });
    if (tempo.size() > 1) {
      std::vector<transport::TempoSegment> deduped;
      deduped.reserve(tempo.size());
      for (auto& s : tempo) {
        if (!deduped.empty() && deduped.back().start_ppq == s.start_ppq) {
          deduped.back() = s;  // Same tick: keep the later (last-writer) segment.
        } else {
          deduped.push_back(s);
        }
      }
      tempo = std::move(deduped);
    }
    project.set_tempo_segments(std::move(tempo));

    std::vector<transport::TimeSignatureSegment> sigs;
    if (const auto* arr = array_at(root, "time_signatures")) {
      size_t index = 0;
      for (const auto& sv : *arr) {
        if (!sv.is_object()) {
          ++index;
          continue;
        }
        transport::TimeSignatureSegment s = time_signature_from_json(sv);
        if (!std::isfinite(s.start_ppq)) {
          result.diagnostics.push_back(
              {DiagnosticSeverity::kError, "invalid_time_signature_start_ppq",
               "time signature segment " + std::to_string(index) + " has non-finite start_ppq"});
          return result;
        }
        if (s.time_sig.numerator <= 0 || s.time_sig.denominator <= 0) {
          result.diagnostics.push_back({DiagnosticSeverity::kError, "invalid_time_signature",
                                        "time signature segment " + std::to_string(index) +
                                            " has non-positive numerator or denominator"});
          return result;
        }
        sigs.push_back(s);
        ++index;
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

    // Warp maps (plain project metadata referenced by EditClip::warp_ref_id).
    if (const auto* arr = array_at(root, "warp_maps")) {
      for (const auto& wv : *arr) {
        if (!wv.is_object()) continue;
        arrangement::WarpMapRef map = warp_map_from_json(wv);
        project.set_warp_map(std::move(map));
      }
    }

    // Referential integrity (warnings, non-fatal): the saved arrangement is
    // preserved verbatim, but dangling clip references and clip/source-kind
    // mismatches are surfaced as diagnostics so a host can repair them. Sources
    // and tracks are fully loaded above, so lookups here see the whole project.
    for (const auto& c : project.clips()) {
      const arrangement::ClipSource* src = project.find_source(c.source_id);
      if (src == nullptr) {
        result.diagnostics.push_back({DiagnosticSeverity::kWarning, "dangling_clip_source",
                                      "clip " + std::to_string(c.id) +
                                          " references missing source " +
                                          std::to_string(c.source_id)});
      }
      const arrangement::Track* track = project.find_track(c.track_id);
      if (track == nullptr) {
        result.diagnostics.push_back({DiagnosticSeverity::kWarning, "dangling_clip_track",
                                      "clip " + std::to_string(c.id) +
                                          " references missing track " +
                                          std::to_string(c.track_id)});
      }
      if (src != nullptr && track != nullptr) {
        const arrangement::SourceKind sk = arrangement::source_kind(*src);
        const bool audio_ok = track->kind == arrangement::Track::Kind::kAudio &&
                              sk == arrangement::SourceKind::kAudio;
        const bool midi_ok =
            track->kind == arrangement::Track::Kind::kMidi && sk == arrangement::SourceKind::kMidi;
        // kAux tracks hold no clip sources; any source kind on one is a mismatch.
        if (!audio_ok && !midi_ok) {
          result.diagnostics.push_back({DiagnosticSeverity::kWarning, "clip_source_kind_mismatch",
                                        "clip " + std::to_string(c.id) +
                                            " source kind does not match track " +
                                            std::to_string(c.track_id) + " kind"});
        }
      }
    }

    // Markers.
    uint32_t max_marker_id = 0;
    if (const auto* arr = array_at(root, "markers")) {
      for (const auto& mv : *arr) {
        if (!mv.is_object()) continue;
        arrangement::ProjectMarker m;
        m.id = uint_or(mv, "id", 0);
        m.ppq = num_or(mv, "ppq", 0.0);
        m.name = str_or(mv, "name", "");
        m.kind = static_cast<uint8_t>(uint_or(mv, "kind", 0));
        m.key_fifths = static_cast<int8_t>(num_or(mv, "key_fifths", 0.0));
        m.key_minor = bool_or(mv, "key_minor", false);
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
            if (!parse_uint32_key(handle_key, &handle)) {
              result.diagnostics.push_back({DiagnosticSeverity::kWarning, "invalid_sysex_handle",
                                            "MIDI SysEx payload handle key \"" + handle_key +
                                                "\" is outside uint32 range; entry ignored"});
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
        if (!parse_uint32_key(key, &clip_id)) {
          result.diagnostics.push_back(
              {DiagnosticSeverity::kWarning, "invalid_midi_content_key",
               "MIDI content clip key \"" + key + "\" is outside uint32 range; entry ignored"});
          continue;
        }
        arrangement::MidiClipEventList events;
        for (const auto& ev : value.as_array()) {
          if (!ev.is_object()) continue;
          arrangement::MidiClipEvent e;
          e.ppq = num_or(ev, "ppq", 0.0);
          e.data0 = midi_word_or_warn(ev, "data0", clip_id, &result.diagnostics);
          e.data1 = midi_word_or_warn(ev, "data1", clip_id, &result.diagnostics);
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
