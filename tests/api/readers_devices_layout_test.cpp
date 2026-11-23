#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>

TEST_CASE("C STFT appends the phase option and result plane", "[c_api][abi][stft]") {
  CHECK(offsetof(SonareStftOptions, phase) == 2 * sizeof(uint32_t));
  CHECK(offsetof(SonareStftResult, phase) == offsetof(SonareStftResult, db) + sizeof(float*));
}
