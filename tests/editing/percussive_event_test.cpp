#include "editing/event_model/percussive_event.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/event_model/event_extractor.h"
#include "editing/event_model/event_renderer.h"
#include "effects/hpss.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/types.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace sonare::editing::event_model;

namespace {

constexpr int kSampleRate = 22050;
/// The extractor's default framing, which every fixture position is reasoned in.
constexpr int kHopLength = 512;
/// An onset lands on a frame boundary, so a detected position is only ever a hop
/// away from the sample the hit was written at. Four hops (93 ms) is wide enough
/// for the detector's own latency and far narrower than the 400 ms the fixtures
/// space their hits by, so it still names one hit uniquely.
/// Where the fixtures write their hits, shared with the cases that measure
/// them: a window anchored on an extracted onset drifts with the detector,
/// where the synthesised position is what the hit actually is.
constexpr int64_t kThreeHitStarts[] = {4410, 13230, 22050};
constexpr float kThreeHitPeaks[] = {0.50f, 0.34f, 0.22f};
constexpr int64_t kMoveHitStarts[] = {6615, 35280};
constexpr int64_t kNoteStart = 8820;
constexpr int64_t kLateHitStart = 52920;

constexpr float kHitDecayMs = 12.0f;
constexpr size_t kHitSamples = 1323;  // 60 ms at 22050 Hz, exactly.

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr size_t kNoMismatch = static_cast<size_t>(-1);

/// @brief Deterministic uniform noise in [-1, 1).
/// @details The seed is per hit so no two synthesised hits carry the same
///          samples: a span that read a neighbour's audio cannot then match.
std::vector<float> noise(uint32_t seed, size_t samples) {
  std::vector<float> output(samples, 0.0f);
  uint32_t state = seed * 2654435761u + 1u;
  for (size_t i = 0; i < samples; ++i) {
    state = state * 1664525u + 1013904223u;
    output[i] = static_cast<float>(state >> 9) / 4194304.0f - 1.0f;
  }
  return output;
}

/// @brief An exponentially decaying noise burst: the fixtures' struck sound.
/// @details Normalised so the peak is exactly @p amplitude, which makes the
///          synthesised level an analytic anchor for peak_amplitude and for the
///          lifted signal's size.
std::vector<float> hit(uint32_t seed, float amplitude) {
  std::vector<float> burst = noise(seed, kHitSamples);
  const double decay = static_cast<double>(kHitDecayMs) * 0.001 * kSampleRate;
  float worst = 0.0f;
  for (size_t i = 0; i < kHitSamples; ++i) {
    burst[i] *= static_cast<float>(std::exp(-static_cast<double>(i) / decay));
    worst = std::max(worst, std::abs(burst[i]));
  }
  REQUIRE(worst > 0.0f);
  const float scale = amplitude / worst;
  for (float& sample : burst) sample *= scale;
  return burst;
}

struct HitSpec {
  size_t start;
  uint32_t seed;
  float amplitude;
};

void add_hits(std::vector<float>& into, const std::vector<HitSpec>& specs) {
  for (const HitSpec& spec : specs) {
    const std::vector<float> burst = hit(spec.seed, spec.amplitude);
    for (size_t i = 0; i < burst.size() && spec.start + i < into.size(); ++i) {
      into[spec.start + i] += burst[i];
    }
  }
}

/// @brief A sustained sine over [@p start, @p end) with a 2 ms raised-cosine
///        edge at each end.
/// @details The edge is short enough that the attack still trips the detector
///          and long enough that the buffer carries no step discontinuity, so
///          the span reads as harmonic rather than as one long click.
void add_note(std::vector<float>& into, size_t start, size_t end, float frequency_hz,
              float amplitude) {
  constexpr size_t kEdgeSamples = 44;  // 2 ms at 22050 Hz.
  REQUIRE(end > start + 2 * kEdgeSamples);
  REQUIRE(end <= into.size());
  for (size_t i = start; i < end; ++i) {
    const size_t position = i - start;
    const size_t remaining = end - 1 - i;
    float envelope = 1.0f;
    if (position < kEdgeSamples) {
      envelope = 0.5f - 0.5f * std::cos(sonare::constants::kPi * static_cast<float>(position) /
                                        static_cast<float>(kEdgeSamples));
    } else if (remaining < kEdgeSamples) {
      envelope = 0.5f - 0.5f * std::cos(sonare::constants::kPi * static_cast<float>(remaining) /
                                        static_cast<float>(kEdgeSamples));
    }
    into[i] += amplitude * envelope *
               static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz *
                                           static_cast<double>(position) / kSampleRate));
  }
}

sonare::Audio audio_of(std::vector<float> samples) {
  return sonare::Audio::from_vector(std::move(samples), kSampleRate);
}

/// @brief Three isolated hits at descending, distinct levels, 400 ms apart,
///        followed by a tail longer than the default 500 ms cap.
sonare::Audio three_hits() {
  std::vector<float> samples(44100, 0.0f);  // 2.0 s
  add_hits(samples, {{static_cast<size_t>(kThreeHitStarts[0]), 1u, kThreeHitPeaks[0]},
                     {static_cast<size_t>(kThreeHitStarts[1]), 2u, kThreeHitPeaks[1]},
                     {static_cast<size_t>(kThreeHitStarts[2]), 3u, kThreeHitPeaks[2]}});
  return audio_of(std::move(samples));
}

/// @brief Two isolated hits inside one second: the compact fixture the parameter
///        sweeps run on, so a sweep stays well under the slow-test threshold.
sonare::Audio two_hits() {
  std::vector<float> samples(22050, 0.0f);  // 1.0 s
  add_hits(samples, {{3528, 31u, 0.50f}, {12348, 32u, 0.30f}});
  return audio_of(std::move(samples));
}

/// @brief A sustained note and, well after it, one isolated hit.
/// @details The two onsets are the same kind of event to the detector and
///          opposite kinds to the separation, which is what percussive_ratio has
///          to tell apart.
sonare::Audio note_then_hit() {
  std::vector<float> samples(66150, 0.0f);  // 3.0 s
  add_note(samples, static_cast<size_t>(kNoteStart), 44100, 330.0f, 0.8f);
  add_hits(samples, {{static_cast<size_t>(kLateHitStart), 11u, 0.50f}});
  return audio_of(std::move(samples));
}

/// @brief A quiet hit sitting on top of a loud sustained note, plus the same
///        note with no hit on it.
/// @details The source peak over the hit's span is the note's, an order above
///          the hit's own peak, so a peak_amplitude measured on the source and
///          one measured on the percussive component cannot be confused. The
///          note-only reference is what "the harmonic content is still sounding"
///          is asserted against.
struct LayeredFixture {
  sonare::Audio mixed;
  sonare::Audio note_only;
  /// The same hit, same seed and same level, with nothing under it. The only
  /// difference from @c mixed at the hit's position is what is sustaining
  /// through it, which is what percussive_ratio actually responds to.
  sonare::Audio hit_only;
  size_t hit_start = 22050;
  float hit_peak = 0.15f;
  float note_amplitude = 0.8f;
};

LayeredFixture layered_fixture() {
  LayeredFixture fixture;
  std::vector<float> note(44100, 0.0f);  // 2.0 s
  add_note(note, 0, 44100, 440.0f, fixture.note_amplitude);
  std::vector<float> mixed = note;
  add_hits(mixed, {{fixture.hit_start, 21u, fixture.hit_peak}});
  std::vector<float> alone(note.size(), 0.0f);
  add_hits(alone, {{fixture.hit_start, 21u, fixture.hit_peak}});
  fixture.note_only = audio_of(std::move(note));
  fixture.mixed = audio_of(std::move(mixed));
  fixture.hit_only = audio_of(std::move(alone));
  return fixture;
}

