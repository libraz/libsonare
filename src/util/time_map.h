#pragma once

/// @file time_map.h
/// @brief Output-frame to input-frame map for time-scale modification.
///
/// A phase vocoder reads its analysis frames at `t_in = t_out * rate`. That form
/// carries one rate for a whole signal and cannot express a region held at unity
/// while its neighbours stretch. This replaces it with a cumulative map
/// `t_in(t_out)`, piecewise-affine and built from a rate profile laid out on the
/// **input** axis — the regions that motivate a profile are found in the analysis
/// frames, so an output-axis profile would need the map the profile defines.
///
/// The map is the only quantity in a stretch that carries history. Synthesis
/// phase does not: the integration step is `hop_length / sample_rate` and the
/// accumulator advances on instantaneous frequency alone, so no rate reaches it.
///
/// A rewind is unrepresentable. Every segment rate is positive and the input
/// breakpoints strictly increase, so `t_in` is strictly increasing in `t_out`,
/// and a consumer that discards input behind its current position — the
/// streaming vocoder does, irrecoverably — stays correct with no further check.

#include <cstddef>
#include <vector>

namespace sonare {

/// One constant-rate span of the profile, on the input frame axis.
struct TimeStretchSegment {
  /// First input frame the rate applies to. Zero for the first segment, finite
  /// and strictly increasing thereafter. The last segment extends to the end of
  /// the input. Finiteness is a separate requirement from strict increase: an
  /// infinite start satisfies the ordering and denotes no span at all.
  float input_start = 0.0f;
  /// Input frames consumed per output frame: > 1 shortens, < 1 lengthens, 1 holds.
  float rate = 1.0f;
};

/// @brief Cumulative input position as a function of output frame.
/// @details Construction validates the profile, so every query on a constructed
///          map is monotone by the type rather than by agreement with the caller.
///
///          Contract:
///          - A profile must hold at least one segment, `input_start` 0.0f on the
///            first, finite and strictly increasing afterwards, and a finite
///            positive `rate` on each. Anything else throws
///            `ErrorCode::InvalidParameter`.
///          - The **exact** position is strictly increasing in `output_frame`.
///            What `input_position` returns is therefore monotone
///            **non-decreasing** — narrowing to float is monotone but not
///            injective, so past 2^24 two adjacent frames can share a returned
///            position. Non-decreasing is what the retention invariant needs and
///            all it ever needed: the discard reads `floor(position)` and only
///            requires that it never moves backwards. Strictly increasing was a
///            strengthening with no consumer, and it contradicted the bit-for-bit
///            clause below, which promises exactly the non-injective `float(t) *
///            rate` at rate 1.0f.
///          - `output_frame` is non-negative. Nothing is defined below zero.
///          - A single-segment map is `constant()`; for it, `input_position(t)`
///            returns `static_cast<float>(t) * rate` — **the same expression the
///            callers use today**, not an equivalent one. Do not restate this as
///            a rounding argument: "an integer times a float needs 48 bits and is
///            exact in double" holds only while `t` is below 2^24, and `t` is an
///            `int`. Identity here is by construction.
///          - The two counts are defined on the **exact** position, the
///            piecewise-affine value before any narrowing, not on what
///            `input_position` returns. That distinction is the whole of their
///            correctness: the narrowed position can round *up* onto the input
///            end and report a frame that has not been reached, which disagrees
///            with today's `ceil(N/rate)` by one. On the exact value the two
///            coincide, so a `constant()` map equals
///            `numeric::checked_projected_count` on the same arguments and no
///            caller's output length moves. Where even that helper's own
///            quotient rounding parts from the exact value — `long double` is
///            `double` on arm64, and a ceiling of a rounded quotient is not
///            always the ceiling of the exact one — **the helper wins**. Not
///            moving a caller's output length is the point; matching the
///            definition is the means.
///          - Counts above `resource::kMaxOfflineAudioSamples` or `INT_MAX` throw
///            `ErrorCode::InvalidParameter`, as the scalar form already does.
class TimeStretchMap {
 public:
  /// Constant rate over the whole input. Equivalent to a one-segment profile.
  explicit TimeStretchMap(float rate);

  explicit TimeStretchMap(std::vector<TimeStretchSegment> segments);

  /// Rewrite the profile in place, validating as the constructors do and reusing
  /// the storage already held. These exist so a real-time holder can rebind
  /// without allocating: assigning a map built elsewhere reuses the destination's
  /// capacity but still allocates for the source, which is the whole cost. A
  /// profile that fits the capacity already reserved allocates nothing; one that
  /// does not grows once, as any container would.
  ///
  /// **Validate fully before touching either vector. A rejected `assign` leaves
  /// the map exactly as it was.** This is not a preference for the stronger
  /// guarantee. `input_position` is `noexcept` and its safety rests on the
  /// invariants the validation establishes, so the alternative to "the old
  /// profile survives" is not "some valid profile survives" — it is a map whose
  /// invariants no longer hold, with no way left to say so.
  void assign(float rate);
  void assign(const std::vector<TimeStretchSegment>& segments);

