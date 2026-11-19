// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "serialize/project_serializer.h"
#include "serialize/project_serializer_internal.h"
#include "serialize/project_serializer_private.h"
#include "util/base64.h"
#include "util/exception.h"
#include "util/json_schema.h"
#include "util/numeric_validation.h"

namespace sonare::serialize {
namespace detail {

namespace schema = sonare::util::json::schema;

// ===========================================================================
// Deterministic base64 (for opaque AssistSidecar binary payloads). The core
// never interprets sidecar bytes; base64 keeps arbitrary bytes (including NUL /
// non-UTF-8) round-trippable inside a JSON string. Standard alphabet + padding.
// ===========================================================================

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<uint8_t>& bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16) |
                            (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                            static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
    out.push_back(kBase64Alphabet[triple & 0x3F]);
    i += 3;
  }
  const size_t remaining = bytes.size() - i;
  if (remaining == 1) {
    const uint32_t triple = static_cast<uint32_t>(bytes[i]) << 16;
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (remaining == 2) {
    const uint32_t triple =
        (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

bool base64_decode(const std::string& text, std::vector<uint8_t>* out, size_t max_output_bytes) {
  return sonare::base64_decode(text, out, max_output_bytes);
}

// ===========================================================================
// Small read helpers. Forward compatibility covers UNKNOWN fields and ABSENT
// known fields: an absent field falls back to the default. A field that is
// present but carries the wrong JSON type is malformed input and is rejected
// with InvalidFormat, uniformly for scalars, arrays, objects and array elements
// -- substituting a default there would let `"sampleRate": "96000"` render the
// whole project at the default rate, or `"clips": {}` load an empty arrangement
// that the next save writes over the user's file.
// ===========================================================================

double num_or(const Value& obj, const std::string& path, const char* key, double fallback) {
  const auto* v = obj.find(key);
  if (!v) return fallback;
  if (!v->is_number()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::must_be(schema::child_path(path, key), schema::Kind::Number));
  }
  return v->as_number();
}

namespace {

template <typename Int>
Int integral_or(const Value& obj, const std::string& path, const char* key, Int fallback) {
  const auto* v = obj.find(key);
  if (!v) return fallback;
  const std::string at = schema::child_path(path, key);
  if (!v->is_number()) {
    throw SonareException(ErrorCode::InvalidFormat, schema::must_be(at, schema::Kind::Number));
  }
  Int converted = 0;
  if (!numeric::checked_integral_cast(v->as_number(), &converted)) {
    throw SonareException(
        ErrorCode::InvalidFormat,
        schema::refusal(at, schema::integer_range_predicate(
                                static_cast<double>(std::numeric_limits<Int>::lowest()),
                                static_cast<double>(std::numeric_limits<Int>::max()))));
  }
  return converted;
}

}  // namespace

uint32_t uint_or(const Value& obj, const std::string& path, const char* key, uint32_t fallback) {
  return integral_or(obj, path, key, fallback);
}

int int_or(const Value& obj, const std::string& path, const char* key, int fallback) {
  return integral_or(obj, path, key, fallback);
}

int8_t int8_or(const Value& obj, const std::string& path, const char* key, int8_t fallback) {
  return integral_or(obj, path, key, fallback);
}

bool parse_uint32_key(const std::string& key, uint32_t* out) {
  if (out == nullptr || key.empty()) return false;
  uint32_t value = 0;
  for (const char c : key) {
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  *out = value;
  return true;
}

const std::string* strict_string_field(const Value& obj, const std::string& path, const char* key) {
  const auto* v = obj.find(key);
  if (!v) return nullptr;
  if (!v->is_string()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::must_be(schema::child_path(path, key), schema::Kind::String));
  }
  return &v->as_string();
}

std::string str_or(const Value& obj, const std::string& path, const char* key,
                   const std::string& fallback) {
  const std::string* value = strict_string_field(obj, path, key);
  return value != nullptr ? *value : fallback;
}

bool bool_or(const Value& obj, const std::string& path, const char* key, bool fallback) {
  const auto* v = obj.find(key);
  if (!v) return fallback;
  if (!v->is_bool()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::must_be(schema::child_path(path, key), schema::Kind::Boolean));
  }
  return v->as_bool();
}

// Reads a UMP data word (full uint32_t range). A present-but-out-of-range value
// (negative, non-finite, or above the uint32_t maximum) is clamped deterministically
// to 0 / 0xFFFFFFFF and recorded as a warning, instead of being silently zeroed.
// An absent field stays a silent 0; a present but non-numeric one is malformed
// input and is rejected like every other wrong-typed scalar.
uint32_t midi_word_or_warn(const Value& obj, const std::string& path, const char* key,
                           uint32_t clip_id, BoundedDiagnostics* diagnostics) {
  const auto* v = obj.find(key);
  if (!v) return 0;
  const std::string at = schema::child_path(path, key);
  if (!v->is_number()) {
    throw SonareException(ErrorCode::InvalidFormat, schema::must_be(at, schema::Kind::Number));
  }
  const double d = v->as_number();
  constexpr double kMaxWord = 4294967295.0;  // 2^32 - 1
  if (!std::isfinite(d) || d < 0.0 || d > kMaxWord) {
    const uint32_t clamped = (!std::isfinite(d) || d < 0.0) ? 0u : static_cast<uint32_t>(kMaxWord);
    diagnostics->warn("midi_word_out_of_range", "MIDI event field \"" + std::string(key) +
                                                    "\" on clip " + std::to_string(clip_id) +
                                                    " is out of range; clamped to " +
                                                    std::to_string(clamped));
    return clamped;
  }
  uint32_t converted = 0;
  if (!numeric::checked_integral_cast(d, &converted)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::refusal(at, schema::integer_range_predicate(0, kMaxWord)));
  }
  return converted;
}

const Array* array_at(const Value& obj, const std::string& path, const char* key) {
  const auto* v = obj.find(key);
  if (!v) return nullptr;
  if (!v->is_array()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::must_be(schema::child_path(path, key), schema::Kind::Array));
  }
  return &v->as_array();
}

const Value* object_value_at(const Value& obj, const std::string& path, const char* key) {
  const auto* v = obj.find(key);
  if (!v) return nullptr;
  if (!v->is_object()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::must_be(schema::child_path(path, key), schema::Kind::Object));
  }
  return v;
}

const Object* object_at(const Value& obj, const std::string& path, const char* key) {
  const Value* v = object_value_at(obj, path, key);
  return v != nullptr ? &v->as_object() : nullptr;
}

const Value& object_element(const Value& element, const std::string& element_path) {
  if (!element.is_object()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          schema::must_be(element_path, schema::Kind::Object));
  }
  return element;
}

}  // namespace detail
}  // namespace sonare::serialize
