/// @file time_map_test.cpp
/// @brief Contract tests for TimeStretchMap, written against time_map.h alone.
///
/// The exact position and the narrowed one are different quantities and the
/// contract separates them: the counts read the exact value, `input_position`
/// returns a float, and only the second can repeat. Both are measured against the
/// same call wherever they can disagree, because the gap is the contract's own
/// reason for the distinction rather than a tolerance to absorb.
///
/// Where the contract is still silent -- a negative committed frame count, and
/// whether `agrees_through` decides on exact pieces or on returned positions --
/// nothing is asserted, and the report says so.

#include "util/time_map.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <climits>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/types.h"

using namespace sonare;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// The cap both scalar callers apply before projecting a count.
std::size_t count_limit() {
  return std::min<std::size_t>(kMaxAudioBufferSize, static_cast<std::size_t>(INT_MAX));
}

/// @brief The error code a call throws, or Ok when it does not throw.
template <typename Fn>
ErrorCode code_of(Fn&& fn) {
  try {
    fn();
  } catch (const SonareException& error) {
    return error.code();
  }
  return ErrorCode::Ok;
}

/// @brief The scalar form the contract promises a constant map reproduces.
float scalar_position(int output_frame, float rate) {
  return static_cast<float>(output_frame) * rate;
}

/// @brief `checked_projected_count` at the limit the scalar callers use.
std::size_t projected(std::size_t input_count, float rate) {
  std::size_t out = 0;
  REQUIRE(numeric::checked_projected_count(input_count, rate, count_limit(), &out));
  return out;
}

/// @brief Smallest T with `input_position(T) >= input_frame_count`, by search.
/// @details The definition `output_frame_count`'s own doc comment gives, computed
///          without reference to the projected-count form the contract also
///          claims it equals.
int smallest_reaching(const TimeStretchMap& map, int input_frame_count, int search_limit) {
  for (int t = 0; t <= search_limit; ++t) {
    if (map.input_position(t) >= static_cast<float>(input_frame_count)) return t;
  }
  return -1;
}

/// @brief Sweeps the returned position over `[0, last_frame]` and checks the
///        contract's monotonicity, returning how many adjacent pairs repeated.
/// @details Non-decreasing is what the accessor promises, and it is satisfied by a
///          constant function, so the sweep also requires the map to have moved.
std::size_t swept_repeats(const TimeStretchMap& map, int last_frame) {
  const float first = map.input_position(0);
  float previous = first;
  std::size_t repeats = 0;
  for (int t = 1; t <= last_frame; ++t) {
    INFO("output frame " << t);
    const float current = map.input_position(t);
    REQUIRE(current >= previous);
    if (current == previous) ++repeats;
    previous = current;
  }
  REQUIRE(previous > first);
  return repeats;
}

using NamedProfile = std::pair<std::string, std::vector<TimeStretchSegment>>;

/// @brief Every profile the contract says construction rejects.
/// @details Shared by the constructor case and the `assign` case, so "assign
///          rejects what construction rejects" is asserted over one list rather
///          than over two that happen to agree.
std::vector<NamedProfile> rejected_profiles() {
  return {
      {"no segment", {}},
      {"first start 1", {{1.0f, 1.0f}}},
      {"first start 0.5", {{0.5f, 1.0f}}},
      {"first start negative", {{-1.0f, 1.0f}}},
      {"first start denormal", {{1e-30f, 1.0f}}},
      {"repeated start", {{0.0f, 1.0f}, {0.0f, 1.0f}}},
      {"repeated later start", {{0.0f, 1.0f}, {10.0f, 1.0f}, {10.0f, 1.0f}}},
      {"descending start", {{0.0f, 1.0f}, {10.0f, 1.0f}, {5.0f, 1.0f}}},
      {"negative start", {{0.0f, 1.0f}, {-5.0f, 1.0f}}},
      // A NaN breakpoint compares false against every bound, so a guard written as
      // one comparison admits it while a guard written as its negation does not.
      {"NaN start", {{0.0f, 1.0f}, {kNaN, 1.0f}}},
      {"infinite start", {{0.0f, 1.0f}, {kInf, 1.0f}}},
      {"zero rate", {{0.0f, 0.0f}}},
      {"negative zero rate", {{0.0f, -0.0f}}},
      {"negative rate", {{0.0f, -1.0f}}},
      {"NaN rate", {{0.0f, kNaN}}},
      {"infinite rate", {{0.0f, kInf}}},
      {"bad rate on a later segment", {{0.0f, 1.0f}, {10.0f, -1.0f}}},
      {"NaN rate on a later segment", {{0.0f, 1.0f}, {10.0f, kNaN}}},
  };
}

