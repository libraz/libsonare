#pragma once

/// @file param_spelling.h
/// @brief Refusal of an unknown assistant request param key.

#include <string>
#include <vector>

#include "util/exception.h"
#include "util/json_schema.h"

namespace sonare::mastering::assistant {

/// @brief Refuses `key` as an unknown key of a flat param list, listing the keys the reader takes.
[[noreturn]] inline void refuse_unknown_param(const std::string& key,
                                              const std::vector<std::string>& accepted) {
  throw SonareParameterRefusal::unknown(ErrorCode::InvalidParameter, key,
                                        sonare::util::json::schema::expected_keys(accepted))
      .as_caller_spelled();
}

}  // namespace sonare::mastering::assistant
