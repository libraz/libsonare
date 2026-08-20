/// @file polyphase_fir_identity_test.cpp
/// @brief Bit-exactness guard for the polyphase interpolation inner loop.
///
/// interpolate_polyphase_sample() feeds every true-peak reading the library
/// publishes, so its arithmetic is a compatibility surface: a change that shifts
/// the result by one ULP moves published dBTP values and any ceiling decision
/// made from them. The reference below is the straightforward centered
/// convolution the fast path is derived from -- per-tap range test, per-tap row
/// lookup, ascending tap order, double accumulation. The fast path may only
/// reorganize storage and hoist loop-invariant work; it may not change which
/// taps are summed or the order they are summed in, and this test enforces that
/// as bit equality rather than a tolerance.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "rt/polyphase_fir.h"
#include "rt/true_peak_filter.h"
#include "rt/true_peak_fir.h"
#include "util/constants.h"

using sonare::constants::kTwoPi;
using sonare::rt::PolyphaseFir;
using sonare::rt::TruePeakFilter;

namespace {

/// Row-per-phase mirror of a FIR, as the coefficients were stored before the
/// tap block was flattened. The reference interpolation reads this so it cannot
/// accidentally share the fast path's indexing.
std::vector<std::vector<float>> reference_rows(const PolyphaseFir& fir) {
  std::vector<std::vector<float>> rows(static_cast<size_t>(std::max(0, fir.phases)));
  for (int phase = 0; phase < fir.phases; ++phase) {
    auto& row = rows[static_cast<size_t>(phase)];
    row.assign(static_cast<size_t>(std::max(0, fir.taps_per_phase)), 0.0f);
    for (int tap = 0; tap < fir.taps_per_phase; ++tap) {
      // The unity (1x) design declares a shape but carries no coefficients --
      // interpolation short-circuits before reading them -- so tolerate a tap
      // block shorter than phases * taps_per_phase here.
      const size_t flat = static_cast<size_t>(phase) * static_cast<size_t>(fir.taps_per_phase) +
                          static_cast<size_t>(tap);
      row[static_cast<size_t>(tap)] = flat < fir.taps.size() ? fir.taps[flat] : 0.0f;
    }
  }
  return rows;
}

/// The unoptimized centered convolution the fast path must reproduce exactly.
float reference_interpolate(const float* data, size_t length, size_t index, int phase,
                            const std::vector<std::vector<float>>& rows, int taps_per_phase,
                            int phases) {
  if (data == nullptr || length == 0 || index >= length || phase < 0 || phase >= phases ||
      taps_per_phase <= 0) {
    return 0.0f;
  }
  if (phases == 1) {
    return data[index];
  }
  const auto& h = rows[static_cast<size_t>(phase)];
  const int half = taps_per_phase / 2;
  double accum = 0.0;
  for (int tap = 0; tap < taps_per_phase; ++tap) {
    const long src = static_cast<long>(index) - static_cast<long>(tap) + static_cast<long>(half);
    if (src < 0 || src >= static_cast<long>(length)) {
      continue;
    }
    accum += static_cast<double>(h[static_cast<size_t>(tap)]) *
             static_cast<double>(data[static_cast<size_t>(src)]);
  }
  return static_cast<float>(accum);
}

/// Bit equality, not numeric equality: `==` would accept -0.0f for +0.0f and
/// reject NaN for NaN, neither of which is the property under test.
bool bit_equal(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

struct Signal {
  std::string name;
  std::vector<float> samples;
};

/// Signal families that exercise the arithmetic differently: an impulse isolates
/// single taps (and every edge position), tones drive the passband and the
/// Nyquist region where the reconstruction overshoots most, noise mixes tap
/// magnitudes randomly, denormal and full-scale levels probe the extremes of the
/// float exponent range, and the alternating pattern maximizes cancellation
/// inside the accumulator.
std::vector<Signal> make_signals(size_t length) {
  std::vector<Signal> signals;

  for (const size_t position : {size_t{0}, length / 2, length - 1}) {
    Signal impulse{"impulse@" + std::to_string(position), std::vector<float>(length, 0.0f)};
    impulse.samples[position] = 1.0f;
    signals.push_back(std::move(impulse));
  }

  for (const double freq : {50.0, 997.0, 11731.0, 19000.0, 23900.0}) {
    Signal tone{"sine" + std::to_string(static_cast<int>(freq)), std::vector<float>(length, 0.0f)};
    for (size_t i = 0; i < length; ++i) {
      tone.samples[i] = static_cast<float>(
          std::sin(static_cast<double>(kTwoPi) * freq * static_cast<double>(i) / 48000.0 + 0.37));
    }
    signals.push_back(std::move(tone));
  }

  std::mt19937 rng(20240517u);
  std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
  for (int seed = 0; seed < 3; ++seed) {
    Signal noise{"noise", std::vector<float>(length, 0.0f)};
    for (size_t i = 0; i < length; ++i) noise.samples[i] = uniform(rng);
    signals.push_back(std::move(noise));
  }

  // Subnormal magnitudes: every product underflows, so the accumulation order is
  // the only thing that can decide the result.
  Signal denormal{"denormal", std::vector<float>(length, 0.0f)};
  for (size_t i = 0; i < length; ++i) {
    denormal.samples[i] = std::numeric_limits<float>::denorm_min() *
                          static_cast<float>((i % 7) + 1) * (i % 2 == 0 ? 1.0f : -1.0f);
  }
  signals.push_back(std::move(denormal));

  Signal tiny{"tiny", std::vector<float>(length, 0.0f)};
  for (size_t i = 0; i < length; ++i) {
    tiny.samples[i] = std::numeric_limits<float>::min() * uniform(rng);
  }
  signals.push_back(std::move(tiny));

  Signal alternating{"alternating_full_scale", std::vector<float>(length, 0.0f)};
  for (size_t i = 0; i < length; ++i) alternating.samples[i] = i % 2 == 0 ? 1.0f : -1.0f;
  signals.push_back(std::move(alternating));

  Signal dc{"dc_full_scale", std::vector<float>(length, 1.0f)};
  signals.push_back(std::move(dc));

  // Terms whose magnitudes differ by far more than the double mantissa can hold,
  // so the accumulator's rounding depends on the order the taps are summed in.
  // Without this family a reordered (but otherwise identical) inner loop passes
  // the sweep, and the "same tap order" half of the contract goes unchecked.
  Signal wide{"wide_dynamic_range", std::vector<float>(length, 0.0f)};
  for (size_t i = 0; i < length; ++i) {
    const float magnitude = (i % 3 == 0) ? 1.0e20f : ((i % 3 == 1) ? 1.0e-20f : 1.0e-4f);
    wide.samples[i] = magnitude * (i % 2 == 0 ? 1.0f : -1.0f);
  }
  signals.push_back(std::move(wide));

  Signal spike{"lone_spike", std::vector<float>(length, 1.0e-25f)};
  spike.samples[length / 2] = -1.0e25f;
  if (length > 3) spike.samples[length / 3] = 1.0e25f;
  signals.push_back(std::move(spike));

  return signals;
}

/// Every FIR the interpolation is ever handed, plus two hand-built shapes the
/// factory cannot produce: an odd tap count (so taps_per_phase / 2 is not the
/// exact center) and a prototype whose length is not a multiple of the phase
/// count (so build_polyphase()'s round-up leaves trailing zero taps).
std::vector<std::pair<std::string, PolyphaseFir>> make_firs() {
  std::vector<std::pair<std::string, PolyphaseFir>> firs;
  for (const int factor : {1, 2, 4, 8, 16}) {
    firs.emplace_back("true_peak_" + std::to_string(factor) + "x",
                      sonare::rt::true_peak_fir_for(factor));
  }
  firs.emplace_back("odd_taps_3x", sonare::rt::design_polyphase_lowpass(3, 3 * 7, 8.0, true));
  firs.emplace_back("ragged_5x", sonare::rt::design_polyphase_lowpass(5, 23, 6.0, true));

  // Coefficients engineered so the result depends on the ORDER the taps are
  // summed in: two huge opposite-signed taps cancel exactly, and whether the
  // unit tap survives depends on whether it is added before or after them. No
  // realistic signal makes a smooth windowed-sinc kernel order-sensitive, so
  // without this design a reordered inner loop would pass the whole sweep.
  PolyphaseFir order_sensitive;
  order_sensitive.phases = 2;
  order_sensitive.taps_per_phase = 4;
  order_sensitive.taps = {1.0e20f, -1.0e20f, 1.0f, 0.0f, 0.0f, 1.0f, -1.0e20f, 1.0e20f};
  firs.emplace_back("order_sensitive", std::move(order_sensitive));
  return firs;
}

/// One signal per family for the (much more expensive) filter-level sweep.
bool is_representative(const std::string& name) {
  return name == "sine11731" || name == "noise" || name == "denormal" ||
         name == "alternating_full_scale" || name.rfind("impulse@", 0) == 0;
}

}  // namespace

TEST_CASE("Polyphase interpolation is bit-identical to the reference convolution",
          "[rt][truepeak][polyphase]") {
  size_t comparisons = 0;
  for (const auto& [fir_name, fir] : make_firs()) {
    const auto rows = reference_rows(fir);
    // Lengths shorter than the stencil are the interesting ones: they leave the
    // hoisted tap range clamped on both sides at once.
    for (const size_t length : {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{13},
                                static_cast<size_t>(std::max(1, fir.taps_per_phase / 2)),
                                static_cast<size_t>(std::max(1, fir.taps_per_phase)),
                                static_cast<size_t>(fir.taps_per_phase + 1), size_t{33}, size_t{64},
                                size_t{129}, size_t{257}, size_t{512}}) {
      for (const auto& signal : make_signals(length)) {
        for (size_t index = 0; index < length; ++index) {
          for (int phase = 0; phase < fir.phases; ++phase) {
            const float expected = reference_interpolate(
                signal.samples.data(), length, index, phase, rows, fir.taps_per_phase, fir.phases);
            const float actual = sonare::rt::interpolate_polyphase_sample(
                signal.samples.data(), length, index, phase, fir);
            ++comparisons;
            if (!bit_equal(expected, actual)) {
              CAPTURE(fir_name, signal.name, length, index, phase, expected, actual);
              FAIL("polyphase interpolation is no longer bit-identical to the reference");
            }
          }
        }
      }
    }
  }
  // A guard that swept nothing would pass silently.
  REQUIRE(comparisons > 500000);
}

TEST_CASE("Polyphase interpolation rejects malformed inputs the same way", "[rt][polyphase]") {
  const PolyphaseFir& fir = sonare::rt::true_peak_fir_for(4);
  const std::vector<float> data(32, 0.5f);

  REQUIRE(sonare::rt::interpolate_polyphase_sample(nullptr, 32, 0, 0, fir) == 0.0f);
  REQUIRE(sonare::rt::interpolate_polyphase_sample(data.data(), 0, 0, 0, fir) == 0.0f);
  REQUIRE(sonare::rt::interpolate_polyphase_sample(data.data(), data.size(), data.size(), 0, fir) ==
          0.0f);
  REQUIRE(sonare::rt::interpolate_polyphase_sample(data.data(), data.size(), 0, -1, fir) == 0.0f);
  REQUIRE(sonare::rt::interpolate_polyphase_sample(data.data(), data.size(), 0, fir.phases, fir) ==
          0.0f);

  // A FIR whose declared shape outruns its coefficient block returns silence
  // instead of reading past the end.
  PolyphaseFir undersized;
  undersized.phases = 4;
  undersized.taps_per_phase = 12;
  undersized.taps.assign(8, 1.0f);
  REQUIRE(sonare::rt::interpolate_polyphase_sample(data.data(), data.size(), 4, 1, undersized) ==
          0.0f);
}

TEST_CASE("TruePeakFilter oversampled output is bit-identical to the reference",
          "[rt][truepeak][polyphase]") {
  // The filter-level check: the same sweep driven through the shipping streaming
  // path (cross-block history, multi-channel, several block sizes) against an
  // open-coded reference upsample. This is what a consumer of truePeakDb
  // actually exercises.
  for (const int factor : {2, 4, 8}) {
    const PolyphaseFir& fir = sonare::rt::true_peak_fir_for(factor);
    const auto rows = reference_rows(fir);
    const size_t history_size = static_cast<size_t>(fir.taps_per_phase);

    for (const int block : {1, 3, 64, 512}) {
      for (const int channels : {1, 2, 8}) {
        const auto signals = make_signals(static_cast<size_t>(block) * 4);
        for (const auto& signal : signals) {
          // The per-sample sweep above already covers every signal family; at
          // filter level one representative per family is enough to catch a
          // history/indexing regression, and the full cross product would cost
          // minutes.
          if (!is_representative(signal.name)) continue;
          TruePeakFilter filter(channels, factor);
          filter.prepare(channels, block);

          // Reference cross-block state, maintained exactly as the filter does.
          std::vector<std::vector<float>> history(static_cast<size_t>(channels),
                                                  std::vector<float>(history_size, 0.0f));

          std::vector<std::vector<float>> inputs(static_cast<size_t>(channels));
          std::vector<std::vector<float>> outputs(
              static_cast<size_t>(channels),
              std::vector<float>(static_cast<size_t>(block) * static_cast<size_t>(factor), 0.0f));
          std::vector<const float*> in_ptrs(static_cast<size_t>(channels));
          std::vector<float*> out_ptrs(static_cast<size_t>(channels));

          const int blocks = static_cast<int>(signal.samples.size()) / block;
          for (int b = 0; b < blocks; ++b) {
            for (int ch = 0; ch < channels; ++ch) {
              const size_t c = static_cast<size_t>(ch);
              inputs[c].assign(static_cast<size_t>(block), 0.0f);
              for (int i = 0; i < block; ++i) {
                // Decorrelate the channels so a per-channel history mix-up
                // cannot pass unnoticed.
                const size_t source = static_cast<size_t>(b * block + i);
                inputs[c][static_cast<size_t>(i)] =
                    signal.samples[source] * (1.0f - 0.1f * static_cast<float>(ch));
              }
              in_ptrs[c] = inputs[c].data();
              out_ptrs[c] = outputs[c].data();
            }
            filter.upsample_with_history(in_ptrs.data(), out_ptrs.data(), channels, block);

            for (int ch = 0; ch < channels; ++ch) {
              const size_t c = static_cast<size_t>(ch);
              std::vector<float> extended(history[c]);
              extended.insert(extended.end(), inputs[c].begin(), inputs[c].end());
              for (int i = 0; i < block; ++i) {
                const size_t index = history_size + static_cast<size_t>(i);
                for (int phase = 0; phase < factor; ++phase) {
                  const float expected =
                      reference_interpolate(extended.data(), extended.size(), index, phase, rows,
                                            fir.taps_per_phase, fir.phases);
                  const float actual = outputs[c][static_cast<size_t>(i * factor + phase)];
                  if (!bit_equal(expected, actual)) {
                    CAPTURE(factor, block, channels, ch, b, i, phase, signal.name, expected,
                            actual);
                    FAIL("streaming true-peak upsample is no longer bit-identical");
                  }
                }
              }
              const size_t keep = std::min(history_size, extended.size());
              std::fill(history[c].begin(), history[c].end(), 0.0f);
              std::copy(extended.end() - static_cast<std::ptrdiff_t>(keep), extended.end(),
                        history[c].end() - static_cast<std::ptrdiff_t>(keep));
            }
          }
        }
      }
    }
  }
}
