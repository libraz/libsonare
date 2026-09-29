#include "c_api/eq_band_json.h"

#include <string>

#include "mastering/eq/eq_band_value.h"
#include "sonare_c_internal.h"
#include "util/json_budget.h"

namespace {

[[noreturn]] void invalid_eq_json(const std::string& message) {
  throw sonare_c_detail::SonareException(sonare::ErrorCode::InvalidParameter,
                                         "sonare_eq_set_band: " + message);
}

// A syntactically malformed document exits as InvalidFormat across the whole C
// ABI (midi_fx_json.h, parse_scene_json); a well-formed document carrying a bad
// field stays InvalidParameter.
[[noreturn]] void malformed_eq_json(const std::string& message) {
  throw sonare_c_detail::SonareException(sonare::ErrorCode::InvalidFormat,
                                         "sonare_eq_set_band: " + message);
}

}  // namespace

namespace sonare::c_api {

sonare::mastering::eq::EqBand parse_eq_band_json(const char* band_json) {
  if (!band_json) invalid_eq_json("band_json must not be null");
  sonare::util::json::Value json;
  try {
    json = sonare::util::json::admit_strict(band_json);
  } catch (const sonare::util::json::JsonError& ex) {
    malformed_eq_json(std::string("invalid JSON: ") + ex.what());
  }
  // Field parsing and validation are shared with the scene walker; this call
  // keeps the "sonare_eq_set_band: " prefix and the InvalidParameter code every
  // existing caller already handles.
  return sonare::mastering::eq::eq_band_from_value(json, "sonare_eq_set_band: ");
}

}  // namespace sonare::c_api
