#pragma once

/// @file room_model.h
/// @brief 3D room geometry (shoebox and polyhedral mesh) with per-surface
///        materials, source/listener placement, a voxel acceleration grid for
///        ray/mesh queries, and geometry validation (returns diagnostics, never
///        throws).

#include <array>
#include <vector>

#include "acoustic/geometry.h"
#include "acoustic/material.h"
#include "acoustic/room_types.h"
#include "core/diagnostic.h"

namespace sonare::acoustic {

/// @brief Source and listener positions in room space (metres).
struct SourceListener {
  Vec3 source;
  Vec3 listener;
};

/// @brief Index of a shoebox wall in `ShoeboxRoom::walls`.
enum ShoeboxWall {
  kWallXMin = 0,  ///< x = 0 plane
  kWallXMax = 1,  ///< x = length plane
  kWallYMin = 2,  ///< y = 0 plane
  kWallYMax = 3,  ///< y = width plane
  kWallZMin = 4,  ///< z = 0 plane (floor)
  kWallZMax = 5,  ///< z = height plane (ceiling)
  kShoeboxWallCount = 6,
};

/// @brief Axis-aligned rectangular room with one material per wall.
///
/// Room occupies [0,length] x [0,width] x [0,height]. Walls are indexed by
/// `ShoeboxWall`. This is the primary geometry for the image-source method,
/// where it maps directly onto Allen–Berkley analytic mirror images.
struct ShoeboxRoom {
  RoomDimensions dims;
  std::array<Material, kShoeboxWallCount> walls;
};

/// @brief Build a shoebox with one uniform material on every wall.
///
/// `absorption` is clamped to [0, 0.999] (a perfectly rigid wall has an
/// unbounded RT60). Shared by the room-reverb engine, the room morph, the C ABI,
/// and the CLI so every entry point clamps and assigns walls identically.
ShoeboxRoom uniform_shoebox(const RoomDimensions& dims, float absorption, float scattering = 0.0f);

/// @brief One caller's wall-material request, in the precedence every public
///        surface documents: preset > per-band absorption > scalar absorption.
///
/// This exists so the four facades that build a uniform room from host options
/// (the C ABI, the Node addon, the WASM wrappers and the mastering insert
/// factory) resolve that precedence, validate the coefficients and apply the
/// scattering bands in ONE place. Each of them used to open-code it, and every
/// copy dropped `scattering_bands` on the two paths that did not also carry
/// `absorption_bands`.
struct WallMaterialRequest {
  /// Named preset. When set it wins over both absorption forms; the scattering
  /// bands below still apply on top of it.
  bool has_preset = false;
  MaterialPreset preset = MaterialPreset::Concrete;
  /// Per-octave-band absorption. Non-empty overrides `absorption`.
  std::vector<float> absorption_bands;
  /// Per-octave-band wall scattering. Applied on ALL THREE material paths, not
  /// only alongside `absorption_bands`: a caller's array is either applied or
  /// the request is rejected, never silently dropped. Bands past its length
  /// read as 0, the documented padding rule.
  std::vector<float> scattering_bands;
  /// Uniform scalar absorption, used when neither form above is supplied.
  float absorption = 0.2f;
};

/// @brief Rejects a caller-supplied absorption/scattering coefficient that is
///        not a finite value in [0, 1].
/// @param field Option name quoted in the error message.
/// @throws SonareException(InvalidParameter)
/// @details Facades reject rather than clamp so the same mistake surfaces the
///          same error everywhere. `uniform_shoebox` still clamps, because its
///          clamp bounds an already-validated value away from the rigid-wall
///          singularity rather than papering over a caller's typo.
void validate_material_coefficient(float value, const char* field);

/// @brief `validate_material_coefficient` over an array, plus the band-count cap.
/// @throws SonareException(InvalidParameter)
void validate_material_bands(const std::vector<float>& values, const char* field);

/// @brief Build a uniform shoebox from a validated material request.
///
/// Validates every coefficient the caller supplied (rejecting, not clamping),
/// resolves the documented precedence and returns a room whose six walls carry
/// the requested absorption AND the requested scattering.
/// @throws SonareException(InvalidParameter) on any out-of-range coefficient or
///         an over-long band array.
ShoeboxRoom make_uniform_room(const RoomDimensions& dims, const WallMaterialRequest& request);

/// @brief Interior volume (m^3) of a shoebox room.
float shoebox_volume(const ShoeboxRoom& room) noexcept;

/// @brief Total interior surface area (m^2) of a shoebox room.
float shoebox_surface_area(const ShoeboxRoom& room) noexcept;

/// @brief Area-weighted mean scattering coefficient of a shoebox's walls in
///        [0,1] (0 for fully specular/empty materials).
///
/// Each wall's scattering is collapsed to the mean over its octave bands, then
/// area-weighted across the six walls. The RIR synthesizer reads this to bias
/// the early/late split and mixing time: rougher (higher-scattering) surfaces
/// diffuse specular energy into the late field sooner. This is a coarse,
/// bounded, monotonic use of the material scattering term — not a ray-traced
/// late tail.
float shoebox_mean_scattering(const ShoeboxRoom& room) noexcept;

/// @brief Inward-facing plane of a shoebox wall (a point on the wall + the
///        unit normal pointing into the room). Used by the image-source method.
Plane wall_plane(const ShoeboxRoom& room, ShoeboxWall wall) noexcept;

/// @brief Triangle-mesh room with a material per face (Borish image-source target).
///
/// This is currently a C++ core/internal modelling type. The stable public RIR
/// surfaces expose shoebox rooms only; hosts that need mesh rooms should treat
/// this as a roadmap hook until a versioned C ABI and binding mirror exist.
///
/// `face_materials` is parallel to `faces`; a single-element vector applies one
/// material to every face. Faces should wind so normals point into the room.
struct PolyhedralRoom {
  std::vector<Triangle> faces;
  std::vector<Material> face_materials;
};

/// @brief Material for face @p index, honouring the single-material shorthand.
const Material& face_material(const PolyhedralRoom& room, size_t index);

/// @brief Result of a ray/mesh query: the nearest forward intersection.
///
/// `t` and `face` are only meaningful when `hit == true`; on a miss they are 0
/// and -1 respectively (a miss is not distinguishable from a hit by reading `t`
/// alone — always check `hit` first).
struct MeshHit {
  bool hit = false;
  float t = 0.0f;  ///< ray parameter (distance along @p dir) of the hit
  int face = -1;   ///< index into the mesh face list, or -1
};

/// @brief Brute-force nearest forward hit over all faces (reference oracle).
MeshHit mesh_first_hit_bruteforce(const std::vector<Triangle>& faces, const Vec3& origin,
                                  const Vec3& dir, bool cull_backface = false) noexcept;

/// @brief Uniform voxel grid over a triangle mesh accelerating ray queries.
///
/// `first_hit` returns the same nearest-forward hit as
/// `mesh_first_hit_bruteforce` but visits only the faces binned into the cells
/// the ray traverses (Amanatides–Woo DDA).
class VoxelGrid {
 public:
  /// Build the grid over @p faces with roughly @p resolution cells per axis
  /// (clamped to >= 1). Safe to call repeatedly. No-op for an empty mesh.
  ///
  /// The grid stores a non-owning reference to @p faces: the caller must keep
  /// the mesh alive for as long as the grid (and any `first_hit` query) is used.
  void build(const std::vector<Triangle>& faces, int resolution = 16);

