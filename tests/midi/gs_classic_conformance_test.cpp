/// @file gs_classic_conformance_test.cpp
/// @brief The GS classic engine against the soundings renderer's reference digests, and
/// the classic unit's agreement across host rates.
///
/// The raw configuration (p0 without overlays, `gs_classic_models_raw.inc`) is drawn at
/// 32 kHz for every state `gs_classic_reference.tsv` records as rendered, and its
/// per-channel third-octave digest is compared with the reference's in every band the
/// reference puts at -60 dBFS or above. Per type the median difference must be at most
/// 0.1 dB and the worst at most 1.0 dB, or 3.0 dB where the type's graph holds a hold, a
/// quantize or a hard-clip shaper. Each type's compared-state count is reported, and every
/// state of a slot the raw model reads that the renderer does not refuse must be among
/// them. The default run compares each type's power-on state; the full sweep is slow and
/// runs under `[gs-classic-conformance-all]`.
///
/// The host-rate case draws the default configuration at power-on through GsClassicUnit at
/// 44.1 and 48 kHz and requires the two to agree within 0.5 dB in every band below 12.5 kHz
/// that either puts at -60 dBFS or above. One drawing's digest moves with where the input
/// falls against the 32 kHz grid and the lfo's start, so each rate's statistic averages band
/// power over eight input offsets spread over half a millisecond, or a pitch node's window,
/// each Welch-averaged over the drawing. Both interleaved offset sets must meet the
/// 0.5 dB rate bound. Where both sets are audible, their paired rate differences must
/// agree within 0.25 dB (the offset count doubles until they do). Common phase variation
/// is shared by both rates and does not measure a rate error. The default run holds the first type;
/// every type runs under `[gs-classic-conformance-all]`.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "core/resample.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_classic/classic_unit.h"
#include "midi/synth/gs_classic/graph_engine.h"
#include "midi/synth/gs_classic/model_registry.h"
#include "util/constants.h"

namespace sonare::midi::synth::gs_classic {
#define GS_CLASSIC_SET_PREFIX gs_classic_models_raw_conformance
#include "midi/gs_classic_models_raw.inc"
#undef GS_CLASSIC_SET_PREFIX
}  // namespace sonare::midi::synth::gs_classic

