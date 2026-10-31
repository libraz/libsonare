/// @file singing_voice_test.cpp
/// @brief The singing engine (midi/synth/singing_voice) as a playable voice: the
///        breath couples loudness and timbre, the compensation table holds the
///        written pitch while the uncompensated fold rises, the vowel moves
///        continuously, a bare MIDI melody sings in tune, a slur carries the
///        breath, and the voice renders deterministically inside its RT budget.
///
/// The level and timbre claims subtract a control render that received no
/// controller, for the reason excitation_axis_test.cpp gives: the voice's own
/// contour moves both while a ramp runs. F0 is read by autocorrelation on 2048
/// sample windows at a 512 hop, inside the middle half of the sustain only.

#include "midi/synth/singing_voice.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/smf.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "singing_measure.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"
#include "support/midi_sequence_render.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::midi::ArticulationMode;
using sonare::midi::Velocity16;
using sonare::midi::synth::ExcitationAxes;
using sonare::midi::synth::kAxisForce;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::SingingPatchParams;
using sonare::midi::synth::SingingPhonation;
using sonare::midi::synth::SingingVoiceCore;
using sonare::test::event;
using sonare::test::render_left;
using sonare::test::spectral_centroid;
using sonare::test::TimedUmp;
using sonare::test::singing::cents_track;
using sonare::test::singing::frame_rms;
using sonare::test::singing::kRate;
using sonare::test::singing::singing_config;
using sonare::test::singing::span_rms;

/// A4, inside the range the compensation table covers with no formant nearby.
constexpr int kNote = 57;
constexpr double kNoteHz = 220.0;
constexpr int kVelocity = 100;
constexpr int kBlock = 256;
constexpr uint8_t kBreathCc = 2;

/// Render RMS under which a window is silence: the threshold the voice must cross.
constexpr double kSilenceRms = 1.0e-5;

NativeSynthConfig quiet_vibrato(NativeSynthConfig cfg) {
  cfg.patch.singing.vibrato_depth = 0.0f;
  return cfg;
}

sonare::midi::Ump cc(uint8_t controller, int value) {
  return sonare::midi::make_midi1_control_change(0, 0, controller,
                                                 static_cast<uint8_t>(std::clamp(value, 0, 127)));
}

sonare::midi::Ump note_on(int note, int velocity = kVelocity) {
  return sonare::midi::make_midi1_note_on(0, 0, static_cast<uint8_t>(note),
                                          static_cast<uint8_t>(velocity));
}

sonare::midi::Ump note_off(int note) {
  return sonare::midi::make_midi1_note_off(0, 0, static_cast<uint8_t>(note), 0);
}

double hz_of(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

/// A breath ramp held at 0.3 for a second, rising to full over two, held a
/// second: the middle half of the four-second sustain is exactly the ramp.
std::vector<float> render_force_ramp(const NativeSynthConfig& cfg) {
  std::vector<TimedUmp> events;
  events.push_back({0, cc(kBreathCc, 38)});
  events.push_back({0, note_on(kNote)});
  for (int at = kRate; at <= 3 * kRate; at += kBlock) {
    const double t = static_cast<double>(at - kRate) / (2.0 * kRate);
    events.push_back({at, cc(kBreathCc, static_cast<int>(std::lround(38.0 + 89.0 * t)))});
  }
  return sonare::test::render_sequence_mono(events, kRate, 4 * kRate, cfg, kBlock);
}

/// Resonance peaks of the vowel's tract, low to high, from its closed-glottis
/// impulse response on a 5 Hz grid between 100 Hz and 4 kHz.
std::vector<double> tract_resonances(float position, float brightness) {
  SingingPhonation tract;
  tract.configure_patch(kRate, static_cast<float>(kNoteHz), SingingPatchParams{});
  tract.set_constriction(position, brightness);
  std::vector<float> h;
  for (int n = 0; n < 8192; ++n) h.push_back(tract.probe_tract(n == 0 ? 1.0f : 0.0f));
  std::vector<double> mag;
  std::vector<double> freq;
  for (double f = 100.0; f <= 4000.0; f += 5.0) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < h.size(); ++n) {
      re += h[n] * std::cos(kTwoPiD * f * static_cast<double>(n) / kRate);
      im -= h[n] * std::sin(kTwoPiD * f * static_cast<double>(n) / kRate);
    }
    mag.push_back(std::hypot(re, im));
    freq.push_back(f);
  }
  std::vector<double> peaks;
  for (std::size_t i = 1; i + 1 < mag.size(); ++i) {
    if (mag[i] > mag[i - 1] && mag[i] >= mag[i + 1]) peaks.push_back(freq[i]);
  }
  return peaks;
}

