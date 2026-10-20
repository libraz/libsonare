#include "acoustic/image_source.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <unordered_map>

#include "acoustic/image_source_test_support.h"
#include "acoustic/late_reverb.h"
#include "util/constants.h"

namespace sonare::acoustic {

namespace {
using sonare::constants::kPi;

std::atomic<test::PolyhedralFrontierObserver> g_polyhedral_frontier_observer{nullptr};

void observe_polyhedral_frontier(std::size_t size) noexcept {
  if (const test::PolyhedralFrontierObserver observer =
          g_polyhedral_frontier_observer.load(std::memory_order_relaxed);
      observer != nullptr) {
    observer(size);
  }
}

// Two images closer than this (Euclidean, m) are the same reflection.
constexpr float kImageCoincidenceTolerance = 1e-4f;

// Spatial hash over the images accepted at one order. Cells are one tolerance wide, so any pair
// within the tolerance sits in the same or an adjacent cell and the 27-cell probe finds it; the
// exact distance test is unchanged.
class ImageCoincidenceIndex {
 public:
  bool contains_near(const std::vector<ImageSource>& images, const Vec3& p) const {
    if (!finite(p)) return false;
    const Cell c = cell_of(p);
    for (double dx = -1.0; dx <= 1.0; dx += 1.0) {
      for (double dy = -1.0; dy <= 1.0; dy += 1.0) {
        for (double dz = -1.0; dz <= 1.0; dz += 1.0) {
          const auto it = cells_.find(Cell{c[0] + dx, c[1] + dy, c[2] + dz});
          if (it == cells_.end()) continue;
          for (const size_t i : it->second) {
            if (length(images[i].position - p) < kImageCoincidenceTolerance) return true;
          }
        }
      }
    }
    return false;
  }

  void add(const Vec3& p, size_t image_index) {
    if (finite(p)) cells_[cell_of(p)].push_back(image_index);
  }

 private:
  using Cell = std::array<double, 3>;
  struct CellHash {
    size_t operator()(const Cell& c) const noexcept {
      size_t h = std::hash<double>{}(c[0]);
      h = h * 1000003u ^ std::hash<double>{}(c[1]);
      return h * 1000003u ^ std::hash<double>{}(c[2]);
    }
  };

  static bool finite(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
  }
  static double cell_coord(float v) {
    return std::floor(static_cast<double>(v) / static_cast<double>(kImageCoincidenceTolerance)) +
           0.0;  // +0.0 folds -0.0 into 0.0
  }
  static Cell cell_of(const Vec3& p) { return {cell_coord(p.x), cell_coord(p.y), cell_coord(p.z)}; }