namespace {

namespace gc = sonare::midi::synth::gs_classic;
using sonare::midi::synth::gs_efx_printed_states;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsEfxTypeDefaults;

constexpr const char* kReferencePath = "tests/midi/gs_classic_reference.tsv";

// Stimulus and digest as the reference TSV header states them.
constexpr std::size_t kHalf = 32000;
constexpr std::size_t kStimulusLength = 2 * kHalf;
constexpr uint32_t kSeedL = 0x12345678u;
constexpr uint32_t kSeedR = 0x9E3779B9u;
constexpr int64_t kSawHz = 220;
constexpr double kStimulusScale = 0.25;
constexpr std::size_t kWindowsPerSecond = 4;
constexpr int kLowestBand = -13;
constexpr int kHighestBand = 11;
constexpr std::size_t kBands = kHighestBand - kLowestBand + 1;
constexpr double kDigestFloorDb = -300.0;
constexpr double kPowerFloor = 1e-30;

// Engine against renderer.
constexpr double kComparedFloorDb = -60.0;
constexpr double kMedianToleranceDb = 0.1;
constexpr double kWorstToleranceDb = 1.0;
constexpr double kWorstToleranceSteppedDb = 3.0;

// Across host rates: the bands stop below the resamplers' 12.5 kHz passband edge, and a
// fifth-second window keeps the frame even at 44.1 kHz.
constexpr int kHighestHostBand = 10;
constexpr std::size_t kHostBands = kHighestHostBand - kLowestBand + 1;
constexpr std::size_t kHostWindowsPerSecond = 5;
constexpr double kHostRateToleranceDb = 0.5;
constexpr double kHostNoiseCeilingDb = 0.25;
constexpr std::size_t kHostOffsets = 8;
constexpr std::size_t kHostMaxOffsets = 64;
constexpr double kHostMinSpanS = 16.0 / 32000.0;
constexpr double kHostPeriods = 3.0;
constexpr double kHostMinSeconds = 2.0;
constexpr double kHostMaxSeconds = 6.0;

constexpr std::size_t kBlock = 4096;
constexpr int kHostBlock = 512;

double noise_sample(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return static_cast<double>(static_cast<int16_t>(s >> 16)) / 32768.0;
}

std::vector<double> make_stimulus(uint32_t seed) {
  std::vector<double> x(kStimulusLength);
  for (std::size_t i = 0; i < kHalf; ++i) x[i] = kStimulusScale * noise_sample(seed);
  for (std::size_t i = 0; i < kHalf; ++i) {
    const int64_t phase = (static_cast<int64_t>(i) * kSawHz * 65536 / 32000) % 65536;
    x[kHalf + i] = kStimulusScale * static_cast<double>(phase - 32768) / 32768.0;
  }
  return x;
}

const std::vector<double>& stimulus(int channel) {
  static const std::vector<double> left = make_stimulus(kSeedL);
  static const std::vector<double> right = make_stimulus(kSeedR);
  return channel == 0 ? left : right;
}

using Drawing = std::array<std::vector<double>, 2>;

/// [channel][window][band] in dB over two seconds of 1/@p per_second windows at @p rate,
/// the bands kLowestBand..@p highest_band.
std::vector<double> digest(const Drawing& d, double rate, std::size_t per_second,
                           int highest_band) {
  const auto window = static_cast<std::size_t>(rate) / per_second;
  const std::size_t windows = 2 * per_second;
  sonare::FFT fft(static_cast<int>(window));
  std::vector<float> frame(window);
  std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.n_bins()));
  const double hz_per_bin = rate / static_cast<double>(window);
  std::vector<double> out;
  for (const std::vector<double>& ch : d) {
    REQUIRE(ch.size() >= windows * window);
    for (std::size_t w = 0; w < windows; ++w) {
      for (std::size_t i = 0; i < window; ++i) frame[i] = static_cast<float>(ch[w * window + i]);
      fft.forward(frame.data(), bins.data());
      for (int n = kLowestBand; n <= highest_band; ++n) {
        const double centre = 1000.0 * std::pow(2.0, n / 3.0);
        const double lo = centre * std::pow(2.0, -1.0 / 6.0);
        const double hi = centre * std::pow(2.0, 1.0 / 6.0);
        double power = 0.0;
        for (std::size_t k = 0; k < bins.size(); ++k) {
          const double f = static_cast<double>(k) * hz_per_bin;
          if (f < lo || f >= hi) continue;
          const double weight = (k == 0 || k + 1 == bins.size()) ? 1.0 : 2.0;
          power += weight * std::norm(std::complex<double>(bins[k])) /
                   (static_cast<double>(window) * static_cast<double>(window));
        }
        out.push_back(power < kPowerFloor ? kDigestFloorDb : 10.0 * std::log10(power));
      }
    }
  }
  return out;
}

/// One state of the reference: a slot of -1 is the power-on state.
struct ReferenceState {
  uint16_t type = 0;
  int slot = -1;
  int byte = -1;
  std::string status;
  std::vector<double> digest;  // [channel][window][band], rendered states only
};

uint16_t parse_type(const std::string& s) {
  return static_cast<uint16_t>(std::stoi(s.substr(0, 2), nullptr, 16) << 8 |
                               std::stoi(s.substr(3, 2), nullptr, 16));
}

const std::vector<ReferenceState>& reference() {
  static const std::vector<ReferenceState> states = [] {
    std::vector<ReferenceState> out;
    std::ifstream in(kReferencePath);
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#' || line.rfind("type", 0) == 0) continue;
      std::istringstream row(line);
      std::vector<std::string> cells;
      for (std::string cell; std::getline(row, cell, '\t');) cells.push_back(cell);
      REQUIRE(cells.size() >= 6);
      const uint16_t type = parse_type(cells[0]);
      const int slot = cells[1] == "-" ? -1 : std::stoi(cells[1]);
      const int byte = cells[2] == "-" ? -1 : std::stoi(cells[2]);
      if (out.empty() || out.back().type != type || out.back().slot != slot ||
          out.back().byte != byte) {
        out.push_back({type, slot, byte, cells[3], {}});
      }
      if (cells[3] != "rendered") continue;
      REQUIRE(cells.size() == 6 + kBands);
      const std::size_t channel = cells[4] == "L" ? 0 : 1;
      const auto window = static_cast<std::size_t>(std::stoi(cells[5]));
      ReferenceState& state = out.back();
      constexpr std::size_t kWindows = 2 * kWindowsPerSecond;
      state.digest.resize(2 * kWindows * kBands, kDigestFloorDb);
      for (std::size_t b = 0; b < kBands; ++b) {
        state.digest[(channel * kWindows + window) * kBands + b] = std::stod(cells[6 + b]);
      }
    }
    return out;
  }();
  return states;
}

