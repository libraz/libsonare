#pragma once

/// @file eq_band_value.h
/// @brief Shared EQ band JSON codec, over an already-parsed util::json::Value.
///
/// The C API's band JSON (`c_api/eq_band_json.cpp`) and the scene walker
/// (`mixing/api/scene_json.cpp`) both build an EqBand from a JSON object and
/// both need to write one back; this is the one place either operation is
/// defined; a caller with raw JSON text parses it itself and hands the
/// resulting Value here, so a scene embedded inside a larger document is not
/// parsed a second time under a different resource budget.

#include <string>
#include <vector>

#include "mastering/eq/eq_band.h"
#include "util/json.h"

namespace sonare::mastering::eq {

/// @brief Parses one EQ band, accepting the same keys/aliases/validation as
///        `sonare_eq_set_band`'s JSON.
/// @param context Prefix for every thrown message (e.g. "sonare_eq_set_band: "
///        or a scene field path), so a caller embedding this in a larger
///        document names where the band came from.
/// @throws SonareException(InvalidParameter) on a value that is not a JSON
///         object, an unrecognised enum spelling, a wrongly-typed field, or a
///         numeric field that is non-finite or out of range.
EqBand eq_band_from_value(const util::json::Value& value, const char* context);

/// @brief Every key `eq_band_from_value` reads: canonical camelCase plus each accepted
///        legacy spelling. A caller reporting unrecognised keys checks against this list.
const std::vector<std::string>& eq_band_known_keys();

/// @brief Serializes one EQ band. Canonical camelCase keys only (no legacy
///        aliases); a field matching @c EqBand{} is omitted, except `enabled`,
///        which is always written.
util::json::Value eq_band_to_value(const EqBand& band);

}  // namespace sonare::mastering::eq
