/// @file wasm_float_saturation_test.cpp
/// @brief What happens to a WASM caller's finite number too wide for a float.
///
/// embind converts a positional `float` parameter in its own glue
/// (`toWireType: value => value`), and the wasm f32 parameter performs the
/// demotion, so a caller's `1e39` — finite in JS, past `FLT_MAX` — reaches C++
/// as an infinity with the caller's finiteness gone. Nothing in the WASM
/// wrapper marks the conversion, and a static scan of the bindings cannot see
/// what refuses it: the guard is nearly always a core validator or the C ABI's
/// own `finite()`, several call hops away and through a config struct.
///
/// So the refusals are asserted here, by driving the value. Cases are one per
/// distinct GUARD rather than one per parameter — the parameters route through
/// a much smaller set of refusal points, and a case per parameter would rot
/// the moment one is added. The float population's SIZE is pinned separately
/// and mechanically, by the per-file distribution in
/// `tests/conformance/wasm_narrowing_records.json`, so a parameter landing
/// outside every guard below reddens that check rather than joining an
/// ungraded population here.
///
/// Every case carries a legitimate-value control, most of them two values that
/// select observably different results: an assertion that only shows the throw
/// cannot tell a guard from an entry point that ignores its argument.
///
/// Guards inside `src/wasm/**` itself are NOT here and cannot be — those
/// translation units compile only under emscripten. They are driven through the
/// WASM surface instead.

#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "core/convert.h"
#include "core/db_convert.h"
#include "core/spectrum.h"
#include "core/synthesis.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "effects/decompose.h"
#include "effects/formant_warp.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "feature/cqt.h"
#include "feature/pitch.h"
#include "feature/rhythm.h"
#include "feature/spectral.h"
#include "feature/vqt.h"
#include "mastering/eq/equalizer.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "metering/basic.h"
#include "metering/clipping.h"
#include "metering/dynamic_range.h"
#include "streaming/stream_analyzer.h"
#include "util/exception.h"

using namespace sonare;

namespace {

/// The value the f32 parameter holds once embind has demoted a caller's 1e39.
/// Not written as a JS-side literal anywhere: by the time any C++ here could
/// observe it, the demotion has already happened.
constexpr float kSaturated = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

/// Whether a call refused its argument. Catches the project exception only, so
/// a crash or a foreign exception fails the case rather than reading as a
/// refusal.
template <typename Fn>
bool refuses(Fn&& call) {
  try {
    call();
  } catch (const SonareException&) {
    return true;
  }
  return false;
}

Audio test_tone(int sample_rate = 22050, std::size_t samples = 8192) {
  std::vector<float> data(samples);
  for (std::size_t i = 0; i < data.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    data[i] = 0.5f * std::sin(2.0f * constants::kPi * 440.0f * t);
  }
  return Audio::from_buffer(data.data(), data.size(), sample_rate);
}

Spectrogram test_spectrogram(const Audio& audio, int n_fft = 512, int hop = 128) {
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop;
  return Spectrogram::compute(audio, config);
}

}  // namespace

// ---------------------------------------------------------------------------
// Core guards
// ---------------------------------------------------------------------------

TEST_CASE("Stretch and shift refuse a saturated ratio", "[conformance][wasm]") {
  const Audio audio = test_tone();

  // timeStretch.rate, through checked_projected_count's finite_positive.
  REQUIRE(refuses([&] { time_stretch(audio, kSaturated, {}); }));
  const Audio slower = time_stretch(audio, 0.5f, {});
  const Audio faster = time_stretch(audio, 2.0f, {});
  REQUIRE(slower.size() > faster.size());

  // pitchShift.semitones, through make_pitch_shift_plan's finite.
  REQUIRE(refuses([&] { pitch_shift(audio, kSaturated, {}); }));
  REQUIRE_FALSE(refuses([&] { pitch_shift(audio, 3.0f, {}); }));
  REQUIRE_FALSE(refuses([&] { pitch_shift(audio, -3.0f, {}); }));

  // noteStretch.stretchRatio, NoteEditor's own finite check.
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = 0;
  region.offset_sample = 2048;
  const editing::pitch_editor::NoteEditor editor;
  REQUIRE(refuses([&] { editor.stretch_note(audio, region, kSaturated); }));
  REQUIRE(editor.stretch_note(audio, region, 2.0f).size() >
          editor.stretch_note(audio, region, 0.5f).size());
}

