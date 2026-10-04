#include "util/sha256.h"

#include <cstring>
#include <limits>

#include "util/exception.h"

namespace sonare::util {

namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

constexpr char kHexDigits[] = "0123456789abcdef";

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32u - count));
}

void compress_block(std::uint32_t state[8], const std::uint8_t block[64]) {
  std::uint32_t words[64];
  for (std::size_t i = 0; i < 16; ++i) {
    const std::size_t offset = i * 4;
    words[i] = (static_cast<std::uint32_t>(block[offset]) << 24u) |
               (static_cast<std::uint32_t>(block[offset + 1]) << 16u) |
               (static_cast<std::uint32_t>(block[offset + 2]) << 8u) |
               static_cast<std::uint32_t>(block[offset + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t lower_sigma =
        rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3u);
    const std::uint32_t upper_sigma =
        rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10u);
    words[i] = words[i - 16] + lower_sigma + words[i - 7] + upper_sigma;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t upper_sigma =
        rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + upper_sigma + choose + kRoundConstants[i] + words[i];
    const std::uint32_t lower_sigma =
        rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = lower_sigma + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
      block_{},
      block_size_(0),
      bit_length_(0),
      digest_{},
      finalized_(false) {}

void Sha256::update(const std::uint8_t* data, std::size_t size) {
  if (finalized_) {
    throw SonareException(ErrorCode::InvalidState, "sha256: update after finalization");
  }
  if (data == nullptr && size != 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sha256: null input with non-zero length");
  }
  const std::uint64_t max_bit_length = std::numeric_limits<std::uint64_t>::max();
  if (size > (max_bit_length - bit_length_) / 8u) {
    throw SonareException(ErrorCode::InvalidParameter, "sha256: input is too large");
  }
  if (size == 0) return;

  bit_length_ += static_cast<std::uint64_t>(size) * 8u;
  const std::uint8_t* cursor = data;
  std::size_t remaining = size;
  while (remaining != 0) {
    const std::size_t available = block_.size() - block_size_;
    const std::size_t copied = remaining < available ? remaining : available;
    std::memcpy(block_.data() + block_size_, cursor, copied);
    block_size_ += copied;
    cursor += copied;
    remaining -= copied;
    if (block_size_ == block_.size()) {
      compress_block(state_.data(), block_.data());
      block_size_ = 0;
    }
  }
}

std::array<std::uint8_t, 32> Sha256::finalize() {
  if (finalized_) return digest_;

  std::array<std::uint32_t, 8> final_state = state_;
  std::array<std::uint8_t, 64> final_block = block_;
  std::size_t final_size = block_size_;
  final_block[final_size] = 0x80u;
  ++final_size;
  if (final_size > 56) {
    if (final_size < final_block.size()) {
      std::memset(final_block.data() + final_size, 0, final_block.size() - final_size);
    }
    compress_block(final_state.data(), final_block.data());
    final_size = 0;
  }
  std::memset(final_block.data() + final_size, 0, 56 - final_size);
  for (unsigned int i = 0; i < 8; ++i) {
    final_block[63 - i] = static_cast<std::uint8_t>(bit_length_ >> (i * 8u));
  }
  compress_block(final_state.data(), final_block.data());

  for (std::size_t i = 0; i < final_state.size(); ++i) {
    const std::uint32_t word = final_state[i];
    digest_[i * 4] = static_cast<std::uint8_t>(word >> 24u);
    digest_[i * 4 + 1] = static_cast<std::uint8_t>(word >> 16u);
    digest_[i * 4 + 2] = static_cast<std::uint8_t>(word >> 8u);
    digest_[i * 4 + 3] = static_cast<std::uint8_t>(word);
  }
  finalized_ = true;
  return digest_;
}

std::string Sha256::finalize_hex() { return sha256_digest_hex(finalize()); }

std::string sha256_digest_hex(const std::array<std::uint8_t, 32>& digest) {
  std::string result;
  result.reserve(64);
  for (const std::uint8_t byte : digest) {
    result.push_back(kHexDigits[byte >> 4u]);
    result.push_back(kHexDigits[byte & 0x0fu]);
  }
  return result;
}

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
  Sha256 digest;
  digest.update(data, size);
  return digest.finalize_hex();
}

}  // namespace sonare::util
