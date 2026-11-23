// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arrangement/automation_lane_validation.h"
#include "arrangement/edit_command_internal.h"
#include "arrangement/edit_source.h"
#include "arrangement/harmonic_limits.h"
#include "arrangement/harmonic_timeline.h"
#include "automation/automation_lane.h"
#include "c_api/synth_patch_fields.h"
#include "mixing/api/scene.h"
#include "mixing/panner.h"
#include "serialize/project_serializer_internal.h"
#include "serialize/project_serializer_private.h"
#include "serialize/serialized_enum_bounds.h"
#include "transport/tempo_map.h"
#include "util/base64.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json_schema.h"
#include "util/numeric_validation.h"

namespace sonare::arrangement::detail {

// The serializer is the only code that needs to normalize a loaded track's
// private lane vector. Keeping this access in the decoder TU avoids adding a
// mutable range to the installed Project API or changing Project's layout.
struct ProjectSerializerAccess {
  static std::vector<Track>& tracks(Project& project) noexcept { return project.tracks_; }
  static std::vector<EditClip>& clips(Project& project) noexcept { return project.clips_; }
};

}  // namespace sonare::arrangement::detail

namespace sonare::serialize {

using constants::kDefaultBpm;

namespace detail {

namespace schema = sonare::util::json::schema;

namespace {

void require_present(const Value& value, const std::string& path, const char* key) {
  if (value.find(key) == nullptr) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::missing_key(schema::child_path(path, key)));
  }
}

uint8_t pitch_class_or(const Value& value, const std::string& path, const char* key) {
  const uint32_t pitch_class = uint_or(value, path, key, arrangement::kUnknownPitchClass);
  if (pitch_class > 11 && pitch_class != arrangement::kUnknownPitchClass) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::refusal(schema::child_path(path, key),
                                          "must be an integer in [0, 11] or " +
                                              std::to_string(arrangement::kUnknownPitchClass)));
  }
  return static_cast<uint8_t>(pitch_class);
}

float float_or(const Value& value, const std::string& path, const char* key, float fallback) {
  float converted = 0.0f;
  if (!numeric::checked_float_cast(num_or(value, path, key, fallback), &converted)) {
    throw SonareException(ErrorCode::InvalidFormat, schema::refusal(schema::child_path(path, key),
                                                                    schema::kFloatRangePredicate));
  }
  return converted;
}

std::string required_string(const Value& value, const std::string& path, const char* key) {
  const std::string field_path = schema::child_path(path, key);
  const Value* field = value.find(key);
  if (field == nullptr) {
    throw SonareException(ErrorCode::InvalidFormat, schema::missing_key(field_path));
  }
  if (!field->is_string()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::must_be(field_path, schema::Kind::String));
  }
  return field->as_string();
}

void double_map_from_json(const Value& value, const std::string& path, const char* key,
                          std::map<std::string, double>* out) {
  require_present(value, path, key);
  const std::string map_path = schema::child_path(path, key);
  const Object* values = object_at(value, path, key);
  if (values == nullptr) return;
  for (const auto& [name, field] : *values) {
    const std::string field_path = schema::child_path(map_path, name);
    if (!field.is_number()) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::must_be(field_path, schema::Kind::Number));
    }
    out->emplace(name, field.as_number());
  }
}

void patch_field_map_from_json(const Value& value, const std::string& path, const char* key,
                               std::map<std::string, arrangement::DestinationPatchField>* out) {
  require_present(value, path, key);
  const std::string map_path = schema::child_path(path, key);
  const Object* fields = object_at(value, path, key);
  if (fields == nullptr) return;
  for (const auto& entry : *fields) {
    const auto& name = entry.first;
    const auto& field = entry.second;
    const std::string field_path = schema::child_path(map_path, name);
    const auto& enum_rows = sonare_c_detail::kSynthPatchEnumFields;
    const auto enum_row = std::find_if(std::begin(enum_rows), std::end(enum_rows),
                                       [&](const auto& row) { return name == row.name; });
    if (enum_row != std::end(enum_rows)) {
      if (!field.is_string()) {
        throw SonareException(ErrorCode::InvalidFormat,
                              schema::must_be(field_path, schema::Kind::String));
      }
      const auto& token = field.as_string();
      const auto* end = enum_row->values.names + enum_row->values.count;
      if (std::find(enum_row->values.names, end, token) == end) {
        throw SonareException(
            ErrorCode::InvalidFormat,
            schema::refusal(field_path, "must name a declared synth patch value"));
      }
      out->emplace(name, token);
      continue;
    }
    const auto& numeric_rows = sonare_c_detail::kSynthPatchNumericFields;
    const auto numeric_row = std::find_if(std::begin(numeric_rows), std::end(numeric_rows),
                                          [&](const auto& row) { return name == row.name; });
    if (numeric_row == std::end(numeric_rows)) {
      std::vector<std::string> accepted;
      for (const auto& row : enum_rows) accepted.emplace_back(row.name);
      for (const auto& row : numeric_rows) accepted.emplace_back(row.name);
      throw SonareException(
          ErrorCode::InvalidFormat,
          schema::unknown_key_expected(field_path, schema::expected_keys(accepted)));
    }
    if (!field.is_number()) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::must_be(field_path, schema::Kind::Number));
    }
    const double number = field.as_number();
    if (std::holds_alternative<int SonareSynthPatch::*>(numeric_row->member)) {
      int converted = 0;
      if (!numeric::checked_integral_cast(number, &converted)) {
        throw SonareException(ErrorCode::InvalidFormat,
                              schema::refusal(field_path, "must fit a signed 32-bit integer"));
      }
    } else {
      float converted = 0.0f;
      if (!numeric::checked_float_cast(number, &converted)) {
        throw SonareException(ErrorCode::InvalidFormat,
                              schema::refusal(field_path, schema::kFloatRangePredicate));
      }
    }
    out->emplace(name, number);
  }
}

}  // namespace

