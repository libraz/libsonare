#pragma once

/// @file geometry.h
/// @brief Minimal 3D vector math and ray/triangle/AABB primitives for the
///        room-acoustics module. Zero-dependency, WASM-safe, header-only.

#include <cmath>

namespace sonare::acoustic {

/// @brief 3D point/vector in room space (metres).
struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) noexcept {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(const Vec3& a, const Vec3& b) noexcept {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(const Vec3& a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }

inline float dot(const Vec3& a, const Vec3& b) noexcept {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& a) noexcept { return std::sqrt(dot(a, a)); }

/// @brief A triangle face (CCW winding defines the outward normal).
struct Triangle {
  Vec3 a, b, c;
};

inline Vec3 triangle_normal(const Triangle& t) noexcept { return cross(t.b - t.a, t.c - t.a); }

inline Vec3 normalize(const Vec3& a) noexcept {
  const float len = length(a);
  return len > 0.0f ? a * (1.0f / len) : a;
}

/// @brief Oriented plane: a point on the plane and a (not necessarily unit) normal.
struct Plane {
  Vec3 point{};
  Vec3 normal{};
};

/// @brief Mirror a point across a plane (specular image position).
inline Vec3 reflect_across_plane(const Vec3& p, const Plane& plane) noexcept {
  const Vec3 n = normalize(plane.normal);
  const float d = dot(p - plane.point, n);
  return p - n * (2.0f * d);
}

/// @brief Axis-aligned bounding box.
struct Aabb {
  Vec3 min{};
  Vec3 max{};
};

inline bool aabb_contains(const Aabb& box, const Vec3& p) noexcept {
  return p.x >= box.min.x && p.x <= box.max.x && p.y >= box.min.y && p.y <= box.max.y &&
         p.z >= box.min.z && p.z <= box.max.z;
}

/// @brief True if @p p (assumed on the triangle's plane) lies within @p tri.
///
/// Barycentric test with a small inclusive tolerance so points on an edge count
/// as inside (used by the image-source reflection-point validity check).
inline bool point_in_triangle(const Vec3& p, const Triangle& tri, float eps = 1e-4f) noexcept {
  const Vec3 v0 = tri.b - tri.a;
  const Vec3 v1 = tri.c - tri.a;
  const Vec3 v2 = p - tri.a;
  const float d00 = dot(v0, v0);
  const float d01 = dot(v0, v1);
  const float d11 = dot(v1, v1);
  const float d20 = dot(v2, v0);
  const float d21 = dot(v2, v1);
  const float denom = d00 * d11 - d01 * d01;
  if (std::fabs(denom) < 1e-20f) return false;
  const float v = (d11 * d20 - d01 * d21) / denom;
  const float w = (d00 * d21 - d01 * d20) / denom;
  const float u = 1.0f - v - w;
  return u >= -eps && v >= -eps && w >= -eps;
}

/// @brief Möller–Trumbore ray/triangle intersection.
///
/// Returns true and sets @p out_t to the (non-negative) ray parameter of the
/// hit when the ray `origin + t*dir` (t >= 0) crosses the triangle. @p dir need
/// not be normalised; @p out_t is then in units of `dir`. @p cull_backface, when
/// true, rejects hits on the back side (used for visibility tests).
///
/// The internal epsilon that rejects degenerate determinants and behind-origin
/// hits is expressed in units of `dir`, so callers should pass a direction of
/// order-1 magnitude (or normalise it) to keep the tolerance physically
/// meaningful for room-scale geometry.
inline bool ray_triangle_intersect(const Vec3& origin, const Vec3& dir, const Triangle& tri,
                                   bool cull_backface, float& out_t) noexcept {
  constexpr float kEps = 1e-7f;
  const Vec3 edge1 = tri.b - tri.a;
  const Vec3 edge2 = tri.c - tri.a;
  const Vec3 pvec = cross(dir, edge2);
  const float det = dot(edge1, pvec);
  if (cull_backface) {
    if (det < kEps) return false;
  } else if (std::fabs(det) < kEps) {
    return false;
  }
  const float inv_det = 1.0f / det;
  const Vec3 tvec = origin - tri.a;
  const float u = dot(tvec, pvec) * inv_det;
  if (u < 0.0f || u > 1.0f) return false;
  const Vec3 qvec = cross(tvec, edge1);
  const float v = dot(dir, qvec) * inv_det;
  if (v < 0.0f || u + v > 1.0f) return false;
  const float t = dot(edge2, qvec) * inv_det;
  if (t < kEps) return false;
  out_t = t;
  return true;
}

}  // namespace sonare::acoustic