/// The first two resonance peaks of a voiced window, read off its harmonics: the
/// strongest harmonic in each band, refined by a parabola through its log level
/// and its neighbours'. A harmonic voice samples its envelope only at k F0.
std::vector<double> harmonic_formants(const std::vector<float>& x, std::size_t from,
                                      std::size_t n) {
  const double f0 = sonare::test::singing::acf_f0(x.data() + from, 4096);
  REQUIRE(f0 > 0.0);
  std::vector<double> level;
  for (int k = 1; k * f0 < 3500.0; ++k) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double w = 0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) / (n - 1));
      re += w * x[from + i] * std::cos(kTwoPiD * k * f0 * static_cast<double>(i) / kRate);
      im -= w * x[from + i] * std::sin(kTwoPiD * k * f0 * static_cast<double>(i) / kRate);
    }
    level.push_back(std::log(std::hypot(re, im) + 1.0e-30));
  }
  std::vector<double> out;
  for (const auto& band : {std::pair<double, double>{250.0, 1000.0}, {1000.0, 3000.0}}) {
    int best = -1;
    for (int k = 1; k + 1 < static_cast<int>(level.size()); ++k) {
      const double f = (k + 1) * f0;
      if (f < band.first || f >= band.second) continue;
      if (best < 0 || level[k] > level[best]) best = k;
    }
    if (best < 1) continue;
    const double den = level[best - 1] - 2.0 * level[best] + level[best + 1];
    const double shift = den != 0.0 ? 0.5 * (level[best - 1] - level[best + 1]) / den : 0.0;
    out.push_back((best + 1 + shift) * f0);
  }
  return out;
}

std::string series(const std::vector<double>& values) {
  std::ostringstream out;
  for (const double v : values) out << v << ' ';
  return out.str();
}

/// A format-0 SMF at 480 PPQ and the default 120 BPM: @p notes as quarter notes,
/// each released as the next sounds, at velocity @p velocity. No controller at all.
std::vector<uint8_t> melody_smf(const std::vector<int>& notes, int velocity) {
  std::vector<uint8_t> track;
  const auto delta = [&](uint32_t ticks) {
    uint8_t bytes[4];
    int count = 0;
    bytes[count++] = static_cast<uint8_t>(ticks & 0x7Fu);
    while ((ticks >>= 7) != 0) bytes[count++] = static_cast<uint8_t>(0x80u | (ticks & 0x7Fu));
    while (count > 0) track.push_back(bytes[--count]);
  };
  for (const int note : notes) {
    delta(0);
    track.insert(track.end(), {0x90, static_cast<uint8_t>(note), static_cast<uint8_t>(velocity)});
    delta(480);
    track.insert(track.end(), {0x80, static_cast<uint8_t>(note), 0x00});
  }
  delta(0);
  track.insert(track.end(), {0xFF, 0x2F, 0x00});
  std::vector<uint8_t> file = {'M', 'T', 'h', 'd',  0,    0,   0,   6,   0,
                               0,   0,   1,   0x01, 0xE0, 'M', 'T', 'r', 'k'};
  const uint32_t size = static_cast<uint32_t>(track.size());
  for (int shift = 24; shift >= 0; shift -= 8) file.push_back(static_cast<uint8_t>(size >> shift));
  file.insert(file.end(), track.begin(), track.end());
  return file;
}