TEST_CASE("Normalization refuses a saturated target", "[conformance][wasm]") {
  const Audio audio = test_tone();
  REQUIRE(refuses([&] { normalize(audio, kSaturated); }));
  REQUIRE(refuses([&] { normalize_rms(audio, kSaturated, true); }));

  // Two legitimate targets produce two different peaks, so the argument is read.
  const Audio quiet = normalize(audio, -20.0f);
  const Audio loud = normalize(audio, -3.0f);
  REQUIRE(metering::peak_db(loud) > metering::peak_db(quiet));
}

TEST_CASE("The dB conversions refuse a saturated reference", "[conformance][wasm]") {
  const std::vector<float> values(16, 1.0f);
  REQUIRE(refuses([&] { power_to_db(values, kSaturated, 1e-10f, 80.0f); }));
  REQUIRE(refuses([&] { power_to_db(values, 1.0f, kSaturated, 80.0f); }));
  REQUIRE(refuses([&] { power_to_db(values, 1.0f, 1e-10f, kSaturated); }));
  REQUIRE(refuses([&] { amplitude_to_db(values, kSaturated, 1e-5f, 80.0f); }));
  REQUIRE(refuses([&] { db_to_power(values, kSaturated); }));
  REQUIRE(refuses([&] { db_to_amplitude(values, kSaturated); }));

  REQUIRE(power_to_db(values, 1.0f, 1e-10f, 80.0f)[0] !=
          power_to_db(values, 4.0f, 1e-10f, 80.0f)[0]);
}

TEST_CASE("The generators refuse a saturated duration or frequency", "[conformance][wasm]") {
  REQUIRE(refuses([&] { tone(440.0f, 22050, kSaturated, 0.0f, 1.0f); }));
  REQUIRE(refuses([&] { chirp(100.0f, 1000.0f, 22050, kSaturated, true); }));
  REQUIRE(refuses([&] { clicks({0.0f, 0.5f}, 22050, 0, kSaturated, 0.01f); }));
  REQUIRE(refuses([&] { clicks({0.0f, 0.5f}, 22050, 0, 1000.0f, kSaturated); }));

  REQUIRE(tone(440.0f, 22050, 0.2f, 0.0f, 1.0f).size() >
          tone(440.0f, 22050, 0.1f, 0.0f, 1.0f).size());
}

TEST_CASE("The tempo features refuse a saturated bound", "[conformance][wasm]") {
  const std::vector<float> envelope(512, 0.5f);

  TempogramConfig tempogram;
  tempogram.hop_length = 512;
  tempogram.win_length = 128;
  REQUIRE(refuses([&] { cyclic_tempogram(envelope, 22050, tempogram, kSaturated, 8); }));
  REQUIRE_FALSE(refuses([&] { cyclic_tempogram(envelope, 22050, tempogram, 30.0f, 8); }));

  PlpConfig plp_config;
  plp_config.sr = 22050;
  plp_config.hop_length = 512;
  plp_config.win_length = 128;
  plp_config.tempo_max = 240.0f;
  PlpConfig saturated_min = plp_config;
  saturated_min.tempo_min = kSaturated;
  PlpConfig saturated_max = plp_config;
  saturated_max.tempo_min = 30.0f;
  saturated_max.tempo_max = kSaturated;
  plp_config.tempo_min = 30.0f;
  REQUIRE(refuses([&] { plp(envelope, saturated_min); }));
  REQUIRE(refuses([&] { plp(envelope, saturated_max); }));
  REQUIRE_FALSE(refuses([&] { plp(envelope, plp_config); }));
}

TEST_CASE("The spectral features refuse a saturated shape argument", "[conformance][wasm]") {
  const Audio audio = test_tone();
  const Spectrogram spec = test_spectrogram(audio);

  // spectralBandwidth.p, named in its own finite_positive guard.
  REQUIRE(refuses([&] { spectral_bandwidth(spec, 22050, kSaturated); }));
  REQUIRE(spectral_bandwidth(spec, 22050, 2.0f)[0] != spectral_bandwidth(spec, 22050, 3.0f)[0]);

  // spectralRolloff.rollPercent, refused by the upper half of a two-sided test.
  REQUIRE(refuses([&] { spectral_rolloff(spec, 22050, kSaturated); }));
  REQUIRE(spectral_rolloff(spec, 22050, 0.85f)[0] != spectral_rolloff(spec, 22050, 0.25f)[0]);

  // spectralContrast.quantile, the same two-sided shape.
  REQUIRE(refuses([&] { spectral_contrast(spec, 22050, 4, 200.0f, kSaturated); }));

  // zeroCrossings.threshold.
  const std::vector<float> ramp = {-1.0f, 1.0f, -1.0f, 1.0f};
  REQUIRE(
      refuses([&] { zero_crossings(ramp.data(), ramp.size(), kSaturated, false, true, true); }));
}