/// @brief Every scalar rate the contract says construction rejects.
std::vector<float> rejected_rates() { return {0.0f, -0.0f, -1.0f, kNaN, kInf, -kInf}; }

/// @brief What any validly constructed map answers, whatever profile it holds.
/// @details Every profile starts at input 0 with output 0, so frame 0 reads input
///          0 exactly, and a zero-length input projects to zero frames. Neither
///          statement names the profile, so both survive a rejected `assign`
///          whichever profile is left behind.
void require_usable(const TimeStretchMap& map) {
  REQUIRE(map.input_position(0) == 0.0f);
  REQUIRE(map.output_frame_count(0) == 0);
  const ErrorCode code = code_of([&map] { return map.output_frame_count(1000); });
  REQUIRE((code == ErrorCode::Ok || code == ErrorCode::InvalidParameter));
  (void)map.constant();
}

/// @brief Positions agree exactly over `[0, last_frame]`.
/// @details Exact equality, because the claim is that an in-place rewrite is
///          indistinguishable from construction rather than close to it.
void require_same_positions(const TimeStretchMap& lhs, const TimeStretchMap& rhs, int last_frame) {
  for (int t = 0; t <= last_frame; ++t) {
    INFO("output frame " << t);
    REQUIRE(lhs.input_position(t) == rhs.input_position(t));
  }
}

TimeStretchMap profile(std::vector<TimeStretchSegment> segments) {
  return TimeStretchMap(std::move(segments));
}

}  // namespace

// --- The compatibility claim -----------------------------------------------

TEST_CASE("a constant map reproduces the scalar product bit for bit", "[time_map]") {
  // Exact equality, not a tolerance: what this guards against is a whole-frame
  // index move, so a tolerance would be the wrong instrument rather than a weak
  // one.
  const std::vector<float> rates = {1.0f,  0.5f, 2.0f,  1.1f, 0.7f, 0.9f,      1.3f,
                                    0.01f, 1.5f, 0.75f, 3.0f, 0.1f, 1.0000001f};

  // Mismatches are accumulated and asserted once. A REQUIRE inside the loop would
  // stop at the first one and report a single frame where the question is which
  // frames, and from where on.
  const auto sweep = [&rates](const std::vector<int>& frames) {
    std::size_t mismatches = 0;
    for (const float rate : rates) {
      const TimeStretchMap map(rate);
      for (const int t : frames) {
        const float got = map.input_position(t);
        const float want = scalar_position(t, rate);
        if (got == want) continue;
        ++mismatches;
        UNSCOPED_INFO("  rate " << rate << " frame " << t << ": input_position " << got
                                << ", scalar product " << want);
      }
    }
    return mismatches;
  };

  SECTION("output frames inside the float integer range") {
    REQUIRE(sweep({0, 1, 2, 3, 7, 64, 1000, 65536, 1000000, 8388607, 8388608}) == 0);
  }

  SECTION("output frames past 2^24, where a float can no longer hold every integer") {
    // The stated proof is that the exact product needs at most 48 significand
    // bits and so lands in double without rounding. That holds for the product of
    // float(t) and rate. It does not hold for the product of t itself: at
    // 2^24 + 1 the integer is not a float, so a computation keeping t exact in
    // double multiplies a different left operand and can land on a different
    // float.
    REQUIRE(sweep({16777215, 16777216, 16777217, 16777218, 16777219, 33554433, 1073741829}) == 0);
  }
}

TEST_CASE("a constant map's frame count is the scalar projected count", "[time_map]") {
  // The count is defined on the exact position; `input_position` returns a
  // narrowed one. Both are measured against the same call because the gap between
  // them is the contract's own reason for the distinction: the narrowed value can
  // round up onto the input end and report a frame the exact position has not
  // reached. The rates below include the ones where it does.
  struct Probe {
    float rate;
    int input_frames;
    int search_limit;
  };
  const std::vector<Probe> probes = {
      {1.0f, 1, 16},
      {1.0f, 100, 256},
      {0.5f, 10, 64},
      {2.0f, 10, 64},
      {1.1f, 7, 64},
      {1.25f, 9, 64},
      {0.75f, 12, 64},
      {3.0f, 31, 64},
      // Rates where the narrowed product lands exactly on the input count while
      // the exact quotient sits just above an integer, so the accessor reaches a
      // frame before the count does.
      {0.7f, 7, 64},
      {0.7f, 14, 64},
      {0.9f, 9, 64},
      {1.3f, 13, 64},
      {0.01f, 1, 4096},
      {0.01f, 2, 4096},
  };

  std::size_t against_scalar = 0;
  std::size_t narrowed_reaches_early = 0;
  std::size_t narrowed_reaches_late = 0;
  for (const Probe& probe : probes) {
    const TimeStretchMap map(probe.rate);
    const int counted = map.output_frame_count(probe.input_frames);
    const int reaching = smallest_reaching(map, probe.input_frames, probe.search_limit);
    const std::size_t scalar = projected(static_cast<std::size_t>(probe.input_frames), probe.rate);
    REQUIRE(reaching >= 0);
    if (counted != static_cast<int>(scalar)) ++against_scalar;
    if (reaching < counted) ++narrowed_reaches_early;
    if (reaching > counted) ++narrowed_reaches_late;
    if (counted != static_cast<int>(scalar) || counted != reaching) {
      UNSCOPED_INFO("  rate " << probe.rate << " input frames " << probe.input_frames
                              << ": output_frame_count " << counted << ", smallest reaching T "
                              << reaching << ", checked_projected_count " << scalar);
    }
  }
  INFO("disagreements with checked_projected_count "
       << against_scalar << ", narrowed position reaching early " << narrowed_reaches_early
       << ", late " << narrowed_reaches_late);
  // The count is the contract's claim and is asserted.
  REQUIRE(against_scalar == 0);
  // The narrowed accessor may round up onto the input end and report a frame the
  // exact position has not reached, so it can only reach the input *earlier* than
  // the count. Reaching later would mean the count skipped a frame that qualifies.
  REQUIRE(narrowed_reaches_late == 0);
}

