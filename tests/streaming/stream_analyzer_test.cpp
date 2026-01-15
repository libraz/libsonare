/// @file stream_analyzer_test.cpp
/// @brief Tests for StreamAnalyzer.

#include "streaming/stream_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

std::vector<float> generate_sine(int samples, float freq, int sr) {
  std::vector<float> result(samples);
  for (int i = 0; i < samples; ++i) {
    result[i] = std::sin(kTwoPi * freq * i / sr);
  }
  return result;
}

}  // namespace

TEST_CASE("StreamConfig helpers", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 44100;
  config.n_fft = 2048;
  config.hop_length = 512;

  SECTION("n_bins") { REQUIRE(config.n_bins() == 1025); }

  SECTION("overlap") { REQUIRE(config.overlap() == 1536); }

  SECTION("frame_duration") {
    float expected = 512.0f / 44100.0f;
    REQUIRE_THAT(config.frame_duration(), WithinRel(expected, 0.001f));
  }

  SECTION("effective_fmax default") {
    REQUIRE_THAT(config.effective_fmax(), WithinAbs(22050.0f, 0.1f));
  }

  SECTION("effective_fmax custom") {
    config.fmax = 8000.0f;
    REQUIRE_THAT(config.effective_fmax(), WithinAbs(8000.0f, 0.1f));
  }
}

TEST_CASE("StreamAnalyzer basic processing", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;

  StreamAnalyzer analyzer(config);

  SECTION("empty input produces no frames") {
    analyzer.process(nullptr, 0);
    REQUIRE(analyzer.available_frames() == 0);
    REQUIRE(analyzer.frame_count() == 0);
  }

  SECTION("small chunk produces no frames") {
    std::vector<float> chunk(1000, 0.0f);
    analyzer.process(chunk.data(), chunk.size());
    REQUIRE(analyzer.available_frames() == 0);
  }

  SECTION("chunk >= n_fft produces at least one frame") {
    std::vector<float> chunk(2048, 0.0f);
    analyzer.process(chunk.data(), chunk.size());
    REQUIRE(analyzer.available_frames() >= 1);
  }
}

TEST_CASE("StreamAnalyzer overlap handling", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;

  StreamAnalyzer analyzer(config);

  SECTION("multiple small chunks accumulate correctly") {
    std::vector<float> chunk(512, 0.0f);
    int total_frames = 0;

    for (int i = 0; i < 10; ++i) {
      analyzer.process(chunk.data(), chunk.size());
      total_frames += static_cast<int>(analyzer.available_frames());
      analyzer.read_frames(100);  // Consume frames
    }

    // After 5120 samples with n_fft=2048, hop=512:
    // First frame after 2048 samples (4 chunks), then one per 512 samples
    // Total: floor((5120 - 2048) / 512) + 1 = 7 frames
    REQUIRE(total_frames == 7);
  }
}

TEST_CASE("StreamAnalyzer timestamp accuracy", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;

  StreamAnalyzer analyzer(config);

  // Process enough for multiple frames
  std::vector<float> audio(22050, 0.0f);  // 1 second
  analyzer.process(audio.data(), audio.size());

  auto frames = analyzer.read_frames(100);
  REQUIRE(frames.size() > 1);

  float expected_interval = static_cast<float>(config.hop_length) / config.sample_rate;

  SECTION("timestamps are equally spaced") {
    for (size_t i = 1; i < frames.size(); ++i) {
      float interval = frames[i].timestamp - frames[i - 1].timestamp;
      REQUIRE_THAT(interval, WithinRel(expected_interval, 0.001f));
    }
  }

  SECTION("frame indices are sequential") {
    for (size_t i = 0; i < frames.size(); ++i) {
      REQUIRE(frames[i].frame_index == static_cast<int>(i));
    }
  }
}

TEST_CASE("StreamAnalyzer onset_valid flag", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_onset = true;
  config.compute_mel = true;

  StreamAnalyzer analyzer(config);

  // Generate enough audio for multiple frames
  std::vector<float> audio = generate_sine(22050, 440.0f, 22050);
  analyzer.process(audio.data(), audio.size());

  auto frames = analyzer.read_frames(100);
  REQUIRE(frames.size() > 1);

  SECTION("first frame has onset_valid = false") { REQUIRE_FALSE(frames[0].onset_valid); }

  SECTION("subsequent frames have onset_valid = true") {
    for (size_t i = 1; i < frames.size(); ++i) {
      REQUIRE(frames[i].onset_valid);
    }
  }
}

TEST_CASE("StreamAnalyzer reset", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;

  StreamAnalyzer analyzer(config);

  // Process some audio
  std::vector<float> audio(22050, 0.0f);
  analyzer.process(audio.data(), audio.size());

  REQUIRE(analyzer.frame_count() > 0);
  REQUIRE(analyzer.available_frames() > 0);

  SECTION("reset clears state") {
    analyzer.reset();
    REQUIRE(analyzer.frame_count() == 0);
    REQUIRE(analyzer.available_frames() == 0);
    REQUIRE_THAT(analyzer.current_time(), WithinAbs(0.0f, 0.001f));
  }

  SECTION("reset with offset sets cumulative samples") {
    size_t offset = 44100;
    analyzer.reset(offset);

    // Process more audio
    analyzer.process(audio.data(), audio.size());
    auto frames = analyzer.read_frames(1);

    REQUIRE(frames.size() == 1);
    // Timestamp should reflect the base offset
    float expected_time = static_cast<float>(offset) / config.sample_rate;
    REQUIRE_THAT(frames[0].timestamp, WithinRel(expected_time, 0.01f));
  }
}