// ===========================================================================
// Decode helpers: util::json::Value -> arrangement model
// ===========================================================================

transport::TempoSegment tempo_segment_from_json(const Value& v, const std::string& path) {
  transport::TempoSegment s;
  s.start_ppq = num_or(v, path, "startPpq", 0.0);
  // The absent-field fallback is the model's own default, so a document with no
  // tempo decodes to the same segment a default-constructed one produces.
  s.bpm = num_or(v, path, "bpm", kDefaultBpm);
  s.start_sample = num_or(v, path, "startSample", 0.0);
  s.end_bpm = num_or(v, path, "endBpm", 0.0);
  return s;
}

transport::TimeSignatureSegment time_signature_from_json(const Value& v, const std::string& path) {
  transport::TimeSignatureSegment s;
  s.start_ppq = num_or(v, path, "startPpq", 0.0);
  s.time_sig.numerator = int_or(v, path, "numerator", 4);
  s.time_sig.denominator = int_or(v, path, "denominator", 4);
  return s;
}

automation::AutomationLane automation_lane_from_json(const Value& v, const std::string& path) {
  const auto target_kind =
      enum_or(v, path, "targetKind", automation::AutomationTargetKind::kOpaque);
  const uint32_t target_param_id = uint_or(v, path, "targetParamId", 0);
  const Value* lane_id_value = v.find("id");
  const uint32_t lane_id = lane_id_value != nullptr ? uint_or(v, path, "id", 0) : 0;
  if (lane_id_value != nullptr && !valid_entity_id(lane_id)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::refusal(schema::child_path(path, "id"),
                                          schema::integer_range_predicate(
                                              1, std::numeric_limits<uint32_t>::max() - 1u)));
  }
  if (lane_id_value != nullptr && target_kind != automation::AutomationTargetKind::kOpaque &&
      target_param_id != 0) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::refusal(schema::child_path(path, "targetParamId"),
                                          "must be 0 for a typed automation lane"));
  }
  automation::AutomationTargetIdentity identity;
  const auto refuse_identity_key = [&](const char* key) {
    if (v.find(key) != nullptr) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::refusal(schema::child_path(path, key), "must be absent"));
    }
  };
  switch (target_kind) {
    case automation::AutomationTargetKind::kTrackSendDb:
      refuse_identity_key("insertIndex");
      refuse_identity_key("processor");
      refuse_identity_key("paramName");
      identity.send_id = required_string(v, path, "sendId");
      if (identity.send_id.empty()) {
        throw SonareException(
            ErrorCode::InvalidFormat,
            schema::refusal(schema::child_path(path, "sendId"), "must not be empty"));
      }
      break;
    case automation::AutomationTargetKind::kTrackInsertParam: {
      refuse_identity_key("sendId");
      const std::string insert_index_path = schema::child_path(path, "insertIndex");
      const Value* insert_index = v.find("insertIndex");
      if (insert_index == nullptr) {
        throw SonareException(ErrorCode::InvalidFormat, schema::missing_key(insert_index_path));
      }
      if (!insert_index->is_number()) {
        throw SonareException(ErrorCode::InvalidFormat,
                              schema::must_be(insert_index_path, schema::Kind::Number));
      }
      int32_t parsed_index = 0;
      if (!numeric::checked_integral_cast(insert_index->as_number(), &parsed_index) ||
          parsed_index < 0) {
        throw SonareException(
            ErrorCode::InvalidFormat,
            schema::refusal(insert_index_path, schema::integer_range_predicate(
                                                   0, std::numeric_limits<int32_t>::max())));
      }
      identity.insert_index = static_cast<std::size_t>(parsed_index);
      identity.processor = required_string(v, path, "processor");
      identity.param_name = required_string(v, path, "paramName");
      if (identity.processor.empty()) {
        throw SonareException(
            ErrorCode::InvalidFormat,
            schema::refusal(schema::child_path(path, "processor"), "must not be empty"));
      }
      if (identity.param_name.empty()) {
        throw SonareException(
            ErrorCode::InvalidFormat,
            schema::refusal(schema::child_path(path, "paramName"), "must not be empty"));
      }
      break;
    }
    case automation::AutomationTargetKind::kOpaque:
    case automation::AutomationTargetKind::kTrackFaderDb:
    case automation::AutomationTargetKind::kTrackPan:
    case automation::AutomationTargetKind::kTrackWidth:
      refuse_identity_key("sendId");
      refuse_identity_key("insertIndex");
      refuse_identity_key("processor");
      refuse_identity_key("paramName");
      break;
  }
  automation::AutomationLane lane(target_param_id, target_kind, std::move(identity));
  if (lane_id_value != nullptr) lane.set_lane_id(lane_id);
  std::vector<automation::Breakpoint> points;
  if (const auto* arr = array_at(v, path, "points")) {
    const std::string points_path = schema::child_path(path, "points");
    std::size_t index = 0;
    for (const auto& element : *arr) {
      const std::string point_path = schema::index_path(points_path, index++);
      const Value& pv = object_element(element, point_path);
      automation::Breakpoint bp;
      bp.ppq = num_or(pv, point_path, "ppq", 0.0);
      bp.value = float_or(pv, point_path, "value", 0.0f);
      bp.curve_to_next = enum_or(pv, point_path, "curveToNext", automation::CurveType::Linear);
      if (!std::isfinite(bp.ppq)) {
        throw SonareException(
            ErrorCode::InvalidFormat,
            schema::refusal(schema::child_path(point_path, "ppq"), "must be finite"));
      }
      try {
        arrangement::check_automation_lane_value(target_kind, target_param_id, bp.value, "value");
      } catch (const SonareParameterRefusal& error) {
        schema::rethrow_under(error, point_path, ErrorCode::InvalidFormat);
      }
      points.push_back(bp);
    }
  }
  lane.set_points(std::move(points));
  return lane;
}

