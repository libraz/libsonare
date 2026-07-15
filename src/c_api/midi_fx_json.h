#pragma once

#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "midi/midi_fx.h"
#include "transport/tempo_map.h"
#include "util/json.h"
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
    root = json::parse_strict(config_json);
  } catch (const json::JsonError&) {
    return SONARE_ERROR_INVALID_FORMAT;
  }
  if (!root.is_object()) return SONARE_ERROR_INVALID_PARAMETER;

  sonare::midi::TransposeConfig transpose;
  if (has_number(root, "transpose_semitones")) {
    transpose.enabled = true;
    if (!int_or(root, "transpose_semitones", 0, &transpose.semitones)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    chain->set_transpose(transpose);
  }

  sonare::midi::VelocityCurveConfig velocity;
  const bool has_velocity = has_number(root, "velocity_scale") ||
                            has_number(root, "velocity_offset") ||
                            has_number(root, "velocity_gamma");
  if (has_velocity) {
    velocity.enabled = true;
    velocity.scale = static_cast<float>(number_or(root, "velocity_scale", 1.0));
    velocity.offset = static_cast<float>(number_or(root, "velocity_offset", 0.0));
    velocity.gamma = static_cast<float>(number_or(root, "velocity_gamma", 1.0));
    if (!std::isfinite(velocity.scale) || !std::isfinite(velocity.offset) ||
        !std::isfinite(velocity.gamma) || velocity.gamma <= 0.0f) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    chain->set_velocity_curve(velocity);
  }

  if (has_number(root, "quantize_ppq")) {
    const double grid_ppq = number_or(root, "quantize_ppq", 0.0);
    const double strength = number_or(root, "quantize_strength", 1.0);
    int64_t grid_frames = 0;
    if (!std::isfinite(grid_ppq) || grid_ppq <= 0.0 || !ppq_frames(grid_ppq, &grid_frames) ||
        !std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::midi::QuantizeConfig quantize;
    quantize.enabled = true;
    quantize.grid_frames = std::max<int64_t>(1, grid_frames);
    quantize.strength = static_cast<float>(strength);
    chain->set_quantize(quantize);
  }

  if (const json::Value* intervals = root.find("chord_intervals")) {
    if (!intervals->is_array()) return SONARE_ERROR_INVALID_PARAMETER;
    sonare::midi::ChordConfig chord;
    chord.enabled = true;
    const auto& values = intervals->as_array();
    if (values.empty() || values.size() > sonare::midi::ChordConfig::kMaxChordNotes) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    chord.count = values.size();
    for (size_t i = 0; i < values.size(); ++i) {
      if (!values[i].is_number() ||
          !sonare::numeric::checked_integral_cast(values[i].as_number(), &chord.intervals[i])) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
    }
    chain->set_chord(chord);
  }

  if (const json::Value* arp_intervals = root.find("arpeggiator_intervals")) {
    if (!arp_intervals->is_array()) return SONARE_ERROR_INVALID_PARAMETER;
    const auto& values = arp_intervals->as_array();
    if (values.empty() || values.size() > sonare::midi::ArpeggiatorConfig::kMaxArpSteps) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    const double step_ppq = number_or(root, "arpeggiator_step_ppq", 0.0);
    int64_t step_frames = 0;
    if (!std::isfinite(step_ppq) || step_ppq <= 0.0 || !ppq_frames(step_ppq, &step_frames)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    // Gate defaults to a full-length (legato) step when omitted.
    const double gate_ppq = number_or(root, "arpeggiator_gate_ppq", step_ppq);
    int64_t gate_frames = 0;
    if (!std::isfinite(gate_ppq) || gate_ppq <= 0.0 || !ppq_frames(gate_ppq, &gate_frames)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::midi::ArpeggiatorConfig arpeggiator;
    arpeggiator.enabled = true;
    arpeggiator.steps = values.size();
    for (size_t i = 0; i < values.size(); ++i) {
      if (!values[i].is_number() || !sonare::numeric::checked_integral_cast(
                                        values[i].as_number(), &arpeggiator.intervals[i])) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
    }
    arpeggiator.step_frames = std::max<int64_t>(1, step_frames);
    arpeggiator.gate_frames = std::min(std::max<int64_t>(1, gate_frames), arpeggiator.step_frames);
    chain->set_arpeggiator(arpeggiator);
  }

  const bool has_humanize = has_number(root, "humanize_ppq") ||
                            has_number(root, "humanize_velocity") || has_number(root, "seed");
  if (has_humanize) {
    const double timing_ppq = number_or(root, "humanize_ppq", 0.0);
    int velocity_amount = 0;
    int seed = 0;
    int64_t timing_frames = 0;
    if (!int_or(root, "humanize_velocity", 0, &velocity_amount) ||
        !int_or(root, "seed", 0, &seed) || !ppq_frames(timing_ppq, &timing_frames)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!std::isfinite(timing_ppq) || timing_ppq < 0.0 || velocity_amount < 0 ||
        velocity_amount > 127 || seed < 0) {
      return SONARE_ERROR_INVALID_PARAMETER;
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
