/// @file sonare_c_note_objects_test.cpp
/// @brief Tests for the note-object C API: sonare_extract_notes,
///        sonare_render_notes and sonare_free_note_objects.
///
/// Tracks are hand-built at 16 kHz with 10 ms frames rather than measured with
/// pYIN, so every expected span, median and metric is predictable.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "util/constants.h"

#ifdef SONARE_WITH_PITCH_EDITOR

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr int kSampleRate = 16000;
constexpr float kFrameRate = 100.0f;  // 10 ms frames
constexpr int64_t kSamplesPerFrame = 160;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr size_t kNoMismatch = static_cast<size_t>(-1);

std::vector<float> sine(float frequency_hz, float amplitude, int n_samples) {
  std::vector<float> out(static_cast<size_t>(n_samples), 0.0f);
  for (int i = 0; i < n_samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz *
                                                static_cast<double>(i) / kSampleRate));
  }
  return out;
}

float hz_at_cents(float cents) {
  return sonare::constants::kA4Hz * std::pow(2.0f, cents / sonare::constants::kCentsPerOctave);
}

/// Index of the first differing sample in [lo, hi), or kNoMismatch.
size_t first_mismatch(const float* a, const float* b, size_t lo, size_t hi) {
  for (size_t i = lo; i < hi; ++i) {
    if (a[i] != b[i]) return i;
  }
  return kNoMismatch;
}

double rms(const float* data, size_t lo, size_t hi) {
  double sum = 0.0;
  for (size_t i = lo; i < hi; ++i) {
    sum += static_cast<double>(data[i]) * static_cast<double>(data[i]);
  }
  return std::sqrt(sum / static_cast<double>(hi - lo));
}

float peak(const float* data, size_t lo, size_t hi) {
  float highest = 0.0f;
  for (size_t i = lo; i < hi; ++i) {
    highest = std::max(highest, std::abs(data[i]));
  }
  return highest;
}

SonareNoteObject* poisoned_notes() {
  return reinterpret_cast<SonareNoteObject*>(static_cast<std::uintptr_t>(0x1));
}

float* poisoned_floats() { return reinterpret_cast<float*>(static_cast<std::uintptr_t>(0x1)); }

SonareError extract_at(const std::vector<float>& samples, const std::vector<float>& f0_hz,
                       const float* voiced_prob, const int32_t* voiced,
                       const SonareNoteExtractorConfig* config, SonareNoteObjectsResult* out) {
  return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, f0_hz.data(),
                              voiced_prob, voiced, f0_hz.size(), kFrameRate, config, out);
}

/// A render carrying neither an envelope pool nor an F0 track.
SonareError render_at(const std::vector<float>& samples, const SonareNoteObject* notes,
                      size_t note_count, const SonareNoteRenderConfig* config, float** out,
                      size_t* out_length) {
  return sonare_render_notes(samples.data(), samples.size(), kSampleRate, notes, note_count,
                             nullptr, 0, nullptr, 0, kFrameRate, config, out, out_length);
}

/// The same with the envelope pool and the F0 track the edit fields index into.
SonareError render_with(const std::vector<float>& samples, const SonareNoteObject* notes,
                        size_t note_count, const float* envelopes, size_t envelope_count,
                        const float* f0_hz, size_t n_frames, const SonareNoteRenderConfig* config,
                        float** out, size_t* out_length) {
  return sonare_render_notes(samples.data(), samples.size(), kSampleRate, notes, note_count,
                             envelopes, envelope_count, f0_hz, n_frames, kFrameRate, config, out,
                             out_length);
}

/// A renderable note carrying nothing but its span, its centre and a zeroed
/// edit. The centre is what a curve edit measures the note's cents against.
SonareNoteObject hand_note(int64_t onset_sample, int64_t offset_sample,
                           float median_hz = sonare::constants::kA4Hz) {
  SonareNoteObject note{};
  note.onset_sample = onset_sample;
  note.offset_sample = offset_sample;
  note.frame_start = static_cast<int32_t>(onset_sample / kSamplesPerFrame);
  note.frame_end = static_cast<int32_t>(offset_sample / kSamplesPerFrame);
  note.median_hz = median_hz;
  return note;
}

/// Largest absolute difference over @p count samples.
float max_difference(const float* a, const float* b, size_t count) {
  float worst = 0.0f;
  for (size_t i = 0; i < count; ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
  return worst;
}

/// Asserts that an edit meant to change the sound did: the output moved away
/// from @p source, and it is still a signal. The amplitude band is what a bare
/// difference check misses -- silence differs from the source by its own peak,
/// so it passes one, and a blow-up passes it too.
void require_edited(const float* out, size_t count, const float* source) {
  REQUIRE(max_difference(source, out, count) > 0.05f);
  const float source_peak = peak(source, 0, count);
  const float out_peak = peak(out, 0, count);
  REQUIRE(out_peak > 0.5f * source_peak);
  REQUIRE(out_peak < 2.0f * source_peak);
}

/// A sine whose pitch swings @p depth_cents either side of @p centre_hz at
/// @p rate_hz, so a note taken from it carries a vibrato a curve edit can act on.
std::vector<float> fm_tone(double centre_hz, double depth_cents, double rate_hz, double amplitude,
                           int n_samples) {
  std::vector<float> out(static_cast<size_t>(n_samples), 0.0f);
  double phase = 0.0;
  for (int i = 0; i < n_samples; ++i) {
    out[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(phase));
    const double t = static_cast<double>(i) / kSampleRate;
    const double cents = depth_cents * std::sin(sonare::constants::kTwoPiD * rate_hz * t);
    const double hz = centre_hz * std::pow(2.0, cents / sonare::constants::kCentsPerOctave);
    phase += sonare::constants::kTwoPiD * hz / kSampleRate;
  }
  return out;
}

/// The F0 track describing @ref fm_tone frame by frame.
std::vector<float> fm_track(double centre_hz, double depth_cents, double rate_hz, size_t frames) {
  std::vector<float> f0(frames, 0.0f);
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kFrameRate;
    const double cents = depth_cents * std::sin(sonare::constants::kTwoPiD * rate_hz * t);
    f0[i] =
        static_cast<float>(centre_hz * std::pow(2.0, cents / sonare::constants::kCentsPerOctave));
  }
  return f0;
}

/// Harmonics of @p f0_hz under a fixed resonance. A bare sine carries no
/// spectral envelope, so a formant warp needs a source that has one.
std::vector<float> vowel_tone(float f0_hz, float formant_hz, float amplitude, int n_samples) {
  constexpr float kBandwidthHz = 500.0f;
  const float nyquist = 0.5f * static_cast<float>(kSampleRate);
  std::vector<float> out(static_cast<size_t>(n_samples), 0.0f);
  for (int h = 1; static_cast<float>(h) * f0_hz < nyquist; ++h) {
    const float harmonic_hz = static_cast<float>(h) * f0_hz;
    const float weight = 1.0f / (1.0f + std::pow((harmonic_hz - formant_hz) / kBandwidthHz, 2.0f));
    const std::vector<float> partial = sine(harmonic_hz, amplitude * weight, n_samples);
    for (int i = 0; i < n_samples; ++i) {
      out[static_cast<size_t>(i)] += partial[static_cast<size_t>(i)];
    }
  }
  return out;
}

