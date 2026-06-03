/// @file stream_analyzer_integration_test.cpp
/// @brief StreamAnalyzer integration simulation tests.

#include "stream_analyzer_test_helpers.h"

TEST_CASE("StreamAnalyzer integration: AudioWorklet simulation", "[streaming][integration]") {
  // Simulate AudioWorklet processing with 128-sample chunks
  StreamConfig config;
  config.sample_rate = 44100;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_mel = true;
  config.compute_chroma = true;
  config.compute_onset = true;
  config.n_mels = 128;
  config.emit_every_n_frames = 1;
  config.bpm_update_interval_sec = 3.0f;  // Lower interval for test

  StreamAnalyzer analyzer(config);

  // Generate 10 seconds of rhythmic audio (enough for BPM estimation)
  int total_samples = 44100 * 10;
  std::vector<float> audio(total_samples, 0.0f);
  float bpm = 120.0f;
  float beat_interval = 60.0f / bpm;
  int beat_samples = static_cast<int>(beat_interval * 44100);

  for (int i = 0; i < total_samples; i += beat_samples) {
    // Add a short click at each beat
    int click_len = std::min(441, total_samples - i);  // 10ms click
    for (int j = 0; j < click_len; ++j) {
      float decay = std::exp(-static_cast<float>(j) / 100.0f);
      audio[i + j] = decay * std::sin(kTwoPi * 1000.0f * j / 44100.0f);
    }
  }

  SECTION("process in 128-sample chunks like AudioWorklet") {
    constexpr int kRenderQuantum = 128;
    size_t total_frames_read = 0;

    for (int offset = 0; offset + kRenderQuantum <= total_samples; offset += kRenderQuantum) {
      analyzer.process(audio.data() + offset, kRenderQuantum);

      // Periodically read frames (like postMessage from worker)
      if (analyzer.available_frames() >= 10) {
        FrameBuffer buffer;
        analyzer.read_frames_soa(10, buffer);
        total_frames_read += buffer.n_frames;

        // Verify buffer contents
        REQUIRE(buffer.n_frames <= 10);
        for (size_t i = 0; i < buffer.n_frames; ++i) {
          REQUIRE(buffer.timestamps[i] >= 0.0f);
        }
      }
    }

    // Read remaining frames
    while (analyzer.available_frames() > 0) {
      FrameBuffer buffer;
      analyzer.read_frames_soa(100, buffer);
      total_frames_read += buffer.n_frames;
    }

    // Should have processed ~5 seconds worth of frames
    // Frames = (total_samples - n_fft) / hop_length + 1
    int expected_frames = (total_samples - config.n_fft) / config.hop_length + 1;
    REQUIRE(total_frames_read > 0);
    REQUIRE(static_cast<int>(total_frames_read) <= expected_frames);
  }

  SECTION("BPM estimation converges after sufficient audio") {
    // Process all at once
    analyzer.process(audio.data(), audio.size());

    auto stats = analyzer.stats();

    // Should have estimated BPM
    REQUIRE(stats.estimate.bpm > 0.0f);
    // BPM should be in reasonable range
    REQUIRE(stats.estimate.bpm >= 60.0f);
    REQUIRE(stats.estimate.bpm <= 200.0f);
  }
}

TEST_CASE("StreamAnalyzer integration: seek simulation", "[streaming][integration]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;

  StreamAnalyzer analyzer(config);

  // Process first 2 seconds
  std::vector<float> audio1 = generate_sine(22050 * 2, 440.0f, 22050);
  analyzer.process(audio1.data(), audio1.size());

  float time_before_reset = analyzer.current_time();
  REQUIRE_THAT(time_before_reset, WithinRel(2.0f, 0.1f));

  SECTION("reset and continue from different position") {
    // Simulate seek to 10 seconds
    size_t seek_offset = 22050 * 10;
    analyzer.reset(seek_offset);

    // Verify state after reset
    REQUIRE(analyzer.frame_count() == 0);
    REQUIRE(analyzer.available_frames() == 0);

    // Process more audio (from new position)
    std::vector<float> audio2 = generate_sine(22050, 880.0f, 22050);
    analyzer.process(audio2.data(), audio2.size());

    // First frame should have timestamp near 10 seconds
    auto frames = analyzer.read_frames(1);
    REQUIRE(frames.size() == 1);
    REQUIRE_THAT(frames[0].timestamp, WithinRel(10.0f, 0.01f));
  }
}

