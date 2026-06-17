#include "acoustic/image_source.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "util/constants.h"

namespace sonare::acoustic {

namespace {
using sonare::constants::kPi;

float beta_from_alpha(float alpha) noexcept { return std::sqrt(std::max(0.0f, 1.0f - alpha)); }

// Per-band β = sqrt(1 - α) for a material over `bands` bands, padding past the
// material's length with its last coefficient via the shared `material_alpha_at`
// policy (an empty/rigid material reads α=0 -> β=1). Note: only absorption
// reduces the specular reflection product; Material::scattering is intentionally
// NOT applied here. The scattered fraction is diffuse energy that the
// deterministic image-source method does not model; it is delegated to the
// statistical late-reverberation tail (which biases its early/late split by the
// room's mean scattering). This is a deliberate modeling choice, not an oversight.
std::vector<float> material_beta(const Material& m, size_t bands) {
  std::vector<float> b(bands, 1.0f);
  for (size_t i = 0; i < bands; ++i) {
    b[i] = beta_from_alpha(material_alpha_at(m, i));
  }
  return b;
}

// Octave-band count shared with the late-tail RT60 path: the MAXIMUM non-empty
// material band count, with repeat-last padding (see `reconcile_band_count`).
size_t common_bands_shoebox(const ShoeboxRoom& room) { return reconcile_band_count(room.walls); }

size_t common_bands_mesh(const PolyhedralRoom& room) {
  return reconcile_band_count(room.face_materials);
}

// Intersection parameter d of segment A->B with a plane: P = A + d*(B-A).
// Returns false when the segment is parallel to the plane.
bool segment_plane(const Vec3& a, const Vec3& b, const Plane& plane, float& out_d) noexcept {
  const Vec3 n = plane.normal;
  const float denom = dot(b - a, n);
  if (std::fabs(denom) < 1e-12f) return false;
  out_d = dot(plane.point - a, n) / denom;
  return true;
}
}  // namespace

std::vector<ImageSource> shoebox_image_sources(const ShoeboxRoom& room,
                                               const SourceListener& placement, int max_order) {
  std::vector<ImageSource> out;
  if (max_order < 0) return out;
  // Bound the lattice so a hostile/typo'd order cannot exhaust memory (order^3).
  max_order = std::min(max_order, kMaxImageSourceOrder);

  const float lx = room.dims.length, ly = room.dims.width, lz = room.dims.height;
  const Vec3 s = placement.source;
  const Vec3 listener = placement.listener;
  const size_t bands = common_bands_shoebox(room);

  const std::vector<float> bx0 = material_beta(room.walls[kWallXMin], bands);
  const std::vector<float> bx1 = material_beta(room.walls[kWallXMax], bands);
  const std::vector<float> by0 = material_beta(room.walls[kWallYMin], bands);
  const std::vector<float> by1 = material_beta(room.walls[kWallYMax], bands);
  const std::vector<float> bz0 = material_beta(room.walls[kWallZMin], bands);
  const std::vector<float> bz1 = material_beta(room.walls[kWallZMax], bands);

  const int m = max_order;  // generous lattice bound; filtered by total order
  for (int px = 0; px <= 1; ++px) {
    for (int mx = -m; mx <= m; ++mx) {
      const int ox = std::abs(2 * mx - px);
      if (ox > max_order) continue;
      for (int py = 0; py <= 1; ++py) {
        for (int my = -m; my <= m; ++my) {
          const int oy = std::abs(2 * my - py);
          if (ox + oy > max_order) continue;
          for (int pz = 0; pz <= 1; ++pz) {
            for (int mz = -m; mz <= m; ++mz) {
              const int oz = std::abs(2 * mz - pz);
              const int order = ox + oy + oz;
              if (order > max_order) continue;

              ImageSource im;
              im.order = order;
              im.position = {(1.0f - 2.0f * px) * s.x + 2.0f * mx * lx,
                             (1.0f - 2.0f * py) * s.y + 2.0f * my * ly,
                             (1.0f - 2.0f * pz) * s.z + 2.0f * mz * lz};
              im.distance = length(im.position - listener);
              im.reflection.resize(bands);
              const int ex0 = std::abs(mx - px), ex1 = std::abs(mx);
              const int ey0 = std::abs(my - py), ey1 = std::abs(my);
              const int ez0 = std::abs(mz - pz), ez1 = std::abs(mz);
              for (size_t b = 0; b < bands; ++b) {
                im.reflection[b] = std::pow(bx0[b], static_cast<float>(ex0)) *
                                   std::pow(bx1[b], static_cast<float>(ex1)) *
                                   std::pow(by0[b], static_cast<float>(ey0)) *
                                   std::pow(by1[b], static_cast<float>(ey1)) *
                                   std::pow(bz0[b], static_cast<float>(ez0)) *
                                   std::pow(bz1[b], static_cast<float>(ez1));
              }
              out.push_back(std::move(im));
            }
          }
        }
      }
    }
  }
  return out;
}

namespace {
// A candidate Borish image with the chain of faces it bounced off (in
// source->listener reflection order).
struct PendingImage {
  Vec3 pos;
  int order = 0;
  std::vector<float> reflection;
  std::vector<int> chain;
};

// True if the open segment A->B is blocked by some mesh face strictly before B.
// @p ignore_face_b is the face B lies on (the reflection/destination face) and
// @p ignore_face_a the face A lies on (the origin reflection face); BOTH are
// excluded so neither endpoint's own face (nor an adjacent coplanar triangle of
// the same wall sharing the endpoint edge) counts as a spurious occluder.
// Standard ISM occlusion excludes both endpoint faces of a reflection leg.
bool occluded(const VoxelGrid& grid, const Vec3& a, const Vec3& b, int ignore_face_b = -1,
              int ignore_face_a = -1) {
  const Vec3 dir = b - a;
  const MeshHit hit = grid.first_hit(a, dir);  // dir spans A->B, so t in [0,1]
  if (!hit.hit) return false;
  if (hit.face == ignore_face_b || hit.face == ignore_face_a) return false;
  return hit.t < 1.0f - 1e-4f;
}

// Validate a candidate image by walking listener -> reflection points -> source,
// checking each reflection point is inside its face and each segment is
// unoccluded. `partial[k]` is the source reflected across chain[0..k).
bool validate_path(const PolyhedralRoom& room, const VoxelGrid& grid,
                   const std::vector<Vec3>& partial, const std::vector<int>& chain,
                   const Vec3& source, const Vec3& listener) {
  Vec3 prev = listener;
  int prev_face = -1;  // the face `prev` lies on (-1 for the listener endpoint)
  for (int k = static_cast<int>(chain.size()) - 1; k >= 0; --k) {
    const int fi = chain[static_cast<size_t>(k)];
    const Triangle& tri = room.faces[static_cast<size_t>(fi)];
    const Plane plane{tri.a, triangle_normal(tri)};
    const Vec3 img = partial[static_cast<size_t>(k) + 1];
    float d;
    if (!segment_plane(prev, img, plane, d) || d <= 1e-4f || d >= 1.0f - 1e-4f) {
      return false;  // reflection point not strictly between the two endpoints
    }
    const Vec3 p = prev + (img - prev) * d;
    if (!point_in_triangle(p, tri)) return false;
    // Ignore both the destination face (fi, where p lies) and the origin face
    // (prev_face, where prev lies) so neither endpoint's wall self-occludes.
    if (occluded(grid, prev, p, fi, prev_face)) return false;
    prev = p;
    prev_face = fi;
  }
  // Last leg: first reflection point -> source. `prev` lies on chain[0]'s face;
  // exclude it as the origin so the reflecting wall does not self-occlude.
  return !occluded(grid, prev, source, -1, prev_face);
}
}  // namespace

std::vector<ImageSource> polyhedral_image_sources(const PolyhedralRoom& room,
                                                  const SourceListener& placement, int max_order) {
  std::vector<ImageSource> out;
  if (max_order < 0 || room.faces.empty()) return out;
  // Bound the breadth-first expansion so a hostile/typo'd order cannot exhaust
  // memory (cost ~ faces^order).
  max_order = std::min(max_order, kMaxImageSourceOrder);

  VoxelGrid grid;
  grid.build(room.faces);
  const size_t bands = common_bands_mesh(room);
  const Vec3 source = placement.source;
  const Vec3 listener = placement.listener;

  // Order 0: direct path, valid when mutually visible.
  if (!occluded(grid, source, listener)) {
    ImageSource direct;
    direct.order = 0;
    direct.position = source;
    direct.distance = length(source - listener);
    direct.reflection.assign(bands, 1.0f);
    out.push_back(direct);
  }

  // Breadth-first reflection across faces, validating each candidate's full path.
  std::vector<PendingImage> frontier;
  frontier.push_back({source, 0, std::vector<float>(bands, 1.0f), {}});

  for (int order = 1; order <= max_order; ++order) {
    std::vector<PendingImage> next;
    for (const auto& parent : frontier) {
      for (size_t f = 0; f < room.faces.size(); ++f) {
        if (!parent.chain.empty() && parent.chain.back() == static_cast<int>(f)) {
          continue;  // reflecting across the same face twice undoes the bounce
        }
        const Triangle& tri = room.faces[f];
        const Plane plane{tri.a, triangle_normal(tri)};
        const Vec3 img = reflect_across_plane(parent.pos, plane);

        PendingImage cand;
        cand.pos = img;
        cand.order = order;
        cand.chain = parent.chain;
        cand.chain.push_back(static_cast<int>(f));
        const std::vector<float> fb = material_beta(face_material(room, f), bands);
        cand.reflection.resize(bands);
        for (size_t b = 0; b < bands; ++b) cand.reflection[b] = parent.reflection[b] * fb[b];

        // Reconstruct the partial-image chain for validation.
        std::vector<Vec3> partial(cand.chain.size() + 1);
        partial[0] = source;
        for (size_t k = 0; k < cand.chain.size(); ++k) {
          const Triangle& t2 = room.faces[static_cast<size_t>(cand.chain[k])];
          partial[k + 1] = reflect_across_plane(partial[k], {t2.a, triangle_normal(t2)});
        }
        if (validate_path(room, grid, partial, cand.chain, source, listener)) {
          // A planar wall is usually several coplanar triangles; a specular
          // point on a shared edge passes the inclusive in-triangle test for
          // more than one of them. Drop a candidate whose image coincides with
          // an already-accepted one (same order) to avoid double-counting that
          // reflection's energy.
          const bool duplicate = std::any_of(out.begin(), out.end(), [&](const ImageSource& e) {
            return e.order == order && length(e.position - img) < 1e-4f;
          });
          if (!duplicate) {
            ImageSource im;
            im.order = order;
            im.position = img;
            im.distance = length(img - listener);
            im.reflection = cand.reflection;
            out.push_back(im);
          }
        }
        next.push_back(std::move(cand));  // keep exploring even if this order was invalid
      }
    }
    frontier.swap(next);
  }
  return out;
}

Audio synthesize_early_ir(const std::vector<ImageSource>& images, int sample_rate,
                          const EarlyIrConfig& config) {
  const float c = config.sound_speed > 0.0f ? config.sound_speed : kSoundSpeed;
  int fdl = config.fdl < 1 ? 1 : config.fdl;
  if (fdl % 2 == 0) ++fdl;  // force odd so the kernel is symmetric about k=0
  const int half = fdl / 2;
  const float sr = static_cast<float>(sample_rate);

  float max_delay = 0.0f;
  for (const auto& im : images) {
    if (im.distance > 1e-6f) max_delay = std::max(max_delay, im.distance / c * sr);
  }
  int length = config.max_samples > 0 ? config.max_samples
                                      : static_cast<int>(std::ceil(max_delay)) + half + 2;
  if (length < 1) length = 1;
  std::vector<float> ir(static_cast<size_t>(length), 0.0f);

  auto band_gain = [&](const ImageSource& im) -> float {
    if (im.reflection.empty()) return 1.0f;
    if (config.band >= 0 && config.band < static_cast<int>(im.reflection.size())) {
      return im.reflection[static_cast<size_t>(config.band)];
    }
    // Broadband fallback: RMS across bands, the energy-correct collapse of
    // per-band amplitude reflection coefficients (an arithmetic mean would
    // overestimate strongly absorbing rooms by up to ~1 dB).
    float sum_sq = 0.0f;
    for (float v : im.reflection) sum_sq += v * v;
    return std::sqrt(sum_sq / static_cast<float>(im.reflection.size()));
  };

  for (const auto& im : images) {
    // Use the negated >-form so a non-finite (NaN) distance is also skipped,
    // matching the max-delay scan above; NaN < 1e-6f would be false and would
    // otherwise render a NaN tap into the IR.
    if (!(im.distance > 1e-6f)) continue;
    const float delay = im.distance / c * sr;
    const float gain = band_gain(im) / (4.0f * kPi * im.distance);
    const int n0 = static_cast<int>(std::lround(delay));
    const float frac = delay - static_cast<float>(n0);
    for (int k = -half; k <= half; ++k) {
      const int idx = n0 + k;
      if (idx < 0 || idx >= length) continue;
      const float x = static_cast<float>(k) - frac;  // continuous offset from the true delay
      float h;
      if (std::fabs(x) < 1e-7f) {
        h = 1.0f;
      } else {
        const float px = kPi * x;
        h = std::sin(px) / px;
      }
      // Lanczos window on the *continuous* offset x (not the integer tap k), so
      // the window is centred on the sinc peak and the rendered amplitude is
      // independent of the fractional delay. (fdl=1 -> half=0 -> passthrough.)
      float w = 1.0f;
      if (half > 0) {
        const float xn = x / static_cast<float>(half + 1);
        if (std::fabs(xn) >= 1.0f) {
          w = 0.0f;
        } else if (std::fabs(xn) > 1e-7f) {
          const float pxn = kPi * xn;
          w = std::sin(pxn) / pxn;
        }
      }
      ir[static_cast<size_t>(idx)] += gain * h * w;
    }
  }
  return Audio::from_vector(std::move(ir), sample_rate);
}

}  // namespace sonare::acoustic
