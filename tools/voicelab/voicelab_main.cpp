/// @file voicelab_main.cpp
/// @brief sonare_voicelab: engine measurement and external-device loopback analysis.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

#include "core/audio_io.h"
#include "editing/voice_changer/realtime.h"
#include "util/constants.h"
#include "voicelab/metrics.h"
#include "voicelab/synthetic_voice.h"

namespace {

using namespace sonare::editing::voice_changer;
using sonare::constants::kCentsPerOctave;
using sonare::constants::kSemitonesPerOctave;
using sonare::voicelab::VoiceSegment;
namespace vl = sonare::voicelab;

constexpr int kBlock = 128;
constexpr int kVowelA = 0;
constexpr int kVowelCount = 5;  ///< /a e i o u/
constexpr float kLevelDb = -20.0f;
// Evaluator frames whose window reaches an onset, a release or the output start are left out.
constexpr double kGuardS = 0.08;
constexpr double kEvaluatorHopS = 0.010;
constexpr double kGrossErrorCents = 50.0;
constexpr double kUnvoicedErrorCents = 1e9;
constexpr double kOutputF0MinHz = 50.0;
constexpr double kOutputF0MaxHz = 1400.0;
constexpr float kLsdOutputF0Hz = 220.0f;  ///< Above this output f0 formants are scored by LSD.

void print_usage(std::FILE* out) {
  std::fprintf(out,
               "Usage: sonare_voicelab <command> [options]\n"
               "\n"
               "Commands:\n"
               "  measure   Run the quality metrics on the voice engines\n"
               "              --engines <list>   comma-separated engine names (grain,psola)\n"
               "              --out <path>       output JSON path\n"
               "  analyze   Analyze a loopback recording from an external device\n"
               "              --dry <wav>        recording taken before processing\n"
               "              --wet <wav>        recording taken after processing\n"
               "              --out <path>       output JSON path\n"
               "\n"
               "Options:\n"
               "  --help    Show this message\n");
}

// ---------------------------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------------------------

/// Minimal streaming JSON writer; non-finite numbers are written as null.
class Json {
 public:
  Json& begin_object() { return open('{'); }
  Json& end_object() { return close('}'); }
  Json& begin_array() { return open('['); }
  Json& end_array() { return close(']'); }

  Json& key(const std::string& k) {
    separate();
    quote(k);
    text_ += ':';
    pending_value_ = true;
    return *this;
  }
  Json& value(double v) {
    separate();
    if (std::isfinite(v)) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%.9g", v);
      text_ += buf;
    } else {
      text_ += "null";
    }
    return *this;
  }
  Json& value(int v) { return value(static_cast<double>(v)); }
  Json& value(bool v) {
    separate();
    text_ += v ? "true" : "false";
    return *this;
  }
  Json& value(const std::string& v) {
    separate();
    quote(v);
    return *this;
  }
  Json& null() {
    separate();
    text_ += "null";
    return *this;
  }
  Json& field(const std::string& k, double v) { return key(k).value(v); }
  Json& field(const std::string& k, int v) { return key(k).value(v); }
  Json& field(const std::string& k, bool v) { return key(k).value(v); }
  Json& field(const std::string& k, const std::string& v) { return key(k).value(v); }

  /// Splices an already-serialized JSON value.
  Json& raw(const std::string& json) {
    separate();
    text_ += json;
    return *this;
  }

  const std::string& str() const { return text_; }

 private:
  Json& open(char c) {
    separate();
    text_ += c;
    first_.push_back(true);
    return *this;
  }
  Json& close(char c) {
    first_.pop_back();
    text_ += c;
    return *this;
  }
  void separate() {
    if (pending_value_) {
      pending_value_ = false;
      return;
    }
    if (!first_.empty()) {
      if (!first_.back()) text_ += ',';
      first_.back() = false;
    }
  }
  void quote(const std::string& s) {
    text_ += '"';
    for (const char c : s) {
      if (c == '"' || c == '\\') {
        text_ += '\\';
        text_ += c;
      } else if (static_cast<unsigned char>(c) < 0x20) {
        text_ += ' ';
      } else {
        text_ += c;
      }
    }
    text_ += '"';
  }

