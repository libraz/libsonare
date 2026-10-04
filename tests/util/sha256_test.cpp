/// @file sha256_test.cpp
/// @brief Tests for the portable SHA-256 utility.

#include "util/sha256.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

#include "util/exception.h"

namespace {

using sonare::ErrorCode;
using sonare::SonareException;
using sonare::util::Sha256;
using sonare::util::sha256_hex;

TEST_CASE("sha256 matches the published empty and abc vectors", "[sha256]") {
  CHECK(sha256_hex(nullptr, 0) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  constexpr char kAbc[] = "abc";
  CHECK(sha256_hex(reinterpret_cast<const std::uint8_t*>(kAbc), 3) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 matches the published multi-block vector", "[sha256]") {
  constexpr char kMessage[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK(sha256_hex(reinterpret_cast<const std::uint8_t*>(kMessage), sizeof(kMessage) - 1) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  const std::string million_a(1'000'000, 'a');
  CHECK(sha256_hex(reinterpret_cast<const std::uint8_t*>(million_a.data()), million_a.size()) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("incremental sha256 accepts irregular chunks", "[sha256]") {
  constexpr char kMessage[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  constexpr std::array<std::size_t, 9> kChunkSizes{1, 7, 2, 13, 3, 5, 11, 4, 17};
  Sha256 digest;
  std::size_t offset = 0;
  std::size_t chunk_index = 0;
  while (offset < sizeof(kMessage) - 1) {
    const std::size_t requested = kChunkSizes[chunk_index % kChunkSizes.size()];
    const std::size_t remaining = sizeof(kMessage) - 1 - offset;
    const std::size_t size = requested < remaining ? requested : remaining;
    digest.update(reinterpret_cast<const std::uint8_t*>(kMessage) + offset, size);
    offset += size;
    ++chunk_index;
  }
  CHECK(digest.finalize_hex() ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(digest.finalize_hex() ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("incremental sha256 accepts byte-by-byte updates", "[sha256]") {
  constexpr char kMessage[] = "abc";
  Sha256 digest;
  for (std::size_t i = 0; i < sizeof(kMessage) - 1; ++i) {
    digest.update(reinterpret_cast<const std::uint8_t*>(kMessage) + i, 1);
  }

  constexpr std::array<std::uint8_t, 32> kExpected{
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
  };
  CHECK(digest.finalize() == kExpected);
  CHECK(digest.finalize() == kExpected);
}

TEST_CASE("incremental sha256 handles padding boundaries", "[sha256]") {
  constexpr std::array<std::size_t, 5> kLengths{55, 56, 63, 64, 65};
  constexpr std::array<const char*, 5> kExpected{
      "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
      "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
      "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34",
      "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
      "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0",
  };
  for (std::size_t i = 0; i < kLengths.size(); ++i) {
    const std::string input(kLengths[i], 'a');
    Sha256 digest;
    digest.update(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
    CHECK(digest.finalize_hex() == kExpected[i]);
  }
}

TEST_CASE("incremental sha256 handles an irregular million-byte stream", "[sha256]") {
  const std::string input(1'000'000, 'a');
  constexpr std::array<std::size_t, 7> kChunkSizes{1, 63, 64, 65, 127, 1024, 4096};
  Sha256 digest;
  std::size_t offset = 0;
  std::size_t chunk_index = 0;
  while (offset < input.size()) {
    const std::size_t requested = kChunkSizes[chunk_index % kChunkSizes.size()];
    const std::size_t remaining = input.size() - offset;
    const std::size_t size = requested < remaining ? requested : remaining;
    digest.update(reinterpret_cast<const std::uint8_t*>(input.data()) + offset, size);
    offset += size;
    ++chunk_index;
  }
  CHECK(digest.finalize_hex() ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 preserves arbitrary byte values", "[sha256]") {
  constexpr std::array<std::uint8_t, 7> kBytes{0x00, 0x01, 0x02, 0xff, 0x80, 0x00, 0xff};
  CHECK(sha256_hex(kBytes.data(), kBytes.size()) ==
        "f35c2ac4429b930b39c58d07f5bb79818235c9ab6b1fa793a9954d050322e85b");
}

TEST_CASE("sha256 rejects a null non-empty buffer", "[sha256]") {
  bool caught = false;
  try {
    static_cast<void>(sha256_hex(nullptr, 1));
  } catch (const SonareException& error) {
    caught = true;
    CHECK(error.code() == ErrorCode::InvalidParameter);
  }
  CHECK(caught);
}

TEST_CASE("incremental sha256 rejects updates after finalization", "[sha256]") {
  Sha256 digest;
  CHECK(digest.finalize_hex() ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  const std::uint8_t byte = 0;
  bool caught = false;
  try {
    digest.update(&byte, 1);
  } catch (const SonareException& error) {
    caught = true;
    CHECK(error.code() == ErrorCode::InvalidState);
  }
  CHECK(caught);
}

}  // namespace