const gc::GsClassicModelRegistry& raw_registry() {
  static const gc::GsClassicModelRegistry registry(gc::gs_classic_models_raw_conformance());
  return registry;
}

/// 02 0C's bytes are filed under the manual's other number for it, which this lookup asks.
std::array<uint8_t, 20> power_on(uint16_t type) {
  const GsEfxTypeDefaults* entry = gs_efx_type_defaults(type);
  if (entry == nullptr) FAIL("no power-on bytes for type " << std::hex << type);
  return entry->params;
}

Drawing draw(const gc::GsClassicModelSet& models, const gc::GsClassicType& type,
             const std::array<uint8_t, 20>& bytes) {
  gc::GsClassicGraph graph;
  REQUIRE(graph.prepare(models, type, kBlock, &gc::gs_classic_extension_kernels()));
  graph.reset();
  for (std::size_t slot = 0; slot < bytes.size(); ++slot) graph.set_byte(slot, bytes[slot]);
  Drawing out;
  out[0].resize(kStimulusLength);
  out[1].resize(kStimulusLength);
  graph.process(stimulus(0).data(), stimulus(1).data(), out[0].data(), out[1].data(),
                kStimulusLength);
  return out;
}

/// The slots some node of @p type reads through a byte.
std::set<int> bound_slots(const gc::GsClassicModelSet& m, const gc::GsClassicType& type) {
  std::set<int> out;
  for (std::size_t n = type.node_begin; n < type.node_end; ++n) {
    const gc::GsClassicNode& node = m.nodes[n];
    for (std::size_t v = 0; v < node.value_count; ++v) {
      const gc::GsClassicValue& value = m.values[node.value_begin + v];
      if (value.kind == gc::GsClassicValueKind::kByte) out.insert(value.slot);
    }
  }
  return out;
}

/// Whether @p type steps its signal: a hold, a quantize or a hard-clip shaper.
bool steps(const gc::GsClassicModelSet& m, const gc::GsClassicType& type) {
  for (std::size_t n = type.node_begin; n < type.node_end; ++n) {
    const gc::GsClassicNode& node = m.nodes[n];
    if (node.kind == gc::GsClassicNodeKind::kHold || node.kind == gc::GsClassicNodeKind::kQuantize)
      return true;
    if (node.kind == gc::GsClassicNodeKind::kShaper &&
        (node.flags & 0x0F) == static_cast<uint8_t>(gc::GsClassicShaperCurve::kHard))
      return true;
  }
  return false;
}

struct TypeResult {
  std::size_t compared = 0;
  std::vector<double> differences;
  std::string worst_at;  ///< where the largest difference fell
  double worst = 0.0;
};

/// Draws each rendered state @p pick keeps and gathers its band differences per type.
template <typename Pick>
std::map<uint16_t, TypeResult> compare(Pick pick) {
  const gc::GsClassicModelRegistry& registry = raw_registry();
  REQUIRE(registry.valid());
  std::map<uint16_t, TypeResult> out;
  for (const ReferenceState& state : reference()) {
    if (state.status != "rendered" || !pick(state)) continue;
    const gc::GsClassicType* model = registry.find(state.type);
    REQUIRE(model != nullptr);
    std::array<uint8_t, 20> bytes = power_on(state.type);
    if (state.slot >= 0)
      bytes[static_cast<std::size_t>(state.slot)] = static_cast<uint8_t>(state.byte);
    const std::vector<double> got =
        digest(draw(registry.models(), *model, bytes), gc::kGsClassicSampleRateHz,
               kWindowsPerSecond, kHighestBand);
    REQUIRE(got.size() == state.digest.size());
    TypeResult& result = out[state.type];
    ++result.compared;
    for (std::size_t i = 0; i < got.size(); ++i) {
      if (state.digest[i] < kComparedFloorDb) continue;
      const double d = std::fabs(got[i] - state.digest[i]);
      result.differences.push_back(d);
      if (d > result.worst) {
        constexpr std::size_t kWindows = 2 * kWindowsPerSecond;
        std::ostringstream where;
        where << "slot " << state.slot << " byte " << state.byte << " channel "
              << i / (kWindows * kBands) << " window " << i / kBands % kWindows << " band "
              << i % kBands << ": " << got[i] << " / " << state.digest[i] << " dB";
        result.worst = d;
        result.worst_at = where.str();
      }
    }
  }
  return out;
}

