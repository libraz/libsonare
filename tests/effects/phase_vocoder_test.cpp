/// @file phase_vocoder_test.cpp
/// @brief Phase vocoder precision and regression tests (P1-1).
/// @details Targets the synthesis-phase accumulator, which was promoted from
///          float to double in src/effects/phase_vocoder.cpp to keep long
///          pitch-shift / time-stretch runs free of monotonic phase drift.

#include "effects/phase_vocoder.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "support/alloc_guard.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;

namespace {

/// @brief Generates a pure sine wave.
Audio make_sine(float freq_hz, int sample_rate, float duration_sec) {
  const int n_samples = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> samples(n_samples);
  const double w = 2.0 * sonare::constants::kPiD * static_cast<double>(freq_hz) /
                   static_cast<double>(sample_rate);
  for (int i = 0; i < n_samples; ++i) {
    samples[i] = static_cast<float>(std::sin(w * static_cast<double>(i)));
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

Audio make_transient_fixture(int sample_rate) {
  std::vector<float> samples(static_cast<size_t>(sample_rate / 2), 0.0f);
  for (size_t onset :
       {static_cast<size_t>(1024), static_cast<size_t>(4096), static_cast<size_t>(8192)}) {
    for (size_t i = 0; i < 32 && onset + i < samples.size(); ++i) {
      const float env = 1.0f - static_cast<float>(i) / 32.0f;
      samples[onset + i] += env * ((i % 2) == 0 ? 0.9f : -0.65f);
    }
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

Audio make_sustained_pad_fixture(int sample_rate) {
  std::vector<float> samples(static_cast<size_t>(sample_rate / 2), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sample_rate);
    samples[i] = 0.35f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 220.0 * t)) +
                 0.18f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 330.0 * t)) +
                 0.08f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 550.0 * t));
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

double rms(const Audio& audio) {
  double energy = 0.0;
  for (float sample : audio) energy += static_cast<double>(sample) * static_cast<double>(sample);
  return audio.empty() ? 0.0 : std::sqrt(energy / static_cast<double>(audio.size()));
}

float peak_abs(const Audio& audio) {
  float peak = 0.0f;
  for (float sample : audio) peak = std::max(peak, std::abs(sample));
  return peak;
}

}  // namespace

TEST_CASE("phase_vocoder stays finite and bounded on long 10s time-stretched signal",
          "[phase_vocoder][precision]") {
  /// Long single-tone signal time-stretched to ~2x. The synthesis accumulator
  /// runs for ~860 frames (≈55k radians of total phase advance). With a float
  /// accumulator, large-magnitude wrap operations lose precision and the
  /// per-frame increment quantization can produce non-finite tails in extreme
  /// cases (denormals/NaN from spurious magnitude spikes). With a double
  /// accumulator, the whole output stays finite and within a sane magnitude
  /// bound vs the input magnitude envelope.
  const int sr = 22050;
  const float duration_sec = 10.0f;
  const float freq = 440.0f;
  Audio audio = make_sine(freq, sr, duration_sec);

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;
  stft_config.center = true;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  REQUIRE(spec.n_frames() >= 200);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;

  Spectrogram out = phase_vocoder(spec, 0.5f, pv_config);  // 2x time stretch
  REQUIRE(!out.empty());
  REQUIRE(out.n_bins() == spec.n_bins());
  REQUIRE(out.n_frames() >= spec.n_frames() + 200);

  const auto& in_mag = spec.magnitude();
  const auto& out_mag = out.magnitude();

  /// Output must be finite end-to-end (no NaN/Inf from accumulator overflow).
  for (float v : out_mag) REQUIRE(std::isfinite(v));

  /// Total magnitude energy ratio should be near unity (interpolation only).
  double in_sum = 0.0;
  double out_sum = 0.0;
  for (float v : in_mag) in_sum += static_cast<double>(v);
  for (float v : out_mag) out_sum += static_cast<double>(v);
  REQUIRE(in_sum > 0.0);
  REQUIRE(out_sum / in_sum > 1.5);  // ~2x because output is ~2x longer
  REQUIRE(out_sum / in_sum < 2.5);

  /// Peak bin should remain the same (440 Hz).
  const int n_fft = stft_config.n_fft;
  const int peak_bin = static_cast<int>(
      std::round(static_cast<double>(freq) * static_cast<double>(n_fft) / static_cast<double>(sr)));
  REQUIRE(peak_bin > 0);

  /// Sample peak bin magnitude in the last 25% of frames; it should be
  /// comparable to the input peak bin magnitude (within a factor of 2).
  const int n_out = out.n_frames();
  const int late_start = (n_out * 3) / 4;
  double out_late_peak = 0.0;
  for (int t = late_start; t < n_out; ++t) {
    out_late_peak = std::max(out_late_peak, static_cast<double>(out_mag[peak_bin * n_out + t]));
  }
  const int n_in = spec.n_frames();
  double in_peak = 0.0;
  for (int t = 0; t < n_in; ++t) {
    in_peak = std::max(in_peak, static_cast<double>(in_mag[peak_bin * n_in + t]));
  }
  REQUIRE(in_peak > 0.0);
  REQUIRE(out_late_peak / in_peak > 0.5);
  REQUIRE(out_late_peak / in_peak < 2.0);
}

