#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/fft.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_object.h"
#include "editing/note_model/note_renderer.h"
#include "editing/note_model/pitch_decomposition.h"
#include "editing/pitch_editor/f0_provider.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/types.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using sonare::ErrorCode;
using sonare::SonareException;
using sonare::editing::pitch_editor::F0Track;
using namespace sonare::editing::note_model;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr size_t kNoDifference = static_cast<size_t>(-1);

/// Cadence the decomposition cases build their curves at. No audio is involved,
/// so it is free of the renderer's sample rate.
constexpr double kCurveFrameRateHz = 100.0;

/// Tap count of the drift low pass: the -3 dB point of three cascaded centred
/// moving averages sits at this fraction of frame_rate / taps.
constexpr double kDriftCutoffRatio = 0.263;

// --- The decomposition's own arithmetic, as an oracle ---------------------

/// @brief Odd tap count the drift low pass uses for @p cutoff_hz over @p frames.
int drift_taps(double frame_rate_hz, double cutoff_hz, size_t frames) {
  long taps = std::lround(kDriftCutoffRatio * frame_rate_hz / cutoff_hz);
  if (taps < 1) taps = 1;
  if (taps % 2 == 0) ++taps;
  long limit = static_cast<long>(frames);
  if (limit % 2 == 0) --limit;
  if (limit >= 1 && taps > limit) taps = limit;
  return static_cast<int>(taps);
}

/// @brief Magnitude response of three cascaded centred moving averages.
/// @details The frequency-domain form of what the decomposition does in time,
///          so it is an independent oracle rather than a copy of it. Exact in
///          the interior, where endpoint replication cannot reach, and real and
///          positive below the first null -- which is what makes the zero-phase
///          claim testable as a sign comparison.
double drift_gain(double hz, int taps, double frame_rate_hz) {
  if (taps <= 1) return 1.0;
  const double numerator = std::sin(sonare::constants::kPiD * hz * taps / frame_rate_hz);
  const double denominator =
      static_cast<double>(taps) * std::sin(sonare::constants::kPiD * hz / frame_rate_hz);
  const double single = numerator / denominator;
  return single * single * single;
}

/// @brief Frames at each end that endpoint replication can still reach.
size_t drift_edge_frames(int taps) { return static_cast<size_t>(3 * (taps - 1) / 2 + 2); }

// --- Building a note whose pitch curve is known exactly --------------------

/// @brief One sinusoidal component of an injected pitch curve, in cents.
struct CurveComponent {
  double hz;
  double cents;
};

/// @brief Cents the components sum to at @p frame.
double injected_cents(const std::vector<CurveComponent>& components, double frame_rate_hz,
                      size_t frame, int frame_offset) {
  const double t = static_cast<double>(static_cast<int>(frame) + frame_offset) / frame_rate_hz;
  double cents = 0.0;
  for (const CurveComponent& component : components) {
    cents += component.cents * std::sin(sonare::constants::kTwoPiD * component.hz * t);
  }
  return cents;
}

/// @brief F0 values whose cents against @p centre_hz are exactly @p components.
std::vector<float> injected_f0(float centre_hz, double frame_rate_hz, size_t frames,
                               const std::vector<CurveComponent>& components,
                               int frame_offset = 0) {
  std::vector<float> values(frames, 0.0f);
  for (size_t i = 0; i < frames; ++i) {
    const double cents = injected_cents(components, frame_rate_hz, i, frame_offset);
    values[i] = static_cast<float>(static_cast<double>(centre_hz) *
                                   std::pow(2.0, cents / sonare::constants::kCentsPerOctave));
  }
  return values;
}

/// Nominal samples per frame for the spans these notes carry.
constexpr int64_t kSamplesPerFrame = 220;

/// @brief A note carrying @p values as its F0 curve, with a well-formed span.
NoteObject curve_note(float centre_hz, double frame_rate_hz, std::vector<float> values,
                      int frame_offset = 0) {
  NoteObject note;
  note.frame_start = frame_offset;
  note.frame_end = frame_offset + static_cast<int>(values.size());
  note.onset_sample = static_cast<int64_t>(note.frame_start) * kSamplesPerFrame;
  note.offset_sample = static_cast<int64_t>(note.frame_end) * kSamplesPerFrame;
  note.median_hz = centre_hz;
  note.median_cents =
      sonare::constants::kCentsPerOctave * std::log2(centre_hz / sonare::constants::kA4Hz);
  note.f0_hz.values = std::move(values);
  note.f0_hz.frame_rate_hz = static_cast<float>(frame_rate_hz);
  note.f0_hz.frame_offset = frame_offset;
  note.amplitude.values.assign(note.f0_hz.values.size(), 0.25f);
  note.amplitude.frame_rate_hz = note.f0_hz.frame_rate_hz;
  note.amplitude.frame_offset = frame_offset;
  note.f0_stability = 1.0f;
  return note;
}

/// @brief Cents of @p f0 against @p centre with unusable frames held from the
///        nearest usable neighbour: forward first, then a backward fill of a
///        leading run that had no usable predecessor.
/// @details Empty when no frame is usable, which is the empty-result case.
std::vector<float> held_cents(const std::vector<float>& f0, float centre) {
  std::vector<float> cents(f0.size(), 0.0f);
  std::vector<bool> usable(f0.size(), false);
  for (size_t i = 0; i < f0.size(); ++i) {
    usable[i] = f0[i] > 0.0f && std::isfinite(f0[i]);
    if (usable[i]) {
      cents[i] = static_cast<float>(sonare::constants::kCentsPerOctave *
                                    std::log2(static_cast<double>(f0[i]) / centre));
    }
  }

  size_t first = 0;
  while (first < f0.size() && !usable[first]) ++first;
  if (first == f0.size()) return {};

  float carried = cents[first];
  for (size_t i = first; i < cents.size(); ++i) {
    if (usable[i]) {
      carried = cents[i];
    } else {
      cents[i] = carried;
    }
  }
  for (size_t i = 0; i < first; ++i) cents[i] = cents[first];
  return cents;
}

// --- Reading a curve or a buffer ------------------------------------------

float peak_of(const std::vector<float>& values, size_t lo, size_t hi) {
  const size_t end = std::min(hi, values.size());
  float highest = 0.0f;
  for (size_t i = lo; i < end; ++i) highest = std::max(highest, std::abs(values[i]));
  return highest;
}

/// @brief Index of the largest signed value in [@p lo, @p hi).
size_t argmax_of(const std::vector<float>& values, size_t lo, size_t hi) {
  const size_t end = std::min(hi, values.size());
  REQUIRE(end > lo);
  size_t best = lo;
  for (size_t i = lo; i < end; ++i) {
    if (values[i] > values[best]) best = i;
  }
  return best;
}

bool all_finite(const std::vector<float>& values) {
  for (const float value : values) {
    if (!std::isfinite(value)) return false;
  }
  return true;
}

bool all_finite(const sonare::Audio& audio) {
  for (size_t i = 0; i < audio.size(); ++i) {
    if (!std::isfinite(audio[i])) return false;
  }
  return true;
}