size_t first_mismatch(const sonare::Audio& a, const sonare::Audio& b, size_t lo, size_t hi) {
  const size_t end = std::min(hi, std::min(a.size(), b.size()));
  for (size_t i = lo; i < end; ++i) {
    if (a[i] != b[i]) return i;
  }
  return kNoMismatch;
}

size_t first_mismatch(const sonare::Audio& a, const sonare::Audio& b) {
  if (a.size() != b.size()) return 0;
  return first_mismatch(a, b, 0, a.size());
}

double rms(const sonare::Audio& audio, size_t lo, size_t hi) {
  const size_t end = std::min(hi, audio.size());
  double acc = 0.0;
  for (size_t i = lo; i < end; ++i) {
    acc += static_cast<double>(audio[i]) * static_cast<double>(audio[i]);
  }
  return std::sqrt(acc / static_cast<double>(std::max<size_t>(1, end - lo)));
}

float peak(const sonare::Audio& audio, size_t lo, size_t hi) {
  const size_t end = std::min(hi, audio.size());
  float highest = 0.0f;
  for (size_t i = lo; i < end; ++i) {
    highest = std::max(highest, std::abs(audio[i]));
  }
  return highest;
}

float max_abs_difference(const sonare::Audio& a, const sonare::Audio& b, size_t lo, size_t hi) {
  const size_t end = std::min(hi, std::min(a.size(), b.size()));
  float worst = 0.0f;
  for (size_t i = lo; i < end; ++i) {
    worst = std::max(worst, std::abs(a[i] - b[i]));
  }
  return worst;
}

/// @brief a - b, sample by sample. The renderer writes source + (gain - 1) * p,
///        so a difference against the source is the lifted signal scaled, and
///        two differences at two gains are the same signal at two scales.
std::vector<float> difference(const sonare::Audio& a, const sonare::Audio& b) {
  REQUIRE(a.size() == b.size());
  std::vector<float> output(a.size(), 0.0f);
  for (size_t i = 0; i < a.size(); ++i) output[i] = a[i] - b[i];
  return output;
}

float peak_of(const std::vector<float>& values) {
  float highest = 0.0f;
  for (const float value : values) highest = std::max(highest, std::abs(value));
  return highest;
}

/// @brief max |a + scale * b| over the whole buffer.
float max_residual(const std::vector<float>& a, const std::vector<float>& b, float scale) {
  REQUIRE(a.size() == b.size());
  float worst = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] + scale * b[i]));
  return worst;
}

/// @brief Index of the event whose onset sits closest to @p sample.
size_t nearest_event(const std::vector<PercussiveEvent>& events, int64_t sample) {
  REQUIRE(!events.empty());
  size_t best = 0;
  int64_t best_distance = std::abs(events[0].onset_sample - sample);
  for (size_t i = 1; i < events.size(); ++i) {
    const int64_t distance = std::abs(events[i].onset_sample - sample);
    if (distance < best_distance) {
      best_distance = distance;
      best = i;
    }
  }
  return best;
}

/// @brief The event nearest @p sample, required to actually be at @p sample.
/// @brief How far in front of its transient an onset may legitimately sit: the
///        detector's own backtrack bound, at the framing the fixtures use.
const int64_t kBacktrackReach =
    static_cast<int64_t>(percussive_onset_defaults().backtrack_range) * kHopLength;

/// @brief The span opens in front of the transient at @p start and closes past
///        it.
/// @details Not a symmetric tolerance on the onset, because the two directions
///          are not the same claim. Peak-picking lands after a transient starts,
///          so an onset at or after @p start means the span opened inside its
///          own hit: it would then measure the next hit's peak, keep its own
///          attack when muted, and take the square opening cut mid-attack.
///          Backtracking is what moves the edge in front, and the early bound is
///          the detector's own limit on that travel, so an onset cannot have
///          wandered back into the previous hit either.
void require_span_covers(const PercussiveEvent& event, int64_t start,
                         int64_t coverage = static_cast<int64_t>(kHitSamples)) {
  REQUIRE(event.onset_sample <= start);
  REQUIRE(event.onset_sample > start - kBacktrackReach);
  REQUIRE(event.offset_sample > start + coverage);
}

/// @brief Whether any span covers @p start. Unlike @ref event_at this answers
///        false for an empty set rather than failing, so it can state that a
///        threshold dropped the event that was there.
bool any_span_covers(const std::vector<PercussiveEvent>& events, int64_t start) {
  for (const PercussiveEvent& event : events) {
    if (event.onset_sample <= start && event.offset_sample > start) return true;
  }
  return false;
}

const PercussiveEvent& event_at(const std::vector<PercussiveEvent>& events, int64_t start) {
  const size_t index = nearest_event(events, start);
  require_span_covers(events[index], start);
  return events[index];
}

/// @brief The separation the default config performs, computed independently of
///        the extractor so a measurement can be checked against the signal it
///        claims to be measured on.
sonare::Audio reference_percussive(const sonare::Audio& audio,
                                   const PercussiveSeparationConfig& separation) {
  sonare::StftConfig stft;
  stft.n_fft = separation.n_fft;
  stft.hop_length = separation.hop_length;
  return sonare::percussive(audio, separation.hpss, stft);
}

/// @brief The error code a call throws, or Ok when it does not throw.
template <typename Fn>
sonare::ErrorCode code_of(Fn&& fn) {
  try {
    fn();
  } catch (const sonare::SonareException& error) {
    return error.code();
  }
  return sonare::ErrorCode::Ok;
}

/// @brief A renderable one-event set over @p audio with a non-identity edit.
PercussiveEvent edited_event(int64_t onset, int64_t offset) {
  PercussiveEvent event;
  event.onset_sample = onset;
  event.offset_sample = offset;
  event.edit.gain_db = -3.0f;
  return event;
}

/// @brief A separation framing, the one input both entry points share.
struct Framing {
  int n_fft;
  int hop_length;
};

/// @brief Framings that break constant overlap-add.
/// @details The rule is n_fft even and at least 2, and hop_length in
///          (0, n_fft / 2]. Each row below is on the far side of exactly one
///          bound, so a rejection cannot come from a second defect in the row.
constexpr Framing kBrokenFramings[] = {
    {2047, 512},   // odd
    {1023, 256},   // odd, at another size
    {1, 1},        // odd and below the minimum
    {0, 512},      // no window at all
    {-2048, 512},  // negative
    {2048, 0},     // no advance
    {2048, -512},  // negative advance
    {2048, 1025},  // one sample past half the window
    {1024, 513},   // the same bound at another size
    {2048, 2048},  // no overlap at all
};

/// @brief Framings inside the rule, including the inclusive end of it.
/// @details Half the window is accepted, so the rejections above are the rule
///          biting and not "any hop that is not the default".
constexpr Framing kValidFramings[] = {
    {2048, 512},   // the default
    {2048, 1024},  // exactly half the window
    {1024, 512},   // the same boundary at another size
};

