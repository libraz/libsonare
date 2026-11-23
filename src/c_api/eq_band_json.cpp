#include "c_api/eq_band_json.h"

#include <algorithm>
#include <string>
#include <vector>

#include "mastering/eq/eq_band_value.h"
#include "sonare_c_internal.h"
#include "util/json_budget.h"
#include "util/json_key.h"
#include "util/json_schema.h"

namespace {

constexpr const char* kRoot = "band";

// A band object is the caller's argument, so every refusal of it is InvalidParameter under `band`.
[[noreturn]] void refuse_band(const std::string& message) {
  throw sonare_c_detail::SonareException(sonare::ErrorCode::InvalidParameter, message);
}

// A band fragment refuses its first unknown key by name; `$` and `x-` keys are exempt as they
// are in a scene.
void refuse_unknown_band_keys(const sonare::util::json::Value& json) {
  const auto& known = sonare::mastering::eq::eq_band_known_keys();
  for (const auto& entry : json.as_object()) {
    const std::string& key = entry.first;
    if (std::find(known.begin(), known.end(), key) != known.end() ||
        sonare::util::json::is_annotation_key(key)) {
      continue;
    }
    throw sonare::SonareParameterRefusal::unknown(
        sonare::ErrorCode::InvalidParameter, sonare::util::json::schema::child_path(kRoot, key));
  }
}

}  // namespace

namespace sonare::c_api {

sonare::mastering::eq::EqBand parse_eq_band_json(const char* band_json) {
  if (!band_json) refuse_band("band_json must not be NULL");
  sonare::util::json::Value json;
  try {
    json = sonare::util::json::admit_strict(band_json);
  } catch (const sonare::util::json::JsonError& ex) {
    throw sonare::SonareParameterRefusal::predicate(
        sonare::ErrorCode::InvalidParameter, kRoot,
        std::string("must be valid JSON: ") + ex.what());
  }
  // Field parsing and validation are shared with the scene walker; it runs first so an earlier
  // spelling of a known key is refused naming the current one rather than as an unknown key.
  sonare::mastering::eq::EqBand band;
  try {
    band = sonare::mastering::eq::eq_band_from_value(json, kRoot);
  } catch (const sonare::SonareParameterRefusal& ex) {
    if (ex.code() != sonare::ErrorCode::InvalidFormat) throw;
    // Keep the refusal's form and subjects so each surface still picks the class from its kind.
    throw ex.respelled(
        sonare::ErrorCode::InvalidParameter, [](const std::string& subject) { return subject; },
        false);
  } catch (const sonare::SonareException& ex) {
    if (ex.code() != sonare::ErrorCode::InvalidFormat) throw;
    refuse_band(ex.what());
  }
  refuse_unknown_band_keys(json);
  return band;
}

}  // namespace sonare::c_api