float audio_peak(const sonare::Audio& audio, size_t lo, size_t hi) {
  const size_t end = std::min(hi, audio.size());
  float highest = 0.0f;
  for (size_t i = lo; i < end; ++i) highest = std::max(highest, std::abs(audio[i]));
  return highest;
}

/// @brief Index of the first differing sample in [@p lo, @p hi), or kNoDifference.
size_t first_sample_difference(const sonare::Audio& a, const sonare::Audio& b, size_t lo,
                               size_t hi) {
  const size_t end = std::min(hi, std::min(a.size(), b.size()));
  for (size_t i = lo; i < end; ++i) {
    if (a[i] != b[i]) return i;
  }
  return kNoDifference;
}

size_t first_sample_difference(const sonare::Audio& a, const sonare::Audio& b) {
  if (a.size() != b.size()) return 0;
  return first_sample_difference(a, b, 0, a.size());
}

template <typename Fn>
ErrorCode code_of(Fn&& call) {
  try {
    call();
  } catch (const SonareException& error) {
    return error.code();
  }
  return ErrorCode::Ok;
}

// --- Measuring vibrato in rendered audio -----------------------------------

constexpr int kRenderSampleRate = 22050;
constexpr int kRenderHop = 220;
constexpr double kRenderFrameRateHz =
    static_cast<double>(kRenderSampleRate) / static_cast<double>(kRenderHop);

/// @brief A sine whose instantaneous pitch swings @p depth_cents either side of
///        @p centre_hz at @p rate_hz.
sonare::Audio fm_tone(double centre_hz, double depth_cents, double rate_hz, double amplitude,
                      int samples) {
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);
  double phase = 0.0;
  for (int i = 0; i < samples; ++i) {
    output[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(phase));
    const double t = static_cast<double>(i) / kRenderSampleRate;
    const double cents = depth_cents * std::sin(sonare::constants::kTwoPiD * rate_hz * t);
    const double hz = centre_hz * std::pow(2.0, cents / sonare::constants::kCentsPerOctave);
    phase += sonare::constants::kTwoPiD * hz / kRenderSampleRate;
  }
  return sonare::Audio::from_vector(std::move(output), kRenderSampleRate);
}

/// @brief The F0 track describing @ref fm_tone frame by frame.
F0Track fm_track(double centre_hz, double depth_cents, double rate_hz, int frames) {
  F0Track track;
  track.sample_rate = kRenderSampleRate;
  track.hop_length = kRenderHop;
  track.f0_hz.resize(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kRenderFrameRateHz;
    const double cents = depth_cents * std::sin(sonare::constants::kTwoPiD * rate_hz * t);
    track.f0_hz[static_cast<size_t>(i)] =
        static_cast<float>(centre_hz * std::pow(2.0, cents / sonare::constants::kCentsPerOctave));
  }
  track.voiced.assign(static_cast<size_t>(frames), true);
  track.voiced_prob.assign(static_cast<size_t>(frames), 1.0f);
  return track;
}

double band_energy(const std::vector<float>& magnitude, double bin_hz, double lo_hz, double hi_hz) {
  const int first = std::max(0, static_cast<int>(std::ceil(lo_hz / bin_hz)));
  const int last = std::min(static_cast<int>(magnitude.size()) - 1,
                            static_cast<int>(std::floor(hi_hz / bin_hz)));
  double total = 0.0;
  for (int bin = first; bin <= last; ++bin) {
    const double value = magnitude[static_cast<size_t>(bin)];
    total += value * value;
  }
  return total;
}

/// @brief First-order FM sideband amplitude over carrier amplitude.
/// @details A pitch that wobbles at @p vibrato_hz puts sidebands one vibrato
///          rate either side of @p carrier_hz, and their ratio to the carrier is
///          J1(beta)/J0(beta) -- monotone in the modulation depth over the range
///          used here. Measuring the signal instead of re-deriving a pitch curve
///          keeps the check independent of the code that produced the edit.
double vibrato_sideband_ratio(const sonare::Audio& audio, double carrier_hz, double vibrato_hz) {
  constexpr int kNfft = 32768;
  const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(kNfft), audio.size()));
  REQUIRE(n > 1);

  std::vector<float> frame(static_cast<size_t>(kNfft), 0.0f);
  for (int i = 0; i < n; ++i) {
    const double window =
        0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * i / static_cast<double>(n - 1));
    frame[static_cast<size_t>(i)] = audio[static_cast<size_t>(i)] * static_cast<float>(window);
  }

  sonare::FFT fft(kNfft);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(frame.data(), spectrum.data());
  std::vector<float> magnitude(static_cast<size_t>(fft.n_bins()), 0.0f);
  for (int bin = 0; bin < fft.n_bins(); ++bin) {
    magnitude[static_cast<size_t>(bin)] = std::abs(spectrum[static_cast<size_t>(bin)]);
  }

  const double bin_hz = static_cast<double>(audio.sample_rate()) / kNfft;
  // Wide enough to hold the analysis window's main lobe, narrow enough to leave
  // a gap between the carrier band and either sideband band.
  const double half = 0.4 * vibrato_hz;
  const double carrier = band_energy(magnitude, bin_hz, carrier_hz - half, carrier_hz + half);
  const double lower = band_energy(magnitude, bin_hz, carrier_hz - vibrato_hz - half,
                                   carrier_hz - vibrato_hz + half);
  const double upper = band_energy(magnitude, bin_hz, carrier_hz + vibrato_hz - half,
                                   carrier_hz + vibrato_hz + half);
  REQUIRE(carrier > 0.0);
  return std::sqrt((lower + upper) / (2.0 * carrier));
}

// --- The composition table's fixture --------------------------------------

constexpr int kTableSamples = 6600;  // 30 frames, ~0.3 s
constexpr int kNoteAStart = 7;
constexpr int kNoteAEnd = 17;
constexpr int kNoteBStart = 20;
constexpr int kNoteBEnd = 30;
/// Stays clear of the default 5 ms edge cross-fade at both note boundaries.
constexpr size_t kSpanMargin = 220;

const std::vector<CurveComponent>& table_components() {
  static const std::vector<CurveComponent> components = {{5.5, 40.0}, {0.7, 25.0}};
  return components;
}

NoteObject table_note(int frame_start, int frame_end) {
  const size_t frames = static_cast<size_t>(frame_end - frame_start);
  return curve_note(
      220.0f, kRenderFrameRateHz,
      injected_f0(220.0f, kRenderFrameRateHz, frames, table_components(), frame_start),
      frame_start);
}

std::vector<NoteObject> table_notes() {
  return {table_note(kNoteAStart, kNoteAEnd), table_note(kNoteBStart, kNoteBEnd)};
}

sonare::Audio table_source() { return fm_tone(220.0, 40.0, 5.5, 0.4, kTableSamples); }

struct SampleRange {
  size_t lo;
  size_t hi;
};

/// @brief The stretches of the buffer no note's span covers, with a margin.
const std::vector<SampleRange>& outside_note_ranges() {
  static const std::vector<SampleRange> ranges = {
      {0, static_cast<size_t>(kNoteAStart) * kRenderHop - kSpanMargin},
      {static_cast<size_t>(kNoteAEnd) * kRenderHop + kSpanMargin,
       static_cast<size_t>(kNoteBStart) * kRenderHop - kSpanMargin}};
  return ranges;
}

