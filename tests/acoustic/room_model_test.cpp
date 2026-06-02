#include "acoustic/room_model.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "acoustic/material.h"
#include "core/diagnostic.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::acoustic;

namespace {
// Closed unit cube [0,1]^3 as 12 triangles (outward winding, irrelevant to the
// parity tests but kept consistent).
std::vector<Triangle> unit_cube() {
  const Vec3 v000{0, 0, 0}, v100{1, 0, 0}, v110{1, 1, 0}, v010{0, 1, 0};
  const Vec3 v001{0, 0, 1}, v101{1, 0, 1}, v111{1, 1, 1}, v011{0, 1, 1};
  return {
      // z = 0
      {v000, v110, v100},
      {v000, v010, v110},
      // z = 1
      {v001, v101, v111},
      {v001, v111, v011},
      // y = 0
      {v000, v100, v101},
      {v000, v101, v001},
      // y = 1
      {v010, v111, v110},
      {v010, v011, v111},
      // x = 0
      {v000, v001, v011},
      {v000, v011, v010},
      // x = 1
      {v100, v110, v111},
      {v100, v111, v101},
  };
}

// A non-axis-aligned tetrahedron: the adversarial case for voxel binning/DDA,
// since faces are slanted and span cells diagonally.
std::vector<Triangle> tetrahedron() {
  const Vec3 a{0.0f, 0.0f, 0.0f}, b{1.0f, 0.0f, 0.0f}, c{0.3f, 1.0f, 0.1f}, d{0.4f, 0.2f, 1.0f};
  return {{a, b, c}, {a, c, d}, {a, d, b}, {b, d, c}};
}

// Two coplanar triangles in the z=0 plane (zero-extent z axis).
std::vector<Triangle> flat_quad() {
  const Vec3 v00{0, 0, 0}, v10{1, 0, 0}, v11{1, 1, 0}, v01{0, 1, 0};
  return {{v00, v10, v11}, {v00, v11, v01}};
}

bool has_code(const std::vector<Diagnostic>& d, const char* code) {
  for (const auto& x : d)
    if (x.code == code) return true;
  return false;
}

struct RayCase {
  Vec3 origin, dir;
};

// Invariant: voxel first_hit agrees with brute force on hit/miss and nearest
// distance. Face index is a tie-break-dependent detail, so here it is only
// sanity-checked (>= 0), not required equal.
void require_grid_matches_bruteforce(const std::vector<Triangle>& mesh, int resolution,
                                     const std::vector<RayCase>& rays) {
  VoxelGrid grid;
  grid.build(mesh, resolution);
  for (const auto& r : rays) {
    const MeshHit bf = mesh_first_hit_bruteforce(mesh, r.origin, r.dir);
    const MeshHit vg = grid.first_hit(r.origin, r.dir);
    REQUIRE(vg.hit == bf.hit);
    if (bf.hit) {
      REQUIRE_THAT(vg.t, WithinAbs(bf.t, 1e-4f));
      REQUIRE(vg.face >= 0);
    }
  }
}
}  // namespace

TEST_CASE("shoebox geometry derives from dimensions", "[acoustic][room_model]") {
  ShoeboxRoom room;
  room.dims = {5.0f, 4.0f, 3.0f};
  REQUIRE_THAT(shoebox_volume(room), WithinAbs(60.0f, 1e-4f));
  // 2*(5*4 + 5*3 + 4*3) = 2*(20+15+12) = 94
  REQUIRE_THAT(shoebox_surface_area(room), WithinAbs(94.0f, 1e-4f));
}

TEST_CASE("point_inside_shoebox", "[acoustic][room_model]") {
  ShoeboxRoom room;
  room.dims = {2.0f, 2.0f, 2.0f};
  REQUIRE(point_inside_shoebox(room, {1.0f, 1.0f, 1.0f}));
  REQUIRE(point_inside_shoebox(room, {0.0f, 0.0f, 0.0f}));  // on boundary
  REQUIRE_FALSE(point_inside_shoebox(room, {2.5f, 1.0f, 1.0f}));
  REQUIRE_FALSE(point_inside_shoebox(room, {1.0f, -0.1f, 1.0f}));
}