  std::unordered_map<Cell, std::vector<size_t>, CellHash> cells_;
};

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

void test::set_polyhedral_frontier_observer(PolyhedralFrontierObserver observer) noexcept {
  g_polyhedral_frontier_observer.store(observer, std::memory_order_relaxed);
}

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
  // pow(beta, e) for every wall, band and wall-bounce count e the lattice can reach (|mx - px| <= m
  // + 1).
  const size_t powers = static_cast<size_t>(m) + 2;
  const auto power_table = [&](const std::vector<float>& beta) {
    std::vector<float> table(bands * powers);
    for (size_t b = 0; b < bands; ++b) {
      for (size_t e = 0; e < powers; ++e) {
        table[b * powers + e] = std::pow(beta[b], static_cast<float>(e));
      }
    }
    return table;
  };
  const std::vector<float> px0 = power_table(bx0), px1 = power_table(bx1);
  const std::vector<float> py0 = power_table(by0), py1 = power_table(by1);
  const std::vector<float> pz0 = power_table(bz0), pz1 = power_table(bz1);
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
                const size_t row = b * powers;
                im.reflection[b] =
                    px0[row + static_cast<size_t>(ex0)] * px1[row + static_cast<size_t>(ex1)] *
                    py0[row + static_cast<size_t>(ey0)] * py1[row + static_cast<size_t>(ey1)] *
                    pz0[row + static_cast<size_t>(ez0)] * pz1[row + static_cast<size_t>(ez1)];
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
  std::uint64_t sequence = 0;
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

float pending_energy(const PendingImage& p) {
  float sum = 0.0f;
  for (float b : p.reflection) sum += b;
  return sum;
}

// Ranking entry for the bounded frontier. Lower energy is worse; sequence is
// the deterministic tie-break, so a later child is worse when reflection
// energies are equal and the earlier child remains in the frontier.
struct FrontierEntry {
  float energy = 0.0f;
  std::uint64_t sequence = 0;
  size_t slot = 0;
};

struct FrontierEntryLess {
  bool operator()(const FrontierEntry& a, const FrontierEntry& b) const noexcept {
    // Keep NaN energies at the bottom of the ranking without violating the
    // strict-weak-ordering requirement of std::set.
    const bool a_nan = std::isnan(a.energy);
    const bool b_nan = std::isnan(b.energy);
    if (a_nan != b_nan) return a_nan;
    if (!a_nan && a.energy != b.energy) return a.energy < b.energy;
    if (a.sequence != b.sequence) return a.sequence > b.sequence;
    return a.slot > b.slot;
  }
};

// Append one child, then prune @p v to its @p keep highest-reflection-energy
// candidates (the perceptually dominant paths). A ranking set keeps each
// append O(log keep) instead of scanning or partitioning the whole frontier,
// while the vector and ranking both stay bounded independently of face count.
void prune_frontier_by_energy(std::vector<PendingImage>& v,
                              std::set<FrontierEntry, FrontierEntryLess>& ranking, size_t keep) {
  const size_t slot = v.size() - 1;
  const FrontierEntry inserted{pending_energy(v.back()), v.back().sequence, slot};
  ranking.insert(inserted);
  if (v.size() <= keep) return;

  const FrontierEntry worst = *ranking.begin();
  if (worst.slot == slot) {
    ranking.erase(worst);
    v.pop_back();
    return;
  }

  ranking.erase(worst);
  v[worst.slot] = std::move(v.back());
  v.pop_back();
  ranking.erase(inserted);
  ranking.insert({pending_energy(v[worst.slot]), v[worst.slot].sequence, worst.slot});
}

// Validate a candidate image by walking listener -> reflection points -> source,
// checking each reflection point is inside its face and each segment is
// unoccluded. `partial[k]` is the source reflected across chain[0..k).
bool validate_path(const PolyhedralRoom& room, const std::vector<Plane>& planes,
                   const VoxelGrid& grid, const std::vector<Vec3>& partial,
                   const std::vector<int>& chain, const Vec3& source, const Vec3& listener) {
  Vec3 prev = listener;
  int prev_face = -1;  // the face `prev` lies on (-1 for the listener endpoint)
  for (int k = static_cast<int>(chain.size()) - 1; k >= 0; --k) {
    const int fi = chain[static_cast<size_t>(k)];
    const Triangle& tri = room.faces[static_cast<size_t>(fi)];
    const Plane& plane = planes[static_cast<size_t>(fi)];
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

  // Per-face reflection coefficients depend only on the face material, so
  // precompute them once instead of recomputing per (parent, face) pair.
  std::vector<std::vector<float>> face_betas(room.faces.size());
  for (size_t f = 0; f < room.faces.size(); ++f) {
    face_betas[f] = material_beta(face_material(room, f), bands);
  }

  // Face planes depend only on the face, so build them once rather than per candidate.
  std::vector<Plane> planes;
  planes.reserve(room.faces.size());
  for (const Triangle& tri : room.faces) planes.push_back({tri.a, triangle_normal(tri)});

  // Breadth-first reflection across faces, validating each candidate's full path.
  std::vector<PendingImage> frontier;
  frontier.push_back({source, 0, std::vector<float>(bands, 1.0f), {}});
  // Scratch for the partial-image chain: the parent's prefix is built once per
  // parent and only the last entry differs between that parent's children.
  std::vector<Vec3> partial;

  for (int order = 1; order <= max_order; ++order) {
    // A duplicate can only coincide with an image accepted at this same order.
    ImageCoincidenceIndex accepted_at_order;
    std::vector<PendingImage> next;
    std::set<FrontierEntry, FrontierEntryLess> ranking;
    std::uint64_t next_sequence = 0;
    // Leave room for one child beyond the cap so pruning can happen
    // immediately after every append without allowing vector growth to track
    // the number of faces.
    next.reserve(kMaxPolyhedralFrontier + 1);
    for (const auto& parent : frontier) {
      const size_t prefix = parent.chain.size();
      partial.resize(prefix + 2);
      partial[0] = source;
      for (size_t k = 0; k < prefix; ++k) {
        partial[k + 1] =
            reflect_across_plane(partial[k], planes[static_cast<size_t>(parent.chain[k])]);
      }
      for (size_t f = 0; f < room.faces.size(); ++f) {
        if (!parent.chain.empty() && parent.chain.back() == static_cast<int>(f)) {
          continue;  // reflecting across the same face twice undoes the bounce
        }
        const Vec3 img = reflect_across_plane(parent.pos, planes[f]);

        PendingImage cand;
        cand.pos = img;
        cand.order = order;
        cand.sequence = next_sequence++;
        cand.chain = parent.chain;
        cand.chain.push_back(static_cast<int>(f));
        const std::vector<float>& fb = face_betas[f];
        cand.reflection.resize(bands);
        for (size_t b = 0; b < bands; ++b) cand.reflection[b] = parent.reflection[b] * fb[b];

        partial[prefix + 1] = reflect_across_plane(partial[prefix], planes[f]);
        if (validate_path(room, planes, grid, partial, cand.chain, source, listener)) {
          // A planar wall is usually several coplanar triangles; a specular
          // point on a shared edge passes the inclusive in-triangle test for
          // more than one of them. Drop a candidate whose image coincides with
          // an already-accepted one (same order) to avoid double-counting that
          // reflection's energy.
          if (!accepted_at_order.contains_near(out, img)) {
            ImageSource im;
            im.order = order;
            im.position = img;
            im.distance = length(img - listener);
            im.reflection = cand.reflection;
            accepted_at_order.add(img, out.size());
            out.push_back(im);
          }
        }
        next.push_back(std::move(cand));  // keep exploring even if this order was invalid
        // Prune after every child append so a dense mesh cannot grow the
        // frontier with the number of faces before an end-of-parent pass.
        observe_polyhedral_frontier(next.size());
        prune_frontier_by_energy(next, ranking, kMaxPolyhedralFrontier);
      }
    }
    frontier.swap(next);
  }
  return out;
}

namespace {
// Fractional-delay kernel half-width; the fdl is forced odd so the kernel is symmetric about k=0.
int early_kernel_half_width(const EarlyIrConfig& config) noexcept {
  int fdl = config.fdl < 1 ? 1 : config.fdl;
  if (fdl % 2 == 0) ++fdl;
  return fdl / 2;
}

float early_sound_speed(const EarlyIrConfig& config) noexcept {
  return config.sound_speed > 0.0f ? config.sound_speed : kSoundSpeed;
}
}  // namespace

int early_ir_natural_length(const std::vector<ImageSource>& images, int sample_rate,
                            const EarlyIrConfig& config) {
  const float c = early_sound_speed(config);
  const int half = early_kernel_half_width(config);
  const float sr = static_cast<float>(sample_rate);

  float max_delay = 0.0f;
  for (const auto& im : images) {
    if (im.distance > 1e-6f) {
      const float delay = im.distance / c * sr;
      if (std::isfinite(delay)) max_delay = std::max(max_delay, delay);
    }
  }
  // Auto-size to the farthest image arrival, clamped to the same ceiling
  // synthesize_late_tail uses (kMaxAutoSamples): without a cap, a pathological
  // room -- huge dimensions and/or a high reflection order -- would otherwise
  // size one unbounded allocation, a WASM OOM risk. Computed in double so the
  // sum cannot overflow int before the clamp is applied.
  const double raw = std::ceil(static_cast<double>(max_delay)) + static_cast<double>(half) + 2.0;
  return std::max(1, static_cast<int>(std::min(raw, static_cast<double>(kMaxAutoSamples))));
}

Audio synthesize_early_ir(const std::vector<ImageSource>& images, int sample_rate,
                          const EarlyIrConfig& config) {
  const float c = early_sound_speed(config);
  const int half = early_kernel_half_width(config);
  const float sr = static_cast<float>(sample_rate);

  double auto_length = static_cast<double>(early_ir_natural_length(images, sample_rate, config));
  // max_samples is an UPPER BOUND, not an exact length: a natural IR shorter than
  // the cap must not be zero-padded out to it (that inflates the convolution cost
  // and, at kMaxRirSeconds, is itself an OOM risk). Only truncate when the natural
  // length actually exceeds the cap.
  if (config.max_samples > 0) {
    auto_length = std::min(auto_length, static_cast<double>(config.max_samples));
  }
  int length = static_cast<int>(auto_length);
  if (length < 1) length = 1;
  std::vector<float> ir(static_cast<size_t>(length), 0.0f);
  const std::int64_t half64 = static_cast<std::int64_t>(half);
  const std::int64_t length64 = static_cast<std::int64_t>(length);

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
    // Use the negated >-form so non-finite distances are skipped consistently.
    if (!(im.distance > 1e-6f)) continue;
    const float delay = im.distance / c * sr;
    if (!std::isfinite(delay)) continue;
    const double delay_d = static_cast<double>(delay);
    if (delay_d <= -static_cast<double>(half64) - 0.5 ||
        delay_d >= static_cast<double>(length64 - 1) + static_cast<double>(half64) + 0.5) {
      continue;
    }
    const float gain = band_gain(im) / (4.0f * kPi * im.distance);
    const std::int64_t n0 = std::llround(delay);
    const float frac = delay - static_cast<float>(n0);
    const std::int64_t first_k = std::max(-half64, -n0);
    const std::int64_t last_k = std::min(half64, (length64 - 1) - n0);
    for (std::int64_t k = first_k; k <= last_k; ++k) {
      const std::int64_t idx64 = n0 + k;
      const size_t idx = static_cast<size_t>(idx64);
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
      ir[idx] += gain * h * w;
    }
  }
  return Audio::from_vector(std::move(ir), sample_rate);
}

}  // namespace sonare::acoustic