void require_within_tolerance(const std::map<uint16_t, TypeResult>& results) {
  const gc::GsClassicModelRegistry& registry = raw_registry();
  for (const auto& [type, result] : results) {
    const gc::GsClassicType* model = registry.find(type);
    REQUIRE(model != nullptr);
    std::vector<double> d = result.differences;
    REQUIRE(!d.empty());
    std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
    const double median = d[d.size() / 2];
    const double worst = *std::max_element(d.begin(), d.end());
    const double worst_limit =
        steps(registry.models(), *model) ? kWorstToleranceSteppedDb : kWorstToleranceDb;
    WARN("type " << std::hex << type << std::dec << ": compared " << result.compared << " states, "
                 << d.size() << " bands, median " << median << " dB, worst " << worst
                 << " dB (limit " << worst_limit << ") at " << result.worst_at);
    CHECK(median <= kMedianToleranceDb);
    CHECK(worst <= worst_limit);
  }
}

/// Host samples the unit's resampler round trip delays by, read off an impulse through a
/// type with no nodes.
std::size_t round_trip(int rate) {
  gc::GsClassicType thru{};
  thru.out_l = 0;
  thru.out_r = 1;
  thru.max_delay_samples = 1;
  gc::GsClassicModelSet set{};
  set.types = &thru;
  set.n_types = 1;
  gc::GsClassicUnit unit(set, thru);
  unit.prepare(static_cast<double>(rate), kHostBlock);
  constexpr std::size_t kAt = 1000;
  std::array<std::vector<float>, 2> io;
  for (auto& ch : io) ch.assign(static_cast<std::size_t>(rate) / 10, 0.0f);
  io[0][kAt] = 1.0f;
  io[1][kAt] = 1.0f;
  for (std::size_t at = 0; at < io[0].size(); at += kHostBlock) {
    const int n = static_cast<int>(std::min<std::size_t>(kHostBlock, io[0].size() - at));
    float* channels[2] = {io[0].data() + at, io[1].data() + at};
    unit.process(channels, 2, n);
  }
  const auto peak = static_cast<std::size_t>(
      std::max_element(io[0].begin(), io[0].end(),
                       [](float x, float y) { return std::fabs(x) < std::fabs(y); }) -
      io[0].begin());
  REQUIRE(peak > kAt);
  return peak - kAt;
}

/// The slowest rate a power-on lfo of @p type runs at, or 0 where none runs; a rate a
/// control drives is not counted.
double slowest_lfo_hz(const gc::GsClassicModelSet& m, const gc::GsClassicType& type) {
  const std::array<uint8_t, 20> bytes = power_on(type.type);
  double slowest = 0.0;
  for (std::size_t n = type.node_begin; n < type.node_end; ++n) {
    const gc::GsClassicNode& node = m.nodes[n];
    if (node.kind != gc::GsClassicNodeKind::kLfo) continue;
    const gc::GsClassicValue& rate = m.values[node.value_begin];
    double hz = 0.0;
    if (rate.kind == gc::GsClassicValueKind::kConst) hz = rate.constant;
    if (rate.kind == gc::GsClassicValueKind::kByte) hz = m.luts[rate.table].v[bytes[rate.slot]];
    if (hz > 0.0 && (slowest == 0.0 || hz < slowest)) slowest = hz;
  }
  return slowest;
}

/// Seconds a type is drawn for: kHostPeriods of its slowest power-on lfo, within bounds.
double render_seconds(const gc::GsClassicModelSet& m, const gc::GsClassicType& type) {
  const double hz = slowest_lfo_hz(m, type);
  const double wanted = hz > 0.0 ? kHostPeriods / hz : 0.0;
  return std::clamp(wanted, kHostMinSeconds, kHostMaxSeconds);
}