/// @brief A note's own span, inset past the cross-fade at either edge.
SampleRange interior_of(const NoteObject& note) {
  return {static_cast<size_t>(note.onset_sample) + kSpanMargin,
          static_cast<size_t>(note.offset_sample) - kSpanMargin};
}

enum class OtherStage { None, Stretch, PitchShift, Formant, EnvelopeGain };

void apply_stage(NoteEdit& edit, OtherStage stage) {
  switch (stage) {
    case OtherStage::None:
      break;
    case OtherStage::Stretch:
      edit.time_stretch_ratio = 1.5f;
      break;
    case OtherStage::PitchShift:
      edit.pitch_shift_semitones = 3.0f;
      break;
    case OtherStage::Formant:
      edit.formant_shift_semitones = 2.0f;
      break;
    case OtherStage::EnvelopeGain:
      edit.amplitude_envelope = {0.2f, 1.0f};
      edit.gain_db = -6.0f;
      break;
  }
}

struct CompositionRow {
  float vibrato_change;
  float drift_change;
  OtherStage stage;
  bool muted;
  float cutoff_hz;
};

}  // namespace

// --- decompose_pitch: which curve a component lands in ---------------------

TEST_CASE("decompose_pitch puts a vibrato-rate oscillation in the vibrato curve",
          "[note_pitch_curve]") {
  constexpr size_t kFrames = 400;
  constexpr float kCentre = 220.0f;
  constexpr double kInjectedCents = 50.0;
  const std::vector<CurveComponent> components = {{5.5, kInjectedCents}};
  const NoteObject note = curve_note(kCentre, kCurveFrameRateHz,
                                     injected_f0(kCentre, kCurveFrameRateHz, kFrames, components));

  // The drift filter is gentle by design -- its -3 dB point is the stated cutoff
  // -- so a 5.5 Hz component cut at 3 Hz is split, not removed. The share is
  // derivable, so it is asserted rather than bounded loosely.
  PitchDecompositionConfig at_cutoff;
  at_cutoff.vibrato_cutoff_hz = 3.0f;
  const int taps = drift_taps(kCurveFrameRateHz, at_cutoff.vibrato_cutoff_hz, kFrames);
  REQUIRE(taps == 9);
  const double gain = drift_gain(5.5, taps, kCurveFrameRateHz);

  const PitchDecomposition split = decompose_pitch(note, at_cutoff);
  REQUIRE(split.drift.size() == kFrames);
  REQUIRE(split.vibrato.size() == kFrames);
  REQUIRE(all_finite(split.drift));
  REQUIRE(all_finite(split.vibrato));

  const size_t edge = drift_edge_frames(taps);
  const float vibrato_peak = peak_of(split.vibrato, edge, kFrames - edge);
  const float drift_peak = peak_of(split.drift, edge, kFrames - edge);
  REQUIRE_THAT(vibrato_peak, WithinRel(kInjectedCents * (1.0 - gain), 0.03));
  REQUIRE_THAT(drift_peak, WithinRel(kInjectedCents * gain, 0.03));
  // A filter wired the other way round swaps these two.
  REQUIRE(vibrato_peak > 2.0f * drift_peak);

  // Cut well below the oscillation the two separate cleanly, which is the
  // property a host relies on when it draws a vibrato.
  PitchDecompositionConfig below;
  below.vibrato_cutoff_hz = 1.5f;
  const int wide_taps = drift_taps(kCurveFrameRateHz, below.vibrato_cutoff_hz, kFrames);
  REQUIRE(wide_taps == 19);
  const PitchDecomposition clean = decompose_pitch(note, below);
  const size_t wide_edge = drift_edge_frames(wide_taps);
  REQUIRE_THAT(peak_of(clean.vibrato, wide_edge, kFrames - wide_edge),
               WithinRel(kInjectedCents, 0.03));
  REQUIRE(peak_of(clean.drift, wide_edge, kFrames - wide_edge) < 0.02f * kInjectedCents);
}

TEST_CASE("decompose_pitch puts a slow oscillation in the drift curve", "[note_pitch_curve]") {
  constexpr size_t kFrames = 400;
  constexpr float kCentre = 220.0f;
  constexpr double kInjectedCents = 40.0;
  const std::vector<CurveComponent> components = {{0.4, kInjectedCents}};
  const NoteObject note = curve_note(kCentre, kCurveFrameRateHz,
                                     injected_f0(kCentre, kCurveFrameRateHz, kFrames, components));

  PitchDecompositionConfig config;
  config.vibrato_cutoff_hz = 3.0f;
  const int taps = drift_taps(kCurveFrameRateHz, config.vibrato_cutoff_hz, kFrames);
  const double gain = drift_gain(0.4, taps, kCurveFrameRateHz);

  const PitchDecomposition split = decompose_pitch(note, config);
  const size_t edge = drift_edge_frames(taps);
  const float drift_peak = peak_of(split.drift, edge, kFrames - edge);
  const float vibrato_peak = peak_of(split.vibrato, edge, kFrames - edge);

  REQUIRE_THAT(drift_peak, WithinRel(kInjectedCents * gain, 0.01));
  REQUIRE_THAT(vibrato_peak, WithinRel(kInjectedCents * (1.0 - gain), 0.08));
  // The companion to the bound above: a swap would put the whole 40 cents here.
  REQUIRE(drift_peak > 20.0f * vibrato_peak);
}

TEST_CASE("decompose_pitch moves an oscillation between the curves as the cutoff crosses it",
          "[note_pitch_curve]") {
  // A host and the renderer disagreeing about the cutoff is a real failure mode,
  // so the cutoff has to demonstrably decide where the same curve lands.
  constexpr size_t kFrames = 400;
  constexpr float kCentre = 220.0f;
  constexpr double kInjectedCents = 45.0;
  const std::vector<CurveComponent> components = {{5.0, kInjectedCents}};
  const NoteObject note = curve_note(kCentre, kCurveFrameRateHz,
                                     injected_f0(kCentre, kCurveFrameRateHz, kFrames, components));

  PitchDecompositionConfig narrow;
  narrow.vibrato_cutoff_hz = 3.0f;
  PitchDecompositionConfig wide;
  wide.vibrato_cutoff_hz = 8.0f;

  const int narrow_taps = drift_taps(kCurveFrameRateHz, narrow.vibrato_cutoff_hz, kFrames);
  const int wide_taps = drift_taps(kCurveFrameRateHz, wide.vibrato_cutoff_hz, kFrames);
  REQUIRE(narrow_taps == 9);
  REQUIRE(wide_taps == 3);
  const double narrow_gain = drift_gain(5.0, narrow_taps, kCurveFrameRateHz);
  const double wide_gain = drift_gain(5.0, wide_taps, kCurveFrameRateHz);

  const PitchDecomposition below = decompose_pitch(note, narrow);
  const PitchDecomposition above = decompose_pitch(note, wide);

  // One window for both, so the two are read the same way.
  const size_t edge = drift_edge_frames(narrow_taps);
  const float below_vibrato = peak_of(below.vibrato, edge, kFrames - edge);
  const float below_drift = peak_of(below.drift, edge, kFrames - edge);
  const float above_vibrato = peak_of(above.vibrato, edge, kFrames - edge);
  const float above_drift = peak_of(above.drift, edge, kFrames - edge);

  // The crossover itself: which curve holds the majority flips with the cutoff.
  REQUIRE(below_vibrato > below_drift);
  REQUIRE(above_drift > above_vibrato);
  // Stated as a comparison between the two results, which is what a mismatched
  // cutoff would break. Bounding one of them alone would not.
  REQUIRE(above_drift > 2.0f * below_drift);
  REQUIRE(below_vibrato > 4.0f * above_vibrato);

  REQUIRE_THAT(below_drift, WithinRel(kInjectedCents * narrow_gain, 0.03));
  REQUIRE_THAT(above_drift, WithinRel(kInjectedCents * wide_gain, 0.03));
}

