// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include "serialize/project_serializer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "arrangement/edit_command_internal.h"
#include "arrangement/edit_source.h"
#include "core/audio.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer_internal.h"
#include "serialize/project_serializer_private.h"
#include "serialize/serialized_enum_bounds.h"
#include "transport/tempo_map.h"
#include "util/base64.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json_budget.h"
#include "util/json_key.h"
#include "util/json_schema.h"
#include "util/numeric_validation.h"
#include "util/resource_limits.h"

namespace sonare::serialize {
namespace {

namespace json = sonare::util::json;
using json::Array;
using json::Object;
using json::Value;
namespace schema = json::schema;

using namespace detail;

using midi::kPartRigModeNames;
using sonare::constants::kDefaultDawSampleRate;

Value part_rig_to_json(const arrangement::ProjectPartRig& entry) {
  Object o;
  o["destinationId"] = static_cast<double>(entry.destination_id);
  o["part"] = static_cast<double>(entry.part);
  o["mode"] = std::string(kPartRigModeNames[static_cast<size_t>(entry.rig.mode)]);
  if (entry.rig.mode == midi::PartRigMode::kChain) {
    Array inserts;
    for (const auto& stage : entry.rig.stages) {
      Object so;
      so["processor"] = stage.processor;
      so["params"] = json::parse(stage.params_json);
      inserts.push_back(std::move(so));
    }
    o["inserts"] = std::move(inserts);
  }
  return o;
}

// Reads one partRigs entry at @p path, refusing a part or mode no rig can take. Whether the
// stages fit the mode, and whether a chain can be built, is Project::set_part_rig's to refuse.
arrangement::ProjectPartRig part_rig_from_json(const Value& v, const std::string& path) {
  arrangement::ProjectPartRig entry;
  entry.destination_id = uint_or(v, path, "destinationId", 0);
  uint8_t part = 0;
  if (!numeric::checked_integral_cast(num_or(v, path, "part", 0.0), &part)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::refusal(schema::child_path(path, "part"),
                                          "must be 0 to 15, or 255 for the destination default"));
  }
  entry.part = part;
  const std::string mode = str_or(v, path, "mode", "");
  const auto* found = std::find(std::begin(kPartRigModeNames), std::end(kPartRigModeNames), mode);
  if (found == std::end(kPartRigModeNames)) {
    throw SonareException(
        ErrorCode::InvalidFormat,
        schema::refusal(schema::child_path(path, "mode"), "must be one of bank, none, chain"));
  }
  entry.rig.mode = static_cast<midi::PartRigMode>(found - std::begin(kPartRigModeNames));
  if (const auto* inserts = array_at(v, path, "inserts")) {
    const std::string inserts_path = schema::child_path(path, "inserts");
    std::size_t index = 0;
    for (const auto& sv : *inserts) {
      const std::string insert_path = schema::index_path(inserts_path, index++);
      const Value& stage = object_element(sv, insert_path);
      if (stage.find("processor") == nullptr) {
        throw SonareException(ErrorCode::InvalidFormat,
                              schema::missing_key(schema::child_path(insert_path, "processor")));
      }
      const Value* params = object_value_at(stage, insert_path, "params");
      entry.rig.stages.push_back({str_or(stage, insert_path, "processor", ""),
                                  params != nullptr ? json::dump(*params) : std::string("{}")});
    }
  }
  return entry;
}

using KeyList = std::initializer_list<const char*>;

// Refuses the first key outside the record's accepted fields. Annotation keys are exempt.
void refuse_unknown_keys(const Value& object, const std::string& path, KeyList known) {
  const auto is_known = [&known](const std::string& key) {
    return std::any_of(known.begin(), known.end(),
                       [&key](const char* candidate) { return key == candidate; });
  };
  for (const auto& entry : object.as_object()) {
    const std::string& key = entry.first;
    if (json::is_annotation_key(key) || is_known(key)) continue;
    throw SonareException(
        ErrorCode::InvalidFormat,
        schema::unknown_key_expected(
            schema::child_path(path, key),
            schema::expected_keys(std::vector<std::string>(known.begin(), known.end()))));
  }
}

// Calls @p check on each object element of the array at @p key. A container or element of
// the wrong type is left to the decoder, which refuses it with its own message.
template <typename Check>
void each_record(const Value& parent, const char* key, const std::string& path, Check check) {
  const Value* array = parent.find(key);
  if (array == nullptr || !array->is_array()) return;
  const auto& elements = array->as_array();
  for (std::size_t index = 0; index < elements.size(); ++index) {
    if (!elements[index].is_object()) continue;
    check(elements[index], schema::index_path(schema::child_path(path, key), index));
  }
}

template <typename Check>
void with_object(const Value& parent, const char* key, const std::string& path, Check check) {
  const Value* object = parent.find(key);
  if (object != nullptr && object->is_object()) check(*object, schema::child_path(path, key));
}

void check_clip_keys(const Value& clip, const std::string& path) {
  refuse_unknown_keys(clip, path,
                      {"id",
                       "trackId",
                       "sourceId",
                       "startPpq",
                       "lengthPpq",
                       "sourceOffsetPpq",
                       "sourceOffsetSec",
                       "gain",
                       "fadeIn",
                       "fadeOut",
                       "loopMode",
                       "loopLengthPpq",
                       "loopCrossfadePpq",
                       "loopPeriodSec",
                       "loopPhaseSec",
                       "warpRefId",
                       "warpMode",
                       "takes",
                       "activeTakeId",
                       "compSegments",
                       "compRenderParts"});
  for (const char* fade : {"fadeIn", "fadeOut"}) {
    with_object(clip, fade, path, [](const Value& v, const std::string& p) {
      refuse_unknown_keys(v, p, {"lengthPpq", "curve"});
    });
  }
  each_record(clip, "takes", path, [](const Value& v, const std::string& p) {
    refuse_unknown_keys(
        v, p, {"id", "sourceId", "sourceOffsetPpq", "name", "sourceOffsetSec", "partial"});
  });
  each_record(clip, "compSegments", path, [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p, {"startPpq", "endPpq", "takeId", "crossfadePpq"});
  });
  each_record(clip, "compRenderParts", path, [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p,
                        {"visibleStartPpq", "visibleEndPpq", "referenceStartPpq", "referenceEndPpq",
                         "takeId", "seamFadeInPpq", "seamFadeOutPpq", "retainedOnly"});
  });
}