void restore_destination_instrument_from_json(const Value& v, const std::string& path,
                                              arrangement::Project* project) {
  const std::string destination_id_path = schema::child_path(path, "destinationId");
  if (v.find("destinationId") == nullptr) {
    throw SonareException(ErrorCode::InvalidFormat, schema::missing_key(destination_id_path));
  }
  const uint32_t destination_id = uint_or(v, path, "destinationId", 0);
  const std::string kind_path = schema::child_path(path, "kind");
  const std::string kind = required_string(v, path, "kind");

  arrangement::DestinationInstrument instrument;
  const auto refuse_unrelated = [&](const char* key) {
    if (v.find(key) != nullptr) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::refusal(schema::child_path(path, key), "must be absent"));
    }
  };
  if (kind == "synth") {
    refuse_unrelated("builtin");
    refuse_unrelated("sf2");
    const Value* config = object_value_at(v, path, "synth");
    if (config == nullptr) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::missing_key(schema::child_path(path, "synth")));
    }
    const std::string config_path = schema::child_path(path, "synth");
    arrangement::DestinationSynthInstrument synth;
    synth.preset = required_string(*config, config_path, "preset");
    if (synth.preset.size() >= sizeof(SonareSynthPatch::preset)) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::refusal(schema::child_path(config_path, "preset"),
                                            "must fit in the synth preset name field"));
    }
    synth.engine_mode = str_or(*config, config_path, "engineMode", "");
    const auto& modes = c_api::synth_names::kEngineModes;
    if (!synth.engine_mode.empty() &&
        std::find(std::begin(modes), std::end(modes), synth.engine_mode) == std::end(modes)) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::refusal(schema::child_path(config_path, "engineMode"),
                                            "must name a declared synth engine mode"));
    }
    if (synth.engine_mode == "sample") {
      throw SonareException(
          ErrorCode::InvalidFormat,
          schema::refusal(schema::child_path(schema::child_path(path, "synth"), "engineMode"),
                          "must not select a non-persistable sample engine"));
    }
    patch_field_map_from_json(*config, config_path, "fields", &synth.fields);
    double_map_from_json(*config, config_path, "engineParams", &synth.engine_params);
    require_present(*config, config_path, "useGmPrograms");
    synth.use_gm_programs = bool_or(*config, config_path, "useGmPrograms", false);
    if (const auto* routes = array_at(*config, config_path, "modRoutings")) {
      const std::string routes_path = schema::child_path(config_path, "modRoutings");
      if (routes->size() > SONARE_SYNTH_PATCH_MOD_ROUTINGS) {
        throw SonareException(ErrorCode::InvalidFormat,
                              schema::refusal(routes_path, "contains too many modulation routes"));
      }
      synth.clears_base_mod_routings = routes->empty();
      std::size_t index = 0;
      for (const auto& route_value : *routes) {
        const std::string route_path = schema::index_path(routes_path, index++);
        const Value& route = object_element(route_value, route_path);
        synth.mod_routings.push_back({int_or(route, route_path, "source", 0),
                                      int_or(route, route_path, "destination", 0),
                                      float_or(route, route_path, "depth", 0.0f)});
      }
    }
    instrument = std::move(synth);
  } else if (kind == "builtin") {
    refuse_unrelated("synth");
    refuse_unrelated("sf2");
    const Value* config = object_value_at(v, path, "builtin");
    if (config == nullptr) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::missing_key(schema::child_path(path, "builtin")));
    }
    const std::string config_path = schema::child_path(path, "builtin");
    arrangement::DestinationBuiltinInstrument builtin;
    for (const char* key :
         {"waveform", "gain", "attackMs", "decayMs", "sustain", "releaseMs", "polyphony"}) {
      require_present(*config, config_path, key);
    }
    builtin.waveform = int_or(*config, config_path, "waveform", builtin.waveform);
    builtin.gain = float_or(*config, config_path, "gain", builtin.gain);
    builtin.attack_ms = float_or(*config, config_path, "attackMs", builtin.attack_ms);
    builtin.decay_ms = float_or(*config, config_path, "decayMs", builtin.decay_ms);
    builtin.sustain = float_or(*config, config_path, "sustain", builtin.sustain);
    builtin.release_ms = float_or(*config, config_path, "releaseMs", builtin.release_ms);
    builtin.polyphony = int_or(*config, config_path, "polyphony", builtin.polyphony);
    instrument = builtin;
  } else if (kind == "sf2") {
    refuse_unrelated("synth");
    refuse_unrelated("builtin");
    const Value* config = object_value_at(v, path, "sf2");
    if (config == nullptr) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::missing_key(schema::child_path(path, "sf2")));
    }
    const std::string config_path = schema::child_path(path, "sf2");
    arrangement::DestinationSf2Instrument sf2;
    for (const char* key : {"gain", "polyphony", "preferModelForModeledFamilies", "clearBankRig",
                            "gsEfxRealization"}) {
      require_present(*config, config_path, key);
    }
    sf2.gain = float_or(*config, config_path, "gain", sf2.gain);
    sf2.polyphony = int_or(*config, config_path, "polyphony", sf2.polyphony);
    sf2.prefer_model_for_modeled_families =
        int_or(*config, config_path, "preferModelForModeledFamilies",
               sf2.prefer_model_for_modeled_families);
    sf2.clear_bank_rig = int_or(*config, config_path, "clearBankRig", sf2.clear_bank_rig);
    sf2.gs_efx_realization =
        int_or(*config, config_path, "gsEfxRealization", sf2.gs_efx_realization);
    if (config->find("soundFontUri") != nullptr) {
      sf2.sound_font_uri = required_string(*config, config_path, "soundFontUri");
    }
    instrument = std::move(sf2);
  } else {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::refusal(kind_path, "must be one of synth, builtin, sf2"));
  }

  arrangement::DestinationInstrumentBinding binding;
  binding.instrument = std::move(instrument);
  if (v.find("bank") != nullptr) {
    const Value* bank_value = object_value_at(v, path, "bank");
    const std::string bank_path = schema::child_path(path, "bank");
    if (bank_value->find("msb") == nullptr) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::missing_key(schema::child_path(bank_path, "msb")));
    }
    if (bank_value->find("lsb") == nullptr) {
      throw SonareException(ErrorCode::InvalidFormat,
                            schema::missing_key(schema::child_path(bank_path, "lsb")));
    }
    binding.bank = arrangement::DestinationMidiBank{int_or(*bank_value, bank_path, "msb", 0),
                                                    int_or(*bank_value, bank_path, "lsb", 0)};
  }
  if (v.find("program") != nullptr) {
    binding.program = int_or(v, path, "program", 0);
  }
  try {
    project->restore_destination_instrument(destination_id, std::move(binding));
  } catch (const SonareParameterRefusal& error) {
    throw error.respelled(
        ErrorCode::InvalidFormat,
        [&](const std::string& subject) {
          return schema::child_path(path, sonare::util::check::camel_path(subject));
        },
        false);
  }
}

