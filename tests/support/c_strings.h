/// @file c_strings.h
/// @brief Calls a C-ABI producer of a caller-owned string and takes the result.

#pragma once

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>

namespace sonare_test {

/// Calls @p fn(args..., &out), requires success, and returns the string after
/// releasing it with sonare_free_string.
template <typename Fn, typename... Args>
std::string c_string(Fn fn, Args&&... args) {
  char* out = nullptr;
  const SonareError err = fn(std::forward<Args>(args)..., &out);
  INFO(sonare_last_error_message());
  REQUIRE(err == SONARE_OK);
  REQUIRE(out != nullptr);
  std::string text(out);
  sonare_free_string(out);
  return text;
}

/// Calls @p fn(args..., &out) expecting a refusal; requires the output to stay
/// NULL and returns the code.
template <typename Fn, typename... Args>
SonareError c_string_error(Fn fn, Args&&... args) {
  char* out = reinterpret_cast<char*>(&out);
  const SonareError err = fn(std::forward<Args>(args)..., &out);
  REQUIRE(err != SONARE_OK);
  REQUIRE(out == nullptr);
  return err;
}

}  // namespace sonare_test
