#pragma once

/// @file audio.h
/// @brief Audio buffer class with efficient slicing and shared ownership.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace sonare {

/// @brief Audio buffer with shared ownership and zero-copy slicing.
/// @details Samples are always mono and normalized to [-1, 1].
/// Slices share the underlying buffer, avoiding unnecessary copies.
class Audio {
 public:
  /// @brief Default constructor creates an empty Audio.
  Audio();

  /// @brief Creates Audio from existing samples.
  /// @param samples Pointer to sample data (will be copied)
  /// @param size Number of samples
  /// @param sample_rate Sample rate in Hz
  /// @return Audio object
  static Audio from_buffer(const float* samples, size_t size, int sample_rate);

  /// @brief Creates Audio from a vector of samples.
  /// @param samples Vector of samples (will be moved)
  /// @param sample_rate Sample rate in Hz
  /// @return Audio object
  static Audio from_vector(std::vector<float> samples, int sample_rate);

  /// @brief Loads Audio from a file (WAV or MP3).
  /// @param path Path to audio file
  /// @return Audio object
  /// @throws SonareException on file not found or decode error
  static Audio from_file(const std::string& path);

  /// @brief Loads Audio from memory buffer.
  /// @param data Pointer to audio data
  /// @param size Size of data in bytes
  /// @return Audio object
  /// @throws SonareException on decode error
  static Audio from_memory(const uint8_t* data, size_t size);

  /// @brief Returns pointer to sample data.
  const float* data() const;

  /// @brief Returns number of samples.
  size_t size() const;

  /// @brief Returns sample rate in Hz.
  int sample_rate() const { return sample_rate_; }

  /// @brief Returns duration in seconds.
  float duration() const;

  /// @brief Returns number of channels (always 1 for mono).
  int channels() const { return 1; }

  /// @brief Returns true if audio is empty.
  bool empty() const { return size() == 0; }

  /// @brief Creates a slice of this audio (shared buffer, zero-copy).
  /// @param start_time Start time in seconds
  /// @param end_time End time in seconds (negative means end of audio)
  /// @return New Audio object sharing the same buffer
  Audio slice(float start_time, float end_time = -1.0f) const;

  /// @brief Creates a slice by sample indices (shared buffer, zero-copy).
  /// @param start_sample Start sample index
  /// @param end_sample End sample index (negative means end of audio)
  /// @return New Audio object sharing the same buffer
  Audio slice_samples(size_t start_sample, size_t end_sample = static_cast<size_t>(-1)) const;

  /// @brief Returns a copy with samples as mono (already mono, returns copy).
  Audio to_mono() const;

  /// @brief Access sample by index.
  float operator[](size_t index) const;

  /// @brief Iterator support.
  const float* begin() const { return data(); }
  const float* end() const { return data() + size(); }

 private:
  /// @brief Private constructor for creating slices.
  Audio(std::shared_ptr<const std::vector<float>> buffer, size_t offset, size_t length,
        int sample_rate);

  std::shared_ptr<const std::vector<float>> buffer_;
  size_t offset_;
  size_t length_;
  int sample_rate_;
};

}  // namespace sonare
