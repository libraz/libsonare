/// @file stream_analyzer_core_test.cpp
/// @brief StreamAnalyzer core behavior tests.

#include <atomic>
#include <limits>
#include <thread>

#include "core/resample.h"
#include "stream_analyzer_test_helpers.h"
#include "support/alloc_guard.h"
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

TEST_CASE("StreamAnalyzer finalize preserves short high-rate terminal impulses",
          "[streaming][resample][contract]") {
  constexpr int kInputSamples = 128;
  constexpr int kFftSize = 256;
  std::vector<float> input(kInputSamples, 0.0f);
  input.back() = 1.0f;

  for (const int sample_rate : {48000, 96000, 192000}) {
    CAPTURE(sample_rate);
    StreamConfig high_config;
    high_config.sample_rate = sample_rate;
    high_config.n_fft = kFftSize;
    high_config.hop_length = 64;
    high_config.n_mels = 8;
    high_config.compute_mel = false;
    high_config.compute_chroma = false;
    high_config.compute_onset = false;
    high_config.compute_spectral = false;

    StreamAnalyzer high_rate(high_config);
    high_rate.process(input.data(), input.size());
    REQUIRE(high_rate.available_frames() == 0);
    high_rate.finalize();
    const auto actual = high_rate.read_frames(1);
    REQUIRE(actual.size() == 1);
    REQUIRE(actual.front().rms_energy > 0.0f);

    const std::vector<float> offline = resample(input.data(), input.size(), sample_rate, 44100);
    REQUIRE_FALSE(offline.empty());
    StreamConfig reference_config = high_config;
    reference_config.sample_rate = 44100;
    StreamAnalyzer reference(reference_config);
    reference.process(offline.data(), offline.size());
    reference.finalize();
    const auto expected = reference.read_frames(1);
    REQUIRE(expected.size() == 1);
    REQUIRE_THAT(actual.front().rms_energy, WithinAbs(expected.front().rms_energy, 1.0e-7f));
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

TEST_CASE("StreamAnalyzer external offsets preserve buffered frame timestamps",
          "[streaming][offset]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.compute_magnitude = false;
  config.compute_mel = false;
  config.compute_chroma = false;
  config.compute_spectral = false;
  config.max_pending_frames = 64;

  std::vector<float> audio(8192, 0.0f);
  StreamAnalyzer reference(config);
  reference.process(audio.data(), audio.size());
  const auto expected = reference.read_frames(64);
  REQUIRE_FALSE(expected.empty());

  for (const size_t chunk_size : {size_t{128}, size_t{512}, size_t{4096}}) {
    CAPTURE(chunk_size);
    StreamAnalyzer analyzer(config);
    for (size_t offset = 0; offset < audio.size(); offset += chunk_size) {
      const size_t count = std::min(chunk_size, audio.size() - offset);
      analyzer.process(audio.data() + offset, count, offset);
    }
    const auto actual = analyzer.read_frames(64);
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
      CAPTURE(i);
      REQUIRE_THAT(actual[i].timestamp, WithinAbs(expected[i].timestamp, 1.0e-7f));
    }
  }
}

TEST_CASE("StreamAnalyzer features are invariant to chunk size", "[streaming][chunk]") {
  // The prior chunk-size test disabled every feature and only compared
  // timestamps. Enable the full feature set and assert element-wise equality
  // against a one-shot analysis so a windowing/accumulation regression at a
  // chunk boundary cannot pass unnoticed.
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.compute_magnitude = true;
  config.compute_mel = true;
  config.compute_chroma = true;
  config.compute_spectral = true;
  config.max_pending_frames = 256;

  // A deterministic non-trivial signal (two partials + slow amplitude drift) so
  // every feature has real structure to compare.
  std::vector<float> audio(12288, 0.0f);
  for (size_t i = 0; i < audio.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(config.sample_rate);
    audio[i] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * t) +
               0.3f * std::sin(2.0f * 3.14159265f * 440.0f * t) * (0.5f + 0.5f * t);
  }

  StreamAnalyzer reference(config);
  reference.process(audio.data(), audio.size());
  const auto expected = reference.read_frames(256);
  REQUIRE_FALSE(expected.empty());

  for (const size_t chunk_size : {size_t{100}, size_t{256}, size_t{512}, size_t{4096}}) {
    CAPTURE(chunk_size);
    StreamAnalyzer analyzer(config);
    for (size_t offset = 0; offset < audio.size(); offset += chunk_size) {
      const size_t count = std::min(chunk_size, audio.size() - offset);
      analyzer.process(audio.data() + offset, count);
    }
    const auto actual = analyzer.read_frames(256);
    REQUIRE(actual.size() == expected.size());
    for (size_t f = 0; f < actual.size(); ++f) {
      CAPTURE(f);
      REQUIRE_THAT(actual[f].spectral_centroid, WithinAbs(expected[f].spectral_centroid, 1.0e-4f));
      REQUIRE_THAT(actual[f].spectral_flatness, WithinAbs(expected[f].spectral_flatness, 1.0e-5f));
      REQUIRE_THAT(actual[f].rms_energy, WithinAbs(expected[f].rms_energy, 1.0e-5f));
      REQUIRE_THAT(actual[f].onset_strength, WithinAbs(expected[f].onset_strength, 1.0e-4f));
      REQUIRE(actual[f].mel.size() == expected[f].mel.size());
      for (size_t k = 0; k < actual[f].mel.size(); ++k) {
        REQUIRE_THAT(actual[f].mel[k], WithinAbs(expected[f].mel[k], 1.0e-4f));
      }
      REQUIRE(actual[f].chroma.size() == expected[f].chroma.size());
      for (size_t k = 0; k < actual[f].chroma.size(); ++k) {
        REQUIRE_THAT(actual[f].chroma[k], WithinAbs(expected[f].chroma[k], 1.0e-4f));
      }
    }
  }
}

