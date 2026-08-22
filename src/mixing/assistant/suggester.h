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
  /// @details Scales every level-like decision — trims, fader offsets, effect
  ///          send levels, EQ cut depths, compression ratios and gain ranges,
  ///          and how far a track is spread from the centre. 1 applies them in
  ///          full.
  ///
  ///          **0 is not an empty suggestion.** It is every level-like decision
  ///          taken and set to zero, which is a decision, plus everything that
  ///          is not a level and therefore does not scale: the bus topology and
  ///          routing, and the physical corrections the image stage makes for a
  ///          measured cancellation — a polarity inversion, an alignment delay
  ///          and a low-end mono fold, none of which is half-wrong at half
  ///          strength. Corrective EQ cuts and effect sends do reach zero and
  ///          disappear with it. If the intent is to suggest nothing at all,
  ///          switch the domains off rather than turning the strength down:
  ///          that skips the work as well.
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
  ///          one. The cross-track measurement each domain reads is skipped
  ///          with it, not only the decision — @ref analyze_mix_profile runs a
  ///          pass only for the domains that read its result.
  ///
  ///          What does *not* scale with these is the per-track profiling: one
  ///          STFT and one loudness measurement per track, which fill @ref
  ///          MixAssistantResult::tracks and are therefore part of the return
  ///          value rather than any one domain's private cost. Switching every
  ///          domain off leaves that and nothing else, so it is the floor a
  ///          caller budgets against.
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
///
///          Each pass here runs only for the domains that read its result, so a
///          @p config with a domain switched off is measured more cheaply as
///          well as decided more cheaply — which is the point of the switch.
///          Band dominance is shared by the EQ and dynamics domains; the
///          alignment, image and mono-fold passes exist for the image domain
///          alone, and the pairwise alignment among them is the most expensive
///          thing the assistant does. A @ref MixProfile measured with a domain
///          off therefore carries empty fields where that domain would have
///          read, and must not be reused for a later call that switches the
///          domain back on.
///
///          The analysis geometry is *not* read from here: each profile carries
///          the geometry it was measured with, and using anything else would
///          make a band energy computed here incomparable with the one the
///          profile already caches.
/// @param tracks The same tracks @p profiles was built from, in the same order.
///        The time-alignment pass reads their sample buffers directly.
/// @param profiles Per-track profiles from @ref analyze_track_profiles.
/// @param config Assistant configuration; the per-domain switches are read.
MixProfile analyze_mix_profile(const std::vector<TrackInput>& tracks,
                               const std::vector<TrackProfile>& profiles,
                               const MixAssistantConfig& config = {});

/// @brief Analyses the tracks and suggests a scene.
/// @details Degenerate input never throws. No tracks, all-silent tracks, a null
///          buffer, a non-positive sample rate or a non-finite sample all yield
///          a result with an empty scene and an empty explanation; each affected
///          track carries its own @ref TrackProfile::exclusion_reason.
///
///          Two tracks sharing a @ref TrackInput::id is the one input that is
///          rejected rather than absorbed, with
///          @ref ErrorCode::InvalidParameter. Absorbing it means shipping a
///          scene the mixer refuses to load, and the refusal names the scene
///          rather than the pair of tracks that collided.
/// @param tracks Tracks to mix, planar, mono or stereo. Ids must be unique.
/// @param config Assistant configuration.
/// @throws SonareException with @ref ErrorCode::InvalidParameter when two
///         tracks share an id.
MixAssistantResult suggest_scene(const std::vector<TrackInput>& tracks,
                                 const MixAssistantConfig& config = {});

/// @brief Suggests a scene from measurements that have already been taken.
/// @details The decision half on its own. Given the profiles and mix profile
///          that @ref suggest_scene would have computed, this returns the same
///          scene without re-analysing anything.
///
///          "That @ref suggest_scene would have computed" is load-bearing.
///          Profiles assembled by hand carry only what the caller filled in, and
///          two decisions read fields a hand-built profile is easy to leave
///          empty: without @ref TrackProfile::spectrum the corrective cuts fall
///          back to the band centres and **the high-pass is never proposed at
///          all**, whatever @ref MixAssistantConfig::enable_high_pass says. No
///          error is raised, because an unmeasured track has no measurement to
///          contradict. Pass profiles from @ref analyze_track_profiles to get the
///          documented behaviour.
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