/// Exact field-by-field equality. The compared calls run identical arithmetic,
/// so a tolerance here would hide a config field that was silently ignored.
bool same_result(const SonareNoteObjectsResult& a, const SonareNoteObjectsResult& b) {
  if (a.count != b.count || a.amplitude_count != b.amplitude_count) return false;
  for (size_t i = 0; i < a.count; ++i) {
    const SonareNoteObject& lhs = a.notes[i];
    const SonareNoteObject& rhs = b.notes[i];
    if (lhs.onset_sample != rhs.onset_sample || lhs.offset_sample != rhs.offset_sample ||
        lhs.amplitude_offset != rhs.amplitude_offset || lhs.frame_start != rhs.frame_start ||
        lhs.frame_end != rhs.frame_end || lhs.median_hz != rhs.median_hz ||
        lhs.median_cents != rhs.median_cents || lhs.f0_stability != rhs.f0_stability) {
      return false;
    }
  }
  for (size_t i = 0; i < a.amplitude_count; ++i) {
    if (a.amplitude[i] != b.amplitude[i]) return false;
  }
  return true;
}

}  // namespace

TEST_CASE("sonare_extract_notes returns one editable note for a sustained track",
          "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.5f, 6400);
  const std::vector<float> f0(40, 440.0f);
  const std::vector<int32_t> voiced(40, 1);

  SonareNoteObjectsResult out{};
  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &out) == SONARE_OK);
  REQUIRE(out.count == 1);
  REQUIRE(out.notes != nullptr);
  REQUIRE(out.notes[0].onset_sample == 0);
  REQUIRE(out.notes[0].offset_sample == 6400);
  REQUIRE(out.notes[0].frame_start == 0);
  REQUIRE(out.notes[0].frame_end == 40);
  REQUIRE_THAT(out.notes[0].median_hz, WithinAbs(440.0f, 0.5f));
  REQUIRE_THAT(out.notes[0].median_cents, WithinAbs(0.0f, 2.0f));

  // Every returned note carries the identity edit, in its 1.0 spelling.
  REQUIRE(out.notes[0].edit.time_offset_samples == 0);
  REQUIRE(out.notes[0].edit.pitch_shift_semitones == 0.0f);
  REQUIRE(out.notes[0].edit.gain_db == 0.0f);
  REQUIRE(out.notes[0].edit.time_stretch_ratio == 1.0f);
  REQUIRE(out.notes[0].edit.muted == 0);

  REQUIRE(out.amplitude != nullptr);
  REQUIRE(out.amplitude_count == 40);
  REQUIRE(out.notes[0].amplitude_offset == 0);
  sonare_free_note_objects(&out);
}

TEST_CASE("sonare_extract_notes places frame bounds and spans on the caller's own track",
          "[c_api][note_objects]") {
  // 440 Hz for 20 frames then a fifth up, which is far past the 50-cent
  // segmentation threshold, so the split lands on frame 20 / sample 3200.
  std::vector<float> samples = sine(440.0f, 0.5f, 3200);
  const std::vector<float> upper = sine(587.33f, 0.25f, 3200);
  samples.insert(samples.end(), upper.begin(), upper.end());

  std::vector<float> f0(40, 440.0f);
  for (size_t i = 20; i < 40; ++i) f0[i] = 587.33f;
  const std::vector<int32_t> voiced(40, 1);

  SonareNoteObjectsResult out{};
  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &out) == SONARE_OK);
  REQUIRE(out.count == 2);
  REQUIRE(out.notes[0].frame_start == 0);
  REQUIRE(out.notes[0].frame_end == 20);
  REQUIRE(out.notes[1].frame_start == 20);
  REQUIRE(out.notes[1].frame_end == 40);
  REQUIRE(out.notes[0].onset_sample == 0);
  REQUIRE(out.notes[0].offset_sample == 3200);
  REQUIRE(out.notes[1].onset_sample == 3200);
  REQUIRE(out.notes[1].offset_sample == 6400);

  // The frame bounds index the caller's array, so each note's F0 is the slice
  // they cut out of it.
  for (size_t i = 0; i < out.count; ++i) {
    const SonareNoteObject& note = out.notes[i];
    REQUIRE(note.onset_sample == note.frame_start * kSamplesPerFrame);
    REQUIRE(note.offset_sample == note.frame_end * kSamplesPerFrame);
    for (int32_t frame = note.frame_start; frame < note.frame_end; ++frame) {
      REQUIRE(f0[static_cast<size_t>(frame)] == (i == 0 ? 440.0f : 587.33f));
    }
  }
  REQUIRE_THAT(out.notes[1].median_hz, WithinAbs(587.33f, 1.0f));
  sonare_free_note_objects(&out);
}

TEST_CASE("sonare_extract_notes returns one source RMS per frame of every span",
          "[c_api][note_objects]") {
  std::vector<float> samples = sine(440.0f, 0.5f, 3200);
  const std::vector<float> upper = sine(587.33f, 0.25f, 3200);
  samples.insert(samples.end(), upper.begin(), upper.end());

  std::vector<float> f0(40, 440.0f);
  for (size_t i = 20; i < 40; ++i) f0[i] = 587.33f;
  const std::vector<int32_t> voiced(40, 1);

  SonareNoteObjectsResult out{};
  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &out) == SONARE_OK);
  REQUIRE(out.count == 2);

  size_t span_total = 0;
  for (size_t i = 0; i < out.count; ++i) {
    const SonareNoteObject& note = out.notes[i];
    // Each note's slice starts where the spans before it ended.
    REQUIRE(note.amplitude_offset == static_cast<int64_t>(span_total));
    const size_t span = static_cast<size_t>(note.frame_end - note.frame_start);
    for (size_t k = 0; k < span; ++k) {
      // The frame's own samples, measured here rather than read back from the
      // curve under test.
      const size_t begin = static_cast<size_t>(
          (static_cast<int64_t>(note.frame_start) + static_cast<int64_t>(k)) * kSamplesPerFrame);
      const size_t end = std::min(begin + static_cast<size_t>(kSamplesPerFrame), samples.size());
      const float expected = static_cast<float>(rms(samples.data(), begin, end));
      REQUIRE_THAT(out.amplitude[span_total + k], WithinAbs(expected, 1e-6f));
    }
    span_total += span;
  }
  REQUIRE(out.amplitude_count == span_total);
  REQUIRE(out.amplitude_count == 40);

  // The second note is half the amplitude of the first, so the curve is a level
  // rather than a normalized shape.
  REQUIRE_THAT(out.amplitude[0] / out.amplitude[20], WithinAbs(2.0f, 0.1f));
  sonare_free_note_objects(&out);
}

