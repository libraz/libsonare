/// @file sonare_c_percussive_events_test.cpp
/// @brief Tests for the percussive-event C API: sonare_extract_percussive_events,
///        sonare_free_percussive_events and sonare_render_percussive_events.
///
/// The model itself is covered in tests/editing/percussive_event_test.cpp. What
/// is tested here is what only the boundary can get wrong: NULL and zero-count
/// handling, the zero-is-default rule the versioned C configs add, the int32
/// spelling of muted, a result cleared before validation and cleared again when
/// released, and a set that travels out of extraction and back into rendering
/// unchanged.
///
/// The fixtures are the core tests' -- 22050 Hz, exponentially decaying noise
/// bursts at distinct levels and distinct seeds -- so the few statistical bounds
/// here are the ones that file already holds.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <vector>

#include "util/constants.h"

namespace {

constexpr int kSampleRate = 22050;
/// The default framing's hop, which every fixture position is reasoned in.
constexpr int64_t kHopLength = 512;
/// How far in front of its transient an onset may legitimately sit: the
/// detector's backtrack bound of ten frames, at the default framing.
constexpr int64_t kBacktrackReach = 10 * kHopLength;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr size_t kNoMismatch = static_cast<size_t>(-1);
/// muted is an int32 on this side and a bool in the core, so every non-zero
/// spelling has to mute -- including one no bool ever produced.
constexpr int32_t kLargeMuted = std::numeric_limits<int32_t>::max();

constexpr size_t kHitSamples = 1323;  // 60 ms at 22050 Hz, exactly.
constexpr float kHitDecayMs = 12.0f;

/// Where three_hits() writes its hits, and at what peak.
constexpr int64_t kThreeHitStarts[] = {4410, 13230, 22050};
constexpr float kThreeHitPeaks[] = {0.50f, 0.34f, 0.22f};
/// Where note_then_hit() writes its sustained note and its isolated hit.
constexpr int64_t kNoteStart = 8820;
constexpr int64_t kLateHitStart = 52920;

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

/// @brief An exponentially decaying noise burst, normalised so its peak is
///        exactly @p amplitude.
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
///        edge at each end, so the buffer carries no step discontinuity and the
///        span reads as harmonic rather than as one long click.
void add_note(std::vector<float>& into, size_t start, size_t end, float frequency_hz,
              float amplitude) {
  constexpr size_t kEdgeSamples = 44;  // 2 ms at 22050 Hz.
  for (size_t i = start; i < end && i < into.size(); ++i) {
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

/// @brief Three isolated hits at descending, distinct levels, 400 ms apart,
///        followed by a tail longer than the default 500 ms cap.
std::vector<float> three_hits() {
  std::vector<float> samples(44100, 0.0f);  // 2.0 s
  add_hits(samples, {{static_cast<size_t>(kThreeHitStarts[0]), 1u, kThreeHitPeaks[0]},
                     {static_cast<size_t>(kThreeHitStarts[1]), 2u, kThreeHitPeaks[1]},
                     {static_cast<size_t>(kThreeHitStarts[2]), 3u, kThreeHitPeaks[2]}});
  return samples;
}

/// @brief Two isolated hits inside one second: the compact fixture the parameter
///        sweeps run on.
std::vector<float> two_hits() {
  std::vector<float> samples(22050, 0.0f);  // 1.0 s
  add_hits(samples, {{3528, 31u, 0.50f}, {12348, 32u, 0.30f}});
  return samples;
}

/// @brief A sustained note and, well after it, one isolated hit.
/// @details The two onsets are the same kind of event to the detector and
///          opposite kinds to the separation, which is what keeps
///          percussive_ratio from being read at one end of its range only.
std::vector<float> note_then_hit() {
  std::vector<float> samples(66150, 0.0f);  // 3.0 s
  add_note(samples, static_cast<size_t>(kNoteStart), 44100, 330.0f, 0.8f);
  add_hits(samples, {{static_cast<size_t>(kLateHitStart), 11u, 0.50f}});
  return samples;
}

SonareError extract_at(const std::vector<float>& samples, const SonarePercussiveEventConfig* config,
                       SonarePercussiveEventsResult* out) {
  return sonare_extract_percussive_events(samples.data(), samples.size(), kSampleRate, config, out);
}

/// An extraction that must succeed, with the result's own NULL/count agreement
/// checked on the way out. The caller owns the result.
SonarePercussiveEventsResult extracted(const std::vector<float>& samples,
                                       const SonarePercussiveEventConfig* config = nullptr) {
  SonarePercussiveEventsResult out{};
  REQUIRE(extract_at(samples, config, &out) == SONARE_OK);
  REQUIRE((out.count == 0) == (out.events == nullptr));
  return out;
}

SonareError render_at(const std::vector<float>& samples, const SonarePercussiveEvent* events,
                      size_t count, const SonarePercussiveRenderConfig* config, float** out,
                      size_t* out_length) {
  return sonare_render_percussive_events(samples.data(), samples.size(), kSampleRate, events, count,
                                         config, out, out_length);
}

/// A render that must succeed, copied out and released immediately, so every
/// call here also exercises the documented sonare_free_floats release path.
std::vector<float> render_ok(const std::vector<float>& samples, const SonarePercussiveEvent* events,
                             size_t count, const SonarePercussiveRenderConfig* config = nullptr) {
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(render_at(samples, events, count, config, &out, &out_length) == SONARE_OK);
  REQUIRE(out_length == samples.size());
  REQUIRE(out != nullptr);
  std::vector<float> copy(out, out + out_length);
  sonare_free_floats(out);
  return copy;
}

SonarePercussiveEvent* poisoned_events() {
  return reinterpret_cast<SonarePercussiveEvent*>(static_cast<std::uintptr_t>(0x1));
}

/// Index of the first differing sample in [lo, hi), or kNoMismatch.
size_t first_mismatch(const std::vector<float>& a, const std::vector<float>& b, size_t lo,
                      size_t hi) {
  if (a.size() != b.size()) return 0;
  const size_t end = std::min(hi, a.size());
  for (size_t i = lo; i < end; ++i) {
    if (a[i] != b[i]) return i;
  }
  return kNoMismatch;
}

size_t first_mismatch(const std::vector<float>& a, const std::vector<float>& b) {
  return first_mismatch(a, b, 0, a.size());
}

double rms(const std::vector<float>& data, size_t lo, size_t hi) {
  const size_t end = std::min(hi, data.size());
  double sum = 0.0;
  for (size_t i = lo; i < end; ++i) {
    sum += static_cast<double>(data[i]) * static_cast<double>(data[i]);
  }
  return std::sqrt(sum / static_cast<double>(std::max<size_t>(1, end - lo)));
}

float peak(const std::vector<float>& data, size_t lo, size_t hi) {
  const size_t end = std::min(hi, data.size());
  float highest = 0.0f;
  for (size_t i = lo; i < end; ++i) highest = std::max(highest, std::abs(data[i]));
  return highest;
}

float max_difference(const std::vector<float>& a, const std::vector<float>& b, size_t lo,
                     size_t hi) {
  const size_t end = std::min(hi, std::min(a.size(), b.size()));
  float worst = 0.0f;
  for (size_t i = lo; i < end; ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
  return worst;
}

float max_difference(const std::vector<float>& a, const std::vector<float>& b) {
  return max_difference(a, b, 0, std::min(a.size(), b.size()));
}

/// @brief Asserts that a render meant to change the sound did, and that what came
///        back is still a signal.
/// @details A bare "the output differs from the source" passes for silence,
///          which differs from the source by the source's own peak, and for a
///          blow-up. So the difference floor is paired with a two-sided band on
///          the amplitude.
void require_edited(const std::vector<float>& rendered, const std::vector<float>& source) {
  REQUIRE(max_difference(rendered, source) > 0.05f);
  const float source_peak = peak(source, 0, source.size());
  const float rendered_peak = peak(rendered, 0, rendered.size());
  REQUIRE(rendered_peak > 0.5f * source_peak);
  REQUIRE(rendered_peak < 2.0f * source_peak);
}

/// Exact field-by-field equality. The compared calls run identical arithmetic,
/// so a tolerance here would hide a config field that was silently ignored.
bool same_events(const SonarePercussiveEventsResult& a, const SonarePercussiveEventsResult& b) {
  if (a.count != b.count) return false;
  for (size_t i = 0; i < a.count; ++i) {
    const SonarePercussiveEvent& lhs = a.events[i];
    const SonarePercussiveEvent& rhs = b.events[i];
    if (lhs.onset_sample != rhs.onset_sample || lhs.offset_sample != rhs.offset_sample ||
        lhs.strength != rhs.strength || lhs.peak_amplitude != rhs.peak_amplitude ||
        lhs.percussive_ratio != rhs.percussive_ratio ||
        lhs.edit.time_offset_samples != rhs.edit.time_offset_samples ||
        lhs.edit.gain_db != rhs.edit.gain_db || lhs.edit.muted != rhs.edit.muted) {
      return false;
    }
  }
  return true;
}

/// Every invariant an extracted set has to satisfy for the renderer to accept it
/// back: spans inside the audio, ascending and disjoint, measurements in range,
/// and the identity edit in its int32 spelling.
void require_well_formed(const SonarePercussiveEventsResult& result, size_t length) {
  REQUIRE((result.count == 0) == (result.events == nullptr));
  int64_t previous_offset = 0;
  for (size_t i = 0; i < result.count; ++i) {
    INFO("event " << i);
    const SonarePercussiveEvent& event = result.events[i];
    REQUIRE(event.onset_sample >= 0);
    REQUIRE(event.offset_sample > event.onset_sample);
    REQUIRE(event.offset_sample <= static_cast<int64_t>(length));
    REQUIRE(event.onset_sample >= previous_offset);
    previous_offset = event.offset_sample;
    REQUIRE(std::isfinite(event.strength));
    REQUIRE(std::isfinite(event.peak_amplitude));
    REQUIRE(event.peak_amplitude >= 0.0f);
    REQUIRE(std::isfinite(event.percussive_ratio));
    REQUIRE(event.percussive_ratio >= 0.0f);
    REQUIRE(event.percussive_ratio <= 1.0f);
    REQUIRE(event.edit.time_offset_samples == 0);
    REQUIRE(event.edit.gain_db == 0.0f);
    REQUIRE(event.edit.muted == 0);
  }
}

/// @brief Index of the event whose onset sits closest to @p sample.
size_t nearest_event(const SonarePercussiveEventsResult& result, int64_t sample) {
  REQUIRE(result.count > 0);
  size_t best = 0;
  int64_t best_distance = std::abs(result.events[0].onset_sample - sample);
  for (size_t i = 1; i < result.count; ++i) {
    const int64_t distance = std::abs(result.events[i].onset_sample - sample);
    if (distance < best_distance) {
      best_distance = distance;
      best = i;
    }
  }
  return best;
}

/// @brief The span opens in front of the transient at @p start and closes past it.
/// @details Not a symmetric window: peak-picking lands after a transient starts
///          and backtracking is what moves the edge in front of it, so an onset
///          at or after @p start would mean the span opened inside its own hit.
///          The early bound is the detector's own limit on that travel, so the
///          span cannot have wandered back into the previous hit either.
void require_span_covers(const SonarePercussiveEvent& event, int64_t start,
                         int64_t coverage = static_cast<int64_t>(kHitSamples)) {
  REQUIRE(event.onset_sample <= start);
  REQUIRE(event.onset_sample > start - kBacktrackReach);
  REQUIRE(event.offset_sample > start + coverage);
}

}  // namespace

// --- Extraction: what comes back across the boundary ----------------------

TEST_CASE("sonare_extract_percussive_events returns one event per hit with a zeroed edit",
          "[c_api][percussive_events]") {
  const std::vector<float> samples = three_hits();
  SonarePercussiveEventsResult out = extracted(samples);

  REQUIRE(out.count == 3);
  REQUIRE(out.events != nullptr);
  require_well_formed(out, samples.size());

  for (size_t i = 0; i < 3; ++i) {
    INFO("hit " << i);
    require_span_covers(out.events[i], kThreeHitStarts[i]);
    REQUIRE(out.events[i].strength > 0.0f);
    // Struck sounds in silence: the separation has to call them percussive. The
    // other end of the range is the note_then_hit case below.
    REQUIRE(out.events[i].percussive_ratio > 0.5f);
  }

  // Each hit's own level, in the order they were synthesised at. A span that
  // measured a neighbour, or the whole buffer, cannot reproduce this.
  REQUIRE(out.events[0].peak_amplitude > out.events[1].peak_amplitude);
  REQUIRE(out.events[1].peak_amplitude > out.events[2].peak_amplitude);
  REQUIRE(out.events[0].peak_amplitude > 0.25f);
  REQUIRE(out.events[0].peak_amplitude <= 0.55f);

  // Interior spans are closed by the following onset exactly; the last one is
  // closed by the 500 ms cap, which is 11025 samples at this rate.
  REQUIRE(out.events[0].offset_sample == out.events[1].onset_sample);
  REQUIRE(out.events[1].offset_sample == out.events[2].onset_sample);
  REQUIRE(out.events[2].offset_sample - out.events[2].onset_sample == 11025);

  sonare_free_percussive_events(&out);
}

TEST_CASE("sonare_extract_percussive_events returns nothing detected as NULL and a zero count",
          "[c_api][percussive_events]") {
  // Audio with nothing in it is not an error, and neither is audio the detector
  // finds nothing in: both come back as a cleared result and SONARE_OK.
  const std::vector<float> silence(11025, 0.0f);
  SonarePercussiveEventsResult from_silence{};
  REQUIRE(extract_at(silence, nullptr, &from_silence) == SONARE_OK);
  REQUIRE(from_silence.events == nullptr);
  REQUIRE(from_silence.count == 0);
  // Releasing a result that owns nothing is a no-op rather than a free of an
  // uninitialised pointer.
  sonare_free_percussive_events(&from_silence);

  const std::vector<float> samples = two_hits();
  SonarePercussiveEventConfig unreachable{};
  unreachable.onset_delta = 1.0e6f;
  SonarePercussiveEventsResult none{};
  REQUIRE(extract_at(samples, &unreachable, &none) == SONARE_OK);
  REQUIRE(none.events == nullptr);
  REQUIRE(none.count == 0);

  // A zero-count set renders the input back bit for bit, so the empty result is
  // usable rather than merely legal.
  REQUIRE(first_mismatch(render_ok(samples, none.events, none.count), samples) == kNoMismatch);
  sonare_free_percussive_events(&none);
}

TEST_CASE("percussive_ratio reads both ends of its range through the C layer",
          "[c_api][percussive_events]") {
  // A metric at its ceiling for every input is unverified, so the fixture has to
  // produce both: an isolated hit sits near 1 and a sustained attack near 0,
  // because the sustain owns its span's energy.
  const std::vector<float> samples = note_then_hit();
  SonarePercussiveEventsResult out = extracted(samples);
  REQUIRE(out.count >= 2);
  require_well_formed(out, samples.size());

  const size_t note_index = nearest_event(out, kNoteStart);
  const size_t hit_index = nearest_event(out, kLateHitStart);
  REQUIRE(note_index != hit_index);
  require_span_covers(out.events[note_index], kNoteStart);
  require_span_covers(out.events[hit_index], kLateHitStart);

  const float note_ratio = out.events[note_index].percussive_ratio;
  const float hit_ratio = out.events[hit_index].percussive_ratio;
  REQUIRE(hit_ratio > 0.5f);
  REQUIRE(note_ratio < 0.9f);
  REQUIRE(note_ratio < 0.5f * hit_ratio);

  sonare_free_percussive_events(&out);
}

// --- The zero-is-default rule ---------------------------------------------

TEST_CASE("a NULL SonarePercussiveEventConfig and a zeroed one select the same defaults",
          "[c_api][percussive_events]") {
  const std::vector<float> samples = three_hits();

  SonarePercussiveEventsResult from_null = extracted(samples);
  SonarePercussiveEventConfig zeroed{};
  SonarePercussiveEventsResult from_zeroed = extracted(samples, &zeroed);
  REQUIRE(from_null.count == 3);
  REQUIRE(same_events(from_null, from_zeroed));

  // struct_version 0 and 1 both select the version-1 layout.
  SonarePercussiveEventConfig version_one{};
  version_one.struct_version = 1;
  SonarePercussiveEventsResult from_v1 = extracted(samples, &version_one);
  REQUIRE(same_events(from_null, from_v1));

  sonare_free_percussive_events(&from_null);
  sonare_free_percussive_events(&from_zeroed);
  sonare_free_percussive_events(&from_v1);
}

TEST_CASE("every SonarePercussiveEventConfig field takes its default at 0",
          "[c_api][percussive_events]") {
  const std::vector<float> samples = three_hits();
  SonarePercussiveEventsResult defaults = extracted(samples);
  REQUIRE(defaults.count == 3);

  SECTION("the framing defaults to 2048 and 512") {
    SonarePercussiveEventConfig config{};
    config.n_fft = 2048;
    config.hop_length = 512;
    SonarePercussiveEventsResult explicit_default = extracted(samples, &config);
    REQUIRE(same_events(defaults, explicit_default));
    sonare_free_percussive_events(&explicit_default);

    // A hop of exactly half the window is the inclusive end of the overlap-add
    // rule, and one sample past it is not. Both are read against the default
    // n_fft, so the pair is resolved field by field.
    SonarePercussiveEventConfig half{};
    half.hop_length = 1024;
    SonarePercussiveEventsResult at_half = extracted(samples, &half);
    require_well_formed(at_half, samples.size());
    sonare_free_percussive_events(&at_half);

    SonarePercussiveEventConfig past_half{};
    past_half.hop_length = 1025;
    SonarePercussiveEventsResult rejected{};
    REQUIRE(extract_at(samples, &past_half, &rejected) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("both median kernels default to 31") {
    SonarePercussiveEventConfig config{};
    config.hpss_kernel_harmonic = 31;
    config.hpss_kernel_percussive = 31;
    SonarePercussiveEventsResult explicit_default = extracted(samples, &config);
    REQUIRE(same_events(defaults, explicit_default));
    sonare_free_percussive_events(&explicit_default);

    // A single-frame harmonic kernel is the median along time reduced to the
    // identity, so nothing is separated by time any more and a struck sound is
    // no longer called percussive by it. Read as the largest ratio in the set,
    // which does not depend on the detector finding the same hits.
    SonarePercussiveEventConfig flattened{};
    flattened.hpss_kernel_harmonic = 1;
    SonarePercussiveEventsResult short_kernel = extracted(samples, &flattened);
    require_well_formed(short_kernel, samples.size());
    REQUIRE(short_kernel.count > 0);
    auto largest_ratio = [](const SonarePercussiveEventsResult& result) {
      float highest = 0.0f;
      for (size_t i = 0; i < result.count; ++i) {
        highest = std::max(highest, result.events[i].percussive_ratio);
      }
      return highest;
    };
    REQUIRE(largest_ratio(short_kernel) < largest_ratio(defaults));
    sonare_free_percussive_events(&short_kernel);
  }

  SECTION("onset_wait defaults to one frame") {
    SonarePercussiveEventConfig config{};
    config.onset_wait = 1;
    SonarePercussiveEventsResult explicit_default = extracted(samples, &config);
    REQUIRE(same_events(defaults, explicit_default));
    sonare_free_percussive_events(&explicit_default);

    // 200 frames is 4.6 s at this framing, longer than the fixture, so the
    // spacing rule leaves exactly the first onset -- at the sample the default
    // run put it, since backtracking is per onset.
    SonarePercussiveEventConfig spaced{};
    spaced.onset_wait = 200;
    SonarePercussiveEventsResult sparse = extracted(samples, &spaced);
    REQUIRE(sparse.count == 1);
    REQUIRE(sparse.events[0].onset_sample == defaults.events[0].onset_sample);
    sonare_free_percussive_events(&sparse);
  }

  SECTION("onset_delta defaults to 0.06") {
    SonarePercussiveEventConfig config{};
    config.onset_delta = 0.06f;
    SonarePercussiveEventsResult explicit_default = extracted(samples, &config);
    REQUIRE(same_events(defaults, explicit_default));
    sonare_free_percussive_events(&explicit_default);

    // Raising it finds fewer, stronger hits. The threshold is derived from the
    // strengths this fixture actually produced rather than picked by eye: set
    // between the top two, no hit but the strongest can clear it.
    std::vector<float> strengths;
    for (size_t i = 0; i < defaults.count; ++i) strengths.push_back(defaults.events[i].strength);
    std::sort(strengths.begin(), strengths.end(), std::greater<float>());
    REQUIRE(strengths.size() == 3);
    REQUIRE(strengths[0] > strengths[1]);

    SonarePercussiveEventConfig selective{};
    selective.onset_delta = 0.5f * (strengths[0] + strengths[1]);
    SonarePercussiveEventsResult fewer = extracted(samples, &selective);
    REQUIRE(fewer.count < defaults.count);
    REQUIRE(fewer.count <= 1);
    if (fewer.count == 1) REQUIRE(fewer.events[0].strength == strengths[0]);
    sonare_free_percussive_events(&fewer);
  }

  SECTION("max_event_ms defaults to 500 ms") {
    SonarePercussiveEventConfig config{};
    config.max_event_ms = 500.0f;
    SonarePercussiveEventsResult explicit_default = extracted(samples, &config);
    REQUIRE(same_events(defaults, explicit_default));
    sonare_free_percussive_events(&explicit_default);

    // 40 ms is 882 samples at this rate, exactly, and shorter than the 400 ms
    // between hits, so it closes every span including the interior ones. The cap
    // moves an offset and never an onset.
    SonarePercussiveEventConfig tight{};
    tight.max_event_ms = 40.0f;
    SonarePercussiveEventsResult capped = extracted(samples, &tight);
    REQUIRE(capped.count == defaults.count);
    for (size_t i = 0; i < capped.count; ++i) {
      INFO("event " << i);
      REQUIRE(capped.events[i].onset_sample == defaults.events[i].onset_sample);
      REQUIRE(capped.events[i].offset_sample - capped.events[i].onset_sample == 882);
    }
    sonare_free_percussive_events(&capped);
  }

  sonare_free_percussive_events(&defaults);
}

TEST_CASE("min_percussive_ratio is assigned at 0 rather than read as a default",
          "[c_api][percussive_events]") {
  // Every other field takes its default at 0; this one's default and its own
  // meaning are both 0, so it is assigned as-is. The first half of that is that
  // nothing changes at 0, the second is that a raised threshold selects.
  const std::vector<float> samples = note_then_hit();
  SonarePercussiveEventsResult from_null = extracted(samples);
  SonarePercussiveEventConfig keep_all{};
  SonarePercussiveEventsResult zeroed = extracted(samples, &keep_all);
  SonarePercussiveEventConfig explicit_zero{};
  explicit_zero.min_percussive_ratio = 0.0f;
  SonarePercussiveEventsResult from_zero = extracted(samples, &explicit_zero);
  REQUIRE(from_null.count >= 2);
  REQUIRE(same_events(from_null, zeroed));
  REQUIRE(same_events(from_null, from_zero));

  float lowest = from_null.events[0].percussive_ratio;
  float highest = from_null.events[0].percussive_ratio;
  for (size_t i = 0; i < from_null.count; ++i) {
    lowest = std::min(lowest, from_null.events[i].percussive_ratio);
    highest = std::max(highest, from_null.events[i].percussive_ratio);
  }
  // Without a spread there is no threshold that selects, and the case is vacuous.
  REQUIRE(highest - lowest > 0.2f);

  SonarePercussiveEventConfig selective{};
  selective.min_percussive_ratio = 0.5f * (lowest + highest);
  SonarePercussiveEventsResult kept = extracted(samples, &selective);

  // Spans are fixed before anything is dropped, so the survivors are the very
  // same events -- compared field by field, not approximately.
  std::vector<size_t> expected;
  for (size_t i = 0; i < from_null.count; ++i) {
    if (from_null.events[i].percussive_ratio >= selective.min_percussive_ratio)
      expected.push_back(i);
  }
  REQUIRE(!expected.empty());
  REQUIRE(expected.size() < from_null.count);
  REQUIRE(kept.count == expected.size());
  for (size_t i = 0; i < kept.count; ++i) {
    INFO("survivor " << i);
    const SonarePercussiveEvent& survivor = kept.events[i];
    const SonarePercussiveEvent& original = from_null.events[expected[i]];
    REQUIRE(survivor.onset_sample == original.onset_sample);
    REQUIRE(survivor.offset_sample == original.offset_sample);
    REQUIRE(survivor.strength == original.strength);
    REQUIRE(survivor.peak_amplitude == original.peak_amplitude);
    REQUIRE(survivor.percussive_ratio == original.percussive_ratio);
  }

  // Both ends of the documented range are values rather than errors.
  SonarePercussiveEventConfig ceiling{};
  ceiling.min_percussive_ratio = 1.0f;
  SonarePercussiveEventsResult at_ceiling = extracted(samples, &ceiling);
  REQUIRE(at_ceiling.count <= from_null.count);
  for (size_t i = 0; i < at_ceiling.count; ++i) {
    REQUIRE(at_ceiling.events[i].percussive_ratio >= 1.0f);
  }

  sonare_free_percussive_events(&from_null);
  sonare_free_percussive_events(&zeroed);
  sonare_free_percussive_events(&from_zero);
  sonare_free_percussive_events(&kept);
  sonare_free_percussive_events(&at_ceiling);
}

// --- Validation, clearing and ownership -----------------------------------

TEST_CASE("sonare_extract_percussive_events rejects malformed arguments and clears its output",
          "[c_api][percussive_events]") {
  const std::vector<float> samples = two_hits();

  // A NULL out is the one rejection with nothing to clear.
  REQUIRE(sonare_extract_percussive_events(samples.data(), samples.size(), kSampleRate, nullptr,
                                           nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  // Every other rejection must leave the caller's result cleared, which the
  // poisoned fields here would otherwise still be carrying.
  auto rejects = [](auto&& call) {
    SonarePercussiveEventsResult out{};
    out.events = poisoned_events();
    out.count = 7;
    REQUIRE(call(&out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out.events == nullptr);
    REQUIRE(out.count == 0);
  };

  SECTION("a config rejected before anything is allocated") {
    for (const int32_t bad_version : {-1, 2, 99}) {
      SonarePercussiveEventConfig config{};
      config.struct_version = bad_version;
      rejects([&](SonarePercussiveEventsResult* out) { return extract_at(samples, &config, out); });
    }

    // Negative integer fields are rejected rather than silently clamped.
    for (const int32_t bad : {-1, -2048}) {
      SonarePercussiveEventConfig n_fft{};
      n_fft.n_fft = bad;
      SonarePercussiveEventConfig hop{};
      hop.hop_length = bad;
      SonarePercussiveEventConfig harmonic{};
      harmonic.hpss_kernel_harmonic = bad;
      SonarePercussiveEventConfig percussive{};
      percussive.hpss_kernel_percussive = bad;
      SonarePercussiveEventConfig wait{};
      wait.onset_wait = bad;
      for (const SonarePercussiveEventConfig* config :
           {&n_fft, &hop, &harmonic, &percussive, &wait}) {
        rejects(
            [&](SonarePercussiveEventsResult* out) { return extract_at(samples, config, out); });
      }
    }

    for (const float bad : {kNaN, kInf, -kInf}) {
      SonarePercussiveEventConfig delta{};
      delta.onset_delta = bad;
      SonarePercussiveEventConfig max_event{};
      max_event.max_event_ms = bad;
      SonarePercussiveEventConfig ratio{};
      ratio.min_percussive_ratio = bad;
      for (const SonarePercussiveEventConfig* config : {&delta, &max_event, &ratio}) {
        rejects(
            [&](SonarePercussiveEventsResult* out) { return extract_at(samples, config, out); });
      }
    }

    SonarePercussiveEventConfig negative_span{};
    negative_span.max_event_ms = -1.0f;
    rejects([&](SonarePercussiveEventsResult* out) {
      return extract_at(samples, &negative_span, out);
    });

    for (const float outside : {-0.01f, -1.0f, 1.01f, 2.0f}) {
      SonarePercussiveEventConfig ratio{};
      ratio.min_percussive_ratio = outside;
      rejects([&](SonarePercussiveEventsResult* out) { return extract_at(samples, &ratio, out); });
    }
  }

  SECTION("a framing rejected after the argument checks, inside the separation") {
    // A framing whose parts are all non-negative reaches the separation and is
    // refused there by the overlap-add rule: n_fft even and at least 2, hop
    // positive and no more than half of it. Negative parts never get that far --
    // the argument check above has them -- and 0 is not a framing at all here but
    // the spelling of the default, so "no window" and "no advance" are the one
    // pair of core-level violations this surface cannot express.
    const int32_t kBrokenFramings[][2] = {{2047, 512},  {1023, 256}, {1, 1},
                                          {2048, 1025}, {1024, 513}, {2048, 2048}};
    for (const auto& framing : kBrokenFramings) {
      INFO("n_fft " << framing[0] << " hop " << framing[1]);
      SonarePercussiveEventConfig config{};
      config.n_fft = framing[0];
      config.hop_length = framing[1];
      rejects([&](SonarePercussiveEventsResult* out) { return extract_at(samples, &config, out); });
    }

    // The median filters take an odd kernel only, so an even one is an error
    // rather than a rounded value.
    for (const int32_t even : {2, 30, 32}) {
      INFO("kernel " << even);
      SonarePercussiveEventConfig harmonic{};
      harmonic.hpss_kernel_harmonic = even;
      SonarePercussiveEventConfig percussive{};
      percussive.hpss_kernel_percussive = even;
      for (const SonarePercussiveEventConfig* config : {&harmonic, &percussive}) {
        rejects(
            [&](SonarePercussiveEventsResult* out) { return extract_at(samples, config, out); });
      }
    }
  }

  SECTION("audio rejected after the argument checks") {
    rejects([&](SonarePercussiveEventsResult* out) {
      return sonare_extract_percussive_events(nullptr, samples.size(), kSampleRate, nullptr, out);
    });
    rejects([&](SonarePercussiveEventsResult* out) {
      return sonare_extract_percussive_events(nullptr, 0, kSampleRate, nullptr, out);
    });
    rejects([&](SonarePercussiveEventsResult* out) {
      return sonare_extract_percussive_events(samples.data(), 0, kSampleRate, nullptr, out);
    });
    for (const int bad_rate : {0, -1, -44100}) {
      rejects([&](SonarePercussiveEventsResult* out) {
        return sonare_extract_percussive_events(samples.data(), samples.size(), bad_rate, nullptr,
                                                out);
      });
    }
    for (const float bad_sample : {kNaN, kInf, -kInf}) {
      std::vector<float> poisoned = samples;
      poisoned[1000] = bad_sample;
      rejects(
          [&](SonarePercussiveEventsResult* out) { return extract_at(poisoned, nullptr, out); });
    }
  }

  // Positive control: the same call with nothing poisoned succeeds.
  SonarePercussiveEventsResult ok = extracted(samples);
  REQUIRE(ok.count == 2);
  sonare_free_percussive_events(&ok);
}

TEST_CASE("sonare_free_percussive_events is NULL-safe, clears, and repeats",
          "[c_api][percussive_events]") {
  sonare_free_percussive_events(nullptr);

  // A zeroed result owns nothing, so releasing it is a no-op rather than a free
  // of an uninitialised pointer.
  SonarePercussiveEventsResult empty{};
  sonare_free_percussive_events(&empty);
  REQUIRE(empty.events == nullptr);
  REQUIRE(empty.count == 0);

  SonarePercussiveEventsResult out = extracted(two_hits());
  REQUIRE(out.events != nullptr);
  REQUIRE(out.count == 2);

  sonare_free_percussive_events(&out);
  REQUIRE(out.events == nullptr);
  REQUIRE(out.count == 0);

  // Cleared on release, so a second call frees nothing: releasing twice is not a
  // double free.
  sonare_free_percussive_events(&out);
  REQUIRE(out.events == nullptr);
  REQUIRE(out.count == 0);
}

// --- Rendering: the round trip --------------------------------------------

TEST_CASE("sonare_render_percussive_events reproduces the input bit for bit for an identity set",
          "[c_api][percussive_events]") {
  const std::vector<float> samples = three_hits();
  SonarePercussiveEventsResult out = extracted(samples);
  REQUIRE(out.count == 3);

  // The set exactly as extraction produced it.
  REQUIRE(first_mismatch(render_ok(samples, out.events, out.count), samples) == kNoMismatch);

  // A zero count, with the events pointer NULL as the header permits and with a
  // pointer the call must then not read.
  REQUIRE(first_mismatch(render_ok(samples, nullptr, 0), samples) == kNoMismatch);
  REQUIRE(first_mismatch(render_ok(samples, out.events, 0), samples) == kNoMismatch);

  sonare_free_percussive_events(&out);
}

TEST_CASE("an extracted set travels back into the renderer with only its edit changed",
          "[c_api][percussive_events]") {
  const std::vector<float> samples = three_hits();
  SonarePercussiveEventsResult out = extracted(samples);
  REQUIRE(out.count == 3);

  std::vector<SonarePercussiveEvent> events(out.events, out.events + out.count);
  // The lifted signal carries this hit's whole peak, so the rendered peak is
  // exactly the gain times the source's: a round -6.02 dB lands on
  // require_edited's floor and +6.02 dB one rounding under its ceiling.
  events[0].edit.gain_db = 3.0f;
  events[1].edit.muted = 1;
  events[2].edit.time_offset_samples = -2000;
  const std::vector<float> rendered = render_ok(samples, events.data(), events.size());
  require_edited(rendered, samples);

  // strength, peak_amplitude and percussive_ratio are ignored on the way in, so
  // corrupting all three cannot move a sample of the output.
  std::vector<SonarePercussiveEvent> corrupted = events;
  for (SonarePercussiveEvent& event : corrupted) {
    event.strength = kNaN;
    event.peak_amplitude = -1.0e30f;
    event.percussive_ratio = kInf;
  }
  REQUIRE(first_mismatch(render_ok(samples, corrupted.data(), corrupted.size()), rendered) ==
          kNoMismatch);

  sonare_free_percussive_events(&out);
}

TEST_CASE("any non-zero muted silences a hit, not only 1", "[c_api][percussive_events]") {
  const std::vector<float> samples = three_hits();
  SonarePercussiveEventsResult out = extracted(samples);
  REQUIRE(out.count == 3);

  SonarePercussiveEvent reference = out.events[0];
  reference.edit.muted = 1;
  const std::vector<float> muted = render_ok(samples, &reference, 1);

  // The hit is gone and the buffer is still a signal: the second hit is not
  // edited, so an output that came back silent or blown up fails the band.
  require_edited(muted, samples);
  const size_t hit_start = static_cast<size_t>(kThreeHitStarts[0]);
  const size_t hit_end = hit_start + kHitSamples;
  const double before = rms(samples, hit_start, hit_end);
  REQUIRE(before > 0.0);
  REQUIRE(rms(muted, hit_start, hit_end) < 0.5 * before);
  REQUIRE(first_mismatch(muted, samples, 0, static_cast<size_t>(out.events[0].onset_sample)) ==
          kNoMismatch);

  // Every non-zero spelling mutes, and the fields a mute discards really are
  // discarded: the same event with a gain and a shift on it renders identically.
  for (const int32_t spelling : {int32_t{1}, int32_t{-1}, kLargeMuted}) {
    INFO("muted " << spelling);
    SonarePercussiveEvent event = out.events[0];
    event.edit.muted = spelling;
    event.edit.gain_db = 12.0f;
    event.edit.time_offset_samples = 5000;
    REQUIRE(first_mismatch(render_ok(samples, &event, 1), muted) == kNoMismatch);
  }

  // Not vacuous: the same gain and shift with muted at 0 render something else.
  SonarePercussiveEvent audible = out.events[0];
  audible.edit.muted = 0;
  audible.edit.gain_db = 12.0f;
  audible.edit.time_offset_samples = 5000;
  REQUIRE(first_mismatch(render_ok(samples, &audible, 1), muted) != kNoMismatch);

  sonare_free_percussive_events(&out);
}

TEST_CASE("the SonarePercussiveRenderConfig fields take their defaults at 0",
          "[c_api][percussive_events]") {
  const std::vector<float> samples = three_hits();

  // A hand-built span the length of the hit itself, so the tail fade falls on
  // the decay rather than on the silence a 500 ms span would end in.
  SonarePercussiveEvent event{};
  event.onset_sample = kThreeHitStarts[0];
  event.offset_sample = kThreeHitStarts[0] + static_cast<int64_t>(kHitSamples);
  event.edit.muted = 1;

  const std::vector<float> zeroed = render_ok(samples, &event, 1);

  SonarePercussiveRenderConfig defaulted{};
  REQUIRE(first_mismatch(render_ok(samples, &event, 1, &defaulted), zeroed) == kNoMismatch);

  SonarePercussiveRenderConfig explicit_default{};
  explicit_default.struct_version = 1;
  explicit_default.n_fft = 2048;
  explicit_default.hop_length = 512;
  explicit_default.hpss_kernel_harmonic = 31;
  explicit_default.hpss_kernel_percussive = 31;
  explicit_default.fade_ms = 5.0f;
  REQUIRE(first_mismatch(render_ok(samples, &event, 1, &explicit_default), zeroed) == kNoMismatch);

  // A longer fade lifts less of the span's tail, so what the mute leaves behind
  // over that tail is nearer the source. Measured over the last 50 ms of the
  // span, which the 50 ms fade covers whole and the 5 ms default barely touches.
  SonarePercussiveRenderConfig slow{};
  slow.fade_ms = 50.0f;
  const std::vector<float> long_fade = render_ok(samples, &event, 1, &slow);
  const size_t tail_start = static_cast<size_t>(event.offset_sample) - 1102;  // 50 ms
  const size_t tail_end = static_cast<size_t>(event.offset_sample);
  auto lifted_rms = [&](const std::vector<float>& rendered) {
    std::vector<float> difference(rendered.size(), 0.0f);
    for (size_t i = 0; i < rendered.size(); ++i) difference[i] = rendered[i] - samples[i];
    return rms(difference, tail_start, tail_end);
  };
  REQUIRE(lifted_rms(zeroed) > 0.0);
  REQUIRE(lifted_rms(long_fade) < lifted_rms(zeroed));
  require_edited(long_fade, samples);
}

TEST_CASE("sonare_render_percussive_events rejects malformed arguments",
          "[c_api][percussive_events]") {
  const std::vector<float> samples = three_hits();
  const int64_t length = static_cast<int64_t>(samples.size());

  // A non-identity edit throughout, so the event is one the renderer must act on.
  auto edited = [](int64_t onset, int64_t offset) {
    SonarePercussiveEvent event{};
    event.onset_sample = onset;
    event.offset_sample = offset;
    event.edit.gain_db = -3.0f;
    return event;
  };

  float* out = nullptr;
  size_t out_length = 0;
  const SonarePercussiveEvent event = edited(4410, 15435);

  REQUIRE(render_at(samples, &event, 1, nullptr, nullptr, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(render_at(samples, &event, 1, nullptr, &out, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  // NULL events is a zero-count spelling only.
  REQUIRE(render_at(samples, nullptr, 1, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_render_percussive_events(nullptr, samples.size(), kSampleRate, &event, 1, nullptr,
                                          &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_render_percussive_events(samples.data(), 0, kSampleRate, &event, 1, nullptr, &out,
                                          &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  for (const int bad_rate : {0, -1}) {
    REQUIRE(sonare_render_percussive_events(samples.data(), samples.size(), bad_rate, &event, 1,
                                            nullptr, &out,
                                            &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  }

  // An empty span has nothing to lift, a reversed one is not a span, and neither
  // end may sit outside the audio.
  for (const SonarePercussiveEvent& malformed :
       {edited(4410, 4410), edited(15435, 4410), edited(-512, 4410), edited(4410, length + 1),
        edited(length, length + 4410)}) {
    REQUIRE(render_at(samples, &malformed, 1, nullptr, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  // Overlapping source spans are not a renderable set; touching ones are.
  const SonarePercussiveEvent overlapping[2] = {edited(0, 22050), edited(11025, 33075)};
  REQUIRE(render_at(samples, overlapping, 2, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const SonarePercussiveEvent adjacent[2] = {edited(0, 22050), edited(22050, 33075)};
  REQUIRE(render_at(samples, adjacent, 2, nullptr, &out, &out_length) == SONARE_OK);
  sonare_free_floats(out);
  out = nullptr;

  // The same rejections with nothing for the renderer to do. The identity edit
  // here is the point rather than an omission: an unrenderable set is
  // unrenderable whether or not this call would have touched the offending
  // event, and the fast path that skips the separation must not skip the checks
  // with it.
  auto identity_span = [](int64_t onset, int64_t offset) {
    SonarePercussiveEvent event{};
    event.onset_sample = onset;
    event.offset_sample = offset;
    return event;
  };
  for (const SonarePercussiveEvent& malformed :
       {identity_span(4410, 4410), identity_span(15435, 4410), identity_span(-512, 4410),
        identity_span(4410, length + 1)}) {
    REQUIRE(render_at(samples, &malformed, 1, nullptr, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
  const SonarePercussiveEvent identity_overlap[2] = {identity_span(0, 22050),
                                                     identity_span(11025, 33075)};
  REQUIRE(render_at(samples, identity_overlap, 2, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);

  for (const float bad : {kNaN, kInf, -kInf}) {
    SonarePercussiveEvent gain = edited(4410, 15435);
    gain.edit.gain_db = bad;
    REQUIRE(render_at(samples, &gain, 1, nullptr, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  for (const int32_t bad_version : {-1, 2, 99}) {
    SonarePercussiveRenderConfig config{};
    config.struct_version = bad_version;
    REQUIRE(render_at(samples, &event, 1, &config, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  for (const int32_t bad : {-1, -2048}) {
    SonarePercussiveRenderConfig n_fft{};
    n_fft.n_fft = bad;
    SonarePercussiveRenderConfig hop{};
    hop.hop_length = bad;
    SonarePercussiveRenderConfig harmonic{};
    harmonic.hpss_kernel_harmonic = bad;
    SonarePercussiveRenderConfig percussive{};
    percussive.hpss_kernel_percussive = bad;
    for (const SonarePercussiveRenderConfig* config : {&n_fft, &hop, &harmonic, &percussive}) {
      REQUIRE(render_at(samples, &event, 1, config, &out, &out_length) ==
              SONARE_ERROR_INVALID_PARAMETER);
    }
  }

  // A zero-length fade is the default rather than a hard cut, so only a negative
  // or non-finite one is rejected here.
  for (const float bad_fade : {kNaN, kInf, -kInf, -1.0f}) {
    SonarePercussiveRenderConfig config{};
    config.fade_ms = bad_fade;
    REQUIRE(render_at(samples, &event, 1, &config, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  // Positive control: the same event under a fully spelled-out config renders.
  SonarePercussiveRenderConfig valid{};
  valid.struct_version = 1;
  valid.n_fft = 1024;
  valid.hop_length = 512;
  valid.fade_ms = 5.0f;
  REQUIRE(render_at(samples, &event, 1, &valid, &out, &out_length) == SONARE_OK);
  REQUIRE(out_length == samples.size());
  sonare_free_floats(out);
}

TEST_CASE("sonare_render_percussive_events rejects a broken framing on every set",
          "[c_api][percussive_events]") {
  // Two promises pull against each other: an all-identity set is a bit-exact
  // pass-through that runs no separation, and the framing is validated anyway.
  // An implementation that checks it where it builds the STFT returns the input
  // instead of an error, and only a set that never reaches the STFT sees that.
  const std::vector<float> samples = three_hits();

  SonarePercussiveEvent identity[2] = {};
  identity[0].onset_sample = 0;
  identity[0].offset_sample = 22050;
  identity[1].onset_sample = 22050;
  identity[1].offset_sample = 44100;

  // The same set on the default framing is the pass-through, so every rejection
  // below is the framing's doing and not the set's.
  REQUIRE(first_mismatch(render_ok(samples, identity, 2), samples) == kNoMismatch);

  // 0 is the default's spelling on this side rather than a framing, so every row
  // here carries two non-zero parts that break the overlap-add rule together.
  const int32_t kBrokenFramings[][2] = {{2047, 512},  {1023, 256}, {1, 1},
                                        {2048, 1025}, {1024, 513}, {2048, 2048}};
  float* out = nullptr;
  size_t out_length = 0;
  for (const auto& framing : kBrokenFramings) {
    INFO("n_fft " << framing[0] << " hop " << framing[1]);
    SonarePercussiveRenderConfig config{};
    config.n_fft = framing[0];
    config.hop_length = framing[1];
    REQUIRE(render_at(samples, identity, 2, &config, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    // And on the emptiest set there is.
    REQUIRE(render_at(samples, nullptr, 0, &config, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  // Inside the rule the pass-through still holds, including at exactly half the
  // window, so the rejections are not simply "any non-default framing".
  const int32_t kValidFramings[][2] = {{2048, 512}, {2048, 1024}, {1024, 512}};
  for (const auto& framing : kValidFramings) {
    INFO("n_fft " << framing[0] << " hop " << framing[1]);
    SonarePercussiveRenderConfig config{};
    config.n_fft = framing[0];
    config.hop_length = framing[1];
    REQUIRE(first_mismatch(render_ok(samples, identity, 2, &config), samples) == kNoMismatch);
    REQUIRE(first_mismatch(render_ok(samples, nullptr, 0, &config), samples) == kNoMismatch);
  }
}

// --- Parameter combinations -----------------------------------------------

TEST_CASE("sonare_extract_percussive_events holds its contract across its config space",
          "[c_api][percussive_events]") {
  // Eight config axes, covered pairwise: every pair of levels from any two axes
  // appears in some row. Picking plausible-looking combinations by eye leaves
  // whole pairs untried, and what interacts here is the framing, the two median
  // kernels, the two detector rules and the two span rules.
  struct Row {
    int32_t struct_version;
    int32_t n_fft;
    int32_t hop_length;
    int32_t kernel_harmonic;
    int32_t kernel_percussive;
    int32_t onset_wait;
    float onset_delta;
    float max_event_ms;
    float min_percussive_ratio;
  };
  static const Row kRows[] = {
      {0, 0, 0, 31, 0, 0, 0.12f, 0.0f, 0.25f},        {0, 2048, 512, 0, 15, 3, 0.06f, 500.0f, 1.0f},
      {0, 1024, 256, 15, 15, 1, 0.0f, 40.0f, 0.0f},   {0, 2048, 1024, 15, 0, 0, 0.06f, 40.0f, 1.0f},
      {1, 0, 0, 0, 0, 1, 0.0f, 500.0f, 0.25f},        {1, 2048, 512, 31, 15, 3, 0.12f, 40.0f, 0.0f},
      {1, 1024, 256, 0, 0, 0, 0.06f, 0.0f, 0.0f},     {1, 2048, 1024, 31, 15, 1, 0.12f, 0.0f, 1.0f},
      {1, 2048, 1024, 15, 0, 3, 0.0f, 0.0f, 0.25f},   {1, 0, 0, 15, 15, 0, 0.12f, 500.0f, 0.0f},
      {0, 0, 0, 0, 15, 3, 0.06f, 40.0f, 0.25f},       {1, 0, 0, 31, 0, 0, 0.0f, 500.0f, 1.0f},
      {1, 2048, 512, 15, 0, 0, 0.0f, 0.0f, 0.25f},    {0, 2048, 512, 31, 0, 1, 0.06f, 500.0f, 1.0f},
      {0, 1024, 256, 31, 15, 3, 0.12f, 500.0f, 1.0f}, {1, 1024, 256, 0, 15, 0, 0.12f, 0.0f, 0.25f},
      {1, 2048, 1024, 0, 15, 1, 0.06f, 500.0f, 0.0f},
  };

  const std::vector<float> samples = two_hits();
  for (const Row& row : kRows) {
    INFO("version " << row.struct_version << " n_fft " << row.n_fft << " hop " << row.hop_length
                    << " kernels " << row.kernel_harmonic << "/" << row.kernel_percussive
                    << " wait " << row.onset_wait << " delta " << row.onset_delta
                    << " max_event_ms " << row.max_event_ms << " min_ratio "
                    << row.min_percussive_ratio);
    SonarePercussiveEventConfig config{};
    config.struct_version = row.struct_version;
    config.n_fft = row.n_fft;
    config.hop_length = row.hop_length;
    config.hpss_kernel_harmonic = row.kernel_harmonic;
    config.hpss_kernel_percussive = row.kernel_percussive;
    config.onset_wait = row.onset_wait;
    config.onset_delta = row.onset_delta;
    config.max_event_ms = row.max_event_ms;
    config.min_percussive_ratio = row.min_percussive_ratio;

    SonarePercussiveEventsResult out = extracted(samples, &config);
    require_well_formed(out, samples.size());
    for (size_t i = 0; i < out.count; ++i) {
      REQUIRE(out.events[i].percussive_ratio >= row.min_percussive_ratio);
    }
    // Two struck sounds in silence: a row that keeps everything and leaves the
    // detector at its default has to find them, or its invariants are vacuous.
    // At the coarsest framing the two sit only eight hops apart, which is the
    // detector's business rather than this call's, so there the guard is only
    // that the row found something.
    if (row.min_percussive_ratio == 0.0f && row.onset_delta <= 0.06f && row.onset_wait <= 1) {
      REQUIRE(out.count >= 1);
      const int32_t hop = row.hop_length == 0 ? 512 : row.hop_length;
      if (hop <= 512) REQUIRE(out.count == 2);
    }

    // Whatever the config, the set it produced is renderable over its own audio
    // and is a pass-through.
    SonarePercussiveRenderConfig render_config{};
    render_config.struct_version = row.struct_version;
    render_config.n_fft = row.n_fft;
    render_config.hop_length = row.hop_length;
    render_config.hpss_kernel_harmonic = row.kernel_harmonic;
    render_config.hpss_kernel_percussive = row.kernel_percussive;
    REQUIRE(first_mismatch(render_ok(samples, out.events, out.count, &render_config), samples) ==
            kNoMismatch);
    sonare_free_percussive_events(&out);
  }
}

TEST_CASE("sonare_render_percussive_events holds its contract across its edit and config space",
          "[c_api][percussive_events]") {
  // Six axes, covered pairwise. What is worth reaching is the interaction of a
  // shift that runs off an end, a mute spelled as something other than 1 (which
  // then discards the gain), and a fade long enough to cover a good part of the
  // span.
  enum class Shift { kNone, kSpanForward, kOffLeft, kOffRight };
  struct Row {
    int32_t struct_version;
    int32_t n_fft;
    int32_t hop_length;
    float fade_ms;
    int32_t muted;
    float gain_db;
    Shift shift;
  };
  static const Row kRows[] = {
      {0, 0, 0, 5.0f, 0, 0.0f, Shift::kNone},
      {0, 1024, 256, 50.0f, -1, 6.0206f, Shift::kOffLeft},
      {0, 2048, 1024, 0.0f, kLargeMuted, -6.0206f, Shift::kSpanForward},
      {0, 2048, 1024, 50.0f, 1, 0.0f, Shift::kOffRight},
      {1, 0, 0, 0.0f, 1, 6.0206f, Shift::kSpanForward},
      {1, 1024, 256, 5.0f, -1, -6.0206f, Shift::kOffRight},
      {1, 2048, 1024, 0.0f, 0, 0.0f, Shift::kOffLeft},
      {1, 0, 0, 50.0f, kLargeMuted, -6.0206f, Shift::kNone},
      {1, 0, 0, 0.0f, -1, 6.0206f, Shift::kNone},
      {0, 0, 0, 5.0f, 1, -6.0206f, Shift::kOffLeft},
      {0, 0, 0, 5.0f, kLargeMuted, 6.0206f, Shift::kOffRight},
      {1, 1024, 256, 0.0f, -1, 0.0f, Shift::kSpanForward},
      {0, 1024, 256, 50.0f, 0, -6.0206f, Shift::kSpanForward},
      {1, 1024, 256, 50.0f, 1, -6.0206f, Shift::kNone},
      {0, 1024, 256, 5.0f, kLargeMuted, 0.0f, Shift::kSpanForward},
      {1, 2048, 1024, 5.0f, -1, 6.0206f, Shift::kNone},
      {1, 2048, 1024, 0.0f, 0, 6.0206f, Shift::kOffRight},
      {0, 0, 0, 0.0f, kLargeMuted, -6.0206f, Shift::kOffLeft},
  };

  const std::vector<float> samples = two_hits();
  SonarePercussiveEventsResult out = extracted(samples);
  REQUIRE(out.count == 2);
  const int64_t span = out.events[0].offset_sample - out.events[0].onset_sample;

  for (const Row& row : kRows) {
    INFO("version " << row.struct_version << " n_fft " << row.n_fft << " hop " << row.hop_length
                    << " fade_ms " << row.fade_ms << " muted " << row.muted << " gain_db "
                    << row.gain_db << " shift " << static_cast<int>(row.shift));
    int64_t offset = 0;
    switch (row.shift) {
      case Shift::kNone:
        offset = 0;
        break;
      case Shift::kSpanForward:
        offset = span;
        break;
      case Shift::kOffLeft:
        offset = -100000;
        break;
      case Shift::kOffRight:
        offset = 100000;
        break;
    }

    // Only the first event is edited, so the second hit is always there to bound
    // the output from below.
    SonarePercussiveEvent edited = out.events[0];
    edited.edit.muted = row.muted;
    edited.edit.gain_db = row.gain_db;
    edited.edit.time_offset_samples = offset;

    SonarePercussiveRenderConfig config{};
    config.struct_version = row.struct_version;
    config.n_fft = row.n_fft;
    config.hop_length = row.hop_length;
    config.fade_ms = row.fade_ms;

    const std::vector<float> rendered = render_ok(samples, &edited, 1, &config);
    for (size_t i = 0; i < rendered.size(); ++i) {
      if (!std::isfinite(rendered[i])) FAIL("non-finite sample at " << i);
    }
    REQUIRE(peak(rendered, 0, rendered.size()) > 0.1f);
    REQUIRE(peak(rendered, 0, rendered.size()) < 2.0f);

    if (row.muted == 0 && row.gain_db == 0.0f && offset == 0) {
      REQUIRE(first_mismatch(rendered, samples) == kNoMismatch);
    }
  }

  sonare_free_percussive_events(&out);
}