TEST_CASE("phase_vocoder is deterministic across repeated runs (double precision)",
          "[phase_vocoder][precision]") {
  /// Determinism is a necessary condition for correctness of the double-precision
  /// rewrite — the accumulator must not depend on stale uninitialized memory.
  /// Two independent runs on the same input must produce bit-identical output.
  const int sr = 22050;
  Audio audio = make_sine(440.0f, sr, 5.0f);

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;

  Spectrogram out_a = phase_vocoder(spec, 1.25f, pv_config);
  Spectrogram out_b = phase_vocoder(spec, 1.25f, pv_config);

  REQUIRE(out_a.n_frames() == out_b.n_frames());
  REQUIRE(out_a.n_bins() == out_b.n_bins());

  const std::complex<float>* a = out_a.complex_data();
  const std::complex<float>* b = out_b.complex_data();
  const size_t n = static_cast<size_t>(out_a.n_frames()) * static_cast<size_t>(out_a.n_bins());
  for (size_t i = 0; i < n; ++i) {
    REQUIRE(a[i].real() == b[i].real());
    REQUIRE(a[i].imag() == b[i].imag());
  }
}

TEST_CASE("StreamingPhaseVocoder prototype is chunk-invariant and matches offline path",
          "[phase_vocoder][streaming][step5]") {
  const int sr = 22050;
  Audio audio = make_sine(330.0f, sr, 0.5f);

  StreamingPhaseVocoderConfig config;
  config.sample_rate = sr;
  config.n_fft = 512;
  config.hop_length = 128;
  config.phase_lock = true;

  StreamingPhaseVocoder once(config);
  REQUIRE(once.latency_samples() == config.n_fft / 2);
  once.push(audio);
  Audio one_shot = once.finish(0.75f);
  REQUIRE(once.pending_input_samples() == 0);

  StreamingPhaseVocoder chunked(config);
  const size_t split_a = 137;
  const size_t split_b = 2048;
  chunked.push(audio.data(), split_a);
  chunked.push(audio.data() + split_a, split_b - split_a);
  chunked.push(audio.data() + split_b, audio.size() - split_b);
  Audio chunked_out = chunked.finish(0.75f);

  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.center = true;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  PhaseVocoderConfig pv_config;
  pv_config.hop_length = config.hop_length;
  Audio offline =
      phase_vocoder_phaselocked(spec, 0.75f, pv_config)
          .to_audio(static_cast<int>(std::ceil(static_cast<float>(audio.size()) / 0.75f)));

  REQUIRE(one_shot.size() == offline.size());
  REQUIRE(chunked_out.size() == offline.size());
  for (size_t i = 0; i < offline.size(); ++i) {
    REQUIRE(std::isfinite(one_shot[i]));
    REQUIRE(std::abs(one_shot[i] - offline[i]) <= 1.0e-4f);
    REQUIRE(std::abs(chunked_out[i] - offline[i]) <= 1.0e-4f);
  }
}

TEST_CASE("StreamingPhaseVocoder prototype normalizes config and reports centered latency",
          "[phase_vocoder][streaming][step5]") {
  StreamingPhaseVocoderConfig config;
  config.sample_rate = 48000;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.win_length = 0;

  StreamingPhaseVocoder streamer(config);
  REQUIRE(streamer.config().win_length == 1024);
  REQUIRE(streamer.latency_samples() == 512);

  StreamingPhaseVocoderConfig short_window = config;
  short_window.win_length = 512;
  StreamingPhaseVocoder short_streamer(short_window);
  REQUIRE(short_streamer.config().win_length == 512);
  REQUIRE(short_streamer.latency_samples() == 512);

  StreamingPhaseVocoderConfig sparse_hop = config;
  sparse_hop.hop_length = 768;
  REQUIRE_THROWS_AS(StreamingPhaseVocoder(sparse_hop), SonareException);
}

