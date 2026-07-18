// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "serialize/project_serializer.h"
#include "serialize/project_serializer_internal.h"
#include "util/base64.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::serialize {
namespace detail {

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

// Base64 decode is shared with the mastering IR loader (see util/base64.h);
// these thin wrappers satisfy the internal-header declarations while keeping a
// single decoder implementation.
uint8_t base64_value(char c) { return sonare::base64_char_value(c); }

bool base64_decode(const std::string& text, std::vector<uint8_t>* out) {
  return sonare::base64_decode(text, out);
}

// ===========================================================================
// Small read helpers (forward-compatible: missing / wrong-typed fields fall back
// to the default, matching the "unknown fields safely ignored" contract).
// ===========================================================================

double num_or(const Value& obj, const char* key, double fallback) {
  const auto* v = obj.find(key);
  return (v && v->is_number()) ? v->as_number() : fallback;
}

uint32_t uint_or(const Value& obj, const char* key, uint32_t fallback) {
  const auto* v = obj.find(key);
  if (!v) return fallback;
  if (!v->is_number()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "integer field must be numeric: " + std::string(key));
  }
  uint32_t converted = 0;
  if (!numeric::checked_integral_cast(v->as_number(), &converted)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "integer field is fractional or out of range: " + std::string(key));
  }
  return converted;
}

int int_or(const Value& obj, const char* key, int fallback) {
  const auto* v = obj.find(key);
  if (!v) return fallback;
  if (!v->is_number()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "integer field must be numeric: " + std::string(key));
  }
  int converted = 0;
  if (!numeric::checked_integral_cast(v->as_number(), &converted)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "integer field is fractional or out of range: " + std::string(key));
  }
  return converted;
}

int8_t int8_or(const Value& obj, const char* key, int8_t fallback) {
  const auto* v = obj.find(key);
  if (!v) return fallback;
  if (!v->is_number()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "integer field must be numeric: " + std::string(key));
  }
  int8_t converted = 0;
  if (!numeric::checked_integral_cast(v->as_number(), &converted)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "integer field is fractional or out of range: " + std::string(key));
  }
  return converted;
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

std::string str_or(const Value& obj, const char* key, const std::string& fallback) {
  const auto* v = obj.find(key);
  return (v && v->is_string()) ? v->as_string() : fallback;
}

bool bool_or(const Value& obj, const char* key, bool fallback) {
  const auto* v = obj.find(key);
  return (v && v->is_bool()) ? v->as_bool() : fallback;
}

// Reads a UMP data word (full uint32_t range). A present-but-out-of-range value
// (negative, non-finite, or above the uint32_t maximum) is clamped deterministically
// to 0 / 0xFFFFFFFF and recorded as a warning, instead of being silently zeroed.
// Absent / non-numeric (the forward-compatible default) stays a silent 0.
uint32_t midi_word_or_warn(const Value& obj, const char* key, uint32_t clip_id,
                           std::vector<Diagnostic>* diagnostics) {
  const auto* v = obj.find(key);
  if (!v || !v->is_number()) return 0;
  const double d = v->as_number();
  constexpr double kMaxWord = 4294967295.0;  // 2^32 - 1
  if (!std::isfinite(d) || d < 0.0 || d > kMaxWord) {
    const uint32_t clamped = (!std::isfinite(d) || d < 0.0) ? 0u : static_cast<uint32_t>(kMaxWord);
    diagnostics->push_back({DiagnosticSeverity::kWarning, "midi_word_out_of_range",
                            "MIDI event field \"" + std::string(key) + "\" on clip " +
                                std::to_string(clip_id) + " is out of range; clamped to " +
                                std::to_string(clamped)});
    return clamped;
  }
  uint32_t converted = 0;
  if (!numeric::checked_integral_cast(d, &converted)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "MIDI data word is fractional: " + std::string(key));
  }
  return converted;
}

const Array* array_at(const Value& obj, const char* key) {
  const auto* v = obj.find(key);
  return (v && v->is_array()) ? &v->as_array() : nullptr;
}

const Object* object_at(const Value& obj, const char* key) {
  const auto* v = obj.find(key);
  return (v && v->is_object()) ? &v->as_object() : nullptr;
}

double num_or_any(const Value& obj, const char* primary, const char* legacy, double fallback) {
  const auto* v = obj.find(primary);
  if (v && v->is_number()) return v->as_number();
  return num_or(obj, legacy, fallback);
}

int int_or_any(const Value& obj, const char* primary, const char* legacy, int fallback) {
  // Match the num_or_any / str_or_any / bool_or_any contract: a present-but-
  // wrong-typed value falls back rather than aborting the whole load. The old
  // form called the strict int_or on `primary` unconditionally, so a non-numeric
  // panMode/panLaw/channelDelaySamples threw and rejected an otherwise-valid
  // project (while faderDb/soloSafe only fell back). A numeric-but-fractional /
  // out-of-range value still throws via int_or -- that is a genuine value error.
  const auto* v = obj.find(primary);
  if (v && v->is_number()) return int_or(obj, primary, fallback);
  const auto* legacy_v = obj.find(legacy);
  if (legacy_v && legacy_v->is_number()) return int_or(obj, legacy, fallback);
  return fallback;
}

std::string str_or_any(const Value& obj, const char* primary, const char* legacy,
                       const std::string& fallback) {
  const auto* v = obj.find(primary);
  if (v && v->is_string()) return v->as_string();
  return str_or(obj, legacy, fallback);
}

bool bool_or_any(const Value& obj, const char* primary, const char* legacy, bool fallback) {
  const auto* v = obj.find(primary);
  if (v && v->is_bool()) return v->as_bool();
  return bool_or(obj, legacy, fallback);
}

// ===========================================================================
// Migration hook. The current serializer knows schema version 1. A document with the same
// MAJOR version is accepted (forward-compatible field handling above); an
// unknown future major is rejected with a diagnostic rather than misread.
// ===========================================================================

bool schema_version_supported(uint32_t version) { return version <= SONARE_PROJECT_SCHEMA_VERSION; }

}  // namespace detail
}  // namespace sonare::serialize
