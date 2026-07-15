// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include "serialize/project_serializer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "arrangement/edit_source.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer_internal.h"
#include "transport/tempo_map.h"
#include "util/exception.h"
#include "util/json.h"
#include "util/numeric_validation.h"

namespace sonare::serialize {
namespace {

namespace json = sonare::util::json;
using json::Array;
using json::Object;
using json::Value;

using namespace detail;

constexpr double kMinProjectSampleRate = 8000.0;
constexpr double kMaxProjectSampleRate = 384000.0;

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
    uint32_t schema_version = 0;
    if (!numeric::checked_integral_cast(version_d, &schema_version)) {
      result.diagnostics.push_back({DiagnosticSeverity::kError, "invalid_version",
                                    "schema version must be a non-negative uint32 integer"});
      return result;
    }
    if (!schema_version_supported(schema_version)) {
      result.diagnostics.push_back({DiagnosticSeverity::kError, "unsupported_schema_version",
                                    "schema version " + std::to_string(schema_version) +
                                        " is newer than supported " +
                                        std::to_string(SONARE_PROJECT_SCHEMA_VERSION)});
      return result;
    }

    arrangement::Project project;
    const auto invalid_entity_id = [](uint32_t id) {
      return id == 0 || id == std::numeric_limits<uint32_t>::max();
    };
    const auto reject_entity_id = [&](const char* entity, uint32_t id, bool duplicate) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, duplicate ? "duplicate_entity_id" : "invalid_entity_id",
           std::string(entity) + " id " + std::to_string(id) +
               (duplicate ? " is duplicated" : " is reserved or out of range")});
    };
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
    const uint32_t overlap_policy = uint_or(root, "overlap_policy", 0);
    if (overlap_policy > static_cast<uint32_t>(arrangement::OverlapPolicy::kAllow)) {
      throw SonareException(ErrorCode::InvalidFormat, "overlap_policy enum is out of range");
    }
    project.set_overlap_policy(static_cast<arrangement::OverlapPolicy>(overlap_policy));

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
        if (invalid_entity_id(sid)) {
          reject_entity_id("source", sid, false);
          return result;
        }
        if (sid > max_source_id) max_source_id = sid;
        if (!project.insert_source_raw(std::move(src))) {
          reject_entity_id("source", sid, true);
          return result;
        }
      }
    }
    if (max_source_id > 0) project.ensure_next_source_id(max_source_id);

    // Tracks.
    uint32_t max_track_id = 0;
    if (const auto* arr = array_at(root, "tracks")) {
      for (const auto& tv : *arr) {
        if (!tv.is_object()) continue;
        arrangement::Track t = track_from_json(tv);
        if (invalid_entity_id(t.id)) {
          reject_entity_id("track", t.id, false);
          return result;
        }
        if (t.id > max_track_id) max_track_id = t.id;
        const uint32_t id = t.id;
        if (!project.insert_track_raw(std::move(t))) {
          reject_entity_id("track", id, true);
          return result;
        }
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
        if (invalid_entity_id(c.id)) {
          reject_entity_id("clip", c.id, false);
          return result;
        }
        if (c.id > max_clip_id) max_clip_id = c.id;
        const uint32_t id = c.id;
        if (!project.insert_clip_raw(std::move(c))) {
          reject_entity_id("clip", id, true);
          return result;
        }
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
        const uint32_t marker_kind = uint_or(mv, "kind", 0);
        if (marker_kind > 4) {
          throw SonareException(ErrorCode::InvalidFormat, "marker kind enum is out of range");
        }
        m.kind = static_cast<uint8_t>(marker_kind);
        // key_fifths/key_minor are written only for the key-signature kind (4)
        // but read unconditionally: non-key markers simply default to 0/false
        // (their correct zero values), so the round-trip is lossless. Benign
        // today; keep in mind if a non-key marker ever gains meaningful key
        // fields, in which case the write side must serialize them too.
        m.key_fifths = int8_or(mv, "key_fifths", 0);
        if (m.key_fifths < -7 || m.key_fifths > 7) {
          throw SonareException(ErrorCode::InvalidFormat, "key_fifths must be within [-7, 7]");
        }
        m.key_minor = bool_or(mv, "key_minor", false);
        if (invalid_entity_id(m.id)) {
          reject_entity_id("marker", m.id, false);
          return result;
        }
        if (std::any_of(
                project.markers().begin(), project.markers().end(),
                [&](const arrangement::ProjectMarker& existing) { return existing.id == m.id; })) {
          reject_entity_id("marker", m.id, true);
          return result;
        }
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
  } catch (const SonareException& e) {
    result.project.reset();
    result.diagnostics.push_back(
        {DiagnosticSeverity::kError,
         e.code() == ErrorCode::InvalidFormat ? "invalid_format" : "deserialize_failed", e.what()});
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
