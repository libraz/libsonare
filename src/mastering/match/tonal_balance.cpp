#include "mastering/match/tonal_balance.h"

#include <stdexcept>

namespace sonare::mastering::match {
namespace {

float average_band(const ReferenceSpectrum& spectrum, float low_hz, float high_hz) {
  if (spectrum.frequencies.size() != spectrum.db.size() || spectrum.frequencies.empty()) {
    throw std::invalid_argument("invalid spectrum");
  }
  float sum = 0.0f;
  int count = 0;
  for (size_t i = 0; i < spectrum.frequencies.size(); ++i) {
    if (spectrum.frequencies[i] >= low_hz && spectrum.frequencies[i] < high_hz) {
      sum += spectrum.db[i];
      ++count;
    }
  }
  return count == 0 ? -120.0f : sum / static_cast<float>(count);
}

}  // namespace

std::vector<TonalBalanceBand> tonal_balance(const ReferenceSpectrum& source,
                                            const ReferenceSpectrum& reference) {
  if (source.sample_rate != reference.sample_rate) {
    throw std::invalid_argument("sample rates must match");
  }
  const std::vector<TonalBalanceBand> ranges = {
      {20.0f, 250.0f, 0.0f, 0.0f, 0.0f},
      {250.0f, 2000.0f, 0.0f, 0.0f, 0.0f},
      {2000.0f, 8000.0f, 0.0f, 0.0f, 0.0f},
      {8000.0f, 20000.0f, 0.0f, 0.0f, 0.0f},
  };
  std::vector<TonalBalanceBand> result;
  result.reserve(ranges.size());
  for (auto band : ranges) {
    band.source_db = average_band(source, band.low_hz, band.high_hz);
    band.reference_db = average_band(reference, band.low_hz, band.high_hz);
    band.deviation_db = band.source_db - band.reference_db;
    result.push_back(band);
  }
  return result;
}

}  // namespace sonare::mastering::match