  /// Input frame position the given output frame reads from, in frames.
  /// Callers keep their own integer/fraction split and end-of-input clamping.
  float input_position(int output_frame) const noexcept;

  /// Smallest frame count `T` whose exact position reaches `input_frame_count`,
  /// which must be non-negative and yields zero at zero — the equality with
  /// `checked_projected_count` above covers that argument too. Callers keep their
  /// own `input_frames > 0` rejection and `max(1, .)` floor where they have them.
  int output_frame_count(int input_frame_count) const;

  /// Sample-domain inverse: smallest `S` whose exact position at output frame
  /// `S / hop_length` — real division, not integer — scaled back by
  /// `hop_length`, reaches `input_sample_count`. The hop cancels for a constant
  /// profile, which is why the scalar form never needed it.
  size_t output_sample_count(size_t input_sample_count, int hop_length) const;

  /// True when the profile holds exactly one segment. A count, not a test for
  /// equal rates: a two-segment profile at one rate is the same function and is
  /// not `constant()`, though it agrees with the scalar map at every position.
  bool constant() const noexcept;

  /// The rate of a `constant()` map. Undefined otherwise.
  float constant_rate() const noexcept;

  /// True when the two maps' affine pieces coincide over `[0,
  /// committed_output_frames]` — the exact positions, not what `input_position`
  /// narrows them to. A streaming caller re-binding a map must pass this against
  /// the bound one: frames already synthesized fix the input the stream has
  /// discarded, while anything above is still free to change.
  ///
  /// The interval is closed, and one frame past the last synthesized frame, on
  /// purpose: the retention decision reads the map at `next_output_frame_` and
  /// then erases, so that frame's position has already been spent irreversibly
  /// while its output frame has not been produced.
  ///
  /// The relation is coincidence of the affine pieces over the interval, which is
  /// decidable in the number of segments and is strictly stronger than agreement
  /// at the integer frames: it also rejects a profile that meets at every frame
  /// and diverges between them. The stronger side is deliberate, since the
  /// retention decision is not the map's only reader. A structural comparison of
  /// segment lists is cheaper and wrong — one segment at rate 1 and three
  /// segments all at rate 1 are the same function.
  bool agrees_through(const TimeStretchMap& other, int committed_output_frames) const noexcept;

 private:
  std::vector<TimeStretchSegment> segments_;
  /// Output-axis start of each segment, derived once at construction:
  /// `output_start_[i + 1] = output_start_[i] + (in_start[i + 1] - in_start[i]) / rate[i]`.
  std::vector<double> output_start_;
};

/// A span of input frames to leave at unity while its neighbours stretch.
struct HoldRange {
  int first_frame = 0;
  int frame_count = 0;
};

/// @brief Profile holding @p holds at unity and stretching the rest so the whole
///        spans what @p target_rate asks of @p input_frame_count.
/// @details Holding part of the input fixes part of the output, so the remainder
///          has to absorb the difference: the rate outside the holds is
///          `(N - H) / (N / target_rate - H)` for `H` held frames. It is not a
///          free parameter and a caller cannot set both.
///
///          Contract:
///          - Holds are on the **input** frame axis, sorted, non-overlapping,
///            non-empty, and inside `[0, input_frame_count)`. Anything else
///            throws `ErrorCode::InvalidParameter`, as does a non-positive
///            `input_frame_count` or a non-finite, non-positive `target_rate`.
///          - Satisfiable exactly when some positive rate solves
///            `H + (N - H) / r == N / target_rate`. That is two conditions, not
///            one: the held frames must be shorter than the requested output,
///            **and** there must be a frame left to stretch — holds covering the
///            whole input can only satisfy `target_rate == 1`, since with `N == H`
///            no `r` changes anything. Otherwise it throws. The request is
///            unsatisfiable rather than approximable, and silently returning a
///            different rate than the caller asked for is worse than refusing.
///          - The resulting map must give the same `output_frame_count` as
///            `TimeStretchMap(target_rate)` on the same input. This does not come
///            free from the algebra: `r` is stored as a `float`, so the cumulative
///            end cannot be made exactly `N / target_rate`, and a stored `r` that
///            rounds low overshoots and costs a whole frame at the ceiling.
///            Round `r` toward `+inf` — the end must land at or below the target,
///            and a larger `r` shortens `(N - H) / r`. Measured over a grid of
///            round `N`, `H` and `target_rate`: 131 of 594 disagree without it,
///            every one by exactly +1, and none with it.
///          - An empty hold list returns the one-segment profile at
///            `target_rate`, so the result is `constant()` and every existing
///            output is reproduced exactly.
std::vector<TimeStretchSegment> hold_profile(const std::vector<HoldRange>& holds,
                                             int input_frame_count, float target_rate);

/// Frames to hold from a note onset, for a note whose attack is not measured.
/// Reassigned-time concentration stays elevated three frames from the frame the
/// pitch track starts on, measured at 22050 Hz with a 2048-sample window and a
/// 512-sample hop, over attacks from 0 to 30 ms. The span is in frames and the
/// attacks are in milliseconds, so another framing covers a different duration
/// with the same count and the figure has to be re-derived rather than reused.
inline constexpr int kDefaultHoldFrames = 3;

}  // namespace sonare
