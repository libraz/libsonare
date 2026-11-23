/// @file error_classification_test.cpp
/// @brief Tests for the shared std::exception -> SonareError classifier.

#include "util/error_classification.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sonare;

TEST_CASE("error_code_for_std_exception matches the C-ABI catch chain", "[util]") {
  REQUIRE(error_code_for_std_exception(std::bad_alloc()) == 5);            // OutOfMemory
  REQUIRE(error_code_for_std_exception(std::length_error("x")) == 5);      // OutOfMemory
  REQUIRE(error_code_for_std_exception(std::invalid_argument("x")) == 4);  // InvalidParameter
  REQUIRE(error_code_for_std_exception(std::logic_error("x")) == 7);       // InvalidState
  REQUIRE(error_code_for_std_exception(std::runtime_error("x")) == 99);    // Unknown
}

TEST_CASE("error_code_for_std_exception classifies by RTTI, not by static type", "[util]") {
  // std::out_of_range and std::domain_error are std::logic_error subclasses, so a
  // naive typeid() == comparison would miss them.
  const std::out_of_range out_of_range("x");
  const std::exception& as_base = out_of_range;
  REQUIRE(error_code_for_std_exception(as_base) == 7);

  const std::domain_error domain_error("x");
  const std::exception& domain_as_base = domain_error;
  REQUIRE(error_code_for_std_exception(domain_as_base) == 7);
}

TEST_CASE("a container asked past max_size() reads as out of memory, not as a state error",
          "[util]") {
  // std::length_error is a std::logic_error subclass, so without its own branch it would be
  // classified as InvalidState and reported with the library's implementation-defined text.
  try {
    std::vector<float> v;
    v.reserve(v.max_size() + std::size_t{1});
    FAIL("reserve past max_size() did not throw");
  } catch (const std::length_error& e) {
    const std::exception& as_base = e;
    CHECK(error_code_for_std_exception(as_base) == 5);
    CHECK(std::string(message_for_std_exception(as_base)) == error_message(ErrorCode::OutOfMemory));
  }
}
