/// @file gain_staging.cpp
/// @brief Static input-trim staging for the mixing assistant.

#include "mixing/assistant/gain_staging.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

#include "util/constants.h"

namespace sonare::mixing::assistant {

using sonare::constants::kFloorDb;

namespace {

// A silent track's integrated loudness arrives either as -inf (the BS.1770
// convention for a program with no gated block) or pinned at the numerical dB
// floor. Comparing against the floor with == would miss a value that landed a
// hair above it after gating and averaging, so the test carries this much
// headroom above the floor instead.
constexpr float kSilenceFloorHeadroomDb = 1.0f;
constexpr float kSilenceLufsThreshold = kFloorDb + kSilenceFloorHeadroomDb;

// BS.1770 integrates over 400 ms gating blocks, so a track shorter than one
// block has no gated measurement to stage against — whatever number comes back
// describes the gate, not the material. Matches the profiler's own default, so
// a track this stage keeps is one the profiler also kept.
constexpr float kMinMeasurableDurationSec = 0.4f;

// band_occupancy sums to 1 for a track with measurable spectral content and to
// 0 for one without; DC-only material has no energy inside any band, the lowest
// of which starts at 20 Hz. Only those two states exist, so half way between
// them is the least sensitive place to draw the line.
constexpr float kMinBandOccupancySum = 0.5f;

// suggestion_strength is documented as a [0, 1] scale. A value outside it would
// invert or exaggerate the trim rather than weaken it, which is not a weaker
// suggestion but a different one.
constexpr float kMinSuggestionStrength = 0.0f;
constexpr float kMaxSuggestionStrength = 1.0f;

// One decimal is the resolution a mixer acts on; more digits make the
// explanation read like a measurement rather than a suggestion.
constexpr int kReasonDecimals = 1;

std::string format_db(float value) {
  // Negative zero would print as "-0.0" and read as a real downward move.
  if (value == 0.0f) value = 0.0f;
  std::ostringstream out;
  // Classic locale, so the decimal point is a point wherever the host runs. A
  // reason string is read by a person and parsed by nobody, but a comma in the
  // middle of a number reads as a second number.
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(kReasonDecimals) << value;
  return out.str();
}

float band_occupancy_sum(const TrackProfile& profile) {
  float total = 0.0f;
  for (const float share : profile.band_occupancy) {
    if (std::isfinite(share)) total += share;
  }
  return total;
}

// Returns why the track is not staged, or nullptr when it is.
//
// The text names which check fired and is what a caller-facing note would say,
// but it is deliberately not emitted: excluding a track means emitting no delta
// at all, and this function's contract is to return suggestions rather than
// annotations.
//
// Each condition is tested here on its own rather than deferred to
// TrackProfile::usable. The profiler already makes the same calls, but this
// stage has to be correct when it is handed a hand-built profile too, so the
// duplication is deliberate.
const char* staging_exclusion(const TrackProfile& profile) {
  if (!profile.usable) {
    return "the profiler marked the track unusable";
  }
  const float measured_lufs = profile.base.loudness.integrated_lufs;
  if (!std::isfinite(measured_lufs) || measured_lufs <= kSilenceLufsThreshold) {
    return "the track is silent, so there is no level to stage";
  }
  if (profile.duration_sec < kMinMeasurableDurationSec) {
    return "the track is shorter than one gating block, so its integrated loudness says nothing "
           "about the material";
  }
  if (band_occupancy_sum(profile) < kMinBandOccupancySum) {
    return "the track has no energy in any analysis band";
  }
  return nullptr;
}

}  // namespace

std::vector<SceneDelta> decide_gain_staging(const std::vector<TrackProfile>& profiles,
                                            const MixAssistantConfig& config) {
  std::vector<SceneDelta> deltas;
  if (!config.enable_gain) return deltas;
  // A target that is not a real level is unreachable by any trim, and
  // subtracting it would write a non-finite value onto every strip.
  if (!std::isfinite(config.target_track_lufs)) return deltas;

  const float strength =
      std::clamp(config.suggestion_strength, kMinSuggestionStrength, kMaxSuggestionStrength);
  const std::string target_text = format_db(config.target_track_lufs);

  deltas.reserve(profiles.size());
  for (const TrackProfile& profile : profiles) {
    if (staging_exclusion(profile) != nullptr) continue;

    // The whole decision: one static offset from the measured level to the
    // absolute target. Nothing here reads profile.source — a class-relative
    // offset belongs to the balance pass and sums on top of this one.
    const float trim_db =
        (config.target_track_lufs - profile.base.loudness.integrated_lufs) * strength;

    SceneDelta delta;
    delta.domain = DeltaDomain::Gain;
    delta.strip_id = profile.strip_id;
    delta.input_trim_db = trim_db;
    delta.reason = "staged " + profile.strip_id + " with " + format_db(trim_db) +
                   " dB of input trim towards the " + target_text + " LUFS target";
    if (trim_db < kMinSuggestedTrimDb || trim_db > kMaxSuggestedTrimDb) {
      delta.reason += ", which is beyond the " + format_db(kMinSuggestedTrimDb) + " to " +
                      format_db(kMaxSuggestedTrimDb) +
                      " dB trim range, so the target will not be fully reached";
    }
    deltas.push_back(std::move(delta));
  }

  return deltas;
}

float decide_master_headroom_db(const std::vector<TrackProfile>& profiles, const api::Scene& scene,
                                const MixAssistantConfig& config) {
  if (!std::isfinite(config.mix_bus_headroom_dbtp)) return 0.0f;

  double peak_amplitude = 0.0;
  for (const auto& profile : profiles) {
    if (!profile.usable) continue;
    if (!std::isfinite(profile.base.loudness.true_peak_db)) continue;

    float gain_db = 0.0f;
    bool found = false;
    for (const auto& strip : scene.strips) {
      if (strip.id != profile.strip_id) continue;
      gain_db = strip.input_trim_db + strip.fader_db + strip.vca_offset_db;
      found = true;
      break;
    }
    if (!found) continue;

    peak_amplitude += std::pow(10.0, (profile.base.loudness.true_peak_db + gain_db) / 20.0);
  }

  if (!(peak_amplitude > 0.0)) return 0.0f;
  const double estimated_peak_dbtp = 20.0 * std::log10(peak_amplitude);
  const double trim = static_cast<double>(config.mix_bus_headroom_dbtp) - estimated_peak_dbtp;
  // Never positive: a mix that already fits needs no help, and pushing it up
  // would be a creative choice this stage has no basis for making.
  return static_cast<float>(std::min(0.0, trim));
}

}  // namespace sonare::mixing::assistant