arrangement::Track track_from_json(const Value& v, const std::string& path) {
  arrangement::Track t;
  t.id = uint_or(v, path, "id", 0);
  t.name = str_or(v, path, "name", "");
  t.kind = enum_or(v, path, "kind", arrangement::Track::Kind::kAudio);
  t.gain = float_or(v, path, "gain", 1.0f);
  t.mute = bool_or(v, path, "mute", false);
  t.solo = bool_or(v, path, "solo", false);
  t.pan = float_or(v, path, "pan", 0.0f);
  if (!mixing::pan_in_range(t.pan)) {
    throw SonareException(
        ErrorCode::InvalidFormat,
        schema::refusal(schema::child_path(path, "pan"), schema::range_predicate(-1.0, 1.0)));
  }
  t.channel_strip_ref = str_or(v, path, "channelStripRef", "");
  t.output_target = str_or(v, path, "outputTarget", "");
  t.midi_destination_id = uint_or(v, path, "midiDestinationId", 0);
  if (const auto* arr = array_at(v, path, "automationLanes")) {
    // Decoded verbatim; the one-lane-per-target invariant is enforced for the
    // whole model by enforce_edit_api_invariants().
    const std::string lanes_path = schema::child_path(path, "automationLanes");
    std::size_t index = 0;
    for (const auto& lv : *arr) {
      const std::string lane_path = schema::index_path(lanes_path, index++);
      t.automation_lanes.push_back(
          automation_lane_from_json(object_element(lv, lane_path), lane_path));
    }
  }
  return t;
}

arrangement::ClipFade fade_from_json(const Value& v, const std::string& path) {
  arrangement::ClipFade f;
  f.length_ppq = num_or(v, path, "lengthPpq", 0.0);
  f.curve = enum_or(v, path, "curve", arrangement::FadeCurve::kLinear);
  return f;
}

arrangement::ClipTake take_from_json(const Value& v, const std::string& path) {
  arrangement::ClipTake take;
  take.id = uint_or(v, path, "id", 0);
  take.source_id = uint_or(v, path, "sourceId", 0);
  take.source_offset_ppq = num_or(v, path, "sourceOffsetPpq", 0.0);
  take.name = str_or(v, path, "name", "");
  if (v.contains("sourceOffsetSec")) {
    take.source_offset_seconds = num_or(v, path, "sourceOffsetSec", 0.0);
  }
  take.partial = bool_or(v, path, "partial", false);
  return take;
}

arrangement::ClipCompSegment comp_segment_from_json(const Value& v, const std::string& path) {
  arrangement::ClipCompSegment segment;
  segment.start_ppq = num_or(v, path, "startPpq", 0.0);
  segment.end_ppq = num_or(v, path, "endPpq", 0.0);
  segment.take_id = uint_or(v, path, "takeId", 0);
  // Gated read: a document written before comp seams could crossfade has no
  // key here and loads as the butt join it was rendered with.
  segment.crossfade_ppq = num_or(v, path, "crossfadePpq", 0.0);
  return segment;
}

