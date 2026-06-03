#pragma once

/// @file assist_registry.h
/// @brief assist seam: control-thread-only, non-owning module registry.
///
/// The AssistRegistry holds raw (non-owning) pointers to the assist module
/// implementations the host has installed. It is CONTROL-THREAD-ONLY: no
/// internal locking, no I/O, no clock/random. The host owns the module objects
/// and guarantees they outlive the registry.
///
/// CRITICAL: the Project / engine / RT path do NOT reference this registry. It
/// is driven only from an explicit control/offline path (CompositionAssist).
/// When SONARE_WITH_ASSIST is OFF this type does not exist on the core surface.

#include "midi/assist/i_counterpoint_engine.h"
#include "midi/assist/i_dissonance_analyzer.h"
#include "midi/assist/i_harmony_context.h"
#include "midi/assist/i_note_generator.h"
#include "midi/assist/i_note_placement_judge.h"
#include "midi/assist/i_rhythm_generator.h"

namespace sonare::midi::assist {

/// @brief Non-owning registry of installed assist module slots.
///
/// Each slot holds a single raw pointer (nullptr = unregistered, which the
/// driver treats as a no-op). Setters replace the current pointer; clear()
/// drops all slots. The registry never deletes the pointees.
class AssistRegistry {
 public:
  AssistRegistry() = default;

  // ---- Typed setters (replace the slot; nullptr clears that slot) ----------

  void register_generator(INoteGenerator* generator) noexcept { generator_ = generator; }
  void register_judge(INotePlacementJudge* judge) noexcept { judge_ = judge; }
  void register_harmony_context(IHarmonyContext* harmony) noexcept { harmony_ = harmony; }
  void register_dissonance_analyzer(IDissonanceAnalyzer* analyzer) noexcept {
    dissonance_ = analyzer;
  }
  void register_counterpoint(ICounterpointEngine* counterpoint) noexcept {
    counterpoint_ = counterpoint;
  }
  void register_rhythm(IRhythmGenerator* rhythm) noexcept { rhythm_ = rhythm; }

  // ---- Slot accessors (nullptr = unregistered) -----------------------------

  INoteGenerator* generator() const noexcept { return generator_; }
  INotePlacementJudge* judge() const noexcept { return judge_; }
  IHarmonyContext* harmony_context() const noexcept { return harmony_; }
  IDissonanceAnalyzer* dissonance_analyzer() const noexcept { return dissonance_; }
  ICounterpointEngine* counterpoint() const noexcept { return counterpoint_; }
  IRhythmGenerator* rhythm() const noexcept { return rhythm_; }

  /// @brief Drops every registered slot (does not delete the pointees).
  void clear() noexcept {
    generator_ = nullptr;
    judge_ = nullptr;
    harmony_ = nullptr;
    dissonance_ = nullptr;
    counterpoint_ = nullptr;
    rhythm_ = nullptr;
  }

 private:
  INoteGenerator* generator_ = nullptr;
  INotePlacementJudge* judge_ = nullptr;
  IHarmonyContext* harmony_ = nullptr;
  IDissonanceAnalyzer* dissonance_ = nullptr;
  ICounterpointEngine* counterpoint_ = nullptr;
  IRhythmGenerator* rhythm_ = nullptr;
};

}  // namespace sonare::midi::assist
