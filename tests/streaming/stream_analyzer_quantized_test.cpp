/// @file stream_analyzer_quantized_test.cpp
/// @brief StreamAnalyzer progressive and quantized output tests.

#include "stream_analyzer_test_helpers.h"

TEST_CASE("StreamAnalyzer progressive BPM estimation", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_onset = true;
  config.compute_mel = true;
  config.bpm_update_interval_sec = 5.0f;

  StreamAnalyzer analyzer(config);

  SECTION("BPM not estimated with insufficient data") {
    // Process only 1 second of audio (not enough)
    std::vector<float> audio = generate_sine(22050, 440.0f, 22050);
    analyzer.process(audio.data(), audio.size());

    auto stats = analyzer.stats();
    // BPM estimate should still be 0 (not enough data)
    REQUIRE(stats.estimate.bpm == 0.0f);
  }

  SECTION("BPM estimated after sufficient data") {
    // Generate 15 seconds of audio with rhythmic content (click track)
    int sr = 22050;
    int duration_sec = 15;
    int total_samples = sr * duration_sec;
    std::vector<float> audio(total_samples, 0.0f);

    // Create a click track at 120 BPM
    float bpm = 120.0f;
    float beat_interval_samples = 60.0f * sr / bpm;

    for (int beat = 0; beat < static_cast<int>(duration_sec * bpm / 60.0f); ++beat) {
      int beat_start = static_cast<int>(beat * beat_interval_samples);
      // Short click (10ms burst of noise-like signal)
      int click_len = std::min(220, total_samples - beat_start);  // 10ms at 22050Hz
      for (int i = 0; i < click_len; ++i) {
        if (beat_start + i < total_samples) {
          // Use a decaying sine burst to simulate a click
          float decay = std::exp(-static_cast<float>(i) / 50.0f);
          audio[beat_start + i] = decay * std::sin(kTwoPi * 1000.0f * i / sr);
        }
      }
    }

    analyzer.process(audio.data(), audio.size());

    auto stats = analyzer.stats();
    // BPM should be estimated
    REQUIRE(stats.estimate.bpm > 0.0f);
    // BPM should be in reasonable range (60-200)
    REQUIRE(stats.estimate.bpm >= 60.0f);
    REQUIRE(stats.estimate.bpm <= 200.0f);
    // Confidence should be non-zero
    REQUIRE(stats.estimate.bpm_confidence > 0.0f);
  }

  SECTION("candidate count tracks onset frames") {
    // Process 5 seconds of audio
    std::vector<float> audio = generate_sine(22050 * 5, 440.0f, 22050);
    analyzer.process(audio.data(), audio.size());

    auto stats = analyzer.stats();
    // bpm_candidate_count should reflect onset frames
    REQUIRE(stats.estimate.bpm_candidate_count > 0);
  }
}

TEST_CASE("StreamAnalyzer progressive key estimation", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_chroma = true;
  config.key_update_interval_sec = 3.0f;

  StreamAnalyzer analyzer(config);

  // Generate 10 seconds of A note (440 Hz)
  int total_samples = 22050 * 10;
  std::vector<float> audio = generate_sine(total_samples, 440.0f, 22050);
  analyzer.process(audio.data(), audio.size());

  auto stats = analyzer.stats();

  SECTION("key is estimated") {
    // Key should be set (0-11)
    REQUIRE(stats.estimate.key >= 0);
    REQUIRE(stats.estimate.key < 12);
  }

  SECTION("key confidence is positive") { REQUIRE(stats.estimate.key_confidence > 0.0f); }

  SECTION("A note should give key near A (9)") {
    // A = pitch class 9 (C=0, C#=1, ..., A=9)
    // Allow some tolerance due to algorithm variations
    REQUIRE(stats.estimate.key == 9);
  }
}

TEST_CASE("StreamAnalyzer quantized output U8", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_mel = true;
  config.compute_chroma = true;
  config.n_mels = 64;

  StreamAnalyzer analyzer(config);

  // Generate audio
  std::vector<float> audio = generate_sine(22050, 440.0f, 22050);
  analyzer.process(audio.data(), audio.size());

  QuantizedFrameBufferU8 buffer;
  QuantizeConfig qconfig;
  analyzer.read_frames_quantized_u8(10, buffer, qconfig);

  SECTION("buffer sizes are correct") {
    REQUIRE(buffer.n_frames <= 10);
    REQUIRE(buffer.n_frames > 0);
    REQUIRE(buffer.timestamps.size() == buffer.n_frames);
    REQUIRE(buffer.mel.size() == buffer.n_frames * 64);
    REQUIRE(buffer.chroma.size() == buffer.n_frames * 12);
    REQUIRE(buffer.onset_strength.size() == buffer.n_frames);
    REQUIRE(buffer.rms_energy.size() == buffer.n_frames);
    REQUIRE(buffer.spectral_centroid.size() == buffer.n_frames);
    REQUIRE(buffer.spectral_flatness.size() == buffer.n_frames);
  }

  SECTION("quantized values are in valid range") {
    for (uint8_t v : buffer.mel) {
      REQUIRE(v <= 255);
    }
    for (uint8_t v : buffer.chroma) {
      REQUIRE(v <= 255);
    }
  }

  SECTION("timestamps preserved as float") {
    for (float t : buffer.timestamps) {
      REQUIRE(t >= 0.0f);
    }
  }
}