TEST_CASE("StreamAnalyzer features are invariant to chunk size at 48kHz", "[streaming][chunk]") {
  // The 22050 Hz sibling above never exercises the resampler (input <=
  // kMaxDirectSampleRate). 48000 Hz is the dominant production input rate and
  // forces the persistent, phase-continuous resampler onto the hot path. Feed
  // the same signal one-shot and in several chunk sizes and assert element-wise
  // feature equality, so a cross-chunk resampler phase-continuity regression at
  // a chunk boundary cannot pass unnoticed.
  StreamConfig config;
  config.sample_rate = 48000;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.compute_magnitude = true;
  config.compute_mel = true;
  config.compute_chroma = true;
  config.compute_spectral = true;
  config.max_pending_frames = 256;

  // ~0.34 s of a deterministic non-trivial signal (two partials + slow
  // amplitude drift) so every feature has real structure to compare.
  std::vector<float> audio(16384, 0.0f);
  for (size_t i = 0; i < audio.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(config.sample_rate);
    audio[i] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * t) +
               0.3f * std::sin(2.0f * 3.14159265f * 440.0f * t) * (0.5f + 0.5f * t);
  }

  StreamAnalyzer reference(config);
  reference.process(audio.data(), audio.size());
  const auto expected = reference.read_frames(256);
  REQUIRE_FALSE(expected.empty());

  for (const size_t chunk_size : {size_t{100}, size_t{256}, size_t{512}, size_t{4096}}) {
    CAPTURE(chunk_size);
    StreamAnalyzer analyzer(config);
    for (size_t offset = 0; offset < audio.size(); offset += chunk_size) {
      const size_t count = std::min(chunk_size, audio.size() - offset);
      analyzer.process(audio.data() + offset, count);
    }
    const auto actual = analyzer.read_frames(256);
    REQUIRE(actual.size() == expected.size());
    for (size_t f = 0; f < actual.size(); ++f) {
      CAPTURE(f);
      REQUIRE_THAT(actual[f].spectral_centroid, WithinAbs(expected[f].spectral_centroid, 1.0e-4f));
      REQUIRE_THAT(actual[f].spectral_flatness, WithinAbs(expected[f].spectral_flatness, 1.0e-5f));
      REQUIRE_THAT(actual[f].rms_energy, WithinAbs(expected[f].rms_energy, 1.0e-5f));
      REQUIRE_THAT(actual[f].onset_strength, WithinAbs(expected[f].onset_strength, 1.0e-4f));
      REQUIRE(actual[f].mel.size() == expected[f].mel.size());
      for (size_t k = 0; k < actual[f].mel.size(); ++k) {
        REQUIRE_THAT(actual[f].mel[k], WithinAbs(expected[f].mel[k], 1.0e-4f));
      }
      REQUIRE(actual[f].chroma.size() == expected[f].chroma.size());
      for (size_t k = 0; k < actual[f].chroma.size(); ++k) {
        REQUIRE_THAT(actual[f].chroma[k], WithinAbs(expected[f].chroma[k], 1.0e-4f));
      }
    }
  }
}