// --- decompose_pitch: the reconstruction identity --------------------------

TEST_CASE("decompose_pitch reconstructs the note's own cents curve exactly", "[note_pitch_curve]") {
  constexpr size_t kFrames = 400;
  constexpr float kCentre = 196.0f;
  const std::vector<CurveComponent> components = {{0.5, 60.0}, {5.5, 40.0}};
  const std::vector<float> f0 = injected_f0(kCentre, kCurveFrameRateHz, kFrames, components);
  const NoteObject note = curve_note(kCentre, kCurveFrameRateHz, f0);

  PitchDecompositionConfig config;
  config.vibrato_cutoff_hz = 3.0f;
  const PitchDecomposition split = decompose_pitch(note, config);
  REQUIRE(split.drift.size() == kFrames);
  REQUIRE(split.vibrato.size() == kFrames);
  REQUIRE_THAT(split.centre_hz, WithinAbs(kCentre, 1.0e-4f));

  const std::vector<float> cents = held_cents(f0, kCentre);
  REQUIRE(cents.size() == kFrames);

  float worst = 0.0f;
  float lowest = cents[0];
  float highest = cents[0];
  for (size_t i = 0; i < kFrames; ++i) {
    worst = std::max(worst, std::abs(split.drift[i] + split.vibrato[i] - cents[i]));
    lowest = std::min(lowest, cents[i]);
    highest = std::max(highest, cents[i]);
  }
  REQUIRE(worst < 1.0e-3f);

  // Two zero curves satisfy the identity too, so both have to carry something.
  REQUIRE(peak_of(split.vibrato, 0, kFrames) > 10.0f);
  REQUIRE(peak_of(split.drift, 0, kFrames) > 10.0f);

  // An average of the curve's own values, however weighted and however the edges
  // are replicated, cannot leave the curve's range.
  REQUIRE(*std::min_element(split.drift.begin(), split.drift.end()) >= lowest - 1.0e-3f);
  REQUIRE(*std::max_element(split.drift.begin(), split.drift.end()) <= highest + 1.0e-3f);
}

TEST_CASE("decompose_pitch leaves the recovered vibrato in phase with the note's",
          "[note_pitch_curve]") {
  // A causal filter would slide the vibrato later in time while leaving its
  // amplitude alone, so an amplitude check cannot see it.
  constexpr size_t kFrames = 400;
  constexpr float kCentre = 220.0f;
  const std::vector<CurveComponent> components = {{5.5, 50.0}};
  const NoteObject note = curve_note(kCentre, kCurveFrameRateHz,
                                     injected_f0(kCentre, kCurveFrameRateHz, kFrames, components));

  PitchDecompositionConfig config;
  config.vibrato_cutoff_hz = 3.0f;
  const int taps = drift_taps(kCurveFrameRateHz, config.vibrato_cutoff_hz, kFrames);
  const PitchDecomposition split = decompose_pitch(note, config);

  std::vector<float> injected(kFrames, 0.0f);
  for (size_t i = 0; i < kFrames; ++i) {
    injected[i] = static_cast<float>(injected_cents(components, kCurveFrameRateHz, i, 0));
  }

  const size_t edge = drift_edge_frames(taps);
  const size_t recovered_peak = argmax_of(split.vibrato, edge, kFrames - edge);
  const size_t injected_peak = argmax_of(injected, edge, kFrames - edge);
  const size_t apart = recovered_peak > injected_peak ? recovered_peak - injected_peak
                                                      : injected_peak - recovered_peak;
  INFO("recovered peak at " << recovered_peak << ", injected at " << injected_peak);
  REQUIRE(apart <= 1);

  // One frame of the 18 in a vibrato cycle, so the bound above already rules out
  // a lag; the sign sweep rules out one that happens to land near a peak.
  int compared = 0;
  for (size_t i = edge; i < kFrames - edge; i += 7) {
    if (std::abs(injected[i]) < 5.0f) continue;
    INFO("frame " << i);
    REQUIRE((split.vibrato[i] > 0.0f) == (injected[i] > 0.0f));
    ++compared;
  }
  REQUIRE(compared > 40);
}

TEST_CASE("decompose_pitch degenerates to drift-only when the cutoff resolves to one tap",
          "[note_pitch_curve]") {
  // 0.263 * 20 / 8 rounds to a single tap, which is a legal filter: it passes
  // the curve through, so there is no vibrato left to report.
  constexpr size_t kFrames = 60;
  constexpr double kFrameRateHz = 20.0;
  constexpr float kCentre = 174.61f;
  const std::vector<CurveComponent> components = {{3.0, 40.0}};
  const std::vector<float> f0 = injected_f0(kCentre, kFrameRateHz, kFrames, components);
  const NoteObject note = curve_note(kCentre, kFrameRateHz, f0);

  PitchDecompositionConfig config;
  config.vibrato_cutoff_hz = 8.0f;
  REQUIRE(drift_taps(kFrameRateHz, config.vibrato_cutoff_hz, kFrames) == 1);

  const PitchDecomposition split = decompose_pitch(note, config);
  REQUIRE(split.drift.size() == kFrames);
  REQUIRE(split.vibrato.size() == kFrames);

  const std::vector<float> cents = held_cents(f0, kCentre);
  float worst = 0.0f;
  for (size_t i = 0; i < kFrames; ++i) {
    // Exact: a one-tap average is the identity, so nothing is subtracted.
    REQUIRE(split.vibrato[i] == 0.0f);
    worst = std::max(worst, std::abs(split.drift[i] - cents[i]));
  }
  REQUIRE(worst < 1.0e-3f);
  REQUIRE(peak_of(split.drift, 0, kFrames) > 10.0f);
}

