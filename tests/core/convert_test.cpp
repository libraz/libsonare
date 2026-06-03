/// @file convert_test.cpp
/// @brief Tests for unit conversion functions.

#include "core/convert.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("hz_to_mel / mel_to_hz", "[convert]") {
  SECTION("low frequency (linear region)") {
    float hz = 500.0f;
    float mel = hz_to_mel(hz);
    REQUIRE_THAT(mel, WithinRel(7.5f, 0.01f));
    REQUIRE_THAT(mel_to_hz(mel), WithinAbs(hz, 0.1f));
  }

  SECTION("high frequency (log region)") {
    float hz = 4000.0f;
    float mel = hz_to_mel(hz);
    REQUIRE_THAT(mel_to_hz(mel), WithinAbs(hz, 0.1f));
  }

  SECTION("1000Hz boundary") {
    float mel = hz_to_mel(1000.0f);
    REQUIRE_THAT(mel, WithinRel(15.0f, 0.01f));
  }
}

TEST_CASE("hz_to_midi / midi_to_hz", "[convert]") {
  // A4 = 440Hz = MIDI 69
  REQUIRE_THAT(hz_to_midi(440.0f), WithinAbs(69.0f, 0.01f));
  REQUIRE_THAT(midi_to_hz(69.0f), WithinAbs(440.0f, 0.01f));

  // C4 = 261.63Hz = MIDI 60
  REQUIRE_THAT(hz_to_midi(261.63f), WithinAbs(60.0f, 0.1f));
  REQUIRE_THAT(midi_to_hz(60.0f), WithinAbs(261.63f, 0.1f));
}

TEST_CASE("hz_to_midi non-positive input is not a valid pitch sentinel", "[convert]") {
  // 0 is a valid MIDI value (C-1), so non-positive Hz must not return 0.0f and
  // masquerade as a real pitch. It should return -inf instead.
  SECTION("zero Hz returns -inf") {
    float midi = hz_to_midi(0.0f);
    REQUIRE(std::isinf(midi));
    REQUIRE(midi < 0.0f);
  }

  SECTION("negative Hz returns -inf") {
    float midi = hz_to_midi(-440.0f);
    REQUIRE(std::isinf(midi));
    REQUIRE(midi < 0.0f);
  }

  SECTION("a frequency that legitimately maps to MIDI 0 is distinguishable") {
    // C-1 is MIDI 0 ~= 8.1758 Hz. This is a valid pitch and must remain finite.
    float c_minus1_hz = midi_to_hz(0.0f);
    float midi = hz_to_midi(c_minus1_hz);
    REQUIRE(std::isfinite(midi));
    REQUIRE_THAT(midi, WithinAbs(0.0f, 0.01f));
  }
}

TEST_CASE("bin_to_hz does not overflow at high sample rates", "[convert]") {
  // bin * sr computed in int overflows for large bins / sample rates. With
  // double-precision accumulation the result must remain accurate.
  SECTION("high sample rate, large bin") {
    int sr = 192000;
    int n_fft = 4096;
    int bin = 2048;  // Nyquist bin; bin * sr = 393'216'000 overflows int32.
    float hz = bin_to_hz(bin, sr, n_fft);
    // Expected: 2048 * 192000 / 4096 = 96000 Hz (Nyquist).
    REQUIRE_THAT(hz, WithinRel(96000.0f, 1e-4f));
  }

  SECTION("standard parameters remain correct") {
    int sr = 22050;
    int n_fft = 2048;
    REQUIRE_THAT(bin_to_hz(0, sr, n_fft), WithinAbs(0.0f, 1e-3f));
    // bin 1024 (Nyquist) -> sr / 2 = 11025 Hz.
    REQUIRE_THAT(bin_to_hz(1024, sr, n_fft), WithinRel(11025.0f, 1e-4f));
  }
}

TEST_CASE("frames_to_samples saturates instead of overflowing int", "[convert]") {
  // frames * hop_length is computed in 64-bit and clamped to int, so a huge
  // frame index (long files) cannot wrap to a negative sample position (UB).
  const int huge = std::numeric_limits<int>::max();
  REQUIRE(frames_to_samples(huge, 512, 2048) == std::numeric_limits<int>::max());
  // Normal values are unaffected.
  REQUIRE(frames_to_samples(10, 512, 0) == 5120);
  REQUIRE(frames_to_samples(10, 512, 2048) == 5120 + 1024);
}

