#include "sonare_c_internal.h"

#include <cmath>

namespace sonare_c_detail {

std::string& last_error_storage() {
  static thread_local std::string storage;
  return storage;
}

void set_last_error(const char* msg) { last_error_storage().assign(msg != nullptr ? msg : ""); }

void clear_last_error() { last_error_storage().clear(); }

std::string& last_warning_storage() {
  static thread_local std::string storage;
  return storage;
}

void set_last_warning(const char* msg) { last_warning_storage().assign(msg != nullptr ? msg : ""); }

void clear_last_warning() { last_warning_storage().clear(); }

SonareError map_sonare_exception(const SonareException& e) {
  switch (e.code()) {
    case sonare::ErrorCode::FileNotFound:
      return SONARE_ERROR_FILE_NOT_FOUND;
    case sonare::ErrorCode::InvalidFormat:
      return SONARE_ERROR_INVALID_FORMAT;
    case sonare::ErrorCode::DecodeFailed:
      return SONARE_ERROR_DECODE_FAILED;
    case sonare::ErrorCode::InvalidParameter:
      return SONARE_ERROR_INVALID_PARAMETER;
    case sonare::ErrorCode::OutOfMemory:
      return SONARE_ERROR_OUT_OF_MEMORY;
    case sonare::ErrorCode::NotImplemented:
      return SONARE_ERROR_NOT_SUPPORTED;
    case sonare::ErrorCode::InvalidState:
      return SONARE_ERROR_INVALID_STATE;
    default:
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
  }
  return SONARE_CHORD_UNKNOWN;
}

}  // namespace sonare_c_detail
