/// @file decide_eq_test.cpp
/// @brief Contract of the mixing assistant's static corrective EQ stage.

#include "mixing/assistant/decide_eq.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "util/constants.h"
#include "util/json.h"

using Catch::Matchers::WithinAbs;
using sonare::mastering::api::insert_factory_names;
using sonare::mixing::api::Insert;
using sonare::mixing::api::InsertSlot;
using sonare::mixing::assistant::analyze_track_profile;
using sonare::mixing::assistant::BandDominance;
using sonare::mixing::assistant::decide_eq;
using sonare::mixing::assistant::DeltaDomain;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::MeanPowerSpectrum;
using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::MixProfile;
using sonare::mixing::assistant::SceneDelta;
using sonare::mixing::assistant::SourceClass;
using sonare::mixing::assistant::TrackInput;
using sonare::mixing::assistant::TrackProfile;

namespace {

// The two processor names the stage may suggest. Spelled out here rather than
// reused from the module so a rename has to be made deliberately in both.
const std::string kParametric = "eq.parametric";
const std::string kCutFilter = "eq.cutFilter";

// Analysis band indices, matching kBands. The mid pair is what most cases use:
// both sit above every high-pass corner in the module's table, so a cut there
// is never suppressed as redundant.
constexpr int kSubBand = 0;
constexpr int kMidBand = 3;
constexpr int kHighMidBand = 4;
constexpr int kHighBand = 5;
constexpr int kAirBand = 6;

// The mid band's span and geometric centre, mirrored from kBands so a cut that
// stops landing where the module says it should fails here.
constexpr float kMidBandLowHz = 500.0f;
constexpr float kMidBandHighHz = 2000.0f;
constexpr float kMidBandCenterHz = 1000.0f;

// Comfortably above the classifier's acceptance floor, so confidence is never
// the reason a hand-built profile is skipped.
constexpr float kHighConfidence = 0.9f;
// Classified, but not confidently enough to carve a part on.
constexpr float kLowConfidence = 0.2f;

// Longer than one loudness gating block, matching the other assistant tests.
constexpr float kMeasurableDurationSec = 2.0f;

// Overlap long enough to read as a standing conflict rather than a coincidence.
constexpr int kOverlapFrames = 256;
// A handful of frames where the two happened to sound together.
constexpr int kBriefOverlapFrames = 4;

// Equal band energy: neither track is in the other's way.
constexpr float kEvenShare = 0.5f;
// Past the interference threshold, but not far enough to reach the ceiling.
constexpr float kContestedShare = 0.75f;
// Effectively one track owning the band outright.
constexpr float kBuriedShare = 0.99f;

// The default ceiling, mirrored so the expectations read as numbers rather than
// as config lookups.
constexpr float kDefaultMaxCutDb = 4.0f;
// dB values assembled from float arithmetic; a thousandth of a dB is far below
// anything audible.
constexpr float kDbTolerance = 1e-3f;
// Frequencies are written into JSON as doubles and read back; a tenth of a Hz
// is far below the resolution any of these corners are specified at.
constexpr float kHzTolerance = 0.1f;

TrackProfile make_profile(const std::string& id, SourceClass source) {
  TrackProfile profile;
  profile.strip_id = id;
  profile.name = id;
  profile.source = source;
  profile.source_confidence = kHighConfidence;
  profile.base.duration_sec = kMeasurableDurationSec;
  profile.duration_sec = kMeasurableDurationSec;
  profile.band_occupancy.fill(1.0f / static_cast<float>(kBandCount));
  profile.usable = true;
  return profile;
}

// Residue shares the high-pass decision is measured against: one comfortably
// inside the window the filter is proposed in, and one on either side of it.
constexpr float kResidueShare = 0.02f;
constexpr float kOwnMaterialShare = 0.30f;
constexpr float kNothingBelowShare = 0.0f;

// STFT geometry the hand-built spectra are written against. Bins land every
// 23.4375 Hz, so bin 1 spans 11.7 Hz to 35.2 Hz and bin 20 spans 457 Hz to
// 480 Hz.
constexpr int kSpectrumNFft = 2048;
constexpr int kSpectrumSampleRate = 48000;
constexpr std::size_t kLowResidueBin = 1;
constexpr std::size_t kProgramBin = 20;

// One bin's worth of power, for a spectrum assembled bin by bin.
struct SpectrumBin {
  std::size_t bin = 0;
  float power = 0.0f;
};

// Frequency at the centre of @p bin under the geometry above.
float bin_hz(std::size_t bin) {
  return static_cast<float>(bin) * static_cast<float>(kSpectrumSampleRate) /
         static_cast<float>(kSpectrumNFft);
}

// Gives @p profile a spectrum holding exactly the listed bins and nothing
// anywhere else, so the frequency a case expects is known by construction rather
// than discovered by running the code under test.
void set_spectrum(TrackProfile& profile, const std::vector<SpectrumBin>& bins,
                  int sample_rate = kSpectrumSampleRate) {
  MeanPowerSpectrum spectrum;
  spectrum.n_bins = kSpectrumNFft / 2 + 1;
  spectrum.n_fft = kSpectrumNFft;
  spectrum.sample_rate = sample_rate;
  spectrum.power.assign(static_cast<std::size_t>(spectrum.n_bins), 0.0f);
  for (const SpectrumBin& entry : bins) {
    REQUIRE(entry.bin < spectrum.power.size());
    spectrum.power[entry.bin] = entry.power;
  }
  profile.spectrum = spectrum;
}

// A flat run of bins, for building a hump wide enough to survive smoothing.
std::vector<SpectrumBin> flat_run(std::size_t first, std::size_t last, float power) {
  std::vector<SpectrumBin> bins;
  for (std::size_t bin = first; bin <= last; ++bin) bins.push_back({bin, power});
  return bins;
}

std::vector<SpectrumBin> operator+(std::vector<SpectrumBin> left,
                                   const std::vector<SpectrumBin>& right) {
  left.insert(left.end(), right.begin(), right.end());
  return left;
}

// Reads back the centre frequency of the only band in a parametric insert.
float only_cut_center_hz(const Insert& insert) {
  const sonare::util::json::Value params = sonare::util::json::parse(insert.params_json);
  REQUIRE(params.contains("band0.frequencyHz"));
  REQUIRE_FALSE(params.contains("band1.frequencyHz"));
  return params["band0.frequencyHz"].as_float();
}

// Gives @p profile a spectrum with @p share of its energy below every high-pass
// corner in the module's table and the rest well above the highest of them. The
// corners span 50 Hz to 400 Hz, and both bins used here sit clear of that range,
// so the measured share is the one the test asked for whichever class the track
// is classified as.
void set_low_energy_share(TrackProfile& profile, float share) {
  MeanPowerSpectrum spectrum;
  spectrum.n_bins = kSpectrumNFft / 2 + 1;
  spectrum.n_fft = kSpectrumNFft;
  spectrum.sample_rate = kSpectrumSampleRate;
  spectrum.power.assign(static_cast<std::size_t>(spectrum.n_bins), 0.0f);
  spectrum.power[kLowResidueBin] = share;
  spectrum.power[kProgramBin] = 1.0f - share;
  profile.spectrum = spectrum;
}

// The high-pass switch is off by default, so a case about the filter has to ask
// for it explicitly.
MixAssistantConfig high_pass_on() {
  MixAssistantConfig config;
  config.enable_high_pass = true;
  return config;
}

// Concentrates @p share of the track's energy in @p band and spreads the rest
// evenly, so the occupancy still sums to 1 the way a measured profile does.
void set_band_occupancy(TrackProfile& profile, int band, float share) {
  const float rest = (1.0f - share) / static_cast<float>(kBandCount - 1);
  profile.band_occupancy.fill(rest);
  profile.band_occupancy[static_cast<std::size_t>(band)] = share;
}

// The same, for several bands at once.
void set_band_occupancy(TrackProfile& profile, const std::vector<int>& bands, float share) {
  const float rest = (1.0f - share * static_cast<float>(bands.size())) /
                     static_cast<float>(kBandCount - static_cast<int>(bands.size()));
  profile.band_occupancy.fill(rest);
  for (int band : bands) profile.band_occupancy[static_cast<std::size_t>(band)] = share;
}

// Shares read against the module's essentiality thresholds. An even spread over
// the seven bands is 0.143, and the module calls a band essential at 0.40 and
// disposable at 0.07.
//
// Well past the essential threshold: the band is what the part is built around.
constexpr float kEssentialShare = 0.45f;
// Well under the disposable one: content is there, but the part does not need it.
constexpr float kSpareShare = 0.02f;
// Inside the module's deliberate gap, so the band is neither.
constexpr float kAmbiguousShare = 0.20f;

// Opens the essentiality gate for @p bands: the part being made room for is
// built around them and the part giving way is not. A collision alone no longer
// earns a cut, so every case that expects one has to arrange this.
void gate_open(TrackProfile& needs_it, TrackProfile& can_spare_it, const std::vector<int>& bands) {
  set_band_occupancy(needs_it, bands, kEssentialShare);
  set_band_occupancy(can_spare_it, bands, kSpareShare);
}

MixProfile make_mix(int track_count) {
  MixProfile mix;
  mix.track_count = track_count;
  mix.dominance.assign(static_cast<std::size_t>(track_count) *
                           static_cast<std::size_t>(track_count) *
                           static_cast<std::size_t>(kBandCount),
                       BandDominance{});
  return mix;
}

void set_dominance(MixProfile& mix, int masker, int maskee, int band, float ratio, int frames) {
  const std::size_t index =
      (static_cast<std::size_t>(masker) * static_cast<std::size_t>(mix.track_count) +
       static_cast<std::size_t>(maskee)) *
          static_cast<std::size_t>(kBandCount) +
      static_cast<std::size_t>(band);
  BandDominance entry;
  entry.ratio = ratio;
  entry.valid_frames = frames;
  mix.dominance[index] = entry;
  // The measure is a share of the pair's energy, so the opposite direction is
  // what is left over. Filling it keeps a hand-built matrix consistent with
  // what the masking pass would have produced.
  const std::size_t mirror =
      (static_cast<std::size_t>(maskee) * static_cast<std::size_t>(mix.track_count) +
       static_cast<std::size_t>(masker)) *
          static_cast<std::size_t>(kBandCount) +
      static_cast<std::size_t>(band);
  BandDominance opposite;
  opposite.ratio = 1.0f - ratio;
  opposite.valid_frames = frames;
  mix.dominance[mirror] = opposite;
}

const SceneDelta* find_delta(const std::vector<SceneDelta>& deltas, const std::string& strip_id,
                             const std::string& processor_name) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id != strip_id) continue;
    for (const Insert& insert : delta.inserts) {
      if (insert.processor_name == processor_name) return &delta;
    }
  }
  return nullptr;
}