TEST_CASE("an empty input yields zero frames and a negative one is rejected", "[time_map]") {
  const TimeStretchMap map(1.25f);
  // Zero is accepted because checked_projected_count returns zero at zero, and
  // the map claims equality with it. Rejecting zero would make the map stricter
  // than the thing it is defined against.
  REQUIRE(map.output_frame_count(0) == 0);
  REQUIRE(map.output_frame_count(0) == static_cast<int>(projected(0, 1.25f)));

  // Negative counts are rejected on the sign. Reaching the same rejection through
  // the width of size_t would not be equivalent: a large enough rate makes
  // ceil(1.8e19 / rate) land under the limit and return a positive count instead
  // of failing.
  REQUIRE(code_of([&map] { return map.output_frame_count(-1); }) == ErrorCode::InvalidParameter);
  REQUIRE(code_of([&map] { return map.output_frame_count(-1000000); }) ==
          ErrorCode::InvalidParameter);
  const TimeStretchMap fast(1.0e12f);
  REQUIRE(code_of([&fast] { return fast.output_frame_count(-1); }) == ErrorCode::InvalidParameter);
}

TEST_CASE("a constant map's sample count does not depend on the hop", "[time_map]") {
  // "The hop cancels for a constant profile" is the one statement about the
  // divisor that does not depend on reading `S / hop_length` one way or the
  // other, so it is what this asserts.
  for (const float rate : {1.0f, 0.5f, 2.0f, 1.1f, 0.7f}) {
    INFO("rate " << rate);
    const TimeStretchMap map(rate);
    const std::size_t input_samples = 44100;
    const std::size_t scalar = projected(input_samples, rate);
    std::size_t first = 0;
    bool have_first = false;
    for (const int hop : {64, 128, 256, 512, 1024}) {
      INFO("hop " << hop);
      const std::size_t got = map.output_sample_count(input_samples, hop);
      if (!have_first) {
        first = got;
        have_first = true;
      }
      REQUIRE(got == first);
      REQUIRE(got == scalar);
    }
  }
}

// --- Monotonicity as a property of the type --------------------------------

TEST_CASE("the returned position never moves backwards, inside a segment or across a boundary",
          "[time_map]") {
  SECTION("a boundary that lands on an integer output frame") {
    // output_start_[1] = 100 / 1.0 = 100 exactly, so frame 100 is the first frame
    // of the second segment and is where a derived breakpoint would land wrong.
    const TimeStretchMap map = profile({{0.0f, 1.0f}, {100.0f, 2.0f}});
    REQUIRE(swept_repeats(map, 400) == 0);
  }

  SECTION("a boundary that lands between two output frames") {
    const TimeStretchMap map = profile({{0.0f, 1.3f}, {37.0f, 0.6f}, {211.0f, 2.25f}});
    REQUIRE(swept_repeats(map, 600) == 0);
  }

  SECTION("segments shorter than one output frame") {
    // Each segment spans a thousandth of an output frame, so one frame steps over
    // many of them and the segment lookup never lands on the one it started in.
    std::vector<TimeStretchSegment> segments;
    for (int i = 0; i < 64; ++i) {
      segments.push_back({static_cast<float>(i), 1000.0f});
    }
    const TimeStretchMap map = profile(std::move(segments));
    REQUIRE(swept_repeats(map, 200) == 0);
  }
}

