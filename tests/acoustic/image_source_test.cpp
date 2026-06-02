#include "acoustic/image_source.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/room_model.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace sonare;
using namespace sonare::acoustic;

namespace {
ShoeboxRoom make_room(float l, float w, float h, float alpha = 0.0f) {
  ShoeboxRoom room;
  room.dims = {l, w, h};
  for (auto& wall : room.walls) wall = uniform_material(alpha, 0.1f);
  return room;
}

int count_order(const std::vector<ImageSource>& v, int order) {
  return static_cast<int>(
      std::count_if(v.begin(), v.end(), [&](const ImageSource& i) { return i.order == order; }));
}

// Nearest image to a target position (for asserting a specific reflection).
const ImageSource* find_near(const std::vector<ImageSource>& v, Vec3 target, float tol = 1e-3f) {
  for (const auto& im : v) {
    if (length(im.position - target) < tol) return &im;
  }
  return nullptr;
}

std::vector<float> sorted_distances(const std::vector<ImageSource>& v, int order) {
  std::vector<float> d;
  for (const auto& im : v)
    if (im.order == order) d.push_back(im.distance);
  std::sort(d.begin(), d.end());
  return d;
}
}  // namespace

TEST_CASE("shoebox direct path is the only order-0 image", "[acoustic][image_source]") {
  const ShoeboxRoom room = make_room(5.0f, 4.0f, 3.0f);
  const SourceListener pl{{1.0f, 1.0f, 1.0f}, {2.0f, 3.0f, 2.0f}};
  const auto images = shoebox_image_sources(room, pl, 0);
  REQUIRE(images.size() == 1);
  REQUIRE(images[0].order == 0);
  REQUIRE(images[0].position.x == 1.0f);
  REQUIRE_THAT(images[0].distance, WithinAbs(std::sqrt(6.0f), 1e-4f));  // |(1,1,1)-(2,3,2)|
  for (float r : images[0].reflection) REQUIRE_THAT(r, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("shoebox first-order enumeration: one image per wall", "[acoustic][image_source]") {
  const ShoeboxRoom room = make_room(5.0f, 4.0f, 3.0f);
  const SourceListener pl{{1.0f, 1.0f, 1.0f}, {2.0f, 3.0f, 2.0f}};
  const auto images = shoebox_image_sources(room, pl, 1);
  REQUIRE(count_order(images, 0) == 1);
  REQUIRE(count_order(images, 1) == 6);  // 2 walls per axis

  // Image off the x=0 wall: (-Sx, Sy, Sz).
  const ImageSource* xmin = find_near(images, {-1.0f, 1.0f, 1.0f});
  REQUIRE(xmin != nullptr);
  REQUIRE(xmin->order == 1);
  // distance |(-1,1,1)-(2,3,2)| = sqrt(9+4+1) = sqrt(14)
  REQUIRE_THAT(xmin->distance, WithinAbs(std::sqrt(14.0f), 1e-4f));

  // Image off the x=L wall: (2L - Sx, Sy, Sz) = (9,1,1).
  REQUIRE(find_near(images, {9.0f, 1.0f, 1.0f}) != nullptr);
}

TEST_CASE("shoebox reflection coefficient is the wall's per-band beta",
          "[acoustic][image_source]") {
  ShoeboxRoom room = make_room(5.0f, 4.0f, 3.0f, /*alpha=*/0.0f);  // rigid by default
  room.walls[kWallXMin] = uniform_material(0.19f, 0.1f);           // beta = sqrt(0.81) = 0.9
  const SourceListener pl{{1.0f, 1.0f, 1.0f}, {2.0f, 3.0f, 2.0f}};
  const auto images = shoebox_image_sources(room, pl, 1);

  const ImageSource* xmin = find_near(images, {-1.0f, 1.0f, 1.0f});
  REQUIRE(xmin != nullptr);
  for (float r : xmin->reflection) REQUIRE_THAT(r, WithinAbs(0.9f, 1e-4f));

  // A reflection off a rigid wall keeps beta = 1.
  const ImageSource* xmax = find_near(images, {9.0f, 1.0f, 1.0f});
  REQUIRE(xmax != nullptr);
  for (float r : xmax->reflection) REQUIRE_THAT(r, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("higher order grows the image count monotonically", "[acoustic][image_source]") {
  const ShoeboxRoom room = make_room(5.0f, 4.0f, 3.0f);
  const SourceListener pl{{1.2f, 1.0f, 1.4f}, {3.0f, 3.0f, 1.6f}};
  const auto n1 = shoebox_image_sources(room, pl, 1).size();
  const auto n2 = shoebox_image_sources(room, pl, 2).size();
  const auto n3 = shoebox_image_sources(room, pl, 3).size();
  REQUIRE(n1 < n2);
  REQUIRE(n2 < n3);
}

TEST_CASE("early IR places a reflection within +/-1 sample with correct gain/polarity",
          "[acoustic][image_source]") {
  ImageSource direct;
  direct.order = 0;
  direct.position = {0.0f, 0.0f, 0.0f};
  direct.distance = 2.0f;
  direct.reflection.assign(6, 1.0f);

  EarlyIrConfig cfg;
  cfg.sound_speed = 343.0f;
  const int sr = 48000;
  const Audio ir = synthesize_early_ir({direct}, sr, cfg);

  const float delay = direct.distance / cfg.sound_speed * sr;  // ~279.88
  const int expected = static_cast<int>(std::lround(delay));   // 280
  const float* d = ir.data();
  const int n = static_cast<int>(ir.size());
  int peak = 0;
  for (int i = 1; i < n; ++i)
    if (std::fabs(d[i]) > std::fabs(d[peak])) peak = i;

  REQUIRE(std::abs(peak - expected) <= 1);
  REQUIRE(d[peak] > 0.0f);  // polarity preserved
  // The peak *sample* of a fractional-delay sinc is < gain (energy spreads
  // across taps); the frac-independent amplitude is the kernel sum (sinc is a
  // partition of unity), which reconstructs gain = 1/(4π·distance).
  const float gain = 1.0f / (4.0f * 3.14159265f * direct.distance);
  float sum = 0.0f;
  for (int i = 0; i < n; ++i) sum += d[i];
  REQUIRE_THAT(sum, WithinRel(gain, 0.02f));
}

TEST_CASE("early IR amplitude (kernel sum) is independent of the fractional delay",
          "[acoustic][image_source]") {
  const int sr = 48000;
  // Two distances chosen to give very different fractional sample delays.
  for (float dist : {1.7f, 2.001f, 3.337f}) {
    ImageSource im;
    im.distance = dist;
    im.reflection.assign(1, 1.0f);
    const Audio ir = synthesize_early_ir({im}, sr);
    float sum = 0.0f;
    for (size_t i = 0; i < ir.size(); ++i) sum += ir.data()[i];
    const float gain = 1.0f / (4.0f * 3.14159265f * dist);
    REQUIRE_THAT(sum, WithinRel(gain, 0.02f));
  }
}

TEST_CASE("empty image set yields a safe single-sample IR", "[acoustic][image_source]") {
  const Audio ir = synthesize_early_ir({}, 48000);
  REQUIRE(ir.size() >= 1);
  REQUIRE(ir.data()[0] == 0.0f);
}

// ---- Borish polyhedral ----

namespace {
std::vector<Triangle> unit_cube() {
  const Vec3 v000{0, 0, 0}, v100{1, 0, 0}, v110{1, 1, 0}, v010{0, 1, 0};
  const Vec3 v001{0, 0, 1}, v101{1, 0, 1}, v111{1, 1, 1}, v011{0, 1, 1};
  return {{v000, v110, v100}, {v000, v010, v110}, {v001, v101, v111}, {v001, v111, v011},
          {v000, v100, v101}, {v000, v101, v001}, {v010, v111, v110}, {v010, v011, v111},
          {v000, v001, v011}, {v000, v011, v010}, {v100, v110, v111}, {v100, v111, v101}};
}
}  // namespace

TEST_CASE("Borish cube order-1 reflections match the shoebox image method",
          "[acoustic][image_source]") {
  PolyhedralRoom mesh;
  mesh.faces = unit_cube();
  mesh.face_materials = {uniform_material(0.0f, 0.1f)};  // rigid, single material
  // Asymmetric placement so each wall's reflection point lands inside one
  // triangle (not on a shared diagonal).
  const SourceListener pl{{0.3f, 0.45f, 0.55f}, {0.7f, 0.6f, 0.4f}};

  const auto borish = polyhedral_image_sources(mesh, pl, 1);
  const ShoeboxRoom box = make_room(1.0f, 1.0f, 1.0f);
  const auto shoebox = shoebox_image_sources(box, pl, 1);

  // Same direct path.
  REQUIRE(count_order(borish, 0) == 1);
  // One first-order reflection per wall (6), matching the analytic distances.
  REQUIRE(count_order(borish, 1) == 6);
  const auto db = sorted_distances(borish, 1);
  const auto ds = sorted_distances(shoebox, 1);
  REQUIRE(db.size() == ds.size());
  for (size_t i = 0; i < db.size(); ++i) REQUIRE_THAT(db[i], WithinAbs(ds[i], 1e-3f));
}

TEST_CASE("Borish does not double-count reflections on coplanar triangles",
          "[acoustic][image_source]") {
  PolyhedralRoom mesh;
  mesh.faces = unit_cube();
  mesh.face_materials = {uniform_material(0.0f, 0.1f)};
  // Symmetric placement: specular points land on the shared diagonals of the
  // wall triangle pairs — the case that previously emitted duplicate images.
  const SourceListener pl{{0.3f, 0.3f, 0.5f}, {0.6f, 0.6f, 0.5f}};
  const auto images = polyhedral_image_sources(mesh, pl, 1);
  REQUIRE(count_order(images, 1) == 6);  // one per wall, not 8
}

TEST_CASE("Borish order-2 cube images are valid (subset of shoebox, no spurious)",
          "[acoustic][image_source]") {
  PolyhedralRoom mesh;
  mesh.faces = unit_cube();
  mesh.face_materials = {uniform_material(0.0f, 0.1f)};
  const SourceListener pl{{0.35f, 0.42f, 0.55f}, {0.62f, 0.58f, 0.47f}};

  const auto borish = polyhedral_image_sources(mesh, pl, 2);
  const ShoeboxRoom box = make_room(1.0f, 1.0f, 1.0f);
  const auto shoebox = shoebox_image_sources(box, pl, 2);
  const auto ref = sorted_distances(shoebox, 2);

  // Every Borish order-2 distance must correspond to a real shoebox image
  // (no spurious/duplicate reflections), and there must be at least one.
  const auto got = sorted_distances(borish, 2);
  REQUIRE(!got.empty());
  for (float dist : got) {
    const bool matched =
        std::any_of(ref.begin(), ref.end(), [&](float r) { return std::fabs(r - dist) < 1e-3f; });
    REQUIRE(matched);
  }
  // No duplicate positions among accepted order-2 images.
  for (size_t i = 0; i < borish.size(); ++i) {
    for (size_t j = i + 1; j < borish.size(); ++j) {
      if (borish[i].order == 2 && borish[j].order == 2) {
        REQUIRE(length(borish[i].position - borish[j].position) > 1e-4f);
      }
    }
  }
}

TEST_CASE("negative order and empty mesh yield no images", "[acoustic][image_source]") {
  const ShoeboxRoom room = make_room(4.0f, 3.0f, 2.5f);
  const SourceListener pl{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 1.5f}};
  REQUIRE(shoebox_image_sources(room, pl, -1).empty());

  PolyhedralRoom empty;
  REQUIRE(polyhedral_image_sources(empty, pl, 2).empty());

  PolyhedralRoom mesh;
  mesh.faces = unit_cube();
  mesh.face_materials = {uniform_material(0.0f, 0.1f)};
  REQUIRE(polyhedral_image_sources(mesh, pl, -1).empty());
}

TEST_CASE("per-band reflection coefficients and band selection", "[acoustic][image_source]") {
  ShoeboxRoom room = make_room(5.0f, 4.0f, 3.0f, /*alpha=*/0.0f);
  // XMin gets frequency-dependent absorption: β0 = sqrt(0.81) = 0.9, β1 = sqrt(0.49) = 0.7.
  Material m;
  m.absorption = {0.19f, 0.51f, 0.0f, 0.0f, 0.0f, 0.0f};
  m.scattering = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
  room.walls[kWallXMin] = m;
  const SourceListener pl{{1.0f, 1.0f, 1.0f}, {2.0f, 3.0f, 2.0f}};
  const auto images = shoebox_image_sources(room, pl, 1);

  const ImageSource* xmin = find_near(images, {-1.0f, 1.0f, 1.0f});
  REQUIRE(xmin != nullptr);
  REQUIRE(xmin->reflection.size() == 6);
  REQUIRE_THAT(xmin->reflection[0], WithinAbs(0.9f, 1e-4f));
  REQUIRE_THAT(xmin->reflection[1], WithinAbs(0.7f, 1e-4f));

  // Band selection picks the corresponding reflection coefficient.
  EarlyIrConfig c0;
  c0.band = 0;
  EarlyIrConfig c1;
  c1.band = 1;
  auto kernel_sum = [&](const Audio& ir) {
    float s = 0.0f;
    for (size_t i = 0; i < ir.size(); ++i) s += ir.data()[i];
    return s;
  };
  const float g0 = kernel_sum(synthesize_early_ir({*xmin}, 48000, c0));
  const float g1 = kernel_sum(synthesize_early_ir({*xmin}, 48000, c1));
  REQUIRE(g0 > g1);  // 0.9 vs 0.7
  REQUIRE_THAT(g0 / g1, WithinRel(0.9f / 0.7f, 0.02f));
}

TEST_CASE("rigid-wall fallback when wall materials are empty", "[acoustic][image_source]") {
  ShoeboxRoom room;
  room.dims = {4.0f, 3.0f, 2.5f};  // walls left default-constructed (empty materials)
  const SourceListener pl{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 1.5f}};
  const auto images = shoebox_image_sources(room, pl, 1);
  REQUIRE(count_order(images, 1) == 6);
  for (const auto& im : images) {
    REQUIRE(im.reflection.size() == 1);  // single rigid band
    REQUIRE_THAT(im.reflection[0], WithinAbs(1.0f, 1e-6f));
  }
}

TEST_CASE("Borish rejects a reflection whose specular point is off the face",
          "[acoustic][image_source]") {
  // A single small floor triangle near the origin.
  PolyhedralRoom mesh;
  mesh.faces = {{{0.0f, 0.0f, 0.0f}, {0.2f, 0.0f, 0.0f}, {0.0f, 0.2f, 0.0f}}};
  mesh.face_materials = {uniform_material(0.0f, 0.1f)};

  SECTION("specular point far outside the triangle -> rejected") {
    // Source/listener high above and offset; the mirror point lands far from
    // the tiny triangle.
    const SourceListener pl{{1.0f, 1.0f, 5.0f}, {1.0f, 1.5f, 5.0f}};
    const auto images = polyhedral_image_sources(mesh, pl, 1);
    REQUIRE(count_order(images, 0) == 1);  // direct path is clear
    REQUIRE(count_order(images, 1) == 0);  // reflection rejected (off-face)
  }

  SECTION("specular point on the triangle -> accepted") {
    // Place both points just above the small triangle so the mirror point lands
    // inside it.
    const SourceListener pl{{0.05f, 0.05f, 0.1f}, {0.08f, 0.06f, 0.1f}};
    const auto images = polyhedral_image_sources(mesh, pl, 1);
    REQUIRE(count_order(images, 1) == 1);  // one valid reflection off the floor
  }
}

TEST_CASE("Borish rejects the direct path when occluded", "[acoustic][image_source]") {
  // A large wall in the x = 0.5 plane separating source and listener.
  PolyhedralRoom mesh;
  mesh.faces = {{{0.5f, -5.0f, -5.0f}, {0.5f, 5.0f, -5.0f}, {0.5f, 5.0f, 5.0f}},
                {{0.5f, -5.0f, -5.0f}, {0.5f, 5.0f, 5.0f}, {0.5f, -5.0f, 5.0f}}};
  mesh.face_materials = {uniform_material(0.0f, 0.1f)};
  const SourceListener pl{{0.2f, 0.0f, 0.0f}, {0.8f, 0.0f, 0.0f}};  // opposite sides
  const auto images = polyhedral_image_sources(mesh, pl, 1);
  REQUIRE(count_order(images, 0) == 0);  // direct path blocked by the wall
}

TEST_CASE("synthesize_early_ir skips non-finite image distances", "[acoustic][image_source]") {
  // A finite reflection plus a hand-built image carrying a NaN distance: the
  // NaN must not be rendered into the IR (the render-loop and max-delay guards
  // treat non-finite distances identically). Result must stay all-finite.
  std::vector<ImageSource> images;
  ImageSource good;
  good.order = 0;
  good.position = {1.0f, 0.0f, 0.0f};
  good.distance = 1.0f;
  good.reflection = {1.0f};
  images.push_back(good);

  ImageSource bad;
  bad.order = 1;
  bad.position = {2.0f, 0.0f, 0.0f};
  bad.distance = std::numeric_limits<float>::quiet_NaN();
  bad.reflection = {1.0f};
  images.push_back(bad);

  const Audio ir = synthesize_early_ir(images, 48000);
  REQUIRE(ir.size() > 0);
  bool all_finite = true;
  for (float s : ir) all_finite = all_finite && std::isfinite(s);
  REQUIRE(all_finite);
}