void check_annotation_keys(const Value& annotation, const std::string& path) {
  refuse_unknown_keys(annotation, path,
                      {"tempoConfidence", "keys", "chords", "sections", "onsets"});
  each_record(annotation, "keys", path, [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p, {"startPpq", "endPpq", "tonicPc", "mode"});
  });
  each_record(annotation, "chords", path, [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p,
                        {"startPpq", "endPpq", "rootPc", "quality", "extensions", "slashBassPc",
                         "romanNumeral", "modulationBoundary"});
  });
  each_record(annotation, "sections", path, [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p, {"startPpq", "endPpq", "label"});
  });
  each_record(annotation, "onsets", path, [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p, {"ppq", "confidence"});
  });
}

// The MIDI content store is a map keyed by clip id (plus the SysEx payload table keyed by
// handle), so the fixed field lists apply to its event records.
void check_midi_content_keys(const Value& content, const std::string& path) {
  for (const auto& [clip_key, events] : content.as_object()) {
    if (clip_key == "__sysex_payloads") {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::unknown_key_expected(
                                schema::child_path(path, clip_key),
                                schema::expected_keys({"__sysexPayloads", "uint32 clip id"})));
    }
    if (clip_key == "__sysexPayloads" || !events.is_array()) continue;
    const auto& elements = events.as_array();
    for (std::size_t index = 0; index < elements.size(); ++index) {
      const Value& event = elements[index];
      if (!event.is_object()) continue;
      const std::string event_path = schema::index_path(schema::child_path(path, clip_key), index);
      refuse_unknown_keys(event, event_path, {"ppq", "word0", "word1", "sysexHandle"});
    }
  }
}

void check_destination_instrument_keys(const Value& instrument, const std::string& path) {
  refuse_unknown_keys(instrument, path,
                      {"destinationId", "kind", "synth", "builtin", "sf2", "bank", "program"});
  with_object(instrument, "synth", path, [](const Value& synth, const std::string& synth_path) {
    refuse_unknown_keys(
        synth, synth_path,
        {"preset", "engineMode", "fields", "engineParams", "modRoutings", "useGmPrograms"});
    each_record(synth, "modRoutings", synth_path,
                [](const Value& route, const std::string& route_path) {
                  refuse_unknown_keys(route, route_path, {"source", "destination", "depth"});
                });
  });
  with_object(
      instrument, "builtin", path, [](const Value& builtin, const std::string& builtin_path) {
        refuse_unknown_keys(
            builtin, builtin_path,
            {"waveform", "gain", "attackMs", "decayMs", "sustain", "releaseMs", "polyphony"});
      });
  with_object(instrument, "sf2", path, [](const Value& sf2, const std::string& sf2_path) {
    refuse_unknown_keys(sf2, sf2_path,
                        {"gain", "polyphony", "preferModelForModeledFamilies", "clearBankRig",
                         "gsEfxRealization", "soundFontUri"});
  });
  with_object(instrument, "bank", path, [](const Value& bank, const std::string& bank_path) {
    refuse_unknown_keys(bank, bank_path, {"msb", "lsb"});
  });
}

// Refuses a key no record of the document reads, before any of the model is built. The scene
// subtree is checked by the scene reader itself, and insert `params` bags are not walked.
void check_project_keys(const Value& root) {
  refuse_unknown_keys(root, "",
                      {"version", "sampleRate", "overlapPolicy", "tempoSegments", "timeSignatures",
                       "sources", "tracks", "clips", "warpMaps", "partRigs", "markers",
                       "annotation", "midiContent", "scene", "instruments", "assistSidecars"});
  each_record(root, "tempoSegments", "", [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p, {"startPpq", "bpm", "startSample", "endBpm"});
  });
  each_record(root, "timeSignatures", "", [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p, {"startPpq", "numerator", "denominator"});
  });
  each_record(root, "sources", "", [](const Value& v, const std::string& p) {
    const bool midi =
        enum_or(v, p, "kind", arrangement::SourceKind::kAudio) == arrangement::SourceKind::kMidi;
    if (midi) {
      refuse_unknown_keys(v, p, {"kind", "id", "name", "channelHint"});
    } else {
      refuse_unknown_keys(v, p,
                          {"kind", "id", "uri", "channelCount", "sampleRateHint", "storageHandleId",
                           "contentHash", "externalStemRole"});
    }
  });
  each_record(root, "tracks", "", [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p,
                        {"id", "name", "kind", "gain", "mute", "solo", "pan", "channelStripRef",
                         "outputTarget", "midiDestinationId", "automationLanes"});
    each_record(v, "automationLanes", p, [](const Value& lane, const std::string& lane_path) {
      refuse_unknown_keys(lane, lane_path,
                          {"id", "targetKind", "targetParamId", "sendId", "insertIndex",
                           "processor", "paramName", "points"});
      each_record(lane, "points", lane_path, [](const Value& point, const std::string& pp) {
        refuse_unknown_keys(point, pp, {"ppq", "value", "curveToNext"});
      });
    });
  });
  each_record(root, "clips", "", check_clip_keys);
  each_record(root, "warpMaps", "", [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p, {"id", "name", "anchors"});
    each_record(v, "anchors", p, [](const Value& anchor, const std::string& ap) {
      refuse_unknown_keys(anchor, ap, {"warpSample", "sourceSample"});
    });
  });
  each_record(root, "partRigs", "", [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p, {"destinationId", "part", "mode", "inserts"});
    each_record(v, "inserts", p, [](const Value& insert, const std::string& ip) {
      refuse_unknown_keys(insert, ip, {"processor", "params"});
    });
  });
  each_record(root, "instruments", "", check_destination_instrument_keys);
  each_record(root, "markers", "", [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p, {"id", "ppq", "name", "kind", "keyFifths", "keyMinor"});
  });
  with_object(root, "annotation", "", check_annotation_keys);
  with_object(root, "midiContent", "", check_midi_content_keys);
  each_record(root, "assistSidecars", "", [](const Value& v, const std::string& p) {
    refuse_unknown_keys(v, p,
                        {"moduleId", "schemaVersion", "targetTrackId", "regionStartPpq",
                         "regionEndPpq", "payloadB64"});
  });
}

