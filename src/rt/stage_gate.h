#pragma once

/// @file stage_gate.h
/// @brief The one way a processor takes a stage out of its signal path.

namespace sonare::rt {

/// @brief Owns whether a bypassable stage runs, and clears the stage's history on
///        the way back in.
/// @details A stage skipped while disabled, bypassed or emptied keeps whatever its
///   filters, delay lines and envelopes held when it stopped; running it again
///   from there replays audio from before the gap. The gate calls the stage's
///   reset exactly once on each inactive-to-active edge, so a stage cannot be
///   skipped without that history being dropped. RT-safe: no allocation.
class StageGate {
 public:
  /// @brief Whether the stage runs now. On an inactive-to-active edge @p reset
  ///        runs first, so the stage resumes from rest.
  template <typename Reset>
  bool admit(bool active, Reset&& reset) {
    if (active && !active_) reset();
    active_ = active;
    return active;
  }

  /// @brief Marks the stage as out of the path, so its next admission resets it.
  ///        For an owner that clears or reloads the stage outside process().
  void close() noexcept { active_ = false; }

  bool active() const noexcept { return active_; }

 private:
  bool active_ = false;
};

}  // namespace sonare::rt
