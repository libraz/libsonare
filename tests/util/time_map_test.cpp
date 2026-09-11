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