struct HostRate {
  int rate = 0;
  std::size_t delay = 0;                 ///< the unit's round trip
  std::array<std::vector<float>, 2> in;  ///< the stimulus repeated past kHostMaxSeconds
};

HostRate host_rate(int rate) {
  HostRate out;
  out.rate = rate;
  out.delay = round_trip(rate);
  // One stimulus period past the longest drawing covers the round trip.
  const auto repeats = static_cast<std::size_t>(std::ceil(kHostMaxSeconds / 2.0)) + 1;
  for (std::size_t c = 0; c < 2; ++c) {
    const std::vector<double>& xd = stimulus(static_cast<int>(c));
    std::vector<float> x;
    for (std::size_t k = 0; k < repeats; ++k) x.insert(x.end(), xd.begin(), xd.end());
    out.in[c] =
        sonare::resample(x.data(), x.size(), static_cast<int>(gc::kGsClassicSampleRateHz), rate);
  }
  return out;
}

/// Band mean-square power, [channel][band], of @p type's power-on drawing through a
/// GsClassicUnit over @p seconds: the input delayed by @p offset host samples, the output
/// advanced by the round trip and the offset, then a Welch average of Hann windows a
/// fifth of a second long at half overlap.
std::vector<double> host_power(const gc::GsClassicModelSet& m, const gc::GsClassicType& type,
                               const HostRate& host, std::size_t offset, double seconds) {
  gc::GsClassicUnit unit(m, type);
  unit.prepare(static_cast<double>(host.rate), kHostBlock);
  const std::array<uint8_t, 20> bytes = power_on(type.type);
  for (std::size_t s = 0; s < bytes.size(); ++s) {
    REQUIRE(unit.set_parameter(static_cast<unsigned int>(s), static_cast<float>(bytes[s])));
  }
  const auto length = static_cast<std::size_t>(seconds * host.rate);
  const std::size_t total = length + host.delay + offset;
  REQUIRE(total <= host.in[0].size() + offset);
  std::array<std::vector<float>, 2> io;
  for (std::size_t c = 0; c < 2; ++c) {
    io[c].assign(offset, 0.0f);
    io[c].insert(io[c].end(), host.in[c].begin(),
                 host.in[c].begin() + static_cast<std::ptrdiff_t>(total - offset));
  }
  for (std::size_t at = 0; at < total; at += kHostBlock) {
    const int n = static_cast<int>(std::min<std::size_t>(kHostBlock, total - at));
    float* channels[2] = {io[0].data() + at, io[1].data() + at};
    unit.process(channels, 2, n);
  }
  REQUIRE(unit.fifo_underruns() == 0);

  const std::size_t window = static_cast<std::size_t>(host.rate) / kHostWindowsPerSecond;
  const std::size_t hop = window / 2;
  std::vector<float> taper(window);
  double taper_power = 0.0;
  for (std::size_t i = 0; i < window; ++i) {
    const double w = 0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * static_cast<double>(i) /
                                          static_cast<double>(window));
    taper[i] = static_cast<float>(w);
    taper_power += w * w;
  }
  sonare::FFT fft(static_cast<int>(window));
  std::vector<float> frame(window);
  std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.n_bins()));
  const double hz_per_bin = static_cast<double>(host.rate) / static_cast<double>(window);
  std::vector<double> out(2 * kHostBands, 0.0);
  std::size_t segments = 0;
  const std::size_t skip = host.delay + offset;
  for (std::size_t start = 0; start + window <= length; start += hop, ++segments) {
    for (std::size_t c = 0; c < 2; ++c) {
      for (std::size_t i = 0; i < window; ++i) frame[i] = io[c][skip + start + i] * taper[i];
      fft.forward(frame.data(), bins.data());
      for (std::size_t b = 0; b < kHostBands; ++b) {
        const int n = kLowestBand + static_cast<int>(b);
        const double centre = 1000.0 * std::pow(2.0, n / 3.0);
        const double lo = centre * std::pow(2.0, -1.0 / 6.0);
        const double hi = centre * std::pow(2.0, 1.0 / 6.0);
        double power = 0.0;
        for (std::size_t k = 0; k < bins.size(); ++k) {
          const double f = static_cast<double>(k) * hz_per_bin;
          if (f < lo || f >= hi) continue;
          const double weight = (k == 0 || k + 1 == bins.size()) ? 1.0 : 2.0;
          power += weight * std::norm(std::complex<double>(bins[k]));
        }
        out[c * kHostBands + b] += power / (static_cast<double>(window) * taper_power);
      }
    }
  }
  REQUIRE(segments > 0);
  for (double& v : out) v /= static_cast<double>(segments);
  return out;
}