  std::string text_;
  std::vector<bool> first_;
  bool pending_value_ = false;
};

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "sonare_voicelab: cannot write '%s'\n", path.c_str());
    return false;
  }
  f << text << '\n';
  return static_cast<bool>(f);
}

// ---------------------------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------------------------

double cents(double a, double b) { return kCentsPerOctave * std::log2(a / b); }

double semitone_ratio(double semitones) { return std::exp2(semitones / kSemitonesPerOctave); }

VoiceSegment steady(double seconds, float f0, int vowel = kVowelA) {
  return {seconds, f0, f0, 0.0f, 0.0f, true, vowel, kLevelDb, 10.0f};
}

/// Alternating voiced (220 Hz /a/) and equal-RMS white-noise segments with 5 ms edges.
std::vector<float> alternating(double sr, int segments, double segment_s) {
  std::vector<VoiceSegment> parts;
  for (int i = 0; i < segments; ++i) {
    parts.push_back({segment_s, 220.0f, 220.0f, 0.0f, 0.0f, i % 2 == 0, kVowelA, kLevelDb, 5.0f});
  }
  return vl::synthesize_voice(parts, sr);
}

/// Every stage except the voice stage set to transparent, the inter-sample-peak limiter on.
RealtimeVoiceChangerConfig transparent(PitchEngine engine, float semitones = 0.0f) {
  RealtimeVoiceChangerConfig c;
  c.wet_mix = 1.0f;
  c.retune = {semitones, 1.0f, 0};
  c.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  c.eq = {20.0f, 0.0f, 0.0f, 0.0f};
  c.gate = {-90.0f, 1.0f, 50.0f, 0.0f};
  c.compressor = {0.0f, 1.0f, 1.0f, 50.0f, 0.0f};
  c.deesser = {7000.0f, -6.0f, 1.0f, 0.0f};
  c.reverb.mix = 0.0f;
  c.limiter.ceiling_db = -1.0f;
  c.voice_engine.engine = engine;
  return c;
}

RealtimeVoiceChangerConfig absolute_config(PitchEngine engine, int semitones, float factor) {
  RealtimeVoiceChangerConfig c = transparent(engine, static_cast<float>(semitones));
  c.formant_mode = FormantMode::Absolute;
  c.formant.factor = factor;
  return c;
}

struct Rendered {
  std::vector<float> out;
  int latency = 0;
};

/// Streams `x` through a fresh mono changer in fixed blocks.
Rendered render(const RealtimeVoiceChangerConfig& config, const std::vector<float>& x, double sr) {
  RealtimeVoiceChanger changer(config);
  changer.prepare(sr, kBlock, 1);
  Rendered r;
  r.latency = changer.latency_samples();
  r.out.assign(x.size(), 0.0f);
  for (std::size_t pos = 0; pos < x.size(); pos += kBlock) {
    const int n = static_cast<int>(std::min<std::size_t>(kBlock, x.size() - pos));
    changer.process_block(x.data() + pos, r.out.data() + pos, n);
  }
  return r;
}

double quantile(std::vector<double> v, double q) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  const auto k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

/// The steady interior of a render: from latency + 100 ms to 30 ms before the end.
struct Span {
  std::size_t begin;
  std::size_t size;
};

Span steady_span(std::size_t n, double sr, int latency) {
  const auto begin = static_cast<std::size_t>(latency) + static_cast<std::size_t>(0.1 * sr);
  const auto tail = static_cast<std::size_t>(0.03 * sr);
  if (begin + tail >= n) return {0, n};
  return {begin, n - tail - begin};
}

float median_f0(const float* y, std::size_t n, double sr) {
  const std::vector<float> track = vl::estimate_f0_track(y, n, sr, kEvaluatorHopS);
  std::vector<double> voiced;
  for (const float f : track) {
    if (f > 0.0f) voiced.push_back(f);
  }
  return voiced.empty() ? 0.0f : static_cast<float>(quantile(voiced, 0.5));
}