const Insert* find_insert(const std::vector<SceneDelta>& deltas, const std::string& strip_id,
                          const std::string& processor_name) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id != strip_id) continue;
    for (const Insert& insert : delta.inserts) {
      if (insert.processor_name == processor_name) return &insert;
    }
  }
  return nullptr;
}

int count_inserts(const std::vector<SceneDelta>& deltas, const std::string& processor_name) {
  int count = 0;
  for (const SceneDelta& delta : deltas) {
    for (const Insert& insert : delta.inserts) {
      if (insert.processor_name == processor_name) ++count;
    }
  }
  return count;
}

// Centre frequency of the single peaking cut the stage put on "pad", and the
// reason that carried it. Both hold the deltas in a named local: find_insert and
// find_delta return pointers into the vector, so reading through one that came
// straight out of a call expression would be reading a destroyed temporary.
float pad_cut_center_hz(const std::vector<TrackProfile>& profiles, const MixProfile& mix) {
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});
  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  return only_cut_center_hz(*insert);
}

std::string pad_cut_reason(const std::vector<TrackProfile>& profiles, const MixProfile& mix) {
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});
  const SceneDelta* delta = find_delta(deltas, "pad", kParametric);
  REQUIRE(delta != nullptr);
  return delta->reason;
}

// "band12.gainDb" splits into 12 and "gainDb"; anything else is not a key the
// parametric EQ reads.
bool split_band_key(const std::string& key, int* out_index, std::string* out_field) {
  if (key.rfind("band", 0) != 0) return false;
  std::size_t cursor = 4;
  int index = 0;
  bool any_digit = false;
  while (cursor < key.size() && std::isdigit(static_cast<unsigned char>(key[cursor])) != 0) {
    index = index * 10 + (key[cursor] - '0');
    any_digit = true;
    ++cursor;
  }
  if (!any_digit) return false;
  if (cursor >= key.size() || key[cursor] != '.') return false;
  if (cursor + 1 >= key.size()) return false;
  *out_index = index;
  *out_field = key.substr(cursor + 1);
  return true;
}