arrangement::ClipCompRenderPart comp_render_part_from_json(const Value& v,
                                                           const std::string& path) {
  arrangement::ClipCompRenderPart part;
  part.visible_start_ppq = num_or(v, path, "visibleStartPpq", 0.0);
  part.visible_end_ppq = num_or(v, path, "visibleEndPpq", 0.0);
  part.reference_start_ppq = num_or(v, path, "referenceStartPpq", 0.0);
  part.reference_end_ppq = num_or(v, path, "referenceEndPpq", 0.0);
  part.take_id = uint_or(v, path, "takeId", 0);
  part.seam_fade_in_ppq = num_or(v, path, "seamFadeInPpq", 0.0);
  part.seam_fade_out_ppq = num_or(v, path, "seamFadeOutPpq", 0.0);
  part.retained_only = bool_or(v, path, "retainedOnly", false);
  return part;
}

namespace {

// Decodes each object element of the array at @p key through @p decode(element, element_path).
template <typename Decode>
void for_each_element(const Value& v, const std::string& path, const char* key, Decode decode) {
  const auto* arr = array_at(v, path, key);
  if (arr == nullptr) return;
  const std::string array_path = schema::child_path(path, key);
  std::size_t index = 0;
  for (const auto& element : *arr) {
    const std::string element_path = schema::index_path(array_path, index++);
    decode(object_element(element, element_path), element_path);
  }
}

}  // namespace

arrangement::EditClip clip_from_json(const Value& v, const std::string& path) {
  arrangement::EditClip c;
  c.id = uint_or(v, path, "id", 0);
  c.track_id = uint_or(v, path, "trackId", 0);
  c.source_id = uint_or(v, path, "sourceId", 0);
  c.start_ppq = num_or(v, path, "startPpq", 0.0);
  c.length_ppq = num_or(v, path, "lengthPpq", 0.0);
  c.source_offset_ppq = num_or(v, path, "sourceOffsetPpq", 0.0);
  if (v.contains("sourceOffsetSec")) {
    c.source_offset_seconds = num_or(v, path, "sourceOffsetSec", 0.0);
  }
  c.gain = float_or(v, path, "gain", 1.0f);
  if (const auto* fi = object_at(v, path, "fadeIn")) {
    c.fade_in = fade_from_json(Value(*fi), schema::child_path(path, "fadeIn"));
  }
  if (const auto* fo = object_at(v, path, "fadeOut")) {
    c.fade_out = fade_from_json(Value(*fo), schema::child_path(path, "fadeOut"));
  }
  c.loop_mode = enum_or(v, path, "loopMode", arrangement::LoopMode::kOff);
  c.loop_length_ppq = num_or(v, path, "loopLengthPpq", 0.0);
  c.loop_crossfade_ppq = num_or(v, path, "loopCrossfadePpq", 0.0);
  const bool has_loop_period = v.contains("loopPeriodSec");
  const bool has_loop_phase = v.contains("loopPhaseSec");
  if (has_loop_period != has_loop_phase) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::missing_key(schema::child_path(
                              path, has_loop_period ? "loopPhaseSec" : "loopPeriodSec")));
  }
  if (has_loop_period) {
    c.loop_anchor = arrangement::LoopAnchor{
        num_or(v, path, "loopPeriodSec", 0.0),
        num_or(v, path, "loopPhaseSec", 0.0),
    };
  }
  c.warp_ref_id = uint_or(v, path, "warpRefId", 0);
  c.warp_mode = enum_or(v, path, "warpMode", arrangement::WarpMode::kOff);
  for_each_element(v, path, "takes", [&c](const Value& tv, const std::string& p) {
    c.takes.push_back(take_from_json(tv, p));
  });
  c.active_take_id = uint_or(v, path, "activeTakeId", 0);
  for_each_element(v, path, "compSegments", [&c](const Value& sv, const std::string& p) {
    c.comp_segments.push_back(comp_segment_from_json(sv, p));
  });
  for_each_element(v, path, "compRenderParts", [&c](const Value& pv, const std::string& p) {
    c.comp_render_parts.push_back(comp_render_part_from_json(pv, p));
  });
  return c;
}

arrangement::WarpMapRef warp_map_from_json(const Value& v, const std::string& path) {
  arrangement::WarpMapRef map;
  map.id = uint_or(v, path, "id", 0);
  map.name = str_or(v, path, "name", "");
  for_each_element(v, path, "anchors", [&map](const Value& av, const std::string& p) {
    arrangement::WarpAnchorRef anchor;
    anchor.warp_sample = num_or(av, p, "warpSample", 0.0);
    anchor.source_sample = num_or(av, p, "sourceSample", 0.0);
    map.anchors.push_back(anchor);
  });
  return map;
}