TEST_CASE("spectralContrast refuses a saturated fmin, but not where it looks",
          "[conformance][wasm]") {
  // The guard that reads as fmin's -- `fmin > 0.0f` -- ADMITS an infinity. What
  // refuses it is the band-topology test below it, which is about whether the
  // highest band fits under nyquist and catches the infinity incidentally.
  // Pinned as its own case because the two are separable: simplifying the band
  // test would remove a refusal nothing names.
  const Audio audio = test_tone();
  const Spectrogram spec = test_spectrogram(audio);

  REQUIRE(kSaturated > 0.0f);  // the guard that does NOT do the work
  REQUIRE(refuses([&] { spectral_contrast(spec, 22050, 4, kSaturated, 0.02f); }));

  const std::vector<float> low = spectral_contrast(spec, 22050, 4, 100.0f, 0.02f);
  const std::vector<float> high = spectral_contrast(spec, 22050, 4, 400.0f, 0.02f);
  REQUIRE(low != high);
}

TEST_CASE("The pitch trackers refuse a saturated bound or threshold", "[conformance][wasm]") {
  const Audio audio = test_tone();

  PitchConfig config;
  config.frame_length = 1024;
  config.hop_length = 256;
  config.fmin = 80.0f;
  config.fmax = 2000.0f;
  config.threshold = 0.1f;

  // fmin is refused by the ordered-pair test rather than by a finiteness check:
  // `fmax > fmin` is false for an infinite fmin, and fmax cannot itself be
  // infinite because the facade narrows it through the checked reader.
  PitchConfig saturated_fmin = config;
  saturated_fmin.fmin = kSaturated;
  PitchConfig saturated_threshold = config;
  saturated_threshold.threshold = kSaturated;
  REQUIRE(refuses([&] { yin_track(audio, saturated_fmin); }));
  REQUIRE(refuses([&] { yin_track(audio, saturated_threshold); }));
  REQUIRE(refuses([&] { pyin(audio, saturated_fmin); }));
  REQUIRE_FALSE(refuses([&] { yin_track(audio, config); }));

  REQUIRE(refuses([&] { piptrack(audio, 512, 128, kSaturated, 2000.0f, 0.1f); }));
  REQUIRE_FALSE(refuses([&] { piptrack(audio, 512, 128, 80.0f, 2000.0f, 0.1f); }));

  // pitchTuning.resolution and estimateTuning.resolution share one guard.
  REQUIRE(refuses([&] { pitch_tuning({440.0f}, kSaturated, 12); }));
  REQUIRE(refuses([&] { estimate_tuning(audio, 512, 128, kSaturated, 12); }));
  REQUIRE(pitch_tuning({445.0f}, 0.01f, 12) != pitch_tuning({435.0f}, 0.01f, 12));
}

TEST_CASE("Key and chord analysis refuse a saturated configuration", "[conformance][wasm]") {
  const Audio audio = test_tone();

  // detectKey.highPassHz, refused by a bound relative to nyquist.
  KeyConfig key;
  key.high_pass_hz = kSaturated;
  REQUIRE(refuses([&] { detect_key(audio, key); }));
  KeyConfig legitimate;
  legitimate.high_pass_hz = 60.0f;
  REQUIRE_FALSE(refuses([&] { detect_key(audio, legitimate); }));

  // detectChords' three, all through one isfinite in the analyzer.
  ChordConfig chord;
  chord.n_fft = 2048;
  chord.hop_length = 512;
  for (float ChordConfig::*field :
       {&ChordConfig::min_duration, &ChordConfig::smoothing_window, &ChordConfig::threshold}) {
    ChordConfig saturated = chord;
    saturated.*field = kSaturated;
    REQUIRE(refuses([&] { detect_chords(audio, saturated); }));
  }
  REQUIRE_FALSE(refuses([&] { detect_chords(audio, chord); }));
}

TEST_CASE("Decomposition refuses a saturated beta", "[conformance][wasm]") {
  const std::vector<float> spectrogram(64, 1.0f);
  REQUIRE(refuses([&] { decompose(spectrogram.data(), 8, 8, 2, 4, "mu", kSaturated); }));
  REQUIRE_FALSE(refuses([&] { decompose(spectrogram.data(), 8, 8, 2, 4, "mu", 1.0f); }));
}