sonare::util::json::Value parse_params(const Insert& insert) {
  return sonare::util::json::parse(insert.params_json);
}

std::vector<TrackProfile> two_tracks() {
  return {make_profile("vox", SourceClass::Vocal), make_profile("pad", SourceClass::Strings)};
}

// The arrangement most cases use: a vocal and a pad, with the vocal built around
// @p bands and the pad able to give them up. The pad has the lower role priority,
// so it is the one that gives way.
std::vector<TrackProfile> two_tracks_contesting(const std::vector<int>& bands) {
  std::vector<TrackProfile> profiles = two_tracks();
  gate_open(profiles[0], profiles[1], bands);
  return profiles;
}

// Frequency the two profiled tracks below actually share, well away from the mid
// band's 1000 Hz centre so a stage that still used the grid cannot pass.
constexpr float kSharedToneHz = 1800.0f;

// Two genuinely profiled tracks that collide at kSharedToneHz inside the mid
// band, each with its own material elsewhere. Profiled once for the whole file:
// an STFT per case pays repeatedly to measure the same thing.
//
// The two are built so the measured occupancy opens the essentiality gate by
// itself: the mid band is nearly all of the vocal, while the pad merely touches
// it and lives up in the high mid. Overriding the occupancy afterwards would
// have made the case prove less than it looks like it proves.
//
// Hand-built spectra prove what the stage does with a measurement; only a real
// profile proves the stage reads the one the profiler actually produced.
const std::vector<TrackProfile>& profiled_collision_pair() {
  static const std::vector<TrackProfile> profiles = [] {
    constexpr float kDurationSec = 0.6f;
    // Below the mid band for one track and above it for the other, so the only
    // thing they share inside the band is the tone.
    constexpr float kVoxOwnHz = 400.0f;
    constexpr float kPadOwnHz = 5000.0f;

    constexpr std::size_t kFrames =
        static_cast<std::size_t>(kDurationSec * static_cast<float>(kSpectrumSampleRate));
    const auto render = [](float shared_amplitude, float own_hz, float own_amplitude) {
      std::vector<float> samples(kFrames, 0.0f);
      for (std::size_t index = 0; index < kFrames; ++index) {
        const float seconds = static_cast<float>(index) / static_cast<float>(kSpectrumSampleRate);
        samples[index] =
            shared_amplitude * std::sin(sonare::constants::kTwoPi * kSharedToneHz * seconds) +
            own_amplitude * std::sin(sonare::constants::kTwoPi * own_hz * seconds);
      }
      return samples;
    };

    const std::vector<float> vox_samples = render(0.3f, kVoxOwnHz, 0.1f);
    const std::vector<float> pad_samples = render(0.05f, kPadOwnHz, 0.4f);

    const auto profile_of = [](const std::string& id, const std::vector<float>& samples,
                               SourceClass source) {
      TrackInput input;
      input.id = id;
      input.left = samples.data();
      input.frame_count = samples.size();
      input.sample_rate = kSpectrumSampleRate;
      TrackProfile profile = analyze_track_profile(input);
      // The classifier is not what these cases are about; only the spectrum has
      // to be the profiler's own.
      profile.source = source;
      profile.source_confidence = kHighConfidence;
      return profile;
    };

    return std::vector<TrackProfile>{profile_of("vox", vox_samples, SourceClass::Vocal),
                                     profile_of("pad", pad_samples, SourceClass::Strings)};
  }();
  return profiles;
}

// The same pair, each carrying residue under its corner, for the cases that need
// the high-pass to actually be proposed.
std::vector<TrackProfile> two_tracks_with_residue(const std::vector<int>& bands = {}) {
  std::vector<TrackProfile> profiles = bands.empty() ? two_tracks() : two_tracks_contesting(bands);
  for (TrackProfile& profile : profiles) set_low_energy_share(profile, kResidueShare);
  return profiles;
}

}  // namespace

TEST_CASE("eq suggests no cut when no band is contested", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  for (int band = 0; band < kBandCount; ++band) {
    set_dominance(mix, 0, 1, band, kEvenShare, kOverlapFrames);
  }

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(count_inserts(deltas, kParametric) == 0);
  for (const SceneDelta& delta : deltas) {
    CHECK(delta.domain == DeltaDomain::Eq);
  }
}