void require_well_formed(const std::vector<PercussiveEvent>& events, const sonare::Audio& audio) {
  int64_t previous_offset = 0;
  for (size_t i = 0; i < events.size(); ++i) {
    INFO("event " << i);
    const PercussiveEvent& event = events[i];
    REQUIRE(event.onset_sample >= 0);
    REQUIRE(event.offset_sample > event.onset_sample);
    REQUIRE(event.offset_sample <= static_cast<int64_t>(audio.size()));
    REQUIRE(event.length_samples() == event.offset_sample - event.onset_sample);
    // Ascending and non-overlapping, which is also what makes the set renderable.
    REQUIRE(event.onset_sample >= previous_offset);
    previous_offset = event.offset_sample;
    REQUIRE(std::isfinite(event.strength));
    REQUIRE(std::isfinite(event.peak_amplitude));
    REQUIRE(event.peak_amplitude >= 0.0f);
    REQUIRE(std::isfinite(event.percussive_ratio));
    REQUIRE(event.percussive_ratio >= 0.0f);
    REQUIRE(event.percussive_ratio <= 1.0f);
    REQUIRE(event.edit.is_identity());
  }
}

}  // namespace

// --- PercussiveEventEdit --------------------------------------------------

TEST_CASE("PercussiveEventEdit is identity only when every field is at its neutral value",
          "[event_model]") {
  REQUIRE(PercussiveEventEdit{}.is_identity());

  PercussiveEventEdit later;
  later.time_offset_samples = 1;
  REQUIRE_FALSE(later.is_identity());

  PercussiveEventEdit earlier;
  earlier.time_offset_samples = -1;
  REQUIRE_FALSE(earlier.is_identity());

  PercussiveEventEdit gain;
  gain.gain_db = -0.5f;
  REQUIRE_FALSE(gain.is_identity());

  PercussiveEventEdit muted;
  muted.muted = true;
  REQUIRE(muted.gain_db == 0.0f);
  REQUIRE(muted.time_offset_samples == 0);
  REQUIRE_FALSE(muted.is_identity());
}

TEST_CASE("PercussiveEventEdit reports a non-finite gain as non-identity", "[event_model]") {
  // The comparison is exact so a non-finite gain reaches the renderer's
  // validation instead of being waved through as "changes nothing".
  for (const float bad : {kNaN, kInf, -kInf}) {
    PercussiveEventEdit edit;
    edit.gain_db = bad;
    REQUIRE_FALSE(edit.is_identity());
  }
}

TEST_CASE("PercussiveEvent reports its span length", "[event_model]") {
  PercussiveEvent event;
  REQUIRE(event.length_samples() == 0);
  event.onset_sample = 4410;
  event.offset_sample = 15435;
  REQUIRE(event.length_samples() == 11025);
}

// --- extract_percussive_events: spans -------------------------------------

TEST_CASE("extract_percussive_events finds every hit, in order, with an identity edit",
          "[event_model]") {
  const sonare::Audio audio = three_hits();
  const std::vector<PercussiveEvent> events = extract_percussive_events(audio);

  REQUIRE(events.size() == 3);
  require_well_formed(events, audio);

  for (size_t i = 0; i < 3; ++i) {
    INFO("hit " << i);
    require_span_covers(events[i], kThreeHitStarts[i]);
    REQUIRE(events[i].strength > 0.0f);
  }
  REQUIRE(events[0].onset_sample < events[1].onset_sample);
  REQUIRE(events[1].onset_sample < events[2].onset_sample);

  // These are struck sounds in silence, so the separation has to call them
  // percussive. The complementary half -- that the figure is not simply pinned
  // at its ceiling for everything -- is the note_then_hit case below.
  for (const PercussiveEvent& event : events) {
    REQUIRE(event.percussive_ratio > 0.5f);
  }
}

TEST_CASE("extract_percussive_events closes a span on the next onset", "[event_model]") {
  const sonare::Audio audio = three_hits();
  const std::vector<PercussiveEvent> events = extract_percussive_events(audio);
  REQUIRE(events.size() == 3);

  // Adjacent, exactly: the interior spans are closed by the following onset and
  // nothing else, so there is no gap and no rounding to absorb.
  REQUIRE(events[0].offset_sample == events[1].onset_sample);
  REQUIRE(events[1].offset_sample == events[2].onset_sample);

  // The hits are 400 ms apart, inside the 500 ms cap, so the cap binds on the
  // last span only. 500 ms is 11025 samples at this rate, exactly.
  REQUIRE(events[0].length_samples() < 11025);
  REQUIRE(events[1].length_samples() < 11025);
  REQUIRE(events[2].length_samples() == 11025);
  REQUIRE(events[2].offset_sample < static_cast<int64_t>(audio.size()));
}

TEST_CASE("extract_percussive_events binds max_event_ms on every span once it is short enough",
          "[event_model]") {
  const sonare::Audio audio = three_hits();
  const std::vector<PercussiveEvent> relaxed = extract_percussive_events(audio);
  REQUIRE(relaxed.size() == 3);

  // 40 ms is 882 samples at 22050 Hz, exactly, and shorter than the 400 ms
  // between hits, so it now closes every span including the interior ones.
  PercussiveEventExtractorConfig tight;
  tight.max_event_ms = 40.0f;
  const std::vector<PercussiveEvent> capped = extract_percussive_events(audio, tight);
  REQUIRE(capped.size() == relaxed.size());
  for (size_t i = 0; i < capped.size(); ++i) {
    INFO("event " << i);
    // The cap moves an offset and never an onset.
    REQUIRE(capped[i].onset_sample == relaxed[i].onset_sample);
    REQUIRE(capped[i].length_samples() == 882);
  }

  // Raised past the tail: the last span is no longer cut at 500 ms. It still has
  // to stay inside the audio, which require_well_formed checks.
  PercussiveEventExtractorConfig loose;
  loose.max_event_ms = 5000.0f;
  const std::vector<PercussiveEvent> uncapped = extract_percussive_events(audio, loose);
  REQUIRE(uncapped.size() == relaxed.size());
  require_well_formed(uncapped, audio);
  for (size_t i = 0; i + 1 < uncapped.size(); ++i) {
    INFO("event " << i);
    // The interior spans were never the cap's business.
    REQUIRE(uncapped[i].onset_sample == relaxed[i].onset_sample);
    REQUIRE(uncapped[i].offset_sample == relaxed[i].offset_sample);
  }
  // 5000 ms from the last onset overshoots the buffer, and a span never runs
  // past the end of the audio whatever the cap says.
  REQUIRE(uncapped.back().length_samples() > 11025);
  REQUIRE(uncapped.back().offset_sample == static_cast<int64_t>(audio.size()));
}

// --- extract_percussive_events: measurements ------------------------------

TEST_CASE("extract_percussive_events measures each hit's own level", "[event_model]") {
  // The three hits are synthesised at 0.50, 0.34 and 0.22 peak with different
  // noise seeds, so a span that measured a neighbour, or measured the whole
  // buffer, cannot reproduce this ordering.
  const sonare::Audio audio = three_hits();
  const std::vector<PercussiveEvent> events = extract_percussive_events(audio);
  REQUIRE(events.size() == 3);

  REQUIRE(events[0].peak_amplitude > events[1].peak_amplitude);
  REQUIRE(events[1].peak_amplitude > events[2].peak_amplitude);

  // Separation is close to linear in the source level, so the measured ratios
  // track the synthesised ones. The band is wide enough for the mask's own
  // level dependence and far narrower than the 1.0 a wrong span would give.
  REQUIRE_THAT(events[0].peak_amplitude / events[1].peak_amplitude,
               WithinAbs(kThreeHitPeaks[0] / kThreeHitPeaks[1], 0.45f));
  REQUIRE_THAT(events[1].peak_amplitude / events[2].peak_amplitude,
               WithinAbs(kThreeHitPeaks[1] / kThreeHitPeaks[2], 0.47f));

  // An isolated hit is nearly all percussive, so the measured peak sits just
  // under the synthesised one rather than anywhere below it.
  REQUIRE(events[0].peak_amplitude > 0.25f);
  REQUIRE(events[0].peak_amplitude <= 0.55f);
}

