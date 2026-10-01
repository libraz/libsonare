/// @file gs_classic_types_01b_test.cpp
/// @brief GS classic realization, MSB 01 types 01 40-01 73: every printed byte is heard.
///
/// For each type and printed slot, the default configuration (p0 models with overlays) is
/// drawn at 32 kHz from the power-on bytes with that slot at the first and at the last byte
/// of its printed range. The two drawings must differ by at least 0.1 dB in some band of the
/// per-channel third-octave digest, or by a relative L2 of at least 1e-3. The printed
/// slots are the rows of `gs_classic_reference.tsv`, their printed ends the model's, and the
/// stimulus and digest are the ones its header states. The default run draws each type's
/// first printed slot; the full sweep is slow and runs under `[gs-classic-types-01b-all]`.
/// Endpoint pairs are checked first; for a range of more than two bytes whose ends alias, the
/// shared sensitivity helper also checks the nearest accepted midpoint.
///
/// The Lo-Fi Type case holds the 01 73 overlay's hold rates to the 01 72 candidate's
/// ladder, and the 3D azimuth case holds the 01 70 / 01 71 overlays' azimuth sections to
/// `stereo.binaural`'s response: the overlays carry those numbers by hand, so these are what
/// keep the two sources one.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "mastering/stereo/binaural_panner.h"
#include "midi/gs_classic_sensitivity.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_classic/graph_engine.h"
#include "midi/synth/gs_classic/model_registry.h"

namespace sonare::midi::synth::gs_classic {
#define GS_CLASSIC_SET_PREFIX gs_classic_models_raw_01b
#include "midi/gs_classic_models_raw.inc"
#undef GS_CLASSIC_SET_PREFIX
}  // namespace sonare::midi::synth::gs_classic

namespace {

namespace gc = sonare::midi::synth::gs_classic;
using gc::test::gs_classic_require_heard;
using sonare::midi::synth::kGsEfxTypeDefaults;

constexpr uint16_t kFirstType = 0x0140;
constexpr uint16_t kLastType = 0x0173;
constexpr uint16_t k3dAuto = 0x0170;
constexpr uint16_t k3dManual = 0x0171;
constexpr int k3dAzimuthSlot = 0;
constexpr int k3dTurnSlot = 3;
constexpr int k3dOutSlot = 14;
constexpr uint8_t k3dPhones = 1;
constexpr const char* kReferencePath = "tests/midi/gs_classic_reference.tsv";

// Stimulus and digest as the reference TSV header states them.
constexpr std::size_t kHalf = 32000;
constexpr std::size_t kStimulusLength = 2 * kHalf;
constexpr uint32_t kSeedL = 0x12345678u;
constexpr uint32_t kSeedR = 0x9E3779B9u;
constexpr int64_t kSawHz = 220;
constexpr double kStimulusScale = 0.25;
constexpr std::size_t kWindow = 8000;
constexpr int kLowestBand = -13;
constexpr int kHighestBand = 11;
constexpr double kDigestFloorDb = -300.0;
constexpr double kPowerFloor = 1e-30;

// Success condition 3.
constexpr double kHeardDb = 0.1;
constexpr double kHeardRelativeL2 = 1e-3;
/// A band under this in both drawings is below anything the comparison should weigh.
constexpr double kBandFloorDb = -100.0;

constexpr std::size_t kBlock = 4096;

struct Printed {
  uint8_t lo = 0;
  uint8_t hi = 0;
};

/// (type, slot) -> the first and last printed byte, for each printed slot the reference
/// TSV lists. A byte outside the printed range is a protocol question, not the model's.
std::map<std::pair<uint16_t, int>, Printed> printed_slots() {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  std::map<std::pair<uint16_t, int>, Printed> out;
  std::ifstream in(kReferencePath);
  REQUIRE(in.good());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line.rfind("type", 0) == 0) continue;
    std::istringstream row(line);
    std::string type, slot;
    std::getline(row, type, '\t');
    std::getline(row, slot, '\t');
    if (slot == "-") continue;
    const auto number = static_cast<uint16_t>(std::stoi(type.substr(0, 2), nullptr, 16) << 8 |
                                              std::stoi(type.substr(3, 2), nullptr, 16));
    if (number < kFirstType || number > kLastType) continue;
    const gc::GsClassicType* model = registry.find(number);
    REQUIRE(model != nullptr);
    const auto at = static_cast<std::size_t>(std::stoi(slot));
    out.emplace(std::make_pair(number, std::stoi(slot)),
                Printed{model->printed_lo[at], model->printed_hi[at]});
  }
  return out;
}

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

