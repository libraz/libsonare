#pragma once

/// @file types.h
/// @brief Common type definitions for libsonare.

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace sonare {

/// @brief Lightweight read-only 2D matrix view (Eigen-independent).
/// @tparam T Element type
template <typename T>
class MatrixView {
 public:
  MatrixView() : data_(nullptr), rows_(0), cols_(0) {}

  /// @brief Constructs a view over existing data.
  /// @param data Pointer to row-major data
  /// @param rows Number of rows
  /// @param cols Number of columns
  MatrixView(const T* data, size_t rows, size_t cols) : data_(data), rows_(rows), cols_(cols) {
    // Keep the dimension product representable so size() and every valid
    // row-major offset can be evaluated without wrapping.
    checked_product(rows_, cols_);
  }

  const T* data() const { return data_; }
  size_t rows() const { return rows_; }
  size_t cols() const { return cols_; }
  size_t size() const { return rows_ * cols_; }
  bool empty() const { return data_ == nullptr || size() == 0; }

  /// @brief Access element at (row, col) in row-major order.
  const T& at(size_t row, size_t col) const { return data_[checked_index(row, col)]; }

  /// @brief Access element at (row, col) in row-major order.
  const T& operator()(size_t row, size_t col) const { return at(row, col); }

  /// @brief Returns pointer to the start of row i.
  const T* row(size_t i) const { return data_ + checked_row_offset(i); }

 private:
  static size_t checked_product(size_t lhs, size_t rhs) {
    if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs) {
      throw std::overflow_error("MatrixView size or index overflow");
    }
    return lhs * rhs;
  }

  size_t checked_row_offset(size_t row) const {
    const size_t offset = checked_product(row, cols_);
    if (row >= rows_) {
      throw std::out_of_range("MatrixView row index out of range");
    }
    return offset;
  }

  size_t checked_index(size_t row, size_t col) const {
    const size_t offset = checked_product(row, cols_);
    if (col > std::numeric_limits<size_t>::max() - offset) {
      throw std::overflow_error("MatrixView index overflow");
    }
    if (row >= rows_ || col >= cols_) {
      throw std::out_of_range("MatrixView index out of range");
    }
    return offset + col;
  }

  const T* data_;
  size_t rows_;
  size_t cols_;
};

/// @brief Error codes for library operations.
/// @details Mirrors the C ABI @c SonareError enum; new values MUST be added to
///          both enums in lockstep and to the @c map_sonare_exception switch in
///          @c sonare_c_internal.cpp.
enum class ErrorCode : int {
  Ok = 0,
  FileNotFound,
  InvalidFormat,
  DecodeFailed,
  InvalidParameter,
  OutOfMemory,
  NotImplemented,
  /// @brief The component is not in a state where the call is allowed
  ///        (e.g. @c process_block before @c prepare, configuration mismatch).
  InvalidState,
  /// @brief A cooperative cancellation callback requested an early stop.
  Cancelled,
  /// @brief Producing or writing an output artefact failed (creating the file,
  ///        a short write, an encoder refusing the data). The read-side sibling
  ///        is @c DecodeFailed; a failure to write is not a failure to decode,
  ///        and callers routing on the code cannot recover the distinction once
  ///        the two are merged.
  EncodeFailed,
};

/// @brief Pitch class (0-11, C=0).
enum class PitchClass : int {
  C = 0,
  Cs = 1,
  D = 2,
  Ds = 3,
  E = 4,
  F = 5,
  Fs = 6,
  G = 7,
  Gs = 8,
  A = 9,
  As = 10,
  B = 11,
};

/// @brief Musical mode.
enum class Mode {
  Major,
  Minor,
  Dorian,
  Phrygian,
  Lydian,
  Mixolydian,
  Locrian,
};

/// @brief Chord quality types.
enum class ChordQuality {
  Major,
  Minor,
  Diminished,
  Augmented,
  Dominant7,
  Major7,
  Minor7,
  Sus2,
  Sus4,
  Unknown,
  Add9,
  MinorAdd9,
  Dim7,
  HalfDim7,
  Major9,
  Dominant9,
  Sus2Add4,
};

/// @brief Song section types.
/// @note @c PreChorus is never produced by @ref SectionAnalyzer: it has no
///       detection branch, so filtering sections on it always yields an empty
///       result. Every other value is reachable. @c Unknown carries a specific
///       meaning — the segmenter found no structure, or a segment matched none
///       of the positive branches — and is reported with @c confidence 0.
enum class SectionType {
  Intro,
  Verse,
  PreChorus,
  Chorus,
  Bridge,
  Instrumental,
  Outro,
  Unknown,
};

/// @brief Window function types.
enum class WindowType {
  Hann,
  Hamming,
  Blackman,
  Rectangular,
};

/// @brief Returns the name of a pitch class.
/// @param pc Pitch class
/// @return String name (e.g., "C", "C#")
inline const char* pitch_class_name(PitchClass pc) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return names[static_cast<int>(pc)];
}

/// @brief Returns the name of a mode.
/// @param m Mode
/// @return Human-readable mode name.
inline const char* mode_name(Mode m) {
  switch (m) {
    case Mode::Major:
      return "major";
    case Mode::Minor:
      return "minor";
    case Mode::Dorian:
      return "dorian";
    case Mode::Phrygian:
      return "phrygian";
    case Mode::Lydian:
      return "lydian";
    case Mode::Mixolydian:
      return "mixolydian";
    case Mode::Locrian:
      return "locrian";
    default:
      return "unknown";
  }
}

/// @brief Returns error message for an error code.
/// @param code Error code
/// @return Human-readable error message
inline const char* error_message(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok:
      return "OK";
    case ErrorCode::FileNotFound:
      return "File not found";
    case ErrorCode::InvalidFormat:
      return "Invalid format";
    case ErrorCode::DecodeFailed:
      return "Decode failed";
    case ErrorCode::InvalidParameter:
      return "Invalid parameter";
    case ErrorCode::OutOfMemory:
      return "Out of memory";
    case ErrorCode::NotImplemented:
      return "Not implemented";
    case ErrorCode::InvalidState:
      return "Invalid state";
    case ErrorCode::Cancelled:
      return "Cancelled";
    case ErrorCode::EncodeFailed:
      return "Encode failed";
    default:
      return "Unknown error";
  }
}

}  // namespace sonare