TEST_CASE("The voice changer refuses a saturated pitch or formant", "[conformance][wasm]") {
  const Audio audio = test_tone();

  // The two halves live in different classes: the pitch check is the voice
  // changer's own, the formant check is inside FormantWarp.
  FormantWarpConfig warp;
  warp.factor = kSaturated;
  REQUIRE(refuses([&] { FormantWarp(warp).process(audio); }));
  FormantWarpConfig legitimate;
  legitimate.factor = 1.2f;
  REQUIRE_FALSE(refuses([&] { FormantWarp(legitimate).process(audio); }));

  const editing::pitch_editor::PitchCorrector corrector;
  REQUIRE(refuses([&] { corrector.correct_to_midi(audio, kSaturated, 60.0f); }));
  REQUIRE(refuses([&] { corrector.correct_to_midi(audio, 60.0f, kSaturated); }));
  REQUIRE_FALSE(refuses([&] { corrector.correct_to_midi(audio, 60.0f, 62.0f); }));
}

TEST_CASE("Metering refuses a saturated window or threshold", "[conformance][wasm]") {
  const Audio audio = test_tone();

  REQUIRE(refuses([&] { metering::clipping_params_from_public(kSaturated, 0); }));
  REQUIRE_FALSE(refuses([&] { metering::clipping_params_from_public(0.99f, 0); }));

  REQUIRE(refuses([&] { metering::silence_ratio(audio, kSaturated, 2048, 512); }));
  REQUIRE(metering::silence_ratio(audio, -80.0f, 2048, 512) !=
          metering::silence_ratio(audio, -1.0f, 2048, 512));

  REQUIRE(refuses([&] { metering::dynamic_range_config_from_public(kSaturated, 0, 0, 0); }));
  REQUIRE(refuses([&] { metering::dynamic_range_config_from_public(0, kSaturated, 0, 0); }));
  REQUIRE(refuses([&] { metering::dynamic_range_config_from_public(0, 0, kSaturated, 0); }));
  REQUIRE(refuses([&] { metering::dynamic_range_config_from_public(0, 0, 0, kSaturated); }));
}

TEST_CASE("Mastering refuses a saturated loudness target", "[conformance][wasm]") {
  using mastering::maximizer::validate_loudness_params;
  REQUIRE(refuses([&] { validate_loudness_params(kSaturated, -1.0f, 50.0f, 12.0f, 4); }));
  REQUIRE(refuses([&] { validate_loudness_params(-14.0f, kSaturated, 50.0f, 12.0f, 4); }));
  REQUIRE(refuses([&] { validate_loudness_params(-14.0f, -1.0f, kSaturated, 12.0f, 4); }));
  REQUIRE_FALSE(refuses([&] { validate_loudness_params(-14.0f, -1.0f, 50.0f, 12.0f, 4); }));
}

TEST_CASE("The streaming setters refuse a saturated value", "[conformance][wasm]") {
  StreamConfig config;
  StreamAnalyzer analyzer(config);
  REQUIRE(refuses([&] { analyzer.set_expected_duration(kSaturated); }));
  REQUIRE(refuses([&] { analyzer.set_normalization_gain(kSaturated); }));
  REQUIRE(refuses([&] { analyzer.set_tuning_ref_hz(kSaturated); }));
  REQUIRE_FALSE(refuses([&] { analyzer.set_expected_duration(30.0f); }));
  REQUIRE_FALSE(refuses([&] { analyzer.set_normalization_gain(2.0f); }));

  mastering::eq::EqualizerProcessor equalizer;
  REQUIRE(refuses([&] { equalizer.set_gain_scale(kSaturated); }));
  REQUIRE(refuses([&] { equalizer.set_output_gain_db(kSaturated); }));
  REQUIRE(refuses([&] { equalizer.set_output_pan(kSaturated); }));
  equalizer.set_output_gain_db(-6.0f);
  REQUIRE(equalizer.output_gain_db() == Catch::Approx(-6.0f));
  equalizer.set_output_pan(0.5f);
  REQUIRE(equalizer.output_pan() == Catch::Approx(0.5f));
}

// ---------------------------------------------------------------------------
// The VQT gamma, which is precise rather than lax
// ---------------------------------------------------------------------------