TEST_CASE("decompose_pitch clamps the drift filter to a note shorter than it",
          "[note_pitch_curve]") {
  // 0.263 * 100 / 0.5 asks for 53 taps over a 21-frame note. Clamping is what
  // keeps the filter from reading past either end of the curve.
  constexpr size_t kFrames = 21;
  constexpr float kCentre = 261.63f;
  const std::vector<CurveComponent> components = {{2.0, 55.0}};
  const std::vector<float> f0 = injected_f0(kCentre, kCurveFrameRateHz, kFrames, components);
  const NoteObject note = curve_note(kCentre, kCurveFrameRateHz, f0);

  PitchDecompositionConfig config;
  config.vibrato_cutoff_hz = 0.5f;
  REQUIRE(drift_taps(kCurveFrameRateHz, config.vibrato_cutoff_hz, kFrames) == 21);

  const PitchDecomposition split = decompose_pitch(note, config);
  REQUIRE(split.drift.size() == kFrames);
  REQUIRE(split.vibrato.size() == kFrames);
  REQUIRE(all_finite(split.drift));
  REQUIRE(all_finite(split.vibrato));

  const std::vector<float> cents = held_cents(f0, kCentre);
  float worst = 0.0f;
  for (size_t i = 0; i < kFrames; ++i) {
    worst = std::max(worst, std::abs(split.drift[i] + split.vibrato[i] - cents[i]));
  }
  REQUIRE(worst < 1.0e-3f);

  // Heavily smoothed rather than merely finite: the drift left of a filter this
  // wide is nearly flat, and the oscillation has to be in the other curve.
  const auto range = std::minmax_element(split.drift.begin(), split.drift.end());
  REQUIRE(*range.second - *range.first < 0.4f * peak_of(cents, 0, kFrames));
  REQUIRE(peak_of(split.vibrato, 0, kFrames) > 10.0f);
}

TEST_CASE("decompose_pitch holds an unusable frame at its nearest usable neighbour",
          "[note_pitch_curve]") {
  constexpr size_t kFrames = 300;
  constexpr float kCentre = 200.0f;
  const std::vector<CurveComponent> components = {{1.0, 30.0}, {5.5, 25.0}};
  std::vector<float> f0 = injected_f0(kCentre, kCurveFrameRateHz, kFrames, components);
  // A leading run with no usable predecessor, an interior run, a trailing run,
  // and one non-finite frame -- the four shapes a track spells unvoiced with.
  for (size_t i = 0; i < 5; ++i) f0[i] = 0.0f;
  for (size_t i = 140; i < 152; ++i) f0[i] = 0.0f;
  for (size_t i = 295; i < kFrames; ++i) f0[i] = 0.0f;
  f0[200] = kNaN;

  const NoteObject note = curve_note(kCentre, kCurveFrameRateHz, f0);
  PitchDecompositionConfig config;
  config.vibrato_cutoff_hz = 3.0f;
  const PitchDecomposition split = decompose_pitch(note, config);

  // Both curves still cover every frame, so a host indexes them by frame without
  // consulting the voicing.
  REQUIRE(split.drift.size() == kFrames);
  REQUIRE(split.vibrato.size() == kFrames);
  REQUIRE(all_finite(split.drift));
  REQUIRE(all_finite(split.vibrato));

  const std::vector<float> cents = held_cents(f0, kCentre);
  REQUIRE(cents.size() == kFrames);
  float worst = 0.0f;
  for (size_t i = 0; i < kFrames; ++i) {
    worst = std::max(worst, std::abs(split.drift[i] + split.vibrato[i] - cents[i]));
  }
  // The held frames are inside this too: log2(0) would be an infinity, and an
  // unheld zero would put the reconstruction hundreds of cents out.
  REQUIRE(worst < 1.0e-3f);
  REQUIRE(peak_of(split.vibrato, 0, kFrames) > 5.0f);
  REQUIRE(peak_of(split.drift, 0, kFrames) > 5.0f);
}

// --- decompose_pitch: the empty measurement and the bad argument ------------

TEST_CASE("decompose_pitch reports an empty result for a note carrying no usable pitch",
          "[note_pitch_curve]") {
  constexpr size_t kFrames = 60;
  constexpr float kCentre = 220.0f;
  constexpr int kOffset = 7;
  const std::vector<CurveComponent> components = {{5.0, 30.0}};
  const std::vector<float> f0 =
      injected_f0(kCentre, kCurveFrameRateHz, kFrames, components, kOffset);

  PitchDecompositionConfig config;
  config.vibrato_cutoff_hz = 4.0f;

  auto require_empty = [&](const NoteObject& note) {
    PitchDecomposition split;
    REQUIRE(code_of([&] { split = decompose_pitch(note, config); }) == ErrorCode::Ok);
    REQUIRE(split.centre_hz == 0.0f);
    REQUIRE(split.drift.empty());
    REQUIRE(split.vibrato.empty());
    // The empty path still describes where the curves would have sat.
    REQUIRE_THAT(split.frame_rate_hz, WithinAbs(static_cast<float>(kCurveFrameRateHz), 1.0e-4f));
    REQUIRE(split.frame_offset == kOffset);
    REQUIRE(split.config.vibrato_cutoff_hz == config.vibrato_cutoff_hz);
  };

  // No curve at all.
  NoteObject no_curve = curve_note(kCentre, kCurveFrameRateHz, {}, kOffset);
  REQUIRE(no_curve.f0_hz.values.empty());
  require_empty(no_curve);

  // A curve, but no centre to measure it against.
  for (const float centre : {0.0f, -5.0f, kNaN, kInf}) {
    NoteObject no_centre = curve_note(kCentre, kCurveFrameRateHz, f0, kOffset);
    no_centre.median_hz = centre;
    require_empty(no_centre);
  }

  // A centre, but not one usable frame to hold anything from.
  for (const float unusable : {0.0f, kNaN, kInf, -1.0f}) {
    NoteObject no_frames = curve_note(kCentre, kCurveFrameRateHz, f0, kOffset);
    no_frames.f0_hz.values.assign(kFrames, unusable);
    require_empty(no_frames);
  }

  // The same note with one usable frame put back is not an empty measurement,
  // so none of the above passes by rejecting every note.
  NoteObject one_frame = curve_note(kCentre, kCurveFrameRateHz, f0, kOffset);
  one_frame.f0_hz.values.assign(kFrames, 0.0f);
  one_frame.f0_hz.values[30] = kCentre;
  const PitchDecomposition split = decompose_pitch(one_frame, config);
  REQUIRE(split.centre_hz == kCentre);
  REQUIRE(split.drift.size() == kFrames);
  REQUIRE(split.vibrato.size() == kFrames);
}