/// The SMF's events at sample positions (120 BPM: half a second per quarter).
std::vector<TimedUmp> smf_events(const std::vector<uint8_t>& smf, int* controller_events) {
  const sonare::midi::SmfImportResult imported = sonare::midi::import_smf(smf);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  std::vector<TimedUmp> out;
  *controller_events = 0;
  for (const sonare::midi::MidiClipEvent& e : imported.clips[0].events()) {
    // The channel-voice status nibble, the same place in a MIDI 1.0 and a 2.0 packet.
    if (((e.ump.words[0] >> 20) & 0x0Fu) == 0xBu) ++*controller_events;
    out.push_back({static_cast<int64_t>(std::lround(e.ppq * 0.5 * kRate)), e.ump});
  }
  return out;
}

/// The melody the default-articulation cases sing: eight quarter notes inside
/// the compensated range.
const std::vector<int> kMelody = {57, 59, 60, 57, 55, 52, 53, 57};

struct MelodyCheck {
  bool every_note_sounds = true;
  bool every_note_in_tune = true;
  bool breath_never_zero = true;
  double worst_cents = 0.0;
};

/// The three halves of the bare-MIDI gate, read over each note's own span.
MelodyCheck check_melody(const std::vector<float>& x) {
  MelodyCheck out;
  const std::size_t note_len = kRate / 2;
  for (std::size_t i = 0; i < kMelody.size(); ++i) {
    const std::size_t start = i * note_len;
    CAPTURE(i, kMelody[i]);
    // (a) the note sounds.
    if (!(span_rms(x, start + note_len / 4, start + 3 * note_len / 4) > 1.0e-3)) {
      out.every_note_sounds = false;
    }
    // (b) at its written pitch: the mean over the note past its onset, which a
    // vibrato swings around rather than away from.
    const std::vector<double> cents =
        cents_track(x, start + note_len / 5, start + note_len - note_len / 25, hz_of(kMelody[i]));
    double mean = 0.0;
    for (const double c : cents) mean += c;
    mean /= static_cast<double>(std::max<std::size_t>(cents.size(), 1));
    out.worst_cents = std::max(out.worst_cents, std::fabs(mean));
    if (cents.empty() || std::fabs(mean) > 20.0) out.every_note_in_tune = false;
    // (c) and the breath never falls to nothing inside it.
    for (std::size_t at = start + note_len / 10; at + 2048 <= start + note_len; at += 512) {
      if (!(span_rms(x, at, at + 2048) > kSilenceRms)) out.breath_never_zero = false;
    }
  }
  return out;
}

}  // namespace

TEST_CASE("singing breath raises level and spectral centroid against a no-CC control",
          "[midi][synth][singing]") {
  NativeSynthConfig cfg = quiet_vibrato(singing_config());
  // The wrapper envelope at its default sustain, so the coupling is shown with a VCA below unity.
  cfg.patch.amp_env.sustain = 0.7f;
  cfg.patch.amp_env.decay_ms = 100.0f;
  NativeSynth ramped(cfg);
  NativeSynth control(cfg);
  for (NativeSynth* s : {&ramped, &control}) {
    s->prepare(kRate, kBlock);
    s->on_event(0, event(note_on(kNote)));
    render_left(*s, kRate / 2);
  }
  std::vector<double> level;
  std::vector<double> centroid;
  std::vector<double> difference;
  for (int seg = 0; seg < 8; ++seg) {
    ramped.on_event(0, event(cc(kBreathCc, 40 + (seg * 87) / 7)));
    render_left(ramped, 4096);
    render_left(control, 4096);
    const std::vector<float> a = render_left(ramped, 8192);
    const std::vector<float> b = render_left(control, 8192);
    std::vector<float> diff(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) diff[i] = a[i] - b[i];
    level.push_back(span_rms(a, 0, a.size()) - span_rms(b, 0, b.size()));
    centroid.push_back(spectral_centroid(a, 0) - spectral_centroid(b, 0));
    difference.push_back(span_rms(diff, 0, diff.size()));
  }
  CAPTURE(series(level), series(centroid), series(difference));
  // The control subtraction is the primary guard; the absolute floor keeps two
  // silent renders, which subtract to an exact zero, from passing it.
  REQUIRE(*std::max_element(difference.begin(), difference.end()) > 1.0e-2);
  for (std::size_t i = 1; i < level.size(); ++i) {
    CAPTURE(i);
    REQUIRE(level[i] > level[i - 1]);
    REQUIRE(centroid[i] > centroid[i - 1]);
  }
}

