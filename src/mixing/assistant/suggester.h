#pragma once

/// @file suggester.h
/// @brief Rule-based mixing assistant: analyses tracks and suggests a scene.
///
/// @details **Offline / control thread only.** Nothing here is realtime-safe.
///          The whole pipeline allocates, runs an STFT per track and evaluates
///          every track pair. Calling it from `process()` will drop audio.
///
/// @details **The assistant suggests; it does not apply.** It returns a
///          @ref mixing::api::Scene and an explanation, and never touches or
///          emits audio. A caller who wants the suggestion realised serialises
///          the scene and hands it to the mixer as its own explicit step. This
///          separation is a product decision — a mix has no single right answer,
///          so nothing is applied behind the user's back — and there is
///          deliberately no convenience entry point that collapses the two
///          halves into one call.
///
/// @details Every decision is rule-based. There is no trained model, no
///          statistical classifier and no learned parameter anywhere in the
///          module; source classification is a single-layer decision table over
///          measured features.

#include <cstddef>
#include <string>
#include <vector>

#include "mixing/api/scene.h"
#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Tunables for @ref suggest_scene.
/// @details Flat by design: every field marshals to one scalar or string, which
///          keeps the binding surfaces mechanical.
struct MixAssistantConfig {
  /// @brief Absolute integrated-loudness target each track is staged towards, in LUFS.
  /// @details A fixed absolute target, not an average taken over the tracks
  ///          present. Normalising to a stated absolute level is the same
  ///          operation broadcast and streaming delivery has performed for
  ///          years; making the target follow the material would instead
  ///          couple every track's staging to which other tracks happen to be
  ///          loaded.
  float target_track_lufs = -18.0f;

  /// @brief Overall strength of the suggestion, in `[0, 1]`.
  /// @details Scales every level-like decision. 0 produces an empty
  ///          suggestion, 1 the full one.
  float suggestion_strength = 1.0f;

  /// @brief Largest cut a single suggested EQ band may apply, in dB.
  /// @details Over-carving is the most common complaint against automatic EQ,
  ///          so the ceiling is conservative and adjustable rather than
  ///          implicit.
  float eq_max_cut_db = 4.0f;

  /// @brief Headroom the summed mix is left with on the master bus, in dBTP.
  /// @details Staging every track to the same absolute target makes the sum of
  ///          them hot, so the master carries a static trim that brings the
  ///          estimated peak down to this level. Well below full scale on
  ///          purpose: a mix is handed on to a mastering stage, and the estimate
  ///          cannot see effect returns or insert make-up gain, so the margin is
  ///          where the unmodelled part goes.
  float mix_bus_headroom_dbtp = -6.0f;

  /// @name Per-domain switches
  /// @brief A disabled domain is not evaluated at all.
  /// @details Skipping the work rather than discarding the result matters: the
  ///          reason to switch a domain off is usually that it is the expensive
  ///          one.
  /// @{
  bool enable_structure = true;
  bool enable_gain = true;
  bool enable_balance = true;
  bool enable_eq = true;
  bool enable_dynamics = true;
  bool enable_image = true;
  /// @}

  /// @brief Suggest a high-pass filter on tracks carrying residue below their
  ///        register.
  /// @details **Off by default.** A survey of mixing best practices tested the
  ///          rule that every track without low-frequency content should be
  ///          high-passed and found it seldom used in studio mixing and
  ///          unsupported by subjective testing; the habit belongs to live
  ///          sound, where the filter protects a system from stage rumble —
  ///          P. Pestana and J. D. Reiss, "Intelligent Audio Production
  ///          Strategies Informed by Best Practices", AES 53rd International
  ///          Conference on Semantic Audio, London, 2014.
  ///
  ///          It is offered at all because the assistant only suggests: nothing
  ///          is applied behind the user's back, so a user who wants the filter
  ///          should be able to ask for it. Switched on, the filter is proposed
  ///          from the track's measured low-frequency content rather than from
  ///          its class, so a part that genuinely plays below its class's usual
  ///          register keeps what it plays.
  ///
  ///          Off is not "computed and discarded": the measurement is not taken
  ///          at all, the same way a disabled domain is not evaluated.
  bool enable_high_pass = false;

  /// @brief Shared STFT geometry for every track.
  int n_fft = 2048;
  int hop_length = 512;
};

/// @brief What the assistant produces.
struct MixAssistantResult {
  /// @brief The suggested scene. Apply it, or do not; nothing has happened yet.
  api::Scene scene{};
  /// @brief One entry per input track, in input order.
  std::vector<TrackProfile> tracks;
  /// @brief Cross-track measurements the decisions were made from.
  MixProfile mix{};
  /// @brief Human-readable reasons, in the order the changes were applied.
  /// @details Reading top to bottom retraces how the scene was built. This is
  ///          assembled from the individual deltas' reasons as they are applied
  ///          and is never re-summarised: a reason recovered after the fact is a
  ///          reason invented after the fact.
  std::vector<std::string> explanation;
};

/// @brief Measures everything that cannot be measured from one track alone.
/// @details Split out so a caller can analyse once and then re-run the decision
///          stages under different configurations without paying for the STFT
///          and the pairwise passes again.
/// @param tracks The same tracks @p profiles was built from, in the same order.
///        The time-alignment pass reads their sample buffers directly.
/// @param profiles Per-track profiles from @ref analyze_track_profiles.
/// @param config Assistant configuration; only the analysis geometry is read.
MixProfile analyze_mix_profile(const std::vector<TrackInput>& tracks,
                               const std::vector<TrackProfile>& profiles,
                               const MixAssistantConfig& config = {});

/// @brief Analyses the tracks and suggests a scene.
/// @details Degenerate input never throws. No tracks, all-silent tracks, a null
///          buffer or a non-positive sample rate all yield a result with an
///          empty scene and an empty explanation.
/// @param tracks Tracks to mix, planar, mono or stereo.
/// @param config Assistant configuration.
MixAssistantResult suggest_scene(const std::vector<TrackInput>& tracks,
                                 const MixAssistantConfig& config = {});

/// @brief Suggests a scene from measurements that have already been taken.
/// @details The decision half on its own. Given the profiles and mix profile
///          that @ref suggest_scene would have computed, this returns the same
///          scene without re-analysing anything.
///
///          There is no interleaved counterpart and no `Audio` overload the way
///          the mastering assistant has: @ref TrackInput is already planar and
///          already carries the channel count, so a stereo track reaches the
///          BS.1770 channel-summed loudness measurement without a separate
///          entry point.
MixAssistantResult suggest_scene(const std::vector<TrackProfile>& profiles, const MixProfile& mix,
                                 const MixAssistantConfig& config = {});

/// @brief Serialises a result to JSON.
/// @details Keys are camelCase, matching every other JSON surface in the
///          library. The scene nests as a real object rather than an embedded
///          string.
std::string mix_assistant_result_to_json(const MixAssistantResult& result);

}  // namespace sonare::mixing::assistant