TEST_CASE("StreamingPhaseVocoder process facade buffers until finalize",
          "[phase_vocoder][streaming][step5]") {
  const int sr = 22050;
  Audio audio = make_sine(220.0f, sr, 0.25f);

  StreamingPhaseVocoderConfig config;
  config.sample_rate = sr;
  config.n_fft = 512;
  config.hop_length = 128;

  StreamingPhaseVocoder streamer(config);
  Audio first = streamer.process(audio.data(), 1000, 1.25f);
  REQUIRE(first.sample_rate() == sr);
  REQUIRE(streamer.pending_input_samples() <= 1000);

  Audio second = streamer.process(audio.slice_samples(1000), 1.25f);
  REQUIRE(second.sample_rate() == sr);
  REQUIRE(streamer.pending_input_samples() <= audio.size());

  StreamingPhaseVocoder reference(config);
  reference.push(audio);
  Audio expected = reference.finish(1.25f);
  Audio tail = streamer.finalize(1.25f);

  std::vector<float> combined;
  combined.insert(combined.end(), first.begin(), first.end());
  combined.insert(combined.end(), second.begin(), second.end());
  combined.insert(combined.end(), tail.begin(), tail.end());

  REQUIRE(streamer.pending_input_samples() == 0);
  REQUIRE(first.size() + second.size() > 0);
  REQUIRE(combined.size() == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    REQUIRE(std::abs(combined[i] - expected[i]) <= 1.0e-4f);
  }
}

TEST_CASE("StreamingPhaseVocoder locks rate until reset", "[phase_vocoder][streaming][step5]") {
  const int sr = 22050;
  Audio audio = make_sine(110.0f, sr, 0.1f);

  StreamingPhaseVocoderConfig config;
  config.sample_rate = sr;
  config.n_fft = 512;
  config.hop_length = 128;

  StreamingPhaseVocoder streamer(config);
  streamer.process(audio.data(), 512, 0.9f);
  REQUIRE_THROWS(streamer.process(audio.data(), 512, 1.1f));

  streamer.reset();
  REQUIRE_NOTHROW(streamer.process(audio.data(), 512, 1.1f));
}

TEST_CASE("StreamingPhaseVocoder process_into uses caller-owned output",
          "[phase_vocoder][streaming][step5][rt]") {
  const int sr = 22050;
  Audio audio = make_sine(180.0f, sr, 0.25f);

  StreamingPhaseVocoderConfig config;
  config.sample_rate = sr;
  config.n_fft = 512;
  config.hop_length = 128;

  StreamingPhaseVocoder reference(config);
  reference.push(audio);
  Audio expected = reference.finish(0.8f);

  StreamingPhaseVocoder streamer(config);
  streamer.reserve(audio.size(), expected.size() + static_cast<size_t>(config.n_fft));
  std::vector<float> out(expected.size() + static_cast<size_t>(config.n_fft), 0.0f);
  size_t written = 0;
  written +=
      streamer.process_into(audio.data(), 768, 0.8f, out.data() + written, out.size() - written);
  written += streamer.process_into(audio.data() + 768, audio.size() - 768, 0.8f,
                                   out.data() + written, out.size() - written);
  written += streamer.finalize_into(0.8f, out.data() + written, out.size() - written);

  REQUIRE(written == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    REQUIRE(std::abs(out[i] - expected[i]) <= 1.0e-4f);
  }
}

TEST_CASE("StreamingPhaseVocoder process_into is allocation-free after reserve",
          "[phase_vocoder][streaming][step5][rt]") {
  const int sr = 22050;
  Audio audio = make_sine(180.0f, sr, 0.25f);

  StreamingPhaseVocoderConfig config;
  config.sample_rate = sr;
  config.n_fft = 512;
  config.hop_length = 128;

  StreamingPhaseVocoder reference(config);
  reference.push(audio);
  Audio expected = reference.finish(0.8f);

  StreamingPhaseVocoder streamer(config);
  streamer.reserve(audio.size(), expected.size() + static_cast<size_t>(config.n_fft));
  std::vector<float> out(expected.size() + static_cast<size_t>(config.n_fft), 0.0f);

  size_t written = 0;
  sonare::test::AllocationGuard guard;
  written +=
      streamer.process_into(audio.data(), 768, 0.8f, out.data() + written, out.size() - written);
  written += streamer.process_into(audio.data() + 768, audio.size() - 768, 0.8f,
                                   out.data() + written, out.size() - written);
  written += streamer.finalize_into(0.8f, out.data() + written, out.size() - written);

  REQUIRE(guard.count() == 0);
  REQUIRE(written == expected.size());
}

