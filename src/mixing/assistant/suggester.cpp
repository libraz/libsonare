/// @file suggester.cpp
/// @brief Mixing assistant pipeline: analyse, decide, compose.

#include "mixing/assistant/suggester.h"

#include <algorithm>
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
#include "util/exception.h"
#include "util/json.h"

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
    SONARE_CHECK_MSG(seen.insert(track.id).second, ErrorCode::InvalidParameter,
                     "duplicate mixing assistant track id '" + track.id + "'");
  }
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
  object.emplace("stripId", util::json::Value(profile.strip_id));
  object.emplace("name", util::json::Value(profile.name));
  object.emplace("source", util::json::Value(source_class_to_string(profile.source)));
  object.emplace("sourceConfidence", util::json::Value(profile.source_confidence));
  object.emplace("usable", util::json::Value(profile.usable));
  object.emplace("exclusionReason", util::json::Value(profile.exclusion_reason));
  object.emplace("channelCount", util::json::Value(profile.channel_count));
  object.emplace("durationSec", util::json::Value(profile.duration_sec));
  object.emplace("integratedLufs", util::json::Value(profile.base.loudness.integrated_lufs));
  object.emplace("truePeakDb", util::json::Value(profile.base.loudness.true_peak_db));
  object.emplace("crestFactorDb", util::json::Value(profile.base.loudness.crest_factor_db));
  object.emplace("spectralCentroidHz", util::json::Value(profile.base.spectral.centroid_hz));
  object.emplace("spectralFlatness", util::json::Value(profile.base.spectral.flatness));
  object.emplace("attackDensity", util::json::Value(profile.base.dynamics.attack_density));
  object.emplace("sustainRatio", util::json::Value(profile.base.dynamics.sustain_ratio));
  object.emplace("bandOccupancy", band_array(profile.band_occupancy));
  return util::json::Value(std::move(object));
}

util::json::Value mix_to_value(const MixProfile& mix, const std::vector<TrackProfile>& profiles) {
  util::json::Object object;
  object.emplace("trackCount", util::json::Value(mix.track_count));

  // The dominance matrix is emitted only where it is actually informative:
  // a full N^2 x 7 dump is mostly zeros and mostly noise for a reader.
  util::json::Array dominance;
  for (int masker = 0; masker < mix.track_count; ++masker) {
    for (int maskee = 0; maskee < mix.track_count; ++maskee) {
      if (masker == maskee) continue;
      for (int band = 0; band < kBandCount; ++band) {
        const BandDominance entry = mix.dominance_at(masker, maskee, band);
        if (entry.valid_frames == 0) continue;
        util::json::Object row;
        row.emplace("masker",
                    util::json::Value(profiles[static_cast<std::size_t>(masker)].strip_id));
        row.emplace("maskee",
                    util::json::Value(profiles[static_cast<std::size_t>(maskee)].strip_id));
        row.emplace("band", util::json::Value(kBandNames[static_cast<std::size_t>(band)]));
        row.emplace("ratio", util::json::Value(entry.ratio));
        row.emplace("validFrames", util::json::Value(entry.valid_frames));
        dominance.emplace_back(util::json::Value(std::move(row)));
      }
    }
  }
  object.emplace("bandDominance", util::json::Value(std::move(dominance)));

  util::json::Array alignment;
  for (const auto& pair : mix.alignment) {
    if (!pair.related) continue;
    util::json::Object row;
    row.emplace(
        "reference",
        util::json::Value(profiles[static_cast<std::size_t>(pair.reference_index)].strip_id));
    row.emplace("target",
                util::json::Value(profiles[static_cast<std::size_t>(pair.target_index)].strip_id));
    row.emplace("lagSamples", util::json::Value(pair.lag_samples));
    row.emplace("correlation", util::json::Value(pair.correlation));
    row.emplace("polarityOpposed", util::json::Value(pair.polarity_opposed));
    alignment.emplace_back(util::json::Value(std::move(row)));
  }
  object.emplace("alignment", util::json::Value(std::move(alignment)));

  util::json::Array crowded;
  for (int band = 0; band < kBandCount; ++band) {
    if (band >= static_cast<int>(mix.image.crowded.size())) break;
    if (!mix.image.crowded[static_cast<std::size_t>(band)]) continue;
    util::json::Object row;
    row.emplace("band", util::json::Value(kBandNames[static_cast<std::size_t>(band)]));
    row.emplace("crowding", util::json::Value(mix.image.crowding[static_cast<std::size_t>(band)]));
    crowded.emplace_back(util::json::Value(std::move(row)));
  }
  object.emplace("crowdedBands", util::json::Value(std::move(crowded)));

  util::json::Array mono_risks;
  for (const auto& risk : mix.mono_risks) {
    util::json::Object row;
    row.emplace("stripId", util::json::Value(risk.strip_id));
    row.emplace("correlation", util::json::Value(risk.correlation));
    row.emplace("width", util::json::Value(risk.width));
    row.emplace("wideLowEnd", util::json::Value(risk.wide_low_end));
    mono_risks.emplace_back(util::json::Value(std::move(row)));
  }
  object.emplace("monoRisks", util::json::Value(std::move(mono_risks)));

  return util::json::Value(std::move(object));
}

}  // namespace