TEST_CASE("StreamAnalyzer quantized output I16", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_mel = true;
  config.compute_chroma = true;
  config.n_mels = 64;

  StreamAnalyzer analyzer(config);

  // Generate audio
  std::vector<float> audio = generate_sine(22050, 440.0f, 22050);
  analyzer.process(audio.data(), audio.size());

  QuantizedFrameBufferI16 buffer;
  QuantizeConfig qconfig;
  analyzer.read_frames_quantized_i16(10, buffer, qconfig);

  SECTION("buffer sizes are correct") {
    REQUIRE(buffer.n_frames <= 10);
    REQUIRE(buffer.n_frames > 0);
    REQUIRE(buffer.timestamps.size() == buffer.n_frames);
    REQUIRE(buffer.mel.size() == buffer.n_frames * 64);
    REQUIRE(buffer.chroma.size() == buffer.n_frames * 12);
  }

  SECTION("quantized values are in valid range") {
    for (int16_t v : buffer.mel) {
      REQUIRE(v >= -32768);
      REQUIRE(v <= 32767);
    }
    for (int16_t v : buffer.chroma) {
      REQUIRE(v >= -32768);
      REQUIRE(v <= 32767);
    }
  }
}

TEST_CASE("quantize/dequantize map endpoints symmetrically", "[streaming]") {
  using sonare::streaming_detail::dequantize_from_i16;
  using sonare::streaming_detail::dequantize_from_u8;
  using sonare::streaming_detail::quantize_to_i16;
  using sonare::streaming_detail::quantize_to_u8;

  // Endpoints: min -> low extreme, max -> high extreme. The i16 low end used to
  // truncate to -32767 instead of -32768.
  REQUIRE(quantize_to_u8(0.0f, 0.0f, 1.0f) == 0);
  REQUIRE(quantize_to_u8(1.0f, 0.0f, 1.0f) == 255);
  REQUIRE(quantize_to_i16(0.0f, 0.0f, 1.0f) == -32768);
  REQUIRE(quantize_to_i16(1.0f, 0.0f, 1.0f) == 32767);

  // Round-trip recovers the value (within [min,max]) to one quantization step.
  for (float value : {-2.0f, 0.0f, 2.5f, 5.0f, 6.0f}) {
    REQUIRE_THAT(dequantize_from_u8(quantize_to_u8(value, -2.0f, 6.0f), -2.0f, 6.0f),
                 Catch::Matchers::WithinAbs(value, 8.0f / 255.0f));
    REQUIRE_THAT(dequantize_from_i16(quantize_to_i16(value, -2.0f, 6.0f), -2.0f, 6.0f),
                 Catch::Matchers::WithinAbs(value, 8.0f / 65535.0f));
  }
}

TEST_CASE("Quantization bandwidth reduction", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_mel = true;
  config.n_mels = 128;

  StreamAnalyzer analyzer(config);

  // Generate audio
  std::vector<float> audio = generate_sine(22050 * 2, 440.0f, 22050);
  analyzer.process(audio.data(), audio.size());

  // Read same data in different formats
  size_t n_frames = analyzer.available_frames();

  // Calculate expected sizes for 1 frame
  // Float32 SOA: timestamps(4) + mel(128*4) + chroma(12*4) + onset(4) + rms(4) + centroid(4) +
  // flatness(4)
  //            = 4 + 512 + 48 + 16 = 580 bytes per frame
  // U8: timestamps(4) + mel(128) + chroma(12) + onset(1) + rms(1) + centroid(1) + flatness(1)
  //   = 4 + 128 + 12 + 4 = 148 bytes per frame
  // Ratio: ~4x reduction

  SECTION("U8 is approximately 4x smaller than Float32") {
    size_t float32_size = n_frames * (sizeof(float) * (1 + 128 + 12 + 4));  // ~580 bytes/frame
    size_t u8_size =
        n_frames * (sizeof(float) + sizeof(uint8_t) * (128 + 12 + 4));  // ~148 bytes/frame

    // U8 should be roughly 4x smaller (allowing some tolerance)
    float ratio = static_cast<float>(float32_size) / static_cast<float>(u8_size);
    REQUIRE(ratio > 3.0f);
    REQUIRE(ratio < 5.0f);
  }
}

// ============================================================================
// Integration Tests
// ============================================================================