TEST_CASE("StreamingPhaseVocoder compacts processed prefixes during streaming",
          "[phase_vocoder][streaming][step5][rt]") {
  const int sr = 22050;
  Audio audio = make_sine(220.0f, sr, 2.0f);

  StreamingPhaseVocoderConfig config;
  config.sample_rate = sr;
  config.n_fft = 512;
  config.hop_length = 128;

  StreamingPhaseVocoder streamer(config);
  streamer.reserve(4096, 8192);
  std::vector<float> out(8192, 0.0f);
  size_t retained_peak = 0;
  size_t written_total = 0;
  for (size_t offset = 0; offset < audio.size(); offset += 1024) {
    const size_t count = std::min<size_t>(1024, audio.size() - offset);
    written_total +=
        streamer.process_into(audio.data() + offset, count, 0.9f, out.data(), out.size());
    retained_peak = std::max(retained_peak, streamer.pending_input_samples());
  }
  written_total += streamer.finalize_into(0.9f, out.data(), out.size());

  REQUIRE(written_total > 0);
  REQUIRE(retained_peak < audio.size() / 4);
  REQUIRE(streamer.pending_input_samples() == 0);
}

TEST_CASE("StreamingPhaseVocoder keeps quality invariants for transient and sustained fixtures",
          "[phase_vocoder][streaming][step5][quality]") {
  const int sr = 22050;
  const float rate = 0.75f;

  StreamingPhaseVocoderConfig config;
  config.sample_rate = sr;
  config.n_fft = 512;
  config.hop_length = 128;
  config.phase_lock = true;

  for (const Audio& audio : {make_transient_fixture(sr), make_sustained_pad_fixture(sr)}) {
    StreamingPhaseVocoder streamer(config);
    streamer.push(audio);
    Audio stretched = streamer.finish(rate);

    StftConfig stft_config;
    stft_config.n_fft = config.n_fft;
    stft_config.hop_length = config.hop_length;
    stft_config.center = true;
    Spectrogram spec = Spectrogram::compute(audio, stft_config);
    PhaseVocoderConfig pv_config;
    pv_config.hop_length = config.hop_length;
    Audio offline =
        phase_vocoder_phaselocked(spec, rate, pv_config)
            .to_audio(static_cast<int>(std::ceil(static_cast<float>(audio.size()) / rate)));

    REQUIRE(stretched.size() ==
            static_cast<size_t>(std::ceil(static_cast<float>(audio.size()) / rate)));
    REQUIRE(stretched.size() == offline.size());
    REQUIRE(rms(stretched) > rms(audio) * 0.25);
    REQUIRE(peak_abs(stretched) > peak_abs(audio) * 0.30f);
    for (size_t i = 0; i < stretched.size(); ++i) {
      REQUIRE(std::isfinite(stretched[i]));
      REQUIRE(std::abs(stretched[i] - offline[i]) <= 1.0e-4f);
    }
  }
}

TEST_CASE("phase_vocoder_phaselocked stays finite on long 10s time-stretched signal",
          "[phase_vocoder][precision]") {
  /// Same long-run finiteness check for the phase-locked variant.
  const int sr = 22050;
  const float duration_sec = 10.0f;
  const float freq = 440.0f;
  Audio audio = make_sine(freq, sr, duration_sec);

  StftConfig stft_config;
  stft_config.n_fft = 2048;
  stft_config.hop_length = 512;
  stft_config.center = true;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  REQUIRE(spec.n_frames() >= 200);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;

  Spectrogram out = phase_vocoder_phaselocked(spec, 0.5f, pv_config);
  REQUIRE(!out.empty());

  const auto& mag = out.magnitude();
  for (float v : mag) REQUIRE(std::isfinite(v));

  /// Peak bin magnitude at the end of the run must remain comparable.
  const int n_fft = stft_config.n_fft;
  const int peak_bin = static_cast<int>(
      std::round(static_cast<double>(freq) * static_cast<double>(n_fft) / static_cast<double>(sr)));
  REQUIRE(peak_bin > 0);
  const int n_out = out.n_frames();
  const int late_start = (n_out * 3) / 4;
  double out_late_peak = 0.0;
  for (int t = late_start; t < n_out; ++t) {
    out_late_peak = std::max(out_late_peak, static_cast<double>(mag[peak_bin * n_out + t]));
  }
  REQUIRE(out_late_peak > 0.0);
  REQUIRE(std::isfinite(out_late_peak));
}