TEST_CASE("eq ignores a collision the two tracks barely shared", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  // One track owns the band outright, but only for a handful of frames, which
  // is a coincidence rather than a standing conflict.
  set_dominance(mix, 1, 0, kMidBand, kBuriedShare, kBriefOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(count_inserts(deltas, kParametric) == 0);
}

TEST_CASE("eq carves the lower-priority track, not the quieter one", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
  MixProfile mix = make_mix(2);
  // The pad is the one burying the vocal, so a rule that attenuated whichever
  // track lost the energy contest would carve the vocal here.
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(find_insert(deltas, "pad", kParametric) != nullptr);
  CHECK(find_insert(deltas, "vox", kParametric) == nullptr);
  CHECK(count_inserts(deltas, kParametric) == 1);
}

TEST_CASE("eq breaks a priority tie on band occupancy", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("gtrL", SourceClass::Guitar),
                                     make_profile("gtrR", SourceClass::Guitar)};
  // The dominant track is the one barely invested in the band, so the band
  // belongs to its neighbour and the dominant track is what gives way.
  // gtrL is the one barely invested in the band, so it both loses the tie-break
  // and is the one that can give the band up.
  set_band_occupancy(profiles[0], kMidBand, kSpareShare);
  set_band_occupancy(profiles[1], kMidBand, kEssentialShare);

  MixProfile mix = make_mix(2);
  set_dominance(mix, 0, 1, kMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(find_insert(deltas, "gtrL", kParametric) != nullptr);
  CHECK(find_insert(deltas, "gtrR", kParametric) == nullptr);
}

TEST_CASE("eq never cuts deeper than the configured ceiling", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kBuriedShare, kOverlapFrames);

  MixAssistantConfig config;
  config.eq_max_cut_db = kDefaultMaxCutDb;
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, config);

  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.contains("band0.gainDb"));
  CHECK_THAT(params["band0.gainDb"].as_float(), WithinAbs(-kDefaultMaxCutDb, kDbTolerance));

  const SceneDelta* delta = find_delta(deltas, "pad", kParametric);
  REQUIRE(delta != nullptr);
  CHECK(delta->reason.find("ceiling") != std::string::npos);
}

TEST_CASE("eq reports the ceiling only when it actually bit", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  MixAssistantConfig config;
  config.eq_max_cut_db = kDefaultMaxCutDb;
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, config);

  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.contains("band0.gainDb"));
  const float gain_db = params["band0.gainDb"].as_float();
  CHECK(gain_db < 0.0f);
  CHECK(gain_db > -kDefaultMaxCutDb);

  const SceneDelta* delta = find_delta(deltas, "pad", kParametric);
  REQUIRE(delta != nullptr);
  CHECK(delta->reason.find("ceiling") == std::string::npos);
}

TEST_CASE("eq raises the cut as the collision worsens, up to the ceiling", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});

  const auto cut_for = [&profiles](float share) {
    MixProfile mix = make_mix(2);
    set_dominance(mix, 1, 0, kMidBand, share, kOverlapFrames);
    const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});
    const Insert* insert = find_insert(deltas, "pad", kParametric);
    REQUIRE(insert != nullptr);
    const sonare::util::json::Value params = parse_params(*insert);
    return -params["band0.gainDb"].as_float();
  };

  CHECK(cut_for(0.80f) > cut_for(0.70f));
  CHECK(cut_for(0.80f) <= kDefaultMaxCutDb);
}

TEST_CASE("eq writes params the parametric equalizer can read", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand, kHighMidBand});
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);
  set_dominance(mix, 1, 0, kHighMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.is_object());

  std::set<int> band_indices;
  for (const auto& [key, value] : params.as_object()) {
    int index = 0;
    std::string field;
    INFO("param key " << key);
    REQUIRE(split_band_key(key, &index, &field));
    band_indices.insert(index);
    // The insert factory's strict parse accepts nothing else.
    CHECK((value.is_number() || value.is_bool()));
  }

  // Bands are numbered contiguously from zero.
  REQUIRE(band_indices.size() == 2);
  CHECK(band_indices.count(0) == 1);
  CHECK(band_indices.count(1) == 1);
}

TEST_CASE("eq suggests only processors the insert factory can build", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks_with_residue({kMidBand});
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, high_pass_on());
  REQUIRE_FALSE(deltas.empty());

  const std::vector<std::string> known = insert_factory_names();
  int inspected = 0;
  for (const SceneDelta& delta : deltas) {
    for (const Insert& insert : delta.inserts) {
      INFO("processor " << insert.processor_name);
      CHECK(std::find(known.begin(), known.end(), insert.processor_name) != known.end());
      ++inspected;
    }
  }
  CHECK(inspected > 0);
  // Both names the stage may emit are exercised by this arrangement.
  CHECK(count_inserts(deltas, kParametric) == 1);
  CHECK(count_inserts(deltas, kCutFilter) == 2);
}

TEST_CASE("eq folds every contested band into one parametric insert", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand, kHighMidBand});
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);
  set_dominance(mix, 1, 0, kHighMidBand, kBuriedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  // A strip drops a second copy of the same processor in the same slot, so two
  // inserts would silently throw one of the two cuts away.
  CHECK(count_inserts(deltas, kParametric) == 1);

  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  CHECK(insert->slot == InsertSlot::PreFader);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.contains("band0.frequencyHz"));
  REQUIRE(params.contains("band1.frequencyHz"));
  CHECK_FALSE(params.contains("band2.frequencyHz"));
  // Ascending frequency order, so band0 is the lower of the two.
  CHECK(params["band0.frequencyHz"].as_float() < params["band1.frequencyHz"].as_float());

  const SceneDelta* delta = find_delta(deltas, "pad", kParametric);
  REQUIRE(delta != nullptr);
  // Both bands are named in the one reason the delta carries.
  CHECK(delta->reason.find("mid") != std::string::npos);
  CHECK(delta->reason.find("highMid") != std::string::npos);
}