TEST_CASE("compensated singing F0 holds the written pitch across a breath ramp",
          "[midi][synth][singing]") {
  NativeSynthConfig cfg = quiet_vibrato(singing_config());
  REQUIRE(cfg.patch.singing.pitch_compensation == 1.0f);
  const std::vector<float> x = render_force_ramp(cfg);
  const std::vector<double> cents = cents_track(x, kRate, 3 * kRate, kNoteHz);
  CAPTURE(series(cents));
  REQUIRE_FALSE(cents.empty());
  for (const double c : cents) REQUIRE(std::fabs(c) <= 20.0);
}

TEST_CASE("uncompensated singing F0 rises with the breath", "[midi][synth][singing]") {
  // The non-vacuity of the case above: with the table weighted out, the same
  // ramp moves F0 by more than a semitone, so holding it took the table.
  NativeSynthConfig cfg = quiet_vibrato(singing_config());
  cfg.patch.singing.pitch_compensation = 0.0f;
  const std::vector<float> x = render_force_ramp(cfg);
  const std::vector<double> cents = cents_track(x, kRate, 3 * kRate, kNoteHz);
  CAPTURE(series(cents));
  REQUIRE(cents.size() > 8);
  const double start = (cents[0] + cents[1]) / 2.0;
  const double end = (cents[cents.size() - 1] + cents[cents.size() - 2]) / 2.0;
  REQUIRE(end - start > 100.0);
}

TEST_CASE("singing vowel sweeps move the tract resonances continuously", "[midi][synth][singing]") {
  // Each sweep has to move a resonance a long way in total, or steps too small
  // to see would pass for continuity; and no single step may jump.
  const auto sweep = [](auto shape) {
    std::vector<std::vector<double>> track;
    for (int step = 0; step <= 80; ++step) {
      const std::vector<double> peaks = shape(step / 80.0f);
      REQUIRE(peaks.size() >= 2);
      track.push_back({peaks[0], peaks[1]});
    }
    double travel = 0.0;
    for (std::size_t i = 1; i < track.size(); ++i) {
      for (int k = 0; k < 2; ++k) {
        CAPTURE(i, k, track[i - 1][k], track[i][k]);
        REQUIRE(std::fabs(track[i][k] - track[i - 1][k]) <= 0.06 * track[i - 1][k]);
        travel = std::max(travel, std::fabs(track[i][k] - track[0][k]) / track[0][k]);
      }
    }
    REQUIRE(travel > 0.3);
  };
  SECTION("constriction place, back to front, at a narrow vowel") {
    sweep([](float t) { return tract_resonances(0.1f + 0.8f * t, 0.6f); });
  }
  SECTION("constriction degree, open to narrow, at the rest place") {
    const SingingPatchParams defaults;
    sweep([&](float t) { return tract_resonances(defaults.position, 1.0f - 0.5f * t); });
  }
}

