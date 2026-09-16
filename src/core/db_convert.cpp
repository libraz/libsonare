/// @file db_convert.cpp
/// @brief Implementation of dB / amplitude / power conversions.

#include "core/db_convert.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare {

namespace {

float resolve_ref(const float* S, std::size_t n, float ref) {
  if (ref > 0.0f) return ref;
  // Use max(|S|) as reference (librosa: ref=np.max behavior).
  return peak_abs(S, n);
}

}  // namespace

std::vector<float> power_to_db(const float* S, std::size_t n, float ref, float amin, float top_db) {
  SONARE_CHECK_MSG(std::isfinite(ref), ErrorCode::InvalidParameter,
                   "power_to_db: ref must be finite, got " + util::to_text(ref));
  SONARE_CHECK_MSG(std::isfinite(amin), ErrorCode::InvalidParameter,
                   "power_to_db: amin must be finite, got " + util::to_text(amin));
  SONARE_CHECK_MSG(std::isfinite(top_db), ErrorCode::InvalidParameter,
                   "power_to_db: top_db must be finite, got " + util::to_text(top_db));
  // amin is finite here, so `amin > 0` and `!(amin <= 0)` agree.
  SONARE_CHECK_MSG(amin > 0.0f, ErrorCode::InvalidParameter,
                   "power_to_db: amin must be > 0, got " + util::to_text(amin));
  SONARE_CHECK_MSG(n == 0 || S != nullptr, ErrorCode::InvalidParameter,
                   "power_to_db: S must not be null when n > 0");
  std::vector<float> out(n);
  if (n == 0) return out;

  const float resolved_ref = resolve_ref(S, n, ref);
  const float ref_floor = std::max(amin, std::abs(resolved_ref));
  const float ref_db = power_to_db_scalar(ref_floor);

  float max_db = -std::numeric_limits<float>::infinity();
  for (std::size_t i = 0; i < n; ++i) {
    // S[i] first on purpose: std::max returns its first argument for a NaN second
    // one, so amin first would floor an unmeasurable bin to a finite dB nothing
    // downstream can tell from silence. numpy's np.maximum propagates likewise.
    const float val = std::max(S[i], amin);
    const float db = power_to_db_scalar(val) - ref_db;
    out[i] = db;
    if (db > max_db) max_db = db;
  }

  if (top_db >= 0.0f && std::isfinite(max_db)) {
    const float floor_db = max_db - top_db;
    for (std::size_t i = 0; i < n; ++i) {
      if (out[i] < floor_db) out[i] = floor_db;
    }
  }
  return out;
}

std::vector<float> power_to_db(const std::vector<float>& S, float ref, float amin, float top_db) {
  return power_to_db(S.data(), S.size(), ref, amin, top_db);
}

std::vector<float> amplitude_to_db(const float* S, std::size_t n, float ref, float amin,
                                   float top_db) {
  SONARE_CHECK_MSG(std::isfinite(ref), ErrorCode::InvalidParameter,
                   "amplitude_to_db: ref must be finite, got " + util::to_text(ref));
  SONARE_CHECK_MSG(std::isfinite(amin), ErrorCode::InvalidParameter,
                   "amplitude_to_db: amin must be finite, got " + util::to_text(amin));
  SONARE_CHECK_MSG(std::isfinite(top_db), ErrorCode::InvalidParameter,
                   "amplitude_to_db: top_db must be finite, got " + util::to_text(top_db));
  // amin is finite here, so `amin > 0` and `!(amin <= 0)` agree.
  SONARE_CHECK_MSG(amin > 0.0f, ErrorCode::InvalidParameter,
                   "amplitude_to_db: amin must be > 0, got " + util::to_text(amin));
  SONARE_CHECK_MSG(n == 0 || S != nullptr, ErrorCode::InvalidParameter,
                   "amplitude_to_db: S must not be null when n > 0");
  // Mirror librosa: amplitude_to_db(S) == power_to_db(S^2, amin=amin^2, ref=ref^2).
  std::vector<float> power(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float v = S[i];
    power[i] = v * v;
  }
  const float ref_power = ref > 0.0f ? ref * ref : ref;  // negative => sentinel
  const float amin_power = amin * amin;
  return power_to_db(power.data(), n, ref_power, amin_power, top_db);
}

std::vector<float> amplitude_to_db(const std::vector<float>& S, float ref, float amin,
                                   float top_db) {
  return amplitude_to_db(S.data(), S.size(), ref, amin, top_db);
}

std::vector<float> db_to_power(const float* S_db, std::size_t n, float ref) {
  SONARE_CHECK_MSG(std::isfinite(ref), ErrorCode::InvalidParameter,
                   "db_to_power: ref must be finite, got " + util::to_text(ref));
  SONARE_CHECK_MSG(n == 0 || S_db != nullptr, ErrorCode::InvalidParameter,
                   "db_to_power: S_db must not be null when n > 0");
  std::vector<float> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = ref * db_to_power_scalar(S_db[i]);
  }
  return out;
}

std::vector<float> db_to_power(const std::vector<float>& S_db, float ref) {
  return db_to_power(S_db.data(), S_db.size(), ref);
}

std::vector<float> db_to_amplitude(const float* S_db, std::size_t n, float ref) {
  SONARE_CHECK_MSG(std::isfinite(ref), ErrorCode::InvalidParameter,
                   "db_to_amplitude: ref must be finite, got " + util::to_text(ref));
  SONARE_CHECK_MSG(n == 0 || S_db != nullptr, ErrorCode::InvalidParameter,
                   "db_to_amplitude: S_db must not be null when n > 0");
  std::vector<float> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = ref * db_to_linear(S_db[i]);
  }
  return out;
}

std::vector<float> db_to_amplitude(const std::vector<float>& S_db, float ref) {
  return db_to_amplitude(S_db.data(), S_db.size(), ref);
}

}  // namespace sonare