TEST_CASE("the returned position repeats where narrowing is not injective", "[time_map]") {
  // The positive evidence for the non-decreasing clause, and what stops it being
  // vacuous: these are the frames where the accessor provably does *not* strictly
  // increase, so a suite asserting the stronger property would fail here.
  //
  // Both mechanisms need output frames around 10^7 -- some 54 hours of output at
  // hop 512 and 44.1 kHz -- so the clause is about correctness of the statement
  // rather than about a reachable defect. It is still inside the documented count
  // limit.
  SECTION("the identity map past 2^24") {
    // float(16777217) narrows onto float(16777216), so the accessor repeats where
    // the exact position advances by a whole frame. Entailed by the bit-for-bit
    // clause: at rate 1.0f the accessor is float(t), which is not injective here.
    const TimeStretchMap map(1.0f);
    REQUIRE(map.input_position(16777216) == map.input_position(16777217));
    REQUIRE(map.input_position(16777217) <= map.input_position(16777218));
    REQUIRE(map.input_position(16777216) < map.input_position(16777218));
    // And the pair sits inside a range a caller would iterate.
    REQUIRE(map.output_frame_count(16777220) > 16777217);
  }

  SECTION("a rate below the position's float spacing") {
    // The second mechanism, which bites before 2^24: once the rate falls under one
    // float ULP of the position, adjacent frames land on the same float. Measured
    // first collisions -- 0.001f at t = 16383999, 0.01f at 13107200, 0.1f at
    // 10485763, 0.5f at 16777216.
    const TimeStretchMap map(0.001f);
    REQUIRE(map.input_position(16383999) == map.input_position(16384000));
    REQUIRE(map.input_position(16383999) <= map.input_position(16384000));
    REQUIRE(map.input_position(16383999) < map.input_position(16390000));
  }
}

// --- Rejections ------------------------------------------------------------

TEST_CASE("a profile the contract rejects throws InvalidParameter", "[time_map]") {
  const ErrorCode kInvalid = ErrorCode::InvalidParameter;

  SECTION("every rejected profile") {
    for (const NamedProfile& entry : rejected_profiles()) {
      INFO(entry.first);
      REQUIRE(code_of([&entry] { return TimeStretchMap(entry.second); }) == kInvalid);
    }
  }

  SECTION("every rejected scalar rate") {
    for (const float rate : rejected_rates()) {
      INFO("rate " << rate);
      REQUIRE(code_of([rate] { return TimeStretchMap(rate); }) == kInvalid);
      REQUIRE(code_of([rate] { return profile({{0.0f, rate}}); }) == kInvalid);
    }
  }

  SECTION("a count past the buffer limit") {
    const TimeStretchMap slow(1.0e-6f);
    REQUIRE(code_of([&slow] { return slow.output_frame_count(1000000); }) == kInvalid);
    REQUIRE(code_of([&slow] { return slow.output_sample_count(std::size_t{1000000}, 512); }) ==
            kInvalid);
  }
}

// --- constant() ------------------------------------------------------------

TEST_CASE("constant describes a single-segment profile", "[time_map]") {
  for (const float rate : {1.0f, 0.5f, 2.0f, 1.1f}) {
    INFO("rate " << rate);
    const TimeStretchMap scalar(rate);
    REQUIRE(scalar.constant());
    REQUIRE(scalar.constant_rate() == rate);

    const TimeStretchMap one_segment = profile({{0.0f, rate}});
    REQUIRE(one_segment.constant());
    REQUIRE(one_segment.constant_rate() == rate);
  }

  const TimeStretchMap two = profile({{0.0f, 1.0f}, {100.0f, 2.0f}});
  REQUIRE_FALSE(two.constant());
}

// --- agrees_through --------------------------------------------------------

TEST_CASE("agrees_through is true up to the divergence and false at it", "[time_map]") {
  // A shared first segment with the boundary on an integer output frame, so the
  // last agreeing frame and the first disagreeing one are adjacent and named.
  const TimeStretchMap held = profile({{0.0f, 1.0f}, {100.0f, 1.0f}});
  const TimeStretchMap sped = profile({{0.0f, 1.0f}, {100.0f, 2.0f}});
  REQUIRE(held.input_position(100) == sped.input_position(100));
  REQUIRE(held.input_position(101) != sped.input_position(101));

  for (const int committed : {0, 1, 50, 99, 100}) {
    INFO("committed " << committed);
    REQUIRE(held.agrees_through(sped, committed));
    REQUIRE(sped.agrees_through(held, committed));
  }
  for (const int committed : {101, 102, 500}) {
    INFO("committed " << committed);
    REQUIRE_FALSE(held.agrees_through(sped, committed));
    REQUIRE_FALSE(sped.agrees_through(held, committed));
  }
}

