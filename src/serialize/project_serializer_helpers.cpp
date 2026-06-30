// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "serialize/project_serializer.h"
#include "serialize/project_serializer_internal.h"

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

// Returns 0..63 for a valid base64 character, or 0xFF for invalid (so a malformed
// payload decodes to a defined value rather than reading OOB; the surrounding
// decode reports failure via the bool result).
uint8_t base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(c - 'A');
  if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 'a' + 26);
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0' + 52);
  if (c == '+') return 62;
  if (c == '/') return 63;
  return 0xFF;
}

// Decodes a standard-alphabet base64 string. Returns false (and leaves *out
// empty) on any malformed length / character so deserialize fails gracefully.
bool base64_decode(const std::string& text, std::vector<uint8_t>* out) {
  out->clear();
  if (text.size() % 4 != 0) return false;
  out->reserve((text.size() / 4) * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    const char c0 = text[i];
    const char c1 = text[i + 1];
    const char c2 = text[i + 2];
    const char c3 = text[i + 3];
    const uint8_t v0 = base64_value(c0);
    const uint8_t v1 = base64_value(c1);
    if (v0 == 0xFF || v1 == 0xFF) return false;
    const bool pad2 = (c2 == '=');
    const bool pad3 = (c3 == '=');
    // Padding may only appear in the final quad, c3 alone or c2+c3 together.
    if ((pad2 || pad3) && i + 4 != text.size()) return false;
    if (pad2 && !pad3) return false;
    uint32_t triple = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12);
    out->push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
    if (!pad2) {
      const uint8_t v2 = base64_value(c2);
      if (v2 == 0xFF) return false;
      triple |= static_cast<uint32_t>(v2) << 6;
      out->push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
      if (!pad3) {
        const uint8_t v3 = base64_value(c3);
        if (v3 == 0xFF) return false;
        triple |= static_cast<uint32_t>(v3);
        out->push_back(static_cast<uint8_t>(triple & 0xFF));
      }
    }
  }
  return true;
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
  if (!v || !v->is_number()) return fallback;
  const double d = v->as_number();
  constexpr double kMaxUint32 = 4294967295.0;  // 2^32 - 1
  return (!std::isfinite(d) || d < 0.0 || d > kMaxUint32) ? fallback : static_cast<uint32_t>(d);
}

bool parse_uint32_key(const std::string& key, uint32_t* out) {
  if (out == nullptr || key.empty()) return false;
  uint64_t value = 0;
  for (const char c : key) {
    if (c < '0' || c > '9') return false;
    value = value * 10u + static_cast<uint64_t>(c - '0');
    if (value > std::numeric_limits<uint32_t>::max()) return false;
  }
  *out = static_cast<uint32_t>(value);
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
  return static_cast<uint32_t>(d);
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