TEST_CASE("hz_to_note / note_to_hz", "[convert]") {
  REQUIRE(hz_to_note(440.0f) == "A4");
  REQUIRE(hz_to_note(261.63f) == "C4");
  REQUIRE_THAT(note_to_hz("A4"), WithinAbs(440.0f, 0.01f));
  REQUIRE_THAT(note_to_hz("C4"), WithinAbs(261.63f, 0.1f));
}

TEST_CASE("hz_to_note with sub-zero octave frequencies", "[convert]") {
  SECTION("very low frequencies should not crash") {
    // Very low frequency - should not crash (negative MIDI index)
    REQUIRE_NOTHROW(hz_to_note(1.0f));
    REQUIRE_NOTHROW(hz_to_note(5.0f));
    REQUIRE_NOTHROW(hz_to_note(0.1f));

    // Should return valid note strings
    std::string note1 = hz_to_note(1.0f);
    REQUIRE(!note1.empty());

    std::string note5 = hz_to_note(5.0f);
    REQUIRE(!note5.empty());
  }
}

TEST_CASE("note_to_hz with non-ASCII input", "[convert]") {
  SECTION("non-ASCII bytes should not cause UB") {
    // Should not crash (UB from negative char in toupper)
    REQUIRE_NOTHROW(note_to_hz("\xC0"));
    // Invalid note should return 0
    REQUIRE(note_to_hz("\xC0") == 0.0f);
  }
}

TEST_CASE("samples_to_frames guards against non-positive hop_length", "[convert]") {
  // Dividing by hop_length without a guard is integer division-by-zero (UB) for
  // hop_length == 0 and yields a meaningless sign-flipped index for a negative
  // hop. A frame index is undefined without a positive hop, so the helper returns
  // 0 frames (defined, non-throwing) rather than invoking UB.
  SECTION("zero hop_length returns 0 instead of dividing by zero") {
    REQUIRE(samples_to_frames(5120, 0, 0) == 0);
    REQUIRE(samples_to_frames(5120, 0, 2048) == 0);
    REQUIRE(samples_to_frames(0, 0, 0) == 0);
  }

  SECTION("negative hop_length returns 0") {
    REQUIRE(samples_to_frames(5120, -512, 0) == 0);
    REQUIRE(samples_to_frames(5120, -512, 2048) == 0);
  }

  SECTION("vector overload is also guarded element-wise") {
    const std::vector<int> samples = {0, 1024, 5120};
    const std::vector<int> framed = samples_to_frames(samples, 0, 2048);
    REQUIRE(framed.size() == samples.size());
    for (int f : framed) REQUIRE(f == 0);
  }

  SECTION("valid hop_length is unaffected") {
    // 5120 samples, hop 512, no n_fft offset -> 10 frames (librosa-compatible).
    REQUIRE(samples_to_frames(5120, 512, 0) == 10);
    // With n_fft = 2048 the centering offset (1024) is subtracted first.
    REQUIRE(samples_to_frames(5120 + 1024, 512, 2048) == 10);
  }
}

TEST_CASE("frames_to_time / time_to_frames", "[convert]") {
  int sr = 22050;
  int hop = 512;

  // 100 frames * 512 / 22050 = 2.3219...
  float time = frames_to_time(100, sr, hop);
  REQUIRE_THAT(time, WithinAbs(2.32f, 0.01f));
  // Round-trip test uses computed time value
  REQUIRE(time_to_frames(time, sr, hop) == 100);
}

TEST_CASE("time_to_frames floor behavior", "[convert]") {
  // Test that time_to_frames uses floor
  int sr = 22050;
  int hop = 512;

  SECTION("exact frame boundaries") {
    // Exactly at frame 10 boundary
    float time_exact = 10.0f * static_cast<float>(hop) / static_cast<float>(sr);
    REQUIRE(time_to_frames(time_exact, sr, hop) == 10);
  }

  SECTION("just before frame boundary") {
    // Slightly before frame 11 boundary should give frame 10
    float time_before = 10.999f * static_cast<float>(hop) / static_cast<float>(sr);
    REQUIRE(time_to_frames(time_before, sr, hop) == 10);
  }

  SECTION("just after frame boundary") {
    // Slightly after frame 10 boundary should give frame 10
    float time_after = 10.001f * static_cast<float>(hop) / static_cast<float>(sr);
    REQUIRE(time_to_frames(time_after, sr, hop) == 10);
  }

  SECTION("zero time") { REQUIRE(time_to_frames(0.0f, sr, hop) == 0); }

  SECTION("very small time") {
    // Less than one hop should give frame 0
    float tiny_time = 0.001f;  // ~22 samples at 22050 Hz, less than 512 hop
    REQUIRE(time_to_frames(tiny_time, sr, hop) == 0);
  }
}