/// Absolute cent errors of the evaluator's output f0 against `expected_hz`, interior frames only.
std::vector<double> pitch_errors(const std::vector<float>& y, double sr, int latency,
                                 double expected_hz) {
  const std::vector<float> track = vl::estimate_f0_track(y.data(), y.size(), sr, kEvaluatorHopS);
  std::vector<double> errors;
  const double guard = kGuardS * sr;
  for (std::size_t i = 0; i < track.size(); ++i) {
    const double centre = static_cast<double>(i) * kEvaluatorHopS * sr;
    if (centre < latency + guard || centre > static_cast<double>(y.size()) - guard) continue;
    // An unvoiced frame counts as a gross error.
    errors.push_back(track[i] > 0.0f ? std::abs(cents(track[i], expected_hz))
                                     : kUnvoicedErrorCents);
  }
  return errors;
}

// ---------------------------------------------------------------------------------------------
// measure
// ---------------------------------------------------------------------------------------------

struct EngineEntry {
  std::string name;
  PitchEngine engine;
};

/// Serializes what `body` writes; a refusal or failure becomes {"error": "..."}.
std::string section(const std::function<void(Json&)>& body) {
  Json j;
  try {
    body(j);
  } catch (const std::exception& e) {
    Json err;
    err.begin_object().field("error", std::string(e.what())).end_object();
    return err.str();
  }
  return j.str();
}

void measure_latency(Json& j, PitchEngine engine, const std::string& name) {
  constexpr int kMaxLag = 4000;
  j.begin_object();
  for (const double sr : {48000.0, 44100.0}) {
    const std::vector<float> x = alternating(sr, 4, 0.15);
    const Rendered unity = render(transparent(engine), x, sr);
    const int measured =
        vl::estimate_latency_samples(x.data(), unity.out.data(), x.size(), kMaxLag, false);
    j.key(sr == 48000.0 ? "sr_48000" : "sr_44100").begin_object();
    j.field("reported_samples", unity.latency);
    j.field("measured_samples", measured);
    j.field("reported_ms", 1000.0 * unity.latency / sr);
    j.key("envelope_measured_samples").begin_object();
    for (const float semitones : {-7.0f, 7.0f}) {
      const Rendered y = render(transparent(engine, semitones), x, sr);
      const int envelope =
          vl::estimate_latency_samples(x.data(), y.out.data(), x.size(), kMaxLag, true);
      j.field(semitones < 0.0f ? "minus_7_semitones" : "plus_7_semitones", envelope);
    }
    j.end_object().end_object();
    std::fprintf(stderr, "  %-5s latency @%.0f Hz: reported %d, measured %d samples\n",
                 name.c_str(), sr, unity.latency, measured);
  }
  j.end_object();
}

void measure_pitch(Json& j, PitchEngine engine, const std::string& name) {
  constexpr double kSr = 48000.0;
  const std::array<float, 5> f0s = {82.0f, 110.0f, 220.0f, 440.0f, 660.0f};
  const std::array<int, 7> shifts = {-12, -7, -3, 0, 3, 7, 12};
  std::vector<double> pool;
  Json cells;
  cells.begin_array();
  for (const float f0 : f0s) {
    const std::vector<float> x = vl::synthesize_voice({steady(0.4, f0)}, kSr);
    for (const int semitones : shifts) {
      const double expected = f0 * semitone_ratio(semitones);
      if (expected < kOutputF0MinHz || expected > kOutputF0MaxHz) continue;
      const Rendered y = render(transparent(engine, static_cast<float>(semitones)), x, kSr);
      const std::vector<double> e = pitch_errors(y.out, kSr, y.latency, expected);
      cells.begin_object()
          .field("input_f0_hz", static_cast<double>(f0))
          .field("semitones", semitones)
          .field("median_cents", quantile(e, 0.5))
          .field("p95_cents", quantile(e, 0.95))
          .end_object();
      pool.insert(pool.end(), e.begin(), e.end());
    }
  }
  cells.end_array();
  const auto gross =
      std::count_if(pool.begin(), pool.end(), [](double e) { return e > kGrossErrorCents; });
  const double median = quantile(pool, 0.5);
  const double p95 = quantile(pool, 0.95);
  const double gross_pct =
      pool.empty() ? std::numeric_limits<double>::quiet_NaN()
                   : 100.0 * static_cast<double>(gross) / static_cast<double>(pool.size());
  j.begin_object()
      .field("frames", static_cast<int>(pool.size()))
      .field("median_cents", median)
      .field("p95_cents", p95)
      .field("gross_error_percent", gross_pct)
      .key("cells")
      .raw(cells.str())
      .end_object();
  std::fprintf(stderr, "  %-5s pitch error: median %.3f cent, p95 %.3f cent, gross %.2f%%\n",
               name.c_str(), median, p95, gross_pct);
}