bool count_array_entities(const Value& value, std::size_t limit, std::size_t* total) {
  if (value.is_array()) {
    if (!resource::bounded_accumulate(value.as_array().size(), limit, total)) return false;
    for (const auto& child : value.as_array()) {
      if (!count_array_entities(child, limit, total)) return false;
    }
  } else if (value.is_object()) {
    for (const auto& [key, child] : value.as_object()) {
      (void)key;
      if (!count_array_entities(child, limit, total)) return false;
    }
  }
  return true;
}

// Mirrors the parser's accounting on the encoder's own tree: one node per JSON
// value (a key is not a value, so it is not a node) and cumulative decoded bytes
// for every key and string value.
void count_document_nodes(const Value& value, ProjectDocumentShape* shape) {
  ++shape->json_nodes;
  if (value.is_string()) {
    shape->string_bytes += value.as_string().size();
  } else if (value.is_array()) {
    for (const auto& child : value.as_array()) count_document_nodes(child, shape);
  } else if (value.is_object()) {
    for (const auto& [key, child] : value.as_object()) {
      shape->string_bytes += key.size();
      count_document_nodes(child, shape);
    }
  }
}

// Payload bytes the decoder accumulates for this model. Its per-entry charge is
// the base64 decoded upper bound, which equals the exact payload size for the
// well-formed base64 the encoder writes.
std::size_t model_decoded_payload_bytes(const arrangement::Project& project,
                                        const arrangement::MidiContentStore& midi) {
  std::size_t total = 0;
  for (const arrangement::AssistSidecar& sidecar : project.assist_sidecars()) {
    total += sidecar.payload.size();
  }
  for (const auto& [handle, payload] : midi.sysex_payloads) {
    (void)handle;
    total += payload.size();
  }
  return total;
}

std::size_t base64_decoded_upper_bound(const std::string& text) noexcept {
  std::size_t decoded_size = (text.size() / 4u) * 3u;
  // Guard each decrement: a malformed short payload (e.g. "==") has a zero base
  // estimate, and an unguarded `--` would wrap to SIZE_MAX and misclassify the
  // rejection as a resource-limit overflow instead of a plain format error.
  if (decoded_size > 0u && !text.empty() && text.back() == '=') --decoded_size;
  if (decoded_size > 0u && text.size() >= 2u && text[text.size() - 2u] == '=') --decoded_size;
  return decoded_size;
}

// Builds the document tree. The two public entry points below share it so the
// measured shape is always the shape of the bytes that would be emitted.
Value project_to_value(const arrangement::Project& project,
                       const arrangement::MidiContentStore& midi) {
  Object root;
  root["version"] = static_cast<double>(SONARE_PROJECT_SCHEMA_VERSION);
  root["sampleRate"] = project.sample_rate();
  root["overlapPolicy"] = enum_token(project.overlap_policy());

  Array tempo;
  for (const auto& s : project.tempo_segments()) tempo.push_back(tempo_segment_to_json(s));
  root["tempoSegments"] = std::move(tempo);

  Array sigs;
  for (const auto& s : project.time_signatures()) sigs.push_back(time_signature_to_json(s));
  root["timeSignatures"] = std::move(sigs);

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
  root["warpMaps"] = std::move(warp_maps);

  if (!project.part_rigs().empty()) {
    Array part_rigs;
    for (const auto& entry : project.part_rigs()) part_rigs.push_back(part_rig_to_json(entry));
    root["partRigs"] = std::move(part_rigs);
  }

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
  root["midiContent"] = midi_content_to_json(midi);
  if (const auto* scene = project.find_scene()) root["scene"] = scene_to_value(*scene);

  Array instruments;
  std::size_t instrument_index = 0;
  for (const auto& [destination_id, binding] : project.destination_instruments()) {
    if (const auto* synth =
            std::get_if<arrangement::DestinationSynthInstrument>(&binding.instrument);
        synth != nullptr && synth->engine_mode == "sample") {
      const std::string field_path = schema::child_path(
          schema::child_path(schema::index_path("instruments", instrument_index), "synth"),
          "engineMode");
      throw SonareException(ErrorCode::NotSupported,
                            schema::refusal(field_path, "cannot be persisted"),
                            ErrorReason::NotPersistable);
    }
    instruments.push_back(destination_instrument_to_json(destination_id, binding));
    ++instrument_index;
  }
  root["instruments"] = std::move(instruments);

  Array sidecars;
  for (const auto& s : project.assist_sidecars()) sidecars.push_back(sidecar_to_json(s));
  root["assistSidecars"] = std::move(sidecars);

  return Value(std::move(root));
}

