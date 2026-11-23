#pragma once

#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "c_api/sonare_c_internal.h"
#include "midi/midi_fx.h"
#include "transport/tempo_map.h"
#include "util/exception.h"
#include "util/json_budget.h"
#include "util/json_key.h"
#include "util/json_schema.h"
#include "util/numeric_validation.h"

namespace sonare_c_detail {

namespace midi_fx_json_detail {

namespace json = sonare::util::json;
namespace schema = sonare::util::json::schema;

// config_json is a per-call argument, so its refusals are InvalidParameter rooted at
// `config`, the name every surface gives it, and a wrong type is a shape refusal.
// The keys are already the caller's, so each refusal is marked caller-spelled.
inline constexpr const char* kConfigSubject = "config";

inline std::string at(std::string_view key) { return schema::child_path(kConfigSubject, key); }

[[noreturn]] inline void refuse(const std::string& subject, const std::string& predicate) {
  throw sonare::SonareParameterRefusal::predicate(sonare::ErrorCode::InvalidParameter, subject,
                                                  predicate)
      .as_caller_spelled();
}

[[noreturn]] inline void refuse_at(std::string_view key, const std::string& predicate) {
  refuse(at(key), predicate);
}

[[noreturn]] inline void refuse_type(const std::string& subject, schema::Kind kind) {
  throw sonare::SonareParameterRefusal::wrong_type(
      sonare::ErrorCode::InvalidParameter, subject,
      std::string("must be ") + schema::kind_text(kind))
      .as_caller_spelled();
}

inline double number_or(const json::Value& obj, const char* key, double fallback) {
  const json::Value* value = obj.find(key);
  return value != nullptr && value->is_number() ? value->as_number() : fallback;
}

inline bool has_number(const json::Value& obj, const char* key) {
  const json::Value* value = obj.find(key);
  return value != nullptr && value->is_number();
}

inline bool int_or(const json::Value& obj, const char* key, int fallback, int* out) {
  const json::Value* value = obj.find(key);
  if (value == nullptr || !value->is_number()) {
    *out = fallback;
    return true;
  }
  return sonare::numeric::checked_integral_cast(value->as_number(), out);
}

// Float sibling of int_or: as_number() is a full double, so the narrowing has
// to be range-checked rather than performed and inspected afterwards.
inline bool float_or(const json::Value& obj, const char* key, float fallback, float* out) {
  const json::Value* value = obj.find(key);
  if (value == nullptr || !value->is_number()) {
    *out = fallback;
    return true;
  }
  return sonare::numeric::checked_float_cast(value->as_number(), out);
}

// Each key the config reads, and whether it holds an array rather than a number.
struct KeySpelling {
  const char* current;
  bool array = false;
};
constexpr KeySpelling kKeys[] = {
    {"transposeSemitones"},
    {"velocityScale"},
    {"velocityOffset"},
    {"velocityGamma"},
    {"quantizePpq"},
    {"quantizeStrength"},
    {"chordIntervals", true},
    {"arpeggiatorIntervals", true},
    {"arpeggiatorStepPpq"},
    {"arpeggiatorGatePpq"},
    {"humanizePpq"},
    {"humanizeVelocity"},
    {"seed"},
};

// Refuses an unknown key by listing the keys the config reads; annotation keys pass.
inline void refuse_unknown_keys(const json::Value& root) {
  for (const auto& entry : root.as_object()) {
    const std::string& key = entry.first;
    if (json::is_annotation_key(key)) continue;
    const bool known =
        std::any_of(std::begin(kKeys), std::end(kKeys),
                    [&key](const KeySpelling& spelling) { return key == spelling.current; });
    if (known) continue;
    std::vector<std::string> accepted;
    for (const KeySpelling& spelling : kKeys) accepted.emplace_back(spelling.current);
    throw sonare::SonareParameterRefusal::unknown(sonare::ErrorCode::InvalidParameter, at(key),
                                                  std::string(), schema::expected_keys(accepted))
        .as_caller_spelled();
  }
}

// A present key holding the wrong kind is refused, never read as absent.
inline void refuse_wrong_kinds(const json::Value& root) {
  for (const KeySpelling& spelling : kKeys) {
    const json::Value* value = root.find(spelling.current);
    if (value == nullptr) continue;
    if (spelling.array ? value->is_array() : value->is_number()) continue;
    refuse_type(at(spelling.current), spelling.array ? schema::Kind::Array : schema::Kind::Number);
  }
}

inline bool ppq_frames(double ppq, int64_t* out) {
  if (!sonare::transport::valid_public_ppq(ppq)) return false;
  return sonare::numeric::checked_round_cast(
      ppq * static_cast<double>(sonare::midi::kMidiFxPpqScale), out);
}

}  // namespace midi_fx_json_detail

/// Reads @p config_json (non-null) into @p chain. Throws SonareParameterRefusal, so it is
/// called inside an entry's exception translation.
inline void midi_fx_chain_from_json(const char* config_json, sonare::midi::MidiFxChain* chain) {
  namespace json = sonare::util::json;
  using namespace midi_fx_json_detail;

  json::Value root;
  try {
    root = json::admit_strict(config_json);
  } catch (const json::JsonError& e) {
    refuse(kConfigSubject, std::string("must be valid JSON: ") + e.what());
  }
  if (!root.is_object()) refuse_type(kConfigSubject, schema::Kind::Object);
  refuse_unknown_keys(root);
  refuse_wrong_kinds(root);

  sonare::midi::TransposeConfig transpose;
  if (has_number(root, "transposeSemitones")) {
    transpose.enabled = true;
    if (!int_or(root, "transposeSemitones", 0, &transpose.semitones)) {
      refuse_at("transposeSemitones", "must be a whole number in range");
    }
    chain->set_transpose(transpose);
  }

  sonare::midi::VelocityCurveConfig velocity;
  const bool has_velocity = has_number(root, "velocityScale") ||
                            has_number(root, "velocityOffset") || has_number(root, "velocityGamma");
  if (has_velocity) {
    velocity.enabled = true;
    for (const auto& [key, fallback, dst] :
         {std::tuple<const char*, float, float*>{"velocityScale", 1.0f, &velocity.scale},
          std::tuple<const char*, float, float*>{"velocityOffset", 0.0f, &velocity.offset},
          std::tuple<const char*, float, float*>{"velocityGamma", 1.0f, &velocity.gamma}}) {
      if (!float_or(root, key, fallback, dst)) {
        refuse_at(key, "must be representable as a 32-bit float");
      }
    }
    if (velocity.gamma <= 0.0f) refuse_at("velocityGamma", "must be > 0");
    chain->set_velocity_curve(velocity);
  }

  if (has_number(root, "quantizePpq")) {
    const double grid_ppq = number_or(root, "quantizePpq", 0.0);
    const double strength = number_or(root, "quantizeStrength", 1.0);
    int64_t grid_frames = 0;
    if (!std::isfinite(grid_ppq) || grid_ppq <= 0.0 || !ppq_frames(grid_ppq, &grid_frames)) {
      refuse_at("quantizePpq", "must be a positive PPQ length in range");
    }
    if (!std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
      refuse_at("quantizeStrength", "must be in [0, 1]");
    }
    sonare::midi::QuantizeConfig quantize;
    quantize.enabled = true;
    quantize.grid_frames = std::max<int64_t>(1, grid_frames);
    quantize.strength = static_cast<float>(strength);
    chain->set_quantize(quantize);
  }

  if (const json::Value* intervals = root.find("chordIntervals")) {
    sonare::midi::ChordConfig chord;
    chord.enabled = true;
    const auto& values = intervals->as_array();
    if (values.empty() || values.size() > sonare::midi::ChordConfig::kMaxChordNotes) {
      refuse_at("chordIntervals", "must hold 1 to " +
                                      std::to_string(sonare::midi::ChordConfig::kMaxChordNotes) +
                                      " intervals");
    }
    chord.count = values.size();
    for (size_t i = 0; i < values.size(); ++i) {
      const std::string element_at = schema::index_path("chordIntervals", i);
      if (!values[i].is_number()) refuse_type(at(element_at), schema::Kind::Number);
      if (!sonare::numeric::checked_integral_cast(values[i].as_number(), &chord.intervals[i])) {
        refuse_at(element_at, "must be a whole number");
      }
    }
    chain->set_chord(chord);
  }

  if (const json::Value* arp_intervals = root.find("arpeggiatorIntervals")) {
    const auto& values = arp_intervals->as_array();
    if (values.empty() || values.size() > sonare::midi::ArpeggiatorConfig::kMaxArpSteps) {
      refuse_at("arpeggiatorIntervals",
                "must hold 1 to " + std::to_string(sonare::midi::ArpeggiatorConfig::kMaxArpSteps) +
                    " intervals");
    }
    const double step_ppq = number_or(root, "arpeggiatorStepPpq", 0.0);
    int64_t step_frames = 0;
    if (!std::isfinite(step_ppq) || step_ppq <= 0.0 || !ppq_frames(step_ppq, &step_frames)) {
      refuse_at("arpeggiatorStepPpq", "must be a positive PPQ length in range");
    }
    // Gate defaults to a full-length (legato) step when omitted.
    const double gate_ppq = number_or(root, "arpeggiatorGatePpq", step_ppq);
    int64_t gate_frames = 0;
    if (!std::isfinite(gate_ppq) || gate_ppq <= 0.0 || !ppq_frames(gate_ppq, &gate_frames)) {
      refuse_at("arpeggiatorGatePpq", "must be a positive PPQ length in range");
    }
    sonare::midi::ArpeggiatorConfig arpeggiator;
    arpeggiator.enabled = true;
    arpeggiator.steps = values.size();
    for (size_t i = 0; i < values.size(); ++i) {
      const std::string element_at = schema::index_path("arpeggiatorIntervals", i);
      if (!values[i].is_number()) refuse_type(at(element_at), schema::Kind::Number);
      if (!sonare::numeric::checked_integral_cast(values[i].as_number(),
                                                  &arpeggiator.intervals[i])) {
        refuse_at(element_at, "must be a whole number");
      }
    }
    arpeggiator.step_frames = std::max<int64_t>(1, step_frames);
    arpeggiator.gate_frames = std::min(std::max<int64_t>(1, gate_frames), arpeggiator.step_frames);
    chain->set_arpeggiator(arpeggiator);
  }

  const bool has_humanize = has_number(root, "humanizePpq") ||
                            has_number(root, "humanizeVelocity") || has_number(root, "seed");
  if (has_humanize) {
    const double timing_ppq = number_or(root, "humanizePpq", 0.0);
    int velocity_amount = 0;
    int seed = 0;
    int64_t timing_frames = 0;
    if (!std::isfinite(timing_ppq) || timing_ppq < 0.0 || !ppq_frames(timing_ppq, &timing_frames)) {
      refuse_at("humanizePpq", "must be a non-negative PPQ length in range");
    }
    if (!int_or(root, "humanizeVelocity", 0, &velocity_amount) || velocity_amount < 0 ||
        velocity_amount > 127) {
      refuse_at("humanizeVelocity", schema::integer_range_predicate(0, 127));
    }
    if (!int_or(root, "seed", 0, &seed) || seed < 0) {
      refuse_at("seed", "must be a non-negative whole number in range");
    }
    sonare::midi::HumanizeConfig humanize;
    humanize.enabled = true;
    humanize.seed = static_cast<uint32_t>(seed);
    humanize.timing_frames = timing_frames;
    humanize.velocity_amount = velocity_amount;
    chain->set_humanize(humanize);
  }

  chain->prepare();
}

}  // namespace sonare_c_detail
