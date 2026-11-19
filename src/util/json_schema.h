#pragma once

/// @file json_schema.h
/// @brief The one path builder and refusal writer every JSON document reader uses.
/// @details A path runs from the document root, dotted, with "[i]" for an element of a structural
///          array and ".i" for an index that is part of a catalog parameter name; the root itself
///          is the empty path. Every refusal of a document reader is composed here, so the shapes
///          are written once: "unknown key <path>", "duplicate key <path>", "<path> must be <a
///          kind>",
///          "<path> must ..." for a range, an absence or a zero, and "the document must be an
///          object" for the root. Keys are rendered with @ref escape_key so a key cannot forge a
///          line. The require_* helpers are the common checks built on the writer.

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>

#include "util/exception.h"
#include "util/json.h"
#include "util/json_key.h"
#include "util/number_format.h"

namespace sonare::util::json::schema {

/// @brief The path of @p key under @p parent; a key under the root is the key alone.
inline std::string child_path(std::string_view parent, std::string_view key) {
  std::string out(parent);
  if (!out.empty()) out += '.';
  out += escape_key(std::string(key));
  return out;
}

/// @brief The path of element @p index of the structural array at @p parent.
inline std::string index_path(std::string_view parent, std::size_t index) {
  return std::string(parent) + "[" + std::to_string(index) + "]";
}

/// @brief The path of element @p index when the index is part of a catalog parameter name.
inline std::string catalog_index_path(std::string_view parent, std::size_t index) {
  return child_path(parent, std::to_string(index));
}

/// @brief The value types a refusal names.
enum class Kind { String, Number, Boolean, Object, Array };

inline const char* kind_text(Kind kind) {
  switch (kind) {
    case Kind::String:
      return "a string";
    case Kind::Number:
      return "a number";
    case Kind::Boolean:
      return "a boolean";
    case Kind::Object:
      return "an object";
    case Kind::Array:
      return "an array";
  }
  return "a value";
}

/// @brief The refusal of a root that is not an object.
inline std::string root_must_be_object() { return "the document must be an object"; }

/// @brief "unknown key <path>".
inline std::string unknown_key(std::string_view path) { return "unknown key " + std::string(path); }

/// @brief "unknown key <path> (renamed to <new path>)".
inline std::string unknown_key_renamed(std::string_view path, std::string_view renamed_to) {
  return unknown_key(path) + " (renamed to " + std::string(renamed_to) + ")";
}

/// @brief "unknown key <path> (expected <list>)", @p expected already joined by ", ".
inline std::string unknown_key_expected(std::string_view path, std::string_view expected) {
  return unknown_key(path) + " (expected " + std::string(expected) + ")";
}

/// @brief "duplicate key <path>".
inline std::string duplicate_key(std::string_view path) {
  return "duplicate key " + std::string(path);
}

/// @brief "<path> must be <a kind>"; at the root, "the document must be <a kind>".
inline std::string must_be(std::string_view path, Kind kind) {
  if (path.empty() && kind == Kind::Object) return root_must_be_object();
  return (path.empty() ? std::string("the document") : std::string(path)) + " must be " +
         kind_text(kind);
}

/// @brief "<path> <predicate>", for a range, an absence, a zero or any other domain statement.
inline std::string refusal(std::string_view path, std::string_view predicate) {
  return std::string(path) + " " + std::string(predicate);
}

/// @brief @p e with @p parent joined in front of its subject (and rename), under @p code.
inline SonareParameterRefusal relabel_under(const SonareParameterRefusal& e,
                                            std::string_view parent, ErrorCode code) {
  const std::string subject = child_path(parent, e.subject());
  if (e.form() == SonareParameterRefusal::Form::UnknownKey) {
    return SonareParameterRefusal::unknown(
        code, subject, e.renamed_to().empty() ? std::string() : child_path(parent, e.renamed_to()));
  }
  return SonareParameterRefusal::predicate(code, subject, e.predicate());
}

/// @brief Throws @ref relabel_under of @p e.
[[noreturn]] inline void rethrow_under(const SonareParameterRefusal& e, std::string_view parent,
                                       ErrorCode code) {
  throw relabel_under(e, parent, code);
}

/// @brief The shortest decimal spelling of @p value that reads back as the same double.
inline std::string bound_text(double value) {
  constexpr int kMinDigits = 6;
  constexpr int kMaxDigits = 17;
  for (int digits = kMinDigits; digits < kMaxDigits; ++digits) {
    std::string text = format_general(value, digits);
    if (std::strtod(text.c_str(), nullptr) == value) return text;
  }
  return format_general(value, kMaxDigits);
}

/// @brief "must be finite and in [lo, hi]".
inline std::string range_predicate(double lo, double hi) {
  return "must be finite and in [" + bound_text(lo) + ", " + bound_text(hi) + "]";
}

/// @brief The domain of a value held as a 32-bit float, for one with no narrower declared range.
inline constexpr const char* kFloatRangePredicate =
    "must be finite and in [-3.4028235e38, 3.4028235e38]";

/// @brief "must be an integer in [lo, hi]".
inline std::string integer_range_predicate(double lo, double hi) {
  return "must be an integer in [" + bound_text(lo) + ", " + bound_text(hi) + "]";
}

/// @brief "<path> must be given", for a required key the document leaves out.
inline std::string missing_key(std::string_view path) { return refusal(path, "must be given"); }

/// @brief Refuses a non-object at @p path, or any key of it not in @p keys.
inline bool has_allowed_keys(const sonare::util::json::Value& object,
                             std::initializer_list<std::string_view> keys, std::string_view path,
                             std::string* error) {
  if (!object.is_object()) {
    if (error) *error = must_be(path, Kind::Object);
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
      if (error) *error = unknown_key(child_path(path, key));
      return false;
    }
  }
  return true;
}