TEST_CASE("VQT gamma refuses an infinity and keeps NaN meaningful", "[conformance][wasm]") {
  // Guarded by isinf rather than isfinite, deliberately: NaN is the documented
  // spelling for "derive the bandwidth from the ERB scale", so a guard that
  // refused both would reject the convention's own value. Both halves are
  // asserted here so a later tightening to isfinite fails rather than passes.
  const Audio audio = test_tone();
  VqtConfig config;
  config.hop_length = 256;
  config.fmin = 65.4f;
  config.n_bins = 24;
  config.bins_per_octave = 12;

  VqtConfig saturated = config;
  saturated.gamma = kSaturated;
  VqtConfig automatic = config;
  automatic.gamma = kNaN;
  VqtConfig explicit_zero = config;
  explicit_zero.gamma = 0.0f;

  REQUIRE(refuses([&] { vqt(audio, saturated); }));
  REQUIRE_FALSE(refuses([&] { vqt(audio, automatic); }));
  REQUIRE_FALSE(refuses([&] { vqt(audio, explicit_zero); }));
}

// ---------------------------------------------------------------------------
// The conversions that are total on purpose
// ---------------------------------------------------------------------------

TEST_CASE("The scale conversions stay total", "[conformance][wasm]") {
  // The passthrough roster's other half. These five are documented as total on
  // every surface, so a saturated value propagates rather than throwing, and a
  // NaN survives because an unvoiced frame of a pitch track is spelled that way.
  // Asserted so that "routing every float through the checked reader" cannot be
  // applied here without a red test.
  REQUIRE(std::isinf(hz_to_mel(kSaturated)));
  REQUIRE(std::isinf(mel_to_hz(kSaturated)));
  REQUIRE(std::isinf(hz_to_midi(kSaturated)));
  REQUIRE(std::isinf(midi_to_hz(kSaturated)));
  REQUIRE(std::isnan(hz_to_midi(kNaN)));
  REQUIRE(hz_to_note(kSaturated) == "?");
  REQUIRE(hz_to_note(kNaN) == "?");

  REQUIRE(hz_to_midi(440.0f) == Catch::Approx(69.0f));
  REQUIRE(hz_to_midi(880.0f) == Catch::Approx(81.0f));
}

TEST_CASE("timeToFrames saturates rather than refusing, so its facade must",
          "[conformance][wasm]") {
  // The one parameter this sweep found unguarded on the WASM side. The defect is
  // not that an infinity arrives -- it is the value that comes BACK: a frame
  // index equal to INT_MAX, which nothing downstream can separate from a real
  // one. The core keeps this behaviour deliberately; the refusal therefore has
  // to be at the binding boundary, and this case is what says so.
  REQUIRE(time_to_frames(kSaturated, 22050, 512) == std::numeric_limits<int>::max());
  REQUIRE(time_to_frames(-kSaturated, 22050, 512) == std::numeric_limits<int>::min());
  REQUIRE(time_to_frames(1.0f, 22050, 512) == 43);
  REQUIRE(time_to_frames(2.0f, 22050, 512) == 86);
}

// ---------------------------------------------------------------------------
// C ABI guards, which every mixing and project float reaches
// ---------------------------------------------------------------------------

TEST_CASE("The C ABI refuses a saturated strip parameter", "[conformance][wasm]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 512);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "strip");
  REQUIRE(strip != nullptr);

  // One shared `finite()` stands in front of the whole family, so a
  // representative of each signature shape is enough; what is asserted is that
  // the refusal reaches the caller as a parameter error rather than as a
  // success carrying an infinity.
  REQUIRE(sonare_strip_set_fader_db(strip, kSaturated) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_input_trim_db(strip, kSaturated) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_pan(strip, kSaturated, -1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_width(strip, kSaturated) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_vca_offset_db(strip, kSaturated) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_dual_pan(strip, kSaturated, 0.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_dual_pan(strip, 0.0f, kSaturated) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_fader_automation(strip, 0, kSaturated, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_pan_automation(strip, 0, kSaturated, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_schedule_width_automation(strip, 0, kSaturated, 0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // The control: two legitimate values are both accepted and are not the same
  // call, so the refusals above are the argument's and not the handle's.
  REQUIRE(sonare_strip_set_fader_db(strip, -6.0f) == SONARE_OK);
  REQUIRE(sonare_strip_set_fader_db(strip, 0.0f) == SONARE_OK);
  REQUIRE(sonare_mixer_set_vca_group_gain_db(mixer, "absent", kSaturated) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_mixer_destroy(mixer);
}
