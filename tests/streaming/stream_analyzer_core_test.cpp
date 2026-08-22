/// @file stream_analyzer_core_test.cpp
/// @brief StreamAnalyzer core behavior tests.

#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>

#include "analysis/progression_patterns.h"
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
  SECTION("tuning_ref_hz outside the accepted range") {
    // Construction accepts exactly what set_tuning_ref_hz accepts. Were create
    // to take a wider range, the same host setting would bin the chromagram one
    // way through the constructor and another through the live setter.
    StreamConfig c = base();
    c.tuning_ref_hz = 100.0f;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
    c.tuning_ref_hz = 1000.0f;
    REQUIRE_THROWS_AS(StreamAnalyzer(c), SonareException);
    c.tuning_ref_hz = kMinTuningRefHz;
    REQUIRE_NOTHROW(StreamAnalyzer(c));
    c.tuning_ref_hz = kMaxTuningRefHz;
    REQUIRE_NOTHROW(StreamAnalyzer(c));
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

TEST_CASE("tuning_ref_hz has one accepted range across create and the live setter",
          "[streaming][numeric]") {
  // One parameter, one accepted range, one normalization rule. The live setter
  // rejects out-of-range values instead of clamping them: clamping would turn a
  // 1000 Hz request into 880 Hz here while the constructor refused the same
  // number outright, so the chromagram would depend on which entry point the
  // host happened to use.
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.n_mels = 24;
  config.compute_chroma = true;
  config.max_pending_frames = 256;

  StreamAnalyzer bounds_probe(config);
  REQUIRE_THROWS_AS(bounds_probe.set_tuning_ref_hz(100.0f), SonareException);
  REQUIRE_THROWS_AS(bounds_probe.set_tuning_ref_hz(1000.0f), SonareException);
  REQUIRE_NOTHROW(bounds_probe.set_tuning_ref_hz(kMinTuningRefHz));
  REQUIRE_NOTHROW(bounds_probe.set_tuning_ref_hz(kMaxTuningRefHz));

  // An accepted value bins the chromagram identically through either path.
  constexpr float kRefHz = 466.16f;  // A4 a semitone sharp
  std::vector<float> audio(12288, 0.0f);
  for (size_t i = 0; i < audio.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(config.sample_rate);
    audio[i] = 0.5f * std::sin(kTwoPi * 233.08f * t) + 0.3f * std::sin(kTwoPi * kRefHz * t);
  }

  StreamConfig at_create = config;
  at_create.tuning_ref_hz = kRefHz;
  StreamAnalyzer created(at_create);
  created.process(audio.data(), audio.size());
  const auto expected = created.read_frames(256);
  REQUIRE_FALSE(expected.empty());

  StreamAnalyzer live(config);
  live.set_tuning_ref_hz(kRefHz);
  live.process(audio.data(), audio.size());
  const auto actual = live.read_frames(256);
  REQUIRE(actual.size() == expected.size());
  for (size_t f = 0; f < actual.size(); ++f) {
    CAPTURE(f);
    REQUIRE(actual[f].chroma.size() == expected[f].chroma.size());
    for (size_t k = 0; k < actual[f].chroma.size(); ++k) {
      REQUIRE_THAT(actual[f].chroma[k], WithinAbs(expected[f].chroma[k], 1.0e-5f));
    }
  }
}

TEST_CASE("normalization gain is refused out of range, never substituted", "[streaming][numeric]") {
  // The sibling setter eight lines below this one in the source rejects rather
  // than clamps, and says why. This one clamped: with no getter for the
  // effective gain, a request outside the range was replaced by an endpoint and
  // the analyzer ran on input the caller never asked for, silently.
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.n_mels = 24;

  StreamAnalyzer analyzer(config);
  REQUIRE_NOTHROW(analyzer.set_normalization_gain(kMinNormalizationGain));
  REQUIRE_NOTHROW(analyzer.set_normalization_gain(kMaxNormalizationGain));
  REQUIRE_NOTHROW(analyzer.set_normalization_gain(1.0f));

  // 3e-4 is what the documented recipe (target / measured) asks for on an
  // integer-scaled buffer. Clamped to 0.01 it left the analysis ~30 dB hot.
  for (const float rejected :
       {3.0e-4f, 0.0f, -2.0f, 1.0e4f, kMinNormalizationGain * 0.5f, kMaxNormalizationGain * 2.0f}) {
    CAPTURE(rejected);
    REQUIRE_THROWS_AS(analyzer.set_normalization_gain(rejected), SonareException);
    try {
      analyzer.set_normalization_gain(rejected);
      FAIL("expected an out-of-range normalization gain to be refused");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
    }
  }

  // A refused request leaves the previously accepted gain in place, and an
  // accepted one is applied exactly: half the gain is half the analyzed
  // amplitude, which the rejected 3e-4 would not have produced after a clamp.
  std::vector<float> audio(8192, 0.0f);
  for (size_t i = 0; i < audio.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(config.sample_rate);
    audio[i] = 0.5f * std::sin(kTwoPi * 440.0f * t);
  }

  const auto rms_with_gain = [&](float gain) {
    StreamAnalyzer scoped(config);
    scoped.set_normalization_gain(gain);
    REQUIRE_THROWS_AS(scoped.set_normalization_gain(1.0e6f), SonareException);
    scoped.process(audio.data(), audio.size());
    const auto frames = scoped.read_frames(8);
    REQUIRE_FALSE(frames.empty());
    return frames.front().rms_energy;
  };

  const float unity = rms_with_gain(1.0f);
  REQUIRE(unity > 0.0f);
  REQUIRE_THAT(rms_with_gain(0.5f), WithinRel(unity * 0.5f, 1.0e-4f));
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

// --- Progression path allocation guards --------------------------------------
//
// The 60-second guard above proves the realtime path allocates nothing for the
// audio it happens to feed. That is a weaker claim than it looks for the
// known-pattern correction: corrections are recorded only when a voted chord is
// confusable with the expected one, and whether any input reaches that branch is
// decided by the pattern table, not by the code. The cases below drive the
// branch on purpose and assert the correction actually landed, so a fixture that
// stopped reaching it would fail rather than pass quietly.

namespace {

/// Voted pattern matching `royalRoad` in C at three positions, with C major
/// where A minor is expected. C major and A minor share C and E, so the
/// position is confusable rather than exact -- the input shape that makes the
/// correction pass record a correction at all.
std::vector<BarChord> confusable_voted_pattern() {
  std::vector<BarChord> voted(4);
  const int roots[] = {5, 7, 4, 0};      // F, G, E, C
  const int qualities[] = {0, 0, 1, 0};  // major, major, minor, major
  for (size_t i = 0; i < voted.size(); ++i) {
    voted[i].bar_index = static_cast<int>(i);
    voted[i].root = roots[i];
    voted[i].quality = qualities[i];
    voted[i].confidence = 0.9f;
  }
  return voted;
}

size_t longest_pattern_name_length() {
  size_t longest = 0;
  for (const auto& pattern : sonare::known_progression_patterns()) {
    longest = std::max(longest, pattern.name.size());
  }
  return longest;
}

}  // namespace

TEST_CASE("StreamAnalyzer known-pattern correction is allocation free on the correcting branch",
          "[streaming][rt]") {
  StreamConfig config;
  config.sample_rate = 8000;
  config.n_fft = 256;
  config.hop_length = 128;
  config.n_mels = 16;
  config.max_progression_entries = 8;
  StreamAnalyzer analyzer(config);

  analyzer.seed_voted_pattern_for_test(confusable_voted_pattern(), 0);

  size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    analyzer.run_pattern_correction_for_test();
    allocations = guard.count();
  }

  // The fixture reached the correcting branch: position 3 was rewritten from
  // the voted C major to royalRoad's expected A minor. Without this the
  // allocation count below would be measuring a path that never ran.
  const auto& estimate = analyzer.progressive_estimate_for_test();
  REQUIRE(estimate.voted_pattern.size() == 4);
  REQUIRE(estimate.voted_pattern[3].root == 9);
  REQUIRE(estimate.voted_pattern[3].quality == 1);
  REQUIRE(estimate.detected_pattern_name == "royalRoad");
  // The three exact positions are left alone.
  REQUIRE(estimate.voted_pattern[0].root == 5);
  REQUIRE(estimate.voted_pattern[1].root == 7);
  REQUIRE(estimate.voted_pattern[2].root == 4);

  REQUIRE(allocations == 0);
}

TEST_CASE("StreamAnalyzer pattern scoring pass is allocation free", "[streaming][rt]") {
  StreamConfig config;
  config.sample_rate = 8000;
  config.n_fft = 256;
  config.hop_length = 128;
  config.n_mels = 16;
  config.max_progression_entries = 16;
  StreamAnalyzer analyzer(config);

  std::vector<BarChord> bars(8);
  for (size_t i = 0; i < bars.size(); ++i) {
    bars[i].bar_index = static_cast<int>(i);
    bars[i].root = (i % 2 == 0) ? 0 : 7;
    bars[i].quality = 0;
    bars[i].confidence = 0.8f;
  }
  analyzer.seed_bar_progression_for_test(bars);

  // Two passes: the first fills the score entries, the second must reuse them.
  for (int pass = 0; pass < 2; ++pass) {
    CAPTURE(pass);
    size_t allocations = 0;
    {
      sonare::test::AllocationGuard guard;
      analyzer.detect_progression_pattern_for_test();
      allocations = guard.count();
    }
    REQUIRE(allocations == 0);
    REQUIRE(analyzer.progressive_estimate_for_test().all_pattern_scores.size() ==
            sonare::known_progression_patterns().size());
  }
}

TEST_CASE("StreamAnalyzer pattern score name buffers are reserved from the pattern table",
          "[streaming][rt]") {
  // Inert while every pattern name fits a std::string's small buffer, which is
  // the situation today. It becomes the load-bearing check the moment a longer
  // name is added -- which is exactly when the scoring pass would otherwise
  // start allocating on the audio thread once per pattern per call.
  StreamConfig config;
  config.sample_rate = 8000;
  config.n_fft = 256;
  config.hop_length = 128;
  config.n_mels = 16;
  StreamAnalyzer analyzer(config);

  const size_t longest = longest_pattern_name_length();
  const auto& scores = analyzer.progressive_estimate_for_test().all_pattern_scores;
  REQUIRE(scores.size() == sonare::known_progression_patterns().size());
  for (const auto& score : scores) {
    REQUIRE(score.first.capacity() >= longest);
  }
}

// --- Bar progression, pattern position and published time fields --------------
//
// The bar path publishes four things a host synchronizes against: the bar
// number, the bar start time, the voted pattern position and the pattern name.
// Each one below is pinned against the quantity it is documented to be, not
// against the array index or the frame grid that happen to coincide with it in
// an uninterrupted stream.

namespace {

/// royalRoad in C: IV-V-iii-vi as absolute (root, quality) pairs.
constexpr std::array<int, 4> kRoyalRoadRoots = {5, 7, 4, 9};
constexpr std::array<int, 4> kRoyalRoadQualities = {0, 0, 1, 1};

/// Bars spelling royalRoad in C from `first_bar_index` for `count` bars, with
/// the bar whose number is `skipped_bar_index` absent from the array. A bar is
/// only recorded when its frames produced a confident chord, so a drum break or
/// a silent bar leaves exactly this shape: a hole in the numbering while the
/// array stays contiguous.
std::vector<BarChord> royal_road_bars(int first_bar_index, int count, int skipped_bar_index) {
  std::vector<BarChord> bars;
  for (int i = 0; i < count; ++i) {
    const int bar_index = first_bar_index + i;
    if (bar_index == skipped_bar_index) continue;
    BarChord bar;
    bar.bar_index = bar_index;
    const size_t pos = static_cast<size_t>(bar_index % 4);
    bar.root = kRoyalRoadRoots[pos];
    bar.quality = kRoyalRoadQualities[pos];
    bar.start_time = static_cast<float>(bar_index) * 2.0f;
    bar.confidence = 0.9f;
    bars.push_back(bar);
  }
  return bars;
}

StreamConfig bar_test_config() {
  StreamConfig config;
  config.sample_rate = 8000;
  config.n_fft = 256;
  config.hop_length = 128;
  config.n_mels = 16;
  return config;
}

}  // namespace

TEST_CASE("StreamAnalyzer votes pattern positions by bar number, not array position",
          "[streaming][bar]") {
  StreamConfig config = bar_test_config();

  SECTION("a bar with no confident chord does not rotate the pattern") {
    StreamAnalyzer analyzer(config);
    analyzer.seed_key_for_test(0);
    // Bars 0..11 with bar 4 missing: eleven entries, so every array position
    // from index 4 on belongs to a different pattern position than its offset.
    analyzer.seed_bar_progression_for_test(royal_road_bars(0, 12, 4));
    analyzer.compute_voted_pattern_for_test(4);

    const auto& voted = analyzer.progressive_estimate_for_test().voted_pattern;
    REQUIRE(voted.size() == 4);
    for (size_t pos = 0; pos < voted.size(); ++pos) {
      CAPTURE(pos);
      REQUIRE(voted[pos].root == kRoyalRoadRoots[pos]);
      REQUIRE(voted[pos].quality == kRoyalRoadQualities[pos]);
    }
  }

  SECTION("dropping the oldest bars does not rotate the pattern") {
    StreamAnalyzer analyzer(config);
    analyzer.seed_key_for_test(0);
    // The state after the history cap dropped bars 0..5: the array now starts
    // at bar 6, which is pattern position 2.
    analyzer.seed_bar_progression_for_test(royal_road_bars(6, 12, -1));
    analyzer.compute_voted_pattern_for_test(4);

    const auto& voted = analyzer.progressive_estimate_for_test().voted_pattern;
    REQUIRE(voted.size() == 4);
    for (size_t pos = 0; pos < voted.size(); ++pos) {
      CAPTURE(pos);
      REQUIRE(voted[pos].root == kRoyalRoadRoots[pos]);
      REQUIRE(voted[pos].quality == kRoyalRoadQualities[pos]);
    }
  }

  SECTION("pattern scoring aligns on the bar number too") {
    StreamAnalyzer analyzer(config);
    analyzer.seed_key_for_test(0);
    analyzer.seed_bar_progression_for_test(royal_road_bars(0, 12, 4));
    analyzer.detect_progression_pattern_for_test();

    // Every recorded bar matches royalRoad exactly once positions come from the
    // bar number, so the score is a full match. Aligned on array offsets, the
    // eight bars after the hole match a rotation instead.
    const auto& estimate = analyzer.progressive_estimate_for_test();
    REQUIRE(estimate.detected_pattern_name == "royalRoad");
    REQUIRE(estimate.detected_pattern_score > 0.99f);
  }
}

TEST_CASE("StreamAnalyzer bar start times share the stream timeline", "[streaming][bar]") {
  StreamConfig config = bar_test_config();
  config.key_update_interval_sec = 0.25f;
  config.bpm_update_interval_sec = 0.25f;
  StreamAnalyzer analyzer(config);

  constexpr int kSeconds = 30;
  const size_t offset_samples = static_cast<size_t>(config.sample_rate) * 7;
  const float base_seconds = static_cast<float>(offset_samples) / config.sample_rate;
  const std::vector<float> audio =
      generate_chord_click_bed(config.sample_rate * kSeconds, config.sample_rate, 120.0f);

  float first_frame_time = std::numeric_limits<float>::max();
  float last_frame_time = 0.0f;
  constexpr size_t kBlock = 1024;
  for (size_t position = 0; position < audio.size(); position += kBlock) {
    const size_t count = std::min(kBlock, audio.size() - position);
    analyzer.process(audio.data() + position, count, offset_samples + position);
    for (const auto& frame : analyzer.read_frames(64)) {
      first_frame_time = std::min(first_frame_time, frame.timestamp);
      last_frame_time = std::max(last_frame_time, frame.timestamp);
    }
  }

  const AnalyzerStats stats = analyzer.stats();
  const auto& bars = stats.estimate.bar_chord_progression;
  CAPTURE(base_seconds, first_frame_time, last_frame_time, bars.size());
  REQUIRE(bars.size() >= 4);
  REQUIRE_THAT(first_frame_time, WithinAbs(base_seconds, 1.0e-3f));

  // Every bar start belongs to the timeline the caller anchored, the same one
  // StreamFrame::timestamp and chord_progression[].start_time are on. The
  // retroactive bars measured their start from a frame count, which knows
  // nothing about the anchor unless the anchor is added back.
  for (const auto& bar : bars) {
    CAPTURE(bar.bar_index, bar.start_time);
    REQUIRE(bar.start_time >= base_seconds - 1.0e-3f);
    REQUIRE(bar.start_time <= last_frame_time + stats.estimate.bar_duration + 1.0e-3f);
  }
  for (const auto& change : stats.estimate.chord_progression) {
    REQUIRE(change.start_time >= base_seconds - 1.0e-3f);
  }
}

TEST_CASE("StreamAnalyzer bar starts advance by the bar duration, not the frame grid",
          "[streaming][bar][long]") {
  StreamConfig config = bar_test_config();
  config.key_update_interval_sec = 0.25f;
  config.bpm_update_interval_sec = 0.25f;

  // A bar boundary is detected on the first analysis frame at or past it, so it
  // always overshoots by up to one hop (16 ms here) and never undershoots.
  // Re-anchoring the next bar to that frame folds the overshoot into the clock
  // once per bar; advancing the phase does not. The published spacing is
  // therefore the discriminator: it must stay inside the range of bar durations
  // the analyzer actually held, whether or not the tempo moved.
  // The tolerance sits between the two error scales it has to separate: the
  // measured worst deviation is 1.6e-6 s (float resolution at a 45-120 s
  // timeline, where one ulp is ~8e-6 s), while re-anchoring costs a mean half
  // hop, 8e-3 s, per bar. 1e-4 s is ~12 ulp of headroom above the first and ~80x
  // below the second. A much longer fixture would need it raised.
  const float hop_seconds =
      static_cast<float>(config.hop_length) / static_cast<float>(config.sample_rate);
  constexpr float kSpacingTolerance = 1.0e-4f;
  REQUIRE(kSpacingTolerance < hop_seconds / 8.0f);

  const auto run = [&](const std::vector<float>& audio) {
    StreamAnalyzer analyzer(config);
    float min_bar_duration = std::numeric_limits<float>::max();
    float max_bar_duration = 0.0f;
    constexpr size_t kBlock = 1024;
    for (size_t position = 0; position < audio.size(); position += kBlock) {
      const size_t count = std::min(kBlock, audio.size() - position);
      analyzer.process(audio.data() + position, count);
      analyzer.read_frames(64);
      const float bar_duration = analyzer.stats().estimate.bar_duration;
      if (bar_duration > 0.0f) {
        min_bar_duration = std::min(min_bar_duration, bar_duration);
        max_bar_duration = std::max(max_bar_duration, bar_duration);
      }
    }

    const auto bars = analyzer.stats().estimate.bar_chord_progression;
    // Measured: 22 bars at a steady tempo, 32 across the tempo change. The
    // floor is a fixture guard -- if a change stops driving the bar path this
    // far, the spacing assertions below stop meaning anything.
    CAPTURE(bars.size(), min_bar_duration, max_bar_duration);
    REQUIRE(bars.size() >= 16);
    REQUIRE(max_bar_duration > 0.0f);
    for (size_t i = 1; i < bars.size(); ++i) {
      const int bar_span = bars[i].bar_index - bars[i - 1].bar_index;
      REQUIRE(bar_span > 0);
      const float spacing =
          (bars[i].start_time - bars[i - 1].start_time) / static_cast<float>(bar_span);
      CAPTURE(i, bars[i].bar_index, spacing);
      REQUIRE(spacing >= min_bar_duration - kSpacingTolerance);
      REQUIRE(spacing <= max_bar_duration + kSpacingTolerance);
    }
  };

  SECTION("steady tempo") {
    run(generate_chord_click_bed(config.sample_rate * 45, config.sample_rate, 120.0f));
  }

  SECTION("tempo change mid-stream") {
    const int half = config.sample_rate * 30;
    std::vector<float> audio = generate_chord_click_bed(half, config.sample_rate, 120.0f);
    const std::vector<float> faster = generate_chord_click_bed(half, config.sample_rate, 150.0f);
    audio.insert(audio.end(), faster.begin(), faster.end());
    run(audio);
  }
}

TEST_CASE("StreamAnalyzer reports an estimate update once per change", "[streaming][contract]") {
  StreamConfig config = bar_test_config();
  config.key_update_interval_sec = 0.3f;
  // Far beyond the fixture, so every reported update is the key's and the count
  // is not the sum of two independent schedules.
  config.bpm_update_interval_sec = 1000.0f;

  const std::vector<float> audio =
      generate_sine(config.sample_rate * 2, 440.0f, config.sample_rate);

  const auto count_updated_snapshots = [&](size_t block) {
    StreamAnalyzer analyzer(config);
    int updates = 0;
    for (size_t position = 0; position < audio.size(); position += block) {
      const size_t count = std::min(block, audio.size() - position);
      analyzer.process(audio.data() + position, count);
      analyzer.read_frames(64);
      if (analyzer.stats().estimate.updated) ++updates;
    }
    return updates;
  };

  SECTION("an update mid-chunk is not lost") {
    // One frame per call sees every update by construction; larger chunks put
    // most updates on a frame that is not the chunk's last, which is where a
    // per-frame flag loses them.
    const int per_frame = count_updated_snapshots(static_cast<size_t>(config.hop_length));
    const int eight_frames = count_updated_snapshots(static_cast<size_t>(config.hop_length) * 8);
    const int sixteen_frames = count_updated_snapshots(static_cast<size_t>(config.hop_length) * 16);
    // 2.0 s of audio re-estimates the key every 0.3 s of stream time, which is
    // six updates (at 0.304 .. 1.824 s; the seventh would fall past the last
    // frame at 1.968 s). Measured: 6, 6, 6.
    CAPTURE(per_frame, eight_frames, sixteen_frames);
    REQUIRE(per_frame == 6);
    REQUIRE(eight_frames == per_frame);
    REQUIRE(sixteen_frames == per_frame);
  }

  SECTION("a call that produces no frame does not repeat the flag") {
    StreamAnalyzer analyzer(config);
    bool saw_update = false;
    for (size_t position = 0; position < audio.size() && !saw_update;
         position += static_cast<size_t>(config.hop_length)) {
      const size_t count =
          std::min(static_cast<size_t>(config.hop_length), audio.size() - position);
      analyzer.process(audio.data() + position, count);
      analyzer.read_frames(64);
      saw_update = analyzer.stats().estimate.updated;
    }
    REQUIRE(saw_update);

    analyzer.process(nullptr, 0);
    REQUIRE_FALSE(analyzer.stats().estimate.updated);
  }
}

TEST_CASE("count_shared_notes spells every quality it can be handed", "[streaming][chord]") {
  using sonare::streaming_detail::are_chords_confusable;
  using sonare::streaming_detail::count_shared_notes;

  constexpr int kMajor = static_cast<int>(ChordQuality::Major);
  constexpr int kMinor = static_cast<int>(ChordQuality::Minor);
  constexpr int kDim = static_cast<int>(ChordQuality::Diminished);
  constexpr int kAug = static_cast<int>(ChordQuality::Augmented);
  constexpr int kDim7 = static_cast<int>(ChordQuality::Dim7);

  // C dim is {C, Eb, Gb} and shares only its root with C major. Treating every
  // non-minor quality as a major triad made it identical to C major.
  REQUIRE(count_shared_notes(0, kDim, 0, kMajor) == 1);
  REQUIRE_FALSE(are_chords_confusable(0, kDim, 0, kMajor));

  // C aug is {C, E, G#}: the root and the major third, but not the fifth.
  REQUIRE(count_shared_notes(0, kAug, 0, kMajor) == 2);
  REQUIRE(count_shared_notes(0, kDim7, 0, kDim) == 3);

  // The relations the correction pass relies on are unchanged.
  REQUIRE(count_shared_notes(0, kMajor, 0, kMajor) == 3);
  REQUIRE(count_shared_notes(0, kMajor, 0, kMinor) == 2);
  REQUIRE(count_shared_notes(0, kMajor, 9, kMinor) == 2);
  REQUIRE(are_chords_confusable(0, kMajor, 9, kMinor));

  // Out-of-range inputs have no spelling and share nothing.
  REQUIRE(count_shared_notes(-1, kMajor, 0, kMajor) == 0);
  REQUIRE(count_shared_notes(0, kMajor, 0, 999) == 0);
}

TEST_CASE("StreamAnalyzer keeps a diminished bar out of a major-triad pattern",
          "[streaming][chord]") {
  StreamConfig config = bar_test_config();
  StreamAnalyzer analyzer(config);

  // royalRoad in C expects F major at position 0. The voted chord there is F
  // diminished, which shares one note with it -- not a confusion, so the
  // position must be left as detected and the pattern must not be claimed on
  // the strength of it.
  std::vector<BarChord> voted(4);
  const std::array<int, 4> roots = {5, 7, 4, 9};
  const std::array<int, 4> qualities = {static_cast<int>(ChordQuality::Diminished), 0, 1, 1};
  for (size_t i = 0; i < voted.size(); ++i) {
    voted[i].bar_index = static_cast<int>(i);
    voted[i].root = roots[i];
    voted[i].quality = qualities[i];
    voted[i].confidence = 0.9f;
  }
  analyzer.seed_voted_pattern_for_test(voted, 0);
  analyzer.run_pattern_correction_for_test();

  const auto& estimate = analyzer.progressive_estimate_for_test();
  REQUIRE(estimate.voted_pattern[0].root == 5);
  REQUIRE(estimate.voted_pattern[0].quality == static_cast<int>(ChordQuality::Diminished));
  REQUIRE(estimate.detected_pattern_name.empty());
}

TEST_CASE("StreamAnalyzer publishes no chord derived from an unknown key", "[streaming][chord]") {
  StreamConfig config = bar_test_config();

  const auto valid_entry = [](const BarChord& chord) {
    return chord.root >= -1 && chord.root < 12 && chord.quality >= 0 &&
           chord.quality < sonare::kNumChordQualities;
  };

  SECTION("the correction pass is inert while the key is unknown") {
    StreamAnalyzer analyzer(config);
    // Rooted so that basic145's degrees, resolved against the -1 sentinel,
    // land a confusable match on the tonic position -- the shape that turned
    // the sentinel into a published chord index.
    std::vector<BarChord> voted(4);
    const std::array<int, 4> roots = {11, 4, 6, 11};
    for (size_t i = 0; i < voted.size(); ++i) {
      voted[i].bar_index = static_cast<int>(i);
      voted[i].root = roots[i];
      voted[i].quality = 0;
      voted[i].confidence = 0.9f;
    }
    analyzer.seed_voted_pattern_for_test(voted, -1);
    analyzer.seed_bar_progression_for_test(royal_road_bars(0, 8, -1));
    analyzer.run_pattern_correction_for_test();

    const auto& estimate = analyzer.progressive_estimate_for_test();
    for (size_t i = 0; i < estimate.voted_pattern.size(); ++i) {
      CAPTURE(i, estimate.voted_pattern[i].root, estimate.voted_pattern[i].quality);
      REQUIRE(valid_entry(estimate.voted_pattern[i]));
      REQUIRE(estimate.voted_pattern[i].root == roots[i]);
    }
    REQUIRE(estimate.detected_pattern_name.empty());
  }

  SECTION("a BPM-before-key stream publishes only valid pattern entries") {
    // The key re-estimates on its own schedule; pushing it past the fixture
    // while the BPM keeps updating reproduces the real ordering in which bar
    // tracking and pattern detection run before any key exists.
    config.bpm_update_interval_sec = 0.25f;
    config.key_update_interval_sec = 1000.0f;
    StreamAnalyzer analyzer(config);

    const std::vector<float> audio =
        generate_chord_click_bed(config.sample_rate * 30, config.sample_rate, 120.0f);
    constexpr size_t kBlock = 1024;
    for (size_t position = 0; position < audio.size(); position += kBlock) {
      const size_t count = std::min(kBlock, audio.size() - position);
      analyzer.process(audio.data() + position, count);
      analyzer.read_frames(64);
    }

    const AnalyzerStats stats = analyzer.stats();
    REQUIRE(stats.estimate.key == -1);
    REQUIRE_FALSE(stats.estimate.bar_chord_progression.empty());
    for (const auto& entry : stats.estimate.voted_pattern) {
      CAPTURE(entry.root, entry.quality);
      REQUIRE(valid_entry(entry));
    }
    // A pattern name is a claim about scale degrees, which needs a key.
    REQUIRE(stats.estimate.detected_pattern_name.empty());
  }
}

TEST_CASE("StreamAnalyzer scalar accessors do not copy the stats snapshot", "[streaming][rt]") {
  StreamConfig config = bar_test_config();
  config.key_update_interval_sec = 0.25f;
  config.bpm_update_interval_sec = 0.25f;
  StreamAnalyzer analyzer(config);

  const std::vector<float> audio =
      generate_chord_click_bed(config.sample_rate * 20, config.sample_rate, 120.0f);
  constexpr size_t kBlock = 1024;
  for (size_t position = 0; position < audio.size(); position += kBlock) {
    const size_t count = std::min(kBlock, audio.size() - position);
    analyzer.process(audio.data() + position, count);
    analyzer.read_frames(64);
  }

  // The accessors are only worth measuring against a populated history: that is
  // what a snapshot copy would have to duplicate for one scalar.
  const AnalyzerStats stats = analyzer.stats();
  REQUIRE_FALSE(stats.estimate.chord_progression.empty());
  REQUIRE(analyzer.frame_count() == stats.total_frames);
  REQUIRE_THAT(analyzer.current_time(), WithinAbs(stats.duration_seconds, 1.0e-6f));

  size_t allocations = 0;
  int frames = 0;
  float seconds = 0.0f;
  {
    sonare::test::AllocationGuard guard;
    for (int i = 0; i < 1000; ++i) {
      frames += analyzer.frame_count();
      seconds += analyzer.current_time();
    }
    allocations = guard.count();
  }
  CAPTURE(frames, seconds);
  REQUIRE(allocations == 0);
}

TEST_CASE("StreamAnalyzer refuses audio fed after finalize", "[streaming][contract]") {
  StreamConfig config;
  config.sample_rate = 22050;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.n_mels = 32;

  const std::vector<float> chunk(2048, 0.25f);

  SECTION("internally tracked offsets") {
    StreamAnalyzer analyzer(config);
    analyzer.process(chunk.data(), chunk.size());
    analyzer.finalize();

    // Silently accepting this resumed on a buffer finalize() had drained, so
    // the resume boundary lost the preceding frame's worth of context.
    REQUIRE_THROWS_AS(analyzer.process(chunk.data(), chunk.size()), SonareException);
    try {
      analyzer.process(chunk.data(), chunk.size());
      FAIL("expected the finalized analyzer to reject more audio");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidState);
    }

    // An empty call carries no audio to lose and stays a no-op.
    REQUIRE_NOTHROW(analyzer.process(nullptr, 0));

    // reset() is the documented way to reuse the analyzer.
    analyzer.reset();
    REQUIRE_NOTHROW(analyzer.process(chunk.data(), chunk.size()));
  }

  SECTION("external offsets") {
    StreamAnalyzer analyzer(config);
    analyzer.process(chunk.data(), chunk.size(), 0);
    analyzer.finalize();
    REQUIRE_THROWS_AS(analyzer.process(chunk.data(), chunk.size(), chunk.size()), SonareException);

    analyzer.reset();
    REQUIRE_NOTHROW(analyzer.process(chunk.data(), chunk.size(), 0));
  }
}

