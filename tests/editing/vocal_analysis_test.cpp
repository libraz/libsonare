#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "editing/vocal_edit/session.h"
#include "util/constants.h"

namespace {

using sonare::editing::vocal_edit::VocalEditSession;
using sonare::editing::vocal_edit::VocalNote;
using sonare::editing::vocal_edit::VocalSessionCreateOptions;

constexpr int kRate = 44100;
constexpr double kLeadSeconds = 0.15;

struct Vibrato {
  double rate_hz = 5.5;
  double depth_cents = 25.0;
  double fade_seconds = 0.2;  // 0 starts at full depth
  double phase = 0.0;         // pi/2 starts on the peak
};

struct Sung {
  double cents;  // MIDI * 100
  double seconds;
};

// A sung phrase: eight-harmonic tone following a per-note pitch, linear glides centred on each
// note change, per-note vibrato, 20 ms edge ramps, kLeadSeconds of silence either side.
std::vector<float> sing(const std::vector<Sung>& notes, double glide_seconds, Vibrato vibrato) {
  double total = 2.0 * kLeadSeconds;
  for (const auto& note : notes) total += note.seconds;
  const std::size_t n = static_cast<std::size_t>(total * kRate);
  std::vector<double> cents(n, notes.front().cents);
  std::vector<double> vib(n, 0.0);
  std::vector<std::size_t> starts;
  double position = kLeadSeconds;
  for (const auto& note : notes) {
    const auto a = static_cast<std::size_t>(position * kRate);
    const auto b = static_cast<std::size_t>((position + note.seconds) * kRate);
    starts.push_back(a);
    for (std::size_t i = a; i < b; ++i) {
      cents[i] = note.cents;
      const double t = static_cast<double>(i - a) / kRate;
      const double grown =
          vibrato.fade_seconds > 0.0 ? std::min(t / vibrato.fade_seconds, 1.0) : 1.0;
      vib[i] = vibrato.depth_cents * grown *
               std::sin(sonare::constants::kTwoPiD * vibrato.rate_hz * t + vibrato.phase);
    }
    position += note.seconds;
  }
  const auto half = static_cast<std::size_t>(glide_seconds * kRate / 2);
  for (std::size_t k = 1; k < starts.size(); ++k) {
    const std::size_t s = starts[k];
    const double from = cents[s - half - 1];
    const double to = cents[s + half];
    for (std::size_t i = s - half; i < s + half; ++i) {
      cents[i] =
          from + (to - from) * static_cast<double>(i - (s - half)) / static_cast<double>(2 * half);
    }
  }
  const auto first = starts.front();
  const auto last = static_cast<std::size_t>(position * kRate);
  std::vector<float> out(n, 0.0f);
  double phase = 0.0;
  float peak = 0.0f;
  const auto ramp = static_cast<std::size_t>(0.02 * kRate);
  for (std::size_t i = first; i < last; ++i) {
    const double midi = (cents[i] + vib[i]) / 100.0;
    const double f0 = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
    phase += sonare::constants::kTwoPiD * f0 / kRate;
    double sample = 0.0;
    for (int h = 1; h <= 8; ++h) sample += std::sin(h * phase) / h;
    double envelope = 1.0;
    if (i - first < ramp) envelope = static_cast<double>(i - first) / ramp;
    if (last - i <= ramp) envelope = static_cast<double>(last - i) / ramp;
    out[i] = static_cast<float>(sample * envelope);
    peak = std::max(peak, std::abs(out[i]));
  }
  for (float& sample : out) sample *= 0.5f / peak;
  return out;
}

std::vector<VocalNote> notes_of(const std::vector<float>& samples,
                                VocalSessionCreateOptions options = {}) {
  return VocalEditSession::create(sonare::Audio::from_vector(samples, kRate), options).notes();
}

double start_seconds(const VocalNote& note) {
  return static_cast<double>(note.source_range.start) / kRate;
}

double cents_from(const VocalNote& note, double midi) { return 100.0 * (note.centre_midi - midi); }

std::string describe(const std::vector<VocalNote>& notes) {
  std::ostringstream out;
  for (const auto& note : notes) {
    out << " [" << start_seconds(note) << "-" << static_cast<double>(note.source_range.end) / kRate
        << " s, centre " << note.centre_midi << "]";
  }
  return out.str();
}

}  // namespace