TEST_CASE("sonare_extract_notes scores pitch steadiness", "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.5f, 6560);
  const std::vector<int32_t> voiced(41, 1);

  SECTION("a constant F0 sits at the top of the scale") {
    const std::vector<float> f0(41, 440.0f);
    SonareNoteObjectsResult out{};
    REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &out) == SONARE_OK);
    REQUIRE(out.count == 1);
    // No deviation to measure, so the metric is at its ceiling.
    REQUIRE(out.notes[0].f0_stability == 1.0f);
    sonare_free_note_objects(&out);
  }

  SECTION("a +-10 cent spread scores 1 - MAD / threshold") {
    // 20 frames at -10 cents, one at the centre, 20 at +10: the median is the
    // centre and the median absolute deviation is 10 cents, so the default
    // 50-cent threshold puts stability at 0.8. The spread stays well inside
    // that threshold, so the span remains one note.
    std::vector<float> f0(41, sonare::constants::kA4Hz);
    for (size_t i = 0; i < 20; ++i) f0[i] = hz_at_cents(-10.0f);
    for (size_t i = 21; i < 41; ++i) f0[i] = hz_at_cents(10.0f);

    SonareNoteObjectsResult out{};
    REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &out) == SONARE_OK);
    REQUIRE(out.count == 1);
    REQUIRE(out.notes[0].frame_start == 0);
    REQUIRE(out.notes[0].frame_end == 41);
    REQUIRE_THAT(out.notes[0].f0_stability, WithinAbs(0.8f, 0.005f));
    sonare_free_note_objects(&out);
  }

  SECTION("a vibrato scores below a pitch held flat") {
    // 5 Hz, +-30 cents: the median absolute deviation of the sine sampled over
    // these 41 frames is 17.6 cents against the 50-cent threshold. The excursion
    // stays inside the threshold, so this is still one note.
    std::vector<float> f0(41, sonare::constants::kA4Hz);
    for (size_t i = 0; i < f0.size(); ++i) {
      const double phase = sonare::constants::kTwoPiD * 5.0 * static_cast<double>(i) / kFrameRate;
      f0[i] = hz_at_cents(30.0f * static_cast<float>(std::sin(phase)));
    }

    SonareNoteObjectsResult out{};
    REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &out) == SONARE_OK);
    REQUIRE(out.count == 1);
    REQUIRE(out.notes[0].frame_end == 41);
    REQUIRE_THAT(out.notes[0].f0_stability, WithinAbs(0.647f, 0.02f));
    sonare_free_note_objects(&out);
  }

  SECTION("an unvoiced frame ends the span rather than being taken into it") {
    std::vector<float> f0(41, sonare::constants::kA4Hz);
    std::vector<int32_t> gapped(41, 1);
    for (size_t i = 15; i < 20; ++i) {
      f0[i] = 0.0f;
      gapped[i] = 0;
    }
    SonareNoteObjectsResult out{};
    REQUIRE(extract_at(samples, f0, nullptr, gapped.data(), nullptr, &out) == SONARE_OK);
    REQUIRE(out.count == 2);
    REQUIRE(out.notes[0].frame_start == 0);
    REQUIRE(out.notes[0].frame_end == 15);
    REQUIRE(out.notes[1].frame_start == 20);
    REQUIRE(out.notes[1].frame_end == 41);
    sonare_free_note_objects(&out);
  }
}

TEST_CASE("sonare_extract_notes returns an empty segmentation as NULL and zero counts",
          "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.5f, 6400);
  const std::vector<float> f0(40, 0.0f);
  const std::vector<int32_t> voiced(40, 0);

  SonareNoteObjectsResult out{};
  out.notes = poisoned_notes();
  out.count = 7;
  out.amplitude = poisoned_floats();
  out.amplitude_count = 7;

  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &out) == SONARE_OK);
  REQUIRE(out.notes == nullptr);
  REQUIRE(out.count == 0);
  REQUIRE(out.amplitude == nullptr);
  REQUIRE(out.amplitude_count == 0);
}

TEST_CASE("sonare_extract_notes reads voiced_prob only when voiced is NULL",
          "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.5f, 6400);
  const std::vector<float> f0(40, 440.0f);

  SECTION("the flags win over the probabilities") {
    const std::vector<float> silent_prob(40, 0.0f);
    const std::vector<int32_t> voiced(40, 1);
    SonareNoteObjectsResult out{};
    REQUIRE(extract_at(samples, f0, silent_prob.data(), voiced.data(), nullptr, &out) == SONARE_OK);
    REQUIRE(out.count == 1);
    REQUIRE(out.notes[0].frame_end == 40);
    sonare_free_note_objects(&out);

    const std::vector<float> confident_prob(40, 1.0f);
    const std::vector<int32_t> unvoiced(40, 0);
    REQUIRE(extract_at(samples, f0, confident_prob.data(), unvoiced.data(), nullptr, &out) ==
            SONARE_OK);
    REQUIRE(out.count == 0);
  }

  SECTION("the probabilities carry the decision when there are no flags") {
    std::vector<float> prob(40, 0.9f);
    SonareNoteObjectsResult out{};
    REQUIRE(extract_at(samples, f0, prob.data(), nullptr, nullptr, &out) == SONARE_OK);
    REQUIRE(out.count == 1);
    REQUIRE(out.notes[0].frame_start == 0);
    REQUIRE(out.notes[0].frame_end == 40);
    sonare_free_note_objects(&out);

    // Below the default 0.5 threshold there is nothing to segment.
    prob.assign(40, 0.2f);
    REQUIRE(extract_at(samples, f0, prob.data(), nullptr, nullptr, &out) == SONARE_OK);
    REQUIRE(out.count == 0);

    // Probabilities straddling the threshold: the low half drops out.
    prob.assign(40, 0.9f);
    for (size_t i = 0; i < 20; ++i) prob[i] = 0.1f;
    REQUIRE(extract_at(samples, f0, prob.data(), nullptr, nullptr, &out) == SONARE_OK);
    REQUIRE(out.count == 1);
    REQUIRE(out.notes[0].frame_start == 20);
    REQUIRE(out.notes[0].frame_end == 40);
    sonare_free_note_objects(&out);
  }
}

TEST_CASE("a NULL SonareNoteExtractorConfig and a zeroed one select the same defaults",
          "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.5f, 6400);
  const std::vector<float> f0(40, 440.0f);
  const std::vector<int32_t> voiced(40, 1);

  SonareNoteObjectsResult from_null{};
  SonareNoteObjectsResult from_zeroed{};
  SonareNoteExtractorConfig zeroed{};
  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &from_null) == SONARE_OK);
  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), &zeroed, &from_zeroed) == SONARE_OK);
  REQUIRE(from_null.count == 1);
  REQUIRE(same_result(from_null, from_zeroed));

  // struct_version 0 and 1 both select the version-1 layout.
  SonareNoteObjectsResult from_v1{};
  SonareNoteExtractorConfig version_one{};
  version_one.struct_version = 1;
  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), &version_one, &from_v1) == SONARE_OK);
  REQUIRE(same_result(from_null, from_v1));

  sonare_free_note_objects(&from_null);
  sonare_free_note_objects(&from_zeroed);
  sonare_free_note_objects(&from_v1);
}

