#pragma once

/// @file chord_tone_generator.h
/// @brief Built-in INoteGenerator: voices the project's chord symbols as notes.
///
/// It reads the harmonic timeline and writes one block of chord tones per chord
/// symbol -- a comp part when several voices are asked for, a bass line when one
/// is. It invents no harmony: a span the timeline does not cover produces
/// nothing, because a chord nobody annotated is not one this module will guess.
///
/// Proposes only, and proposes content rather than structure, on the same terms
/// as its sibling; the family's contract is in sonare_c_assist.h.
///
/// @note `INoteGenerator::generate` receives no query context, unlike
/// `ICounterpointEngine::derive`. That is the seam's shape, not a choice made
/// here, so this module takes its harmony and placement modules at construction
/// instead. Passing neither installs the built-in pair, which means a host that
/// registered its own IHarmonyContext does NOT change what this module reads
/// unless it also passes it here.
///
/// Control/offline thread only.

#include "arrangement/project_view.h"
#include "midi/assist/i_harmony_context.h"
#include "midi/assist/i_note_generator.h"
#include "midi/assist/i_note_placement_judge.h"
#include "midi/assist/modules/dissonance_analyzer.h"
#include "midi/assist/modules/harmony_context.h"
#include "midi/assist/modules/placement_judge.h"

namespace sonare::midi::assist::modules {

struct ChordToneGeneratorConfig {
  /// Chord tones voiced per chord, counted upward from the lowest one that fits
  /// the voice. 1 is a bass line; 3 is a triad. Clamped to what the chord has.
  int voice_count = 3;
};

class ChordToneGenerator final : public INoteGenerator {
 public:
  /// @param harmony Non-owning, or NULL for the built-in timeline reader.
  /// @param judge Non-owning, or NULL for the built-in range/scale judge.
  explicit ChordToneGenerator(const IHarmonyContext* harmony = nullptr,
                              const INotePlacementJudge* judge = nullptr,
                              ChordToneGeneratorConfig config = {}) noexcept;

  const char* module_id() const noexcept override { return kModuleId; }

  /// @brief Voices every chord the timeline covers inside the request's scope.
  /// @details `request.params_json` is read as @ref GeneratorParams: it must
  ///          name `target_clip_id`, and may narrow the voice with
  ///          `low_note` / `high_note` and set `base_velocity`.
  /// @return An AssistResult carrying at most one PatchMidiClip, a JSON
  ///         candidate payload of the per-note decisions, and diagnostics.
  ///         Never throws for a malformed request -- it comes back empty with a
  ///         reason, because a throw is discarded by the driver and the reason
  ///         would be lost with it.
  AssistResult generate(const arrangement::ProjectView& view,
                        const AssistRequest& request) override;

  static constexpr const char* kModuleId = "sonare.builtin.chord_tone_generator";

 private:
  // Declaration order is load-bearing: harmony_ resolves to the injected context
  // or to own_harmony_, and the owned dissonance analyzer and judge are then
  // built ON harmony_ -- so injecting a harmony context reaches the placement
  // rules too, rather than leaving them reading a second, built-in timeline.
  TimelineHarmonyContext own_harmony_{};
  const IHarmonyContext* harmony_ = nullptr;
  IntervalDissonanceAnalyzer own_dissonance_;
  RangeScaleJudge own_judge_;
  const INotePlacementJudge* judge_ = nullptr;
  ChordToneGeneratorConfig config_{};
};

}  // namespace sonare::midi::assist::modules