arrangement::ClipSource source_from_json(const Value& v, const std::string& path) {
  const auto kind = enum_or(v, path, "kind", arrangement::SourceKind::kAudio);
  if (kind == arrangement::SourceKind::kMidi) {
    arrangement::MidiSourceRef m;
    m.id = uint_or(v, path, "id", 0);
    m.name = str_or(v, path, "name", "");
    m.channel_hint = uint_or(v, path, "channelHint", 0);
    return m;
  }
  arrangement::AudioSourceRef a;
  a.id = uint_or(v, path, "id", 0);
  a.uri = str_or(v, path, "uri", "");
  a.channel_count = uint_or(v, path, "channelCount", 0);
  a.sample_rate_hint = num_or(v, path, "sampleRateHint", 0.0);
  a.storage_handle_id = uint_or(v, path, "storageHandleId", 0);
  // Gated read: absent content_hash (older documents) loads as empty.
  a.content_hash = str_or(v, path, "contentHash", "");
  a.external_stem_role = str_or(v, path, "externalStemRole", "");
  return a;
}

arrangement::ChordSymbol chord_from_json(const Value& v, const std::string& path) {
  arrangement::ChordSymbol c;
  c.start_ppq = num_or(v, path, "startPpq", 0.0);
  c.end_ppq = num_or(v, path, "endPpq", 0.0);
  c.root_pc = pitch_class_or(v, path, "rootPc");
  c.quality = enum_or(v, path, "quality", arrangement::ChordQuality::kUnknown);
  if (const auto* arr = array_at(v, path, "extensions")) {
    const std::string extensions_path = schema::child_path(path, "extensions");
    std::size_t index = 0;
    for (const auto& ev : *arr) {
      const std::string at = schema::index_path(extensions_path, index++);
      if (!ev.is_number()) {
        throw SonareException(ErrorCode::InvalidFormat, schema::must_be(at, schema::Kind::Number));
      }
      uint8_t extension = 0;
      if (!numeric::checked_integral_cast(ev.as_number(), &extension)) {
        throw SonareException(ErrorCode::InvalidFormat,
                              schema::refusal(at, schema::integer_range_predicate(0, 255)));
      }
      c.extensions.push_back(extension);
    }
  }
  c.slash_bass_pc = pitch_class_or(v, path, "slashBassPc");
  c.roman_numeral = str_or(v, path, "romanNumeral", "");
  c.modulation_boundary = bool_or(v, path, "modulationBoundary", false);
  return c;
}

arrangement::KeySegment key_segment_from_json(const Value& v, const std::string& path) {
  arrangement::KeySegment k;
  k.start_ppq = num_or(v, path, "startPpq", 0.0);
  k.end_ppq = num_or(v, path, "endPpq", 0.0);
  k.tonic_pc = pitch_class_or(v, path, "tonicPc");
  k.mode = enum_or(v, path, "mode", arrangement::KeyMode::kUnknown);
  return k;
}

void annotation_from_json(const Value& v, const std::string& path,
                          arrangement::ProjectAnnotation* a) {
  a->tempo_confidence = float_or(v, path, "tempoConfidence", 0.0f);
  for_each_element(v, path, "keys", [a](const Value& kv, const std::string& p) {
    a->keys.push_back(key_segment_from_json(kv, p));
  });
  for_each_element(v, path, "chords", [a](const Value& cv, const std::string& p) {
    a->chords.push_back(chord_from_json(cv, p));
  });
  for_each_element(v, path, "sections", [a](const Value& sv, const std::string& p) {
    arrangement::SectionSegment s;
    s.start_ppq = num_or(sv, p, "startPpq", 0.0);
    s.end_ppq = num_or(sv, p, "endPpq", 0.0);
    s.label = str_or(sv, p, "label", "");
    a->sections.push_back(std::move(s));
  });
  for_each_element(v, path, "onsets", [a](const Value& ov, const std::string& p) {
    arrangement::OnsetMarker on;
    on.ppq = num_or(ov, p, "ppq", 0.0);
    on.confidence = float_or(ov, p, "confidence", 0.0f);
    a->onsets.push_back(on);
  });
}

void sidecar_fields_from_json(const Value& v, const std::string& path,
                              arrangement::AssistSidecar* out) {
  out->module_id = str_or(v, path, "moduleId", "");
  out->schema_version = uint_or(v, path, "schemaVersion", 0);
  out->target_track_id = uint_or(v, path, "targetTrackId", 0);
  out->region_start_ppq = num_or(v, path, "regionStartPpq", 0.0);
  out->region_end_ppq = num_or(v, path, "regionEndPpq", 0.0);
}

bool earlier_assist_module_id(const std::string& module_id, std::string* current) {
  static const std::pair<const char*, const char*> kRenamed[] = {
      {"sonare.builtin.chord_tone_generator", "sonare.builtin.chordToneGenerator"},
      {"sonare.builtin.diatonic_harmonizer", "sonare.builtin.diatonicHarmonizer"},
  };
  for (const auto& [old_id, new_id] : kRenamed) {
    if (module_id != old_id) continue;
    *current = new_id;
    return true;
  }
  return false;
}

bool sidecar_from_borrowed_payload(const std::string& encoded_payload,
                                   arrangement::AssistSidecar* out, size_t max_payload_bytes) {
  return sonare::base64_decode(encoded_payload, &out->payload, max_payload_bytes);
}

// Metadata is assigned before payload decoding, preserving the installed
// helper's field-by-field mutation and metadata-first error precedence. A
// malformed payload therefore leaves the decoded metadata in place while the
// caller records the failed decode.
bool sidecar_from_json(const Value& v, const std::string& path, arrangement::AssistSidecar* out,
                       size_t max_payload_bytes) {
  sidecar_fields_from_json(v, path, out);
  static const std::string kEmptyPayload;
  const std::string* payload = strict_string_field(v, path, "payloadB64");
  return sidecar_from_borrowed_payload(payload != nullptr ? *payload : kEmptyPayload, out,
                                       max_payload_bytes);
}

