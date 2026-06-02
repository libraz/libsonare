#include "acoustic/material.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "acoustic/room_types.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::acoustic;

TEST_CASE("material presets have full band coverage in [0,1]", "[acoustic][material]") {
  for (auto preset : {MaterialPreset::Concrete, MaterialPreset::Wood, MaterialPreset::Curtain,
                      MaterialPreset::Carpet, MaterialPreset::Glass}) {
    const Material m = make_material(preset);
    REQUIRE(m.absorption.size() == static_cast<size_t>(kDefaultOctaveBands));
    REQUIRE(m.scattering.size() == static_cast<size_t>(kDefaultOctaveBands));
    for (float a : m.absorption) REQUIRE((a >= 0.0f && a <= 1.0f));
    for (float s : m.scattering) REQUIRE((s >= 0.0f && s <= 1.0f));
  }
}

TEST_CASE("curtain absorbs more high frequency than concrete", "[acoustic][material]") {
  const Material curtain = make_material(MaterialPreset::Curtain);
  const Material concrete = make_material(MaterialPreset::Concrete);
  // 4 kHz band (index 5) — heavy fabric is far more absorptive than concrete.
  REQUIRE(curtain.absorption.back() > concrete.absorption.back());
}

TEST_CASE("material blend is endpoint-exact and continuous in t", "[acoustic][material]") {
  const Material a = make_material(MaterialPreset::Concrete);
  const Material b = make_material(MaterialPreset::Curtain);

  SECTION("endpoints") {
    const Material at0 = mix_materials(a, b, 0.0f);
    const Material at1 = mix_materials(a, b, 1.0f);
    for (size_t i = 0; i < a.absorption.size(); ++i) {
      REQUIRE_THAT(at0.absorption[i], WithinAbs(a.absorption[i], 1e-6f));
      REQUIRE_THAT(at1.absorption[i], WithinAbs(b.absorption[i], 1e-6f));
    }
  }

  SECTION("midpoint is the average") {
    const Material mid = mix_materials(a, b, 0.5f);
    for (size_t i = 0; i < a.absorption.size(); ++i) {
      REQUIRE_THAT(mid.absorption[i], WithinAbs(0.5f * (a.absorption[i] + b.absorption[i]), 1e-6f));
    }
  }

  SECTION("small step in t -> small step in output (Lipschitz/continuous)") {
    const float eps = 1e-3f;
    const Material m0 = mix_materials(a, b, 0.4f);
    const Material m1 = mix_materials(a, b, 0.4f + eps);
    for (size_t i = 0; i < a.absorption.size(); ++i) {
      const float span = std::abs(b.absorption[i] - a.absorption[i]);
      REQUIRE(std::abs(m1.absorption[i] - m0.absorption[i]) <= span * eps + 1e-6f);
    }
  }

  SECTION("t is clamped outside [0,1]") {
    const Material lo = mix_materials(a, b, -1.0f);
    const Material hi = mix_materials(a, b, 2.0f);
    for (size_t i = 0; i < a.absorption.size(); ++i) {
      REQUIRE_THAT(lo.absorption[i], WithinAbs(a.absorption[i], 1e-6f));
      REQUIRE_THAT(hi.absorption[i], WithinAbs(b.absorption[i], 1e-6f));
    }
  }
}

TEST_CASE("material blend with differing band counts uses the shorter length",
          "[acoustic][material]") {
  const Material six = make_material(MaterialPreset::Concrete);  // 6 bands
  const Material three = uniform_material(0.5f, 0.3f, 3);        // 3 bands
  const Material m = mix_materials(six, three, 0.5f);
  REQUIRE(m.absorption.size() == 3);
  REQUIRE(m.scattering.size() == 3);
  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_THAT(m.absorption[i], WithinAbs(0.5f * (six.absorption[i] + 0.5f), 1e-6f));
  }
}

TEST_CASE("uniform material is constant across bands", "[acoustic][material]") {
  const Material m = uniform_material(0.3f, 0.2f, kDefaultOctaveBands);
  REQUIRE(m.absorption.size() == static_cast<size_t>(kDefaultOctaveBands));
  for (float a : m.absorption) REQUIRE_THAT(a, WithinAbs(0.3f, 1e-6f));
  for (float s : m.scattering) REQUIRE_THAT(s, WithinAbs(0.2f, 1e-6f));
}