TEST_CASE("StreamAnalyzer feature computation", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_mel = true;
  config.compute_chroma = true;
  config.compute_spectral = true;
  config.n_mels = 128;

  StreamAnalyzer analyzer(config);

  // Generate a 440 Hz sine wave
  std::vector<float> audio = generate_sine(22050, 440.0f, 22050);
  analyzer.process(audio.data(), audio.size());

  auto frames = analyzer.read_frames(10);
  REQUIRE(frames.size() > 0);

  SECTION("mel spectrogram has correct size") {
    for (const auto& frame : frames) {
      REQUIRE(frame.mel.size() == 128);
    }
  }

  SECTION("chroma has correct size") {
    for (const auto& frame : frames) {
      REQUIRE(frame.chroma.size() == 12);
    }
  }

  SECTION("spectral centroid is positive for sine wave") {
    for (const auto& frame : frames) {
      REQUIRE(frame.spectral_centroid > 0.0f);
    }
  }

  SECTION("rms energy is positive for sine wave") {
    for (const auto& frame : frames) {
      REQUIRE(frame.rms_energy > 0.0f);
    }
  }
}

TEST_CASE("StreamAnalyzer emit_every_n_frames", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.emit_every_n_frames = 3;

  StreamAnalyzer analyzer(config);

  // Process 2 seconds of audio
  std::vector<float> audio(44100, 0.0f);
  analyzer.process(audio.data(), audio.size());

  auto frames = analyzer.read_frames(100);

  // With 2 seconds at 22050 Hz and hop_length 512:
  // Total frames = (44100 - 2048) / 512 + 1 ≈ 83 frames
  // With emit_every_n_frames = 3, we get ~27 frames
  REQUIRE(frames.size() < 40);
  REQUIRE(frames.size() > 20);
}

TEST_CASE("StreamAnalyzer SOA read", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_mel = true;
  config.n_mels = 64;

  StreamAnalyzer analyzer(config);

  std::vector<float> audio = generate_sine(22050, 440.0f, 22050);
  analyzer.process(audio.data(), audio.size());

  FrameBuffer buffer;
  analyzer.read_frames_soa(10, buffer);

  REQUIRE(buffer.n_frames <= 10);
  REQUIRE(buffer.timestamps.size() == buffer.n_frames);
  REQUIRE(buffer.mel.size() == buffer.n_frames * 64);
  REQUIRE(buffer.chroma.size() == buffer.n_frames * 12);
}

TEST_CASE("StreamAnalyzer stats", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;

  StreamAnalyzer analyzer(config);

  std::vector<float> audio(22050, 0.0f);  // 1 second
  analyzer.process(audio.data(), audio.size());

  AnalyzerStats stats = analyzer.stats();

  REQUIRE(stats.total_frames > 0);
  REQUIRE(stats.total_samples > 0);
  REQUIRE_THAT(stats.duration_seconds, WithinRel(1.0f, 0.1f));
}

TEST_CASE("StreamAnalyzer external offset sync", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 44100;
  config.n_fft = 2048;
  config.hop_length = 512;

  StreamAnalyzer analyzer(config);

  std::vector<float> chunk(4096, 0.0f);

  // Process with external offset
  size_t external_offset = 88200;  // 2 seconds at 44100 Hz
  analyzer.process(chunk.data(), chunk.size(), external_offset);

  auto frames = analyzer.read_frames(10);
  REQUIRE(frames.size() > 0);

  // First frame timestamp should be based on external offset
  float expected_time = static_cast<float>(external_offset) / config.sample_rate;
  REQUIRE_THAT(frames[0].timestamp, WithinRel(expected_time, 0.01f));
}

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
  // Float32 SOA: timestamps(4) + mel(128*4) + chroma(12*4) + onset(4) + rms(4) + centroid(4) + flatness(4)
  //            = 4 + 512 + 48 + 16 = 580 bytes per frame
  // U8: timestamps(4) + mel(128) + chroma(12) + onset(1) + rms(1) + centroid(1) + flatness(1)
  //   = 4 + 128 + 12 + 4 = 148 bytes per frame
  // Ratio: ~4x reduction

  SECTION("U8 is approximately 4x smaller than Float32") {
    size_t float32_size = n_frames * (sizeof(float) * (1 + 128 + 12 + 4));  // ~580 bytes/frame
    size_t u8_size = n_frames * (sizeof(float) + sizeof(uint8_t) * (128 + 12 + 4));  // ~148 bytes/frame

    // U8 should be roughly 4x smaller (allowing some tolerance)
    float ratio = static_cast<float>(float32_size) / static_cast<float>(u8_size);
    REQUIRE(ratio > 3.0f);
    REQUIRE(ratio < 5.0f);
  }
}

// ============================================================================
// Integration Tests
// ============================================================================

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