/// Largest per-formant error (percent) a cell may show; the shifter's known limit is wider.
double formant_limit_pct(float input_f0, int semitones, float factor) {
  const bool tight = input_f0 == 110.0f && (semitones < 0 || factor == 1.0f);
  return tight ? 5.0 : 20.0;
}

void measure_formants(Json& j, PitchEngine engine, const std::string& name) {
  constexpr double kSr = 48000.0;
  j.begin_array();
  double worst_pct = 0.0;
  double worst_lsd = 0.0;
  for (const float f0 : {110.0f, 220.0f}) {
    for (const int semitones : {-12, 12}) {
      for (const float factor : {1.0f, 1.2f}) {
        const float out_f0 = f0 * static_cast<float>(semitone_ratio(semitones));
        const bool by_lsd = out_f0 > kLsdOutputF0Hz;
        // A configuration the engine refuses is recorded in the cell instead of ending the run.
        j.raw(section([&](Json& c) {
          c.begin_object()
              .field("input_f0_hz", static_cast<double>(f0))
              .field("semitones", semitones)
              .field("formant_factor", static_cast<double>(factor))
              .field("output_f0_hz", static_cast<double>(out_f0));
          double cell_worst = 0.0;
          c.key(by_lsd ? "lsd_db" : "formant_error_pct").begin_array();
          for (int vowel = 0; vowel < kVowelCount; ++vowel) {
            const std::vector<float> x = vl::synthesize_voice({steady(0.5, f0, vowel)}, kSr);
            const Rendered y = render(absolute_config(engine, semitones, factor), x, kSr);
            const Span span = steady_span(x.size(), kSr, y.latency);
            if (by_lsd) {
              const auto ref_begin = static_cast<std::size_t>(0.1 * kSr);
              const float lsd = vl::log_spectral_distance_db(
                  x.data() + ref_begin, y.out.data() + span.begin, span.size, kSr, factor);
              c.value(static_cast<double>(lsd));
              cell_worst = std::max(cell_worst, static_cast<double>(lsd));
            } else {
              const std::array<float, 3> known = vl::vowel_formants_hz(vowel);
              const std::array<float, 3> got =
                  vl::estimate_formants(y.out.data() + span.begin, span.size, kSr);
              c.begin_array();
              for (std::size_t k = 0; k < 3; ++k) {
                const double pct = 100.0 * (got[k] / (known[k] * factor) - 1.0);
                c.value(pct);
                cell_worst = std::max(cell_worst, std::abs(pct));
              }
              c.end_array();
            }
          }
          c.end_array();
          if (by_lsd) {
            c.field("worst_lsd_db", cell_worst);
            worst_lsd = std::max(worst_lsd, cell_worst);
          } else {
            c.field("worst_abs_error_pct", cell_worst)
                .field("limit_pct", formant_limit_pct(f0, semitones, factor))
                .field("within_limit", cell_worst <= formant_limit_pct(f0, semitones, factor));
            worst_pct = std::max(worst_pct, cell_worst);
          }
          c.end_object();
        }));
      }
    }
  }
  j.end_array();
  std::fprintf(stderr, "  %-5s formants: worst error %.2f%%, worst LSD %.2f dB\n", name.c_str(),
               worst_pct, worst_lsd);
}

void measure_inharmonic(Json& j, PitchEngine engine, const std::string& name) {
  j.begin_array();
  for (const double sr : {48000.0, 44100.0}) {
    const std::vector<float> x = vl::synthesize_voice({steady(0.5, 220.0f)}, sr);
    for (const bool absolute : {false, true}) {
      const RealtimeVoiceChangerConfig config =
          absolute ? absolute_config(engine, 12, 1.2f) : transparent(engine, 12.0f);
      const Rendered y = render(config, x, sr);
      const Span span = steady_span(x.size(), sr, y.latency);
      const float* out = y.out.data() + span.begin;
      const float f0 = median_f0(out, span.size, sr);
      const float db = vl::inharmonic_energy_db(out, span.size, sr, f0);
      j.begin_object()
          .field("sample_rate", sr)
          .field("formant_mode", std::string(absolute ? "absolute" : "relative"))
          .field("output_f0_hz", static_cast<double>(f0))
          .field("inharmonic_energy_db", static_cast<double>(db))
          .end_object();
      std::fprintf(stderr, "  %-5s inharmonic @%.0f Hz %-8s: %.1f dB\n", name.c_str(), sr,
                   absolute ? "absolute" : "relative", static_cast<double>(db));
    }
  }
  j.end_array();
}