/// @brief Confirms the object at @p path holds @p key (presence only; type is unchecked).
inline bool require_key(const sonare::util::json::Value& object, const char* key,
                        std::string_view path, std::string* error) {
  if (object.find(key) != nullptr) return true;
  if (error) *error = missing_key(child_path(path, key));
  return false;
}

/// @brief Verifies @p key exists, is a number, finite and inside `[lo, hi]`.
/// @param integer When true, also requires the value to have no fractional part.
inline bool require_number(const sonare::util::json::Value& object, const char* key, double lo,
                           double hi, std::string_view path, std::string* error,
                           bool integer = false) {
  const std::string at = child_path(path, key);
  const auto* value = object.find(key);
  if (!value) {
    if (error) *error = missing_key(at);
    return false;
  }
  if (!value->is_number()) {
    if (error) *error = must_be(at, Kind::Number);
    return false;
  }
  const double number = value->as_number();
  const bool whole = !integer || std::floor(number) == number;
  if (!std::isfinite(number) || !whole || number < lo || number > hi) {
    if (error)
      *error = refusal(at, integer ? integer_range_predicate(lo, hi) : range_predicate(lo, hi));
    return false;
  }
  return true;
}

/// @brief Verifies @p key exists, is a string, and its length is in `[min_len, max_len]`.
inline bool require_string(const sonare::util::json::Value& object, const char* key,
                           std::size_t min_len, std::size_t max_len, std::string_view path,
                           std::string* error) {
  const std::string at = child_path(path, key);
  const auto* value = object.find(key);
  if (!value) {
    if (error) *error = missing_key(at);
    return false;
  }
  if (!value->is_string()) {
    if (error) *error = must_be(at, Kind::String);
    return false;
  }
  const auto& s = value->as_string();
  if (s.size() < min_len || s.size() > max_len) {
    if (error) {
      *error = refusal(at, "must be a string of " + std::to_string(min_len) + " to " +
                               std::to_string(max_len) + " characters");
    }
    return false;
  }
  return true;
}

}  // namespace sonare::util::json::schema