TEST_CASE(
    "decompose_pitch reports an empty result rather than a cadence error when there is "
    "nothing to decompose",
    "[note_pitch_curve]") {
  // A note with nothing to decompose never reaches the frame-rate check: its
  // cadence describes a filter that would never have been designed. The rule is
  // uniform over every input that yields the empty result.
  constexpr size_t kFrames = 40;
  constexpr float kCentre = 220.0f;
  constexpr int kOffset = 11;
  const std::vector<CurveComponent> components = {{5.0, 30.0}};
  const std::vector<float> f0 =
      injected_f0(kCentre, kCurveFrameRateHz, kFrames, components, kOffset);

  PitchDecompositionConfig config;
  config.vibrato_cutoff_hz = 4.0f;

  auto require_empty = [&](const NoteObject& note, float frame_rate) {
    PitchDecomposition split;
    INFO("frame rate " << frame_rate);
    REQUIRE(code_of([&] { split = decompose_pitch(note, config); }) == ErrorCode::Ok);
    REQUIRE(split.centre_hz == 0.0f);
    REQUIRE(split.drift.empty());
    REQUIRE(split.vibrato.empty());
    REQUIRE(split.frame_rate_hz == frame_rate);
    REQUIRE(split.frame_offset == kOffset);
    REQUIRE(split.config.vibrato_cutoff_hz == config.vibrato_cutoff_hz);
  };

  for (const float frame_rate : {0.0f, -100.0f}) {
    // A curve every frame of which is unusable.
    NoteObject unusable = curve_note(kCentre, kCurveFrameRateHz, f0, kOffset);
    unusable.f0_hz.values.assign(kFrames, 0.0f);
    unusable.f0_hz.frame_rate_hz = frame_rate;
    require_empty(unusable, frame_rate);

    // The other two ways to reach the empty result read the same way.
    NoteObject no_curve = curve_note(kCentre, kCurveFrameRateHz, {}, kOffset);
    no_curve.f0_hz.frame_rate_hz = frame_rate;
    require_empty(no_curve, frame_rate);

    NoteObject no_centre = curve_note(kCentre, kCurveFrameRateHz, f0, kOffset);
    no_centre.median_hz = 0.0f;
    no_centre.f0_hz.frame_rate_hz = frame_rate;
    require_empty(no_centre, frame_rate);

    // One usable frame put back, same bad cadence: the check sits below the
    // short circuits rather than having gone away.
    NoteObject one_frame = curve_note(kCentre, kCurveFrameRateHz, f0, kOffset);
    one_frame.f0_hz.values.assign(kFrames, 0.0f);
    one_frame.f0_hz.values[20] = kCentre * 1.05f;
    one_frame.f0_hz.frame_rate_hz = frame_rate;
    INFO("frame rate " << frame_rate);
    REQUIRE(code_of([&] { decompose_pitch(one_frame, config); }) == ErrorCode::InvalidParameter);
  }
}

TEST_CASE("decompose_pitch rejects a non-finite or non-positive cutoff and frame rate",
          "[note_pitch_curve]") {
  constexpr size_t kFrames = 200;
  constexpr float kCentre = 220.0f;
  const std::vector<CurveComponent> components = {{5.0, 30.0}};
  const NoteObject note = curve_note(kCentre, kCurveFrameRateHz,
                                     injected_f0(kCentre, kCurveFrameRateHz, kFrames, components));

  for (const float cutoff : {0.0f, -1.0f, -3.0f, kNaN, kInf, -kInf}) {
    PitchDecompositionConfig config;
    config.vibrato_cutoff_hz = cutoff;
    INFO("cutoff " << cutoff);
    REQUIRE(code_of([&] { decompose_pitch(note, config); }) == ErrorCode::InvalidParameter);
  }

  // A curve with pitch in it but no cadence: the cents are readable, the filter
  // it would be smoothed with is not designable.
  for (const float frame_rate : {0.0f, -100.0f, kNaN, kInf}) {
    NoteObject no_cadence = note;
    no_cadence.f0_hz.frame_rate_hz = frame_rate;
    INFO("frame rate " << frame_rate);
    REQUIRE(code_of([&] { decompose_pitch(no_cadence); }) == ErrorCode::InvalidParameter);
  }

  REQUIRE(code_of([&] { decompose_pitch(note); }) == ErrorCode::Ok);
}

TEST_CASE("decompose_pitch carries the note's cadence, offset and its own config back",
          "[note_pitch_curve]") {
  constexpr size_t kFrames = 120;
  constexpr float kCentre = 293.66f;
  constexpr int kOffset = 37;
  constexpr double kFrameRateHz = 86.13;
  const std::vector<CurveComponent> components = {{4.5, 35.0}};
  const NoteObject note =
      curve_note(kCentre, kFrameRateHz,
                 injected_f0(kCentre, kFrameRateHz, kFrames, components, kOffset), kOffset);

  PitchDecompositionConfig config;
  config.vibrato_cutoff_hz = 4.25f;
  const PitchDecomposition split = decompose_pitch(note, config);

  REQUIRE_THAT(split.frame_rate_hz, WithinAbs(static_cast<float>(kFrameRateHz), 1.0e-3f));
  REQUIRE(split.frame_offset == kOffset);
  REQUIRE(split.config.vibrato_cutoff_hz == config.vibrato_cutoff_hz);
  REQUIRE_THAT(split.centre_hz, WithinAbs(kCentre, 1.0e-3f));
  REQUIRE(split.drift.size() == kFrames);
  REQUIRE(split.vibrato.size() == kFrames);

  // The default config is echoed as the default, not as a zero.
  const PitchDecomposition defaulted = decompose_pitch(note);
  REQUIRE(defaulted.config.vibrato_cutoff_hz == PitchDecompositionConfig{}.vibrato_cutoff_hz);
  REQUIRE(defaulted.config.vibrato_cutoff_hz != config.vibrato_cutoff_hz);
}

// --- render_notes: the curve edits at their neutral value -------------------

TEST_CASE("render_notes reproduces the input bit for bit with both curve edits at zero",
          "[note_pitch_curve]") {
  const sonare::Audio audio = table_source();
  std::vector<NoteObject> notes = table_notes();
  for (NoteObject& note : notes) {
    note.edit.vibrato_depth_change = 0.0f;
    note.edit.drift_change = 0.0f;
    REQUIRE(note.edit.is_identity());
  }

  REQUIRE(first_sample_difference(render_notes(audio, notes), audio) == kNoDifference);

  // The cutoff is a stage that did not run, so it cannot have left a trace.
  NoteRenderConfig narrow;
  narrow.decomposition.vibrato_cutoff_hz = 3.0f;
  NoteRenderConfig wide;
  wide.decomposition.vibrato_cutoff_hz = 8.0f;
  REQUIRE(first_sample_difference(render_notes(audio, notes, narrow), audio) == kNoDifference);
  REQUIRE(first_sample_difference(render_notes(audio, notes, wide), audio) == kNoDifference);
}

TEST_CASE("render_notes reads the decomposition cutoff from its config", "[note_pitch_curve]") {
  // The companion to the identity above: the field it says has no effect at zero
  // must have one when the edit is real.
  const sonare::Audio audio = table_source();

  NoteRenderConfig narrow;
  narrow.fade_ms = 0.0f;
  narrow.decomposition.vibrato_cutoff_hz = 3.0f;
  NoteRenderConfig wide = narrow;
  wide.decomposition.vibrato_cutoff_hz = 8.0f;

  NoteObject edited = table_note(kNoteAStart, kNoteAEnd);
  edited.edit.vibrato_depth_change = 1.0f;
  REQUIRE(first_sample_difference(render_notes(audio, {edited}, narrow),
                                  render_notes(audio, {edited}, wide)) != kNoDifference);

  // The same note edited only elsewhere is cutoff-blind again.
  NoteObject gained = table_note(kNoteAStart, kNoteAEnd);
  gained.edit.gain_db = -3.0f;
  REQUIRE(first_sample_difference(render_notes(audio, {gained}, narrow),
                                  render_notes(audio, {gained}, wide)) == kNoDifference);
}