struct Drawing {
  std::vector<double> ch[2];
};

Drawing draw(const gc::GsClassicModelSet& models, const gc::GsClassicType& type,
             const std::array<uint8_t, 20>& bytes) {
  gc::GsClassicGraph graph;
  REQUIRE(graph.prepare(models, type, kBlock, &gc::gs_classic_extension_kernels()));
  graph.reset();
  for (std::size_t slot = 0; slot < bytes.size(); ++slot) graph.set_byte(slot, bytes[slot]);
  Drawing out;
  out.ch[0].resize(kStimulusLength);
  out.ch[1].resize(kStimulusLength);
  graph.process(stimulus(0).data(), stimulus(1).data(), out.ch[0].data(), out.ch[1].data(),
                kStimulusLength);
  return out;
}

/// [channel][window][band] in dB.
std::vector<double> digest(const Drawing& d) {
  sonare::FFT fft(static_cast<int>(kWindow));
  std::vector<float> frame(kWindow);
  std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.n_bins()));
  const double hz_per_bin = 32000.0 / static_cast<double>(kWindow);
  std::vector<double> out;
  for (int c = 0; c < 2; ++c) {
    for (std::size_t start = 0; start + kWindow <= kStimulusLength; start += kWindow) {
      for (std::size_t i = 0; i < kWindow; ++i) frame[i] = static_cast<float>(d.ch[c][start + i]);
      fft.forward(frame.data(), bins.data());
      for (int n = kLowestBand; n <= kHighestBand; ++n) {
        const double centre = 1000.0 * std::pow(2.0, n / 3.0);
        const double lo = centre * std::pow(2.0, -1.0 / 6.0);
        const double hi = centre * std::pow(2.0, 1.0 / 6.0);
        double power = 0.0;
        for (std::size_t k = 0; k < bins.size(); ++k) {
          const double f = static_cast<double>(k) * hz_per_bin;
          if (f < lo || f >= hi) continue;
          const double weight = (k == 0 || k + 1 == bins.size()) ? 1.0 : 2.0;
          power += weight * std::norm(std::complex<double>(bins[k])) /
                   (static_cast<double>(kWindow) * static_cast<double>(kWindow));
        }
        out.push_back(power < kPowerFloor ? kDigestFloorDb : 10.0 * std::log10(power));
      }
    }
  }
  return out;
}

double largest_band_difference(const std::vector<double>& a, const std::vector<double>& b) {
  double worst = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] < kBandFloorDb && b[i] < kBandFloorDb) continue;
    worst = std::max(worst, std::fabs(a[i] - b[i]));
  }
  return worst;
}

double relative_l2(const Drawing& a, const Drawing& b) {
  double diff = 0.0, ea = 0.0, eb = 0.0;
  for (int c = 0; c < 2; ++c) {
    for (std::size_t i = 0; i < kStimulusLength; ++i) {
      const double d = a.ch[c][i] - b.ch[c][i];
      diff += d * d;
      ea += a.ch[c][i] * a.ch[c][i];
      eb += b.ch[c][i] * b.ch[c][i];
    }
  }
  const double scale = std::max(ea, eb);
  return scale > 0.0 ? std::sqrt(diff / scale) : 0.0;
}

std::array<uint8_t, 20> power_on(uint16_t type) {
  for (const auto& entry : kGsEfxTypeDefaults) {
    if (entry.type == type) return entry.params;
  }
  FAIL("no power-on bytes for type " << std::hex << type);
  return {};
}

/// Whether the slot at its two ends changes the default configuration's drawing, with the
/// other bytes at `context`.
bool heard_in(uint16_t type, int slot, const Printed& ends,
              const std::array<uint8_t, 20>& context) {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const gc::GsClassicType* model = registry.find(type);
  REQUIRE(model != nullptr);
  std::array<uint8_t, 20> low = context;
  std::array<uint8_t, 20> high = context;
  low[static_cast<std::size_t>(slot)] = ends.lo;
  high[static_cast<std::size_t>(slot)] = ends.hi;
  const Drawing a = draw(registry.models(), *model, low);
  const Drawing b = draw(registry.models(), *model, high);
  const double band_db = largest_band_difference(digest(a), digest(b));
  const double l2 = relative_l2(a, b);
  UNSCOPED_INFO("largest band difference " << band_db << " dB, relative L2 " << l2);
  return band_db >= kHeardDb || l2 >= kHeardRelativeL2;
}

}  // namespace