TEST_CASE("StreamAnalyzer keeps a finite bar duration when the tempo drops out",
          "[streaming][bar][long]") {
  StreamConfig config = bar_test_config();
  config.key_update_interval_sec = 0.25f;
  config.bpm_update_interval_sec = 0.25f;
  StreamAnalyzer analyzer(config);

  const auto feed = [&](const std::vector<float>& audio) {
    constexpr size_t kBlock = 1024;
    for (size_t position = 0; position < audio.size(); position += kBlock) {
      const size_t count = std::min(kBlock, audio.size() - position);
      analyzer.process(audio.data() + position, count);
      analyzer.read_frames(64);
    }
  };

  // Rhythmic for long enough to activate bar tracking, then silent for longer
  // than the onset history window, so every onset the tempo estimator can see
  // is a zero. find_best_tempo then reports "no usable tempo" as BPM 0.
  feed(generate_chord_click_bed(config.sample_rate * 30, config.sample_rate, 120.0f));
  const float bar_duration_before = analyzer.stats().estimate.bar_duration;
  REQUIRE(bar_duration_before > 0.0f);

  feed(std::vector<float>(static_cast<size_t>(config.sample_rate) * 70, 0.0f));

  const AnalyzerStats stats = analyzer.stats();
  CAPTURE(bar_duration_before, stats.estimate.bpm, stats.estimate.bar_duration);
  // The drop-out really happened, so the assertions below are not vacuous.
  REQUIRE(stats.estimate.bpm == 0.0f);

  // 0 BPM is the absence of a tempo, not a tempo of zero: re-deriving a bar
  // length from it published an infinite bar_duration on all four surfaces and
  // stalled the bar boundary test forever, because current_time can never
  // reach bar_start_time_ + inf. A published bar length must stay a length the
  // supported tempo range can produce -- the analyzer counts four beats to the
  // bar, so that is 4 * 60 / kBpmMax .. 4 * 60 / kBpmMin. (The value need not
  // be the one held before the silence: the estimate wanders while the onset
  // window drains, and each of those tempos was a real estimate.)
  REQUIRE(std::isfinite(stats.estimate.bar_duration));
  const float shortest_bar = 4.0f * 60.0f / sonare::streaming_detail::kBpmMax;
  const float longest_bar = 4.0f * 60.0f / sonare::streaming_detail::kBpmMin;
  REQUIRE(stats.estimate.bar_duration >= shortest_bar - 1.0e-3f);
  REQUIRE(stats.estimate.bar_duration <= longest_bar + 1.0e-3f);
}