// Shape of an already-built document, minus the byte count (which only the dump
// can supply).
ProjectDocumentShape measure_document(const Value& document, const arrangement::Project& project,
                                      const arrangement::MidiContentStore& midi) {
  ProjectDocumentShape shape;
  count_document_nodes(document, &shape);
  // Counting only: the unlimited budget makes the accumulate step infallible.
  (void)count_array_entities(document, std::numeric_limits<std::size_t>::max(), &shape.entities);
  shape.decoded_payload_bytes = model_decoded_payload_bytes(project, midi);
  return shape;
}

constexpr const char* kOverBudgetMessage =
    "encoded project exceeds the persistence budget and could not be loaded back";

/// Orders @p segments by start_ppq and keeps only the last segment of each tick.
template <typename Segment>
void sort_and_dedupe_by_start_ppq(std::vector<Segment>* segments) {
  transport::sort_by_start(segments);
  std::vector<Segment> deduped;
  deduped.reserve(segments->size());
  for (Segment& s : *segments) {
    if (!deduped.empty() && deduped.back().start_ppq == s.start_ppq) {
      deduped.back() = s;
    } else {
      deduped.push_back(s);
    }
  }
  *segments = std::move(deduped);
}

}  // namespace

// ===========================================================================
// Public: serialize
// ===========================================================================

ProjectDocumentShape measure_project_document(const arrangement::Project& project,
                                              const arrangement::MidiContentStore& midi) {
  const Value document = project_to_value(project, midi);
  ProjectDocumentShape shape = measure_document(document, project, midi);
  shape.json_bytes = json::dump(document).size();
  return shape;
}

bool try_project_to_json(const arrangement::Project& project,
                         const arrangement::MidiContentStore& midi, std::string* out_json,
                         const resource::ProjectImportResourceLimits& limits) {
  if (out_json == nullptr) return false;
  out_json->clear();
  // The opaque payloads are a property of the model, so their budget is checked
  // before the document (and the base64 expansion of those payloads) is built.
  if (model_decoded_payload_bytes(project, midi) > limits.max_decoded_payload_bytes) return false;
  const Value document = project_to_value(project, midi);
  // The dump is the largest remaining allocation on this path, so the counted
  // budgets are checked on the tree first; passing 0 bytes here trivially
  // satisfies the byte budget, which is checked against the dumped text below.
  const ProjectDocumentShape shape = measure_document(document, project, midi);
  if (!resource::project_document_fits(0u, shape.json_nodes, shape.entities, shape.string_bytes,
                                       shape.decoded_payload_bytes, limits)) {
    return false;
  }
  std::string text = json::dump(document);
  if (text.size() > limits.max_json_bytes) return false;
  *out_json = std::move(text);
  return true;
}

std::string project_to_json(const arrangement::Project& project,
                            const arrangement::MidiContentStore& midi,
                            const resource::ProjectImportResourceLimits& limits) {
  std::string text;
  if (!try_project_to_json(project, midi, &text, limits)) {
    throw SonareException(ErrorCode::InvalidState, kOverBudgetMessage);
  }
  return text;
}

// ===========================================================================
// Public: deserialize
// ===========================================================================