TEST_CASE("agrees_through compares positions rather than structure", "[time_map]") {
  // Different segment counts, identical positions everywhere. A comparison that
  // read the profile instead of the map would call these different.
  const TimeStretchMap scalar(1.0f);
  const TimeStretchMap split = profile({{0.0f, 1.0f}, {50.0f, 1.0f}, {123.0f, 1.0f}});
  for (int t = 0; t <= 400; ++t) {
    INFO("output frame " << t);
    REQUIRE(scalar.input_position(t) == split.input_position(t));
  }
  for (const int committed : {0, 1, 49, 50, 51, 123, 400}) {
    INFO("committed " << committed);
    REQUIRE(scalar.agrees_through(split, committed));
    REQUIRE(split.agrees_through(scalar, committed));
  }
}

TEST_CASE("agrees_through at zero committed frames compares one frame", "[time_map]") {
  // Every map reads input 0 at output 0, so two maps that agree nowhere else
  // still agree over the closed range [0, 0]. The degenerate end of the range is
  // where an implementation iterating `t < committed` differs from one iterating
  // `t <= committed`.
  const TimeStretchMap slow(0.5f);
  const TimeStretchMap fast(2.0f);
  REQUIRE(slow.input_position(0) == fast.input_position(0));
  REQUIRE(slow.agrees_through(fast, 0));
  REQUIRE_FALSE(slow.agrees_through(fast, 1));
  REQUIRE(slow.agrees_through(slow, 0));
  REQUIRE(slow.agrees_through(slow, 1000000));
}

// --- assign ----------------------------------------------------------------

TEST_CASE("assign rejects exactly what construction rejects", "[time_map]") {
  const ErrorCode kInvalid = ErrorCode::InvalidParameter;

  SECTION("profiles") {
    for (const NamedProfile& entry : rejected_profiles()) {
      INFO(entry.first);
      // The same input to both routes. A validating constructor beside a
      // permissive assign is invisible unless one list drives both.
      REQUIRE(code_of([&entry] { return TimeStretchMap(entry.second); }) == kInvalid);
      TimeStretchMap map(1.0f);
      REQUIRE(code_of([&map, &entry] { map.assign(entry.second); }) == kInvalid);
      require_usable(map);
    }
  }

  SECTION("scalar rates") {
    for (const float rate : rejected_rates()) {
      INFO("rate " << rate);
      REQUIRE(code_of([rate] { return TimeStretchMap(rate); }) == kInvalid);
      TimeStretchMap map(1.0f);
      REQUIRE(code_of([&map, rate] { map.assign(rate); }) == kInvalid);
      require_usable(map);
    }
  }

  SECTION("a rejected assign onto a multi-segment map") {
    // The destination holds more storage than the rejected source needs, which is
    // where an in-place rewrite has the most room to leave the map half-written.
    for (const NamedProfile& entry : rejected_profiles()) {
      INFO(entry.first);
      TimeStretchMap map = profile({{0.0f, 1.3f}, {37.0f, 0.6f}, {211.0f, 2.25f}});
      REQUIRE(code_of([&map, &entry] { map.assign(entry.second); }) == kInvalid);
      require_usable(map);
    }
  }
}

TEST_CASE("assign produces the same map as construction", "[time_map]") {
  SECTION("a scalar rate onto profiles of every shape") {
    for (const float rate : {1.0f, 0.5f, 2.0f, 1.1f, 0.7f}) {
      INFO("rate " << rate);
      const TimeStretchMap built(rate);
      std::vector<TimeStretchMap> destinations;
      destinations.push_back(TimeStretchMap(3.0f));
      destinations.push_back(profile({{0.0f, 1.3f}, {37.0f, 0.6f}}));
      destinations.push_back(profile({{0.0f, 2.0f}, {5.0f, 0.25f}, {9.0f, 4.0f}, {40.0f, 1.0f}}));
      for (TimeStretchMap& destination : destinations) {
        destination.assign(rate);
        REQUIRE(destination.constant());
        REQUIRE(destination.constant_rate() == rate);
        require_same_positions(destination, built, 400);
      }
    }
  }

  SECTION("a segment profile onto a constant map") {
    const std::vector<TimeStretchSegment> segments = {{0.0f, 1.3f}, {37.0f, 0.6f}, {211.0f, 2.25f}};
    const TimeStretchMap built(segments);
    TimeStretchMap destination(3.0f);
    REQUIRE(destination.constant());
    destination.assign(segments);
    // constant() follows the new profile, not the one the storage came from.
    REQUIRE_FALSE(destination.constant());
    require_same_positions(destination, built, 600);
    REQUIRE(destination.output_frame_count(500) == built.output_frame_count(500));
    REQUIRE(destination.output_sample_count(std::size_t{44100}, 512) ==
            built.output_sample_count(std::size_t{44100}, 512));
  }
}