MixProfile analyze_mix_profile(const std::vector<TrackInput>& tracks,
                               const std::vector<TrackProfile>& profiles,
                               const MixAssistantConfig& config) {
  MixProfile mix;
  mix.track_count = static_cast<int>(profiles.size());
  if (profiles.empty()) return mix;

  // Each pass is run only for the domains that read its result, because
  // switching a domain off is how a caller asks not to pay for it. Band
  // dominance is the one measurement two domains share; everything else here
  // exists for the image domain alone, and the pairwise alignment inside it is
  // the most expensive thing the assistant does.
  if (config.enable_eq || config.enable_dynamics) {
    mix.dominance = analyze_band_dominance(profiles);
  }
  if (config.enable_image) {
    mix.alignment = analyze_phase_alignment(tracks, profiles);
    // Both image passes read the same per-channel band energies, so they are
    // measured once and handed to each rather than transformed twice.
    const std::vector<TrackChannelEnergy> energy = measure_track_channel_energy(tracks, profiles);
    mix.image = analyze_image_occupancy(energy);
    mix.mono_risks = analyze_mono_risks(tracks, profiles, energy);
  }
  return mix;
}

MixAssistantResult suggest_scene(const std::vector<TrackInput>& tracks,
                                 const MixAssistantConfig& config) {
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
  MixAssistantResult result;
  result.tracks = profiles;
  result.mix = mix;
  if (profiles.empty() || !any_usable(profiles)) {
    return result;
  }

  // A disabled domain is skipped rather than evaluated and discarded: the
  // reason to switch one off is usually that it is the expensive one.
  std::vector<SceneDelta> deltas;
  if (config.enable_structure) append(deltas, decide_structure(profiles, mix, config));
  if (config.enable_gain) append(deltas, decide_gain_staging(profiles, config));
  if (config.enable_balance) append(deltas, decide_balance(profiles, config));
  if (config.enable_eq) append(deltas, decide_eq(profiles, mix, config));
  if (config.enable_dynamics) append(deltas, decide_dynamics(profiles, mix, config));
  if (config.enable_image) append(deltas, decide_image(profiles, mix, config));

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
    const float headroom_db = decide_master_headroom_db(profiles, result.scene, config);
    if (headroom_db < 0.0f) {
      for (auto& bus : result.scene.buses) {
        if (bus.role != "master") continue;
        bus.input_trim_db += headroom_db;
        notes.push_back("pulled the master bus down to leave the summed mix its headroom");
        break;
      }
    }
  }

  // The explanation is the deltas' own reasons in application order, never a
  // re-summary. Reading it top to bottom retraces how the scene was built.
  std::vector<const SceneDelta*> ordered;
  ordered.reserve(deltas.size());
  for (const auto& delta : deltas) ordered.push_back(&delta);
  std::stable_sort(ordered.begin(), ordered.end(), [](const SceneDelta* a, const SceneDelta* b) {
    return static_cast<int>(a->domain) < static_cast<int>(b->domain);
  });
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
  root.emplace("scene", std::move(scene));
  root.emplace("tracks", json::Value(std::move(tracks)));
  root.emplace("mix", mix_to_value(result.mix, result.tracks));
  root.emplace("explanation", json::Value(std::move(explanation)));
  return json::dump(json::Value(std::move(root)));
}

}  // namespace sonare::mixing::assistant
