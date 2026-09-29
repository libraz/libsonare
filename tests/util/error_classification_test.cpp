/// @file error_classification_test.cpp
/// @brief Tests for the shared std::exception -> SonareError classifier.

#include "util/error_classification.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace sonare;

TEST_CASE("error_code_for_std_exception matches the C-ABI catch chain", "[util]") {
  REQUIRE(error_code_for_std_exception(std::bad_alloc()) == 5);            // OutOfMemory
  REQUIRE(error_code_for_std_exception(std::invalid_argument("x")) == 4);  // InvalidParameter
  REQUIRE(error_code_for_std_exception(std::logic_error("x")) == 7);       // InvalidState
  REQUIRE(error_code_for_std_exception(std::runtime_error("x")) == 99);    // Unknown
}

TEST_CASE("error_code_for_std_exception classifies by RTTI, not by static type", "[util]") {
  // std::out_of_range and std::domain_error are std::logic_error subclasses;
  // std::length_error is a std::logic_error subclass too. A caller that only
  // caught std::logic_error verbatim (rather than classifying via dynamic_cast)
  // would still get these right, but a naive typeid() == comparison would not.
  const std::out_of_range out_of_range("x");
  const std::exception& as_base = out_of_range;
  REQUIRE(error_code_for_std_exception(as_base) == 7);

  const std::domain_error domain_error("x");
  const std::exception& domain_as_base = domain_error;
  REQUIRE(error_code_for_std_exception(domain_as_base) == 7);
}