TEST_CASE("every SonareNoteExtractorConfig field takes its default at 0", "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.5f, 6400);
  const std::vector<int32_t> voiced(40, 1);

  SECTION("segmentation_threshold_cents defaults to 50") {
    // A 100-cent step: a split under the default, one note under a wider
    // threshold.
    std::vector<float> f0(40, sonare::constants::kA4Hz);
    for (size_t i = 20; i < 40; ++i) f0[i] = hz_at_cents(100.0f);

    SonareNoteObjectsResult zeroed{};
    SonareNoteExtractorConfig config{};
    REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), &config, &zeroed) == SONARE_OK);
    REQUIRE(zeroed.count == 2);
    REQUIRE(zeroed.notes[1].frame_start == 20);

    SonareNoteObjectsResult explicit_default{};
    config.segmentation_threshold_cents = 50.0f;
    REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), &config, &explicit_default) ==
            SONARE_OK);
    REQUIRE(same_result(zeroed, explicit_default));

    SonareNoteObjectsResult widened{};
    config.segmentation_threshold_cents = 300.0f;
    REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), &config, &widened) == SONARE_OK);
    REQUIRE(widened.count == 1);
    REQUIRE(widened.notes[0].frame_end == 40);

    sonare_free_note_objects(&zeroed);
    sonare_free_note_objects(&explicit_default);
    sonare_free_note_objects(&widened);
  }

  SECTION("min_note_ms defaults to 30") {
    // A four-frame burst: 40 ms survives the default and is dropped at 100 ms.
    std::vector<float> f0(40, 0.0f);
    std::vector<int32_t> gated(40, 0);
    for (size_t i = 10; i < 14; ++i) {
      f0[i] = sonare::constants::kA4Hz;
      gated[i] = 1;
    }

    SonareNoteObjectsResult zeroed{};
    SonareNoteExtractorConfig config{};
    REQUIRE(extract_at(samples, f0, nullptr, gated.data(), &config, &zeroed) == SONARE_OK);
    REQUIRE(zeroed.count == 1);
    REQUIRE(zeroed.notes[0].frame_start == 10);
    REQUIRE(zeroed.notes[0].frame_end == 14);

    SonareNoteObjectsResult explicit_default{};
    config.min_note_ms = 30.0f;
    REQUIRE(extract_at(samples, f0, nullptr, gated.data(), &config, &explicit_default) ==
            SONARE_OK);
    REQUIRE(same_result(zeroed, explicit_default));

    SonareNoteObjectsResult strict{};
    config.min_note_ms = 100.0f;
    REQUIRE(extract_at(samples, f0, nullptr, gated.data(), &config, &strict) == SONARE_OK);
    REQUIRE(strict.count == 0);

    sonare_free_note_objects(&zeroed);
    sonare_free_note_objects(&explicit_default);
  }

  SECTION("reference_hz defaults to A4") {
    const std::vector<float> f0(40, sonare::constants::kA4Hz);

    SonareNoteObjectsResult zeroed{};
    SonareNoteExtractorConfig config{};
    REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), &config, &zeroed) == SONARE_OK);
    REQUIRE(zeroed.count == 1);
    REQUIRE_THAT(zeroed.notes[0].median_cents, WithinAbs(0.0f, 1.0f));

    SonareNoteObjectsResult explicit_default{};
    config.reference_hz = sonare::constants::kA4Hz;
    REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), &config, &explicit_default) ==
            SONARE_OK);
    REQUIRE(same_result(zeroed, explicit_default));

    // An octave-down reference moves the cents, not the measured pitch.
    SonareNoteObjectsResult octave_down{};
    config.reference_hz = 0.5f * sonare::constants::kA4Hz;
    REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), &config, &octave_down) == SONARE_OK);
    REQUIRE(octave_down.count == 1);
    REQUIRE_THAT(octave_down.notes[0].median_cents, WithinAbs(1200.0f, 1.0f));
    REQUIRE_THAT(octave_down.notes[0].median_hz, WithinAbs(sonare::constants::kA4Hz, 0.5f));

    sonare_free_note_objects(&zeroed);
    sonare_free_note_objects(&explicit_default);
    sonare_free_note_objects(&octave_down);
  }

  SECTION("voiced_threshold defaults to 0.5") {
    const std::vector<float> f0(40, sonare::constants::kA4Hz);
    const std::vector<float> prob(40, 0.6f);

    SonareNoteObjectsResult zeroed{};
    SonareNoteExtractorConfig config{};
    REQUIRE(extract_at(samples, f0, prob.data(), nullptr, &config, &zeroed) == SONARE_OK);
    REQUIRE(zeroed.count == 1);

    SonareNoteObjectsResult explicit_default{};
    config.voiced_threshold = 0.5f;
    REQUIRE(extract_at(samples, f0, prob.data(), nullptr, &config, &explicit_default) == SONARE_OK);
    REQUIRE(same_result(zeroed, explicit_default));

    SonareNoteObjectsResult strict{};
    config.voiced_threshold = 0.7f;
    REQUIRE(extract_at(samples, f0, prob.data(), nullptr, &config, &strict) == SONARE_OK);
    REQUIRE(strict.count == 0);

    sonare_free_note_objects(&zeroed);
    sonare_free_note_objects(&explicit_default);
  }
}

