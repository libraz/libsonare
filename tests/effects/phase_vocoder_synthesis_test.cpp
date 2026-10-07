/// @file phase_vocoder_synthesis_test.cpp
/// @brief Frame accounting shared by every phase-vocoder path: transition order,
///        real-FFT endpoints under phase locking, window-pair geometry, and the
///        output length of empty and shorter-than-a-hop input.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "effects/phase_vocoder.h"
#include "effects/time_stretch.h"
#include "engine/tempo_sync.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/phase.h"

using namespace sonare;
using sonare::constants::kTwoPiD;

namespace {

constexpr int kSmallFft = 16;
constexpr int kSmallHop = 4;
constexpr int kSmallBins = kSmallFft / 2 + 1;
constexpr int kSmallRate = 16000;

/// Bin-major spectrogram whose bins are all zero except those @p fill sets.
template <typename Fill>
Spectrogram make_spectrum(int n_frames, Fill fill) {
  std::vector<std::complex<float>> data(static_cast<size_t>(kSmallBins * n_frames));
  for (int bin = 0; bin < kSmallBins; ++bin) {
    for (int frame = 0; frame < n_frames; ++frame) {
      data[static_cast<size_t>(bin * n_frames + frame)] = fill(bin, frame);
    }
  }
  return Spectrogram::from_complex(data.data(), kSmallBins, n_frames, kSmallFft, kSmallHop,
                                   kSmallRate, WindowType::Hann);
}

double phase_error(double actual, double expected) {
  return std::abs(phase::wrap(actual - expected));
}

std::vector<float> render_stream(const StreamingPhaseVocoderConfig& config,
                                 const std::vector<float>& input, float rate) {
  StreamingPhaseVocoder stream(config);
  stream.push(input.data(), input.size());
  const Audio out = stream.finish(rate);
  return std::vector<float>(out.begin(), out.end());
}

double rms(const std::vector<float>& samples, size_t begin, size_t end) {
  double energy = 0.0;
  for (size_t i = begin; i < end; ++i) energy += static_cast<double>(samples[i]) * samples[i];
  return end > begin ? std::sqrt(energy / static_cast<double>(end - begin)) : 0.0;
}

}  // namespace

TEST_CASE("phase vocoder reproduces analysis phases at rate 1", "[phase_vocoder][synthesis]") {
  const std::vector<std::vector<float>> phase_tracks = {{0.0f, 0.2f, 0.6f, 1.1f, 1.7f},
                                                        {0.0f, 0.2f, 0.4f, 0.6f, 0.8f}};
  for (const auto& phases : phase_tracks) {
    const int n_frames = static_cast<int>(phases.size());
    const Spectrogram spec = make_spectrum(n_frames, [&](int bin, int frame) {
      return bin == 1 ? std::polar(1.0f, phases[static_cast<size_t>(frame)])
                      : std::complex<float>{};
    });
    for (const bool locked : {false, true}) {
      INFO("phase locked " << locked << ", third phase " << phases[2]);
      const Spectrogram out =
          locked ? phase_vocoder_phaselocked(spec, 1.0f) : phase_vocoder(spec, 1.0f);
      REQUIRE(out.n_frames() == n_frames);
      for (int frame = 0; frame < n_frames; ++frame) {
        const std::complex<float> value = out.complex_data()[n_frames + frame];
        INFO("frame " << frame);
        CHECK(std::abs(std::abs(value) - 1.0f) < 1e-5f);
        CHECK(phase_error(std::arg(value), phases[static_cast<size_t>(frame)]) < 1e-5);
      }
    }
  }
}

TEST_CASE("phase locking keeps real DC and Nyquist bins real under stretching",
          "[phase_vocoder][synthesis]") {
  constexpr int kFrames = 20;
  const Spectrogram spec = make_spectrum(kFrames, [](int bin, int frame) {
    if (bin == 0) return std::complex<float>(1.0f, 0.0f);
    if (bin == kSmallBins - 1) return std::complex<float>(0.5f, 0.0f);
    if (bin == 3) return std::polar(0.01f, 0.2f * static_cast<float>(frame));
    return std::complex<float>{};
  });

  for (const float rate : {0.5f, 1.0f, 2.0f}) {
    for (const bool locked : {true, false}) {
      INFO("rate " << rate << ", phase locked " << locked);
      const Spectrogram out =
          locked ? phase_vocoder_phaselocked(spec, rate) : phase_vocoder(spec, rate);
      const int n_frames = out.n_frames();
      for (int frame = 0; frame < n_frames; ++frame) {
        INFO("frame " << frame);
        const std::complex<float> dc = out.complex_data()[frame];
        const std::complex<float> nyquist =
            out.complex_data()[static_cast<size_t>((kSmallBins - 1) * n_frames + frame)];
        CHECK(std::abs(dc - std::complex<float>(1.0f, 0.0f)) < 1e-4f);
        CHECK(std::abs(nyquist - std::complex<float>(0.5f, 0.0f)) < 1e-4f);
      }
    }
  }
}

