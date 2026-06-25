/// @file stream_analyzer_core_test.cpp
/// @brief StreamAnalyzer core behavior tests.

#include <limits>

#include "stream_analyzer_test_helpers.h"
#include "util/exception.h"

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

TEST_CASE("StreamAnalyzer finalize flushes a partial tail frame", "[streaming]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.n_mels = 32;
  config.emit_every_n_frames = 3;

  StreamAnalyzer analyzer(config);

  std::vector<float> tail(600, 0.0f);
  analyzer.process(tail.data(), tail.size());
  REQUIRE(analyzer.available_frames() == 0);
  REQUIRE(analyzer.frame_count() == 0);

  analyzer.finalize();
  REQUIRE(analyzer.available_frames() == 1);
  REQUIRE(analyzer.frame_count() == 1);
  REQUIRE_THAT(analyzer.current_time(), WithinAbs(600.0f / 22050.0f, 1.0e-4f));

  analyzer.finalize();
  REQUIRE(analyzer.available_frames() == 1);
  REQUIRE(analyzer.frame_count() == 1);

  auto frames = analyzer.read_frames(2);
  REQUIRE(frames.size() == 1);
  REQUIRE(frames[0].frame_index == 0);
  REQUIRE_THAT(frames[0].timestamp, WithinAbs(0.0f, 1.0e-6f));

  analyzer.reset();
  analyzer.process(tail.data(), tail.size());
  analyzer.finalize();
  REQUIRE(analyzer.available_frames() == 1);
  REQUIRE(analyzer.frame_count() == 1);
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

TEST_CASE("StreamAnalyzer clamps degenerate sizing params", "[streaming][edge]") {
  // The C-ABI rejects these, but Node/WASM construct StreamAnalyzer directly.
  // magnitude_downsample == 0 would integer-divide n_bins() by zero when sizing
  // the per-frame magnitude vector; a non-positive hop_length would stall the
  // frame loop; emit_every_n_frames <= 0 breaks the emission throttle. The
  // constructor must clamp each to >= 1 and remain crash-free.
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 0;
  config.emit_every_n_frames = 0;
  config.magnitude_downsample = 0;
  config.compute_magnitude = true;  // off by default; enable to exercise sizing

  StreamAnalyzer analyzer(config);
  REQUIRE(analyzer.config().hop_length >= 1);
  REQUIRE(analyzer.config().emit_every_n_frames >= 1);
  REQUIRE(analyzer.config().magnitude_downsample >= 1);

  // Processing must not crash and the magnitude vector must be fully populated
  // (full n_bins, since downsample clamped to 1). Keep the buffer just past
  // n_fft: hop_length is clamped to 1 here, so every extra sample is one more
  // full FFT frame and a 1 s buffer would mean ~20k frames.
  std::vector<float> audio = generate_sine(2560, 440.0f, 22050);
  analyzer.process(audio.data(), audio.size());
  auto frames = analyzer.read_frames(4);
  REQUIRE_FALSE(frames.empty());
  REQUIRE(frames[0].magnitude.size() == static_cast<size_t>(config.n_bins()));
}

TEST_CASE("StreamAnalyzer rejects malformed config geometry", "[streaming][edge]") {
  // Relationship and positive-value checks that the flat C ABI enforces before
  // construction must also fire on direct C++/Node/WASM construction, otherwise
  // those surfaces silently produce garbage spectra instead of an error. These
  // are distinct from the degenerate sizing params (hop/emit/downsample) which
  // are deliberately clamped, not rejected, for crash-safety.
  auto base = []() {
    StreamConfig c;
    c.sample_rate = 22050;
    c.n_fft = 2048;
    c.hop_length = 512;
    return c;
  };

  SECTION("non-positive sample_rate") {
    StreamConfig c = base();
    c.sample_rate = 0;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
  }
  SECTION("non-positive n_fft") {
    StreamConfig c = base();
    c.n_fft = 0;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
  }
  SECTION("non-positive n_mels") {
    StreamConfig c = base();
    c.n_mels = 0;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
  }
  SECTION("hop_length exceeds n_fft") {
    StreamConfig c = base();
    c.hop_length = c.n_fft + 1;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
  }
  SECTION("negative fmin") {
    StreamConfig c = base();
    c.fmin = -1.0f;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
  }
  SECTION("fmax not greater than fmin") {
    StreamConfig c = base();
    c.fmin = 8000.0f;
    c.fmax = 4000.0f;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
  }
  SECTION("non-positive tuning_ref_hz") {
    StreamConfig c = base();
    c.tuning_ref_hz = 0.0f;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
  }
  SECTION("non-finite update interval") {
    StreamConfig c = base();
    c.bpm_update_interval_sec = std::numeric_limits<float>::infinity();
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
  }
  SECTION("valid config still constructs") { REQUIRE_NOTHROW(StreamAnalyzer(base())); }
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