/// The span the input offsets are spread over: kHostMinSpanS, or the longest power-on
/// pitch window where that is longer, since a pitch node's output cycles with it.
double offset_span_s(const gc::GsClassicModelSet& m, const gc::GsClassicType& type) {
  const std::array<uint8_t, 20> bytes = power_on(type.type);
  double span = kHostMinSpanS;
  for (std::size_t n = type.node_begin; n < type.node_end; ++n) {
    const gc::GsClassicNode& node = m.nodes[n];
    if (node.kind != gc::GsClassicNodeKind::kPitch) continue;
    const gc::GsClassicValue& window = m.values[node.value_begin + 1];
    double ms = 0.0;
    if (window.kind == gc::GsClassicValueKind::kConst) ms = window.constant;
    if (window.kind == gc::GsClassicValueKind::kByte)
      ms = m.luts[window.table].v[bytes[window.slot]];
    span = std::max(span, ms / 1000.0);
  }
  return span;
}

/// The band power, in dB, averaged over @p count input offsets spread evenly over @p span_s;
/// set 0 takes the even points of a grid of 2 * @p count and set 1 the odd ones, so the two
/// sets are disjoint and alike.
std::vector<double> host_digest(const gc::GsClassicModelSet& m, const gc::GsClassicType& type,
                                const HostRate& host, int set, std::size_t count, double span_s,
                                double seconds) {
  std::vector<double> sum(2 * kHostBands, 0.0);
  for (std::size_t k = 0; k < count; ++k) {
    const double at_s = static_cast<double>(2 * k + static_cast<std::size_t>(set)) * span_s /
                        static_cast<double>(2 * count);
    const auto offset = static_cast<std::size_t>(std::lround(at_s * host.rate));
    const std::vector<double> p = host_power(m, type, host, offset, seconds);
    for (std::size_t i = 0; i < sum.size(); ++i) sum[i] += p[i];
  }
  std::vector<double> out;
  for (double v : sum) {
    const double mean = v / static_cast<double>(count);
    out.push_back(mean < kPowerFloor ? kDigestFloorDb : 10.0 * std::log10(mean));
  }
  return out;
}

/// The largest band difference over the bands either digest puts at kComparedFloorDb or
/// above; @p at, when given, receives its index.
double host_difference(const std::vector<double>& a, const std::vector<double>& b,
                       std::size_t* at) {
  double worst = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] < kComparedFloorDb && b[i] < kComparedFloorDb) continue;
    const double d = std::fabs(a[i] - b[i]);
    if (d > worst) {
      worst = d;
      if (at != nullptr) *at = i;
    }
  }
  return worst;
}

struct HostComparison {
  double across_even;
  double across_odd;
  double residual_noise;
};

HostComparison host_comparison(const std::vector<double>& even44, const std::vector<double>& even48,
                               const std::vector<double>& odd44, const std::vector<double>& odd48) {
  double residual = 0.0;
  for (std::size_t i = 0; i < even44.size(); ++i) {
    const bool even_active = even44[i] >= kComparedFloorDb || even48[i] >= kComparedFloorDb;
    const bool odd_active = odd44[i] >= kComparedFloorDb || odd48[i] >= kComparedFloorDb;
    // Below the floor a phase set shows no rate difference; its absolute check still runs.
    if (!even_active || !odd_active) continue;
    const double delta_even = even44[i] - even48[i];
    const double delta_odd = odd44[i] - odd48[i];
    residual = std::max(residual, std::fabs(delta_even - delta_odd));
  }
  return {host_difference(even44, even48, nullptr), host_difference(odd44, odd48, nullptr),
          residual};
}