TEST_CASE("GS classic types 01 40-01 73: each type's first printed byte is heard",
          "[gs-classic-types-01b]") {
  gs_classic_require_heard(
      printed_slots(), [](bool first_of_type) { return first_of_type; }, power_on, heard_in);
}

TEST_CASE("GS classic types 01 40-01 73: every printed byte is heard",
          "[gs-classic-types-01b-all][.][slow]") {
  gs_classic_require_heard(printed_slots(), [](bool) { return true; }, power_on, heard_in);
}

namespace {

/// Values of a type's nodes that read a byte slot.
std::size_t byte_reads(const gc::GsClassicModelSet& m, const gc::GsClassicType& t) {
  std::size_t n = 0;
  for (uint16_t i = t.node_begin; i < t.node_end; ++i) {
    for (uint8_t k = 0; k < m.nodes[i].value_count; ++k) {
      n += m.values[m.nodes[i].value_begin + k].kind == gc::GsClassicValueKind::kByte ? 1 : 0;
    }
  }
  return n;
}

/// The byte tables every hold node of a type reads its rate through.
std::vector<const gc::GsClassicLut*> hold_rates(const gc::GsClassicModelSet& m,
                                                const gc::GsClassicType& t, uint8_t slot) {
  std::vector<const gc::GsClassicLut*> out;
  for (uint16_t i = t.node_begin; i < t.node_end; ++i) {
    const gc::GsClassicNode& node = m.nodes[i];
    if (node.kind != gc::GsClassicNodeKind::kHold) continue;
    const gc::GsClassicValue& rate = m.values[node.value_begin];
    REQUIRE(rate.kind == gc::GsClassicValueKind::kByte);
    REQUIRE(rate.slot == slot);
    out.push_back(&m.luts[rate.table]);
  }
  return out;
}

const gc::GsClassicModelRegistry& raw_registry() {
  static const gc::GsClassicModelRegistry raw(gc::gs_classic_models_raw_01b());
  return raw;
}

}  // namespace

TEST_CASE("GS classic types 01b: Lo-Fi 2's Type holds at the first Lo-Fi type's ladder",
          "[gs-classic-types-01b]") {
  constexpr uint16_t kLoFi1 = 0x0172;
  constexpr uint16_t kLoFi2 = 0x0173;
  constexpr uint8_t kLoFi1TypeSlot = 1;
  constexpr uint8_t kLoFi2TypeSlot = 0;
  constexpr int kLoFi2States = 6;
  const gc::GsClassicModelRegistry& raw = raw_registry();
  const gc::GsClassicModelRegistry& overlaid = gc::gs_classic_default_registry();
  REQUIRE(raw.valid());
  REQUIRE(overlaid.valid());
  const gc::GsClassicType* ladder_type = raw.find(kLoFi1);
  const gc::GsClassicType* type = overlaid.find(kLoFi2);
  REQUIRE(ladder_type != nullptr);
  REQUIRE(type != nullptr);
  const auto ladder = hold_rates(raw.models(), *ladder_type, kLoFi1TypeSlot);
  REQUIRE(ladder.size() == 1);
  const auto holds = hold_rates(overlaid.models(), *type, kLoFi2TypeSlot);
  // One hold a side.
  REQUIRE(holds.size() == 2);
  for (const gc::GsClassicLut* lut : holds) {
    for (int state = 0; state < kLoFi2States; ++state) {
      INFO("state " << state);
      CHECK(lut->v[state] == ladder.front()->v[state]);
    }
  }
}