// --- render_notes: what the edit does to the audio --------------------------

TEST_CASE("render_notes scales the vibrato a note was measured with", "[note_pitch_curve]") {
  constexpr int kSamples = 22000;  // 100 frames, ~1 s
  constexpr double kCentreHz = 220.0;
  constexpr double kVibratoHz = 5.5;
  constexpr double kDepthCents = 40.0;

  const sonare::Audio audio = fm_tone(kCentreHz, kDepthCents, kVibratoHz, 0.4, kSamples);
  const F0Track track = fm_track(kCentreHz, kDepthCents, kVibratoHz, kSamples / kRenderHop);
  const std::vector<NoteObject> notes = extract_notes(audio, track);
  REQUIRE(notes.size() == 1);
  REQUIRE(notes[0].onset_sample == 0);
  REQUIRE(notes[0].offset_sample == kSamples);

  NoteRenderConfig config;
  config.fade_ms = 0.0f;
  // Cut below the vibrato so the edit acts on all of it; at the 3 Hz default a
  // quarter of a 5.5 Hz oscillation is drift and would survive the edit.
  config.decomposition.vibrato_cutoff_hz = 1.5f;

  auto render_with = [&](float change) {
    std::vector<NoteObject> edited = notes;
    edited[0].edit.vibrato_depth_change = change;
    const sonare::Audio rendered = render_notes(audio, edited, config);
    REQUIRE(rendered.size() == audio.size());
    REQUIRE(all_finite(rendered));
    return rendered;
  };

  const double source = vibrato_sideband_ratio(audio, kCentreHz, kVibratoHz);
  const double doubled = vibrato_sideband_ratio(render_with(1.0f), kCentreHz, kVibratoHz);
  const double halved = vibrato_sideband_ratio(render_with(-0.5f), kCentreHz, kVibratoHz);
  const double flattened = vibrato_sideband_ratio(render_with(-1.0f), kCentreHz, kVibratoHz);
  INFO("source " << source << " doubled " << doubled << " halved " << halved << " flattened "
                 << flattened);

  // +-40 cents at 5.5 Hz is a modulation index of 0.92, whose first sidebands
  // sit at J1/J0 = 0.52 of the carrier.
  REQUIRE_THAT(source, WithinAbs(0.52, 0.03));
  // Not merely "the output changed": the depth asked for is the depth measured.
  // -0.5 leaves an index of 0.46 (0.24), +1 doubles it to 1.85 (1.86). Both are
  // two-sided, so an edit that overshoots fails as loudly as one that does
  // nothing.
  REQUIRE_THAT(halved, WithinAbs(0.24, 0.05));
  REQUIRE_THAT(doubled, WithinAbs(1.86, 0.20));
  // Floor of an unmodulated tone through this metric is 0.006; the rest of the
  // margin is PSOLA residual.
  REQUIRE(flattened < 0.10);
  // The ordering is what a wrong sign convention breaks.
  REQUIRE(doubled > source);
  REQUIRE(source > halved);
  REQUIRE(halved > flattened);
}

TEST_CASE("render_notes keeps the input's length for a vibrato-only edit", "[note_pitch_curve]") {
  // The resynthesis is duration-preserving, so a curve edit alone neither moves
  // the note nor resizes the buffer.
  const sonare::Audio audio = table_source();
  std::vector<NoteObject> notes = table_notes();
  for (NoteObject& note : notes) {
    note.edit.vibrato_depth_change = -1.0f;
    note.edit.drift_change = 0.5f;
  }

  const sonare::Audio rendered = render_notes(audio, notes);
  REQUIRE(rendered.size() == audio.size());
  REQUIRE(rendered.sample_rate() == audio.sample_rate());
  REQUIRE(all_finite(rendered));
  REQUIRE(audio_peak(rendered, 0, rendered.size()) < 4.0f);

  // The edited spans still carry the note, so "unchanged length" is not a
  // silenced buffer.
  for (const NoteObject& note : notes) {
    const SampleRange span = interior_of(note);
    REQUIRE(audio_peak(rendered, span.lo, span.hi) > 0.1f);
  }
  // Nothing moved: the untouched stretches are the source's samples.
  for (const SampleRange& range : outside_note_ranges()) {
    REQUIRE(first_sample_difference(audio, rendered, range.lo, range.hi) == kNoDifference);
  }
}

// --- render_notes: validation ----------------------------------------------

TEST_CASE("render_notes rejects a curve edit on a note with no pitch and renders its other edits",
          "[note_pitch_curve]") {
  // The pair is the point: the requirement belongs to the edit, not to the note.
  // Rejecting the note itself would make an unpitched span unrenderable.
  const sonare::Audio audio = table_source();

  NoteObject no_pitch = table_note(kNoteAStart, kNoteAEnd);
  no_pitch.median_hz = 0.0f;
  no_pitch.median_cents = 0.0f;

  for (const float change : {0.5f, -1.0f, 2.0f}) {
    NoteObject vibrato = no_pitch;
    vibrato.edit.vibrato_depth_change = change;
    INFO("vibrato_depth_change " << change);
    REQUIRE(code_of([&] { render_notes(audio, {vibrato}); }) == ErrorCode::InvalidParameter);

    NoteObject drift = no_pitch;
    drift.edit.drift_change = change;
    REQUIRE(code_of([&] { render_notes(audio, {drift}); }) == ErrorCode::InvalidParameter);
  }

  // The other half: the same note renders every other edit it is given.
  NoteObject gained = no_pitch;
  gained.edit.gain_db = -6.0206f;
  REQUIRE(gained.edit.vibrato_depth_change == 0.0f);
  REQUIRE(gained.edit.drift_change == 0.0f);
  sonare::Audio rendered;
  REQUIRE(code_of([&] { rendered = render_notes(audio, {gained}); }) == ErrorCode::Ok);
  REQUIRE(rendered.size() == audio.size());

  // The gain landed, so "renders fine" is not "renders nothing".
  const SampleRange span = interior_of(gained);
  const float source_peak = audio_peak(audio, span.lo, span.hi);
  REQUIRE(source_peak > 0.3f);
  REQUIRE_THAT(audio_peak(rendered, span.lo, span.hi) / source_peak, WithinAbs(0.5f, 0.05f));

  // The same rule reaches the other two things a curve edit needs.
  NoteObject no_curve = table_note(kNoteAStart, kNoteAEnd);
  no_curve.f0_hz.values.clear();
  no_curve.edit.vibrato_depth_change = 0.5f;
  REQUIRE(code_of([&] { render_notes(audio, {no_curve}); }) == ErrorCode::InvalidParameter);

  NoteObject no_cadence = table_note(kNoteAStart, kNoteAEnd);
  no_cadence.f0_hz.frame_rate_hz = 0.0f;
  no_cadence.edit.drift_change = 0.5f;
  REQUIRE(code_of([&] { render_notes(audio, {no_cadence}); }) == ErrorCode::InvalidParameter);
}

