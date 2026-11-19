#include "midi/assist/modules/generator_params.h"

#include <cmath>

#include "util/json_budget.h"
#include "util/json_schema.h"

namespace sonare::midi::assist::modules {

namespace {

namespace json = sonare::util::json;
namespace schema = sonare::util::json::schema;

bool read_u8(const json::Value& root, const char* key, uint8_t* out, std::string* error) {
  const json::Value* value = root.find(key);
  if (value == nullptr) return true;
  if (!value->is_number()) {
    *error = schema::must_be(schema::child_path("", key), schema::Kind::Number);
    return false;
  }
  const double number = value->as_number();
  if (!std::isfinite(number) || number < 0.0 || number > 127.0 || number != std::floor(number)) {
    *error = schema::refusal(schema::child_path("", key), schema::integer_range_predicate(0, 127));
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
    *error = schema::must_be(schema::child_path("", key), schema::Kind::Number);
    return false;
  }
  const double number = value->as_number();
  if (!std::isfinite(number) || number < 0.0 || number > 4294967295.0 ||
      number != std::floor(number)) {
    *error = schema::refusal(schema::child_path("", key),
                             schema::integer_range_predicate(0, 4294967295.0));
    return false;
  }
  *out = static_cast<arrangement::ClipId>(number);
  return true;
}

}  // namespace

bool read_generator_params(const std::string& params_json, GeneratorParams* out,
                           std::string* out_error,
                           const resource::ProjectImportResourceLimits& limits) {
  if (out == nullptr || out_error == nullptr) return false;
  if (params_json.empty()) return true;

  json::Value root;
  try {
    root = json::admit_strict(params_json, limits);
  } catch (const json::JsonError& error) {
    *out_error = error.what();
    return false;
  }
  if (!root.is_object()) {
    *out_error = schema::root_must_be_object();
    return false;
  }

  if (!read_clip_id(root, "target_clip_id", &out->target_clip_id, out_error)) return false;
  if (!read_clip_id(root, "source_clip_id", &out->source_clip_id, out_error)) return false;
  if (!read_u8(root, "low_note", &out->low_note, out_error)) return false;
  if (!read_u8(root, "high_note", &out->high_note, out_error)) return false;
  if (!read_u8(root, "base_velocity", &out->base_velocity, out_error)) return false;

  if (const json::Value* value = root.find("velocity_scale")) {
    if (!value->is_number()) {
      *out_error = schema::must_be("velocity_scale", schema::Kind::Number);
      return false;
    }
    const double scale = value->as_number();
    if (!std::isfinite(scale) || scale <= 0.0 || scale > 2.0) {
      *out_error = schema::refusal("velocity_scale", "must be finite and in (0, 2]");
      return false;
    }
    out->velocity_scale = static_cast<float>(scale);
  }

  if (out->low_note > out->high_note) {
    *out_error = schema::refusal("low_note", "must not be above high_note");
    return false;
  }
  if (out->base_velocity == 0) {
    *out_error = schema::refusal("base_velocity", schema::integer_range_predicate(1, 127));
    return false;
  }
  return true;
}

}  // namespace sonare::midi::assist::modules