TEST_CASE("eq high-passes each source class at its own corner when asked to",
          "[mixing][assistant]") {
  struct Expected {
    SourceClass source;
    std::string id;
    // 0 means the class keeps its low end.
    float corner_hz;
  };

  // Mirrors the module's table on purpose: a corner that moves without anyone
  // meaning to move it fails here rather than passing whatever the code says.
  const std::vector<Expected> expected{
      {SourceClass::Vocal, "vox", 80.0f},        {SourceClass::Lead, "lead", 80.0f},
      {SourceClass::Kick, "kick", 0.0f},         {SourceClass::Snare, "snare", 80.0f},
      {SourceClass::Bass, "bass", 0.0f},         {SourceClass::Guitar, "gtr", 75.0f},
      {SourceClass::Keys, "keys", 50.0f},        {SourceClass::Strings, "strings", 60.0f},
      {SourceClass::Tom, "tom", 60.0f},          {SourceClass::Backing, "bvox", 100.0f},
      {SourceClass::Percussion, "perc", 150.0f}, {SourceClass::HiHat, "hat", 400.0f},
      {SourceClass::Cymbal, "cym", 400.0f},      {SourceClass::Fx, "fx", 0.0f},
  };

  std::vector<TrackProfile> profiles;
  profiles.reserve(expected.size());
  for (const Expected& row : expected) {
    profiles.push_back(make_profile(row.id, row.source));
    // Every track carries the same residue, so the only thing that can vary the
    // corner between them is the class table.
    set_low_energy_share(profiles.back(), kResidueShare);
  }

  const MixProfile mix = make_mix(static_cast<int>(profiles.size()));
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, high_pass_on());

  for (const Expected& row : expected) {
    INFO("track " << row.id);
    const Insert* insert = find_insert(deltas, row.id, kCutFilter);
    if (row.corner_hz == 0.0f) {
      // The low end is what these classes are made of, so nothing is swept.
      CHECK(insert == nullptr);
      continue;
    }
    REQUIRE(insert != nullptr);
    CHECK(insert->slot == InsertSlot::PreFader);
    const sonare::util::json::Value params = parse_params(*insert);
    REQUIRE(params.contains("highPassFrequencyHz"));
    CHECK_THAT(params["highPassFrequencyHz"].as_float(), WithinAbs(row.corner_hz, kHzTolerance));
    REQUIRE(params.contains("highPassEnabled"));
    CHECK(params["highPassEnabled"].as_bool());
    // The stage has no opinion about the top end and must not imply one.
    CHECK_FALSE(params.contains("lowPassEnabled"));
    for (const auto& [key, value] : params.as_object()) {
      INFO("param key " << key);
      CHECK((value.is_number() || value.is_bool()));
    }
  }
}

TEST_CASE("eq skips a band the high-pass has already removed", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("hat", SourceClass::HiHat),
                                     make_profile("perc", SourceClass::Percussion)};
  // The hi-hat has the lower role priority, so it is the one that gives way.
  gate_open(profiles[1], profiles[0], {kSubBand});
  for (TrackProfile& profile : profiles) set_low_energy_share(profile, kResidueShare);
  MixProfile mix = make_mix(2);
  // The sub band tops out at 60 Hz, well under both classes' corners.
  set_dominance(mix, 0, 1, kSubBand, kBuriedShare, kOverlapFrames);

  SECTION("with the high-pass proposed") {
    const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, high_pass_on());

    CHECK(count_inserts(deltas, kParametric) == 0);
    CHECK(count_inserts(deltas, kCutFilter) == 2);
  }

  SECTION("with no high-pass proposed") {
    // Nothing removed the sub band, so the collision in it still stands. The
    // suppression is a consequence of the filter, not a rule of its own.
    const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

    CHECK(count_inserts(deltas, kCutFilter) == 0);
    CHECK(count_inserts(deltas, kParametric) == 1);
  }
}

TEST_CASE("eq carves a band only when one part needs it and the other can spare it",
          "[mixing][assistant]") {
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  // The same collision throughout: the pad is in the vocal's way in the mid band
  // and the pad is the one that gives way. Only the two shares change.
  const auto cuts_for = [&mix](float vox_share, float pad_share) {
    std::vector<TrackProfile> profiles = two_tracks();
    set_band_occupancy(profiles[0], kMidBand, vox_share);
    set_band_occupancy(profiles[1], kMidBand, pad_share);
    return count_inserts(decide_eq(profiles, mix, MixAssistantConfig{}), kParametric);
  };

  SECTION("essential to the part being made room for, disposable to the one giving way") {
    CHECK(cuts_for(kEssentialShare, kSpareShare) == 1);
  }

  SECTION("essential to both: the part giving way needs the band too") {
    CHECK(cuts_for(kEssentialShare, kEssentialShare) == 0);
  }

  SECTION("disposable to both: nothing is being made room for") {
    CHECK(cuts_for(kSpareShare, kSpareShare) == 0);
  }

  SECTION("neither essential nor disposable, on the side being made room for") {
    CHECK(cuts_for(kAmbiguousShare, kSpareShare) == 0);
  }

  SECTION("neither essential nor disposable, on the side giving way") {
    CHECK(cuts_for(kEssentialShare, kAmbiguousShare) == 0);
  }
}

TEST_CASE("eq emits no delta at all for a track whose only band is gated out",
          "[mixing][assistant]") {
  // The collision is real and the priority table names a victim, so before the
  // gate this track earned a cut. An empty eq delta would read downstream as a
  // decision to leave the strip flat, which is a different statement from having
  // found nothing worth correcting.
  std::vector<TrackProfile> profiles = two_tracks();
  set_band_occupancy(profiles[0], kMidBand, kEssentialShare);
  set_band_occupancy(profiles[1], kMidBand, kEssentialShare);

  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kBuriedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(deltas.empty());
  CHECK(find_delta(deltas, "pad", kParametric) == nullptr);
}

TEST_CASE("eq names both shares in the reason for a gated cut", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const std::string reason = pad_cut_reason(profiles, mix);
  INFO(reason);
  // Why this band and not another: the vocal is built around it and the pad is
  // not. Both figures are the ones the gate was evaluated on.
  CHECK(reason.find("which vox needs at 45.0% of its energy") != std::string::npos);
  CHECK(reason.find("pad can spare at 2.0% of its own") != std::string::npos);
}

TEST_CASE("eq places the cut at the overlap, not at the band's centre", "[mixing][assistant]") {
  // The mid band runs two octaves, and everything the two tracks share sits in
  // its top third. A stage that still used the grid would put the filter an
  // octave below the collision it was justified by.
  std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
  const std::vector<SpectrumBin> shared{{75, 1.0f}, {76, 2.0f}, {77, 4.0f}, {78, 2.0f}, {79, 1.0f}};
  set_spectrum(profiles[0], shared);
  set_spectrum(profiles[1], shared);

  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  const float center_hz = only_cut_center_hz(*insert);
  INFO("centre " << center_hz << " Hz, hump spans " << bin_hz(75) << " to " << bin_hz(79));
  CHECK(center_hz > bin_hz(70));
  CHECK(center_hz < bin_hz(84));
  CHECK(center_hz > kMidBandCenterHz);

  const SceneDelta* delta = find_delta(deltas, "pad", kParametric);
  REQUIRE(delta != nullptr);
  // The reason has to say the figure was measured; a bare frequency next to a
  // band name reads as a label for the band.
  CHECK(delta->reason.find("measured overlap in mid") != std::string::npos);
  CHECK(delta->reason.find("band centre") == std::string::npos);
}

