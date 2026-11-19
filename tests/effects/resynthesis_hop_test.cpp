/// @file resynthesis_hop_test.cpp
/// @brief Every resynthesizing call runs an absent hop as a quarter of n_fft.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/event_model/event_extractor.h"
#include "editing/event_model/event_renderer.h"
#include "effects/decompose.h"
#include "effects/hpss.h"
#include "effects/pitch_shift.h"
#include "effects/spectral_edit.h"
#include "effects/time_stretch.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "util/constants.h"

using namespace sonare;

namespace {

using sonare::constants::kTwoPi;

constexpr int kSampleRate = 22050;
constexpr int kNFft = 1024;
constexpr int kQuarterHop = kNFft / 4;

// A tone with clicks every 2048 samples, so the percussive paths have onsets to find.
Audio make_signal() {
  std::vector<float> samples(kSampleRate / 2);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.3f * std::sin(kTwoPi * 330.0f * static_cast<float>(i) / kSampleRate);
    if (i % 2048 < 8) samples[i] += 0.6f;
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

std::vector<float> samples_of(const Audio& audio) { return {audio.begin(), audio.end()}; }

}  // namespace

TEST_CASE("resolve_resynthesis_hop derives a quarter of n_fft, at least 1", "[effects][hop]") {
  CHECK(resolve_resynthesis_hop(0, 1024) == 256);
  CHECK(resolve_resynthesis_hop(0, 2048) == 512);
  CHECK(resolve_resynthesis_hop(0, 3) == 1);
  CHECK(resolve_resynthesis_hop(0, 2) == 1);
  CHECK(resolve_resynthesis_hop(0, 1) == 1);
  CHECK(resolve_resynthesis_hop(300, 1024) == 300);
  CHECK(resolve_resynthesis_hop(-1, 1024) == -1);
}

TEST_CASE("an absent hop equals the explicit quarter hop on every resynthesizing call",
          "[effects][hop]") {
  const Audio audio = make_signal();

  SECTION("time stretch, both backends") {
    for (const StretchBackend backend :
         {StretchBackend::NativeSpectral, StretchBackend::PhaseVocoder}) {
      TimeStretchConfig absent;
      absent.n_fft = kNFft;
      absent.backend = backend;
      TimeStretchConfig given = absent;
      given.hop_length = kQuarterHop;
      CHECK(samples_of(time_stretch(audio, 1.25f, absent)) ==
            samples_of(time_stretch(audio, 1.25f, given)));
    }
  }

  SECTION("pitch shift, both backends") {
    for (const StretchBackend backend :
         {StretchBackend::NativeSpectral, StretchBackend::PhaseVocoder}) {
      PitchShiftConfig absent;
      absent.n_fft = kNFft;
      absent.backend = backend;
      PitchShiftConfig given = absent;
      given.hop_length = kQuarterHop;
      CHECK(samples_of(pitch_shift(audio, 3.0f, absent)) ==
            samples_of(pitch_shift(audio, 3.0f, given)));
    }
  }

  SECTION("hpss, hpss with residual and the single components") {
    StftConfig absent;
    absent.n_fft = kNFft;
    absent.hop_length = 0;
    StftConfig given = absent;
    given.hop_length = kQuarterHop;
    const HpssConfig config;

    const HpssAudioResult a = hpss(audio, config, absent);
    const HpssAudioResult g = hpss(audio, config, given);
    CHECK(samples_of(a.harmonic) == samples_of(g.harmonic));
    CHECK(samples_of(a.percussive) == samples_of(g.percussive));

    const HpssAudioResultWithResidual ar = hpss_with_residual(audio, config, absent);
    const HpssAudioResultWithResidual gr = hpss_with_residual(audio, config, given);
    CHECK(samples_of(ar.residual) == samples_of(gr.residual));
    CHECK(samples_of(ar.harmonic) == samples_of(gr.harmonic));

    CHECK(samples_of(harmonic(audio, config, absent)) ==
          samples_of(harmonic(audio, config, given)));
    CHECK(samples_of(percussive(audio, config, absent)) ==
          samples_of(percussive(audio, config, given)));
    CHECK(samples_of(residual(audio, config, absent)) ==
          samples_of(residual(audio, config, given)));
  }

  SECTION("griffin_lim, mel_to_audio and mfcc_to_audio") {
    const Spectrogram spec = Spectrogram::compute(audio, make_stft_config(kNFft, kQuarterHop));
    const std::vector<float>& magnitude = spec.magnitude();
    GriffinLimConfig gl;
    gl.n_iter = 2;
    CHECK(samples_of(
              griffin_lim(magnitude, spec.n_bins(), spec.n_frames(), kNFft, 0, kSampleRate, gl)) ==
          samples_of(griffin_lim(magnitude, spec.n_bins(), spec.n_frames(), kNFft, kQuarterHop,
                                 kSampleRate, gl)));

    MelConfig given;
    given.n_fft = kNFft;
    given.hop_length = kQuarterHop;
    given.n_mels = 40;
    MelConfig absent = given;
    absent.hop_length = 0;
    const MelSpectrogram mel = MelSpectrogram::compute(audio, given);
    const MatrixView<float> power = mel.power();
    CHECK(samples_of(
              mel_to_audio(power.data(), mel.n_mels(), mel.n_frames(), absent, 2, kSampleRate)) ==
          samples_of(
              mel_to_audio(power.data(), mel.n_mels(), mel.n_frames(), given, 2, kSampleRate)));

    const std::vector<float> mfcc = mel.mfcc(13);
    CHECK(samples_of(mfcc_to_audio(mfcc.data(), 13, mel.n_frames(), absent, 2, kSampleRate)) ==
          samples_of(mfcc_to_audio(mfcc.data(), 13, mel.n_frames(), given, 2, kSampleRate)));
  }

  SECTION("stems") {
    DecomposeStemsConfig absent;
    absent.n_fft = kNFft;
    absent.n_components = 2;
    absent.n_iter = 4;
    absent.init = "nndsvd";
    DecomposeStemsConfig given = absent;
    given.hop_length = kQuarterHop;
    const DecomposeStemsResult a = decompose_stems(audio.data(), audio.size(), kSampleRate, absent);
    const DecomposeStemsResult g = decompose_stems(audio.data(), audio.size(), kSampleRate, given);
    CHECK(a.components == g.components);
    CHECK(a.H == g.H);
  }

  SECTION("percussive events: extraction and rendering") {
    editing::event_model::PercussiveEventExtractorConfig absent;
    absent.separation.n_fft = kNFft;
    editing::event_model::PercussiveEventExtractorConfig given = absent;
    given.separation.hop_length = kQuarterHop;
    const auto a = editing::event_model::extract_percussive_events(audio, absent);
    const auto g = editing::event_model::extract_percussive_events(audio, given);
    REQUIRE(a.size() == g.size());
    REQUIRE_FALSE(a.empty());
    for (std::size_t i = 0; i < a.size(); ++i) {
      CHECK(a[i].onset_sample == g[i].onset_sample);
      CHECK(a[i].offset_sample == g[i].offset_sample);
    }

    auto events = a;
    events[0].edit.gain_db = -6.0f;
    editing::event_model::PercussiveEventRenderConfig render_absent;
    render_absent.separation = absent.separation;
    editing::event_model::PercussiveEventRenderConfig render_given;
    render_given.separation = given.separation;
    CHECK(
        samples_of(editing::event_model::render_percussive_events(audio, events, render_absent)) ==
        samples_of(editing::event_model::render_percussive_events(audio, events, render_given)));
  }

  SECTION("spectral edit") {
    SpectralEditConfig absent;
    absent.n_fft = kNFft;
    SpectralEditConfig given = absent;
    given.hop_length = kQuarterHop;
    SpectralRegionOp op;
    op.start_sample = 1000;
    op.end_sample = 6000;
    op.low_hz = 200.0f;
    op.high_hz = 2000.0f;
    op.gain_db = -12.0f;
    CHECK(samples_of(spectral_edit(audio, absent, &op, 1)) ==
          samples_of(spectral_edit(audio, given, &op, 1)));
  }
}