TEST_CASE("assign does not accumulate across rewrites", "[time_map]") {
  // Shrinking then regrowing is where a rewrite that reuses storage leaves a tail
  // behind: the third assign must produce the first map, not the first map with
  // the second's leftovers under it.
  const std::vector<TimeStretchSegment> wide = {
      {0.0f, 1.3f}, {37.0f, 0.6f}, {211.0f, 2.25f}, {400.0f, 0.9f}, {880.0f, 1.75f}};
  const std::vector<TimeStretchSegment> narrow = {{0.0f, 0.5f}};
  const TimeStretchMap fresh_wide(wide);
  const TimeStretchMap fresh_narrow(narrow);

  SECTION("wide, narrow, wide") {
    TimeStretchMap map(wide);
    map.assign(narrow);
    require_same_positions(map, fresh_narrow, 600);
    map.assign(wide);
    REQUIRE_FALSE(map.constant());
    require_same_positions(map, fresh_wide, 1200);
  }

  SECTION("narrow, wide, narrow") {
    TimeStretchMap map(narrow);
    map.assign(wide);
    require_same_positions(map, fresh_wide, 1200);
    map.assign(narrow);
    REQUIRE(map.constant());
    REQUIRE(map.constant_rate() == 0.5f);
    require_same_positions(map, fresh_narrow, 600);
  }

  SECTION("alternating the scalar and segment overloads") {
    TimeStretchMap map(wide);
    map.assign(2.0f);
    REQUIRE(map.constant());
    map.assign(wide);
    REQUIRE_FALSE(map.constant());
    require_same_positions(map, fresh_wide, 1200);
    map.assign(0.5f);
    require_same_positions(map, fresh_narrow, 600);
  }

  SECTION("a rejected assign between two accepted ones") {
    TimeStretchMap map(wide);
    REQUIRE(code_of([&map] { map.assign(std::vector<TimeStretchSegment>{}); }) ==
            ErrorCode::InvalidParameter);
    map.assign(wide);
    require_same_positions(map, fresh_wide, 1200);
  }
}

// --- hold_profile ----------------------------------------------------------

namespace {

struct HoldCase {
  int input_frames;
  std::vector<HoldRange> holds;
  float target_rate;
};

int held_frames(const std::vector<HoldRange>& holds) {
  int total = 0;
  for (const HoldRange& hold : holds) total += hold.frame_count;
  return total;
}

}  // namespace

TEST_CASE("a hold profile lands the whole signal where the target rate asks", "[time_map]") {
  // The property, not the formula. The oracle is the scalar map through the same
  // public API, so this checks the redistribution without trusting the algebra
  // that produced it.
  //
  // Two fixtures below are here because they fail when the rate outside the holds
  // is rounded to nearest rather than toward +inf: 1000 frames at 1.25 gives 801
  // against 800, and 100 frames with one held at 1.25 gives 81 against 80. They
  // are what makes the rounding clause load-bearing rather than merely true.
  const std::vector<HoldCase> cases = {
      {1000, {{0, 3}, {500, 3}}, 0.5f},
      {1000, {{0, 3}, {500, 3}}, 1.25f},
      {480, {{0, 3}}, 0.75f},
      {480, {{0, 3}}, 1.5f},
      {100, {{10, 10}}, 0.5f},
      {100, {{10, 10}}, 2.0f},
      {1024, {{64, 8}, {512, 8}}, 0.8f},
      {200, {{0, 3}, {100, 3}}, 2.0f},
      {100, {{1, 1}}, 1.25f},
      {100, {{50, 50}}, 1.5f},
      {100, {{0, 10}}, 1.6f},
  };

  std::size_t mismatches = 0;
  for (const HoldCase& probe : cases) {
    const std::vector<TimeStretchSegment> segments =
        hold_profile(probe.holds, probe.input_frames, probe.target_rate);
    REQUIRE(!segments.empty());
    // The map requires this of any profile, so a hold at frame 0 must produce it
    // rather than a leading stretch segment of zero length.
    REQUIRE(segments.front().input_start == 0.0f);

    const TimeStretchMap held(segments);
    const TimeStretchMap scalar(probe.target_rate);
    const int from_holds = held.output_frame_count(probe.input_frames);
    const int from_scalar = scalar.output_frame_count(probe.input_frames);
    if (from_holds != from_scalar) {
      ++mismatches;
      UNSCOPED_INFO("  input frames " << probe.input_frames << " held " << held_frames(probe.holds)
                                      << " target " << probe.target_rate << ": hold profile "
                                      << from_holds << ", scalar " << from_scalar);
    }
  }
  INFO("hold profiles whose length differs from the scalar map: " << mismatches);
  REQUIRE(mismatches == 0);
}

