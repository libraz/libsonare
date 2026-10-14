#include "sonare_c_internal.h"

#include <cmath>

#include "c_api/sonare_c_error_mapping.h"
#include "util/error_classification.h"

namespace sonare_c_detail {

namespace {

constexpr const char* kErrorDetailUnavailable = "Error detail unavailable";

const char*& last_error_fallback_storage() noexcept {
  static thread_local const char* fallback = nullptr;
  return fallback;
}

}  // namespace

std::string& last_error_storage() {
  static thread_local std::string storage;
  return storage;
}

SonareError& last_error_code_storage() {
  static thread_local SonareError code = SONARE_OK;
  return code;
}

const char* last_error_message() noexcept {
  const char* fallback = last_error_fallback_storage();
  return fallback != nullptr ? fallback : last_error_storage().c_str();
}

void set_last_error(SonareError code, const char* msg) noexcept {
  last_error_code_storage() = code;
  last_error_fallback_storage() = nullptr;
  try {
    last_error_storage().assign(msg != nullptr ? msg : "");
  } catch (...) {
    last_error_fallback_storage() = kErrorDetailUnavailable;
  }
}

void clear_last_error() noexcept {
  last_error_code_storage() = SONARE_OK;
  last_error_storage().clear();
  last_error_fallback_storage() = nullptr;
}

std::string& last_warning_storage() {
  static thread_local std::string storage;
  return storage;
}

void set_last_warning(const char* msg) { last_warning_storage().assign(msg != nullptr ? msg : ""); }

std::vector<sonare::Diagnostic>& last_diagnostics_storage() {
  static thread_local std::vector<sonare::Diagnostic> storage;
  return storage;
}

void set_last_diagnostics(const std::vector<sonare::Diagnostic>& diagnostics) {
  last_diagnostics_storage() = diagnostics;
}

void clear_last_warning() {
  last_warning_storage().clear();
  last_diagnostics_storage().clear();
}

SonareError map_sonare_exception(const SonareException& e) {
  return c_error_from_error_code(e.code());
}

SonareError record_current_exception() noexcept {
  try {
    throw;
  } catch (const SonareException& e) {
    const SonareError code = map_sonare_exception(e);
    set_last_error(code, e.what());
    return code;
  } catch (const std::exception& e) {
    const SonareError code = static_cast<SonareError>(sonare::error_code_for_std_exception(e));
    set_last_error(code, e.what());
    return code;
  } catch (...) {
    set_last_error(SONARE_ERROR_UNKNOWN, kUnknownExceptionMessage);
    return SONARE_ERROR_UNKNOWN;
  }
}

SonareError validate_audio_params(const float* samples, size_t length, int sample_rate) {
  clear_last_error();
  // Delegate to the shared core policy so the C ABI and the WASM bindings (which
  // bypass this translation unit) enforce identical empty/range/finite rules.
  try {
    sonare::validate_offline_audio_input(samples, length, sample_rate);
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  }
  return SONARE_OK;
}

SonareError validate_audio_params_window(const float* samples, size_t length, int sample_rate,
                                         size_t scan_offset, size_t scan_count) {
  clear_last_error();
  try {
    sonare::validate_offline_audio_window(samples, length, sample_rate, scan_offset, scan_count);
  } catch (const SonareException& e) {
    return map_sonare_exception(e);
  }
  return SONARE_OK;
}

SonareGrooveType to_c_groove_type(const std::string& groove) {
  if (groove == "swing") return SONARE_GROOVE_SWING;
  if (groove == "shuffle") return SONARE_GROOVE_SHUFFLE;
  return SONARE_GROOVE_STRAIGHT;
}

SonareChordQuality to_c_chord_quality(ChordQuality quality) {
  switch (quality) {
    case ChordQuality::Major:
      return SONARE_CHORD_MAJOR;
    case ChordQuality::Minor:
      return SONARE_CHORD_MINOR;
    case ChordQuality::Diminished:
      return SONARE_CHORD_DIMINISHED;
    case ChordQuality::Augmented:
      return SONARE_CHORD_AUGMENTED;
    case ChordQuality::Dominant7:
      return SONARE_CHORD_DOMINANT7;
    case ChordQuality::Major7:
      return SONARE_CHORD_MAJOR7;
    case ChordQuality::Minor7:
      return SONARE_CHORD_MINOR7;
    case ChordQuality::Sus2:
      return SONARE_CHORD_SUS2;
    case ChordQuality::Sus4:
      return SONARE_CHORD_SUS4;
    case ChordQuality::Unknown:
      return SONARE_CHORD_UNKNOWN;
    case ChordQuality::Add9:
      return SONARE_CHORD_ADD9;
    case ChordQuality::MinorAdd9:
      return SONARE_CHORD_MINOR_ADD9;
    case ChordQuality::Dim7:
      return SONARE_CHORD_DIM7;
    case ChordQuality::HalfDim7:
      return SONARE_CHORD_HALF_DIM7;
    case ChordQuality::Major9:
      return SONARE_CHORD_MAJOR9;
    case ChordQuality::Dominant9:
      return SONARE_CHORD_DOMINANT9;
    case ChordQuality::Sus2Add4:
      return SONARE_CHORD_SUS2_ADD4;
    case ChordQuality::Major6:
      return SONARE_CHORD_MAJOR6;
    case ChordQuality::Minor6:
      return SONARE_CHORD_MINOR6;
    case ChordQuality::MinorMajor7:
      return SONARE_CHORD_MINOR_MAJOR7;
    case ChordQuality::Dominant7Sus4:
      return SONARE_CHORD_DOMINANT7_SUS4;
    case ChordQuality::Dominant11:
      return SONARE_CHORD_DOMINANT11;
    case ChordQuality::Dominant13:
      return SONARE_CHORD_DOMINANT13;
    case ChordQuality::Dominant7b9:
      return SONARE_CHORD_DOMINANT7_FLAT9;
    case ChordQuality::Dominant7s9:
      return SONARE_CHORD_DOMINANT7_SHARP9;
  }
  return SONARE_CHORD_UNKNOWN;
}

}  // namespace sonare_c_detail