mixing::api::Scene scene_from_value(const Value& v) {
  try {
    // Walk the already-parsed sub-tree. Dumping it back to text and re-parsing it
    // would cost a second full parse under a different (unlimited) resource regime.
    return mixing::api::scene_from_value(v, "scene");
  } catch (const SonareException& error) {
    // A scene nested in a project is malformed persisted input, while the
    // standalone control-thread API reports InvalidParameter.
    throw SonareException(ErrorCode::InvalidFormat, error.what(), error.reason());
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

// Annotation spans the C ABI setters admit: finite non-negative start, finite end after it.
bool valid_annotation_span(double start, double end) noexcept {
  return valid_position_ppq(start) && std::isfinite(end) && end > start;
}

// Clamps a negative decoded gain to silence and reports it as one warning.
void clamp_negative_gain(float* gain, const char* owner, uint32_t id,
                         BoundedDiagnostics* diagnostics) {
  if (!(*gain < 0.0f)) return;
  *gain = 0.0f;
  if (diagnostics != nullptr) {
    diagnostics->warn(
        std::string(owner) + "_gain_clamped",
        std::string(owner) + " " + std::to_string(id) + " had a negative gain; clamped to zero");
  }
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

  // ---- Gains are non-negative ----------------------------------------------
  // A negative gain has a defined repair (silence), and every one reports it the
  // same way, whichever object carries the field.
  for (arrangement::Track& track : arrangement::detail::ProjectSerializerAccess::tracks(*project)) {
    clamp_negative_gain(&track.gain, "track", track.id, diagnostics);
  }
  for (arrangement::EditClip& clip :
       arrangement::detail::ProjectSerializerAccess::clips(*project)) {
    clamp_negative_gain(&clip.gain, "clip", clip.id, diagnostics);
  }

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
                                "tempo segment startPpq must be finite and non-negative"};
    }
    if (!transport::valid_public_tempo_segment(s)) {
      return InvariantViolation{
          "invalid_tempo_segment",
          "tempo segment startPpq or bpm is outside the range the edit API accepts"};
    }
  }
  for (const transport::TimeSignatureSegment& s : project->time_signatures()) {
    if (!valid_position_ppq(s.start_ppq)) {
      return InvariantViolation{"invalid_time_signature_start_ppq",
                                "time signature segment startPpq must be finite and non-negative"};
    }
    if (!transport::valid_public_time_signature_segment(s)) {
      return InvariantViolation{
          "invalid_time_signature_segment",
          "time signature segment startPpq, numerator or denominator is outside the range the "
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

  // ---- Harmonic annotation spans -------------------------------------------
  // The annotation setters admit a span that starts at or after 0 and ends after it,
  // and at most kMaxChordExtensions extensions per chord. Clamping would move or
  // truncate musical content, so a violation is fatal.
  for (const arrangement::KeySegment& key : project->annotation().keys) {
    if (!valid_annotation_span(key.start_ppq, key.end_ppq)) {
      return InvariantViolation{"invalid_annotation_key_span",
                                "annotation key segment must have a finite non-negative "
                                "startPpq and an endPpq after it"};
    }
  }
  for (const arrangement::ChordSymbol& chord : project->annotation().chords) {
    if (!valid_annotation_span(chord.start_ppq, chord.end_ppq)) {
      return InvariantViolation{"invalid_annotation_chord_span",
                                "annotation chord must have a finite non-negative startPpq "
                                "and an endPpq after it"};
    }
    if (chord.extensions.size() > arrangement::kMaxChordExtensions) {
      return InvariantViolation{"invalid_annotation_chord_extensions",
                                "annotation chord carries more than " +
                                    std::to_string(arrangement::kMaxChordExtensions) +
                                    " extensions"};
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
                                    "must have a finite non-negative startPpq and "
                                    "sourceOffsetPpq and a finite positive lengthPpq"};
    }
    if (c.source_offset_seconds.has_value() &&
        !(std::isfinite(*c.source_offset_seconds) && *c.source_offset_seconds >= 0.0)) {
      return InvariantViolation{"invalid_clip_source_offset_seconds",
                                label + "sourceOffsetSec must be finite and non-negative"};
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
                                    "loopLengthPpq must be finite (and non-negative when "
                                    "looping) and loopCrossfadePpq finite and non-negative"};
    }
    if (c.loop_anchor.has_value() &&
        !(std::isfinite(c.loop_anchor->period_seconds) && c.loop_anchor->period_seconds > 0.0 &&
          std::isfinite(c.loop_anchor->phase_seconds))) {
      return InvariantViolation{
          "invalid_clip_loop_anchor",
          label + "loopPeriodSec must be positive and phaseSec must be finite"};
    }
    for (const arrangement::ClipTake& take : c.takes) {
      if (take.id == 0) {
        return InvariantViolation{"invalid_clip_take_id",
                                  label + "carries a take with the reserved id 0"};
      }
      if (!valid_position_ppq(take.source_offset_ppq)) {
        return InvariantViolation{"invalid_clip_take_ppq",
                                  label + "take " + std::to_string(take.id) +
                                      " sourceOffsetPpq must be finite and non-negative"};
      }
      if (take.source_offset_seconds.has_value() &&
          !(std::isfinite(*take.source_offset_seconds) && *take.source_offset_seconds >= 0.0)) {
        return InvariantViolation{"invalid_clip_take_source_offset_seconds",
                                  label + "take " + std::to_string(take.id) +
                                      " sourceOffsetSec must be finite and non-negative"};
      }
    }
    if (!arrangement::detail::valid_clip_take_structure(*project, c, c.takes, c.active_take_id)) {
      return InvariantViolation{
          "invalid_clip_take_structure",
          label +
              "takes must have unique non-zero ids, a valid activeTakeId, and no takes on a "
              "MIDI track"};
    }
    for (const arrangement::ClipCompSegment& segment : c.comp_segments) {
      if (!valid_position_ppq(segment.start_ppq) || !valid_length_ppq(segment.end_ppq) ||
          !(segment.end_ppq > segment.start_ppq)) {
        return InvariantViolation{
            "invalid_clip_comp_segment_ppq",
            label + "comp segment bounds must be finite with 0 <= startPpq < endPpq"};
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
  // Persistent ids identify edits; typed lane identities identify their playback targets.
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
      if (target == 0 && lane.target_kind() == automation::AutomationTargetKind::kOpaque) {
        // Opaque target id 0 predates the non-zero requirement, so documents
        // carrying such a lane exist. It remains unaddressable and is dropped.
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
      if (!lane.valid_target_identity()) {
        return InvariantViolation{
            "invalid_automation_target_identity",
            "track " + std::to_string(track_id) +
                " contains an automation lane with an invalid target identity"};
      }
      const auto existing =
          target != 0 ? first_lane_by_target.find(target) : first_lane_by_target.end();
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
      if (target != 0) first_lane_by_target.emplace(target, kept.size());
      kept.push_back(std::move(lane));
      kept_order.push_back(source_index);
    }

    std::vector<size_t> lane_id_order(kept.size());
    for (size_t i = 0; i < lane_id_order.size(); ++i) lane_id_order[i] = i;
    std::stable_sort(lane_id_order.begin(), lane_id_order.end(),
                     [&kept_order](size_t a, size_t b) { return kept_order[a] < kept_order[b]; });
    for (const size_t index : lane_id_order) {
      automation::AutomationLane& lane = kept[index];
      if (lane.target_kind() != automation::AutomationTargetKind::kOpaque) {
        lane.set_target_param_id(0);
      }
      if (lane.lane_id() == 0) {
        const uint32_t lane_id = project->allocate_automation_lane_id();
        if (lane_id == 0) {
          return InvariantViolation{
              "automation_lane_id_exhausted",
              "project automation lanes exceed the range of persistent lane ids"};
        }
        lane.set_lane_id(lane_id);
      }
    }

    std::vector<automation::AutomationLane> normalized;
    std::vector<size_t> normalized_order;
    normalized.reserve(kept.size());
    normalized_order.reserve(kept.size());
    using TypedLaneKey =
        std::tuple<uint32_t, std::string, std::optional<std::size_t>, std::string, std::string>;
    std::map<TypedLaneKey, size_t> typed_lane_slots;
    for (size_t kept_index = 0; kept_index < kept.size(); ++kept_index) {
      automation::AutomationLane& lane = kept[kept_index];
      if (lane.target_kind() == automation::AutomationTargetKind::kOpaque) {
        normalized.push_back(std::move(lane));
        normalized_order.push_back(kept_order[kept_index]);
        continue;
      }
      const automation::AutomationTargetKind kind = lane.target_kind();
      const auto& identity = lane.target_identity();
      const TypedLaneKey key{static_cast<uint32_t>(kind), identity.send_id, identity.insert_index,
                             identity.processor, identity.param_name};
      const auto existing = typed_lane_slots.find(key);
      if (existing != typed_lane_slots.end()) {
        const size_t existing_index = existing->second;
        if (kept_order[kept_index] > normalized_order[existing_index]) {
          normalized[existing_index] = std::move(lane);  // Last writer wins, in the first slot.
          normalized_order[existing_index] = kept_order[kept_index];
        }
        if (diagnostics != nullptr) {
          if (kind == automation::AutomationTargetKind::kTrackFaderDb ||
              kind == automation::AutomationTargetKind::kTrackPan ||
              kind == automation::AutomationTargetKind::kTrackWidth) {
            diagnostics->warn("duplicate_automation_target_kind",
                              "track " + std::to_string(track_id) +
                                  " carries more than one automation lane for target kind " +
                                  std::to_string(static_cast<uint32_t>(kind)) +
                                  "; kept the last one");
          } else {
            diagnostics->warn("duplicate_automation_target_identity",
                              "track " + std::to_string(track_id) +
                                  " carries more than one automation lane for target identity " +
                                  std::to_string(static_cast<uint32_t>(kind)) +
                                  "; kept the last one");
          }
        }
        continue;
      }
      typed_lane_slots.emplace(key, normalized.size());
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
          "assist sidecar moduleId must be non-empty and must not contain an embedded NUL"};
    }
    if (!valid_position_ppq(sidecar.region_start_ppq)) {
      return InvariantViolation{"invalid_assist_sidecar_region_start_ppq",
                                "assist sidecar regionStartPpq must be finite and non-negative"};
    }
    if (!valid_position_ppq(sidecar.region_end_ppq)) {
      return InvariantViolation{"invalid_assist_sidecar_region_end_ppq",
                                "assist sidecar regionEndPpq must be finite and non-negative"};
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