TEST_CASE("vocal note centre reads an off-grid tone to within a cent", "[vocal_analysis]") {
  // pYIN bins are 10 cents wide and sit 0.79 cents below the A440 grid, so a median of bin
  // centres read these offsets as -0.79, +2.21, +6.21, -0.35 and +6.21 cents.
  for (const double offset : {0.0, 7.0, 13.0, 19.56, 33.0}) {
    const auto notes = notes_of(sing({{6900.0 + offset, 1.0}}, 0.0, Vibrato{5.5, 0.0, 0.0}));
    REQUIRE(notes.size() == 1);
    REQUIRE(notes[0].has_pitch);
    INFO("offset " << offset << " cents read as " << cents_from(notes[0], 69.0));
    CHECK(cents_from(notes[0], 69.0) == Catch::Approx(offset).margin(1.0));
  }
}

TEST_CASE("a vibrato that develops after the onset does not split the note", "[vocal_analysis]") {
  // The take that found this: G4+20 to A4-15 over a 60 ms glide, the vibrato reaching its full
  // 25 cents 200 ms into the A, which a single mid-glide reference frame cut in two there.
  const auto notes = sing({{6720.0, 0.5}, {6885.0, 0.6}}, 0.06, Vibrato{5.5, 25.0, 0.2});
  const auto result = notes_of(notes);
  INFO("notes:" << describe(result));
  REQUIRE(result.size() == 2);
  CHECK(start_seconds(result[1]) == Catch::Approx(kLeadSeconds + 0.5).margin(0.04));
  CHECK(static_cast<double>(result[1].source_range.end) / kRate > kLeadSeconds + 0.5 + 0.6 - 0.05);
  CHECK(cents_from(result[0], 67.0) == Catch::Approx(20.0).margin(1.5));
  CHECK(cents_from(result[1], 69.0) == Catch::Approx(-15.0).margin(1.5));
}

TEST_CASE("a held note stays one note across vibrato rates, depths and starting phases",
          "[vocal_analysis]") {
  // A vibrato starting on its peak put the old reference frame 25 cents off centre.
  const Vibrato cases[] = {{4.0, 45.0, 0.2, 0.0},
                           {7.0, 35.0, 0.0, 0.0},
                           {5.5, 25.0, 0.0, sonare::constants::kPiD / 2},
                           {4.0, 25.0, 0.0, 0.0}};
  for (const auto& vibrato : cases) {
    const auto notes = notes_of(sing({{6180.0, 1.5}}, 0.0, vibrato));
    INFO("vibrato " << vibrato.rate_hz << " Hz +-" << vibrato.depth_cents << " cents, phase "
                    << vibrato.phase);
    REQUIRE(notes.size() == 1);
    CHECK(cents_from(notes[0], 62.0) == Catch::Approx(-20.0).margin(1.0));
  }
}

TEST_CASE("the glide into a note belongs to it", "[vocal_analysis]") {
  // Without vibrato the mid-glide reference frame left a 30-50 ms glide fragment as a third note.
  for (const double glide : {0.06, 0.1}) {
    const auto notes =
        notes_of(sing({{6000.0, 0.7}, {6200.0, 0.7}}, glide, Vibrato{5.5, 0.0, 0.0}));
    INFO("glide " << glide * 1000 << " ms");
    REQUIRE(notes.size() == 2);
    CHECK(start_seconds(notes[1]) == Catch::Approx(kLeadSeconds + 0.7).margin(glide));
    CHECK(cents_from(notes[1], 62.0) == Catch::Approx(0.0).margin(1.0));
  }
}

TEST_CASE("adjacent notes a semitone apart still separate under vibrato", "[vocal_analysis]") {
  const auto notes = notes_of(sing({{6000.0, 0.6}, {6100.0, 0.6}}, 0.06, Vibrato{5.5, 25.0, 0.2}));
  REQUIRE(notes.size() == 2);
  CHECK(start_seconds(notes[1]) == Catch::Approx(kLeadSeconds + 0.6).margin(0.05));
  CHECK(cents_from(notes[0], 60.0) == Catch::Approx(0.0).margin(2.0));
  CHECK(cents_from(notes[1], 61.0) == Catch::Approx(0.0).margin(2.0));
}

TEST_CASE("a step below the segmentation threshold is one note until the threshold is lowered",
          "[vocal_analysis]") {
  const auto samples = sing({{6000.0, 0.6}, {6045.0, 0.6}}, 0.06, Vibrato{5.5, 0.0, 0.0});
  CHECK(notes_of(samples).size() == 1);
  VocalSessionCreateOptions options;
  options.analysis_options.segmentation_threshold_cents = 40.0;
  const auto notes = notes_of(samples, options);
  REQUIRE(notes.size() == 2);
  CHECK(cents_from(notes[1], 60.0) == Catch::Approx(45.0).margin(1.0));
}