TEST_CASE("singing renders bit-identically from the same notes and controllers",
          "[midi][synth][singing]") {
  std::vector<TimedUmp> events = {
      {0, note_on(kNote)},          {kRate / 4, cc(kBreathCc, 90)},  {kRate / 3, cc(74, 40)},
      {kRate / 2, note_off(kNote)}, {kRate / 2, note_on(kNote + 2)}, {kRate, note_off(kNote + 2)}};
  const NativeSynthConfig cfg = singing_config();
  const std::vector<float> a =
      sonare::test::render_sequence_mono(events, kRate, kRate * 3 / 2, cfg);
  const std::vector<float> b =
      sonare::test::render_sequence_mono(events, kRate, kRate * 3 / 2, cfg);
  REQUIRE(span_rms(a, 0, a.size()) > 1.0e-3);
  REQUIRE(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

TEST_CASE("a singing tap leaves the sustained vowel where it was", "[midi][synth][singing]") {
  std::vector<std::vector<double>> formants;
  for (const float depth : {0.0f, 1.0f}) {
    NativeSynthConfig cfg = quiet_vibrato(singing_config());
    cfg.patch.singing.tap_depth = depth;
    const std::vector<float> x =
        sonare::test::render_sequence_mono({{0, note_on(kNote)}}, kRate, kRate, cfg);
    formants.push_back(harmonic_formants(x, kRate / 2, 8192));
    CAPTURE(depth, series(formants.back()));
    REQUIRE(formants.back().size() == 2);
  }
  CAPTURE(series(formants[0]), series(formants[1]));
  for (int k = 0; k < 2; ++k) {
    REQUIRE(std::fabs(formants[1][k] - formants[0][k]) <= 0.02 * formants[0][k]);
  }
}

/// Valley over peak of the 10 ms envelope around a tap of @p depth fired into a
/// sustained vowel at constant breath, on the assembly the engine renders.
double tap_valley_ratio(float depth) {
  const SingingPatchParams defaults;
  SingingPhonation voice;
  voice.configure_patch(kRate, static_cast<float>(kNoteHz), defaults);
  const int tap_len = static_cast<int>(2.0f * defaults.tap_ms * 0.001f * kRate);
  const int start = kRate / 2;
  std::vector<float> x;
  for (int n = 0; n < kRate; ++n) {
    const int k = n - start;
    if (k >= 0 && k <= tap_len && k % 32 == 0) {
      const float s = std::sin(static_cast<float>(sonare::constants::kPi) * k / tap_len);
      voice.set_tap(defaults.tap_position, depth * s * s);
    }
    x.push_back(voice.process(0.6f, 0.0f, 0.55f, 0.0f));
  }
  const std::vector<double> env = frame_rms(x, 480);
  const std::size_t at = static_cast<std::size_t>(start) / 480;
  const double peak = *std::max_element(env.begin() + static_cast<long>(at) - 20,
                                        env.begin() + static_cast<long>(at));
  const double valley = *std::min_element(env.begin() + static_cast<long>(at),
                                          env.begin() + static_cast<long>(at) + 10);
  return valley / peak;
}

TEST_CASE("a singing tap carves a syllable gap into a sounding vowel", "[midi][synth][singing]") {
  // The tap's acoustic effect where it can show one: over a vowel already
  // sounding at constant breath. At a note-on the onset's own near-silence
  // masks it, which is why this is read on the assembly rather than per note.
  const double bare = tap_valley_ratio(0.0f);
  const double tapped = tap_valley_ratio(0.7f);
  CAPTURE(bare, tapped);
  REQUIRE(bare > 0.9);
  REQUIRE(tapped <= 0.5 * bare);
}

TEST_CASE("a singing tap closes without a burst", "[midi][synth][singing]") {
  // A closure against a lossless rigid tract rings and releases louder than the
  // vowel it interrupts; the 10 ms peak from the note-on to 50 ms past the tap
  // must stay within 1.25x the same note's sustain sung with no tap at all.
  const auto render = [](float depth) {
    NativeSynthConfig cfg = quiet_vibrato(singing_config());
    cfg.patch.singing.tap_depth = depth;
    return sonare::test::render_sequence_mono({{0, note_on(kNote)}}, kRate, kRate, cfg);
  };
  const std::vector<float> bare = render(0.0f);
  const double sustain = span_rms(bare, kRate / 2, kRate);
  REQUIRE(sustain > 1.0e-2);
  const SingingPatchParams defaults;
  const std::size_t window =
      static_cast<std::size_t>((2.0 * defaults.tap_ms + 50.0) * 0.001 * kRate);
  for (const float depth : {0.3f, 0.7f, 1.0f}) {
    const std::vector<double> env = frame_rms(render(depth), 480);
    const double peak = *std::max_element(env.begin(), env.begin() + window / 480);
    CAPTURE(depth, peak, sustain);
    REQUIRE(peak <= 1.25 * sustain);
  }
}

TEST_CASE("a bare MIDI melody sings every note at its written pitch", "[midi][synth][singing]") {
  int controllers = -1;
  const std::vector<TimedUmp> events = smf_events(melody_smf(kMelody, kVelocity), &controllers);
  REQUIRE(controllers == 0);
  const int length = static_cast<int>(kMelody.size()) * kRate / 2;
  SECTION("at the preset's default articulation") {
    const std::vector<float> x =
        sonare::test::render_sequence_mono(events, kRate, length, singing_config());
    const MelodyCheck check = check_melody(x);
    CAPTURE(check.worst_cents);
    REQUIRE(check.every_note_sounds);
    REQUIRE(check.every_note_in_tune);
    REQUIRE(check.breath_never_zero);
  }
  SECTION("velocity that reaches no breath leaves the voice silent") {
    // The non-vacuity of (c): with no velocity-to-breath and no controller,
    // nothing drives the fold, so the breath gate above is measuring the default.
    NativeSynthConfig cfg = singing_config();
    cfg.patch.singing.velocity_to_force = 0.0f;
    const std::vector<float> x = sonare::test::render_sequence_mono(events, kRate, length, cfg);
    REQUIRE_FALSE(check_melody(x).breath_never_zero);
  }
}

TEST_CASE("a slur carries the singing breath across the note boundary", "[midi][synth][singing]") {
  // Overlapping keys under the carrying articulation: each boundary is a slur,
  // so no new note starts, no tap fires and the breath is never re-ramped.
  NativeSynthConfig cfg = quiet_vibrato(singing_config());
  const std::vector<int> notes = {57, 59, 60, 57};
  const int note_len = kRate / 2;
  std::vector<TimedUmp> events;
  for (std::size_t i = 0; i < notes.size(); ++i) {
    const int at = static_cast<int>(i) * note_len;
    events.push_back({at, note_on(notes[i])});
    events.push_back(
        {at + note_len + (i + 1 < notes.size() ? kRate / 100 : 0), note_off(notes[i])});
  }
  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  REQUIRE(synth.set_articulation(0, ArticulationMode::kMonoLegato));
  std::stable_sort(events.begin(), events.end(),
                   [](const TimedUmp& a, const TimedUmp& b) { return a.sample < b.sample; });
  std::vector<float> x;
  std::size_t next = 0;
  for (int at = 0; at < static_cast<int>(notes.size()) * note_len; at += 64) {
    while (next < events.size() && events[next].sample <= at) {
      synth.on_event(0, event(events[next].ump));
      ++next;
    }
    const std::vector<float> block = render_left(synth, 64);
    x.insert(x.end(), block.begin(), block.end());
  }
  const std::vector<double> env = frame_rms(x, 480);
  for (std::size_t b = 1; b < notes.size(); ++b) {
    const std::size_t at = b * static_cast<std::size_t>(note_len) / 480;
    const double valley = *std::min_element(env.begin() + static_cast<long>(at) - 6,
                                            env.begin() + static_cast<long>(at) + 6);
    const double before = env[at - 15];
    const double after = env[at + 15];
    CAPTURE(b, valley, before, after);
    REQUIRE(valley > 0.8 * std::min(before, after));
  }
}

TEST_CASE("singing breath carries the fold across the phonation threshold",
          "[midi][synth][singing]") {
  // The reed is forbidden this behaviour (its breath only colours a tone it never
  // silences); here force zero is below threshold and force one well above it.
  SingingPatchParams params;
  params.vibrato_depth = 0.0f;
  params.tap_depth = 0.0f;
  params.breath_noise = 0.0f;
  SingingVoiceCore voice;
  voice.start(params, kRate, kNote, Velocity16::from7(kVelocity), 1);
  std::vector<float> x;
  constexpr int kSteps = 21;
  constexpr int kHold = kRate / 4;
  for (int step = 0; step < kSteps; ++step) {
    ExcitationAxes axes;
    axes.force = static_cast<float>(step) / static_cast<float>(kSteps - 1);
    voice.set_excitation_base(axes, kAxisForce);
    for (int n = 0; n < kHold; ++n) x.push_back(voice.render(1.0f));
  }
  std::vector<double> level;
  for (int step = 0; step < kSteps; ++step) {
    const std::size_t end = static_cast<std::size_t>((step + 1) * kHold);
    level.push_back(span_rms(x, end - kHold / 2, end));
  }
  CAPTURE(series(level));
  REQUIRE(level.front() < kSilenceRms);
  REQUIRE(level.back() > 0.05);
  const auto onset = std::find_if(level.begin(), level.end(), [](double l) { return l > 1.0e-3; });
  REQUIRE(onset != level.begin());
  REQUIRE(onset != level.end());
  // Once over the threshold the fold oscillates at the note, not merely leaks breath.
  const std::size_t last = x.size() - kHold / 2;
  const double f = sonare::test::singing::acf_f0(x.data() + last, 4096);
  REQUIRE(std::fabs(1200.0 * std::log2(f / kNoteHz)) < 50.0);
}

TEST_CASE("the singing voice shares no code with the reed", "[midi][synth][singing]") {
  // Structural: a shared reed core would make the reed's "breath never silences"
  // invariant false for half its callers while every reed test stayed green.
  for (const char* path : {"src/midi/synth/singing_voice.h", "src/midi/synth/singing_voice.cpp"}) {
    std::ifstream file(path);
    REQUIRE(file.good());
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    CAPTURE(path);
    REQUIRE(text.find("reed_voice.h") == std::string::npos);
    REQUIRE(text.find("ReedVoiceCore") == std::string::npos);
  }
}

TEST_CASE("four singing voices render a 512-sample block inside its real-time budget",
          "[.][midi][synth][singing][rt-budget]") {
  // 512 samples at 48 kHz is 10.67 ms of audio; the render has to take less.
  NativeSynthConfig cfg = singing_config();
  NativeSynth synth(cfg);
  synth.prepare(kRate, 512);
  for (const int note : {50, 54, 57, 61}) synth.on_event(0, event(note_on(note)));
  render_left(synth, kRate / 2);
  std::vector<float> left(512);
  std::vector<float> right(512);
  float* channels[2] = {left.data(), right.data()};
  double worst_ms = 0.0;
  double total_ms = 0.0;
  constexpr int kBlocks = 400;
  for (int b = 0; b < kBlocks; ++b) {
    const auto t0 = std::chrono::steady_clock::now();
    synth.process(channels, 2, 512);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    worst_ms = std::max(worst_ms, ms);
    total_ms += ms;
  }
  WARN("4 singing voices, 48 kHz, 512-sample block: mean " << total_ms / kBlocks << " ms, worst "
                                                           << worst_ms << " ms of 10.67 ms");
#if defined(NDEBUG)
  REQUIRE(total_ms / kBlocks < 512.0 * 1000.0 / kRate);
#endif
}
