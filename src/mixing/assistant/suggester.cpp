/// @file suggester.cpp
/// @brief Mixing assistant pipeline: analyse, decide, compose.

#include "mixing/assistant/suggester.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>

#include "mixing/assistant/decide_balance.h"
#include "mixing/assistant/decide_dynamics.h"
#include "mixing/assistant/decide_eq.h"
#include "mixing/assistant/decide_image.h"
#include "mixing/assistant/decide_structure.h"
#include "mixing/assistant/gain_staging.h"
#include "mixing/assistant/image_occupancy.h"
#include "mixing/assistant/masking.h"
#include "mixing/assistant/phase_alignment.h"
#include "mixing/assistant/scene_delta_internal.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/json.h"
#include "util/number_format.h"

namespace sonare::mixing::assistant {
namespace {

// The suggestion is built against an empty scene rather than an existing one:
// the assistant proposes a starting point, and merging into a scene the user
// has already shaped is a different operation with different conflict rules.
api::Scene empty_scene_for(const std::vector<TrackProfile>& profiles) {
  api::Scene scene;
  scene.strips.reserve(profiles.size());
  for (const auto& profile : profiles) {
    api::Strip strip;
    strip.id = profile.strip_id;
    scene.strips.push_back(std::move(strip));
  }
  return scene;
}

// Final clamped input trim of each strip under @p deltas, index-parallel to
// @p profiles: what the dynamics inserts, which run after the trim, receive.
std::vector<float> staged_input_trims(const std::vector<TrackProfile>& profiles,
                                      const std::vector<SceneDelta>& deltas) {
  const api::Scene staged = apply_deltas(empty_scene_for(profiles), deltas, nullptr);
  std::vector<float> trims(profiles.size(), 0.0f);
  for (std::size_t index = 0; index < profiles.size() && index < staged.strips.size(); ++index) {
    trims[index] = staged.strips[index].input_trim_db;
  }
  return trims;
}

void append(std::vector<SceneDelta>& into, std::vector<SceneDelta> from) {
  into.insert(into.end(), std::make_move_iterator(from.begin()),
              std::make_move_iterator(from.end()));
}

// Two tracks sharing a strip id produce two strips with that id, of which
// apply_deltas only ever reaches the first; the second ships as an empty stub
// and the mixer rejects the whole scene at load time as a "duplicate or invalid
// strip id". The complaint then names the scene rather than the input that
// produced it, so it is raised here, where the caller can still see which of
// their tracks collided.
void require_unique_ids(const std::vector<TrackInput>& tracks) {
  std::set<std::string> seen;
  for (const TrackInput& track : tracks) {
    SONARE_CHECK_MSG(!track.id.empty(), ErrorCode::InvalidParameter,
                     "mixing assistant track id must not be empty");
    SONARE_CHECK_MSG(seen.insert(track.id).second, ErrorCode::InvalidParameter,
                     "duplicate mixing assistant track id '" + track.id + "'");
  }
}

void require_unique_profile_ids(const std::vector<TrackProfile>& profiles) {
  std::set<std::string> seen;
  for (const TrackProfile& profile : profiles) {
    SONARE_CHECK_MSG(!profile.strip_id.empty(), ErrorCode::InvalidParameter,
                     "mixing assistant profile id must not be empty");
    SONARE_CHECK_MSG(seen.insert(profile.strip_id).second, ErrorCode::InvalidParameter,
                     "duplicate mixing assistant profile id '" + profile.strip_id + "'");
  }
}

void require_finite_config(const MixAssistantConfig& config) {
  struct Scalar {
    const char* name;
    float MixAssistantConfig::*member;
  };
  constexpr Scalar scalars[] = {
      {"target_track_lufs", &MixAssistantConfig::target_track_lufs},
      {"suggestion_strength", &MixAssistantConfig::suggestion_strength},
      {"eq_max_cut_db", &MixAssistantConfig::eq_max_cut_db},
      {"mix_bus_headroom_dbtp", &MixAssistantConfig::mix_bus_headroom_dbtp},
      {"tempo_bpm", &MixAssistantConfig::tempo_bpm},
  };
  for (const Scalar& scalar : scalars) {
    SONARE_CHECK_MSG(std::isfinite(config.*scalar.member), ErrorCode::InvalidParameter,
                     std::string("mixing assistant ") + scalar.name + " must be finite");
  }
}

void validate_analysis_inputs(const std::vector<TrackInput>& tracks,
                              const std::vector<TrackProfile>& profiles) {
  SONARE_CHECK_MSG(tracks.size() == profiles.size(), ErrorCode::InvalidParameter,
                   "mixing assistant tracks and profiles must have the same size");
  require_unique_ids(tracks);
  require_unique_profile_ids(profiles);
  for (std::size_t index = 0; index < tracks.size(); ++index) {
    SONARE_CHECK_MSG(tracks[index].id == profiles[index].strip_id, ErrorCode::InvalidParameter,
                     "mixing assistant track and profile ids must match in order");
  }
}

bool trims_differ(const std::vector<float>& recorded, const std::vector<float>& current) {
  if (recorded.size() != current.size()) return true;
  for (std::size_t index = 0; index < recorded.size(); ++index) {
    if (recorded[index] != current[index]) return true;
  }
  return false;
}

std::vector<TrackChannelEnergy> project_channel_energy(const std::vector<TrackChannelEnergy>& raw,
                                                       const std::vector<float>& trims) {
  if (raw.size() != trims.size()) return {};
  std::vector<TrackChannelEnergy> projected = raw;
  for (std::size_t index = 0; index < projected.size(); ++index) {
    const double gain = static_cast<double>(db_to_power_scalar(trims[index]));
    for (auto* plane : {&projected[index].left, &projected[index].right, &projected[index].mid,
                        &projected[index].side}) {
      for (double& value : *plane) value *= gain;
    }
  }
  return projected;
}

void cache_image_measurements(MixProfile& mix) {
  if (mix.cached_alignment.empty()) {
    mix.cached_alignment = std::move(mix.alignment);
  }
  mix.alignment.clear();
  if (mix.cached_mono_risks.empty()) {
    mix.cached_mono_risks = std::move(mix.mono_risks);
  }
  mix.mono_risks.clear();
}

void restore_image_measurements(MixProfile& mix) {
  if (mix.alignment.empty()) {
    mix.alignment = std::move(mix.cached_alignment);
  }
  mix.cached_alignment.clear();
  if (mix.mono_risks.empty()) {
    mix.mono_risks = std::move(mix.cached_mono_risks);
  }
  mix.cached_mono_risks.clear();
}

void clear_image_measurements(MixProfile& mix) {
  mix.alignment.clear();
  mix.cached_alignment.clear();
  mix.mono_risks.clear();
  mix.cached_mono_risks.clear();
}

bool any_usable(const std::vector<TrackProfile>& profiles) {
  return std::any_of(profiles.begin(), profiles.end(),
                     [](const TrackProfile& profile) { return profile.usable; });
}

util::json::Value band_array(const std::array<float, kBandCount>& values) {
  util::json::Object object;
  for (int band = 0; band < kBandCount; ++band) {
    object.emplace(kBandNames[static_cast<std::size_t>(band)],
                   util::json::Value(values[static_cast<std::size_t>(band)]));
  }
  return util::json::Value(std::move(object));
}

util::json::Value track_to_value(const TrackProfile& profile) {
  util::json::Object object;
  util::json::put(object, "stripId", profile.strip_id);
  util::json::put(object, "name", profile.name);
  util::json::put(object, "source", source_class_to_string(profile.source));
  util::json::put(object, "sourceConfidence", profile.source_confidence);
  util::json::put(object, "usable", profile.usable);
  util::json::put(object, "exclusionReason", profile.exclusion_reason);
  util::json::put(object, "channelCount", profile.channel_count);
  util::json::put(object, "durationSec", profile.duration_sec);
  util::json::put(object, "integratedLufs", profile.base.loudness.integrated_lufs);
  util::json::put(object, "truePeakDb", profile.base.loudness.true_peak_db);
  util::json::put(object, "crestFactorDb", profile.base.loudness.crest_factor_db);
  util::json::put(object, "spectralCentroidHz", profile.base.spectral.centroid_hz);
  util::json::put(object, "spectralFlatness", profile.base.spectral.flatness);
  util::json::put(object, "attackDensity", profile.base.dynamics.attack_density);
  util::json::put(object, "sustainRatio", profile.base.dynamics.sustain_ratio);
  util::json::put(object, "bandOccupancy", band_array(profile.band_occupancy));
  return util::json::Value(std::move(object));
}

util::json::Value mix_to_value(const MixProfile& mix, const std::vector<TrackProfile>& profiles) {
  util::json::Object object;
  util::json::put(object, "trackCount", mix.track_count);

  // The dominance matrix is emitted only where it is actually informative:
  // a full N^2 x 7 dump is mostly zeros and mostly noise for a reader.
  util::json::Array dominance;
  const int safe_track_count = std::max(mix.track_count, 0);
  const int serializable_track_count =
      std::min(safe_track_count, static_cast<int>(profiles.size()));
  for (int masker = 0; masker < serializable_track_count; ++masker) {
    for (int maskee = 0; maskee < serializable_track_count; ++maskee) {
      if (masker == maskee) continue;
      for (int band = 0; band < kBandCount; ++band) {
        const BandDominance entry = mix.dominance_at(masker, maskee, band);
        if (entry.valid_frames == 0) continue;
        util::json::Object row;
        util::json::put(row, "masker", profiles[static_cast<std::size_t>(masker)].strip_id);
        util::json::put(row, "maskee", profiles[static_cast<std::size_t>(maskee)].strip_id);
        util::json::put(row, "band", kBandNames[static_cast<std::size_t>(band)]);
        util::json::put(row, "ratio", entry.ratio);
        util::json::put(row, "validFrames", entry.valid_frames);
        dominance.emplace_back(util::json::Value(std::move(row)));
      }
    }
  }
  util::json::put(object, "bandDominance", std::move(dominance));

  util::json::Array alignment;
  for (const auto& pair : mix.alignment) {
    if (!pair.related) continue;
    if (pair.reference_index < 0 || pair.target_index < 0 ||
        pair.reference_index >= serializable_track_count ||
        pair.target_index >= serializable_track_count) {
      continue;
    }
    util::json::Object row;
    row.emplace(
        "reference",
        util::json::Value(profiles[static_cast<std::size_t>(pair.reference_index)].strip_id));
    util::json::put(row, "target", profiles[static_cast<std::size_t>(pair.target_index)].strip_id);
    util::json::put(row, "lagSamples", pair.lag_samples);
    util::json::put(row, "correlation", pair.correlation);
    util::json::put(row, "polarityOpposed", pair.polarity_opposed);
    alignment.emplace_back(util::json::Value(std::move(row)));
  }
  util::json::put(object, "alignment", std::move(alignment));

  util::json::Array crowded;
  for (int band = 0; band < kBandCount; ++band) {
    if (band >= static_cast<int>(mix.image.crowded.size())) break;
    if (!mix.image.crowded[static_cast<std::size_t>(band)]) continue;
    util::json::Object row;
    util::json::put(row, "band", kBandNames[static_cast<std::size_t>(band)]);
    util::json::put(row, "crowding", mix.image.crowding[static_cast<std::size_t>(band)]);
    crowded.emplace_back(util::json::Value(std::move(row)));
  }
  util::json::put(object, "crowdedBands", std::move(crowded));

  util::json::Array mono_risks;
  for (const auto& risk : mix.mono_risks) {
    util::json::Object row;
    util::json::put(row, "stripId", risk.strip_id);
    util::json::put(row, "correlation", risk.correlation);
    util::json::put(row, "width", risk.width);
    util::json::put(row, "wideLowEnd", risk.wide_low_end);
    mono_risks.emplace_back(util::json::Value(std::move(row)));
  }
  util::json::put(object, "monoRisks", std::move(mono_risks));

  return util::json::Value(std::move(object));
}

}  // namespace

MixProfile analyze_mix_profile(const std::vector<TrackInput>& tracks,
                               const std::vector<TrackProfile>& profiles,
                               const MixAssistantConfig& config) {
  require_finite_config(config);
  validate_analysis_inputs(tracks, profiles);

  MixProfile mix;
  mix.track_count = static_cast<int>(profiles.size());
  mix.source_strip_ids.reserve(profiles.size());
  for (const TrackProfile& profile : profiles) {
    mix.source_strip_ids.push_back(profile.strip_id);
  }
  if (profiles.empty()) return mix;

  // Each pass is run only for the domains that read its result, because
  // switching a domain off is how a caller asks not to pay for it. Band
  // dominance is the one measurement two domains share; everything else here
  // exists for the image domain alone, and the pairwise alignment inside it is
  // the most expensive thing the assistant does.
  // Cross-track energy comparisons are taken at the levels the gain stage trims
  // each track to, since every decision reading them acts after the trim.
  const std::vector<float> trims =
      staged_input_trims(profiles, config.enable_gain ? decide_gain_staging(profiles, config)
                                                      : std::vector<SceneDelta>{});
  mix.analysis_input_trim_db = trims;
  mix.dominance_measured = config.enable_eq || config.enable_dynamics;
  if (config.enable_eq || config.enable_dynamics) {
    mix.dominance = analyze_band_dominance(profiles, trims);
  }
  if (config.enable_image) {
    mix.alignment = analyze_phase_alignment(tracks, profiles);
    // Both image passes read the same per-channel band energies, so they are
    // measured once and handed to each rather than transformed twice.
    const std::vector<TrackChannelEnergy> energy = measure_track_channel_energy(tracks, profiles);
    mix.channel_energy = energy;
    std::vector<TrackChannelEnergy> staged = project_channel_energy(energy, trims);
    mix.image = analyze_image_occupancy(staged);
    // Mono risk reads each track's own channel ratios, which a gain does not move.
    mix.mono_risks = analyze_mono_risks(tracks, profiles, energy);
  }
  return mix;
}

MixAssistantResult suggest_scene(const std::vector<TrackInput>& tracks,
                                 const MixAssistantConfig& config) {
  require_finite_config(config);
  require_unique_ids(tracks);

  TrackProfileConfig profile_config;
  profile_config.n_fft = config.n_fft;
  profile_config.hop_length = config.hop_length;

  // Profiling classifies as it measures, so the decomposed
  // analyze_track_profiles -> analyze_mix_profile -> suggest_scene path reaches
  // the same result as this one without a step the caller has to discover.
  std::vector<TrackProfile> profiles = analyze_track_profiles(tracks, profile_config);
  if (!any_usable(profiles)) {
    MixAssistantResult result;
    result.tracks = std::move(profiles);
    result.mix.track_count = static_cast<int>(result.tracks.size());
    return result;
  }
  const MixProfile mix = analyze_mix_profile(tracks, profiles, config);
  return suggest_scene(profiles, mix, config);
}

MixAssistantResult suggest_scene(const std::vector<TrackProfile>& profiles, const MixProfile& mix,
                                 const MixAssistantConfig& config) {
  require_finite_config(config);
  require_unique_profile_ids(profiles);

  MixProfile effective_mix = mix;
  if (!mix.source_strip_ids.empty()) {
    SONARE_CHECK_MSG(mix.track_count == static_cast<int>(profiles.size()),
                     ErrorCode::InvalidParameter,
                     "mixing assistant cached track count does not match profiles");
    SONARE_CHECK_MSG(mix.source_strip_ids.size() == profiles.size(), ErrorCode::InvalidParameter,
                     "mixing assistant cached profile identity count does not match profiles");
    for (std::size_t index = 0; index < profiles.size(); ++index) {
      SONARE_CHECK_MSG(mix.source_strip_ids[index] == profiles[index].strip_id,
                       ErrorCode::InvalidParameter,
                       "mixing assistant cached profile identities must match in order");
    }
    SONARE_CHECK_MSG(mix.analysis_input_trim_db.size() == profiles.size(),
                     ErrorCode::InvalidParameter,
                     "mixing assistant cached input trim count does not match profiles");

    const std::vector<float> current_trims =
        staged_input_trims(profiles, config.enable_gain ? decide_gain_staging(profiles, config)
                                                        : std::vector<SceneDelta>{});
    const bool input_trims_changed = trims_differ(mix.analysis_input_trim_db, current_trims);
    effective_mix.analysis_input_trim_db = current_trims;

    // Re-sweep dominance from cached envelopes only when it was measured.
    if (config.enable_eq || config.enable_dynamics) {
      if (!mix.dominance_measured) {
        effective_mix.dominance.clear();
      } else if (input_trims_changed || effective_mix.dominance.empty()) {
        effective_mix.dominance = analyze_band_dominance(profiles, current_trims);
      }
    } else {
      effective_mix.dominance.clear();
    }

    // Reproject image occupancy; alignment and mono risks are level-invariant.
    if (!config.enable_image) {
      cache_image_measurements(effective_mix);
      effective_mix.image = ImageOccupancy{};
    } else if (mix.channel_energy.size() != profiles.size()) {
      clear_image_measurements(effective_mix);
      effective_mix.image = ImageOccupancy{};
    } else {
      restore_image_measurements(effective_mix);
      if (input_trims_changed || effective_mix.image.histogram.empty()) {
        effective_mix.image =
            analyze_image_occupancy(project_channel_energy(mix.channel_energy, current_trims));
      }
    }
  }

  MixAssistantResult result;
  result.tracks = profiles;
  result.mix = effective_mix;
  if (profiles.empty() || !any_usable(profiles)) {
    return result;
  }

  // A disabled domain is skipped rather than evaluated and discarded: the
  // reason to switch one off is usually that it is the expensive one.
  std::vector<SceneDelta> deltas;
  if (config.enable_structure) append(deltas, decide_structure(profiles, effective_mix, config));
  if (config.enable_gain) append(deltas, decide_gain_staging(profiles, config));
  if (config.enable_balance) append(deltas, decide_balance(profiles, config));
  if (config.enable_eq) append(deltas, decide_eq(profiles, effective_mix, config));
  if (config.enable_dynamics) {
    append(deltas,
           decide_dynamics(profiles, effective_mix, config, staged_input_trims(profiles, deltas)));
  }
  if (config.enable_image) append(deltas, decide_image(profiles, effective_mix, config));

  api::Scene base = empty_scene_for(profiles);
  std::vector<std::string> notes;
  result.scene = apply_deltas(base, deltas, &notes);

  // Master headroom is not a sixth decision domain: it is a composition-level
  // consequence of the others, in the same way the range clamp inside
  // apply_deltas is. It can only be computed once every gain contribution has
  // been summed, and it changes nothing any domain decided — it moves the
  // output, not the mix. The number itself comes from the gain-staging module
  // rather than being invented here.
  if (config.enable_gain) {
    for (const auto& profile : profiles) {
      if (profile.usable || profile.peak_measured) continue;
      notes.push_back("the master headroom estimate leaves out " + profile.strip_id +
                      " because its peak could not be measured (" + profile.exclusion_reason + ")");
    }
    const float headroom_db = decide_master_headroom_db(profiles, result.scene, config);
    if (headroom_db < 0.0f) {
      const auto master = std::find_if(result.scene.buses.begin(), result.scene.buses.end(),
                                       [](const api::Bus& bus) { return bus.role == "master"; });
      if (master != result.scene.buses.end()) {
        master->input_trim_db += headroom_db;
        notes.push_back("pulled the master bus down to leave the summed mix its headroom");
      } else {
        // The master bus is a structure decision; without one there is nowhere to put the trim.
        notes.push_back("the summed mix needs " + util::format_fixed(-headroom_db, 1) +
                        " dB less level to keep its headroom, but no master bus was suggested to "
                        "carry that trim because the structure domain is switched off");
      }
    }
  }

  // The explanation is the deltas' own reasons in application order, never a
  // re-summary. Reading it top to bottom retraces how the scene was built.
  const std::vector<const SceneDelta*> ordered = detail::ordered_deltas(deltas);
  result.explanation.reserve(ordered.size() + notes.size());
  for (const SceneDelta* delta : ordered) {
    if (!delta->reason.empty()) result.explanation.push_back(delta->reason);
  }
  for (auto& text : notes) result.explanation.push_back(std::move(text));

  return result;
}

std::string mix_assistant_result_to_json(const MixAssistantResult& result) {
  namespace json = sonare::util::json;

  // Parsed back into a tree so the scene nests as a real object rather than an
  // escaped string. scene_to_json uses the same writer, so the round trip is
  // lossless and locale-safe.
  json::Value scene = json::parse(api::scene_to_json(result.scene));

  json::Array tracks;
  tracks.reserve(result.tracks.size());
  for (const auto& track : result.tracks) tracks.emplace_back(track_to_value(track));

  json::Array explanation;
  explanation.reserve(result.explanation.size());
  for (const auto& line : result.explanation) explanation.emplace_back(json::Value(line));

  json::Object root;
  util::json::put(root, "scene", std::move(scene));
  util::json::put(root, "tracks", std::move(tracks));
  util::json::put(root, "mix", mix_to_value(result.mix, result.tracks));
  util::json::put(root, "explanation", std::move(explanation));
  return json::dump(json::Value(std::move(root)));
}

const std::vector<std::string>& mix_assistant_result_schema_paths() {
  static const std::vector<std::string> paths = {
      "scene",
      "scene.version",
      "scene.strips",
      "scene.strips[].id",
      "scene.strips[].inputTrimDb",
      "scene.strips[].faderDb",
      "scene.strips[].vcaOffsetDb",
      "scene.strips[].pan",
      "scene.strips[].width",
      "scene.strips[].muted",
      "scene.strips[].soloed",
      "scene.strips[].soloSafe",
      "scene.strips[].panMode",
      "scene.strips[].dualPanLeft",
      "scene.strips[].dualPanRight",
      "scene.strips[].polarityInvertLeft",
      "scene.strips[].polarityInvertRight",
      "scene.strips[].panLaw",
      "scene.strips[].channelDelaySamples",
      "scene.strips[].sourceLayout",
      "scene.strips[].surroundPan",
      "scene.strips[].surroundPan.azimuth",
      "scene.strips[].surroundPan.elevation",
      "scene.strips[].surroundPan.divergence",
      "scene.strips[].surroundPan.lfe",
      "scene.strips[].surroundPan.distance",
      "scene.strips[].metering",
      "scene.strips[].metering.enabled",
      "scene.strips[].metering.lufs",
      "scene.strips[].metering.truePeak",
      "scene.strips[].metering.truePeakOversample",
      "scene.strips[].inserts",
      "scene.strips[].inserts[].slot",
      "scene.strips[].inserts[].processor",
      "scene.strips[].inserts[].params",
      "scene.strips[].inserts[].sidechainKey",
      "scene.strips[].sends",
      "scene.strips[].sends[].id",
      "scene.strips[].sends[].destinationBusId",
      "scene.strips[].sends[].sendDb",
      "scene.strips[].sends[].timing",
      "scene.strips[].eq",
      "scene.strips[].eq.enabled",
      "scene.strips[].eq.bands",
      "scene.strips[].eq.bands[].type",
      "scene.strips[].eq.bands[].frequencyHz",
      "scene.strips[].eq.bands[].gainDb",
      "scene.strips[].eq.bands[].q",
      "scene.strips[].eq.bands[].enabled",
      "scene.strips[].eq.bands[].coeffMode",
      "scene.strips[].eq.bands[].slopeDbOct",
      "scene.strips[].eq.bands[].placement",
      "scene.strips[].eq.bands[].phase",
      "scene.strips[].eq.bands[].soloed",
      "scene.strips[].eq.bands[].bypassed",
      "scene.strips[].eq.bands[].proportionalQ",
      "scene.strips[].eq.bands[].proportionalQStrength",
      "scene.strips[].eq.bands[].dynamic",
      "scene.strips[].eq.bands[].thresholdDb",
      "scene.strips[].eq.bands[].autoThreshold",
      "scene.strips[].eq.bands[].ratio",
      "scene.strips[].eq.bands[].rangeDb",
      "scene.strips[].eq.bands[].attackMs",
      "scene.strips[].eq.bands[].releaseMs",
      "scene.strips[].eq.bands[].detectorDelayMs",
      "scene.strips[].eq.bands[].externalSidechain",
      "scene.strips[].eq.bands[].sidechainFreqHz",
      "scene.strips[].eq.bands[].sidechainQ",
      "scene.buses",
      "scene.buses[].id",
      "scene.buses[].role",
      "scene.buses[].layout",
      "scene.buses[].inputTrimDb",
      "scene.buses[].width",
      "scene.buses[].polarityInvertLeft",
      "scene.buses[].polarityInvertRight",
      "scene.buses[].pan",
      "scene.buses[].panMode",
      "scene.buses[].dualPanLeft",
      "scene.buses[].dualPanRight",
      "scene.buses[].panLaw",
      "scene.buses[].inserts",
      "scene.buses[].inserts[].slot",
      "scene.buses[].inserts[].processor",
      "scene.buses[].inserts[].params",
      "scene.buses[].inserts[].sidechainKey",
      "scene.buses[].eq",
      "scene.buses[].eq.enabled",
      "scene.buses[].eq.bands",
      "scene.buses[].eq.bands[].type",
      "scene.buses[].eq.bands[].frequencyHz",
      "scene.buses[].eq.bands[].gainDb",
      "scene.buses[].eq.bands[].q",
      "scene.buses[].eq.bands[].enabled",
      "scene.buses[].eq.bands[].coeffMode",
      "scene.buses[].eq.bands[].slopeDbOct",
      "scene.buses[].eq.bands[].placement",
      "scene.buses[].eq.bands[].phase",
      "scene.buses[].eq.bands[].soloed",
      "scene.buses[].eq.bands[].bypassed",
      "scene.buses[].eq.bands[].proportionalQ",
      "scene.buses[].eq.bands[].proportionalQStrength",
      "scene.buses[].eq.bands[].dynamic",
      "scene.buses[].eq.bands[].thresholdDb",
      "scene.buses[].eq.bands[].autoThreshold",
      "scene.buses[].eq.bands[].ratio",
      "scene.buses[].eq.bands[].rangeDb",
      "scene.buses[].eq.bands[].attackMs",
      "scene.buses[].eq.bands[].releaseMs",
      "scene.buses[].eq.bands[].detectorDelayMs",
      "scene.buses[].eq.bands[].externalSidechain",
      "scene.buses[].eq.bands[].sidechainFreqHz",
      "scene.buses[].eq.bands[].sidechainQ",
      "scene.vcaGroups",
      "scene.vcaGroups[].id",
      "scene.vcaGroups[].gainDb",
      "scene.vcaGroups[].members",
      "scene.connections",
      "scene.connections[].source",
      "scene.connections[].destination",
      "tracks",
      "tracks[].stripId",
      "tracks[].name",
      "tracks[].source",
      "tracks[].sourceConfidence",
      "tracks[].usable",
      "tracks[].exclusionReason",
      "tracks[].channelCount",
      "tracks[].durationSec",
      "tracks[].integratedLufs",
      "tracks[].truePeakDb",
      "tracks[].crestFactorDb",
      "tracks[].spectralCentroidHz",
      "tracks[].spectralFlatness",
      "tracks[].attackDensity",
      "tracks[].sustainRatio",
      "tracks[].bandOccupancy",
      "tracks[].bandOccupancy.sub",
      "tracks[].bandOccupancy.low",
      "tracks[].bandOccupancy.lowMid",
      "tracks[].bandOccupancy.mid",
      "tracks[].bandOccupancy.highMid",
      "tracks[].bandOccupancy.high",
      "tracks[].bandOccupancy.air",
      "mix",
      "mix.trackCount",
      "mix.bandDominance",
      "mix.bandDominance[].masker",
      "mix.bandDominance[].maskee",
      "mix.bandDominance[].band",
      "mix.bandDominance[].ratio",
      "mix.bandDominance[].validFrames",
      "mix.alignment",
      "mix.alignment[].reference",
      "mix.alignment[].target",
      "mix.alignment[].lagSamples",
      "mix.alignment[].correlation",
      "mix.alignment[].polarityOpposed",
      "mix.crowdedBands",
      "mix.crowdedBands[].band",
      "mix.crowdedBands[].crowding",
      "mix.monoRisks",
      "mix.monoRisks[].stripId",
      "mix.monoRisks[].correlation",
      "mix.monoRisks[].width",
      "mix.monoRisks[].wideLowEnd",
      "explanation",
  };
  return paths;
}

}  // namespace sonare::mixing::assistant
