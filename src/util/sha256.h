#pragma once

/// @file
/// @brief Portable SHA-256 digest formatting for byte buffers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sonare::util {

/// @brief Incremental SHA-256 digest calculator.
/// @details A calculator may be finalized repeatedly; every finalization returns
///          the same digest. Calling update after finalization throws
///          ErrorCode::InvalidState.
class Sha256 {
 public:
  Sha256() noexcept;

  /// @brief Appends bytes to the digest input.
  /// @param data Bytes to append. May be null only when @p size is zero.
  /// @param size Number of bytes at @p data.
  /// @throws sonare::SonareException with ErrorCode::InvalidParameter when @p data is
  ///         null and @p size is non-zero or the cumulative input is too large.
  /// @throws sonare::SonareException with ErrorCode::InvalidState after finalization.
  void update(const std::uint8_t* data, std::size_t size);

  /// @brief Finalizes the digest and returns its raw 32-byte value.
  /// @return The SHA-256 digest in the standard big-endian byte order.
  std::array<std::uint8_t, 32> finalize();

  /// @brief Finalizes the digest and returns its lowercase hexadecimal form.
  std::string finalize_hex();

 private:
  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t bit_length_ = 0;
  std::array<std::uint8_t, 32> digest_{};
  bool finalized_ = false;
};

/// @brief Computes a SHA-256 digest and returns its canonical lowercase hex form.
/// @param data Bytes to hash. May be null only when @p size is zero.
/// @param size Number of bytes at @p data.
/// @throws sonare::SonareException with ErrorCode::InvalidParameter when @p data is
///         null and @p size is non-zero.
std::string sha256_hex(const std::uint8_t* data, std::size_t size);

/// @brief Formats a raw SHA-256 digest as 64 lowercase hexadecimal characters.
std::string sha256_digest_hex(const std::array<std::uint8_t, 32>& digest);

}  // namespace sonare::util