TEST_CASE("sonare_render_notes reproduces the input bit for bit for identity edits",
          "[c_api][note_objects]") {
  std::vector<float> samples = sine(440.0f, 0.5f, 3200);
  const std::vector<float> upper = sine(587.33f, 0.25f, 3200);
  samples.insert(samples.end(), upper.begin(), upper.end());

  std::vector<float> f0(40, 440.0f);
  for (size_t i = 20; i < 40; ++i) f0[i] = 587.33f;
  const std::vector<int32_t> voiced(40, 1);

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &extracted) == SONARE_OK);
  REQUIRE(extracted.count == 2);

  float* out = nullptr;
  size_t out_length = 0;

  SECTION("the notes as extracted round trip") {
    REQUIRE(render_at(samples, extracted.notes, extracted.count, nullptr, &out, &out_length) ==
            SONARE_OK);
    REQUIRE(out_length == samples.size());
    REQUIRE(first_mismatch(samples.data(), out, 0, out_length) == kNoMismatch);
    sonare_free_floats(out);
  }

  SECTION("a zeroed edit is the identity, so time_stretch_ratio 0 reads as 1") {
    for (size_t i = 0; i < extracted.count; ++i) {
      extracted.notes[i].edit = SonareNoteEdit{};
      REQUIRE(extracted.notes[i].edit.time_stretch_ratio == 0.0f);
    }
    REQUIRE(render_at(samples, extracted.notes, extracted.count, nullptr, &out, &out_length) ==
            SONARE_OK);
    REQUIRE(out_length == samples.size());
    REQUIRE(first_mismatch(samples.data(), out, 0, out_length) == kNoMismatch);
    sonare_free_floats(out);

    // Non-vacuity: the same call with one field off its neutral value does
    // change the output, so the equality above is the identity property rather
    // than a pass-through of everything.
    extracted.notes[0].edit.gain_db = -6.0f;
    REQUIRE(render_at(samples, extracted.notes, extracted.count, nullptr, &out, &out_length) ==
            SONARE_OK);
    REQUIRE(first_mismatch(samples.data(), out, 0, out_length) != kNoMismatch);
    sonare_free_floats(out);
  }

  SECTION("an empty note set is a pass-through") {
    REQUIRE(render_at(samples, nullptr, 0, nullptr, &out, &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    REQUIRE(first_mismatch(samples.data(), out, 0, out_length) == kNoMismatch);
    sonare_free_floats(out);
  }

  sonare_free_note_objects(&extracted);
}

TEST_CASE("sonare_render_notes applies a gain edit inside the note only", "[c_api][note_objects]") {
  // 0.25 amplitude leaves room for the +6 dB case to stay below full scale.
  const std::vector<float> samples = sine(440.0f, 0.25f, 8000);
  constexpr size_t kOnset = 1920;
  constexpr size_t kOffset = 6080;
  // Clear of the 5 ms (80-sample) edge cross-fades, where the interior is
  // exactly the source times the linear gain.
  constexpr size_t kMargin = 160;

  const double source_rms = rms(samples.data(), kOnset + kMargin, kOffset - kMargin);
  REQUIRE(source_rms > 0.0);

  for (const float gain_db : {-6.0206f, 6.0206f}) {
    SonareNoteObject note = hand_note(kOnset, kOffset);
    note.edit.gain_db = gain_db;

    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(render_at(samples, &note, 1, nullptr, &out, &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());

    const double expected = std::pow(10.0, static_cast<double>(gain_db) / 20.0);
    REQUIRE_THAT(rms(out, kOnset + kMargin, kOffset - kMargin) / source_rms,
                 WithinRel(expected, 0.001));
    // Outside the note's own span the buffer is untouched, bit for bit.
    REQUIRE(first_mismatch(samples.data(), out, 0, kOnset) == kNoMismatch);
    REQUIRE(first_mismatch(samples.data(), out, kOffset, out_length) == kNoMismatch);
    sonare_free_floats(out);
  }
}

TEST_CASE("sonare_render_notes silences a muted note", "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.5f, 8000);
  constexpr size_t kOnset = 1920;
  constexpr size_t kOffset = 6080;
  constexpr size_t kMargin = 160;

  SonareNoteObject note = hand_note(kOnset, kOffset);
  note.edit.muted = 1;

  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(render_at(samples, &note, 1, nullptr, &out, &out_length) == SONARE_OK);
  REQUIRE(out_length == samples.size());
  REQUIRE(peak(samples.data(), kOnset + kMargin, kOffset - kMargin) > 0.4f);
  REQUIRE(peak(out, kOnset + kMargin, kOffset - kMargin) == 0.0f);
  REQUIRE(first_mismatch(samples.data(), out, 0, kOnset) == kNoMismatch);
  REQUIRE(first_mismatch(samples.data(), out, kOffset, out_length) == kNoMismatch);
  sonare_free_floats(out);
}

TEST_CASE("the SonareNoteRenderConfig fade takes its default at 0", "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.5f, 8000);
  constexpr size_t kOnset = 1920;
  constexpr size_t kOffset = 6080;

  SonareNoteObject note = hand_note(kOnset, kOffset);
  note.edit.muted = 1;

  auto rendered_with = [&](const SonareNoteRenderConfig* config) {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(render_at(samples, &note, 1, config, &out, &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    std::vector<float> copy(out, out + out_length);
    sonare_free_floats(out);
    return copy;
  };

  const std::vector<float> from_null = rendered_with(nullptr);
  SonareNoteRenderConfig config{};
  const std::vector<float> from_zeroed = rendered_with(&config);
  REQUIRE(first_mismatch(from_null.data(), from_zeroed.data(), 0, from_null.size()) == kNoMismatch);

  config.struct_version = 1;
  config.fade_ms = 5.0f;
  const std::vector<float> from_explicit_default = rendered_with(&config);
  REQUIRE(first_mismatch(from_null.data(), from_explicit_default.data(), 0, from_null.size()) ==
          kNoMismatch);

  // A 20 ms fade is still ramping 90 samples in, where the default 5 ms one has
  // already reached silence.
  config.fade_ms = 20.0f;
  const std::vector<float> from_long_fade = rendered_with(&config);
  REQUIRE(from_null[kOnset + 90] == 0.0f);
  REQUIRE(std::abs(from_long_fade[kOnset + 90]) > 1e-3f);
}

TEST_CASE(
    "a zeroed SonareNoteEdit is still the identity once it carries the envelope and curve fields",
    "[c_api][note_objects]") {
  constexpr size_t kFrames = 40;
  constexpr int kSamples = static_cast<int>(kFrames) * static_cast<int>(kSamplesPerFrame);
  const std::vector<float> samples = fm_tone(220.0, 30.0, 5.5, 0.4, kSamples);
  const std::vector<float> f0 = fm_track(220.0, 30.0, 5.5, kFrames);
  const std::vector<int32_t> voiced(kFrames, 1);

  SonareNoteObjectsResult extracted{};
  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &extracted) == SONARE_OK);
  REQUIRE(extracted.count >= 1);
  for (size_t i = 0; i < extracted.count; ++i) {
    extracted.notes[i].edit = SonareNoteEdit{};
    REQUIRE(extracted.notes[i].edit.envelope_offset == 0);
    REQUIRE(extracted.notes[i].edit.envelope_count == 0);
    REQUIRE(extracted.notes[i].edit.formant_shift_semitones == 0.0f);
    REQUIRE(extracted.notes[i].edit.vibrato_depth_change == 0.0f);
    REQUIRE(extracted.notes[i].edit.drift_change == 0.0f);
  }

  // The track is handed in, so the identity has to survive the path that reads
  // it rather than only the one that never looks.
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(render_with(samples, extracted.notes, extracted.count, nullptr, 0, f0.data(), f0.size(),
                      nullptr, &out, &out_length) == SONARE_OK);
  REQUIRE(out_length == samples.size());
  REQUIRE(first_mismatch(samples.data(), out, 0, out_length) == kNoMismatch);
  sonare_free_floats(out);

  // Non-vacuity, one new field at a time: each moves the output off the source,
  // so the equality above is the identity rather than an edit nobody read.
  const std::vector<float> pool = {0.25f, 1.0f};
  auto moves_the_output = [&](const float* envelopes, size_t envelope_count) {
    float* edited = nullptr;
    size_t edited_length = 0;
    REQUIRE(render_with(samples, extracted.notes, extracted.count, envelopes, envelope_count,
                        f0.data(), f0.size(), nullptr, &edited, &edited_length) == SONARE_OK);
    const bool moved = first_mismatch(samples.data(), edited, 0, edited_length) != kNoMismatch;
    sonare_free_floats(edited);
    extracted.notes[0].edit = SonareNoteEdit{};
    return moved;
  };

  extracted.notes[0].edit.formant_shift_semitones = 2.0f;
  REQUIRE(moves_the_output(nullptr, 0));
  extracted.notes[0].edit.vibrato_depth_change = -1.0f;
  REQUIRE(moves_the_output(nullptr, 0));
  extracted.notes[0].edit.drift_change = 1.0f;
  REQUIRE(moves_the_output(nullptr, 0));
  extracted.notes[0].edit.envelope_count = pool.size();
  REQUIRE(moves_the_output(pool.data(), pool.size()));

  sonare_free_note_objects(&extracted);
}

TEST_CASE("sonare_render_notes reads each note's envelope at its own offset in the pool",
          "[c_api][note_objects]") {
  // 400 Hz at 16 kHz is 40 samples a period, so each window below holds a whole
  // number of them and the source's RMS is the same in all four.
  const std::vector<float> samples = sine(400.0f, 0.5f, 6400);

  // The note's two points sit at offset 3; the filler around them is a 2x gain
  // that must never be read.
  const std::vector<float> pool = {2.0f, 2.0f, 2.0f, 0.25f, 1.0f, 2.0f};
  SonareNoteObject notes[2] = {hand_note(0, 3200), hand_note(3200, 6400)};
  notes[0].edit.envelope_offset = 3;
  notes[0].edit.envelope_count = 2;
  // A second note in the same call carrying no envelope. -6.0206 dB is exactly
  // half, flat across its whole span.
  notes[1].edit.gain_db = -6.0206f;

  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(render_with(samples, notes, 2, pool.data(), pool.size(), nullptr, 0, nullptr, &out,
                      &out_length) == SONARE_OK);
  REQUIRE(out_length == samples.size());

  // Inset past the 5 ms (80-sample) cross-fade at each span edge.
  const double head_source = rms(samples.data(), 160, 800);
  const double tail_source = rms(samples.data(), 2400, 3040);
  REQUIRE(head_source > 0.0);
  REQUIRE_THAT(tail_source, WithinRel(head_source, 1.0e-4));

  // 0.25 -> 1.0 stretched over the note leaves the envelope at 0.3625 and 0.8875
  // at these two windows' centres. A window's RMS sits slightly above its centre
  // value because the ramp is squared into it: c*sqrt(1 + (mh/c)^2/3) for slope m
  // over half-span h, which is +0.71% here and +0.12% there. Reading the pool at
  // 0 would put 2.0 at both.
  REQUIRE_THAT(rms(out, 160, 800) / head_source, WithinRel(0.36508, 0.02));
  REQUIRE_THAT(rms(out, 2400, 3040) / tail_source, WithinRel(0.88856, 0.02));

  // The second note is untouched by the pool: half throughout, not a ramp.
  REQUIRE_THAT(rms(out, 3360, 4000) / rms(samples.data(), 3360, 4000), WithinRel(0.5, 0.002));
  REQUIRE_THAT(rms(out, 5600, 6240) / rms(samples.data(), 5600, 6240), WithinRel(0.5, 0.002));
  sonare_free_floats(out);
}

TEST_CASE("sonare_render_notes bounds-checks every note's slice of the envelope pool",
          "[c_api][note_objects]") {
  const std::vector<float> samples = sine(400.0f, 0.4f, 8000);
  const std::vector<float> pool = {1.0f, 1.0f, 0.25f, 1.0f};

  auto note_with = [](int64_t envelope_offset, size_t envelope_count) {
    SonareNoteObject note = hand_note(1920, 6080);
    note.edit.envelope_offset = envelope_offset;
    note.edit.envelope_count = envelope_count;
    return note;
  };

  float* out = nullptr;
  size_t out_length = 0;
  auto rendered = [&](const SonareNoteObject& note, const float* envelopes, size_t envelope_count) {
    return render_with(samples, &note, 1, envelopes, envelope_count, nullptr, 0, nullptr, &out,
                       &out_length);
  };

  // A note asking for points from a pool that was never handed in, with and
  // without a count claiming otherwise.
  REQUIRE(rendered(note_with(0, 2), nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(rendered(note_with(0, 2), nullptr, pool.size()) == SONARE_ERROR_INVALID_PARAMETER);
  // Before the pool.
  REQUIRE(rendered(note_with(-1, 2), pool.data(), pool.size()) == SONARE_ERROR_INVALID_PARAMETER);
  // Exactly one element past its end, which is where a wrong comparison sits.
  REQUIRE(rendered(note_with(3, 2), pool.data(), pool.size()) == SONARE_ERROR_INVALID_PARAMETER);

  // A value the note's own slice covers has to be a usable linear gain.
  for (const float bad : {kNaN, kInf, -kInf, -1.0f}) {
    std::vector<float> poisoned = pool;
    poisoned[2] = bad;
    REQUIRE(rendered(note_with(2, 2), poisoned.data(), poisoned.size()) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  // Positive controls: a slice ending exactly at the pool's end, and the whole
  // pool. Neither is one past anything.
  REQUIRE(rendered(note_with(2, 2), pool.data(), pool.size()) == SONARE_OK);
  REQUIRE(out_length == samples.size());
  sonare_free_floats(out);
  REQUIRE(rendered(note_with(0, pool.size()), pool.data(), pool.size()) == SONARE_OK);
  sonare_free_floats(out);
}

TEST_CASE("sonare_render_notes warps the spectral envelope only when the formant shift is set",
          "[c_api][note_objects]") {
  const std::vector<float> samples = vowel_tone(200.0f, 1200.0f, 0.1f, 6400);

  auto rendered_with = [&](float formant_semitones, float gain_db) {
    SonareNoteObject note = hand_note(0, 6400, 200.0f);
    note.edit.formant_shift_semitones = formant_semitones;
    note.edit.gain_db = gain_db;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(render_at(samples, &note, 1, nullptr, &out, &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    std::vector<float> copy(out, out + out_length);
    sonare_free_floats(out);
    return copy;
  };

  // 0 runs no warp at all, so an edit carrying nothing else is still the
  // identity -- an LPC round at factor 1 would not come back bit for bit.
  const std::vector<float> flat = rendered_with(0.0f, 0.0f);
  REQUIRE(first_mismatch(samples.data(), flat.data(), 0, flat.size()) == kNoMismatch);

  const std::vector<float> warped = rendered_with(2.0f, 0.0f);
  require_edited(warped.data(), warped.size(), samples.data());

  // And it is read on a note the renderer was resynthesizing anyway, so the
  // field is not merely deciding whether any work happens at all.
  const std::vector<float> gain_only = rendered_with(0.0f, -3.0f);
  const std::vector<float> gain_and_warp = rendered_with(2.0f, -3.0f);
  require_edited(gain_and_warp.data(), gain_and_warp.size(), gain_only.data());
}

TEST_CASE("sonare_render_notes rejects a curve edit with no F0 track and applies it with one",
          "[c_api][note_objects]") {
  constexpr size_t kFrames = 40;
  constexpr int kSamples = static_cast<int>(kFrames) * static_cast<int>(kSamplesPerFrame);
  const std::vector<float> samples = fm_tone(220.0, 30.0, 5.5, 0.4, kSamples);
  const std::vector<float> f0 = fm_track(220.0, 30.0, 5.5, kFrames);

  float* out = nullptr;
  size_t out_length = 0;
  for (const bool on_the_vibrato : {true, false}) {
    SonareNoteObject note = hand_note(0, 6400, 220.0f);
    if (on_the_vibrato) {
      note.edit.vibrato_depth_change = -1.0f;
    } else {
      note.edit.drift_change = 1.0f;
    }

    // The curve the edit acts on is the caller's own track, and there is none.
    REQUIRE(render_at(samples, &note, 1, nullptr, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);

    // Its companion: the same edit with the track renders and moves the audio,
    // which is what makes the rejection about the track rather than the field.
    REQUIRE(render_with(samples, &note, 1, nullptr, 0, f0.data(), f0.size(), nullptr, &out,
                        &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    require_edited(out, out_length, samples.data());
    sonare_free_floats(out);
  }
}

TEST_CASE("sonare_render_notes rejects an F0 track that does not cover the notes it is given",
          "[c_api][note_objects]") {
  constexpr size_t kFrames = 40;
  constexpr int kSamples = static_cast<int>(kFrames) * static_cast<int>(kSamplesPerFrame);
  const std::vector<float> samples = fm_tone(220.0, 30.0, 5.5, 0.4, kSamples);
  const std::vector<float> f0 = fm_track(220.0, 30.0, 5.5, kFrames);

  SonareNoteObject note = hand_note(0, 6400, 220.0f);
  note.edit.vibrato_depth_change = -1.0f;
  REQUIRE(note.frame_end == static_cast<int32_t>(kFrames));

  float* out = nullptr;
  size_t out_length = 0;
  // A frame count without the array it counts.
  REQUIRE(render_with(samples, &note, 1, nullptr, 0, nullptr, f0.size(), nullptr, &out,
                      &out_length) == SONARE_ERROR_INVALID_PARAMETER);

  // A note whose frames run past the end of the track: the curve it would be
  // edited on is not there to slice.
  SonareNoteObject beyond = note;
  beyond.frame_end = static_cast<int32_t>(kFrames) + 5;
  REQUIRE(render_with(samples, &beyond, 1, nullptr, 0, f0.data(), f0.size(), nullptr, &out,
                      &out_length) == SONARE_ERROR_INVALID_PARAMETER);

  // Every note is sliced out of the track once one is given, whatever its edit
  // asks for, so an identity note running past the end is a read out of bounds
  // just the same.
  SonareNoteObject identity_beyond = hand_note(0, 6400, 220.0f);
  identity_beyond.frame_end = static_cast<int32_t>(kFrames) + 5;
  REQUIRE(identity_beyond.edit.vibrato_depth_change == 0.0f);
  REQUIRE(identity_beyond.edit.drift_change == 0.0f);
  REQUIRE(render_with(samples, &identity_beyond, 1, nullptr, 0, f0.data(), f0.size(), nullptr, &out,
                      &out_length) == SONARE_ERROR_INVALID_PARAMETER);

  // Its companion: the same identity note inside the track renders, and renders
  // the source back, so the rejection is about the bounds and not about the note
  // carrying no edit.
  const SonareNoteObject identity_inside = hand_note(0, 6400, 220.0f);
  REQUIRE(render_with(samples, &identity_inside, 1, nullptr, 0, f0.data(), f0.size(), nullptr, &out,
                      &out_length) == SONARE_OK);
  REQUIRE(first_mismatch(samples.data(), out, 0, out_length) == kNoMismatch);
  sonare_free_floats(out);

  for (const float bad_rate : {0.0f, -100.0f, kNaN, kInf}) {
    REQUIRE(sonare_render_notes(samples.data(), samples.size(), kSampleRate, &note, 1, nullptr, 0,
                                f0.data(), f0.size(), bad_rate, nullptr, &out,
                                &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  }

  // Positive control: a note ending exactly at the track's last frame is inside
  // it, so none of the above passes by rejecting every curve edit.
  REQUIRE(render_with(samples, &note, 1, nullptr, 0, f0.data(), f0.size(), nullptr, &out,
                      &out_length) == SONARE_OK);
  sonare_free_floats(out);
}

TEST_CASE("the SonareNoteRenderConfig vibrato cutoff decides which curve an edit acts on",
          "[c_api][note_objects]") {
  constexpr size_t kFrames = 40;
  constexpr int kSamples = static_cast<int>(kFrames) * static_cast<int>(kSamplesPerFrame);
  const std::vector<float> samples = fm_tone(220.0, 30.0, 5.5, 0.4, kSamples);
  const std::vector<float> f0 = fm_track(220.0, 30.0, 5.5, kFrames);

  SonareNoteObject note = hand_note(0, 6400, 220.0f);
  note.edit.vibrato_depth_change = -1.0f;

  auto rendered_with = [&](const SonareNoteRenderConfig* config) {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(render_with(samples, &note, 1, nullptr, 0, f0.data(), f0.size(), config, &out,
                        &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    std::vector<float> copy(out, out + out_length);
    sonare_free_floats(out);
    return copy;
  };

  const std::vector<float> from_null = rendered_with(nullptr);

  // 0 keeps the default 3 Hz, so a zeroed config and an explicit 3 both land on
  // what a NULL config renders.
  SonareNoteRenderConfig config{};
  const std::vector<float> from_zeroed = rendered_with(&config);
  REQUIRE(first_mismatch(from_null.data(), from_zeroed.data(), 0, from_null.size()) == kNoMismatch);

  config.vibrato_cutoff_hz = 3.0f;
  const std::vector<float> from_explicit_default = rendered_with(&config);
  REQUIRE(first_mismatch(from_null.data(), from_explicit_default.data(), 0, from_null.size()) ==
          kNoMismatch);

  // 5.5 Hz sits above a 3 Hz cut and below an 8 Hz one, so flattening the
  // vibrato takes most of the swing at 3 and little of it at 8. A cutoff that
  // was never read would render these two the same.
  config.vibrato_cutoff_hz = 8.0f;
  const std::vector<float> from_wide = rendered_with(&config);
  require_edited(from_wide.data(), from_wide.size(), from_null.data());

  // And in the direction the filter dictates: at a 3 Hz cut the 5.5 Hz swing is
  // vibrato and flattening takes it, at 8 Hz it is mostly drift and survives, so
  // the 3 Hz render is the one that moved further from the source. Measured 1.85
  // apart. Wired backwards this inverts rather than merely shrinking.
  REQUIRE(max_difference(samples.data(), from_null.data(), from_null.size()) >
          1.5f * max_difference(samples.data(), from_wide.data(), from_wide.size()));

  float* out = nullptr;
  size_t out_length = 0;
  for (const float bad : {kNaN, kInf, -kInf, -1.0f}) {
    SonareNoteRenderConfig rejected{};
    rejected.vibrato_cutoff_hz = bad;
    REQUIRE(render_with(samples, &note, 1, nullptr, 0, f0.data(), f0.size(), &rejected, &out,
                        &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_extract_notes rejects malformed arguments and clears its output",
          "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.5f, 6400);
  const std::vector<float> f0(40, 440.0f);
  const std::vector<float> prob(40, 1.0f);
  const std::vector<int32_t> voiced(40, 1);

  // Every rejection must leave the caller's result cleared, which the poisoned
  // fields here would otherwise still be carrying.
  auto rejects = [](auto&& call) {
    SonareNoteObjectsResult out{};
    out.notes = poisoned_notes();
    out.count = 7;
    out.amplitude = poisoned_floats();
    out.amplitude_count = 7;
    REQUIRE(call(&out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out.notes == nullptr);
    REQUIRE(out.count == 0);
    REQUIRE(out.amplitude == nullptr);
    REQUIRE(out.amplitude_count == 0);
  };

  REQUIRE(sonare_extract_notes(samples.data(), samples.size(), kSampleRate, f0.data(), nullptr,
                               voiced.data(), f0.size(), kFrameRate, nullptr,
                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, nullptr, prob.data(),
                                voiced.data(), f0.size(), kFrameRate, nullptr, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, f0.data(), nullptr,
                                voiced.data(), 0, kFrameRate, nullptr, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, f0.data(), nullptr,
                                nullptr, f0.size(), kFrameRate, nullptr, out);
  });
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_extract_notes(nullptr, 0, kSampleRate, f0.data(), nullptr, voiced.data(),
                                f0.size(), kFrameRate, nullptr, out);
  });

  for (const float bad_rate : {0.0f, -100.0f, kNaN, kInf}) {
    rejects([&](SonareNoteObjectsResult* out) {
      return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, f0.data(), nullptr,
                                  voiced.data(), f0.size(), bad_rate, nullptr, out);
    });
  }

  for (const float bad_f0 : {kNaN, kInf, -kInf, -1.0f}) {
    std::vector<float> poisoned = f0;
    poisoned[7] = bad_f0;
    rejects([&](SonareNoteObjectsResult* out) {
      return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, poisoned.data(),
                                  nullptr, voiced.data(), poisoned.size(), kFrameRate, nullptr,
                                  out);
    });
  }

  for (const float bad_prob : {kNaN, kInf, -0.1f, 1.1f}) {
    std::vector<float> poisoned = prob;
    poisoned[7] = bad_prob;
    rejects([&](SonareNoteObjectsResult* out) {
      return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, f0.data(),
                                  poisoned.data(), nullptr, f0.size(), kFrameRate, nullptr, out);
    });
  }

  for (const int32_t bad_version : {-1, 2, 99}) {
    SonareNoteExtractorConfig config{};
    config.struct_version = bad_version;
    rejects([&](SonareNoteObjectsResult* out) {
      return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, f0.data(), nullptr,
                                  voiced.data(), f0.size(), kFrameRate, &config, out);
    });
  }

  for (const float bad : {kNaN, kInf, -1.0f}) {
    SonareNoteExtractorConfig threshold{};
    threshold.segmentation_threshold_cents = bad;
    SonareNoteExtractorConfig min_note{};
    min_note.min_note_ms = bad;
    SonareNoteExtractorConfig reference{};
    reference.reference_hz = bad;
    SonareNoteExtractorConfig voiced_threshold{};
    voiced_threshold.voiced_threshold = bad;
    for (const SonareNoteExtractorConfig* config :
         {&threshold, &min_note, &reference, &voiced_threshold}) {
      rejects([&](SonareNoteObjectsResult* out) {
        return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, f0.data(), nullptr,
                                    voiced.data(), f0.size(), kFrameRate, config, out);
      });
    }
  }

  // A voiced threshold above 1 can never be met, so it is a malformed value
  // rather than a strict one.
  SonareNoteExtractorConfig unreachable{};
  unreachable.voiced_threshold = 1.5f;
  rejects([&](SonareNoteObjectsResult* out) {
    return sonare_extract_notes(samples.data(), samples.size(), kSampleRate, f0.data(), nullptr,
                                voiced.data(), f0.size(), kFrameRate, &unreachable, out);
  });

  // Positive control: the same call with nothing poisoned succeeds.
  SonareNoteObjectsResult ok{};
  REQUIRE(extract_at(samples, f0, prob.data(), voiced.data(), nullptr, &ok) == SONARE_OK);
  REQUIRE(ok.count == 1);
  sonare_free_note_objects(&ok);
}

TEST_CASE("sonare_render_notes rejects malformed spans, edits and config",
          "[c_api][note_objects]") {
  const std::vector<float> samples = sine(440.0f, 0.4f, 8000);

  // A non-identity edit throughout, so the note is one the renderer must act on.
  auto edited = [](int64_t onset, int64_t offset) {
    SonareNoteObject note = hand_note(onset, offset);
    note.edit.gain_db = -3.0f;
    return note;
  };

  float* out = nullptr;
  size_t out_length = 0;
  const SonareNoteObject note = edited(1920, 6080);

  REQUIRE(render_at(samples, &note, 1, nullptr, nullptr, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(render_at(samples, &note, 1, nullptr, &out, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(render_at(samples, nullptr, 1, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_render_notes(nullptr, 0, kSampleRate, &note, 1, nullptr, 0, nullptr, 0, kFrameRate,
                              nullptr, &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);

  // Empty, reversed and negative spans have nothing to render into.
  const SonareNoteObject empty_span = edited(1920, 1920);
  REQUIRE(render_at(samples, &empty_span, 1, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const SonareNoteObject reversed = edited(4000, 1920);
  REQUIRE(render_at(samples, &reversed, 1, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const SonareNoteObject negative_onset = edited(-160, 1600);
  REQUIRE(render_at(samples, &negative_onset, 1, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // Overlapping source spans: the model is monophonic, so this is not a
  // renderable set. Adjacent spans share a boundary and stay legal.
  const SonareNoteObject overlapping[2] = {edited(0, 3200), edited(1600, 4800)};
  REQUIRE(render_at(samples, overlapping, 2, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const SonareNoteObject adjacent[2] = {edited(0, 3200), edited(3200, 4800)};
  REQUIRE(render_at(samples, adjacent, 2, nullptr, &out, &out_length) == SONARE_OK);
  sonare_free_floats(out);

  for (const float bad : {kNaN, kInf, -kInf}) {
    SonareNoteObject pitch = edited(1920, 6080);
    pitch.edit.pitch_shift_semitones = bad;
    REQUIRE(render_at(samples, &pitch, 1, nullptr, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);

    SonareNoteObject gain = edited(1920, 6080);
    gain.edit.gain_db = bad;
    REQUIRE(render_at(samples, &gain, 1, nullptr, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);

    SonareNoteObject stretch = edited(1920, 6080);
    stretch.edit.time_stretch_ratio = bad;
    REQUIRE(render_at(samples, &stretch, 1, nullptr, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  // 0 is the identity spelling, so only a genuinely negative ratio is rejected.
  SonareNoteObject negative_ratio = edited(1920, 6080);
  negative_ratio.edit.time_stretch_ratio = -1.0f;
  REQUIRE(render_at(samples, &negative_ratio, 1, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);

  for (const int32_t bad_version : {-1, 2, 99}) {
    SonareNoteRenderConfig config{};
    config.struct_version = bad_version;
    REQUIRE(render_at(samples, &note, 1, &config, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  for (const float bad_fade : {kNaN, kInf, -kInf, -1.0f}) {
    SonareNoteRenderConfig config{};
    config.fade_ms = bad_fade;
    REQUIRE(render_at(samples, &note, 1, &config, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  // Positive control: the same note under a valid config renders.
  SonareNoteRenderConfig valid{};
  valid.struct_version = 1;
  valid.fade_ms = 5.0f;
  REQUIRE(render_at(samples, &note, 1, &valid, &out, &out_length) == SONARE_OK);
  REQUIRE(out_length == samples.size());
  sonare_free_floats(out);
}

TEST_CASE("sonare_free_note_objects clears the result it releases", "[c_api][note_objects]") {
  sonare_free_note_objects(nullptr);

  // A zeroed result owns nothing, so releasing it is a no-op rather than a free
  // of an uninitialized pointer.
  SonareNoteObjectsResult empty{};
  sonare_free_note_objects(&empty);
  REQUIRE(empty.notes == nullptr);
  REQUIRE(empty.amplitude == nullptr);

  const std::vector<float> samples = sine(440.0f, 0.5f, 6400);
  const std::vector<float> f0(40, 440.0f);
  const std::vector<int32_t> voiced(40, 1);
  SonareNoteObjectsResult out{};
  REQUIRE(extract_at(samples, f0, nullptr, voiced.data(), nullptr, &out) == SONARE_OK);
  REQUIRE(out.notes != nullptr);
  REQUIRE(out.amplitude != nullptr);

  sonare_free_note_objects(&out);
  REQUIRE(out.notes == nullptr);
  REQUIRE(out.count == 0);
  REQUIRE(out.amplitude == nullptr);
  REQUIRE(out.amplitude_count == 0);
}

#else

TEST_CASE("the note-object C API reports NOT_SUPPORTED without the pitch editor",
          "[c_api][note_objects]") {
  const std::vector<float> samples(1600, 0.25f);
  const std::vector<float> f0(10, 440.0f);
  const std::vector<int32_t> voiced(10, 1);

  SonareNoteObjectsResult out{};
  REQUIRE(sonare_extract_notes(samples.data(), samples.size(), 16000, f0.data(), nullptr,
                               voiced.data(), f0.size(), 100.0f, nullptr,
                               &out) == SONARE_ERROR_NOT_SUPPORTED);

  float* rendered = nullptr;
  size_t rendered_length = 0;
  REQUIRE(sonare_render_notes(samples.data(), samples.size(), 16000, nullptr, 0, nullptr, 0,
                              nullptr, 0, 100.0f, nullptr, &rendered,
                              &rendered_length) == SONARE_ERROR_NOT_SUPPORTED);
}

#endif