TEST_CASE("phase-locked streaming stretch keeps a DC offset and adds no low-frequency artifact",
          "[phase_vocoder][synthesis]") {
  for (const int sample_rate : {44100, 48000}) {
    StreamingPhaseVocoderConfig config;
    config.sample_rate = sample_rate;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.phase_lock = true;
    std::vector<float> input(static_cast<size_t>(sample_rate));
    const double tone_hz = 984.375;
    for (size_t i = 0; i < input.size(); ++i) {
      const double t = static_cast<double>(i) / sample_rate;
      input[i] = 0.05f + 0.2f * static_cast<float>(std::sin(kTwoPiD * tone_hz * t));
    }
    for (const float rate : {0.5f, 2.0f}) {
      INFO("sample rate " << sample_rate << ", rate " << rate);
      const std::vector<float> out = render_stream(config, input, rate);
      REQUIRE(out.size() == static_cast<size_t>(std::ceil(input.size() / rate)));
      const size_t edge = 2048;
      REQUIRE(out.size() > 4 * edge);
      double mean = 0.0;
      for (size_t i = edge; i < out.size() - edge; ++i) {
        REQUIRE(std::isfinite(out[i]));
        mean += out[i];
      }
      mean /= static_cast<double>(out.size() - 2 * edge);
      CHECK(std::abs(mean - 0.05) < 0.005);

      // Block means over one tone period's multiple track the offset everywhere.
      const size_t block = static_cast<size_t>(std::lround(sample_rate / tone_hz * 8.0));
      for (size_t start = edge; start + block < out.size() - edge; start += block) {
        double block_mean = 0.0;
        for (size_t i = start; i < start + block; ++i) block_mean += out[i];
        block_mean /= static_cast<double>(block);
        INFO("block at " << start);
        CHECK(std::abs(block_mean - 0.05) < 0.01);
      }
    }
  }
}

TEST_CASE("streaming phase vocoder refuses a window pair that leaves samples uncovered",
          "[phase_vocoder][synthesis]") {
  StreamingPhaseVocoderConfig config;
  config.sample_rate = 48000;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.phase_lock = false;

  for (const int win_length : {128, 256}) {
    INFO("win_length " << win_length);
    config.win_length = win_length;
    CHECK_THROWS_AS(StreamingPhaseVocoder(config), SonareException);
  }

  StreamingPhaseVocoderConfig two_point;
  two_point.sample_rate = 48000;
  two_point.n_fft = 2;
  two_point.hop_length = 1;
  CHECK_THROWS_AS(StreamingPhaseVocoder(two_point), SonareException);

  for (const int win_length : {512, 1024}) {
    config.win_length = win_length;
    for (int position = 128; position < 128 + config.hop_length; position += 17) {
      INFO("win_length " << win_length << ", impulse at " << position);
      std::vector<float> input(8192, 0.0f);
      input[static_cast<size_t>(position)] = 0.25f;
      const std::vector<float> out = render_stream(config, input, 1.0f);
      REQUIRE(out.size() == input.size());
      CHECK(std::abs(out[static_cast<size_t>(position)] - 0.25f) < 1e-3f);
    }
  }
}

TEST_CASE("validate_cola_geometry checks the resolved window pair's coverage",
          "[phase_vocoder][synthesis][spectrum]") {
  CHECK_NOTHROW(validate_cola_geometry(1024, 256, WindowType::Hann, 1024));
  CHECK_NOTHROW(validate_cola_geometry(1024, 256, WindowType::Hann, 512));
  CHECK_THROWS_AS(validate_cola_geometry(1024, 256, WindowType::Hann, 256), SonareException);
  CHECK_THROWS_AS(validate_cola_geometry(1024, 256, WindowType::Hann, 128), SonareException);
  CHECK_THROWS_AS(validate_cola_geometry(2, 1, WindowType::Hann, 2), SonareException);
  CHECK_NOTHROW(validate_cola_geometry(4, 2, WindowType::Rectangular, 2));
  CHECK_THROWS_AS(validate_cola_geometry(1024, 256, WindowType::Hann, 0), SonareException);
  CHECK_THROWS_AS(validate_cola_geometry(1024, 256, WindowType::Hann, 2048), SonareException);
}

TEST_CASE("finalizing a streaming phase vocoder without input emits nothing",
          "[phase_vocoder][synthesis]") {
  StreamingPhaseVocoderConfig config;
  config.sample_rate = 48000;
  config.n_fft = 1024;
  config.hop_length = 256;
  for (const float rate : {0.5f, 1.0f, 2.0f}) {
    INFO("rate " << rate);
    StreamingPhaseVocoder stream(config);
    CHECK(stream.finalize(rate).empty());
    CHECK(stream.finalize(rate).empty());

    std::vector<float> out(32, 1.0f);
    StreamingPhaseVocoder into(config);
    CHECK(into.finalize_into(rate, out.data(), out.size()) == 0);

    const std::vector<float> input(8192, 0.1f);
    StreamingPhaseVocoder used(config);
    used.push(input.data(), input.size());
    CHECK(used.finalize(rate).size() == static_cast<size_t>(std::ceil(input.size() / rate)));
    CHECK(used.finalize(rate).empty());
  }
}

