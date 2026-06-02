#include "acoustic/room_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sonare::acoustic {

namespace {
constexpr float kInf = std::numeric_limits<float>::infinity();

bool finite_positive(float v) noexcept { return std::isfinite(v) && v > 0.0f; }

// Slab test: ray (o + t*d, t>=0) vs AABB. Returns the entry/exit parameters.
bool ray_aabb(const Aabb& box, const Vec3& o, const Vec3& d, float& t_enter,
              float& t_exit) noexcept {
  constexpr float kEps = 1e-12f;
  float tmin = 0.0f;
  float tmax = kInf;
  const float od[3] = {o.x, o.y, o.z};
  const float dd[3] = {d.x, d.y, d.z};
  const float lo[3] = {box.min.x, box.min.y, box.min.z};
  const float hi[3] = {box.max.x, box.max.y, box.max.z};
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(dd[a]) < kEps) {
      if (od[a] < lo[a] || od[a] > hi[a]) return false;
    } else {
      const float inv = 1.0f / dd[a];
      float t1 = (lo[a] - od[a]) * inv;
      float t2 = (hi[a] - od[a]) * inv;
      if (t1 > t2) std::swap(t1, t2);
      tmin = std::max(tmin, t1);
      tmax = std::min(tmax, t2);
      if (tmin > tmax) return false;
    }
  }
  t_enter = tmin;
  t_exit = tmax;
  return true;
}

int clamp_cell(float coord, float lo, float cell, int n) noexcept {
  if (cell <= 0.0f) return 0;
  int i = static_cast<int>(std::floor((coord - lo) / cell));
  return std::clamp(i, 0, n - 1);
}

// Deterministic nearest-hit policy shared by the brute-force and voxel paths so
// they return identical results: strictly smaller distance wins; on a tie
// (within kTieEps), the lower face index wins. The Borish image-source
// visibility test relies on this stable face identity.
constexpr float kTieEps = 1e-6f;
inline bool better_hit(float t, int fi, const MeshHit& best) noexcept {
  return t < best.t - kTieEps || (t <= best.t + kTieEps && (!best.hit || fi < best.face));
}
}  // namespace

ShoeboxRoom uniform_shoebox(const RoomDimensions& dims, float absorption, float scattering) {
  ShoeboxRoom room;
  room.dims = dims;
  const Material wall = uniform_material(std::clamp(absorption, 0.0f, 0.999f), scattering);
  for (Material& w : room.walls) w = wall;
  return room;
}

float shoebox_volume(const ShoeboxRoom& room) noexcept { return room_volume(room.dims); }

float shoebox_surface_area(const ShoeboxRoom& room) noexcept {
  return room_surface_area(room.dims);
}