TEST_CASE("eq places the cut where both tracks are, not where either one is",
          "[mixing][assistant]") {
  // The pad owns the bottom of the band outright and the vocal owns the top of
  // it; they meet only in the middle, and the middle is much quieter than
  // either. A measure that summed the two would pick one of the loud shoulders,
  // which is the one place there is no collision at all.
  std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
  set_spectrum(profiles[0], flat_run(30, 34, 20.0f) + flat_run(55, 65, 1.0f));
  set_spectrum(profiles[1], flat_run(78, 82, 20.0f) + flat_run(55, 65, 1.0f));

  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const float center_hz = pad_cut_center_hz(profiles, mix);
  INFO("centre " << center_hz << " Hz, shared span " << bin_hz(55) << " to " << bin_hz(65));
  CHECK(center_hz >= bin_hz(55));
  CHECK(center_hz <= bin_hz(65));
}

TEST_CASE("eq smooths the overlap before choosing a frequency", "[mixing][assistant]") {
  // A single bin three times the height of a hump twenty bins wide. Raw, the
  // lone bin wins; that is noise rather than a resonance, and a filter placed on
  // it would move between renders for reasons nobody can hear.
  std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
  const std::vector<SpectrumBin> spiky =
      std::vector<SpectrumBin>{{30, 3.0f}} + flat_run(55, 75, 1.0f);
  set_spectrum(profiles[0], spiky);
  set_spectrum(profiles[1], spiky);

  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const float center_hz = pad_cut_center_hz(profiles, mix);
  INFO("centre " << center_hz << " Hz, spike at " << bin_hz(30) << " Hz, hump " << bin_hz(55)
                 << " to " << bin_hz(75));
  CHECK(center_hz >= bin_hz(55));
  CHECK(center_hz <= bin_hz(75));
}

TEST_CASE("eq keeps the cut inside the band that justified it", "[mixing][assistant]") {
  // A shared peak just above the band, a hundred times anything inside it. The
  // smoothing window reaches it — that is deliberate, so a band edge is not
  // decided by how many neighbours happen to be in range — but it must not be
  // able to pull the filter out of the band the collision was found in.
  std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
  const std::vector<SpectrumBin> shared =
      std::vector<SpectrumBin>{{87, 100.0f}} + flat_run(40, 50, 1.0f);
  set_spectrum(profiles[0], shared);
  set_spectrum(profiles[1], shared);
  // Just outside the band and well inside the smoothing kernel of the highest
  // bin that is inside it.
  REQUIRE(bin_hz(87) > kMidBandHighHz);

  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const float center_hz = pad_cut_center_hz(profiles, mix);
  INFO("centre " << center_hz << " Hz, outside peak at " << bin_hz(87) << " Hz");
  CHECK(center_hz >= kMidBandLowHz);
  CHECK(center_hz <= kMidBandHighHz);
}

TEST_CASE("eq measures the air band, which has no finite upper edge", "[mixing][assistant]") {
  SECTION("the span runs to Nyquist") {
    std::vector<TrackProfile> profiles = two_tracks_contesting({kAirBand});
    const std::vector<SpectrumBin> shared = flat_run(795, 805, 1.0f);
    set_spectrum(profiles[0], shared);
    set_spectrum(profiles[1], shared);

    MixProfile mix = make_mix(2);
    set_dominance(mix, 1, 0, kAirBand, kContestedShare, kOverlapFrames);

    const float center_hz = pad_cut_center_hz(profiles, mix);
    INFO("centre " << center_hz << " Hz, shared span " << bin_hz(795) << " to " << bin_hz(805));
    CHECK(center_hz >= bin_hz(795));
    CHECK(center_hz <= bin_hz(805));
  }

  SECTION("a band that reaches past Nyquist keeps the part below it") {
    // At 32 kHz the air band's usable span is 12 kHz to 16 kHz rather than the
    // nominal one, and the content sits inside what is left.
    constexpr int kLowRate = 32000;
    std::vector<TrackProfile> profiles = two_tracks_contesting({kAirBand});
    const std::vector<SpectrumBin> shared = flat_run(895, 905, 1.0f);
    set_spectrum(profiles[0], shared, kLowRate);
    set_spectrum(profiles[1], shared, kLowRate);

    MixProfile mix = make_mix(2);
    set_dominance(mix, 1, 0, kAirBand, kContestedShare, kOverlapFrames);

    const float center_hz = pad_cut_center_hz(profiles, mix);
    INFO("centre " << center_hz << " Hz");
    CHECK(center_hz > 12000.0f);
    CHECK(center_hz <= 0.5f * static_cast<float>(kLowRate));
  }
}

