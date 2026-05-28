// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file json_schema.h
/// @brief Lightweight schema validation helpers for util::json::Value trees.
/// @details Header-only, no extra dependencies. Designed to be embedded inside
///          subsystem-specific validators (e.g. realtime voice changer presets)
///          so the per-field error messages stay tailored, while the boilerplate
///          (`is_object`, key allowlist, finite/range checks) lives in one place.

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

#include "util/json.h"

namespace sonare::util::json::schema {

/// @brief Rejects any object key not in the allow-list.
/// @param object  The Value being checked; must be an object.
/// @param keys    Allow-listed keys (string_view literals are fine).
/// @param path    Human-readable JSON path used in error messages.
/// @param error   Optional output for the first failure reason.
inline bool has_allowed_keys(const sonare::util::json::Value& object,
                             std::initializer_list<std::string_view> keys, std::string_view path,
                             std::string* error) {
  if (!object.is_object()) {
    if (error) *error = std::string(path) + " must be an object";
    return false;
  }
  for (const auto& [key, _value] : object.as_object()) {
    bool allowed = false;
    for (std::string_view allowed_key : keys) {
      if (key == allowed_key) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      if (error) *error = "unknown field: " + std::string(path) + "." + key;
      return false;
    }
  }
  return true;
}

/// @brief Confirms an object contains @p key (presence only; type is unchecked).
inline bool require_key(const sonare::util::json::Value& object, const char* key,
                        std::string_view path, std::string* error) {
  if (object.find(key) != nullptr) return true;
  if (error) *error = "missing field: " + std::string(path) + "." + key;
  return false;
}

/// @brief Verifies @p key exists, is numeric, finite, and inside `[lo, hi]`.
/// @param integer When true, also requires the value to have no fractional part.
inline bool require_number(const sonare::util::json::Value& object, const char* key, double lo,
                           double hi, std::string_view path, std::string* error,
                           bool integer = false) {
  const auto* value = object.find(key);
  if (!value || !value->is_number()) {
    if (error) *error = "field must be numeric: " + std::string(path) + "." + key;
    return false;
  }
  const double number = value->as_number();
  if (!std::isfinite(number)) {
    if (error) *error = "field must be finite: " + std::string(path) + "." + key;
    return false;
  }
  if (integer && std::floor(number) != number) {
    if (error) *error = "field must be an integer: " + std::string(path) + "." + key;
    return false;
  }
  if (number < lo || number > hi) {
    if (error) *error = "field is out of range: " + std::string(path) + "." + key;
    return false;
  }
  return true;
}

/// @brief Verifies @p key exists, is a string, and its length is in `[min_len, max_len]`.
inline bool require_string(const sonare::util::json::Value& object, const char* key,
                           std::size_t min_len, std::size_t max_len, std::string_view path,
                           std::string* error) {
  const auto* value = object.find(key);
  if (!value || !value->is_string()) {
    if (error) *error = "field must be a string: " + std::string(path) + "." + key;
    return false;
  }
  const auto& s = value->as_string();
  if (s.size() < min_len || s.size() > max_len) {
    if (error)
      *error = "field length is out of range: " + std::string(path) + "." + std::string(key);
    return false;
  }
  return true;
}

}  // namespace sonare::util::json::schema