TEST_CASE("extract_percussive_events measures peak_amplitude on the percussive component",
          "[event_model]") {
  const LayeredFixture fixture = layered_fixture();
  const std::vector<PercussiveEvent> events = extract_percussive_events(fixture.mixed);
  const PercussiveEvent& event = event_at(events, static_cast<int64_t>(fixture.hit_start));

  const size_t onset = static_cast<size_t>(event.onset_sample);
  const size_t offset = static_cast<size_t>(event.offset_sample);

  // The source over this span is dominated by the 0.8 note, so the two candidate
  // signals are an order apart and the answer says which one was measured.
  REQUIRE(peak(fixture.mixed, onset, offset) > 0.75f);
  REQUIRE(event.peak_amplitude > 0.0f);
  REQUIRE(event.peak_amplitude < 0.35f);

  // Tightened against the separation itself: the same HPSS at the same framing,
  // run here rather than trusted from the extractor. A factor-of-two band
  // absorbs the difference between measuring the component over the span and
  // separating the span, and nothing wider.
  const sonare::Audio component = reference_percussive(fixture.mixed, PercussiveSeparationConfig{});
  const float reference = peak(component, onset, offset);
  REQUIRE(reference > 0.0f);
  REQUIRE(event.peak_amplitude >= 0.5f * reference);
  REQUIRE(event.peak_amplitude <= 2.0f * reference);
}

TEST_CASE("extract_percussive_events separates a struck sound from a sustained attack",
          "[event_model]") {
  // Both onsets are detected; what tells them apart is how much of the span the
  // separation called percussive. A figure pinned at 1 for everything fails the
  // ordering here, not merely the bounds.
  //
  // The fixture's note ends before the hit begins, which is the condition the
  // ordering needs: the figure describes a span, so it separates the two only
  // where nothing sustains through both. The buried-hit case above is the other
  // side of that.
  const sonare::Audio audio = note_then_hit();
  const std::vector<PercussiveEvent> events = extract_percussive_events(audio);
  REQUIRE(events.size() >= 2);
  require_well_formed(events, audio);

  const size_t note_index = nearest_event(events, kNoteStart);
  const size_t hit_index = nearest_event(events, kLateHitStart);
  REQUIRE(note_index != hit_index);
  require_span_covers(events[note_index], kNoteStart);
  require_span_covers(events[hit_index], kLateHitStart);

  const float note_ratio = events[note_index].percussive_ratio;
  const float hit_ratio = events[hit_index].percussive_ratio;

  REQUIRE(hit_ratio > note_ratio);
  // Discriminating rather than merely ordered, from both ends: the note's span
  // is 500 ms of sustained tone and must not read near the ceiling, and the
  // hit's span must not read near the floor.
  REQUIRE(note_ratio < 0.9f);
  REQUIRE(hit_ratio > 0.5f);
  REQUIRE(note_ratio < 0.5f * hit_ratio);
}

TEST_CASE("extract_percussive_events reads a hit under a sustain as barely percussive",
          "[event_model]") {
  // The figure is a property of the span's energy, not evidence about the onset
  // that opened it. A genuine hit over a loud sustain reads near 0 because the
  // sustain owns the span, so a low ratio is not "no hit here". The case below
  // is the same samples twice, differing only in what sustains through them.
  const LayeredFixture fixture = layered_fixture();
  const int64_t hit_start = static_cast<int64_t>(fixture.hit_start);

  const std::vector<PercussiveEvent> buried = extract_percussive_events(fixture.mixed);
  const PercussiveEvent& under_sustain = event_at(buried, hit_start);
  const std::vector<PercussiveEvent> alone = extract_percussive_events(fixture.hit_only);
  const PercussiveEvent& isolated = event_at(alone, hit_start);

  // Opposite ends of the figure, for the identical hit.
  REQUIRE(under_sustain.percussive_ratio < 0.05f);
  REQUIRE(isolated.percussive_ratio > 0.5f);

  // The buried one is a real hit all the same: the separation measured a peak on
  // it that the sustain cannot account for. Without this the case would only be
  // saying the ratio is small, which silence also satisfies.
  REQUIRE(under_sustain.peak_amplitude > 0.3f * fixture.hit_peak);

  // So the default threshold is load-bearing rather than merely permissive: a
  // threshold anywhere between the two readings drops a hit that is genuinely
  // there, while keeping the identical hit in silence.
  PercussiveEventExtractorConfig defaults;
  REQUIRE(defaults.min_percussive_ratio == 0.0f);
  REQUIRE(any_span_covers(buried, hit_start));

  PercussiveEventExtractorConfig selective;
  selective.min_percussive_ratio =
      0.5f * (under_sustain.percussive_ratio + isolated.percussive_ratio);
  REQUIRE_FALSE(any_span_covers(extract_percussive_events(fixture.mixed, selective), hit_start));
  REQUIRE(any_span_covers(extract_percussive_events(fixture.hit_only, selective), hit_start));
}

TEST_CASE("extract_percussive_events selects on min_percussive_ratio without moving a span",
          "[event_model]") {
  // The defect this guards against is filtering the onset list and then closing
  // the spans, which silently lengthens every survivor that had a dropped
  // neighbour. Selection happens after measurement, so the survivors are the
  // very same events -- compared here bit for bit, not approximately.
  const sonare::Audio audio = note_then_hit();

  PercussiveEventExtractorConfig keep_all;
  REQUIRE(keep_all.min_percussive_ratio == 0.0f);
  const std::vector<PercussiveEvent> all = extract_percussive_events(audio, keep_all);
  REQUIRE(all.size() >= 2);

  float lowest = all.front().percussive_ratio;
  float highest = all.front().percussive_ratio;
  for (const PercussiveEvent& event : all) {
    lowest = std::min(lowest, event.percussive_ratio);
    highest = std::max(highest, event.percussive_ratio);
  }
  // Without a spread there is no threshold that selects, and the case is vacuous.
  REQUIRE(highest - lowest > 0.2f);

  PercussiveEventExtractorConfig selective = keep_all;
  selective.min_percussive_ratio = 0.5f * (lowest + highest);
  const std::vector<PercussiveEvent> kept = extract_percussive_events(audio, selective);

  std::vector<PercussiveEvent> expected;
  for (const PercussiveEvent& event : all) {
    if (event.percussive_ratio >= selective.min_percussive_ratio) expected.push_back(event);
  }
  REQUIRE(!expected.empty());
  REQUIRE(expected.size() < all.size());
  REQUIRE(kept.size() == expected.size());
  for (size_t i = 0; i < kept.size(); ++i) {
    INFO("survivor " << i);
    REQUIRE(kept[i].onset_sample == expected[i].onset_sample);
    REQUIRE(kept[i].offset_sample == expected[i].offset_sample);
    REQUIRE(kept[i].strength == expected[i].strength);
    REQUIRE(kept[i].peak_amplitude == expected[i].peak_amplitude);
    REQUIRE(kept[i].percussive_ratio == expected[i].percussive_ratio);
  }

  // Both ends of the range are accepted values, not errors: 0 keeps everything
  // and 1 keeps only what the separation called wholly percussive.
  PercussiveEventExtractorConfig ceiling = keep_all;
  ceiling.min_percussive_ratio = 1.0f;
  const std::vector<PercussiveEvent> at_ceiling = extract_percussive_events(audio, ceiling);
  REQUIRE(at_ceiling.size() <= all.size());
  for (const PercussiveEvent& event : at_ceiling) {
    REQUIRE(event.percussive_ratio >= 1.0f);
  }
}

