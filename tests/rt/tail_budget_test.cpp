/// @file tail_budget_test.cpp
/// @brief TailBudget residual-scale contract.

#include "rt/tail_budget.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("TailBudget recirculation includes a heard output scale", "[boundary]") {
  const int unit_scale = sonare::rt::TailBudget{}.recirculation(1.0, 0.9, 1.0).samples();
  const int doubled_scale = sonare::rt::TailBudget{}.recirculation(1.0, 0.9, 2.0).samples();
  const int half_scale = sonare::rt::TailBudget{}.recirculation(1.0, 0.9, 0.5).samples();
  REQUIRE(unit_scale == 110);
  REQUIRE(doubled_scale == 116);
  REQUIRE(half_scale == 103);
  REQUIRE(doubled_scale > unit_scale);
  REQUIRE(half_scale < unit_scale);
  REQUIRE(sonare::rt::TailBudget{}.recirculation(1.0, 1.0, 0.0).samples() == 0);
}

TEST_CASE("TailBudget decay includes a heard output scale", "[boundary]") {
  const int unit_scale = sonare::rt::TailBudget{}.decay(0.9, 1.0).samples();
  const int doubled_scale = sonare::rt::TailBudget{}.decay(0.9, 2.0).samples();
  REQUIRE(unit_scale == 110);
  REQUIRE(doubled_scale == 116);
  REQUIRE(doubled_scale > unit_scale);
}