TEST_CASE("StreamAnalyzer external-offset timestamps track the resampled timeline at 48kHz",
          "[streaming][offset]") {
  // At 48 kHz the analyzer resamples internally. The streaming resampler
  // compensates its whole-sample filter latency (r8brain CDSPResampler reports
  // getLatency() == 0), so an external sample offset applied on top of the
  // resampled frame positions stays a pure constant shift -- accurate to within
  // about one input sample rather than drifting by the filter length. Compare
  // the external-offset path against the offset-less (internal) path plus a
  // constant offset; a filter-length drift would blow the tolerance.
  StreamConfig config;
  config.sample_rate = 48000;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.compute_spectral = true;
  config.max_pending_frames = 256;

  std::vector<float> audio(16384, 0.0f);
  for (size_t i = 0; i < audio.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(config.sample_rate);
    audio[i] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * t) +
               0.3f * std::sin(2.0f * 3.14159265f * 440.0f * t);
  }

  StreamAnalyzer internal(config);
  internal.process(audio.data(), audio.size());
  const auto internal_frames = internal.read_frames(256);
  REQUIRE_FALSE(internal_frames.empty());

  constexpr size_t kOffset = 96000;  // 2 s at 48 kHz, in the external sample domain.
  StreamAnalyzer external(config);
  const size_t chunk = 4096;
  for (size_t off = 0; off < audio.size(); off += chunk) {
    const size_t count = std::min(chunk, audio.size() - off);
    external.process(audio.data() + off, count, kOffset + off);
  }
  const auto external_frames = external.read_frames(256);
  REQUIRE(external_frames.size() == internal_frames.size());

  const float offset_sec = static_cast<float>(kOffset) / static_cast<float>(config.sample_rate);
  const float sample_sec = 1.0f / static_cast<float>(config.sample_rate);
  for (size_t f = 0; f < external_frames.size(); ++f) {
    CAPTURE(f);
    // Timestamp is a constant offset of the internal timeline (no drift), and
    // the frame content is identical -- the offset only relabels timestamps.
    REQUIRE_THAT(external_frames[f].timestamp,
                 WithinAbs(internal_frames[f].timestamp + offset_sec, 2.0f * sample_sec));
    REQUIRE_THAT(external_frames[f].spectral_centroid,
                 WithinAbs(internal_frames[f].spectral_centroid, 1.0e-4f));
  }
}