TEST_CASE("shoebox validation reports placement errors as diagnostics", "[acoustic][room_model]") {
  ShoeboxRoom room;
  room.dims = {4.0f, 3.0f, 2.5f};

  SECTION("valid placement yields no errors") {
    SourceListener pl{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 1.5f}};
    const auto d = validate_shoebox(room, pl);
    REQUIRE_FALSE(has_error(d));
  }

  SECTION("source outside the room is an Error, not a crash") {
    SourceListener pl{{10.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 1.5f}};
    const auto d = validate_shoebox(room, pl);
    REQUIRE(has_error(d));
    REQUIRE(has_code(d, "acoustic.source_outside_room"));
  }

  SECTION("listener outside the room is a distinct Error") {
    SourceListener pl{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 9.0f}};
    const auto d = validate_shoebox(room, pl);
    REQUIRE(has_code(d, "acoustic.listener_outside_room"));
    REQUIRE_FALSE(has_code(d, "acoustic.source_outside_room"));
  }

  SECTION("invalid dimensions short-circuit to a single Error") {
    ShoeboxRoom bad;
    bad.dims = {0.0f, 3.0f, 2.5f};
    const auto d = validate_shoebox(bad, {{0, 0, 0}, {0, 0, 0}});
    REQUIRE(has_code(d, "acoustic.invalid_dimensions"));
  }

  SECTION("coincident source/listener is a Warning") {
    SourceListener pl{{1.5f, 1.5f, 1.0f}, {1.5f, 1.5f, 1.0f}};
    const auto d = validate_shoebox(room, pl);
    REQUIRE_FALSE(has_error(d));
    REQUIRE(has_code(d, "acoustic.source_equals_listener"));
  }
}

TEST_CASE("point_inside_mesh on a closed cube", "[acoustic][room_model]") {
  const auto cube = unit_cube();
  REQUIRE(point_inside_mesh(cube, {0.5f, 0.5f, 0.5f}));
  REQUIRE_FALSE(point_inside_mesh(cube, {1.5f, 0.5f, 0.5f}));
  REQUIRE_FALSE(point_inside_mesh(cube, {-0.5f, 0.5f, 0.5f}));
}

TEST_CASE("voxel grid first_hit matches brute force", "[acoustic][room_model]") {
  // Asymmetric origins/directions so hits land in face interiors.
  const std::vector<RayCase> cube_rays{
      {{0.4f, 0.55f, 0.45f}, {1.0f, 0.13f, -0.07f}},
      {{0.4f, 0.55f, 0.45f}, {-1.0f, 0.11f, 0.09f}},
      {{0.6f, 0.35f, 0.52f}, {0.1f, 1.0f, 0.05f}},
      {{0.52f, 0.61f, 0.33f}, {-0.08f, -1.0f, 0.06f}},
      {{0.45f, 0.5f, 0.4f}, {0.07f, 0.04f, 1.0f}},
      {{-1.0f, 0.43f, 0.57f}, {1.0f, 0.05f, -0.03f}},  // enters from outside
      {{0.3f, 0.7f, 0.2f}, {0.21f, -0.53f, 0.88f}},
      {{2.0f, 2.0f, 2.0f}, {1.0f, 1.0f, 1.0f}},  // points away -> miss
  };

  SECTION("cube across resolutions (1 = single cell, 8, 64)") {
    for (int res : {1, 8, 64}) {
      require_grid_matches_bruteforce(unit_cube(), res, cube_rays);
    }
  }

  SECTION("non-axis-aligned tetrahedron (slanted faces span cells)") {
    const std::vector<RayCase> rays{
        {{0.4f, 0.3f, 0.3f}, {1.0f, 0.2f, 0.1f}},  {{0.4f, 0.3f, 0.3f}, {-0.5f, 1.0f, 0.3f}},
        {{0.4f, 0.3f, 0.3f}, {0.1f, -0.2f, 1.0f}}, {{0.4f, 0.3f, 0.3f}, {-1.0f, -1.0f, -1.0f}},
        {{2.0f, 2.0f, 2.0f}, {1.0f, 1.0f, 1.0f}},  // miss
    };
    require_grid_matches_bruteforce(tetrahedron(), 8, rays);
  }

  SECTION("flat (zero-extent z) mesh") {
    const std::vector<RayCase> rays{
        {{0.5f, 0.5f, 1.0f}, {0.0f, 0.0f, -1.0f}},   // straight down onto quad
        {{0.3f, 0.3f, 1.0f}, {0.1f, 0.05f, -1.0f}},  // oblique hit
        {{0.5f, 0.5f, 1.0f}, {0.0f, 0.0f, 1.0f}},    // away -> miss
    };
    require_grid_matches_bruteforce(flat_quad(), 8, rays);
  }
}

TEST_CASE("voxel and brute force agree on face identity at a shared edge",
          "[acoustic][room_model]") {
  // Two triangles meeting along the diagonal edge (0,0,0)-(1,1,0). A ray aimed
  // through a point on that shared edge hits both at the same distance; the
  // deterministic tie-break must make both query paths return the same (lower)
  // face index.
  const std::vector<Triangle> mesh{
      {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}},
      {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
  };
  VoxelGrid grid;
  grid.build(mesh, 8);
  // Aim straight down at the midpoint of the shared diagonal (0.5,0.5,0).
  const Vec3 origin{0.5f, 0.5f, 1.0f}, dir{0.0f, 0.0f, -1.0f};
  const MeshHit bf = mesh_first_hit_bruteforce(mesh, origin, dir);
  const MeshHit vg = grid.first_hit(origin, dir);
  REQUIRE(bf.hit);
  REQUIRE(vg.hit);
  REQUIRE_THAT(vg.t, WithinAbs(bf.t, 1e-4f));
  REQUIRE(vg.face == bf.face);  // identical face on the tie
}

