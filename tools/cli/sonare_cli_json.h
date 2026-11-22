#pragma once

/// @file sonare_cli_json.h
/// @brief JSON serialization for the native CLI's machine-readable output.

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

/// Non-finite numbers follow the level convention: a NaN (not measured) is never written — a
/// keyed value drops its key, and an array value is refused, so a document array holding one
/// goes through measured_array() — while -inf / +inf are written as null, the end the
/// field's schema declares with `x-nullMeans`.
class JsonBuilder {
 public:
  /// Pins the output stream to the classic locale, so that a real number is
  /// written with a `.` decimal separator regardless of the host's locale.
  JsonBuilder();

  JsonBuilder& begin_object();
  JsonBuilder& end_object();
  JsonBuilder& begin_array();
  JsonBuilder& end_array();
  JsonBuilder& key(const std::string& k);
  JsonBuilder& value(const std::string& v);
  JsonBuilder& value(const char* v);
  JsonBuilder& value(int v);
  JsonBuilder& value(size_t v);
  JsonBuilder& value(float v);
  JsonBuilder& value(double v);
  JsonBuilder& value(bool v);
  JsonBuilder& null_value();
  JsonBuilder& kv(const std::string& k, const std::string& v);
  JsonBuilder& kv(const std::string& k, const char* v);
  JsonBuilder& kv(const std::string& k, int v);
  JsonBuilder& kv(const std::string& k, size_t v);
  JsonBuilder& kv(const std::string& k, float v);
  JsonBuilder& kv(const std::string& k, double v);
  JsonBuilder& kv(const std::string& k, bool v);
  JsonBuilder& float_array(const std::vector<float>& arr);
  /// Writes the measured (non-NaN) entries of @p values under @p values_key and their
  /// ascending indices under @p index_key.
  JsonBuilder& measured_array(const std::string& values_key, const std::string& index_key,
                              const float* values, size_t count);
  std::string build() const;
  void print() const;

 private:
  void append_separator();
  static std::string escape(const std::string& s);

  std::ostringstream ss_;
  std::vector<bool> needs_comma_;
};