TEST_CASE("render_notes rejects a non-finite vibrato or drift change", "[note_pitch_curve]") {
  const sonare::Audio audio = table_source();

  for (const float bad : {kNaN, kInf, -kInf}) {
    NoteObject vibrato = table_note(kNoteAStart, kNoteAEnd);
    vibrato.edit.vibrato_depth_change = bad;
    REQUIRE_FALSE(vibrato.edit.is_identity());
    REQUIRE(code_of([&] { render_notes(audio, {vibrato}); }) == ErrorCode::InvalidParameter);

    NoteObject drift = table_note(kNoteAStart, kNoteAEnd);
    drift.edit.drift_change = bad;
    REQUIRE_FALSE(drift.edit.is_identity());
    REQUIRE(code_of([&] { render_notes(audio, {drift}); }) == ErrorCode::InvalidParameter);
  }

  // A wide but finite change is an edit, not an error -- neither field is bounded.
  for (const float wide : {-4.0f, 4.0f}) {
    NoteObject note = table_note(kNoteAStart, kNoteAEnd);
    note.edit.vibrato_depth_change = wide;
    note.edit.drift_change = -wide;
    REQUIRE(code_of([&] { render_notes(audio, {note}); }) == ErrorCode::Ok);
  }
}

TEST_CASE("render_notes silences a muted note without running the pitch curve stage",
          "[note_pitch_curve]") {
  const sonare::Audio audio = table_source();

  NoteObject muted = table_note(kNoteAStart, kNoteAEnd);
  muted.edit.muted = true;
  muted.edit.vibrato_depth_change = 1.5f;
  muted.edit.drift_change = -1.0f;

  sonare::Audio rendered;
  REQUIRE(code_of([&] { rendered = render_notes(audio, {muted}); }) == ErrorCode::Ok);
  REQUIRE(rendered.size() == audio.size());

  const SampleRange span = interior_of(muted);
  REQUIRE(audio_peak(audio, span.lo, span.hi) > 0.3f);
  REQUIRE(audio_peak(rendered, span.lo, span.hi) < 1.0e-6f);
  for (const SampleRange& range : outside_note_ranges()) {
    REQUIRE(first_sample_difference(audio, rendered, range.lo, range.hi) == kNoDifference);
  }

  // Bit-identical to the same mute with no curve edit: the stage did not run,
  // rather than running and being overwritten by the silence.
  NoteObject plain = table_note(kNoteAStart, kNoteAEnd);
  plain.edit.muted = true;
  REQUIRE(first_sample_difference(rendered, render_notes(audio, {plain})) == kNoDifference);
}

// --- render_notes: composing with the rest of the chain ---------------------

TEST_CASE("render_notes composes the curve edits with the rest of the per-note chain",
          "[note_pitch_curve]") {
  // A two-way covering array over the five factors that can interact: the curve
  // edits, one other stage, the mute, and the cutoff. Every pair of factor
  // values appears in some row, which hand-picked combinations do not give.
  static const std::vector<CompositionRow> kRows = {
      {0.0f, 0.0f, OtherStage::None, false, 3.0f},
      {0.0f, -1.0f, OtherStage::Stretch, true, 8.0f},
      {0.0f, 0.5f, OtherStage::PitchShift, false, 8.0f},
      {0.0f, 0.0f, OtherStage::Formant, true, 3.0f},
      {0.0f, -1.0f, OtherStage::EnvelopeGain, false, 8.0f},
      {-1.0f, -1.0f, OtherStage::None, true, 8.0f},
      {-1.0f, 0.5f, OtherStage::Stretch, false, 3.0f},
      {-1.0f, 0.0f, OtherStage::PitchShift, true, 3.0f},
      {-1.0f, 0.5f, OtherStage::Formant, true, 8.0f},
      {-1.0f, 0.0f, OtherStage::EnvelopeGain, true, 3.0f},
      {1.0f, 0.5f, OtherStage::None, false, 3.0f},
      {1.0f, 0.0f, OtherStage::Stretch, true, 8.0f},
      {1.0f, -1.0f, OtherStage::PitchShift, false, 8.0f},
      {1.0f, -1.0f, OtherStage::Formant, false, 3.0f},
      {1.0f, 0.5f, OtherStage::EnvelopeGain, false, 8.0f},
  };

  const sonare::Audio audio = table_source();
  REQUIRE(audio.size() == static_cast<size_t>(kTableSamples));

  auto notes_for = [](const CompositionRow& row, float vibrato_change, float drift_change) {
    std::vector<NoteObject> notes = table_notes();
    for (NoteObject& note : notes) {
      note.edit.vibrato_depth_change = vibrato_change;
      note.edit.drift_change = drift_change;
      note.edit.muted = row.muted;
      apply_stage(note.edit, row.stage);
    }
    return notes;
  };

  for (size_t index = 0; index < kRows.size(); ++index) {
    const CompositionRow& row = kRows[index];
    INFO("composition row " << (index + 1));

    NoteRenderConfig config;
    config.decomposition.vibrato_cutoff_hz = row.cutoff_hz;
    const std::vector<NoteObject> notes = notes_for(row, row.vibrato_change, row.drift_change);

    sonare::Audio rendered;
    REQUIRE(code_of([&] { rendered = render_notes(audio, notes, config); }) == ErrorCode::Ok);
    // The renderer's contract is the input's length whatever the note did to its
    // own span, so a lengthened note is written back into the same buffer.
    REQUIRE(rendered.size() == audio.size());
    REQUIRE(rendered.sample_rate() == audio.sample_rate());
    REQUIRE(all_finite(rendered));
    REQUIRE(audio_peak(rendered, 0, rendered.size()) < 4.0f);

    if (row.muted) {
      // A mute takes the note out of the chain entirely, so nothing the other
      // factors ask for can reach the buffer.
      for (const NoteObject& note : notes) {
        const SampleRange span = interior_of(note);
        REQUIRE(audio_peak(rendered, span.lo, span.hi) < 1.0e-6f);
      }
      for (const SampleRange& range : outside_note_ranges()) {
        REQUIRE(first_sample_difference(audio, rendered, range.lo, range.hi) == kNoDifference);
      }
      const std::vector<NoteObject> without = notes_for(row, 0.0f, 0.0f);
      REQUIRE(first_sample_difference(rendered, render_notes(audio, without, config)) ==
              kNoDifference);
    } else if (row.stage == OtherStage::Stretch) {
      // A lengthened note writes into its neighbour's samples by design, so only
      // the stretch of buffer ahead of the first note is still the source's.
      REQUIRE(first_sample_difference(audio, rendered, outside_note_ranges()[0].lo,
                                      outside_note_ranges()[0].hi) == kNoDifference);
    } else {
      for (const SampleRange& range : outside_note_ranges()) {
        REQUIRE(first_sample_difference(audio, rendered, range.lo, range.hi) == kNoDifference);
      }
    }

    if (row.vibrato_change == 0.0f && row.drift_change == 0.0f) {
      // The new fields do not disturb the old chain: with both at zero the stage
      // does not run, so the cutoff that configures it cannot move a sample.
      NoteRenderConfig other = config;
      other.decomposition.vibrato_cutoff_hz = row.cutoff_hz == 3.0f ? 8.0f : 3.0f;
      REQUIRE(first_sample_difference(rendered, render_notes(audio, notes, other)) ==
              kNoDifference);
    }
  }
}
