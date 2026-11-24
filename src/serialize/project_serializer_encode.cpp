// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include <string>
#include <utility>
#include <variant>

#include "arrangement/edit_source.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"
#include "mixing/api/scene_json_detail.h"
#include "serialize/project_serializer_internal.h"
#include "transport/tempo_map.h"

namespace sonare::serialize {
namespace detail {

// ===========================================================================
// Encode: arrangement model -> util::json::Value
// ===========================================================================

Value tempo_segment_to_json(const transport::TempoSegment& s) {
  Object o;
  o["startPpq"] = s.start_ppq;
  o["bpm"] = s.bpm;
  o["startSample"] = s.start_sample;
  o["endBpm"] = s.end_bpm;
  // end_ppq is internal (populated during TempoMap normalization), not user
  // input — intentionally not serialized.
  return o;
}

Value time_signature_to_json(const transport::TimeSignatureSegment& s) {
  Object o;
  o["startPpq"] = s.start_ppq;
  o["numerator"] = s.time_sig.numerator;
  o["denominator"] = s.time_sig.denominator;
  return o;
}

Value automation_lane_to_json(const automation::AutomationLane& lane) {
  if (lane.lane_id() == 0 || lane.lane_id() == std::numeric_limits<uint32_t>::max()) {
    throw SonareException(ErrorCode::InvalidState,
                          "automation lane must have a serializable lane id");
  }
  const bool opaque = lane.target_kind() == automation::AutomationTargetKind::kOpaque;
  if ((opaque && lane.target_param_id() == 0) || (!opaque && lane.target_param_id() != 0)) {
    throw SonareException(ErrorCode::InvalidState,
                          "automation lane target id does not match its target kind");
  }
  Object o;
  o["id"] = static_cast<double>(lane.lane_id());
  o["targetParamId"] = static_cast<double>(lane.target_param_id());
  const auto& identity = lane.target_identity();
  if (!lane.valid_target_identity()) {
    throw SonareException(ErrorCode::InvalidState,
                          "automation lane target identity does not match its kind");
  }
  if (lane.target_kind() != automation::AutomationTargetKind::kOpaque) {
    o["targetKind"] = enum_token(lane.target_kind());
  }
  switch (lane.target_kind()) {
    case automation::AutomationTargetKind::kTrackSendDb:
      o["sendId"] = identity.send_id;
      break;
    case automation::AutomationTargetKind::kTrackInsertParam: {
      const std::size_t insert_index = *identity.insert_index;
      if (insert_index > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
        throw SonareException(ErrorCode::InvalidState,
                              "insertIndex exceeds the project JSON limit");
      }
      o["insertIndex"] = static_cast<double>(insert_index);
      o["processor"] = identity.processor;
      o["paramName"] = identity.param_name;
      break;
    }
    case automation::AutomationTargetKind::kOpaque:
    case automation::AutomationTargetKind::kTrackFaderDb:
    case automation::AutomationTargetKind::kTrackPan:
    case automation::AutomationTargetKind::kTrackWidth:
      break;
  }
  Array points;
  for (const auto& p : lane.points()) {
    Object po;
    po["ppq"] = p.ppq;
    po["value"] = p.value;
    po["curveToNext"] = enum_token(p.curve_to_next);
    points.push_back(std::move(po));
  }
  o["points"] = std::move(points);
  return o;
}

Value destination_instrument_to_json(uint32_t destination_id,
                                     const arrangement::DestinationInstrumentBinding& binding) {
  Object o;
  o["destinationId"] = static_cast<double>(destination_id);
  if (const auto* synth =
          std::get_if<arrangement::DestinationSynthInstrument>(&binding.instrument)) {
    o["kind"] = std::string("synth");
    Object config;
    config["preset"] = synth->preset;
    if (!synth->engine_mode.empty()) config["engineMode"] = synth->engine_mode;
    Object fields;
    for (const auto& [name, value] : synth->fields) {
      if (const auto* scalar = std::get_if<double>(&value)) {
        fields[name] = *scalar;
      } else {
        fields[name] = std::get<std::string>(value);
      }
    }
    config["fields"] = std::move(fields);
    Object engine_params;
    for (const auto& [name, value] : synth->engine_params) engine_params[name] = value;
    config["engineParams"] = std::move(engine_params);
    if (!synth->mod_routings.empty() || synth->clears_base_mod_routings) {
      Array mod_routings;
      for (const auto& route : synth->mod_routings) {
        Object route_value;
        route_value["source"] = route.source;
        route_value["destination"] = route.destination;
        route_value["depth"] = route.depth;
        mod_routings.push_back(std::move(route_value));
      }
      config["modRoutings"] = std::move(mod_routings);
    }
    config["useGmPrograms"] = synth->use_gm_programs;
    o["synth"] = std::move(config);
  } else if (const auto* builtin =
                 std::get_if<arrangement::DestinationBuiltinInstrument>(&binding.instrument)) {
    o["kind"] = std::string("builtin");
    Object config;
    config["waveform"] = builtin->waveform;
    config["gain"] = builtin->gain;
    config["attackMs"] = builtin->attack_ms;
    config["decayMs"] = builtin->decay_ms;
    config["sustain"] = builtin->sustain;
    config["releaseMs"] = builtin->release_ms;
    config["polyphony"] = builtin->polyphony;
    o["builtin"] = std::move(config);
  } else {
    const auto& sf2 = std::get<arrangement::DestinationSf2Instrument>(binding.instrument);
    o["kind"] = std::string("sf2");
    Object config;
    config["gain"] = sf2.gain;
    config["polyphony"] = sf2.polyphony;
    config["preferModelForModeledFamilies"] = sf2.prefer_model_for_modeled_families;
    config["clearBankRig"] = sf2.clear_bank_rig;
    config["gsEfxRealization"] = sf2.gs_efx_realization;
    if (sf2.sound_font_uri.has_value()) config["soundFontUri"] = *sf2.sound_font_uri;
    o["sf2"] = std::move(config);
  }
  if (binding.bank.has_value()) {
    Object bank;
    bank["msb"] = binding.bank->msb;
    bank["lsb"] = binding.bank->lsb;
    o["bank"] = std::move(bank);
  }
  if (binding.program.has_value()) o["program"] = *binding.program;
  return o;
}

Value track_to_json(const arrangement::Track& t) {
  Object o;
  o["id"] = static_cast<double>(t.id);
  o["name"] = t.name;
  o["kind"] = enum_token(t.kind);
  o["gain"] = static_cast<double>(t.gain);
  o["mute"] = t.mute;
  o["solo"] = t.solo;
  o["pan"] = static_cast<double>(t.pan);
  o["channelStripRef"] = t.channel_strip_ref;
  o["outputTarget"] = t.output_target;
  o["midiDestinationId"] = static_cast<double>(t.midi_destination_id);
  Array lanes;
  for (const auto& lane : t.automation_lanes) {
    lanes.push_back(automation_lane_to_json(lane));
  }
  o["automationLanes"] = std::move(lanes);
  return o;
}

Value fade_to_json(const arrangement::ClipFade& f) {
  Object o;
  o["lengthPpq"] = f.length_ppq;
  o["curve"] = enum_token(f.curve);
  return o;
}

Value take_to_json(const arrangement::ClipTake& take) {
  Object o;
  o["id"] = static_cast<double>(take.id);
  o["sourceId"] = static_cast<double>(take.source_id);
  o["sourceOffsetPpq"] = take.source_offset_ppq;
  o["name"] = take.name;
  if (take.source_offset_seconds.has_value()) {
    o["sourceOffsetSec"] = *take.source_offset_seconds;
  }
  // Optional; only emitted when set so existing projects round-trip byte-for-byte.
  if (take.partial) o["partial"] = true;
  return o;
}

Value comp_segment_to_json(const arrangement::ClipCompSegment& segment) {
  Object o;
  o["startPpq"] = segment.start_ppq;
  o["endPpq"] = segment.end_ppq;
  o["takeId"] = static_cast<double>(segment.take_id);
  o["crossfadePpq"] = segment.crossfade_ppq;
  return o;
}

Value comp_render_part_to_json(const arrangement::ClipCompRenderPart& part) {
  Object o;
  o["visibleStartPpq"] = part.visible_start_ppq;
  o["visibleEndPpq"] = part.visible_end_ppq;
  o["referenceStartPpq"] = part.reference_start_ppq;
  o["referenceEndPpq"] = part.reference_end_ppq;
  o["takeId"] = static_cast<double>(part.take_id);
  o["seamFadeInPpq"] = part.seam_fade_in_ppq;
  o["seamFadeOutPpq"] = part.seam_fade_out_ppq;
  o["retainedOnly"] = part.retained_only;
  return o;
}

Value clip_to_json(const arrangement::EditClip& c) {
  Object o;
  o["id"] = static_cast<double>(c.id);
  o["trackId"] = static_cast<double>(c.track_id);
  o["sourceId"] = static_cast<double>(c.source_id);
  o["startPpq"] = c.start_ppq;
  o["lengthPpq"] = c.length_ppq;
  o["sourceOffsetPpq"] = c.source_offset_ppq;
  if (c.source_offset_seconds.has_value()) {
    o["sourceOffsetSec"] = *c.source_offset_seconds;
  }
  o["gain"] = c.gain;
  o["fadeIn"] = fade_to_json(c.fade_in);
  o["fadeOut"] = fade_to_json(c.fade_out);
  o["loopMode"] = enum_token(c.loop_mode);
  o["loopLengthPpq"] = c.loop_length_ppq;
  if (c.loop_anchor.has_value()) {
    o["loopPeriodSec"] = c.loop_anchor->period_seconds;
    o["loopPhaseSec"] = c.loop_anchor->phase_seconds;
  }
  // Optional; only emitted when set so existing projects round-trip byte-for-byte.
  if (c.loop_crossfade_ppq != 0.0) o["loopCrossfadePpq"] = c.loop_crossfade_ppq;
  o["warpRefId"] = static_cast<double>(c.warp_ref_id);
  o["warpMode"] = enum_token(c.warp_mode);
  if (!c.takes.empty()) {
    Array takes;
    for (const auto& take : c.takes) {
      takes.push_back(take_to_json(take));
    }
    o["takes"] = std::move(takes);
  }
  if (c.active_take_id != 0) {
    o["activeTakeId"] = static_cast<double>(c.active_take_id);
  }
  if (!c.comp_segments.empty()) {
    Array segments;
    for (const auto& segment : c.comp_segments) {
      segments.push_back(comp_segment_to_json(segment));
    }
    o["compSegments"] = std::move(segments);
  }
  if (!c.comp_render_parts.empty()) {
    Array parts;
    for (const auto& part : c.comp_render_parts) {
      parts.push_back(comp_render_part_to_json(part));
    }
    o["compRenderParts"] = std::move(parts);
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
    ao["warpSample"] = anchor.warp_sample;
    ao["sourceSample"] = anchor.source_sample;
    anchors.push_back(std::move(ao));
  }
  o["anchors"] = std::move(anchors);
  return o;
}

Value source_to_json(const arrangement::ClipSource& src) {
  Object o;
  if (const auto* audio = std::get_if<arrangement::AudioSourceRef>(&src)) {
    o["kind"] = enum_token(arrangement::SourceKind::kAudio);
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
    o["channelCount"] = static_cast<double>(audio->channel_count);
    o["sampleRateHint"] = audio->sample_rate_hint;
    o["storageHandleId"] = static_cast<double>(audio->storage_handle_id);
    // Additive + optional: only emit when set so existing projects without a
    // content hash serialize byte-for-byte identically (no golden churn).
    if (!audio->content_hash.empty()) {
      o["contentHash"] = audio->content_hash;
    }
    if (!audio->external_stem_role.empty()) {
      o["externalStemRole"] = audio->external_stem_role;
    }
  } else {
    const auto& m = std::get<arrangement::MidiSourceRef>(src);
    o["kind"] = enum_token(arrangement::SourceKind::kMidi);
    o["id"] = static_cast<double>(m.id);
    o["name"] = m.name;
    o["channelHint"] = static_cast<double>(m.channel_hint);
  }
  return o;
}

Value marker_to_json(const arrangement::ProjectMarker& m) {
  Object o;
  o["id"] = static_cast<double>(m.id);
  o["ppq"] = m.ppq;
  o["name"] = m.name;
  // The default marker kind is omitted. Preserve nonzero key fields from older
  // documents even on non-key kinds, so loading and saving keeps their data.
  if (m.kind != static_cast<uint8_t>(arrangement::MarkerKind::kMarker)) {
    o["kind"] = enum_token(static_cast<arrangement::MarkerKind>(m.kind));
  }
  if (m.kind == static_cast<uint8_t>(arrangement::MarkerKind::kKeySignature) || m.key_fifths != 0 ||
      m.key_minor) {
    o["keyFifths"] = static_cast<double>(m.key_fifths);
    o["keyMinor"] = m.key_minor;
  }
  return o;
}

Value chord_to_json(const arrangement::ChordSymbol& c) {
  Object o;
  o["startPpq"] = c.start_ppq;
  o["endPpq"] = c.end_ppq;
  o["rootPc"] = static_cast<double>(c.root_pc);
  o["quality"] = enum_token(c.quality);
  Array ext;
  for (uint8_t e : c.extensions) ext.push_back(static_cast<double>(e));
  o["extensions"] = std::move(ext);
  o["slashBassPc"] = static_cast<double>(c.slash_bass_pc);
  o["romanNumeral"] = c.roman_numeral;
  o["modulationBoundary"] = c.modulation_boundary;
  return o;
}

Value key_segment_to_json(const arrangement::KeySegment& k) {
  Object o;
  o["startPpq"] = k.start_ppq;
  o["endPpq"] = k.end_ppq;
  o["tonicPc"] = static_cast<double>(k.tonic_pc);
  o["mode"] = enum_token(k.mode);
  return o;
}

Value annotation_to_json(const arrangement::ProjectAnnotation& a) {
  Object o;
  o["tempoConfidence"] = a.tempo_confidence;
  Array keys;
  for (const auto& k : a.keys) keys.push_back(key_segment_to_json(k));
  o["keys"] = std::move(keys);
  Array chords;
  for (const auto& c : a.chords) chords.push_back(chord_to_json(c));
  o["chords"] = std::move(chords);
  Array sections;
  for (const auto& s : a.sections) {
    Object so;
    so["startPpq"] = s.start_ppq;
    so["endPpq"] = s.end_ppq;
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
  o["moduleId"] = s.module_id;
  o["schemaVersion"] = static_cast<double>(s.schema_version);
  // Opaque payload: base64 so arbitrary bytes round-trip verbatim regardless of
  // whether the owning module is registered or the schema_version is known.
  o["payloadB64"] = base64_encode(s.payload);
  o["targetTrackId"] = static_cast<double>(s.target_track_id);
  o["regionStartPpq"] = s.region_start_ppq;
  o["regionEndPpq"] = s.region_end_ppq;
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
      eo["word0"] = static_cast<double>(e.word0);
      eo["word1"] = static_cast<double>(e.word1);
      if (e.sysex_handle != 0) {
        eo["sysexHandle"] = static_cast<double>(e.sysex_handle);
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
    o["__sysexPayloads"] = std::move(payloads);
  }
  return o;
}

namespace {

// The scene params bag is admitted on its own before it is attached to the
// scene object. That standalone depth can fit while the surrounding scene
// wrappers put it over the parser's limit. Such a scene takes the dump/parse
// path so it fails with the same parser error a standalone scene dump gives;
// ordinary scenes stay as one in-memory Value.
bool exceeds_default_json_depth(const Value& value, std::size_t depth) {
  if (depth > json::kDefaultMaxDepth) return true;
  if (value.is_array()) {
    for (const auto& child : value.as_array()) {
      if (exceeds_default_json_depth(child, depth + 1)) return true;
    }
  } else if (value.is_object()) {
    for (const auto& [key, child] : value.as_object()) {
      (void)key;
      if (exceeds_default_json_depth(child, depth + 1)) return true;
    }
  }
  return false;
}

}  // namespace

Value scene_to_value(const mixing::api::Scene& scene) {
  Value value = mixing::api::detail::scene_to_value(scene);
  if (exceeds_default_json_depth(value, 0)) {
    return json::parse(json::dump(value));
  }
  return value;
}

}  // namespace detail
}  // namespace sonare::serialize
