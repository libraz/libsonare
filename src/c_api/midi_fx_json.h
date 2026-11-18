#pragma once

#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "c_api/sonare_c_internal.h"
#include "midi/midi_fx.h"
#include "transport/tempo_map.h"
#include "util/json_budget.h"
#include "util/json_key.h"
#include "util/numeric_validation.h"

namespace sonare_c_detail {

namespace midi_fx_json_detail {

namespace json = sonare::util::json;

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

// Earlier snake_case spelling of each key, refused by name rather than as unknown.
struct KeySpelling {
  const char* current;
  const char* earlier;
};
constexpr KeySpelling kKeys[] = {
    {"transposeSemitones", "transpose_semitones"},
    {"velocityScale", "velocity_scale"},
    {"velocityOffset", "velocity_offset"},
    {"velocityGamma", "velocity_gamma"},
    {"quantizePpq", "quantize_ppq"},
    {"quantizeStrength", "quantize_strength"},
    {"chordIntervals", "chord_intervals"},
    {"arpeggiatorIntervals", "arpeggiator_intervals"},
    {"arpeggiatorStepPpq", "arpeggiator_step_ppq"},
    {"arpeggiatorGatePpq", "arpeggiator_gate_ppq"},
    {"humanizePpq", "humanize_ppq"},
    {"humanizeVelocity", "humanize_velocity"},
    {"seed", nullptr},
};

// Refuses an unknown key, naming an earlier spelling's replacement; annotation keys pass.
inline SonareError refuse_unknown_keys(const json::Value& root) {
  for (const auto& entry : root.as_object()) {
    const std::string& key = entry.first;
    if (json::is_annotation_key(key)) continue;
    const KeySpelling* earlier = nullptr;
    bool known = false;
    for (const KeySpelling& spelling : kKeys) {
      known = known || key == spelling.current;
      if (spelling.earlier != nullptr && key == spelling.earlier) earlier = &spelling;
    }
    if (known) continue;
    const std::string message = earlier != nullptr
                                    ? key + " is an earlier spelling; write " + earlier->current
                                    : "unknown MIDI-FX key '" + json::escape_key(key) + "'";
    set_last_error(SONARE_ERROR_INVALID_FORMAT, message.c_str());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  return SONARE_OK;
}

inline bool ppq_frames(double ppq, int64_t* out) {
  if (!sonare::transport::valid_public_ppq(ppq)) return false;
  return sonare::numeric::checked_round_cast(
      ppq * static_cast<double>(sonare::midi::kMidiFxPpqScale), out);
}

}  // namespace midi_fx_json_detail

inline SonareError midi_fx_chain_from_json(const char* config_json,
                                           sonare::midi::MidiFxChain* chain) {
  namespace json = sonare::util::json;
  using namespace midi_fx_json_detail;

  if (config_json == nullptr || chain == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  json::Value root;
  try {
    root = json::admit_strict(config_json);
  } catch (const json::JsonError&) {
    return SONARE_ERROR_INVALID_FORMAT;
  }
  if (!root.is_object()) return SONARE_ERROR_INVALID_FORMAT;
  if (const SonareError key_err = refuse_unknown_keys(root); key_err != SONARE_OK) return key_err;

  sonare::midi::TransposeConfig transpose;
  if (has_number(root, "transposeSemitones")) {
    transpose.enabled = true;
    if (!int_or(root, "transposeSemitones", 0, &transpose.semitones)) {
      return SONARE_ERROR_INVALID_FORMAT;
    }
    chain->set_transpose(transpose);
  }

  sonare::midi::VelocityCurveConfig velocity;
  const bool has_velocity = has_number(root, "velocityScale") ||
                            has_number(root, "velocityOffset") || has_number(root, "velocityGamma");
  if (has_velocity) {
    velocity.enabled = true;
    if (!float_or(root, "velocityScale", 1.0f, &velocity.scale) ||
        !float_or(root, "velocityOffset", 0.0f, &velocity.offset) ||
        !float_or(root, "velocityGamma", 1.0f, &velocity.gamma) || velocity.gamma <= 0.0f) {
      return SONARE_ERROR_INVALID_FORMAT;
    }
    chain->set_velocity_curve(velocity);
  }

  if (has_number(root, "quantizePpq")) {
    const double grid_ppq = number_or(root, "quantizePpq", 0.0);
    const double strength = number_or(root, "quantizeStrength", 1.0);
    int64_t grid_frames = 0;
    if (!std::isfinite(grid_ppq) || grid_ppq <= 0.0 || !ppq_frames(grid_ppq, &grid_frames) ||
        !std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
      return SONARE_ERROR_INVALID_FORMAT;
    }
    sonare::midi::QuantizeConfig quantize;
    quantize.enabled = true;
    quantize.grid_frames = std::max<int64_t>(1, grid_frames);
    quantize.strength = static_cast<float>(strength);
    chain->set_quantize(quantize);
  }

  if (const json::Value* intervals = root.find("chordIntervals")) {
    if (!intervals->is_array()) return SONARE_ERROR_INVALID_FORMAT;
    sonare::midi::ChordConfig chord;
    chord.enabled = true;
    const auto& values = intervals->as_array();
    if (values.empty() || values.size() > sonare::midi::ChordConfig::kMaxChordNotes) {
      return SONARE_ERROR_INVALID_FORMAT;
    }
    chord.count = values.size();
    for (size_t i = 0; i < values.size(); ++i) {
      if (!values[i].is_number() ||
          !sonare::numeric::checked_integral_cast(values[i].as_number(), &chord.intervals[i])) {
        return SONARE_ERROR_INVALID_FORMAT;
      }
    }
    chain->set_chord(chord);
  }

  if (const json::Value* arp_intervals = root.find("arpeggiatorIntervals")) {
    if (!arp_intervals->is_array()) return SONARE_ERROR_INVALID_FORMAT;
    const auto& values = arp_intervals->as_array();
    if (values.empty() || values.size() > sonare::midi::ArpeggiatorConfig::kMaxArpSteps) {
      return SONARE_ERROR_INVALID_FORMAT;
    }
    const double step_ppq = number_or(root, "arpeggiatorStepPpq", 0.0);
    int64_t step_frames = 0;
    if (!std::isfinite(step_ppq) || step_ppq <= 0.0 || !ppq_frames(step_ppq, &step_frames)) {
      return SONARE_ERROR_INVALID_FORMAT;
    }
    // Gate defaults to a full-length (legato) step when omitted.
    const double gate_ppq = number_or(root, "arpeggiatorGatePpq", step_ppq);
    int64_t gate_frames = 0;
    if (!std::isfinite(gate_ppq) || gate_ppq <= 0.0 || !ppq_frames(gate_ppq, &gate_frames)) {
      return SONARE_ERROR_INVALID_FORMAT;
    }
    sonare::midi::ArpeggiatorConfig arpeggiator;
    arpeggiator.enabled = true;
    arpeggiator.steps = values.size();
    for (size_t i = 0; i < values.size(); ++i) {
      if (!values[i].is_number() || !sonare::numeric::checked_integral_cast(
                                        values[i].as_number(), &arpeggiator.intervals[i])) {
        return SONARE_ERROR_INVALID_FORMAT;
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
    if (!int_or(root, "humanizeVelocity", 0, &velocity_amount) || !int_or(root, "seed", 0, &seed) ||
        !ppq_frames(timing_ppq, &timing_frames)) {
      return SONARE_ERROR_INVALID_FORMAT;
    }
    if (!std::isfinite(timing_ppq) || timing_ppq < 0.0 || velocity_amount < 0 ||
        velocity_amount > 127 || seed < 0) {
      return SONARE_ERROR_INVALID_FORMAT;
    }
    sonare::midi::HumanizeConfig humanize;
    humanize.enabled = true;
    humanize.seed = static_cast<uint32_t>(seed);
    humanize.timing_frames = timing_frames;
    humanize.velocity_amount = velocity_amount;
    chain->set_humanize(humanize);
  }

  chain->prepare();
  return SONARE_OK;
}

}  // namespace sonare_c_detail
