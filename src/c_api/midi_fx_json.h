#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "midi/midi_fx.h"
#include "sonare_c.h"
#include "util/json.h"

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

inline int int_or(const json::Value& obj, const char* key, int fallback) {
  const double value = number_or(obj, key, static_cast<double>(fallback));
  if (!std::isfinite(value)) return fallback;
  return static_cast<int>(std::lround(value));
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
    transpose.semitones = int_or(root, "transpose_semitones", 0);
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
    if (!std::isfinite(grid_ppq) || grid_ppq <= 0.0 || !std::isfinite(strength) || strength < 0.0 ||
        strength > 1.0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::midi::QuantizeConfig quantize;
    quantize.enabled = true;
    quantize.grid_frames = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(grid_ppq * sonare::midi::kMidiFxPpqScale)));
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
      if (!values[i].is_number()) return SONARE_ERROR_INVALID_PARAMETER;
      chord.intervals[i] = static_cast<int>(std::lround(values[i].as_number()));
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
    if (!std::isfinite(step_ppq) || step_ppq <= 0.0) return SONARE_ERROR_INVALID_PARAMETER;
    // Gate defaults to a full-length (legato) step when omitted.
    const double gate_ppq = number_or(root, "arpeggiator_gate_ppq", step_ppq);
    if (!std::isfinite(gate_ppq) || gate_ppq <= 0.0) return SONARE_ERROR_INVALID_PARAMETER;
    sonare::midi::ArpeggiatorConfig arpeggiator;
    arpeggiator.enabled = true;
    arpeggiator.steps = values.size();
    for (size_t i = 0; i < values.size(); ++i) {
      if (!values[i].is_number()) return SONARE_ERROR_INVALID_PARAMETER;
      arpeggiator.intervals[i] = static_cast<int>(std::lround(values[i].as_number()));
    }
    arpeggiator.step_frames = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(step_ppq * sonare::midi::kMidiFxPpqScale)));
    const int64_t gate_frames = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(gate_ppq * sonare::midi::kMidiFxPpqScale)));
    arpeggiator.gate_frames = std::min(gate_frames, arpeggiator.step_frames);
    chain->set_arpeggiator(arpeggiator);
  }

  const bool has_humanize = has_number(root, "humanize_ppq") ||
                            has_number(root, "humanize_velocity") || has_number(root, "seed");
  if (has_humanize) {
    const double timing_ppq = number_or(root, "humanize_ppq", 0.0);
    const int velocity_amount = int_or(root, "humanize_velocity", 0);
    const int seed = int_or(root, "seed", 0);
    if (!std::isfinite(timing_ppq) || timing_ppq < 0.0 || velocity_amount < 0 ||
        velocity_amount > 127 || seed < 0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::midi::HumanizeConfig humanize;
    humanize.enabled = true;
    humanize.seed = static_cast<uint32_t>(seed);
    humanize.timing_frames =
        static_cast<int64_t>(std::llround(timing_ppq * sonare::midi::kMidiFxPpqScale));
    humanize.velocity_amount = velocity_amount;
    chain->set_humanize(humanize);
  }

  chain->prepare();
  return SONARE_OK;
}

}  // namespace sonare_c_detail