TEST_CASE("input shorter than a hop is synthesized rather than silenced",
          "[phase_vocoder][synthesis]") {
  StreamingPhaseVocoderConfig config;
  config.sample_rate = 48000;
  config.n_fft = 2048;
  config.hop_length = 512;

  for (const size_t length : {1u, 64u, 128u, 255u, 511u, 512u, 513u, 1024u}) {
    const std::vector<float> input(length, 0.1f);
    for (const float rate : {0.5f, 1.0f, 2.0f}) {
      INFO("length " << length << ", rate " << rate);
      const size_t expected = static_cast<size_t>(std::ceil(static_cast<float>(length) / rate));
      const std::vector<float> once = render_stream(config, input, rate);
      REQUIRE(once.size() == expected);
      CHECK(rms(once, 0, once.size()) > 0.01);
      if (rate == 1.0f) {
        for (size_t i = 0; i < length; ++i) CHECK(std::abs(once[i] - 0.1f) < 1e-4f);
      }

      StreamingPhaseVocoder split(config);
      std::vector<float> chunked;
      const size_t half = length / 2;
      const Audio first = split.process(input.data(), half, rate);
      chunked.insert(chunked.end(), first.begin(), first.end());
      const Audio second = split.process(input.data() + half, length - half, rate);
      chunked.insert(chunked.end(), second.begin(), second.end());
      const Audio tail = split.finalize(rate);
      chunked.insert(chunked.end(), tail.begin(), tail.end());
      REQUIRE(chunked.size() == once.size());
      for (size_t i = 0; i < once.size(); ++i) CHECK(std::abs(chunked[i] - once[i]) < 1e-5f);

      StreamingPhaseVocoder into(config);
      std::vector<float> buffer(expected + 16, 0.0f);
      size_t written = into.process_into(input.data(), length, rate, buffer.data(), buffer.size());
      written += into.finalize_into(rate, buffer.data() + written, buffer.size() - written);
      REQUIRE(written == expected);
      for (size_t i = 0; i < expected; ++i) CHECK(std::abs(buffer[i] - once[i]) < 1e-5f);
    }

    for (const StretchBackend backend :
         {StretchBackend::PhaseVocoder, StretchBackend::NativeSpectral}) {
      INFO("length " << length << ", offline backend " << static_cast<int>(backend));
      TimeStretchConfig stretch;
      stretch.backend = backend;
      stretch.n_fft = 2048;
      stretch.hop_length = 512;
      const Audio audio = Audio::from_vector(input, 48000);
      const Audio out = time_stretch(audio, 1.0f, stretch);
      REQUIRE(out.size() == length);
      for (size_t i = 0; i < length; ++i) CHECK(std::abs(out[i] - 0.1f) < 1e-4f);
    }
  }
}

TEST_CASE("short tempo-sync segments survive in mono and duplicated stereo",
          "[phase_vocoder][synthesis][tempo_sync]") {
  engine::TempoSyncWarpBakeConfig config;
  config.sample_rate = 48000;
  config.n_fft = 2048;
  config.hop_length = 512;
  config.join_crossfade_samples = 0;

  for (const size_t length : {1u, 64u, 128u, 511u, 512u, 1024u}) {
    INFO("length " << length);
    const std::vector<float> source(length, 0.1f);
    const std::vector<engine::TempoSyncWarpSegment> segments = {{0, length, length}};
    const std::vector<float> mono =
        engine::bake_tempo_sync_warp_channel(source.data(), length, segments, config);
    const auto stereo = engine::bake_tempo_sync_warp_channels({source.data(), source.data()},
                                                              length, segments, config);
    REQUIRE(mono.size() == length);
    REQUIRE(stereo.size() == 2);
    for (size_t i = 0; i < length; ++i) {
      CHECK(std::abs(mono[i] - 0.1f) < 1e-4f);
      CHECK(std::abs(stereo[0][i] - 0.1f) < 1e-4f);
      CHECK(std::abs(stereo[1][i] - 0.1f) < 1e-4f);
    }
  }

  std::vector<float> sine(48000);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] =
        0.2f * static_cast<float>(std::sin(kTwoPiD * 1000.0 * static_cast<double>(i) / 48000.0));
  }
  std::vector<engine::TempoSyncWarpSegment> segments;
  for (size_t offset = 0; offset < sine.size(); offset += 128) {
    segments.push_back({offset, 128, 128});
  }
  const std::vector<float> mono =
      engine::bake_tempo_sync_warp_channel(sine.data(), sine.size(), segments, config);
  const auto stereo = engine::bake_tempo_sync_warp_channels({sine.data(), sine.data()}, sine.size(),
                                                            segments, config);
  REQUIRE(mono.size() == sine.size());
  CHECK(rms(mono, 0, mono.size()) > 0.13);
  CHECK(rms(stereo[0], 0, stereo[0].size()) > 0.13);
}