TEST_CASE("an empty hold list reproduces the scalar map exactly", "[time_map]") {
  for (const float target : {0.5f, 1.0f, 1.25f, 2.0f}) {
    INFO("target rate " << target);
    const std::vector<TimeStretchSegment> segments = hold_profile({}, 1000, target);
    REQUIRE(segments.size() == 1);
    REQUIRE(segments.front().input_start == 0.0f);
    REQUIRE(segments.front().rate == target);

    const TimeStretchMap built(segments);
    const TimeStretchMap scalar(target);
    REQUIRE(built.constant());
    REQUIRE(built.constant_rate() == target);
    require_same_positions(built, scalar, 800);
    REQUIRE(built.output_frame_count(1000) == scalar.output_frame_count(1000));
  }
}

TEST_CASE("the feasibility boundary is where the holds fill the requested output", "[time_map]") {
  // 100 input frames with 50 held. The requested output is 100 / target, so the
  // holds fill it exactly at target 2.0 -- a round number, which is what puts it
  // on the boundary rather than near it.
  const std::vector<HoldRange> holds = {{25, 50}};
  const int input_frames = 100;

  SECTION("below the boundary the request is satisfiable") {
    for (const float target : {0.5f, 1.0f, 1.5f, 1.9f}) {
      INFO("target rate " << target);
      const std::vector<TimeStretchSegment> segments = hold_profile(holds, input_frames, target);
      REQUIRE(!segments.empty());
      const TimeStretchMap held(segments);
      REQUIRE(held.output_frame_count(input_frames) ==
              TimeStretchMap(target).output_frame_count(input_frames));
    }
  }

  SECTION("at the boundary the holds are exactly as long as the output") {
    // 100 / 2.0 == 50 == the held frames. "At least as long" makes this a throw,
    // which is where a > and a >= part company.
    REQUIRE(code_of([&] { return hold_profile(holds, input_frames, 2.0f); }) ==
            ErrorCode::InvalidParameter);
  }

  SECTION("above the boundary the request is refused rather than approximated") {
    for (const float target : {2.1f, 2.5f, 4.0f, 100.0f}) {
      INFO("target rate " << target);
      REQUIRE(code_of([&] { return hold_profile(holds, input_frames, target); }) ==
              ErrorCode::InvalidParameter);
    }
  }

  SECTION("lengthening is satisfiable however much is held") {
    for (const int count : {1, 50, 90, 99}) {
      INFO("held frames " << count);
      const std::vector<TimeStretchSegment> segments =
          hold_profile({{0, count}}, input_frames, 0.5f);
      REQUIRE(!segments.empty());
    }
  }
}

TEST_CASE("a hold at either end of the input still builds a usable profile", "[time_map]") {
  const int input_frames = 100;

  SECTION("a hold starting at frame zero") {
    // The first segment is the hold, not the stretch, and the map still requires
    // input_start 0.0f on it.
    const std::vector<TimeStretchSegment> segments = hold_profile({{0, 10}}, input_frames, 1.6f);
    REQUIRE(segments.front().input_start == 0.0f);
    REQUIRE(segments.front().rate == 1.0f);
    const TimeStretchMap held(segments);
    REQUIRE(held.output_frame_count(input_frames) ==
            TimeStretchMap(1.6f).output_frame_count(input_frames));
  }

  SECTION("a hold ending exactly at the last input frame") {
    // No trailing stretch segment exists, so the profile's last segment is the
    // hold and nothing follows it.
    const std::vector<TimeStretchSegment> segments = hold_profile({{50, 50}}, input_frames, 1.5f);
    REQUIRE(segments.front().input_start == 0.0f);
    REQUIRE(segments.back().rate == 1.0f);
    const TimeStretchMap held(segments);
    REQUIRE(held.output_frame_count(input_frames) ==
            TimeStretchMap(1.5f).output_frame_count(input_frames));
  }

  SECTION("a hold covering the whole input, at unity") {
    // With N == H the equation reads N + 0/r == N / target, which holds for any
    // positive r exactly when target is 1. There is no stretch region, and none
    // is needed.
    const std::vector<HoldRange> whole = {{0, input_frames}};
    const std::vector<TimeStretchSegment> segments = hold_profile(whole, input_frames, 1.0f);
    REQUIRE(!segments.empty());
    REQUIRE(segments.front().input_start == 0.0f);
    const TimeStretchMap held(segments);
    REQUIRE(held.output_frame_count(input_frames) ==
            TimeStretchMap(1.0f).output_frame_count(input_frames));
  }

  SECTION("a hold covering the whole input, at any other rate") {
    // The same equation has no solution: no r moves a total that has no stretch
    // region in it. The held frames being shorter than the requested output is
    // not enough -- 100 held against 200 requested passes that and still cannot
    // be satisfied.
    const std::vector<HoldRange> whole = {{0, input_frames}};
    for (const float target : {0.5f, 0.25f, 1.5f, 2.0f}) {
      INFO("target rate " << target);
      REQUIRE(code_of([&] { return hold_profile(whole, input_frames, target); }) ==
              ErrorCode::InvalidParameter);
    }
  }
}