// --- render_percussive_events: pass-through -------------------------------

TEST_CASE("render_percussive_events reproduces the input bit for bit for an identity set",
          "[event_model]") {
  const sonare::Audio audio = three_hits();

  // Hand-built spans first, so the property does not depend on the extractor.
  std::vector<PercussiveEvent> hand_built(2);
  hand_built[0].onset_sample = 0;
  hand_built[0].offset_sample = 22050;
  hand_built[1].onset_sample = 22050;
  hand_built[1].offset_sample = 44100;
  for (const PercussiveEvent& event : hand_built) REQUIRE(event.edit.is_identity());
  REQUIRE(first_mismatch(render_percussive_events(audio, hand_built), audio) == kNoMismatch);

  // Then the shape a caller actually holds between an extract and a render.
  const std::vector<PercussiveEvent> extracted = extract_percussive_events(audio);
  REQUIRE(!extracted.empty());
  const sonare::Audio rendered = render_percussive_events(audio, extracted);
  REQUIRE(rendered.size() == audio.size());
  REQUIRE(rendered.sample_rate() == audio.sample_rate());
  REQUIRE(first_mismatch(rendered, audio) == kNoMismatch);
}

TEST_CASE("render_percussive_events reproduces the input bit for bit for an empty set",
          "[event_model]") {
  const sonare::Audio audio = two_hits();
  const sonare::Audio rendered = render_percussive_events(audio, {});

  REQUIRE(rendered.size() == audio.size());
  REQUIRE(rendered.sample_rate() == audio.sample_rate());
  REQUIRE(first_mismatch(rendered, audio) == kNoMismatch);
}

// --- render_percussive_events: the separation is what moves ---------------

TEST_CASE("render_percussive_events mutes a hit and leaves the note under it sounding",
          "[event_model]") {
  // This is the property the whole separation exists for, so both halves are
  // asserted: the hit goes and the note stays, at its own level.
  const LayeredFixture fixture = layered_fixture();
  const std::vector<PercussiveEvent> events = extract_percussive_events(fixture.mixed);

  std::vector<PercussiveEvent> one = {event_at(events, static_cast<int64_t>(fixture.hit_start))};
  one[0].edit.muted = true;
  REQUIRE_FALSE(one[0].edit.is_identity());
  const sonare::Audio rendered = render_percussive_events(fixture.mixed, one);

  REQUIRE(rendered.size() == fixture.mixed.size());
  REQUIRE(rendered.sample_rate() == fixture.mixed.sample_rate());

  const size_t onset = static_cast<size_t>(one[0].onset_sample);
  const size_t offset = static_cast<size_t>(one[0].offset_sample);
  const size_t hit_end = fixture.hit_start + kHitSamples;

  // Half one: over the hit's own window, what is left of the hit is a fraction
  // of what was there. The "before" figure is the synthesised peak exactly,
  // recovered by subtracting the note-only reference.
  const float before =
      max_abs_difference(fixture.mixed, fixture.note_only, fixture.hit_start, hit_end);
  REQUIRE_THAT(before, WithinAbs(fixture.hit_peak, 1.0e-5f));
  const float after = max_abs_difference(rendered, fixture.note_only, fixture.hit_start, hit_end);
  REQUIRE(after < 0.5f * before);

  // Half two: the note is still there at its own level, bounded from both sides
  // rather than merely "not silent". A 0.8 sine has RMS 0.8 / sqrt(2).
  const double note_rms = 0.8 / std::sqrt(2.0);
  REQUIRE_THAT(rms(fixture.note_only, onset, offset), WithinRel(note_rms, 0.02));
  REQUIRE_THAT(rms(rendered, onset, offset), WithinRel(note_rms, 0.10));

  // Nothing outside the span moved at all.
  REQUIRE(first_mismatch(fixture.mixed, rendered, 0, onset) == kNoMismatch);
  REQUIRE(first_mismatch(fixture.mixed, rendered, offset, fixture.mixed.size()) == kNoMismatch);
}

TEST_CASE("render_percussive_events moves a hit to its new position", "[event_model]") {
  // Two isolated hits 1.3 s apart. The first is shifted by its own span length,
  // so source and destination are adjacent and disjoint.
  std::vector<float> samples(48510, 0.0f);  // 2.2 s
  add_hits(samples, {{static_cast<size_t>(kMoveHitStarts[0]), 41u, 0.50f},
                     {static_cast<size_t>(kMoveHitStarts[1]), 42u, 0.32f}});
  const sonare::Audio audio = audio_of(std::move(samples));

  const std::vector<PercussiveEvent> events = extract_percussive_events(audio);
  REQUIRE(events.size() == 2);
  std::vector<PercussiveEvent> one = {events[0]};
  require_span_covers(one[0], kMoveHitStarts[0]);
  const int64_t span = one[0].length_samples();
  REQUIRE(span == 11025);  // the 500 ms cap, the next onset being further off
  one[0].edit.time_offset_samples = span;

  const sonare::Audio rendered = render_percussive_events(audio, one);
  REQUIRE(rendered.size() == audio.size());

  // The span's own edges decide what is written; where the hit was synthesised
  // decides where to listen. They are not the same sample, because the span
  // opens in front of the transient.
  const size_t span_start = static_cast<size_t>(one[0].onset_sample);
  const size_t old_start = static_cast<size_t>(kMoveHitStarts[0]);
  const size_t new_start = old_start + static_cast<size_t>(span);
  const double source_level = rms(audio, old_start, old_start + kHitSamples);
  REQUIRE(source_level > 0.0);

  // Energy leaves the old position ...
  REQUIRE(rms(rendered, old_start, old_start + kHitSamples) < 0.4 * source_level);
  // ... and arrives at the new one, at the level it left with. Bounded above as
  // well: an arrival at ten times the level is not a move.
  REQUIRE(rms(rendered, new_start, new_start + kHitSamples) > 0.5 * source_level);
  REQUIRE(rms(rendered, new_start, new_start + kHitSamples) < 1.4 * source_level);

  // The moved signal is the lifted one translated, sample for sample, and not a
  // signal separated afresh at the destination. Rendering the same event muted
  // gives source - lifted, so `audio - silenced` is the lifted signal where it
  // sat and `rendered - silenced` is it where it landed; the two must agree
  // exactly. Like the gain relations, this holds whatever the lifted signal
  // turns out to be, so the tolerance is float rounding and nothing more.
  std::vector<PercussiveEvent> muted = {events[0]};
  muted[0].edit.muted = true;
  const sonare::Audio silenced = render_percussive_events(audio, muted);
  const size_t span_end = static_cast<size_t>(one[0].offset_sample);
  REQUIRE(span_end + static_cast<size_t>(span) <= audio.size());  // nothing truncated here
  float worst = 0.0f;
  for (size_t i = span_start; i < span_end; ++i) {
    const size_t destination = i + static_cast<size_t>(span);
    worst = std::max(worst, std::abs((rendered[destination] - silenced[destination]) -
                                     (audio[i] - silenced[i])));
  }
  REQUIRE(worst < 1.0e-5f);
  // ... and the translated signal is the hit, not a sliver of it.
  REQUIRE(max_abs_difference(audio, silenced, old_start, old_start + kHitSamples) > 0.15f);

  // Only the two spans are written, so the second hit is untouched bit for bit.
  REQUIRE(first_mismatch(audio, rendered, 0, span_start) == kNoMismatch);
  REQUIRE(first_mismatch(audio, rendered, span_end + static_cast<size_t>(span), audio.size()) ==
          kNoMismatch);
}

