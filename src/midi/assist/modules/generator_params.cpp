#include "midi/assist/modules/generator_params.h"

#include <cmath>

#include "util/json.h"

namespace sonare::midi::assist::modules {

namespace {

namespace json = sonare::util::json;

bool read_u8(const json::Value& root, const char* key, uint8_t* out, std::string* error) {
  const json::Value* value = root.find(key);
  if (value == nullptr) return true;
  if (!value->is_number()) {
    *error = std::string(key) + " must be a number";
    return false;
  }
  const double number = value->as_number();
  if (!std::isfinite(number) || number < 0.0 || number > 127.0 || number != std::floor(number)) {
    *error = std::string(key) + " must be an integer in 0..127";
    return false;
  }
  *out = static_cast<uint8_t>(number);
  return true;
}

bool read_clip_id(const json::Value& root, const char* key, arrangement::ClipId* out,
                  std::string* error) {
  const json::Value* value = root.find(key);
  if (value == nullptr) return true;
  if (!value->is_number()) {
    *error = std::string(key) + " must be a number";
    return false;
  }
  const double number = value->as_number();
  if (!std::isfinite(number) || number < 0.0 || number > 4294967295.0 ||
      number != std::floor(number)) {
    *error = std::string(key) + " must be a non-negative integer clip id";
    return false;
  }
  *out = static_cast<arrangement::ClipId>(number);
  return true;
}

}  // namespace

bool read_generator_params(const std::string& params_json, GeneratorParams* out,
                           std::string* out_error) {
  if (out == nullptr || out_error == nullptr) return false;
  if (params_json.empty()) return true;

  json::Value root;
  try {
    root = json::parse_strict(params_json);
  } catch (const json::JsonError& error) {
    *out_error = std::string("params_json is not valid JSON: ") + error.what();
    return false;
  }
  if (!root.is_object()) {
    *out_error = "params_json must be a JSON object";
    return false;
  }

  if (!read_clip_id(root, "target_clip_id", &out->target_clip_id, out_error)) return false;
  if (!read_clip_id(root, "source_clip_id", &out->source_clip_id, out_error)) return false;
  if (!read_u8(root, "low_note", &out->low_note, out_error)) return false;
  if (!read_u8(root, "high_note", &out->high_note, out_error)) return false;
  if (!read_u8(root, "base_velocity", &out->base_velocity, out_error)) return false;

  if (const json::Value* value = root.find("velocity_scale")) {
    if (!value->is_number()) {
      *out_error = "velocity_scale must be a number";
      return false;
    }
    const double scale = value->as_number();
    if (!std::isfinite(scale) || scale <= 0.0 || scale > 2.0) {
      *out_error = "velocity_scale must be within (0, 2]";
      return false;
    }
    out->velocity_scale = static_cast<float>(scale);
  }

  if (out->low_note > out->high_note) {
    *out_error = "low_note must not be above high_note";
    return false;
  }
  if (out->base_velocity == 0) {
    *out_error = "base_velocity must be within 1..127";
    return false;
  }
  return true;
}

}  // namespace sonare::midi::assist::modules