Plane wall_plane(const ShoeboxRoom& room, ShoeboxWall wall) noexcept {
  const float lx = room.dims.length, ly = room.dims.width, lz = room.dims.height;
  switch (wall) {
    case kWallXMin:
      return {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    case kWallXMax:
      return {{lx, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}};
    case kWallYMin:
      return {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    case kWallYMax:
      return {{0.0f, ly, 0.0f}, {0.0f, -1.0f, 0.0f}};
    case kWallZMin:
      return {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    case kWallZMax:
    default:
      return {{0.0f, 0.0f, lz}, {0.0f, 0.0f, -1.0f}};
  }
}

const Material& face_material(const PolyhedralRoom& room, size_t index) {
  static const Material kEmpty;
  // Guard the empty case explicitly: `size() - 1` would wrap to SIZE_MAX and the
  // subsequent index would be out of bounds. Validation reports empty materials
  // as an Error, but this accessor must stay memory-safe regardless.
  if (room.face_materials.empty()) return kEmpty;
  if (room.face_materials.size() == 1) return room.face_materials[0];
  const size_t i = std::min(index, room.face_materials.size() - 1);
  return room.face_materials[i];
}

MeshHit mesh_first_hit_bruteforce(const std::vector<Triangle>& faces, const Vec3& origin,
                                  const Vec3& dir, bool cull_backface) noexcept {
  MeshHit best;
  best.t = kInf;
  for (size_t f = 0; f < faces.size(); ++f) {
    float t = 0.0f;
    if (ray_triangle_intersect(origin, dir, faces[f], cull_backface, t) &&
        better_hit(t, static_cast<int>(f), best)) {
      best.hit = true;
      best.t = t;
      best.face = static_cast<int>(f);
    }
  }
  if (!best.hit) best.t = 0.0f;
  return best;
}

void VoxelGrid::build(const std::vector<Triangle>& faces, int resolution) {
  faces_ = &faces;
  cells_.clear();
  if (faces.empty()) {
    nx_ = ny_ = nz_ = 1;
    cells_.assign(1, {});
    return;
  }
  // Mesh AABB.
  Vec3 lo{kInf, kInf, kInf};
  Vec3 hi{-kInf, -kInf, -kInf};
  auto expand = [&](const Vec3& p) {
    lo.x = std::min(lo.x, p.x);
    lo.y = std::min(lo.y, p.y);
    lo.z = std::min(lo.z, p.z);
    hi.x = std::max(hi.x, p.x);
    hi.y = std::max(hi.y, p.y);
    hi.z = std::max(hi.z, p.z);
  };
  for (const auto& tri : faces) {
    expand(tri.a);
    expand(tri.b);
    expand(tri.c);
  }
  // Pad slightly so surface points map inside the grid.
  const Vec3 pad{1e-4f, 1e-4f, 1e-4f};
  bounds_.min = lo - pad;
  bounds_.max = hi + pad;

  const int res = std::max(1, resolution);
  const Vec3 extent = bounds_.max - bounds_.min;
  nx_ = extent.x > 0.0f ? res : 1;
  ny_ = extent.y > 0.0f ? res : 1;
  nz_ = extent.z > 0.0f ? res : 1;
  cell_size_ = {extent.x / static_cast<float>(nx_), extent.y / static_cast<float>(ny_),
                extent.z / static_cast<float>(nz_)};

  cells_.assign(static_cast<size_t>(nx_) * ny_ * nz_, {});

  // Bin each face into every cell its AABB overlaps (conservative).
  for (size_t f = 0; f < faces.size(); ++f) {
    const Triangle& tri = faces[f];
    Vec3 tlo{std::min({tri.a.x, tri.b.x, tri.c.x}), std::min({tri.a.y, tri.b.y, tri.c.y}),
             std::min({tri.a.z, tri.b.z, tri.c.z})};
    Vec3 thi{std::max({tri.a.x, tri.b.x, tri.c.x}), std::max({tri.a.y, tri.b.y, tri.c.y}),
             std::max({tri.a.z, tri.b.z, tri.c.z})};
    const int ix0 = clamp_cell(tlo.x, bounds_.min.x, cell_size_.x, nx_);
    const int ix1 = clamp_cell(thi.x, bounds_.min.x, cell_size_.x, nx_);
    const int iy0 = clamp_cell(tlo.y, bounds_.min.y, cell_size_.y, ny_);
    const int iy1 = clamp_cell(thi.y, bounds_.min.y, cell_size_.y, ny_);
    const int iz0 = clamp_cell(tlo.z, bounds_.min.z, cell_size_.z, nz_);
    const int iz1 = clamp_cell(thi.z, bounds_.min.z, cell_size_.z, nz_);
    for (int iz = iz0; iz <= iz1; ++iz) {
      for (int iy = iy0; iy <= iy1; ++iy) {
        for (int ix = ix0; ix <= ix1; ++ix) {
          cells_[static_cast<size_t>(cell_index(ix, iy, iz))].push_back(static_cast<int>(f));
        }
      }
    }
  }
}

MeshHit VoxelGrid::first_hit(const Vec3& origin, const Vec3& dir,
                             bool cull_backface) const noexcept {
  MeshHit best;
  best.t = kInf;
  if (empty()) {
    best.t = 0.0f;
    return best;
  }
  float t_enter, t_exit;
  if (!ray_aabb(bounds_, origin, dir, t_enter, t_exit)) {
    best.t = 0.0f;
    return best;
  }
  // Entry point nudged just inside the grid. The nudge is scaled by 1/|dir| so
  // the physical offset (~1e-5 m) is independent of the direction's magnitude
  // (callers may pass non-normalised directions).
  const float dir_len = length(dir);
  const float nudge = dir_len > 0.0f ? 1e-5f / dir_len : 0.0f;
  const Vec3 p = origin + dir * (t_enter + nudge);
  int ix = clamp_cell(p.x, bounds_.min.x, cell_size_.x, nx_);
  int iy = clamp_cell(p.y, bounds_.min.y, cell_size_.y, ny_);
  int iz = clamp_cell(p.z, bounds_.min.z, cell_size_.z, nz_);

  const float o[3] = {origin.x, origin.y, origin.z};
  const float d[3] = {dir.x, dir.y, dir.z};
  const float mn[3] = {bounds_.min.x, bounds_.min.y, bounds_.min.z};
  const float cs[3] = {cell_size_.x, cell_size_.y, cell_size_.z};
  int idx[3] = {ix, iy, iz};
  const int n[3] = {nx_, ny_, nz_};

  int step[3];
  float t_max[3];
  float t_delta[3];
  for (int a = 0; a < 3; ++a) {
    if (cs[a] <= 0.0f || std::fabs(d[a]) < 1e-12f) {
      step[a] = 0;
      t_max[a] = kInf;
      t_delta[a] = kInf;
    } else if (d[a] > 0.0f) {
      step[a] = 1;
      const float next = mn[a] + static_cast<float>(idx[a] + 1) * cs[a];
      t_max[a] = (next - o[a]) / d[a];
      t_delta[a] = cs[a] / d[a];
    } else {
      step[a] = -1;
      const float next = mn[a] + static_cast<float>(idx[a]) * cs[a];
      t_max[a] = (next - o[a]) / d[a];
      t_delta[a] = -cs[a] / d[a];
    }
  }

  while (idx[0] >= 0 && idx[0] < n[0] && idx[1] >= 0 && idx[1] < n[1] && idx[2] >= 0 &&
         idx[2] < n[2]) {
    const auto& bucket = cells_[static_cast<size_t>(cell_index(idx[0], idx[1], idx[2]))];
    for (int f : bucket) {
      float t = 0.0f;
      if (ray_triangle_intersect(origin, dir, (*faces_)[static_cast<size_t>(f)], cull_backface,
                                 t) &&
          better_hit(t, f, best)) {
        best.hit = true;
        best.t = t;
        best.face = f;
      }
    }
    const float exit_t = std::min({t_max[0], t_max[1], t_max[2]});
    // Continue one cell layer past the hit when it is within kTieEps of the cell
    // exit, so a tied face (same intersection, lower index) binned into the next
    // cell is still considered — keeping first_hit identical to the brute-force
    // tie-break policy in better_hit().
    if (best.hit && best.t + kTieEps <= exit_t) break;
    if (exit_t == kInf) break;  // no further cells reachable
    // Advance along the axis with the nearest boundary.
    int axis = 0;
    if (t_max[1] < t_max[axis]) axis = 1;
    if (t_max[2] < t_max[axis]) axis = 2;
    idx[axis] += step[axis];
    t_max[axis] += t_delta[axis];
  }

  if (!best.hit) best.t = 0.0f;
  return best;
}

bool point_inside_shoebox(const ShoeboxRoom& room, const Vec3& p) noexcept {
  return p.x >= 0.0f && p.x <= room.dims.length && p.y >= 0.0f && p.y <= room.dims.width &&
         p.z >= 0.0f && p.z <= room.dims.height;
}

bool point_inside_mesh(const std::vector<Triangle>& faces, const Vec3& p) noexcept {
  // Parity ray cast; odd crossing count => inside a closed mesh. A generic
  // (non-axis-aligned, asymmetric) direction avoids grazing the shared edges of
  // axis-aligned meshes, which would otherwise double-count and flip the parity.
  const Vec3 dir{1.0f, 0.41421356f, 0.31622777f};
  int crossings = 0;
  for (const auto& tri : faces) {
    float t;
    if (ray_triangle_intersect(p, dir, tri, /*cull_backface=*/false, t)) {
      ++crossings;
    }
  }
  return (crossings & 1) != 0;
}

std::vector<Diagnostic> validate_shoebox(const ShoeboxRoom& room, const SourceListener& placement) {
  std::vector<Diagnostic> diags;
  const bool dims_ok = finite_positive(room.dims.length) && finite_positive(room.dims.width) &&
                       finite_positive(room.dims.height);
  if (!dims_ok) {
    diags.push_back({Diagnostic::Severity::Error, "acoustic.invalid_dimensions",
                     "shoebox dimensions must be finite and positive"});
    return diags;  // further placement checks are meaningless
  }
  if (!point_inside_shoebox(room, placement.source)) {
    diags.push_back({Diagnostic::Severity::Error, "acoustic.source_outside_room",
                     "source position lies outside the room"});
  }
  if (!point_inside_shoebox(room, placement.listener)) {
    diags.push_back({Diagnostic::Severity::Error, "acoustic.listener_outside_room",
                     "listener position lies outside the room"});
  }
  const Vec3 sep = placement.source - placement.listener;
  if (length(sep) < 1e-4f) {
    diags.push_back({Diagnostic::Severity::Warning, "acoustic.source_equals_listener",
                     "source and listener coincide; DRR is undefined"});
  }
  // The image-source method multiplies per-band wall reflection coefficients, so
  // the six walls must share a band count (and have at least one band). Mismatch
  // is recoverable (the synthesizer falls back to the common band count) -> Warning.
  size_t bands = 0;
  bool band_mismatch = false;
  for (const auto& w : room.walls) {
    if (w.absorption.empty()) {
      band_mismatch = true;
      break;
    }
    if (bands == 0) {
      bands = w.absorption.size();
    } else if (w.absorption.size() != bands) {
      band_mismatch = true;
      break;
    }
  }
  if (band_mismatch) {
    diags.push_back({Diagnostic::Severity::Warning, "acoustic.material_band_mismatch",
                     "wall materials have empty or inconsistent octave-band counts"});
  }
  return diags;
}

std::vector<Diagnostic> validate_polyhedral(const PolyhedralRoom& room,
                                            const SourceListener& placement) {
  std::vector<Diagnostic> diags;
  if (room.faces.empty()) {
    diags.push_back(
        {Diagnostic::Severity::Error, "acoustic.empty_mesh", "polyhedral room has no faces"});
    return diags;
  }
  if (room.face_materials.empty()) {
    diags.push_back({Diagnostic::Severity::Error, "acoustic.no_materials",
                     "polyhedral room has no face materials"});
  } else if (room.face_materials.size() != 1 && room.face_materials.size() != room.faces.size()) {
    diags.push_back({Diagnostic::Severity::Error, "acoustic.material_count_mismatch",
                     "face_materials must have size 1 or match the face count"});
  }
  int degenerate = 0;
  for (const auto& tri : room.faces) {
    // |triangle_normal| equals twice the area. Compare it against the triangle's
    // own scale (longest edge squared) so the degeneracy test is dimensionless
    // and works for meshes modelled in any unit, not an absolute area in m^2.
    const float e0 = dot(tri.b - tri.a, tri.b - tri.a);
    const float e1 = dot(tri.c - tri.b, tri.c - tri.b);
    const float e2 = dot(tri.a - tri.c, tri.a - tri.c);
    const float scale = std::max({e0, e1, e2});
    if (length(triangle_normal(tri)) < 1e-6f * std::max(scale, 1e-12f)) ++degenerate;
  }
  if (degenerate > 0) {
    diags.push_back({Diagnostic::Severity::Warning, "acoustic.degenerate_face",
                     "mesh contains degenerate (zero-area) faces"});
  }
  if (!point_inside_mesh(room.faces, placement.source)) {
    diags.push_back({Diagnostic::Severity::Error, "acoustic.source_outside_room",
                     "source position lies outside the mesh"});
  }
  if (!point_inside_mesh(room.faces, placement.listener)) {
    diags.push_back({Diagnostic::Severity::Error, "acoustic.listener_outside_room",
                     "listener position lies outside the mesh"});
  }
  return diags;
}

}  // namespace sonare::acoustic