TEST_CASE("eq falls back to the band centre when nothing can be measured", "[mixing][assistant]") {
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  SECTION("no spectrum at all") {
    const std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
    CHECK_THAT(pad_cut_center_hz(profiles, mix), WithinAbs(kMidBandCenterHz, kHzTolerance));

    // The wording has to change with the number, or a grid point reads as a
    // measurement.
    const std::string reason = pad_cut_reason(profiles, mix);
    CHECK(reason.find("mid band centre") != std::string::npos);
    CHECK(reason.find("measured overlap") == std::string::npos);
  }

  SECTION("only one of the two tracks was measured") {
    std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
    set_spectrum(profiles[0], flat_run(55, 65, 1.0f));
    CHECK_THAT(pad_cut_center_hz(profiles, mix), WithinAbs(kMidBandCenterHz, kHzTolerance));
  }

  SECTION("the two were measured with different geometries") {
    std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
    set_spectrum(profiles[0], flat_run(55, 65, 1.0f));
    set_spectrum(profiles[1], flat_run(55, 65, 1.0f), 44100);
    CHECK_THAT(pad_cut_center_hz(profiles, mix), WithinAbs(kMidBandCenterHz, kHzTolerance));
  }

  SECTION("neither track has anything inside the band") {
    std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
    const std::vector<SpectrumBin> outside{{5, 1.0f}};
    set_spectrum(profiles[0], outside);
    set_spectrum(profiles[1], outside);
    CHECK_THAT(pad_cut_center_hz(profiles, mix), WithinAbs(kMidBandCenterHz, kHzTolerance));
  }

  SECTION("they share the band with each other but not the same bins") {
    std::vector<TrackProfile> profiles = two_tracks_contesting({kMidBand});
    set_spectrum(profiles[0], flat_run(30, 40, 1.0f));
    set_spectrum(profiles[1], flat_run(60, 70, 1.0f));
    CHECK_THAT(pad_cut_center_hz(profiles, mix), WithinAbs(kMidBandCenterHz, kHzTolerance));
  }

  SECTION("the band lies entirely above Nyquist") {
    constexpr int kNarrowRate = 8000;
    std::vector<TrackProfile> profiles = two_tracks_contesting({kHighBand});
    const std::vector<SpectrumBin> shared = flat_run(55, 65, 1.0f);
    set_spectrum(profiles[0], shared, kNarrowRate);
    set_spectrum(profiles[1], shared, kNarrowRate);

    MixProfile high_mix = make_mix(2);
    set_dominance(high_mix, 1, 0, kHighBand, kContestedShare, kOverlapFrames);
    // The high band starts at 6 kHz and Nyquist is 4 kHz, so there is no span to
    // measure in and the grid point is all that is left.
    CHECK_THAT(pad_cut_center_hz(profiles, high_mix),
               WithinAbs(std::sqrt(6000.0f * 12000.0f), 1.0f));
  }
}

TEST_CASE("eq measures the cut centre from genuinely profiled tracks", "[mixing][assistant]") {
  const std::vector<TrackProfile>& profiles = profiled_collision_pair();
  REQUIRE(profiles.size() == 2);
  REQUIRE(profiles[0].usable);
  REQUIRE(profiles[1].usable);

  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});
  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  const float center_hz = only_cut_center_hz(*insert);
  INFO("centre " << center_hz << " Hz, shared tone at " << kSharedToneHz << " Hz");
  CHECK_THAT(center_hz, WithinAbs(kSharedToneHz, 200.0f));
  CHECK(center_hz > kMidBandCenterHz);

  const SceneDelta* delta = find_delta(deltas, "pad", kParametric);
  REQUIRE(delta != nullptr);
  CHECK(delta->reason.find("measured overlap in mid") != std::string::npos);
}

TEST_CASE("eq suggests no high-pass unless it is asked for", "[mixing][assistant]") {
  // Every track carries residue under its corner, so the class-only rule would
  // high-pass all of them. Nothing here asks for a filter.
  const std::vector<TrackProfile> profiles = two_tracks_with_residue();
  const MixProfile mix = make_mix(2);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(count_inserts(deltas, kCutFilter) == 0);
  CHECK(deltas.empty());
}

TEST_CASE("eq high-passes a track whose low end is residue", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("vox", SourceClass::Vocal)};
  set_low_energy_share(profiles[0], kResidueShare);
  const MixProfile mix = make_mix(1);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, high_pass_on());

  const Insert* insert = find_insert(deltas, "vox", kCutFilter);
  REQUIRE(insert != nullptr);
  CHECK(insert->slot == InsertSlot::PreFader);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.contains("highPassFrequencyHz"));
  CHECK_THAT(params["highPassFrequencyHz"].as_float(), WithinAbs(80.0f, kHzTolerance));

  const SceneDelta* delta = find_delta(deltas, "vox", kCutFilter);
  REQUIRE(delta != nullptr);
  CHECK(delta->domain == DeltaDomain::Eq);
  CHECK(delta->reason.find("80 Hz") != std::string::npos);
  // The measurement the decision was made from, not just its verdict.
  CHECK(delta->reason.find("2.0%") != std::string::npos);
}

TEST_CASE("eq leaves a track whose low end is its own material alone", "[mixing][assistant]") {
  // A part written under its class's usual register still classifies as that
  // class, so the class table hands it a corner that sits over notes it is
  // actually playing. This is the case the class-only rule got wrong.
  std::vector<TrackProfile> profiles{make_profile("vox", SourceClass::Vocal)};
  set_low_energy_share(profiles[0], kOwnMaterialShare);
  const MixProfile mix = make_mix(1);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, high_pass_on());

  CHECK(count_inserts(deltas, kCutFilter) == 0);
  CHECK(deltas.empty());
}

TEST_CASE("eq leaves a track with nothing below its corner alone", "[mixing][assistant]") {
  // Nothing down there to remove, so the filter would only make the suggestion
  // look busier than it is.
  std::vector<TrackProfile> profiles{make_profile("hat", SourceClass::HiHat)};
  set_low_energy_share(profiles[0], kNothingBelowShare);
  const MixProfile mix = make_mix(1);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, high_pass_on());

  CHECK(count_inserts(deltas, kCutFilter) == 0);

  SECTION("and a profile that was never given a spectrum reads the same way") {
    std::vector<TrackProfile> unmeasured{make_profile("hat", SourceClass::HiHat)};
    CHECK(count_inserts(decide_eq(unmeasured, mix, high_pass_on()), kCutFilter) == 0);
  }
}