TEST_CASE("voxel grid on an empty mesh returns no hit without crashing", "[acoustic][room_model]") {
  const std::vector<Triangle> no_faces;  // must outlive the grid (non-owning)
  VoxelGrid grid;
  grid.build(no_faces);
  REQUIRE(grid.empty());
  const MeshHit h = grid.first_hit({0, 0, 0}, {1, 0, 0});
  REQUIRE_FALSE(h.hit);
}

TEST_CASE("polyhedral validation reports mesh problems as diagnostics", "[acoustic][room_model]") {
  const auto cube = unit_cube();

  SECTION("valid mesh + interior placement: no errors") {
    PolyhedralRoom room;
    room.faces = cube;
    room.face_materials = {make_material(MaterialPreset::Wood)};  // single
    SourceListener pl{{0.5f, 0.5f, 0.5f}, {0.3f, 0.3f, 0.3f}};
    const auto d = validate_polyhedral(room, pl);
    REQUIRE_FALSE(has_error(d));
  }

  SECTION("empty mesh is an Error") {
    PolyhedralRoom room;
    const auto d = validate_polyhedral(room, {{0, 0, 0}, {0, 0, 0}});
    REQUIRE(has_code(d, "acoustic.empty_mesh"));
  }

  SECTION("material count mismatch is an Error") {
    PolyhedralRoom room;
    room.faces = cube;
    room.face_materials = {make_material(MaterialPreset::Wood),
                           make_material(MaterialPreset::Glass)};  // 2 != 12, != 1
    SourceListener pl{{0.5f, 0.5f, 0.5f}, {0.3f, 0.3f, 0.3f}};
    const auto d = validate_polyhedral(room, pl);
    REQUIRE(has_code(d, "acoustic.material_count_mismatch"));
  }

  SECTION("source outside the mesh is an Error") {
    PolyhedralRoom room;
    room.faces = cube;
    room.face_materials = {make_material(MaterialPreset::Wood)};
    SourceListener pl{{5.0f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}};
    const auto d = validate_polyhedral(room, pl);
    REQUIRE(has_code(d, "acoustic.source_outside_room"));
  }

  SECTION("listener outside the mesh is a distinct Error") {
    PolyhedralRoom room;
    room.faces = cube;
    room.face_materials = {make_material(MaterialPreset::Wood)};
    SourceListener pl{{0.5f, 0.5f, 0.5f}, {5.0f, 0.5f, 0.5f}};
    const auto d = validate_polyhedral(room, pl);
    REQUIRE(has_code(d, "acoustic.listener_outside_room"));
  }

  SECTION("empty face_materials is an Error (no crash)") {
    PolyhedralRoom room;
    room.faces = cube;  // non-empty faces, empty materials
    SourceListener pl{{0.5f, 0.5f, 0.5f}, {0.3f, 0.3f, 0.3f}};
    const auto d = validate_polyhedral(room, pl);
    REQUIRE(has_code(d, "acoustic.no_materials"));
  }

  SECTION("degenerate (zero-area) face is a Warning, not an Error") {
    PolyhedralRoom room;
    room.faces = cube;
    // Append a collapsed triangle (all vertices colinear/equal).
    room.faces.push_back({{0.2f, 0.2f, 0.2f}, {0.2f, 0.2f, 0.2f}, {0.4f, 0.4f, 0.4f}});
    room.face_materials = {make_material(MaterialPreset::Wood)};
    SourceListener pl{{0.5f, 0.5f, 0.5f}, {0.3f, 0.3f, 0.3f}};
    const auto d = validate_polyhedral(room, pl);
    REQUIRE(has_code(d, "acoustic.degenerate_face"));
    REQUIRE_FALSE(has_error(d));
  }
}

TEST_CASE("face_material honours single-material shorthand", "[acoustic][room_model]") {
  PolyhedralRoom room;
  room.faces = unit_cube();
  room.face_materials = {make_material(MaterialPreset::Carpet)};
  // Any index returns the single material.
  REQUIRE(face_material(room, 0).absorption == room.face_materials[0].absorption);
  REQUIRE(face_material(room, 11).absorption == room.face_materials[0].absorption);
}

TEST_CASE("face_material per-face lookup and out-of-range clamp", "[acoustic][room_model]") {
  PolyhedralRoom room;
  room.faces = {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  room.face_materials = {make_material(MaterialPreset::Wood), make_material(MaterialPreset::Glass)};
  REQUIRE(face_material(room, 0).absorption == room.face_materials[0].absorption);
  REQUIRE(face_material(room, 1).absorption == room.face_materials[1].absorption);
  // Out-of-range index clamps to the last material.
  REQUIRE(face_material(room, 99).absorption == room.face_materials[1].absorption);
}

TEST_CASE("face_material on empty materials is memory-safe", "[acoustic][room_model]") {
  PolyhedralRoom room;
  room.faces = unit_cube();  // no materials assigned
  const Material& m = face_material(room, 0);
  REQUIRE(m.absorption.empty());  // returns the static empty fallback, no crash
}