TEST_CASE("GS classic types 01b: anchored overlays leave the power-on drawing as p0 draws it",
          "[gs-classic-types-01b]") {
  // Each replaces a constant p0 fitted with a law anchored to it at the power-on byte:
  // Trem Sep's spread weight on 01 41, the loop and all-pass lengths Type scales on 01 55.
  constexpr uint16_t kAnchored[] = {0x0141, 0x0155};
  constexpr double kSameDrawing = 1e-9;
  const gc::GsClassicModelRegistry& raw = raw_registry();
  const gc::GsClassicModelRegistry& overlaid = gc::gs_classic_default_registry();
  REQUIRE(raw.valid());
  REQUIRE(overlaid.valid());
  double worst = 0.0;
  for (uint16_t type : kAnchored) {
    INFO("type " << std::hex << type);
    const gc::GsClassicType* r = raw.find(type);
    const gc::GsClassicType* d = overlaid.find(type);
    REQUIRE(r != nullptr);
    REQUIRE(d != nullptr);
    // Each overlay binds a slot the candidate left unread or read one way only.
    REQUIRE(byte_reads(overlaid.models(), *d) > byte_reads(raw.models(), *r));
    const std::array<uint8_t, 20> bytes = power_on(type);
    const double l2 =
        relative_l2(draw(raw.models(), *r, bytes), draw(overlaid.models(), *d, bytes));
    INFO("relative L2 " << l2);
    CHECK(l2 <= kSameDrawing);
    worst = std::max(worst, l2);
  }
  WARN("largest power-on relative L2 against the raw configuration " << worst);
}

TEST_CASE("GS classic 01 41: wet tremolo preserves p0 at power-on and on dry balance",
          "[gs-classic-types-01b]") {
  constexpr uint16_t kType = 0x0141;
  constexpr std::size_t kBalanceSlot = 0x12 - 0x03;
  constexpr double kSameDrawing = 1e-9;
  const gc::GsClassicModelRegistry& raw = raw_registry();
  const gc::GsClassicModelRegistry& overlaid = gc::gs_classic_default_registry();
  REQUIRE(raw.valid());
  REQUIRE(overlaid.valid());
  const gc::GsClassicType* raw_type = raw.find(kType);
  const gc::GsClassicType* default_type = overlaid.find(kType);
  REQUIRE(raw_type != nullptr);
  REQUIRE(default_type != nullptr);

  auto check_same = [&](const std::array<uint8_t, 20>& bytes, const char* state) {
    INFO(state);
    const double l2 = relative_l2(draw(raw.models(), *raw_type, bytes),
                                  draw(overlaid.models(), *default_type, bytes));
    CHECK(l2 <= kSameDrawing);
  };

  const std::array<uint8_t, 20> power = power_on(kType);
  check_same(power, "power-on");
  auto dry = power;
  dry[kBalanceSlot] = 0;
  check_same(dry, "dry balance");
}

namespace {

namespace st = sonare::mastering::stereo;

// Placement: 31 positions 12 degrees apart; byte 4 * (position + 16) - 2 names position.
constexpr int kLastPosition = 15;
constexpr double kDegreesPerPosition = 12.0;
constexpr int kLowestAzimuthCentre = -13;
constexpr int kHighestAzimuthCentre = 10;  // 24 third-octave centres, 49.6 Hz .. 10.1 kHz
constexpr double kPannerRate = 48000.0;
constexpr std::size_t kSettle = 4800;  // past both glides (20 ms) at either rate
constexpr std::size_t kResponse = 16384;
constexpr int kBandFft = 65536;

/// Mean power over each third-octave band of @p ir at @p rate, in dB.
std::vector<double> band_db(const std::vector<double>& ir, double rate) {
  sonare::FFT fft(kBandFft);
  std::vector<float> frame(static_cast<std::size_t>(kBandFft), 0.0f);
  for (std::size_t i = 0; i < ir.size() && i < frame.size(); ++i) {
    frame[i] = static_cast<float>(ir[i]);
  }
  std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.n_bins()));
  fft.forward(frame.data(), bins.data());
  const double hz_per_bin = rate / static_cast<double>(kBandFft);
  std::vector<double> out;
  for (int n = kLowestAzimuthCentre; n <= kHighestAzimuthCentre; ++n) {
    const double centre = 1000.0 * std::pow(2.0, n / 3.0);
    const double lo = centre * std::pow(2.0, -1.0 / 6.0);
    const double hi = centre * std::pow(2.0, 1.0 / 6.0);
    double power = 0.0;
    int count = 0;
    for (std::size_t k = 0; k < bins.size(); ++k) {
      const double f = static_cast<double>(k) * hz_per_bin;
      if (f < lo || f >= hi) continue;
      power += std::norm(std::complex<double>(bins[k]));
      ++count;
    }
    REQUIRE(count > 0);
    out.push_back(10.0 * std::log10(power / count));
  }
  return out;
}