TEST_CASE("render_percussive_events scales a hit by its gain", "[event_model]") {
  const sonare::Audio audio = three_hits();
  const std::vector<PercussiveEvent> events = extract_percussive_events(audio);
  REQUIRE(!events.empty());

  auto render_with = [&](float gain_db, bool muted) {
    std::vector<PercussiveEvent> one = {events[0]};
    one[0].edit.gain_db = gain_db;
    one[0].edit.muted = muted;
    return render_percussive_events(audio, one);
  };

  const sonare::Audio louder = render_with(6.0206f, false);
  const sonare::Audio quieter = render_with(-6.0206f, false);
  const sonare::Audio silenced = render_with(0.0f, true);

  // The render is source + (gain - 1) * lifted, so a difference against the
  // source is the lifted signal at a known scale: +1 at +6.02 dB, -1/2 at
  // -6.02 dB, -1 when muted. Those relations are exact, whatever the lifted
  // signal turns out to be, so the tolerance is float rounding on a unit-scale
  // buffer and nothing more.
  const std::vector<float> up = difference(louder, audio);
  const std::vector<float> down = difference(quieter, audio);
  const std::vector<float> gone = difference(silenced, audio);
  REQUIRE(max_residual(up, down, 2.0f) < 1.0e-5f);
  REQUIRE(max_residual(up, gone, 1.0f) < 1.0e-5f);

  // The lifted signal is the hit and not a sliver of it, against the peak the
  // fixture synthesised: an isolated struck sound is nearly all percussive.
  REQUIRE(peak_of(up) > 0.3f * kThreeHitPeaks[0]);
  REQUIRE(peak_of(up) < 1.2f * kThreeHitPeaks[0]);

  // Direction and rough size over the hit itself. The window is anchored where
  // the hit was written, not on the span's opening edge -- the span opens in
  // front of the transient, so an edge-anchored window ends before the hit does
  // and misses the attack the ratio is about.
  const size_t window = static_cast<size_t>(kThreeHitStarts[0]);
  require_span_covers(events[0], kThreeHitStarts[0]);
  const double source_level = rms(audio, window, window + kHitSamples);
  REQUIRE(source_level > 0.0);
  const double louder_ratio = rms(louder, window, window + kHitSamples) / source_level;
  const double quieter_ratio = rms(quieter, window, window + kHitSamples) / source_level;
  REQUIRE(louder_ratio > 1.3);
  REQUIRE(louder_ratio < 2.05);
  REQUIRE(quieter_ratio > 0.45);
  REQUIRE(quieter_ratio < 0.85);
}

TEST_CASE("render_percussive_events truncates a shift that runs past either end", "[event_model]") {
  const sonare::Audio audio = three_hits();
  const std::vector<PercussiveEvent> events = extract_percussive_events(audio);
  REQUIRE(!events.empty());

  auto render_shifted = [&](int64_t offset_samples) {
    std::vector<PercussiveEvent> one = {events[0]};
    one[0].edit.time_offset_samples = offset_samples;
    return render_percussive_events(audio, one);
  };

  std::vector<PercussiveEvent> muted = {events[0]};
  muted[0].edit.muted = true;
  const sonare::Audio silenced = render_percussive_events(audio, muted);

  // Pushed clean past an end nothing arrives, so the result is the muted render
  // exactly. That is also the statement that nothing wrapped around.
  for (const int64_t offset_samples : {int64_t{100000}, int64_t{-100000}}) {
    INFO("offset " << offset_samples);
    const sonare::Audio rendered = render_shifted(offset_samples);
    REQUIRE(rendered.size() == audio.size());
    REQUIRE(rendered.sample_rate() == audio.sample_rate());
    REQUIRE(first_mismatch(rendered, silenced) == kNoMismatch);
  }

  // Half the span past the end: the half that fits still arrives.
  const int64_t span = events[0].length_samples();
  const int64_t overhang = static_cast<int64_t>(audio.size()) - events[0].onset_sample - span / 2;
  REQUIRE(overhang > 0);
  const sonare::Audio clipped = render_shifted(overhang);
  REQUIRE(clipped.size() == audio.size());
  const size_t hit_start = static_cast<size_t>(kThreeHitStarts[0]);
  const size_t destination = hit_start + static_cast<size_t>(overhang);
  REQUIRE(destination + kHitSamples <= audio.size());  // the hit itself still fits
  const double source_level = rms(audio, hit_start, hit_start + kHitSamples);
  REQUIRE(rms(clipped, destination, destination + kHitSamples) > 0.3 * source_level);
  REQUIRE(rms(clipped, destination, destination + kHitSamples) < 1.4 * source_level);
  // Nothing is written before the span, which is where a wrap would land.
  REQUIRE(first_mismatch(audio, clipped, 0, static_cast<size_t>(events[0].onset_sample)) ==
          kNoMismatch);
}

TEST_CASE("render_percussive_events accepts moved events that land on top of each other",
          "[event_model]") {
  // Overlap is a property of the source spans. Two events moved onto the same
  // stretch of the timeline are a legal set.
  const sonare::Audio audio = three_hits();
  const std::vector<PercussiveEvent> events = extract_percussive_events(audio);
  REQUIRE(events.size() == 3);

  // Moving the first event forward and the second back by the same amount
  // crosses them over when that amount is more than half the gap between the
  // spans and less than half their combined extent. Both bounds come from the
  // spans the extractor produced: a shift picked from the onset positions
  // happens to overlap for one set of spans and not for another, and then the
  // case passes its own precondition instead of testing the library.
  std::vector<PercussiveEvent> moved = {events[0], events[1]};
  const int64_t gap_half = (moved[1].onset_sample - moved[0].offset_sample) / 2;
  const int64_t extent_half = (moved[1].offset_sample - moved[0].onset_sample) / 2;
  const int64_t shift = (gap_half + extent_half) / 2;
  REQUIRE(shift > gap_half);
  REQUIRE(shift < extent_half);
  moved[0].edit.time_offset_samples = shift;
  moved[1].edit.time_offset_samples = -shift;

  // The destinations genuinely overlap, which is the case that must not be
  // rejected.
  REQUIRE(moved[0].onset_sample + shift < moved[1].offset_sample - shift);
  REQUIRE(moved[1].onset_sample - shift < moved[0].offset_sample + shift);

  const sonare::Audio rendered = render_percussive_events(audio, moved);
  REQUIRE(rendered.size() == audio.size());
  REQUIRE(rendered.sample_rate() == audio.sample_rate());
  for (size_t i = 0; i < rendered.size(); i += 97) {
    REQUIRE(std::isfinite(rendered[i]));
  }
}

// --- Validation -----------------------------------------------------------