TEST_CASE("hold_profile rejects the hold sets and arguments the contract names", "[time_map]") {
  const ErrorCode kInvalid = ErrorCode::InvalidParameter;
  const int input_frames = 100;
  const auto rejected = [&](const std::vector<HoldRange>& holds, const char* label) {
    INFO(label);
    REQUIRE(code_of([&] { return hold_profile(holds, input_frames, 1.0f); }) == kInvalid);
  };

  SECTION("hold sets") {
    rejected({{10, 10}, {5, 5}}, "unsorted");
    rejected({{10, 10}, {15, 5}}, "overlapping");
    rejected({{10, 10}, {19, 5}}, "overlapping by one frame");
    rejected({{10, 0}}, "empty");
    rejected({{10, -5}}, "negative length");
    rejected({{-1, 5}}, "starting before zero");
    rejected({{100, 5}}, "starting at the input end");
    rejected({{101, 5}}, "starting past the input end");
    rejected({{95, 10}}, "running past the input end");
    rejected({{10, 10}, {20, 10}, {15, 5}}, "sorted pair followed by an earlier one");
  }

  SECTION("input frame counts") {
    for (const int frames : {0, -1, -1000}) {
      INFO("input frames " << frames);
      REQUIRE(code_of([frames] { return hold_profile({{0, 1}}, frames, 1.0f); }) == kInvalid);
      REQUIRE(code_of([frames] { return hold_profile({}, frames, 1.0f); }) == kInvalid);
    }
  }

  SECTION("target rates") {
    for (const float target : rejected_rates()) {
      INFO("target rate " << target);
      REQUIRE(code_of([target] { return hold_profile({{10, 10}}, 100, target); }) == kInvalid);
      REQUIRE(code_of([target] { return hold_profile({}, 100, target); }) == kInvalid);
    }
  }
}

TEST_CASE("the hold profile matches the scalar count across the measured grid", "[time_map]") {
  // The grid the rounding clause was measured over, run against the shipped code
  // rather than against the derivation that produced the number. Stated as axis
  // ranges rather than as a point count, because a product says nothing about
  // which regions it covers: input frames 100 to 8192, held frames 1 to 32,
  // target rate 0.25 to 3.0, one hold placed at frame 1 so every point has a
  // stretch segment on both sides of it.
  //
  // The two fixtures that fail under round-to-nearest live in the case above, not
  // here, so narrowing this grid cannot take them with it.
  const std::vector<int> input_counts = {100, 256, 480, 512, 1000, 1024, 2000, 4410, 8192};
  const std::vector<int> hold_counts = {1, 3, 6, 8, 16, 32};
  const std::vector<float> targets = {0.25f, 0.5f, 0.75f, 0.8f, 1.0f, 1.25f,
                                      1.5f,  1.6f, 2.0f,  2.5f, 3.0f};

  std::size_t checked = 0;
  std::size_t skipped = 0;
  std::size_t mismatches = 0;
  for (const int frames : input_counts) {
    for (const int held : hold_counts) {
      for (const float target : targets) {
        const std::vector<HoldRange> holds = {{1, held}};
        std::vector<TimeStretchSegment> segments;
        if (code_of([&] { segments = hold_profile(holds, frames, target); }) != ErrorCode::Ok) {
          ++skipped;
          continue;
        }
        ++checked;
        const TimeStretchMap held_map(segments);
        const int from_holds = held_map.output_frame_count(frames);
        const int from_scalar = TimeStretchMap(target).output_frame_count(frames);
        if (from_holds == from_scalar) continue;
        ++mismatches;
        if (mismatches <= 8) {
          UNSCOPED_INFO("  input frames " << frames << " held " << held << " target " << target
                                          << ": hold profile " << from_holds << ", scalar "
                                          << from_scalar);
        }
      }
    }
  }

  INFO("checked " << checked << ", skipped as unsatisfiable " << skipped << ", mismatched "
                  << mismatches);
  // A sweep that skipped its way to an empty population would assert nothing. The
  // floor is far under the grid's size and far over zero, so it catches a
  // feasibility rule that rejects most of the grid without pinning one that
  // accepts all of it.
  REQUIRE(checked > 400);
  REQUIRE(mismatches == 0);
}
