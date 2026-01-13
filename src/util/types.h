#pragma once

/// @file types.h
/// @brief Common type definitions for libsonare.

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
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
  MatrixView(const T* data, size_t rows, size_t cols) : data_(data), rows_(rows), cols_(cols) {}

  const T* data() const { return data_; }
  size_t rows() const { return rows_; }
  size_t cols() const { return cols_; }
  size_t size() const { return rows_ * cols_; }
  bool empty() const { return data_ == nullptr || size() == 0; }

  /// @brief Access element at (row, col) in row-major order.
  const T& at(size_t row, size_t col) const { return data_[row * cols_ + col]; }

  /// @brief Access element at (row, col) in row-major order.
  const T& operator()(size_t row, size_t col) const { return at(row, col); }

  /// @brief Returns pointer to the start of row i.
  const T* row(size_t i) const { return data_ + i * cols_; }

 private:
  const T* data_;
  size_t rows_;
  size_t cols_;
};

/// @brief Error codes for library operations.
enum class ErrorCode : int {
  Ok = 0,
  FileNotFound,
  InvalidFormat,
  DecodeFailed,
  InvalidParameter,
  OutOfMemory,
  NotImplemented,
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
};

/// @brief Song section types.
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
/// @return "major" or "minor"
inline const char* mode_name(Mode m) { return m == Mode::Major ? "major" : "minor"; }

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
  }
  return "Unknown error";
}

}  // namespace sonare