  MeshHit first_hit(const Vec3& origin, const Vec3& dir, bool cull_backface = false) const noexcept;

  bool empty() const noexcept { return faces_ == nullptr || faces_->empty(); }

 private:
  int cell_index(int ix, int iy, int iz) const noexcept { return (iz * ny_ + iy) * nx_ + ix; }

  const std::vector<Triangle>* faces_ = nullptr;
  Aabb bounds_{};
  int nx_ = 1, ny_ = 1, nz_ = 1;
  Vec3 cell_size_{};
  std::vector<std::vector<int>> cells_;  // face indices per cell
};

/// @brief True if @p p lies within the (closed) shoebox interior.
bool point_inside_shoebox(const ShoeboxRoom& room, const Vec3& p) noexcept;

/// @brief True if @p p lies inside or on the boundary of a closed mesh.
///
/// Boundary-inclusive (matching `point_inside_shoebox`): a point lying exactly
/// on a face is reported as inside. Interior points use a parity ray-cast (odd
/// crossing count) with a fixed asymmetric direction chosen to avoid grazing
/// axis-aligned shared edges. Assumes a watertight (closed) mesh; an open mesh
/// or a mesh whose faces align with that direction may misclassify interior
/// boundary cases.
bool point_inside_mesh(const std::vector<Triangle>& faces, const Vec3& p) noexcept;

/// @brief Validate shoebox geometry and placement; never throws.
///
/// Reports invalid/degenerate dimensions and out-of-room source/listener as
/// `Diagnostic` (Error/Warning). An empty result (no errors) means the room is
/// usable by the image-source method.
std::vector<Diagnostic> validate_shoebox(const ShoeboxRoom& room, const SourceListener& placement);

/// @brief Hard upper bound on the polyhedral mesh face count.
///
/// The image-source cost (and the acceleration grid / per-face precompute)
/// scales with the face count, so an absurd mesh is a memory/CPU exhaustion
/// vector. A watertight room mesh needs orders of magnitude fewer faces than
/// this; `validate_polyhedral` rejects anything above it.
inline constexpr size_t kMaxPolyhedralFaces = 100000;

/// @brief Validate polyhedral geometry and placement; never throws.
std::vector<Diagnostic> validate_polyhedral(const PolyhedralRoom& room,
                                            const SourceListener& placement);

}  // namespace sonare::acoustic