TEST_CASE("StreamAnalyzer external offset discontinuities require reset", "[streaming][offset]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 2048;
  config.hop_length = 512;
  StreamAnalyzer analyzer(config);
  std::vector<float> chunk(128, 0.0f);

  analyzer.process(chunk.data(), chunk.size(), 1000);
  REQUIRE_THROWS_AS(analyzer.process(chunk.data(), chunk.size(), 1129), SonareException);
  REQUIRE_THROWS_AS(analyzer.process(chunk.data(), chunk.size()), SonareException);

  analyzer.reset(5000);
  REQUIRE_NOTHROW(analyzer.process(chunk.data(), chunk.size(), 5000));
  analyzer.reset();
  REQUIRE_NOTHROW(analyzer.process(chunk.data(), chunk.size()));
  REQUIRE_THROWS_AS(analyzer.process(chunk.data(), chunk.size(), 128), SonareException);
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

TEST_CASE("StreamAnalyzer rejects degenerate sizing params", "[streaming][edge]") {
  // The C-ABI rejects these, and Node/WASM construct StreamAnalyzer directly.
  // Rather than silently clamping (which would morph the request into a
  // different, denser analyzer), the constructor throws to stay consistent with
  // the C-ABI oracle. magnitude_downsample == 0 would integer-divide n_bins() by
  // zero; a non-positive hop_length would stall the frame loop;
  // emit_every_n_frames <= 0 breaks the emission throttle.
  auto make = [](int hop, int emit, int downsample) {
    StreamConfig config;
    config.sample_rate = 22050;
    config.n_fft = 2048;
    config.hop_length = hop;
    config.emit_every_n_frames = emit;
    config.magnitude_downsample = downsample;
    config.compute_magnitude = true;
    return StreamAnalyzer(config);
  };

  REQUIRE_THROWS_AS(make(0, 3, 1), SonareException);
  REQUIRE_THROWS_AS(make(512, 0, 1), SonareException);
  REQUIRE_THROWS_AS(make(512, 3, 0), SonareException);

  // A valid config still constructs and processes normally.
  StreamAnalyzer analyzer = make(512, 1, 1);
  std::vector<float> audio = generate_sine(8192, 440.0f, 22050);
  analyzer.process(audio.data(), audio.size());
  auto frames = analyzer.read_frames(16);
  REQUIRE_FALSE(frames.empty());
  REQUIRE(frames[0].magnitude.size() == static_cast<size_t>(analyzer.config().n_bins()));
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
  SECTION("zero progression cap") {
    StreamConfig c = base();
    c.max_progression_entries = 0;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
  }
  SECTION("valid config still constructs") { REQUIRE_NOTHROW(StreamAnalyzer(base())); }
}

TEST_CASE("StreamAnalyzer rejects non-finite runtime and quantization parameters",
          "[streaming][edge][numeric]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 512;
  config.hop_length = 128;
  config.n_mels = 24;
  StreamAnalyzer analyzer(config);

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (float invalid : {nan, inf, -inf}) {
    REQUIRE_THROWS_AS(analyzer.set_expected_duration(invalid), SonareException);
    REQUIRE_THROWS_AS(analyzer.set_normalization_gain(invalid), SonareException);
    REQUIRE_THROWS_AS(analyzer.set_tuning_ref_hz(invalid), SonareException);
  }

  QuantizedFrameBufferU8 u8;
  QuantizedFrameBufferI16 i16;
  QuantizeConfig quantize;
  quantize.mel_db_min = nan;
  REQUIRE_THROWS_AS(analyzer.read_frames_quantized_u8(0, u8, quantize), SonareException);
  quantize = {};
  quantize.mel_db_max = inf;
  REQUIRE_THROWS_AS(analyzer.read_frames_quantized_i16(0, i16, quantize), SonareException);
  quantize = {};
  quantize.onset_max = 0.0f;
  REQUIRE_THROWS_AS(analyzer.read_frames_quantized_u8(0, u8, quantize), SonareException);
  quantize = {};
  quantize.mel_db_min = quantize.mel_db_max;
  REQUIRE_THROWS_AS(analyzer.read_frames_quantized_u8(0, u8, quantize), SonareException);
  quantize = {};
  quantize.mel_db_min = 1.0f;
  quantize.mel_db_max = 0.0f;
  REQUIRE_THROWS_AS(analyzer.read_frames_quantized_i16(0, i16, quantize), SonareException);
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

TEST_CASE("StreamAnalyzer pending output is bounded with drop-newest telemetry",
          "[streaming][long]") {
  StreamConfig config;
  config.sample_rate = 8000;
  config.n_fft = 32;
  config.hop_length = 32;
  config.n_mels = 8;
  config.max_pending_frames = 4;
  StreamAnalyzer analyzer(config);

  // Accelerated long-consumer-stall simulation: hundreds of complete frames
  // arrive without one read, while retained memory stays at the configured cap.
  std::vector<float> audio(32 * 512, 0.0f);
  analyzer.process(audio.data(), audio.size());

  const AnalyzerStats stats = analyzer.stats();
  REQUIRE(analyzer.available_frames() == 4);
  REQUIRE(stats.pending_frames == 4);
  REQUIRE(stats.dropped_output_frames > 0);
  REQUIRE(stats.pending_frames + stats.dropped_output_frames ==
          static_cast<size_t>(stats.total_frames));

  const auto retained = analyzer.read_frames(4);
  REQUIRE(retained.size() == 4);
  for (size_t i = 0; i < retained.size(); ++i) {
    REQUIRE(retained[i].frame_index == static_cast<int>(i));
  }

  analyzer.reset();
  REQUIRE(analyzer.stats().pending_frames == 0);
  REQUIRE(analyzer.stats().dropped_output_frames == 0);
}

TEST_CASE("StreamAnalyzer publishes frames and stats to one concurrent consumer",
          "[streaming][concurrency]") {
  StreamConfig config;
  config.sample_rate = 8000;
  config.n_fft = 32;
  config.hop_length = 32;
  config.n_mels = 8;
  config.compute_magnitude = false;
  config.compute_mel = false;
  config.compute_chroma = false;
  config.compute_onset = false;
  config.compute_spectral = false;
  config.max_pending_frames = 8;
  config.max_progression_entries = 4;
  StreamAnalyzer analyzer(config);

  constexpr int kBlocks = 2000;
  std::array<float, 32> block{};
  std::atomic<bool> start{false};
  std::atomic<bool> producer_done{false};
  std::atomic<bool> valid{true};

  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int i = 0; i < kBlocks; ++i) analyzer.process(block.data(), block.size());
    producer_done.store(true, std::memory_order_release);
  });

  int last_frame_index = -1;
  int last_total_frames = 0;
  start.store(true, std::memory_order_release);
  while (!producer_done.load(std::memory_order_acquire) || analyzer.available_frames() > 0) {
    const AnalyzerStats snapshot = analyzer.stats();
    if (snapshot.total_frames < last_total_frames ||
        snapshot.pending_frames > config.max_pending_frames ||
        snapshot.estimate.used_frames > snapshot.total_frames) {
      valid.store(false, std::memory_order_relaxed);
    }
    last_total_frames = snapshot.total_frames;

    const auto frames = analyzer.read_frames(1);
    if (!frames.empty()) {
      if (frames.front().frame_index <= last_frame_index) {
        valid.store(false, std::memory_order_relaxed);
      }
      last_frame_index = frames.front().frame_index;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();

  const AnalyzerStats final_stats = analyzer.stats();
  REQUIRE(valid.load(std::memory_order_relaxed));
  REQUIRE(final_stats.total_frames == kBlocks);
  REQUIRE(final_stats.pending_frames == 0);
  REQUIRE(final_stats.dropped_output_frames < static_cast<size_t>(kBlocks));
}

TEST_CASE("StreamAnalyzer realtime process path remains allocation free across bounded histories",
          "[streaming][long][rt]") {
  StreamConfig config;
  config.sample_rate = 8000;
  config.n_fft = 256;
  config.hop_length = 128;
  config.n_mels = 16;
  config.max_pending_frames = 4;
  config.max_progression_entries = 4;
  config.key_update_interval_sec = 0.25f;
  config.bpm_update_interval_sec = 0.25f;
  StreamAnalyzer analyzer(config);
  StreamConfig reference_config = config;
  reference_config.max_progression_entries = 4096;
  StreamAnalyzer reference(reference_config);

  constexpr int kSeconds = 60;
  constexpr size_t kBlock = 128;
  std::vector<float> audio(static_cast<size_t>(config.sample_rate * kSeconds), 0.0f);
  const std::array<std::array<float, 3>, 2> chords = {
      std::array<float, 3>{261.63f, 329.63f, 392.0f},
      std::array<float, 3>{392.0f, 493.88f, 587.33f},
  };
  for (size_t i = 0; i < audio.size(); ++i) {
    const size_t half_second = static_cast<size_t>(config.sample_rate / 2);
    const auto& chord = chords[(i / half_second) % chords.size()];
    const float t = static_cast<float>(i) / static_cast<float>(config.sample_rate);
    for (float frequency : chord) {
      audio[i] += 0.15f * std::sin(2.0f * sonare::constants::kPi * frequency * t);
    }
    if (i % half_second < 8) {
      audio[i] += 0.8f * (1.0f - static_cast<float>(i % half_second) / 8.0f);
    }
  }

  size_t allocations = 0;
  for (size_t offset = 0; offset < audio.size(); offset += kBlock) {
    const size_t count = std::min(kBlock, audio.size() - offset);
    size_t block_allocations = 0;
    {
      sonare::test::AllocationGuard guard;
      analyzer.process(audio.data() + offset, count);
      block_allocations = guard.count();
    }
    CAPTURE(offset, analyzer.frame_count());
    REQUIRE(block_allocations == 0);
    allocations += block_allocations;
    analyzer.read_frames(16);
    reference.process(audio.data() + offset, count);
    reference.read_frames(16);
  }

  const AnalyzerStats stats = analyzer.stats();
  const AnalyzerStats reference_stats = reference.stats();
  REQUIRE(allocations == 0);
  REQUIRE(analyzer.full_chroma_history_size_for_test() ==
          StreamAnalyzer::full_chroma_history_cap_for_test());
  REQUIRE(stats.estimate.chord_progression.size() <= config.max_progression_entries);
  REQUIRE(stats.estimate.bar_chord_progression.size() <= config.max_progression_entries);
  REQUIRE(stats.dropped_chord_progression_entries > 0);
  REQUIRE(stats.dropped_bar_progression_entries > 0);
  REQUIRE(stats.estimate.chord_progression.size() + stats.dropped_chord_progression_entries ==
          reference_stats.estimate.chord_progression.size());
  REQUIRE(stats.estimate.bar_chord_progression.size() + stats.dropped_bar_progression_entries ==
          reference_stats.estimate.bar_chord_progression.size());

  const size_t chord_suffix =
      reference_stats.estimate.chord_progression.size() - stats.estimate.chord_progression.size();
  for (size_t i = 0; i < stats.estimate.chord_progression.size(); ++i) {
    const auto& retained = stats.estimate.chord_progression[i];
    const auto& expected = reference_stats.estimate.chord_progression[chord_suffix + i];
    REQUIRE(retained.root == expected.root);
    REQUIRE(retained.quality == expected.quality);
    REQUIRE(retained.start_time == expected.start_time);
  }

  const size_t bar_suffix = reference_stats.estimate.bar_chord_progression.size() -
                            stats.estimate.bar_chord_progression.size();
  for (size_t i = 0; i < stats.estimate.bar_chord_progression.size(); ++i) {
    const auto& retained = stats.estimate.bar_chord_progression[i];
    const auto& expected = reference_stats.estimate.bar_chord_progression[bar_suffix + i];
    REQUIRE(retained.bar_index == expected.bar_index);
    REQUIRE(retained.root == expected.root);
    REQUIRE(retained.quality == expected.quality);
  }
}

TEST_CASE("StreamAnalyzer 24-hour-equivalent callback count stays bounded and allocation free",
          "[streaming][long][.][slow][rt]") {
  StreamConfig config;
  config.sample_rate = 64;
  config.n_fft = 32;
  config.hop_length = 32;
  config.n_mels = 8;
  config.compute_mel = false;
  config.compute_chroma = false;
  config.compute_onset = false;
  config.compute_spectral = false;
  config.max_pending_frames = 4;
  config.max_progression_entries = 4;
  StreamAnalyzer analyzer(config);

  constexpr size_t kCallbacks = 24u * 60u * 60u * 2u;
  std::array<float, 32> block{};
  size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    for (size_t i = 0; i < kCallbacks; ++i) {
      analyzer.process(block.data(), block.size());
    }
    allocations = guard.count();
  }

  const AnalyzerStats stats = analyzer.stats();
  REQUIRE(allocations == 0);
  REQUIRE(stats.total_frames == static_cast<int>(kCallbacks));
  REQUIRE(stats.pending_frames == config.max_pending_frames);
  REQUIRE(stats.pending_frames + stats.dropped_output_frames == kCallbacks);
  REQUIRE(stats.estimate.chord_progression.empty());
  REQUIRE(stats.estimate.bar_chord_progression.empty());
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