/// Requires each type @p pick keeps to draw alike at 44.1 and 48 kHz, against the
/// stability of the paired rate differences across phase sets.
template <typename Pick>
void require_rate_independent(Pick pick) {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const gc::GsClassicModelSet& m = registry.models();
  const std::array<HostRate, 2> rates = {host_rate(44100), host_rate(48000)};
  WARN("GS classic round trip: " << rates[0].delay << " samples at 44.1 kHz, " << rates[1].delay
                                 << " at 48 kHz");
  std::size_t compared = 0;
  double worst_across = 0.0;
  for (std::size_t t = 0; t < m.n_types; ++t) {
    const gc::GsClassicType& type = m.types[t];
    if (!pick(t)) continue;
    INFO("type " << std::hex << type.type);
    const double seconds = render_seconds(m, type);
    // Same phases at both rates, so modulation is not read as rate error.
    std::size_t offsets = kHostOffsets;
    const double span = offset_span_s(m, type);
    std::vector<double> at48;
    std::vector<double> at44;
    HostComparison comparison{};
    for (;; offsets *= 2) {
      at48 = host_digest(m, type, rates[1], 0, offsets, span, seconds);
      at44 = host_digest(m, type, rates[0], 0, offsets, span, seconds);
      const auto odd48 = host_digest(m, type, rates[1], 1, offsets, span, seconds);
      const auto odd44 = host_digest(m, type, rates[0], 1, offsets, span, seconds);
      comparison = host_comparison(at44, at48, odd44, odd48);
      if (comparison.residual_noise <= kHostNoiseCeilingDb || offsets >= kHostMaxOffsets) break;
    }
    CHECK(comparison.residual_noise <= kHostNoiseCeilingDb);
    std::size_t at = 0;
    host_difference(at44, at48, &at);
    WARN("type " << std::hex << type.type << std::dec << ": " << seconds << " s, " << offsets
                 << " offsets over " << span * 1000.0 << " ms, across rates "
                 << comparison.across_even << " dB even / " << comparison.across_odd
                 << " dB odd, residual noise " << comparison.residual_noise << " dB");
    INFO("worst at channel " << at / kHostBands << " band " << at % kHostBands << ": " << at44[at]
                             << " / " << at48[at] << " dB");
    CHECK(comparison.across_even <= kHostRateToleranceDb);
    CHECK(comparison.across_odd <= kHostRateToleranceDb);
    worst_across = std::max({worst_across, comparison.across_even, comparison.across_odd});
    ++compared;
  }
  CHECK(compared > 0);
  WARN("GS classic host rates: worst band difference " << worst_across << " dB over " << compared
                                                       << " types");
}

}  // namespace

TEST_CASE("GS classic host comparison distinguishes phase variation from rate error",
          "[gs-classic-conformance]") {
  SECTION("common phase variation cancels in the paired rate residual") {
    const auto result = host_comparison({-20.0}, {-20.1}, {-40.0}, {-40.1});
    CHECK(result.across_even <= kHostRateToleranceDb);
    CHECK(result.across_odd <= kHostRateToleranceDb);
    CHECK(result.residual_noise <= kHostNoiseCeilingDb);
  }
  SECTION("unstable rate error fails the residual ceiling") {
    const auto result = host_comparison({-20.0}, {-20.0}, {-20.0}, {-20.3});
    CHECK(result.across_even <= kHostRateToleranceDb);
    CHECK(result.across_odd <= kHostRateToleranceDb);
    CHECK(result.residual_noise > kHostNoiseCeilingDb);
  }
  SECTION("stable rate error still fails the absolute ceiling") {
    const auto result = host_comparison({-20.0}, {-20.6}, {-40.0}, {-40.6});
    CHECK(result.across_even > kHostRateToleranceDb);
    CHECK(result.across_odd > kHostRateToleranceDb);
    CHECK(result.residual_noise <= kHostNoiseCeilingDb);
  }
  SECTION("the odd set is independently checked") {
    const auto result = host_comparison({-20.0}, {-20.0}, {-20.0}, {-20.6});
    CHECK(result.across_even <= kHostRateToleranceDb);
    CHECK(result.across_odd > kHostRateToleranceDb);
    CHECK(result.residual_noise > kHostNoiseCeilingDb);
  }
  SECTION("inactive rate pairs contribute no below-floor error") {
    const auto result = host_comparison({-70.0}, {-100.0}, {-20.0}, {-20.0});
    CHECK(result.across_even == 0.0);
    CHECK(result.across_odd == 0.0);
    CHECK(result.residual_noise == 0.0);
  }
  SECTION("an active phase set keeps its absolute check when the other is inactive") {
    const auto result = host_comparison({-70.0}, {-100.0}, {-20.0}, {-20.6});
    CHECK(result.across_even == 0.0);
    CHECK(result.across_odd > kHostRateToleranceDb);
    CHECK(result.residual_noise == 0.0);
  }
}