TEST_CASE("StreamAnalyzer integration: all output formats", "[streaming][integration]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_mel = true;
  config.compute_chroma = true;
  config.n_mels = 64;

  // Generate 1 second of audio
  std::vector<float> audio = generate_sine(22050, 440.0f, 22050);

  SECTION("SOA, U8, and I16 produce same frame count") {
    StreamAnalyzer analyzer1(config);
    StreamAnalyzer analyzer2(config);
    StreamAnalyzer analyzer3(config);

    analyzer1.process(audio.data(), audio.size());
    analyzer2.process(audio.data(), audio.size());
    analyzer3.process(audio.data(), audio.size());

    FrameBuffer soa_buffer;
    QuantizedFrameBufferU8 u8_buffer;
    QuantizedFrameBufferI16 i16_buffer;

    analyzer1.read_frames_soa(100, soa_buffer);
    analyzer2.read_frames_quantized_u8(100, u8_buffer);
    analyzer3.read_frames_quantized_i16(100, i16_buffer);

    REQUIRE(soa_buffer.n_frames == u8_buffer.n_frames);
    REQUIRE(soa_buffer.n_frames == i16_buffer.n_frames);
    REQUIRE(soa_buffer.n_frames > 0);
  }
}

// ============================================================================
// P0-E regression test: extended ChordQuality enumerators in bar-vote table
// ============================================================================

TEST_CASE("StreamAnalyzer kBarVoteSlots constant pins kNumChordQualities cardinality",
          "[streaming][chord]") {
  // Compile-time regression guard for P0-E. Before the fix, kBarVoteSlots was
  // 12 * 4 = 48 (only covering Major/Minor/Diminished/Augmented), so any
  // ChordQuality >= 4 would compute a vote-table index of
  //   root * 4 + quality  (wrong)
  // that could alias into a neighbour's slot or exceed the array bounds.
  //
  // After the fix, kBarVoteSlots = 12 * kNumChordQualities = 204, and vote
  // indices use `root * kNumChordQualities + quality`.  We pin the expected
  // value so that a future reduction of kNumChordQualities (which would
  // reintroduce the bug) fails the build.
  static_assert(sonare::kNumChordQualities == 17,
                "kNumChordQualities changed — update kBarVoteSlots and this test");
  static_assert(sonare::StreamAnalyzer::kBarVoteSlots == 12 * 17,
                "kBarVoteSlots must equal 12 * kNumChordQualities");

  // The Sus2Add4 enumerator (index 16) is the highest-numbered quality; its
  // vote-table index for root=11 (B) would be 11 * 17 + 16 = 203, which is
  // exactly kBarVoteSlots - 1. Verify it is strictly less than kBarVoteSlots
  // (i.e., in-bounds).
  constexpr int kLastQualityIdx = static_cast<int>(sonare::ChordQuality::Sus2Add4);
  constexpr int kHighestVoteIdx = 11 * sonare::kNumChordQualities + kLastQualityIdx;
  static_assert(kHighestVoteIdx < sonare::StreamAnalyzer::kBarVoteSlots,
                "Vote index for Sus2Add4 with root=B exceeds kBarVoteSlots — array OOB");

  // Runtime check: a StreamAnalyzer driven through enough audio to emit chord
  // frames must not crash, and the per-frame chord_quality values must be
  // representable within the extended quality range (0 .. kNumChordQualities-1).
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_chroma = true;

  StreamAnalyzer analyzer(config);

  // Synthesise a G dominant-7 chord: G(392Hz) + B(494Hz) + D(587Hz) + F(698Hz).
  // This gives the analyser the best chance of detecting a non-triad quality.
  constexpr int kSr = 22050;
  constexpr int kDuration = kSr * 8;  // 8 seconds
  std::vector<float> audio(static_cast<size_t>(kDuration), 0.0f);
  constexpr float kAmp = 0.25f;
  const float freqs[] = {392.0f, 494.0f, 587.0f, 698.0f};
  for (int i = 0; i < kDuration; ++i) {
    float sample = 0.0f;
    for (float f : freqs) {
      sample += kAmp * std::sin(kTwoPi * f * static_cast<float>(i) / kSr);
    }
    audio[static_cast<size_t>(i)] = sample;
  }

  analyzer.process(audio.data(), audio.size());

  auto frames = analyzer.read_frames(1000);
  REQUIRE(frames.size() > 0);

  // Every per-frame chord_quality must be within the now-correct range.
  // Before the fix, extended-quality indices could wrap into invalid slots;
  // after the fix they are all in [0, kNumChordQualities).
  for (const auto& frame : frames) {
    REQUIRE(frame.chord_quality >= 0);
    REQUIRE(frame.chord_quality < sonare::kNumChordQualities);
  }
}

// ============================================================================
// H7 regression: onset_accumulator_ must stay bounded over long streams
// ============================================================================
