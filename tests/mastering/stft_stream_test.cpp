/// @file stft_stream_test.cpp
/// @brief Bit-identity of the frame-wise STFT helpers against Spectrogram.
///
/// Every comparison here is an exact float equality, never an Approx: the
/// helpers exist to replace a Spectrogram round trip without moving a result,
/// so a tolerance would hide exactly the drift the tests are for.

#include "mastering/common/stft_stream.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numeric>
#include <random>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "core/window.h"
#include "util/constants.h"

namespace {

using sonare::Audio;
using sonare::PadMode;
using sonare::Spectrogram;
using sonare::stft_frame_count;
using sonare::StftConfig;
using sonare::mastering::common::IstftAccumulator;
using sonare::mastering::common::StftFrameReader;

constexpr int kSampleRate = 48000;

/// One analysis geometry the helpers must reproduce.
struct Geometry {
  const char* name;
  std::size_t length;
  int n_fft;
  int hop_length;
  int win_length;  ///< 0 selects n_fft
  bool center;
  PadMode pad_mode;
};

/// @details "odd frame count above 128" yields 141 frames, so a loop that
///          silently processed pairs of frames would show. "signal shorter than
///          one frame" is uncentred so the padded length really does fall below
///          n_fft, which is the only way to reach the frame count's returns-1
///          path. "reflect pad longer than signal" drives reflect_index past one
///          full period.
const Geometry kGeometries[] = {
    {"denoise default", 8000, 1024, 256, 0, true, PadMode::Constant},
    {"odd frame count above 128", 8960, 256, 64, 0, true, PadMode::Constant},
    {"uncentred", 6000, 256, 64, 0, false, PadMode::Constant},
    {"reflect padding", 4000, 512, 128, 0, true, PadMode::Reflect},
    {"reflect pad longer than signal", 100, 512, 128, 0, true, PadMode::Reflect},
    {"window shorter than fft", 4000, 512, 128, 300, true, PadMode::Constant},
    {"signal shorter than one frame", 300, 1024, 256, 0, false, PadMode::Constant},
    {"hop equal to fft", 4000, 256, 256, 0, true, PadMode::Constant},
};

StftConfig config_of(const Geometry& geometry) {
  StftConfig config;
  config.n_fft = geometry.n_fft;
  config.hop_length = geometry.hop_length;
  config.win_length = geometry.win_length;
  config.center = geometry.center;
  config.pad_mode = geometry.pad_mode;
  return config;
}

/// Two tones plus noise, so neither the spectrum nor the waveform is sparse.
std::vector<float> make_signal(std::size_t length, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> noise(-0.2f, 0.2f);
  std::vector<float> samples(length);
  for (std::size_t i = 0; i < length; ++i) {
    const float t = static_cast<float>(i);
    samples[i] = 0.55f * std::sin(0.031f * t) + 0.2f * std::sin(0.27f * t + 0.7f) + noise(rng);
  }
  return samples;
}

float rms(const Audio& audio) {
  double sum = 0.0;
  for (std::size_t i = 0; i < audio.size(); ++i) {
    sum += static_cast<double>(audio.data()[i]) * static_cast<double>(audio.data()[i]);
  }
  return audio.empty() ? 0.0f
                       : static_cast<float>(std::sqrt(sum / static_cast<double>(audio.size())));
}

float plane_rms(const Spectrogram& spec) {
  double sum = 0.0;
  for (int t = 0; t < spec.n_frames(); ++t) {
    for (int b = 0; b < spec.n_bins(); ++b) {
      sum += static_cast<double>(std::norm(spec.at(b, t)));
    }
  }
  const double count = static_cast<double>(spec.n_frames()) * static_cast<double>(spec.n_bins());
  return static_cast<float>(std::sqrt(sum / count));
}

/// True when the plane carries structure a cell-for-cell comparison can fail on.
/// A single-frame geometry cannot differ across frames, so there the equivalent
/// control is that the one frame is not constant across bins.
bool plane_has_contrast(const Spectrogram& spec) {
  if (spec.n_frames() < 2) {
    for (int b = 1; b < spec.n_bins(); ++b) {
      if (spec.at(b, 0) != spec.at(0, 0)) return true;
    }
    return false;
  }
  for (int t = 1; t < spec.n_frames(); ++t) {
    for (int b = 0; b < spec.n_bins(); ++b) {
      if (spec.at(b, t) != spec.at(b, 0)) return true;
    }
  }
  return false;
}

/// Per-(bin, frame) real gain standing in for a repair mask.
float mask_gain(int bin, int frame) {
  return 0.15f + 0.8f * static_cast<float>((bin * 7 + frame * 5) % 13) / 13.0f;
}

/// Index of the first sample where @p a and @p b differ exactly, or -1.
int first_difference(const Audio& a, const Audio& b) {
  const std::size_t count = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (a.data()[i] != b.data()[i]) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

TEST_CASE("StftFrameReader reproduces Spectrogram::compute bit for bit",
          "[mastering][stft-stream]") {
  for (const Geometry& geometry : kGeometries) {
    CAPTURE(geometry.name);
    const StftConfig config = config_of(geometry);
    const Audio audio = Audio::from_vector(make_signal(geometry.length, 12345u), kSampleRate);
    const Spectrogram spec = Spectrogram::compute(audio, config);

    StftFrameReader reader(audio.data(), audio.size(), kSampleRate, config);
    REQUIRE(reader.n_frames() == spec.n_frames());
    REQUIRE(reader.n_bins() == spec.n_bins());
    REQUIRE(reader.n_frames() == stft_frame_count(audio.size(), config));

    // Positive controls before the comparison: the plane carries energy, and it
    // is not one frame repeated, so agreement is agreement about something.
    REQUIRE(plane_rms(spec) > 0.0f);
    REQUIRE(plane_has_contrast(spec));

    int bad_bin = -1;
    int bad_frame = -1;
    for (int t = 0; t < spec.n_frames() && bad_frame < 0; ++t) {
      const std::complex<float>* computed = reader.frame(t);
      for (int b = 0; b < spec.n_bins(); ++b) {
        const std::complex<float>& reference = spec.at(b, t);
        if (computed[b].real() != reference.real() || computed[b].imag() != reference.imag()) {
          bad_bin = b;
          bad_frame = t;
          break;
        }
      }
    }
    CAPTURE(bad_bin, bad_frame);
    REQUIRE(bad_frame == -1);
  }
}

TEST_CASE("StftFrameReader frames are independent of the order they are read in",
          "[mastering][stft-stream]") {
  for (const Geometry& geometry : kGeometries) {
    CAPTURE(geometry.name);
    const StftConfig config = config_of(geometry);
    const Audio audio = Audio::from_vector(make_signal(geometry.length, 777u), kSampleRate);
    const Spectrogram spec = Spectrogram::compute(audio, config);

    REQUIRE(plane_rms(spec) > 0.0f);
    REQUIRE(plane_has_contrast(spec));

    StftFrameReader reader(audio.data(), audio.size(), kSampleRate, config);
    std::vector<int> order(static_cast<std::size_t>(spec.n_frames()));
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937(99u));

    int bad_bin = -1;
    int bad_frame = -1;
    for (int t : order) {
      const std::complex<float>* computed = reader.frame(t);
      for (int b = 0; b < spec.n_bins(); ++b) {
        const std::complex<float>& reference = spec.at(b, t);
        if (computed[b].real() != reference.real() || computed[b].imag() != reference.imag()) {
          bad_bin = b;
          bad_frame = t;
          break;
        }
      }
      if (bad_frame >= 0) break;
    }
    CAPTURE(bad_bin, bad_frame);
    REQUIRE(bad_frame == -1);
  }
}

TEST_CASE("IstftAccumulator reproduces Spectrogram::to_audio bit for bit",
          "[mastering][stft-stream]") {
  for (const Geometry& geometry : kGeometries) {
    CAPTURE(geometry.name);
    const StftConfig config = config_of(geometry);
    const int target_length = static_cast<int>(geometry.length);
    const Audio audio = Audio::from_vector(make_signal(geometry.length, 4242u), kSampleRate);
    const Spectrogram spec = Spectrogram::compute(audio, config);

    const Audio reference = spec.to_audio(target_length);
    REQUIRE(reference.size() == geometry.length);
    REQUIRE(rms(reference) > 0.0f);

    IstftAccumulator accumulator(spec.n_frames(), kSampleRate, config, target_length);
    REQUIRE(accumulator.n_bins() == spec.n_bins());

    std::vector<std::complex<float>> column(static_cast<std::size_t>(spec.n_bins()));
    for (int t = 0; t < spec.n_frames(); ++t) {
      for (int b = 0; b < spec.n_bins(); ++b) {
        column[static_cast<std::size_t>(b)] = spec.at(b, t);
      }
      accumulator.push(column.data());
    }
    const Audio streamed = accumulator.finish();

    REQUIRE(streamed.size() == reference.size());
    const int bad_sample = first_difference(streamed, reference);
    CAPTURE(bad_sample);
    REQUIRE(bad_sample == -1);
  }
}

TEST_CASE("A masked plane round-trips through IstftAccumulator bit for bit",
          "[mastering][stft-stream]") {
  for (const Geometry& geometry : kGeometries) {
    CAPTURE(geometry.name);
    const StftConfig config = config_of(geometry);
    const int target_length = static_cast<int>(geometry.length);
    const Audio audio = Audio::from_vector(make_signal(geometry.length, 31337u), kSampleRate);
    const Spectrogram spec = Spectrogram::compute(audio, config);

    const int n_bins = spec.n_bins();
    const int n_frames = spec.n_frames();
    std::vector<std::complex<float>> plane(static_cast<std::size_t>(n_bins) *
                                           static_cast<std::size_t>(n_frames));
    for (int b = 0; b < n_bins; ++b) {
      for (int t = 0; t < n_frames; ++t) {
        plane[static_cast<std::size_t>(b) * static_cast<std::size_t>(n_frames) +
              static_cast<std::size_t>(t)] = spec.at(b, t) * mask_gain(b, t);
      }
    }

    const Spectrogram masked = Spectrogram::from_complex(
        plane.data(), n_bins, n_frames, config.n_fft, config.hop_length, kSampleRate, config.window,
        config.center, config.win_length, config.pad_mode);
    const Audio reference = masked.to_audio(target_length);
    REQUIRE(reference.size() == geometry.length);
    REQUIRE(rms(reference) > 0.0f);

    IstftAccumulator accumulator(n_frames, kSampleRate, config, target_length);
    std::vector<std::complex<float>> column(static_cast<std::size_t>(n_bins));
    for (int t = 0; t < n_frames; ++t) {
      for (int b = 0; b < n_bins; ++b) {
        column[static_cast<std::size_t>(b)] = spec.at(b, t) * mask_gain(b, t);
      }
      accumulator.push(column.data());
    }
    const Audio streamed = accumulator.finish();

    REQUIRE(streamed.size() == reference.size());
    const int bad_sample = first_difference(streamed, reference);
    CAPTURE(bad_sample);
    REQUIRE(bad_sample == -1);
  }
}

TEST_CASE("The hop == nFft geometry reaches both sides of the window-sum select",
          "[mastering][stft-stream]") {
  // The un-normalized branch is only worth reproducing if some geometry takes
  // it, so the window sum is rebuilt here from the windows directly -- nothing
  // in this case reads the accumulator it is a control for.
  const Geometry* geometry = nullptr;
  for (const Geometry& candidate : kGeometries) {
    if (candidate.hop_length == candidate.n_fft) geometry = &candidate;
  }
  REQUIRE(geometry != nullptr);

  const StftConfig config = config_of(*geometry);
  const int n_fft = config.n_fft;
  const int win_length = config.actual_win_length();
  const int win_offset = (n_fft - win_length) / 2;
  const auto analysis = sonare::get_window_cached(config.window, win_length, true);
  const auto synthesis = sonare::get_window_cached(config.window, win_length, false);

  std::vector<float> product(static_cast<std::size_t>(n_fft), 0.0f);
  for (int i = 0; i < win_length; ++i) {
    product[static_cast<std::size_t>(win_offset + i)] =
        (*analysis)[static_cast<std::size_t>(i)] * (*synthesis)[static_cast<std::size_t>(i)];
  }

  const int n_frames = stft_frame_count(geometry->length, config);
  const std::size_t full_length =
      static_cast<std::size_t>(n_frames - 1) * static_cast<std::size_t>(config.hop_length) +
      static_cast<std::size_t>(n_fft);
  std::vector<float> window_sum(full_length, 0.0f);
  for (int t = 0; t < n_frames; ++t) {
    const std::size_t start =
        static_cast<std::size_t>(t) * static_cast<std::size_t>(config.hop_length);
    for (int i = 0; i < n_fft; ++i) {
      window_sum[start + static_cast<std::size_t>(i)] += product[static_cast<std::size_t>(i)];
    }
  }

  const std::size_t trim_start = config.center ? static_cast<std::size_t>(n_fft / 2) : 0;
  const std::size_t trim_end = std::min(full_length, trim_start + geometry->length);
  int below = 0;
  int above = 0;
  for (std::size_t i = trim_start; i < trim_end; ++i) {
    if (window_sum[i] > sonare::constants::kSpectrumEpsilon) {
      ++above;
    } else {
      ++below;
    }
  }
  CAPTURE(below, above);
  REQUIRE(below > 0);
  REQUIRE(above > 0);
}
