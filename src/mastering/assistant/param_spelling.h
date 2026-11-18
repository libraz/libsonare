#pragma once

/// @file param_spelling.h
/// @brief Refusal of snake_case spellings of assistant request param keys.

#include <initializer_list>
#include <string>

#include "util/exception.h"

namespace sonare::mastering::assistant {

/// @brief Throw InvalidParameter when `key` is the snake_case spelling of one of `known`.
/// @details Assistant param keys are camelCase only. A key that is not a known
///          spelling falls through so the caller reports it as unknown.
inline void refuse_snake_case_param(const std::string& key,
                                    std::initializer_list<const char*> known) {
  if (key.find('_') == std::string::npos) {
    return;
  }
  std::string camel;
  bool upper_next = false;
  for (const char c : key) {
    if (c == '_') {
      upper_next = true;
    } else if (upper_next && c >= 'a' && c <= 'z') {
      camel.push_back(static_cast<char>(c - 'a' + 'A'));
      upper_next = false;
    } else {
      camel.push_back(c);
      upper_next = false;
    }
  }
  for (const char* name : known) {
    if (camel == name) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "assistant param " + key + " is an earlier spelling; write " + camel);
    }
  }
}

}  // namespace sonare::mastering::assistant