TEST_CASE("extract_percussive_events rejects malformed audio and config", "[event_model]") {
  const sonare::Audio audio = three_hits();
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;

  const sonare::Audio empty_audio;
  REQUIRE(empty_audio.empty());
  REQUIRE(code_of([&] { extract_percussive_events(empty_audio); }) == kInvalid);

  // The separation is an inverse STFT, so a framing that cannot be overlap-added
  // back is an error before any audio is read.
  for (const Framing& broken : kBrokenFramings) {
    INFO("n_fft " << broken.n_fft << " hop " << broken.hop_length);
    PercussiveEventExtractorConfig config;
    config.separation.n_fft = broken.n_fft;
    config.separation.hop_length = broken.hop_length;
    REQUIRE(code_of([&] { extract_percussive_events(audio, config); }) == kInvalid);
  }

  for (const float bad : {kNaN, kInf, -kInf, 0.0f, -1.0f}) {
    INFO("max_event_ms " << bad);
    PercussiveEventExtractorConfig config;
    config.max_event_ms = bad;
    REQUIRE(code_of([&] { extract_percussive_events(audio, config); }) == kInvalid);
  }

  for (const float bad : {-0.01f, -1.0f, 1.01f, 2.0f, kNaN, kInf, -kInf}) {
    INFO("min_percussive_ratio " << bad);
    PercussiveEventExtractorConfig config;
    config.min_percussive_ratio = bad;
    REQUIRE(code_of([&] { extract_percussive_events(audio, config); }) == kInvalid);
  }

  // Both ends of the documented range are inside it.
  for (const float good : {0.0f, 1.0f}) {
    INFO("min_percussive_ratio " << good);
    PercussiveEventExtractorConfig config;
    config.min_percussive_ratio = good;
    REQUIRE_NOTHROW(extract_percussive_events(audio, config));
  }

  // A hop of exactly half the window is the inclusive end of the overlap rule,
  // so it extracts rather than throwing. The compact fixture keeps the cost of
  // running a separation per framing down.
  const sonare::Audio compact = two_hits();
  for (const Framing& valid : kValidFramings) {
    INFO("n_fft " << valid.n_fft << " hop " << valid.hop_length);
    PercussiveEventExtractorConfig config;
    config.separation.n_fft = valid.n_fft;
    config.separation.hop_length = valid.hop_length;
    REQUIRE_NOTHROW(extract_percussive_events(compact, config));
  }
  REQUIRE_NOTHROW(extract_percussive_events(audio));
}

TEST_CASE("render_percussive_events rejects malformed audio, spans, edits and config",
          "[event_model]") {
  const sonare::Audio audio = three_hits();
  const int64_t length = static_cast<int64_t>(audio.size());
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;

  const sonare::Audio empty_audio;
  REQUIRE(code_of([&] { render_percussive_events(empty_audio, {edited_event(0, 1600)}); }) ==
          kInvalid);

  // An empty span has nothing to lift, and a reversed one is not a span.
  REQUIRE(code_of([&] { render_percussive_events(audio, {edited_event(4410, 4410)}); }) ==
          kInvalid);
  REQUIRE(code_of([&] { render_percussive_events(audio, {edited_event(15435, 4410)}); }) ==
          kInvalid);

  // Outside the audio at either end.
  REQUIRE(code_of([&] { render_percussive_events(audio, {edited_event(-512, 4410)}); }) ==
          kInvalid);
  REQUIRE(code_of([&] { render_percussive_events(audio, {edited_event(4410, length + 1)}); }) ==
          kInvalid);
  REQUIRE(code_of([&] {
            render_percussive_events(audio, {edited_event(length, length + 4410)});
          }) == kInvalid);

  // Overlapping source spans are not a renderable set; touching ones are.
  const std::vector<PercussiveEvent> overlapping = {edited_event(0, 22050),
                                                    edited_event(11025, 33075)};
  REQUIRE(code_of([&] { render_percussive_events(audio, overlapping); }) == kInvalid);
  const std::vector<PercussiveEvent> adjacent = {edited_event(0, 22050),
                                                 edited_event(22050, 33075)};
  REQUIRE_NOTHROW(render_percussive_events(audio, adjacent));

  for (const float bad : {kNaN, kInf, -kInf}) {
    INFO("gain_db " << bad);
    PercussiveEvent event = edited_event(4410, 15435);
    event.edit.gain_db = bad;
    REQUIRE_FALSE(event.edit.is_identity());
    REQUIRE(code_of([&] { render_percussive_events(audio, {event}); }) == kInvalid);
  }

  // A zero-length fade is rejected, not degraded to a hard cut: what the fade
  // shapes is the signal being subtracted, so cutting it square at the span's
  // end leaves a step in the output -- a click at every edited event.
  for (const float bad : {kNaN, kInf, -kInf, -1.0f, 0.0f}) {
    INFO("fade_ms " << bad);
    PercussiveEventRenderConfig config;
    config.fade_ms = bad;
    REQUIRE(code_of([&] {
              render_percussive_events(audio, {edited_event(4410, 15435)}, config);
            }) == kInvalid);
  }

  // The renderer repeats the extraction's separation, so it is under the same
  // overlap-add rule. The all-identity seam, where the framing is checked
  // although no separation runs, is its own case below.
  for (const Framing& broken : kBrokenFramings) {
    INFO("n_fft " << broken.n_fft << " hop " << broken.hop_length);
    PercussiveEventRenderConfig config;
    config.separation.n_fft = broken.n_fft;
    config.separation.hop_length = broken.hop_length;
    REQUIRE(code_of([&] {
              render_percussive_events(audio, {edited_event(4410, 15435)}, config);
            }) == kInvalid);
  }

  for (const Framing& valid : kValidFramings) {
    INFO("n_fft " << valid.n_fft << " hop " << valid.hop_length);
    PercussiveEventRenderConfig config;
    config.separation.n_fft = valid.n_fft;
    config.separation.hop_length = valid.hop_length;
    REQUIRE_NOTHROW(render_percussive_events(audio, {edited_event(4410, 15435)}, config));
  }

  REQUIRE_NOTHROW(render_percussive_events(audio, {edited_event(4410, 15435)}));
}

TEST_CASE("render_percussive_events rejects a broken framing even for an all-identity set",
          "[event_model]") {
  // Two promises pull against each other here: an all-identity set is a
  // bit-exact pass-through that runs no separation, and the framing is checked
  // anyway. An implementation that validates the framing where it builds the
  // STFT returns the input instead of throwing, and only this case sees it.
  const sonare::Audio audio = three_hits();
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;

  std::vector<PercussiveEvent> identity(2);
  identity[0].onset_sample = 0;
  identity[0].offset_sample = 22050;
  identity[1].onset_sample = 22050;
  identity[1].offset_sample = 44100;
  for (const PercussiveEvent& event : identity) REQUIRE(event.edit.is_identity());

  // The same set on the default framing is the pass-through, so every throw
  // below is the framing's doing and not the set's.
  REQUIRE(first_mismatch(render_percussive_events(audio, identity), audio) == kNoMismatch);

  for (const Framing& broken : kBrokenFramings) {
    INFO("n_fft " << broken.n_fft << " hop " << broken.hop_length);
    PercussiveEventRenderConfig config;
    config.separation.n_fft = broken.n_fft;
    config.separation.hop_length = broken.hop_length;
    REQUIRE(code_of([&] { render_percussive_events(audio, identity, config); }) == kInvalid);
    // And on the emptiest set there is: an unusable config is an error on every
    // set, not on the ones that happen to reach the separation.
    REQUIRE(code_of([&] { render_percussive_events(audio, {}, config); }) == kInvalid);
  }

  // Inside the rule the pass-through still holds, including at exactly half the
  // window, so the rejections are not simply "any non-default framing".
  for (const Framing& valid : kValidFramings) {
    INFO("n_fft " << valid.n_fft << " hop " << valid.hop_length);
    PercussiveEventRenderConfig config;
    config.separation.n_fft = valid.n_fft;
    config.separation.hop_length = valid.hop_length;
    REQUIRE(first_mismatch(render_percussive_events(audio, identity, config), audio) ==
            kNoMismatch);
    REQUIRE(first_mismatch(render_percussive_events(audio, {}, config), audio) == kNoMismatch);
  }
}