TEST_CASE("phase_vocoder short-signal regression preserves frame count and magnitude",
          "[phase_vocoder][regression]") {
  /// Short signal: identity rate (1.0f) must keep frame count within ±1 and
  /// preserve the magnitude envelope. Exercises the t_out==0 initialization
  /// branch that was changed to double-precision casting.
  const int sr = 22050;
  Audio audio = make_sine(440.0f, sr, 0.5f);  // 0.5 s

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  REQUIRE(spec.n_frames() >= 2);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;
  Spectrogram out = phase_vocoder(spec, 1.0f, pv_config);

  REQUIRE(!out.empty());
  REQUIRE(out.n_bins() == spec.n_bins());
  REQUIRE(std::abs(out.n_frames() - spec.n_frames()) <= 1);

  /// Total magnitude energy should match the input within tight tolerance:
  /// the synthesis path only resamples in time and reuses analysis magnitudes,
  /// so the ratio is dominated by interpolation, not by phase precision.
  const auto& in_mag = spec.magnitude();
  const auto& out_mag = out.magnitude();
  double in_sum = 0.0;
  double out_sum = 0.0;
  for (float v : in_mag) in_sum += static_cast<double>(v);
  for (float v : out_mag) out_sum += static_cast<double>(v);
  REQUIRE(in_sum > 0.0);
  const double ratio = out_sum / in_sum;
  REQUIRE(ratio > 0.8);
  REQUIRE(ratio < 1.2);

  for (float v : out_mag) REQUIRE(std::isfinite(v));
}

TEST_CASE("compute_instantaneous_frequency rejects degenerate parameters",
          "[phase_vocoder][validation]") {
  /// n_bins == 1 implies n_fft = (n_bins - 1) * 2 = 0, which would divide the
  /// bin-frequency formula by zero and emit NaN. sample_rate == 0 divides the
  /// time-step by zero. Both must be rejected with InvalidParameter rather than
  /// silently producing non-finite output.
  const std::vector<float> phase{0.1f, 0.2f, 0.3f};
  const std::vector<float> prev{0.0f, 0.1f, 0.2f};

  /// n_bins == 1 (n_fft would be 0).
  REQUIRE_THROWS_AS(compute_instantaneous_frequency(phase.data(), prev.data(), 1, 512, 22050),
                    SonareException);

  /// sample_rate == 0 (time-step divide by zero).
  REQUIRE_THROWS_AS(compute_instantaneous_frequency(phase.data(), prev.data(), 3, 512, 0),
                    SonareException);

  /// Error code is specifically InvalidParameter for both cases.
  try {
    compute_instantaneous_frequency(phase.data(), prev.data(), 1, 512, 22050);
    FAIL("expected SonareException for n_bins == 1");
  } catch (const SonareException& e) {
    REQUIRE(e.code() == ErrorCode::InvalidParameter);
  }
  try {
    compute_instantaneous_frequency(phase.data(), prev.data(), 3, 512, 0);
    FAIL("expected SonareException for sample_rate == 0");
  } catch (const SonareException& e) {
    REQUIRE(e.code() == ErrorCode::InvalidParameter);
  }

  /// A valid call (n_bins >= 2, sample_rate > 0, hop_length > 0) still succeeds
  /// and returns finite values.
  std::vector<float> freq =
      compute_instantaneous_frequency(phase.data(), prev.data(), 3, 512, 22050);
  REQUIRE(freq.size() == 3);
  for (float v : freq) REQUIRE(std::isfinite(v));
}

TEST_CASE("phase_vocoder short-signal rate change still produces finite output",
          "[phase_vocoder][regression]") {
  /// Time-stretch and squash on a short signal. Verifies the rate-≠-1 path
  /// (which exercises non-zero phase_diff and the in-loop accumulator update)
  /// still produces finite output under the double-precision rewrite.
  const int sr = 22050;
  Audio audio = make_sine(440.0f, sr, 0.5f);

  StftConfig stft_config;
  stft_config.n_fft = 1024;
  stft_config.hop_length = 256;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;

  for (float rate : {0.5f, 1.25f, 2.0f}) {
    Spectrogram out = phase_vocoder(spec, rate, pv_config);
    REQUIRE(!out.empty());
    const auto& mag = out.magnitude();
    for (float v : mag) REQUIRE(std::isfinite(v));
  }
}
