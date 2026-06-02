#include "acoustic/material.h"

#include <algorithm>
#include <array>

namespace sonare::acoustic {

namespace {
// Nominal octave-band coefficients (125 / 250 / 500 / 1k / 2k / 4k Hz),
// drawn from standard architectural-acoustics tables. Absorption is the
// dominant term for RT60; scattering is a coarse texture estimate used by the
// ray-tracing late tail.
struct PresetTable {
  std::array<float, kDefaultOctaveBands> absorption;
  std::array<float, kDefaultOctaveBands> scattering;
};

constexpr PresetTable kConcrete{{0.01f, 0.01f, 0.02f, 0.02f, 0.02f, 0.03f},
                                {0.10f, 0.10f, 0.10f, 0.10f, 0.10f, 0.10f}};
constexpr PresetTable kWood{{0.15f, 0.11f, 0.10f, 0.07f, 0.06f, 0.07f},
                            {0.10f, 0.12f, 0.14f, 0.16f, 0.18f, 0.20f}};
constexpr PresetTable kCurtain{{0.07f, 0.31f, 0.49f, 0.75f, 0.70f, 0.60f},
                               {0.10f, 0.15f, 0.25f, 0.35f, 0.45f, 0.50f}};
constexpr PresetTable kCarpet{{0.02f, 0.06f, 0.14f, 0.37f, 0.60f, 0.65f},
                              {0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f}};
constexpr PresetTable kGlass{{0.18f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f},
                             {0.05f, 0.05f, 0.05f, 0.05f, 0.05f, 0.05f}};

Material from_table(const PresetTable& table) {
  Material m;
  m.absorption.assign(table.absorption.begin(), table.absorption.end());
  m.scattering.assign(table.scattering.begin(), table.scattering.end());
  return m;
}
}  // namespace

Material make_material(MaterialPreset preset) {
  switch (preset) {
    case MaterialPreset::Concrete:
      return from_table(kConcrete);
    case MaterialPreset::Wood:
      return from_table(kWood);
    case MaterialPreset::Curtain:
      return from_table(kCurtain);
    case MaterialPreset::Carpet:
      return from_table(kCarpet);
    case MaterialPreset::Glass:
      return from_table(kGlass);
  }
  return from_table(kConcrete);  // unreachable; keeps the compiler happy
}

Material uniform_material(float absorption, float scattering, int n_bands) {
  const int n = n_bands > 0 ? n_bands : kDefaultOctaveBands;
  Material m;
  m.absorption.assign(static_cast<size_t>(n), std::clamp(absorption, 0.0f, 1.0f));
  m.scattering.assign(static_cast<size_t>(n), std::clamp(scattering, 0.0f, 1.0f));
  return m;
}

Material mix_materials(const Material& a, const Material& b, float t) {
  const float w = std::clamp(t, 0.0f, 1.0f);
  Material m;
  // Use a single common band count for both vectors so the result preserves the
  // Material invariant absorption.size() == scattering.size() even if an input
  // material is itself ragged. The shorter length across all four vectors wins.
  const size_t n = std::min(
      {a.absorption.size(), b.absorption.size(), a.scattering.size(), b.scattering.size()});
  m.absorption.resize(n);
  m.scattering.resize(n);
  for (size_t i = 0; i < n; ++i) {
    m.absorption[i] = a.absorption[i] * (1.0f - w) + b.absorption[i] * w;
    m.scattering[i] = a.scattering[i] * (1.0f - w) + b.scattering[i] * w;
  }
  return m;
}

}  // namespace sonare::acoustic