DeserializeResult project_from_json(const std::string& json_text) {
  DeserializeResult result;

  const auto& import_limits = resource::kDefaultProjectImportResourceLimits;

  Value root;
  try {
    root = json::admit_strict(json_text, import_limits);
  } catch (const json::JsonResourceError& e) {
    result.diagnostics.push_back({DiagnosticSeverity::kError, "resource_limit_exceeded", e.what()});
    return result;
  } catch (const json::JsonError& e) {
    result.reason = ErrorReason::MalformedJson;
    result.diagnostics.push_back({DiagnosticSeverity::kError, "malformed_json", e.what()});
    return result;
  } catch (...) {
    // Defensive: parse() only throws JsonError, but never let anything escape.
    result.reason = ErrorReason::MalformedJson;
    result.diagnostics.push_back(
        {DiagnosticSeverity::kError, "malformed_json", "unknown parse failure"});
    return result;
  }

  try {
    if (!root.is_object()) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, "not_an_object", schema::root_must_be_object()});
      return result;
    }

    std::size_t entity_count = 0;
    if (!count_array_entities(root, import_limits.max_entities, &entity_count)) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, "resource_limit_exceeded", "project entity limit exceeded"});
      return result;
    }

    const auto* version = root.find("version");
    if (!version || !version->is_number()) {
      result.diagnostics.push_back({DiagnosticSeverity::kError, "missing_version",
                                    version == nullptr
                                        ? schema::missing_key("version")
                                        : schema::must_be("version", schema::Kind::Number)});
      return result;
    }
    const double version_d = version->as_number();
    uint32_t schema_version = 0;
    if (!numeric::checked_integral_cast(version_d, &schema_version)) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, "invalid_version",
           schema::refusal("version", schema::integer_range_predicate(
                                          0, std::numeric_limits<uint32_t>::max()))});
      return result;
    }
    if (schema_version != SONARE_PROJECT_SCHEMA_VERSION) {
      result.reason = ErrorReason::UnsupportedSchemaVersion;
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, "unsupported_schema_version",
           "project schema version " + std::to_string(schema_version) + " is " +
               (schema_version < SONARE_PROJECT_SCHEMA_VERSION ? "older" : "newer") +
               " than the version this release reads (" +
               std::to_string(SONARE_PROJECT_SCHEMA_VERSION) + ")"});
      return result;
    }

    check_project_keys(root);

    arrangement::Project project;
    const auto reject_payload_limit = [&]() {
      result.diagnostics.push_back({DiagnosticSeverity::kError, "resource_limit_exceeded",
                                    "project decoded payload limit exceeded"});
    };
    const auto reject_entity_id = [&](const std::string& element_path, bool duplicate) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, duplicate ? "duplicate_entity_id" : "invalid_entity_id",
           schema::refusal(schema::child_path(element_path, "id"),
                           duplicate ? "must be unique" : "must not be 0 or 4294967295")});
    };
    // Default matches arrangement::Project's constructor default (48 kHz, the
    // conventional DAW render rate) so a document that omits "sampleRate"
    // round-trips to the same rate an in-memory project would have.
    const double sample_rate = num_or(root, "", "sampleRate", kDefaultDawSampleRate);
    if (!is_supported_sample_rate(sample_rate)) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, "invalid_sample_rate",
           schema::refusal("sampleRate", schema::integer_range_predicate(kMinAudioSampleRate,
                                                                         kMaxAudioSampleRate))});
      return result;
    }
    project.set_sample_rate(sample_rate);
    project.set_overlap_policy(
        enum_or(root, "", "overlapPolicy", arrangement::OverlapPolicy::kDisallow));

    // Tempo / time signature. Segments are validated (finite, in-domain BPM and
    // start_ppq) and normalized on load: a segment with an out-of-domain BPM or
    // a non-finite start_ppq is rejected with a diagnostic rather than
    // propagated; the surviving segments are sorted by start_ppq and de-duped on
    // start_ppq (last writer wins) so the in-memory map is well-ordered.
    std::vector<transport::TempoSegment> tempo;
    if (const auto* arr = array_at(root, "", "tempoSegments")) {
      size_t index = 0;
      for (const auto& sv : *arr) {
        const std::string at = schema::index_path("tempoSegments", index);
        transport::TempoSegment s = tempo_segment_from_json(object_element(sv, at), at);
        if (!std::isfinite(s.start_ppq)) {
          result.diagnostics.push_back(
              {DiagnosticSeverity::kError, "invalid_tempo_start_ppq",
               schema::refusal(schema::child_path(at, "startPpq"), "must be finite")});
          return result;
        }
        try {
          transport::check_public_tempo_segment(s, index, "tempoSegments", "endBpm");
        } catch (const SonareParameterRefusal& e) {
          const bool is_start = e.subject() == schema::child_path(at, "bpm");
          const double bpm = is_start ? s.bpm : s.end_bpm;
          const char* diagnostic = std::isfinite(bpm) && bpm > transport::kMaxPublicTempoBpm
                                       ? "invalid_tempo_segment"
                                   : is_start ? "invalid_tempo_bpm"
                                              : "invalid_tempo_end_bpm";
          result.diagnostics.push_back({DiagnosticSeverity::kError, diagnostic, e.what()});
          return result;
        }
        if (!std::isfinite(s.start_sample)) s.start_sample = 0.0;
        tempo.push_back(s);
        ++index;
      }
    }
    // The last segment for a tick wins, so the map has a single segment per tick.
    sort_and_dedupe_by_start_ppq(&tempo);
    project.set_tempo_segments(std::move(tempo));

    std::vector<transport::TimeSignatureSegment> sigs;
    if (const auto* arr = array_at(root, "", "timeSignatures")) {
      size_t index = 0;
      for (const auto& sv : *arr) {
        const std::string at = schema::index_path("timeSignatures", index);
        transport::TimeSignatureSegment s = time_signature_from_json(object_element(sv, at), at);
        if (!std::isfinite(s.start_ppq)) {
          result.diagnostics.push_back(
              {DiagnosticSeverity::kError, "invalid_time_signature_start_ppq",
               schema::refusal(schema::child_path(at, "startPpq"), "must be finite")});
          return result;
        }
        if (s.time_sig.numerator <= 0 || s.time_sig.denominator <= 0) {
          const char* bad = s.time_sig.numerator <= 0 ? "numerator" : "denominator";
          result.diagnostics.push_back(
              {DiagnosticSeverity::kError, "invalid_time_signature",
               schema::refusal(schema::child_path(at, bad), "must be positive")});
          return result;
        }
        sigs.push_back(s);
        ++index;
      }
    }
    // Same rule as tempo loading and the edit model's strictly increasing timeline invariant.
    sort_and_dedupe_by_start_ppq(&sigs);
    project.set_time_signatures(std::move(sigs));

    // Sources (insert verbatim, preserving ids, then bump the id counters so a
    // later add_* allocates a fresh non-colliding id).
    uint32_t max_source_id = 0;
    std::unordered_set<arrangement::SourceId> source_ids;
    if (const auto* arr = array_at(root, "", "sources")) {
      std::size_t index = 0;
      for (const auto& sv : *arr) {
        const std::string at = schema::index_path("sources", index++);
        arrangement::ClipSource src = source_from_json(object_element(sv, at), at);
        const arrangement::SourceId sid = arrangement::source_id(src);
        if (!valid_entity_id(sid)) {
          reject_entity_id(at, false);
          return result;
        }
        if (sid > max_source_id) max_source_id = sid;
        if (!source_ids.insert(sid).second ||
            !project.insert_source_raw(std::move(src), arrangement::Project::kAppend, true)) {
          reject_entity_id(at, true);
          return result;
        }
      }
    }
    if (max_source_id > 0) project.ensure_next_source_id(max_source_id);

    // Per-entry decode warnings, malformed warp/key entries, and normalization
    // warnings share one bounded sink so a hostile document cannot make
    // diagnostic reporting a far larger allocation than the input; finalize it
    // just before the successful return. Fatal errors bypass this and abort the
    // load directly.
    BoundedDiagnostics decode_diagnostics(&result.diagnostics);

    // Tracks.
    uint32_t max_track_id = 0;
    uint32_t max_automation_lane_id = 0;
    std::unordered_set<arrangement::TrackId> track_ids;
    std::unordered_set<uint32_t> automation_lane_ids;
    if (const auto* arr = array_at(root, "", "tracks")) {
      std::size_t index = 0;
      for (const auto& tv : *arr) {
        const std::size_t track_index = index++;
        const std::string at = schema::index_path("tracks", track_index);
        arrangement::Track t = track_from_json(object_element(tv, at), at);
        if (!valid_entity_id(t.id)) {
          reject_entity_id(at, false);
          return result;
        }
        if (t.id > max_track_id) max_track_id = t.id;
        const uint32_t id = t.id;
        if (!track_ids.insert(id).second ||
            !project.insert_track_raw(std::move(t), arrangement::Project::kAppend, true)) {
          reject_entity_id(at, true);
          return result;
        }
        const arrangement::Track& inserted = project.tracks().back();
        for (std::size_t lane_index = 0; lane_index < inserted.automation_lanes.size();
             ++lane_index) {
          const uint32_t lane_id = inserted.automation_lanes[lane_index].lane_id();
          if (lane_id == 0) continue;
          const std::string lane_path =
              schema::index_path(schema::child_path(at, "automationLanes"), lane_index);
          if (!valid_entity_id(lane_id)) {
            reject_entity_id(lane_path, false);
            return result;
          }
          if (!automation_lane_ids.insert(lane_id).second) {
            reject_entity_id(lane_path, true);
            return result;
          }
          max_automation_lane_id = std::max(max_automation_lane_id, lane_id);
        }
      }
    }
    if (max_track_id > 0) project.ensure_next_track_id(max_track_id);
    if (max_automation_lane_id > 0) {
      project.ensure_next_automation_lane_id(max_automation_lane_id);
    }

    // Clips (insert verbatim; bypasses overlap validation to preserve the saved
    // arrangement exactly, including comp lanes that intentionally overlap).
    uint32_t max_clip_id = 0;
    std::unordered_set<arrangement::ClipId> clip_ids;
    if (const auto* arr = array_at(root, "", "clips")) {
      std::size_t index = 0;
      for (const auto& cv : *arr) {
        const std::string at = schema::index_path("clips", index++);
        arrangement::EditClip c = clip_from_json(object_element(cv, at), at);
        if (!valid_entity_id(c.id)) {
          reject_entity_id(at, false);
          return result;
        }
        if (c.id > max_clip_id) max_clip_id = c.id;
        const uint32_t id = c.id;
        // Clip PPQ fields are checked by enforce_edit_api_invariants() below,
        // with the rest of the edit-API value set, so one enumeration covers
        // every clip field no matter which decoder produced it.
        if (!clip_ids.insert(id).second ||
            !project.insert_clip_raw(std::move(c), arrangement::Project::kAppend, true)) {
          reject_entity_id(at, true);
          return result;
        }
      }
    }
    if (max_clip_id > 0) project.ensure_next_clip_id(max_clip_id);

    // Warp maps (plain project metadata referenced by EditClip::warp_ref_id).
    std::unordered_set<arrangement::WarpRefId> warp_ids;
    if (const auto* arr = array_at(root, "", "warpMaps")) {
      std::size_t index = 0;
      for (const auto& wv : *arr) {
        const std::string at = schema::index_path("warpMaps", index++);
        arrangement::WarpMapRef map = warp_map_from_json(object_element(wv, at), at);
        const uint32_t map_id = map.id;
        if (!project.set_warp_map(std::move(map))) {
          decode_diagnostics.warn("invalid_warp_map",
                                  "warp map " + std::to_string(map_id) +
                                      " has an invalid id or malformed anchors and was dropped");
        } else {
          warp_ids.insert(map_id);
        }
      }
    }

    // Part rigs: an entry the setter would refuse is refused under its document path, never
    // dropped; a chain in a build without insert chains stays NotSupported.
    if (const auto* arr = array_at(root, "", "partRigs")) {
      std::size_t index = 0;
      for (const auto& rv : *arr) {
        const std::string at = schema::index_path("partRigs", index++);
        try {
          project.set_part_rig(part_rig_from_json(object_element(rv, at), at));
        } catch (const SonareParameterRefusal& e) {
          schema::rethrow_under(e, at,
                                e.code() == ErrorCode::NotSupported ? ErrorCode::NotSupported
                                                                    : ErrorCode::InvalidFormat);
        }
      }
    }

    if (const auto* arr = array_at(root, "", "instruments")) {
      std::size_t index = 0;
      for (const auto& instrument : *arr) {
        const std::string at = schema::index_path("instruments", index++);
        restore_destination_instrument_from_json(object_element(instrument, at), at, &project);
      }
    }

    // Referential integrity (warnings, non-fatal): the saved arrangement is
    // preserved verbatim, but dangling clip references and clip/source-kind
    // mismatches are surfaced as diagnostics so a host can repair them. Sources
    // and tracks are fully loaded above, so lookups here see the whole project.
    // A malformed document may intentionally make every clip dangling. Bound
    // the retained diagnostic strings so that reporting the problem does not
    // become a second, much larger allocation than the document itself.
    constexpr size_t kMaxReferentialDiagnostics = 128;
    size_t referential_diagnostic_count = 0;
    size_t suppressed_referential_diagnostics = 0;
    const auto emit_referential_warning = [&](std::string code, std::string message) {
      if (referential_diagnostic_count < kMaxReferentialDiagnostics) {
        result.diagnostics.push_back(
            {DiagnosticSeverity::kWarning, std::move(code), std::move(message)});
        ++referential_diagnostic_count;
      } else {
        ++suppressed_referential_diagnostics;
      }
    };
    std::unordered_map<arrangement::SourceId, const arrangement::ClipSource*> source_index;
    source_index.reserve(project.sources().size());
    for (const arrangement::ClipSource& source : project.sources()) {
      source_index.emplace(arrangement::source_id(source), &source);
    }
    std::unordered_map<arrangement::TrackId, const arrangement::Track*> track_index;
    track_index.reserve(project.tracks().size());
    for (const arrangement::Track& track : project.tracks()) {
      track_index.emplace(track.id, &track);
    }
    for (const auto& c : project.clips()) {
      const auto source_it = source_index.find(c.source_id);
      const arrangement::ClipSource* src =
          source_it == source_index.end() ? nullptr : source_it->second;
      if (src == nullptr) {
        emit_referential_warning("dangling_clip_source", "clip " + std::to_string(c.id) +
                                                             " references missing source " +
                                                             std::to_string(c.source_id));
      }
      const auto track_it = track_index.find(c.track_id);
      const arrangement::Track* track = track_it == track_index.end() ? nullptr : track_it->second;
      if (track == nullptr) {
        emit_referential_warning("dangling_clip_track", "clip " + std::to_string(c.id) +
                                                            " references missing track " +
                                                            std::to_string(c.track_id));
      }
      if (c.warp_ref_id != 0 && warp_ids.find(c.warp_ref_id) == warp_ids.end()) {
        emit_referential_warning("dangling_clip_warp", "clip " + std::to_string(c.id) +
                                                           " references missing warp map " +
                                                           std::to_string(c.warp_ref_id));
      }
      if (src != nullptr && track != nullptr) {
        const arrangement::SourceKind sk = arrangement::source_kind(*src);
        if (!arrangement::detail::track_kind_accepts_source_kind(track->kind, sk)) {
          emit_referential_warning("clip_source_kind_mismatch",
                                   "clip " + std::to_string(c.id) +
                                       " source kind does not match track " +
                                       std::to_string(c.track_id) + " kind");
        }
      }
      for (const arrangement::ClipTake& take : c.takes) {
        const arrangement::SourceId take_source_id =
            take.source_id == 0 ? c.source_id : take.source_id;
        const auto take_source_it = source_index.find(take_source_id);
        const arrangement::ClipSource* take_source =
            take_source_it == source_index.end() ? nullptr : take_source_it->second;
        if (take_source == nullptr) {
          emit_referential_warning("dangling_clip_take_source",
                                   "clip " + std::to_string(c.id) + " take " +
                                       std::to_string(take.id) + " references missing source " +
                                       std::to_string(take_source_id));
        }
        if (take_source != nullptr && track != nullptr &&
            !arrangement::detail::track_kind_accepts_source_kind(
                track->kind, arrangement::source_kind(*take_source))) {
          emit_referential_warning(
              "clip_take_source_kind_mismatch",
              "clip " + std::to_string(c.id) + " take " + std::to_string(take.id) +
                  " source kind does not match track " + std::to_string(c.track_id) + " kind");
        }
      }
    }
    if (suppressed_referential_diagnostics > 0) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kWarning, "referential_diagnostics_truncated",
           std::to_string(suppressed_referential_diagnostics) +
               " additional dangling-reference diagnostics were suppressed"});
    }

    // Markers.
    uint32_t max_marker_id = 0;
    std::unordered_set<uint32_t> marker_ids;
    if (const auto* arr = array_at(root, "", "markers")) {
      std::size_t index = 0;
      for (const auto& element : *arr) {
        const std::string at = schema::index_path("markers", index++);
        const Value& mv = object_element(element, at);
        arrangement::ProjectMarker m;
        m.id = uint_or(mv, at, "id", 0);
        m.ppq = num_or(mv, at, "ppq", 0.0);
        m.name = str_or(mv, at, "name", "");
        m.kind = static_cast<uint8_t>(enum_or(mv, at, "kind", arrangement::MarkerKind::kMarker));
        // Read unconditionally, and written by marker_to_json whenever non-zero
        // regardless of kind, so a marker that carries key fields on a non-key
        // kind (which the C ABI accepts) survives a save/load unchanged. Only
        // key-signature markers carry the SMF fifths range restriction.
        m.key_fifths = int8_or(mv, at, "keyFifths", 0);
        if (m.kind == static_cast<uint8_t>(arrangement::MarkerKind::kKeySignature) &&
            (m.key_fifths < -7 || m.key_fifths > 7)) {
          throw SonareException(ErrorCode::InvalidFormat,
                                schema::refusal(schema::child_path(at, "keyFifths"),
                                                schema::integer_range_predicate(-7, 7)));
        }
        m.key_minor = bool_or(mv, at, "keyMinor", false);
        if (!valid_entity_id(m.id)) {
          reject_entity_id(at, false);
          return result;
        }
        if (!marker_ids.insert(m.id).second) {
          reject_entity_id(at, true);
          return result;
        }
        if (m.id > max_marker_id) max_marker_id = m.id;
        project.markers_mutable().push_back(std::move(m));
      }
    }
    if (max_marker_id > 0) project.ensure_next_marker_id(max_marker_id);

    // Annotation.
    if (const auto* av = object_value_at(root, "", "annotation")) {
      annotation_from_json(*av, "annotation", &project.annotation());
    }

    // Mixer scene.
    if (const auto* sv = object_value_at(root, "", "scene")) {
      project.set_scene(scene_from_value(*sv));
    }

    // Assist sidecars (lossless, even for unregistered modules). The payload
    // string is borrowed from the admitted root until decode completes; this
    // keeps the loader's payload-first limit/error order without copying it.
    std::size_t decoded_payload_bytes = 0;
    if (const auto* arr = array_at(root, "", "assistSidecars")) {
      std::size_t index = 0;
      for (const auto& element : *arr) {
        const std::string at = schema::index_path("assistSidecars", index++);
        const Value& sv = object_element(element, at);
        static const std::string kEmptyPayload;
        const std::string* encoded_payload = strict_string_field(sv, at, "payloadB64");
        const std::string& payload = encoded_payload != nullptr ? *encoded_payload : kEmptyPayload;
        const std::size_t remaining_payload_bytes =
            import_limits.max_decoded_payload_bytes - decoded_payload_bytes;
        if (base64_decoded_upper_bound(payload) > remaining_payload_bytes) {
          reject_payload_limit();
          return result;
        }
        arrangement::AssistSidecar sidecar;
        sidecar_fields_from_json(sv, at, &sidecar);
        if (std::string current_id; earlier_assist_module_id(sidecar.module_id, &current_id)) {
          result.diagnostics.push_back(
              {DiagnosticSeverity::kError, "legacy_assist_module_id",
               schema::refusal(schema::child_path(at, "moduleId"),
                               "is an earlier spelling; write " + current_id)});
          return result;
        }
        if (!sidecar_from_borrowed_payload(payload, &sidecar, remaining_payload_bytes)) {
          result.diagnostics.push_back(
              {DiagnosticSeverity::kError, "invalid_sidecar_payload",
               schema::refusal(schema::child_path(at, "payloadB64"), "must be valid base64")});
          return result;
        }
        if (!resource::bounded_accumulate(sidecar.payload.size(),
                                          import_limits.max_decoded_payload_bytes,
                                          &decoded_payload_bytes)) {
          reject_payload_limit();
          return result;
        }
        project.add_assist_sidecar(std::move(sidecar));
      }
    }

    // MIDI content store (keyed by clip id string).
    if (const auto* mc = object_at(root, "", "midiContent")) {
      for (const auto& [key, value] : *mc) {
        if (key == "__sysexPayloads") {
          const std::string table_path = schema::child_path("midiContent", key);
          if (!value.is_object()) {
            throw SonareException(ErrorCode::InvalidFormat,
                                  schema::must_be(table_path, schema::Kind::Object));
          }
          for (const auto& [handle_key, payload_value] : value.as_object()) {
            const std::string payload_path = schema::child_path(table_path, handle_key);
            if (!payload_value.is_string()) {
              throw SonareException(ErrorCode::InvalidFormat,
                                    schema::must_be(payload_path, schema::Kind::String));
            }
            arrangement::ClipId handle = 0;
            if (!parse_uint32_key(handle_key, &handle)) {
              decode_diagnostics.warn("invalid_sysex_handle",
                                      "MIDI SysEx payload handle key \"" + handle_key +
                                          "\" is outside uint32 range; entry ignored");
              continue;
            }
            const std::size_t remaining_payload_bytes =
                import_limits.max_decoded_payload_bytes - decoded_payload_bytes;
            if (base64_decoded_upper_bound(payload_value.as_string()) > remaining_payload_bytes) {
              reject_payload_limit();
              return result;
            }
            std::vector<uint8_t> payload;
            if (!sonare::base64_decode(payload_value.as_string(), &payload,
                                       remaining_payload_bytes)) {
              result.diagnostics.push_back({DiagnosticSeverity::kError, "invalid_sysex_payload",
                                            schema::refusal(payload_path, "must be valid base64")});
              return result;
            }
            if (!resource::bounded_accumulate(payload.size(),
                                              import_limits.max_decoded_payload_bytes,
                                              &decoded_payload_bytes)) {
              reject_payload_limit();
              return result;
            }
            result.midi.sysex_payloads[handle] = std::move(payload);
          }
          continue;
        }
        const std::string content_path = schema::child_path("midiContent", key);
        if (!value.is_array()) {
          throw SonareException(ErrorCode::InvalidFormat,
                                schema::must_be(content_path, schema::Kind::Array));
        }
        arrangement::ClipId clip_id = 0;
        if (!parse_uint32_key(key, &clip_id)) {
          decode_diagnostics.warn(
              "invalid_midi_content_key",
              "MIDI content clip key \"" + key + "\" is outside uint32 range; entry ignored");
          continue;
        }
        arrangement::MidiClipEventList events;
        std::size_t event_index = 0;
        for (const auto& element : value.as_array()) {
          const std::string event_path = schema::index_path(content_path, event_index++);
          const Value& ev = object_element(element, event_path);
          arrangement::MidiClipEvent e;
          e.ppq = num_or(ev, event_path, "ppq", 0.0);
          e.word0 = midi_word_or_warn(ev, event_path, "word0", clip_id, &decode_diagnostics);
          e.word1 = midi_word_or_warn(ev, event_path, "word1", clip_id, &decode_diagnostics);
          e.sysex_handle = uint_or(ev, event_path, "sysexHandle", 0);
          events.push_back(e);
        }
        result.midi.events[clip_id] = std::move(events);
      }
    }

    // Last gate before the model escapes: bring it into the state space the C
    // ABI edit API can produce, so no decoder above can hand back a project the
    // setters could neither have built nor can now address.
    if (const auto violation =
            enforce_edit_api_invariants(&project, &result.midi, &decode_diagnostics)) {
      result.diagnostics.push_back(
          {DiagnosticSeverity::kError, violation->code, violation->message});
      return result;
    }

    // The edit commands prune incrementally, so a loaded store must start pruned.
    result.midi.prune_unreferenced_sysex_payloads();

    decode_diagnostics.finalize();
    result.project = std::move(project);
    return result;
  } catch (const SonareException& e) {
    result.project.reset();
    result.reason = e.reason();
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