TEST_CASE("render_percussive_events validates an event whose edit is the identity",
          "[event_model]") {
  // An unrenderable set is unrenderable whether or not this call would touch it,
  // so the all-identity fast path does not get to skip the checks.
  const sonare::Audio audio = three_hits();
  const int64_t length = static_cast<int64_t>(audio.size());
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;

  auto identity_span = [](int64_t onset, int64_t offset) {
    PercussiveEvent event;
    event.onset_sample = onset;
    event.offset_sample = offset;
    REQUIRE(event.edit.is_identity());
    return event;
  };

  REQUIRE(code_of([&] { render_percussive_events(audio, {identity_span(4410, 4410)}); }) ==
          kInvalid);
  REQUIRE(code_of([&] { render_percussive_events(audio, {identity_span(15435, 4410)}); }) ==
          kInvalid);
  REQUIRE(code_of([&] { render_percussive_events(audio, {identity_span(-512, 4410)}); }) ==
          kInvalid);
  REQUIRE(code_of([&] { render_percussive_events(audio, {identity_span(4410, length + 1)}); }) ==
          kInvalid);

  const std::vector<PercussiveEvent> overlapping = {identity_span(0, 22050),
                                                    identity_span(11025, 33075)};
  REQUIRE(code_of([&] { render_percussive_events(audio, overlapping); }) == kInvalid);

  // One bad event poisons a set that is otherwise renderable and otherwise
  // entirely identity.
  const std::vector<PercussiveEvent> mixed = {identity_span(0, 11025), identity_span(22050, 11025)};
  REQUIRE(code_of([&] { render_percussive_events(audio, mixed); }) == kInvalid);
}

// --- Parameter combinations -----------------------------------------------

TEST_CASE("extract_percussive_events holds its contract across its config space", "[event_model]") {
  // Six independent config axes, covered pairwise: every pair of levels from any
  // two axes appears in some row. Every framing here is inside the overlap-add
  // rule, one of them at exactly half the window. Picking plausible-looking combinations by eye
  // leaves whole pairs untried, and the interactions here are between the
  // framing, the mask shape and the two span rules.
  struct Row {
    int n_fft;
    int hop_length;
    float max_event_ms;
    float min_percussive_ratio;
    bool soft_mask;
    float power;
  };
  static const Row kRows[] = {
      {2048, 256, 500.0f, 1.00f, true, 1.0f},  {1024, 512, 5000.0f, 1.00f, false, 2.0f},
      {2048, 512, 40.0f, 0.00f, false, 1.0f},  {1024, 256, 40.0f, 0.25f, true, 2.0f},
      {1024, 512, 500.0f, 0.00f, true, 2.0f},  {2048, 256, 5000.0f, 0.25f, false, 1.0f},
      {2048, 512, 500.0f, 0.25f, false, 2.0f}, {1024, 256, 5000.0f, 0.00f, true, 1.0f},
      {2048, 256, 40.0f, 1.00f, true, 2.0f},
  };

  const sonare::Audio audio = two_hits();
  for (const Row& row : kRows) {
    INFO("n_fft " << row.n_fft << " hop " << row.hop_length << " max_event_ms " << row.max_event_ms
                  << " min_ratio " << row.min_percussive_ratio << " soft_mask " << row.soft_mask
                  << " power " << row.power);
    PercussiveEventExtractorConfig config;
    config.separation.n_fft = row.n_fft;
    config.separation.hop_length = row.hop_length;
    config.separation.hpss.use_soft_mask = row.soft_mask;
    config.separation.hpss.power = row.power;
    config.max_event_ms = row.max_event_ms;
    config.min_percussive_ratio = row.min_percussive_ratio;

    const std::vector<PercussiveEvent> events = extract_percussive_events(audio, config);
    require_well_formed(events, audio);
    for (const PercussiveEvent& event : events) {
      REQUIRE(event.percussive_ratio >= row.min_percussive_ratio);
    }
    // Two struck sounds in silence: a row that keeps everything must find them,
    // or the row's invariants are vacuous.
    if (row.min_percussive_ratio == 0.0f) REQUIRE(events.size() == 2);

    // Whatever the config, the set it produced is renderable and a pass-through.
    PercussiveEventRenderConfig render_config;
    render_config.separation = config.separation;
    REQUIRE(first_mismatch(render_percussive_events(audio, events, render_config), audio) ==
            kNoMismatch);
  }
}

TEST_CASE("render_percussive_events holds its contract across its edit and config space",
          "[event_model]") {
  // Five axes, covered pairwise. The interactions worth reaching are between a
  // shift that runs off an end, a mute that discards the gain, and a fade long
  // enough to cover a good part of the span.
  struct Row {
    int64_t time_offset_samples;
    float gain_db;
    bool muted;
    float fade_ms;
    int n_fft;
    int hop_length;
  };
  static const Row kRows[] = {
      {0, -6.0206f, true, 50.0f, 1024, 256},        {-2000, -24.0f, false, 1.0f, 2048, 512},
      {-100000, 6.0206f, true, 5.0f, 2048, 512},    {2000, 0.0f, false, 5.0f, 1024, 256},
      {100000, 6.0206f, false, 50.0f, 2048, 512},   {100000, 0.0f, true, 1.0f, 1024, 256},
      {-2000, -24.0f, true, 5.0f, 1024, 256},       {2000, -6.0206f, true, 1.0f, 2048, 512},
      {-100000, -6.0206f, false, 50.0f, 1024, 256}, {0, 0.0f, false, 1.0f, 2048, 512},
      {-2000, 0.0f, true, 50.0f, 2048, 512},        {2000, 6.0206f, true, 50.0f, 1024, 256},
      {100000, -6.0206f, true, 5.0f, 1024, 256},    {-100000, -24.0f, true, 50.0f, 2048, 512},
      {0, 6.0206f, false, 1.0f, 2048, 512},         {-100000, 0.0f, true, 1.0f, 2048, 512},
      {0, -24.0f, false, 5.0f, 2048, 512},          {-2000, -6.0206f, false, 5.0f, 1024, 256},
      {-2000, 6.0206f, true, 5.0f, 2048, 512},      {2000, -24.0f, true, 5.0f, 2048, 512},
      {100000, -24.0f, true, 5.0f, 1024, 256},
  };

  const sonare::Audio audio = two_hits();
  const std::vector<PercussiveEvent> events = extract_percussive_events(audio);
  REQUIRE(events.size() == 2);

  for (const Row& row : kRows) {
    INFO("offset " << row.time_offset_samples << " gain_db " << row.gain_db << " muted "
                   << row.muted << " fade_ms " << row.fade_ms << " n_fft " << row.n_fft << " hop "
                   << row.hop_length);
    std::vector<PercussiveEvent> edited = {events[0]};
    edited[0].edit.time_offset_samples = row.time_offset_samples;
    edited[0].edit.gain_db = row.gain_db;
    edited[0].edit.muted = row.muted;

    PercussiveEventRenderConfig config;
    config.fade_ms = row.fade_ms;
    config.separation.n_fft = row.n_fft;
    config.separation.hop_length = row.hop_length;

    const sonare::Audio rendered = render_percussive_events(audio, edited, config);
    REQUIRE(rendered.size() == audio.size());
    REQUIRE(rendered.sample_rate() == audio.sample_rate());
    for (size_t i = 0; i < rendered.size(); ++i) {
      if (!std::isfinite(rendered[i])) FAIL("non-finite sample at " << i);
    }
    // The second hit is never edited, so the output is neither silent nor blown
    // up whatever this row did to the first.
    REQUIRE(peak(rendered, 0, rendered.size()) > 0.1f);
    REQUIRE(peak(rendered, 0, rendered.size()) < 2.0f);

    if (edited[0].edit.is_identity()) {
      REQUIRE(first_mismatch(rendered, audio) == kNoMismatch);
    }
  }
}