TEST_CASE("GS classic conformance: the reference lists every printed state of the raw models",
          "[gs-classic-conformance]") {
  const gc::GsClassicModelRegistry& registry = raw_registry();
  REQUIRE(registry.valid());
  const gc::GsClassicModelSet& m = registry.models();
  std::map<uint16_t, std::vector<const ReferenceState*>> by_type;
  for (const ReferenceState& state : reference()) by_type[state.type].push_back(&state);
  REQUIRE(by_type.size() == m.n_types);

  for (const auto& [type, states] : by_type) {
    INFO("type " << std::hex << type);
    const gc::GsClassicType* model = registry.find(type);
    REQUIRE(model != nullptr);
    const std::set<int> bound = bound_slots(m, *model);
    const std::array<uint8_t, 20> rest = power_on(type);
    REQUIRE(states.front()->slot == -1);
    CHECK(states.front()->status == "rendered");

    std::map<int, std::vector<const ReferenceState*>> by_slot;
    for (const ReferenceState* state : states) {
      if (state->slot >= 0) by_slot[state->slot].push_back(state);
    }
    for (std::size_t s = 0; s < gc::kGsClassicByteSlots; ++s) {
      const bool printed =
          model->printed_lo[s] != model->printed_hi[s] || model->printed_lo[s] != 0;
      CHECK(printed == (by_slot.count(static_cast<int>(s)) != 0));
    }
    for (const auto& [slot, ends] : by_slot) {
      INFO("slot " << slot);
      REQUIRE(ends.size() == 2);
      const auto at = static_cast<std::size_t>(slot);
      // The reference draws the protocol's accepted ends, which a state list narrows.
      const int states = gs_efx_printed_states(type, static_cast<uint8_t>(slot));
      CHECK(ends[0]->byte == 0);
      CHECK(ends[1]->byte == (states > 0 ? states - 1 : 127));
      for (const ReferenceState* end : ends) {
        INFO("byte " << end->byte << " status " << end->status);
        if (bound.count(slot) == 0) {
          CHECK(end->status == "unbound");
        } else if (end->byte == rest[at]) {
          CHECK(end->status == "same-as-power-on");
        } else {
          // A bound state is compared unless the renderer refuses it.
          CHECK((end->status == "rendered" || end->status == "unrenderable"));
        }
      }
    }
  }
}

TEST_CASE("GS classic conformance: each type's power-on drawing matches the renderer's",
          "[gs-classic-conformance]") {
  const auto results = compare([](const ReferenceState& s) { return s.slot < 0; });
  REQUIRE(results.size() == raw_registry().models().n_types);
  require_within_tolerance(results);
}

TEST_CASE("GS classic conformance: every rendered state matches the renderer's",
          "[.][slow][gs-classic-conformance-all]") {
  const auto results = compare([](const ReferenceState&) { return true; });
  REQUIRE(results.size() == raw_registry().models().n_types);
  std::size_t rendered = 0;
  for (const ReferenceState& state : reference()) rendered += state.status == "rendered";
  std::size_t compared = 0;
  for (const auto& [type, result] : results) compared += result.compared;
  CHECK(compared == rendered);
  require_within_tolerance(results);
}

TEST_CASE("GS classic conformance: the first type draws alike at 44.1 and 48 kHz",
          "[gs-classic-conformance]") {
  require_rate_independent([](std::size_t t) { return t == 0; });
}

TEST_CASE("GS classic conformance: every type draws alike at 44.1 and 48 kHz",
          "[.][slow][gs-classic-conformance-all]") {
  require_rate_independent([](std::size_t) { return true; });
}
