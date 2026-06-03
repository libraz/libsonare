#include "mir/grid_snap.h"

#include <algorithm>
#include <cmath>

namespace sonare::mir {
namespace {

// Snaps `ppq` to the nearest multiple of `step` offset by `origin`, blended by
// `strength` in [0, 1]. step <= 0 leaves ppq unchanged.
double snap_to_step(double ppq, double origin, double step, double strength) noexcept {
  if (!(step > 0.0) || !std::isfinite(ppq)) return ppq;
  const double s = std::clamp(strength, 0.0, 1.0);
  const double rel = (ppq - origin) / step;
  const double snapped = origin + std::round(rel) * step;
  return ppq + (snapped - ppq) * s;
}

}  // namespace

SnapGrid make_grid(const transport::TempoMap& tempo_map, double ppq) {
  SnapGrid grid;
  grid.time_sig = tempo_map.time_signature_at_ppq(ppq);
  grid.origin_ppq = tempo_map.bar_start_ppq(ppq);
  return grid;
}

double beat_length_ppq(const SnapGrid& grid) noexcept {
  const int den = std::max(grid.time_sig.denominator, 1);
  // 1 ppq == 1 quarter note; a beat is the denominator note value in quarters.
  return 4.0 / static_cast<double>(den);
}

double bar_length_ppq(const SnapGrid& grid) noexcept {
  const int num = std::max(grid.time_sig.numerator, 1);
  return static_cast<double>(num) * beat_length_ppq(grid);
}

double snap_to_beat(const SnapGrid& grid, double ppq, double strength) noexcept {
  return snap_to_step(ppq, grid.origin_ppq, beat_length_ppq(grid), strength);
}

double snap_to_bar(const SnapGrid& grid, double ppq, double strength) noexcept {
  return snap_to_step(ppq, grid.origin_ppq, bar_length_ppq(grid), strength);
}

double snap_to_subdivision(const SnapGrid& grid, double ppq, int division,
                           double strength) noexcept {
  const int div = division > 0 ? division : 1;
  const double step = beat_length_ppq(grid) / static_cast<double>(div);
  return snap_to_step(ppq, grid.origin_ppq, step, strength);
}

std::vector<double> snap_to_beat(const SnapGrid& grid, const std::vector<double>& ppqs,
                                 double strength) {
  std::vector<double> out;
  out.reserve(ppqs.size());
  for (double p : ppqs) out.push_back(snap_to_beat(grid, p, strength));
  return out;
}

std::vector<double> snap_to_subdivision(const SnapGrid& grid, const std::vector<double>& ppqs,
                                        int division, double strength) {
  std::vector<double> out;
  out.reserve(ppqs.size());
  for (double p : ppqs) out.push_back(snap_to_subdivision(grid, p, division, strength));
  return out;
}

}  // namespace sonare::mir