TEST_CASE("eq peaking cuts are the same with the high-pass switch either way",
          "[mixing][assistant]") {
  // The collision is in the mid band, well above every corner in the table, so
  // no high-pass can make it redundant and the switch must not touch it.
  const std::vector<TrackProfile> profiles = two_tracks_with_residue({kMidBand});
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> off = decide_eq(profiles, mix, MixAssistantConfig{});
  const std::vector<SceneDelta> on = decide_eq(profiles, mix, high_pass_on());

  const Insert* off_insert = find_insert(off, "pad", kParametric);
  const Insert* on_insert = find_insert(on, "pad", kParametric);
  REQUIRE(off_insert != nullptr);
  REQUIRE(on_insert != nullptr);
  CHECK(off_insert->params_json == on_insert->params_json);
  CHECK(count_inserts(off, kParametric) == count_inserts(on, kParametric));

  const SceneDelta* off_delta = find_delta(off, "pad", kParametric);
  const SceneDelta* on_delta = find_delta(on, "pad", kParametric);
  REQUIRE(off_delta != nullptr);
  REQUIRE(on_delta != nullptr);
  CHECK(off_delta->reason == on_delta->reason);

  // Only the filter itself differs.
  CHECK(count_inserts(off, kCutFilter) == 0);
  CHECK(count_inserts(on, kCutFilter) == 2);
}

TEST_CASE("eq measures the high-pass from a real profiled track", "[mixing][assistant]") {
  // The hand-built spectra above would pass just as well against a profiler that
  // never filled TrackProfile::spectrum at all, so one case runs the real
  // analysis: a part at 300 Hz with a little 40 Hz rumble under it.
  constexpr int kSampleRate = 48000;
  constexpr float kDurationSec = 0.6f;
  constexpr float kProgramAmplitude = 0.3f;
  constexpr float kRumbleAmplitude = 0.045f;

  const std::size_t frames =
      static_cast<std::size_t>(kDurationSec * static_cast<float>(kSampleRate));
  std::vector<float> samples(frames, 0.0f);
  for (std::size_t index = 0; index < frames; ++index) {
    const float seconds = static_cast<float>(index) / static_cast<float>(kSampleRate);
    samples[index] = kProgramAmplitude * std::sin(sonare::constants::kTwoPi * 300.0f * seconds) +
                     kRumbleAmplitude * std::sin(sonare::constants::kTwoPi * 40.0f * seconds);
  }

  TrackInput input;
  input.id = "vox";
  input.left = samples.data();
  input.frame_count = frames;
  input.sample_rate = kSampleRate;

  TrackProfile profile = analyze_track_profile(input);
  REQUIRE(profile.usable);
  // The classifier is not what this case is about; only the measured spectrum
  // has to be real.
  profile.source = SourceClass::Vocal;
  profile.source_confidence = kHighConfidence;
  REQUIRE(profile.spectrum.energy_share_below(80.0f) > 0.0f);

  const std::vector<TrackProfile> profiles{profile};
  const MixProfile mix = make_mix(1);

  CHECK(count_inserts(decide_eq(profiles, mix, MixAssistantConfig{}), kCutFilter) == 0);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, high_pass_on());
  const Insert* insert = find_insert(deltas, "vox", kCutFilter);
  REQUIRE(insert != nullptr);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.contains("highPassFrequencyHz"));
  CHECK_THAT(params["highPassFrequencyHz"].as_float(), WithinAbs(80.0f, kHzTolerance));

  const SceneDelta* delta = find_delta(deltas, "vox", kCutFilter);
  REQUIRE(delta != nullptr);
  CHECK(delta->reason.find("high-passed vox at 80 Hz") != std::string::npos);
}

TEST_CASE("eq leaves unidentified, unusable and low-confidence tracks alone",
          "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("mystery", SourceClass::Unknown),
                                     make_profile("silent", SourceClass::Vocal),
                                     make_profile("maybeLead", SourceClass::Lead)};
  profiles[1].usable = false;
  profiles[2].source_confidence = kLowConfidence;

  MixProfile mix = make_mix(3);
  for (int masker = 0; masker < 3; ++masker) {
    for (int maskee = 0; maskee < 3; ++maskee) {
      if (masker == maskee) continue;
      set_dominance(mix, masker, maskee, kMidBand, kBuriedShare, kOverlapFrames);
    }
  }

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  // Not a zero-valued suggestion; no suggestion at all.
  CHECK(deltas.empty());
}

TEST_CASE("eq makes no peaking cut when the ceiling is zero", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks_with_residue();
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kBuriedShare, kOverlapFrames);

  MixAssistantConfig config = high_pass_on();
  config.eq_max_cut_db = 0.0f;
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, config);

  CHECK(count_inserts(deltas, kParametric) == 0);
  // Tidying the low end is not a cut governed by the ceiling.
  CHECK(count_inserts(deltas, kCutFilter) == 2);
}

TEST_CASE("eq returns nothing when the domain is switched off", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kBuriedShare, kOverlapFrames);

  MixAssistantConfig config;
  config.enable_eq = false;

  CHECK(decide_eq(profiles, mix, config).empty());
}

TEST_CASE("eq handles no tracks and a single track", "[mixing][assistant]") {
  SECTION("no tracks") {
    const std::vector<TrackProfile> profiles;
    const MixProfile mix;
    std::vector<SceneDelta> deltas;
    REQUIRE_NOTHROW(deltas = decide_eq(profiles, mix, MixAssistantConfig{}));
    CHECK(deltas.empty());
  }

  SECTION("one track") {
    std::vector<TrackProfile> profiles{make_profile("vox", SourceClass::Vocal)};
    set_low_energy_share(profiles[0], kResidueShare);
    const MixProfile mix = make_mix(1);
    std::vector<SceneDelta> deltas;
    REQUIRE_NOTHROW(deltas = decide_eq(profiles, mix, high_pass_on()));
    // Nothing to collide with, so only the low-end tidy remains.
    CHECK(count_inserts(deltas, kParametric) == 0);
    CHECK(count_inserts(deltas, kCutFilter) == 1);
  }

  SECTION("a mix profile that never got measured") {
    const std::vector<TrackProfile> profiles = two_tracks();
    const MixProfile mix;
    std::vector<SceneDelta> deltas;
    REQUIRE_NOTHROW(deltas = decide_eq(profiles, mix, MixAssistantConfig{}));
    CHECK(count_inserts(deltas, kParametric) == 0);
  }
}