/// Per-ear band response of the classic graph at @p position, phones, turning off.
std::array<std::vector<double>, 2> classic_response(const gc::GsClassicType& type, int position) {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  std::array<uint8_t, 20> bytes = power_on(type.type);
  bytes[k3dAzimuthSlot] = static_cast<uint8_t>(4 * (position + 16) - 2);
  if (type.type == k3dAuto) bytes[k3dTurnSlot] = 0;
  bytes[k3dOutSlot] = k3dPhones;
  gc::GsClassicGraph graph;
  REQUIRE(graph.prepare(registry.models(), type, kBlock, &gc::gs_classic_extension_kernels()));
  graph.reset();
  for (std::size_t slot = 0; slot < bytes.size(); ++slot) graph.set_byte(slot, bytes[slot]);
  const std::size_t n = kSettle + kResponse;
  std::vector<double> in(n, 0.0);
  in[kSettle] = 1.0;
  std::vector<double> l(n), r(n);
  graph.process(in.data(), in.data(), l.data(), r.data(), n);
  return {band_db({l.begin() + kSettle, l.end()}, gc::kGsClassicSampleRateHz),
          band_db({r.begin() + kSettle, r.end()}, gc::kGsClassicSampleRateHz)};
}

/// Per-ear band response of `stereo.binaural` at @p degrees, phones.
std::array<std::vector<double>, 2> panner_response(double degrees) {
  st::BinauralPannerConfig config;
  config.azimuth_deg = static_cast<float>(degrees);
  st::BinauralPanner panner(config);
  const std::size_t n = kSettle + kResponse;
  panner.prepare(kPannerRate, static_cast<int>(n));
  std::vector<float> l(n, 0.0f), r(n, 0.0f);
  l[kSettle] = 1.0f;
  r[kSettle] = 1.0f;
  float* planes[2] = {l.data(), r.data()};
  panner.process(planes, 2, static_cast<int>(n));
  return {band_db({l.begin() + kSettle, l.end()}, kPannerRate),
          band_db({r.begin() + kSettle, r.end()}, kPannerRate)};
}

}  // namespace

TEST_CASE("GS classic types 01b: the 3D azimuth follows stereo.binaural's ring",
          "[gs-classic-types-01b]") {
  // Per ear and written position, the third-octave response relative to the front: the
  // overlay's sections are fitted so the classic head's change matches the ring's.
  constexpr double kRmsToleranceDb = 0.75;
  constexpr double kMaxToleranceDb = 2.0;
  constexpr double kShapedDb = 6.0;
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const auto panner_front = panner_response(0.0);
  double worst_rms = 0.0;
  double worst_max = 0.0;
  double largest_change = 0.0;
  for (uint16_t number : {k3dAuto, k3dManual}) {
    const gc::GsClassicType* type = registry.find(number);
    REQUIRE(type != nullptr);
    const auto classic_front = classic_response(*type, 0);
    for (int position = -kLastPosition; position <= kLastPosition; ++position) {
      const double degrees = kDegreesPerPosition * position;
      const auto classic = classic_response(*type, position);
      const auto panner = panner_response(degrees);
      for (int ear = 0; ear < 2; ++ear) {
        double sum = 0.0;
        double most = 0.0;
        const std::size_t bands = classic[ear].size();
        for (std::size_t b = 0; b < bands; ++b) {
          const double want = panner[ear][b] - panner_front[ear][b];
          const double got = classic[ear][b] - classic_front[ear][b];
          sum += (got - want) * (got - want);
          most = std::max(most, std::fabs(got - want));
          largest_change = std::max(largest_change, std::fabs(want));
        }
        const double rms = std::sqrt(sum / static_cast<double>(bands));
        INFO("type " << std::hex << number << std::dec << " " << degrees << " degrees, ear " << ear
                     << ": rms " << rms << " dB, largest " << most << " dB");
        CHECK(rms <= kRmsToleranceDb);
        CHECK(most <= kMaxToleranceDb);
        worst_rms = std::max(worst_rms, rms);
        worst_max = std::max(worst_max, most);
      }
    }
  }
  // The comparison is only worth something against a response that moves with azimuth.
  CHECK(largest_change > kShapedDb);
  WARN("3D azimuth against stereo.binaural: worst rms " << worst_rms << " dB, worst band "
                                                        << worst_max << " dB");
}