bool parse_engines(const std::string& list, std::vector<EngineEntry>* out) {
  std::size_t pos = 0;
  while (pos <= list.size()) {
    const std::size_t comma = list.find(',', pos);
    const std::string item = list.substr(pos, comma == std::string::npos ? comma : comma - pos);
    if (item == "grain") {
      out->push_back({item, PitchEngine::Grain});
    } else if (item == "psola") {
      out->push_back({item, PitchEngine::Psola});
    } else {
      std::fprintf(stderr, "sonare_voicelab: unknown engine '%s' (grain, psola)\n", item.c_str());
      return false;
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return !out->empty();
}

int run_measure(const std::string& engines, const std::string& out_path) {
  if (out_path.empty()) {
    std::fprintf(stderr, "sonare_voicelab: measure needs --out\n");
    return 1;
  }
  std::vector<EngineEntry> entries;
  if (!parse_engines(engines.empty() ? "grain,psola" : engines, &entries)) return 1;

  Json json;
  json.begin_object().key("engines").begin_object();
  for (const EngineEntry& e : entries) {
    std::fprintf(stderr, "engine %s\n", e.name.c_str());
    json.key(e.name).begin_object();
    json.key("latency").raw(section([&](Json& j) { measure_latency(j, e.engine, e.name); }));
    json.key("pitch_error").raw(section([&](Json& j) { measure_pitch(j, e.engine, e.name); }));
    json.key("formants").raw(section([&](Json& j) { measure_formants(j, e.engine, e.name); }));
    json.key("aliasing").raw(section([&](Json& j) { measure_inharmonic(j, e.engine, e.name); }));
    json.end_object();
  }
  json.end_object().end_object();
  return write_text(out_path, json.str()) ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// analyze
// ---------------------------------------------------------------------------------------------

struct Recording {
  std::vector<float> samples;
  double sample_rate = 0.0;
};

Recording load_recording(const std::string& path) {
  auto [samples, sr] = sonare::load_audio(path);
  return {std::move(samples), static_cast<double>(sr)};
}

void write_track_summary(Json& j, const std::vector<float>& x, double sr) {
  constexpr double kHopS = 0.010;
  const std::vector<float> track = vl::estimate_f0_track(x.data(), x.size(), sr, kHopS);
  std::vector<double> voiced;
  for (const float f : track) {
    if (f > 0.0f) voiced.push_back(f);
  }
  j.begin_object()
      .field("frames", static_cast<int>(track.size()))
      .field("voiced_fraction",
             track.empty() ? 0.0
                           : static_cast<double>(voiced.size()) / static_cast<double>(track.size()))
      .field("median_hz", quantile(voiced, 0.5))
      .field("p05_hz", quantile(voiced, 0.05))
      .field("p95_hz", quantile(voiced, 0.95))
      .end_object();
}

void write_signal(Json& j, const std::vector<float>& x, double sr) {
  // Formants and inharmonic energy are read from the interior, away from the edges.
  const std::size_t edge = x.size() / 10;
  const float* mid = x.data() + edge;
  const std::size_t mid_n = x.size() - 2 * edge;
  const float f0 = median_f0(x.data(), x.size(), sr);
  const std::array<float, 3> f = vl::estimate_formants(mid, mid_n, sr);
  j.begin_object();
  j.key("f0_track");
  write_track_summary(j, x, sr);
  j.key("formants_hz")
      .begin_array()
      .value(static_cast<double>(f[0]))
      .value(static_cast<double>(f[1]))
      .value(static_cast<double>(f[2]))
      .end_array();
  j.field("inharmonic_energy_db",
          f0 > 0.0f ? static_cast<double>(vl::inharmonic_energy_db(mid, mid_n, sr, f0))
                    : std::numeric_limits<double>::quiet_NaN());
  j.end_object();
}

int run_analyze(const std::string& dry_path, const std::string& wet_path,
                const std::string& out_path) {
  if (dry_path.empty() || wet_path.empty() || out_path.empty()) {
    std::fprintf(stderr, "sonare_voicelab: analyze needs --dry, --wet and --out\n");
    return 1;
  }
  const Recording dry = load_recording(dry_path);
  const Recording wet = load_recording(wet_path);
  if (dry.sample_rate != wet.sample_rate) {
    std::fprintf(stderr, "sonare_voicelab: dry and wet sample rates differ (%.0f, %.0f)\n",
                 dry.sample_rate, wet.sample_rate);
    return 1;
  }
  const double sr = dry.sample_rate;
  const std::size_t n = std::min(dry.samples.size(), wet.samples.size());
  if (n == 0) {
    std::fprintf(stderr, "sonare_voicelab: empty recording\n");
    return 1;
  }
  const int max_lag =
      static_cast<int>(std::min<std::size_t>(n / 2, static_cast<std::size_t>(sr / 4)));
  const int waveform =
      vl::estimate_latency_samples(dry.samples.data(), wet.samples.data(), n, max_lag, false);
  const int envelope =
      vl::estimate_latency_samples(dry.samples.data(), wet.samples.data(), n, max_lag, true);

  Json json;
  json.begin_object().field("sample_rate", sr);
  json.key("latency")
      .begin_object()
      .field("waveform_samples", waveform)
      .field("waveform_ms", 1000.0 * waveform / sr)
      .field("envelope_samples", envelope)
      .field("envelope_ms", 1000.0 * envelope / sr)
      .end_object();
  json.key("dry");
  write_signal(json, dry.samples, sr);
  json.key("wet");
  write_signal(json, wet.samples, sr);
  json.end_object();
  std::fprintf(stderr, "latency: waveform %d samples (%.2f ms), envelope %d samples (%.2f ms)\n",
               waveform, 1000.0 * waveform / sr, envelope, 1000.0 * envelope / sr);
  return write_text(out_path, json.str()) ? 0 : 1;
}

struct Args {
  std::string engines;
  std::string dry;
  std::string wet;
  std::string out;
};

/// Reads the value following a flag; returns false when it is missing.
bool take_value(int argc, char** argv, int* i, std::string* dst) {
  if (*i + 1 >= argc) {
    std::fprintf(stderr, "sonare_voicelab: %s needs a value\n", argv[*i]);
    return false;
  }
  *dst = argv[++*i];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(stderr);
    return 1;
  }
  const std::string command = argv[1];
  if (command == "--help" || command == "-h" || command == "help") {
    print_usage(stdout);
    return 0;
  }
  const bool is_measure = command == "measure";
  const bool is_analyze = command == "analyze";
  if (!is_measure && !is_analyze) {
    std::fprintf(stderr, "sonare_voicelab: unknown command '%s'\n", command.c_str());
    print_usage(stderr);
    return 1;
  }

  Args args;
  for (int i = 2; i < argc; ++i) {
    const char* flag = argv[i];
    if (std::strcmp(flag, "--help") == 0 || std::strcmp(flag, "-h") == 0) {
      print_usage(stdout);
      return 0;
    }
    bool ok = false;
    if (std::strcmp(flag, "--out") == 0) {
      ok = take_value(argc, argv, &i, &args.out);
    } else if (is_measure && std::strcmp(flag, "--engines") == 0) {
      ok = take_value(argc, argv, &i, &args.engines);
    } else if (is_analyze && std::strcmp(flag, "--dry") == 0) {
      ok = take_value(argc, argv, &i, &args.dry);
    } else if (is_analyze && std::strcmp(flag, "--wet") == 0) {
      ok = take_value(argc, argv, &i, &args.wet);
    } else {
      std::fprintf(stderr, "sonare_voicelab: unknown option '%s'\n", flag);
      return 1;
    }
    if (!ok) return 1;
  }

  try {
    return is_measure ? run_measure(args.engines, args.out)
                      : run_analyze(args.dry, args.wet, args.out);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "sonare_voicelab: %s\n", e.what());
    return 1;
  }
}
