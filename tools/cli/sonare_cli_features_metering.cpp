#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

#include "c_api/mel_refusal_names.h"
#include "sonare_cli.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

template <typename Call>
auto with_mel_cli_subjects(Call&& call) -> decltype(call()) {
  try {
    return call();
  } catch (const SonareParameterRefusal& refusal) {
    throw refusal.respelled(
        refusal.code(),
        [](const std::string& subject) {
          for (const auto& [core, field] : sonare_c_detail::kMelRangeNames) {
            if (subject == core) {
              std::string flag = "--" + std::string(field);
              std::replace(flag.begin(), flag.end(), '_', '-');
              return flag;
            }
          }
          return subject;
        },
        true);
  }
}

}  // namespace

int cmd_mel(const CliArgs& args, const Audio& audio) {
  MelConfig config;
  config.n_mels = args.get_int("n-mels", config.n_mels);
  config.n_fft = args.get_int("n-fft", config.n_fft);
  config.hop_length = args.get_int("hop-length", config.hop_length);
  config.htk = args.has("htk");
  config.fmin = args.get_float("fmin-hz", config.fmin);
  config.fmax = args.has("fmax-hz") ? args.fmax : static_cast<float>(audio.sample_rate()) / 2.0f;

  MelSpectrogram mel =
      with_mel_cli_subjects([&] { return MelSpectrogram::compute(audio, config); });
  const float* data = mel.power_data();
  size_t total = static_cast<size_t>(mel.n_mels()) * static_cast<size_t>(mel.n_frames());

  float min_v = *std::min_element(data, data + total);
  float max_v = *std::max_element(data, data + total);
  // Accumulate in double; a float sum drops low digits well before a full mel grid ends.
  float mean_v =
      static_cast<float>(std::accumulate(data, data + total, 0.0) / static_cast<double>(total));

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_mels", mel.n_mels())
        .kv("n_frames", mel.n_frames())
        .kv("duration", mel.duration())
        .kv("sample_rate", mel.sample_rate())
        .kv("hop_length", mel.hop_length())
        .key("stats")
        .begin_object()
        .kv("min", min_v)
        .kv("max", max_v)
        .kv("mean", mean_v)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Mel Spectrogram:\n";
    printf("  Shape:       %d bands x %d frames\n", mel.n_mels(), mel.n_frames());
    printf("  Duration:    %.2fs\n", mel.duration());
    printf("  Sample Rate: %d Hz\n", mel.sample_rate());
    printf("  Stats:       min=%.4f, max=%.4f, mean=%.4f\n", min_v, max_v, mean_v);
  }
  return 0;
}

int cmd_chroma(const CliArgs& args, const Audio& audio) {
  ChromaConfig config;
  config.n_fft = args.get_int("n-fft", config.n_fft);
  config.hop_length = args.get_int("hop-length", config.hop_length);

  Chroma chroma = Chroma::compute(audio, config);
  auto mean_energy = chroma.mean_energy();
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("n_chroma", chroma.n_chroma())
        .kv("n_frames", chroma.n_frames())
        .kv("duration", chroma.duration())
        .key("mean_energy")
        .begin_array();
    for (float value : mean_energy) json.value(value);
    json.end_array().end_object().print();
  } else {
    std::cout << "Chromagram:\n";
    printf("  Shape:    %d bins x %d frames\n", chroma.n_chroma(), chroma.n_frames());
    printf("  Duration: %.2fs\n", chroma.duration());
    std::cout << "\nMean Energy by Pitch Class:\n";
    float max_e = *std::max_element(mean_energy.begin(), mean_energy.end());
    // A fully silent input leaves every pitch-class energy at 0, so max_e is 0;
    // guard the division (as cmd_nnls_chroma does) to avoid a 0/0 -> NaN -> int
    // cast, which is undefined behavior. The bars then render empty.
    if (max_e <= 0.0f) max_e = 1.0f;
    for (int i = 0; i < 12; ++i) {
      int bar = static_cast<int>(mean_energy[i] / max_e * 20);
      printf("  %-2s: %.3f ", names[i], mean_energy[i]);
      for (int j = 0; j < bar; ++j) std::cout << "*";
      std::cout << "\n";
    }
  }
  return 0;
}

int cmd_tonnetz(const CliArgs& args, const Audio& audio) {
  ChromaConfig config;
  config.n_fft = args.get_int("n-fft", config.n_fft);
  config.hop_length = args.get_int("hop-length", config.hop_length);
  Chroma chroma = Chroma::compute(audio, config);
  auto values = tonnetz(chroma);
  Stats stats = Stats::compute(values);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_dims", 6)
        .kv("n_frames", chroma.n_frames())
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Tonnetz:\n";
    printf("  Shape: %d dims x %d frames\n", 6, chroma.n_frames());
    printf("  Stats: mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std, stats.min,
           stats.max);
  }
  return 0;
}

int cmd_spectral(const CliArgs& args, const Audio& audio) {
  StftConfig stft;
  stft.n_fft = args.get_int("n-fft", stft.n_fft);
  stft.hop_length = args.get_int("hop-length", stft.hop_length);
  Spectrogram spec = Spectrogram::compute(audio, stft);
  int sr = audio.sample_rate();

  auto centroid = spectral_centroid(spec, sr);
  auto bandwidth = spectral_bandwidth(spec, sr);
  auto rolloff = spectral_rolloff(spec, sr);
  auto flatness = spectral_flatness(spec);
  auto zcr = zero_crossing_rate(audio, stft.n_fft, stft.hop_length);
  auto rms = rms_energy(audio, stft.n_fft, stft.hop_length);

  Stats sc = Stats::compute(centroid), sb = Stats::compute(bandwidth),
        sr_s = Stats::compute(rolloff), sf = Stats::compute(flatness), sz = Stats::compute(zcr),
        se = Stats::compute(rms);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_frames", centroid.size())
        .key("features")
        .begin_object()
        .key("centroid")
        .begin_object()
        .kv("mean", sc.mean)
        .kv("std", sc.std)
        .kv("min", sc.min)
        .kv("max", sc.max)
        .end_object()
        .key("bandwidth")
        .begin_object()
        .kv("mean", sb.mean)
        .kv("std", sb.std)
        .kv("min", sb.min)
        .kv("max", sb.max)
        .end_object()
        .key("rolloff")
        .begin_object()
        .kv("mean", sr_s.mean)
        .kv("std", sr_s.std)
        .kv("min", sr_s.min)
        .kv("max", sr_s.max)
        .end_object()
        .key("flatness")
        .begin_object()
        .kv("mean", sf.mean)
        .kv("std", sf.std)
        .kv("min", sf.min)
        .kv("max", sf.max)
        .end_object()
        .key("zcr")
        .begin_object()
        .kv("mean", sz.mean)
        .kv("std", sz.std)
        .kv("min", sz.min)
        .kv("max", sz.max)
        .end_object()
        .key("rms")
        .begin_object()
        .kv("mean", se.mean)
        .kv("std", se.std)
        .kv("min", se.min)
        .kv("max", se.max)
        .end_object()
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Spectral Features (" << centroid.size() << " frames):\n";
    printf("  Feature          Mean       Std        Min        Max\n");
    printf("  centroid         %-10.1f %-10.1f %-10.1f %.1f\n", sc.mean, sc.std, sc.min, sc.max);
    printf("  bandwidth        %-10.1f %-10.1f %-10.1f %.1f\n", sb.mean, sb.std, sb.min, sb.max);
    printf("  rolloff          %-10.1f %-10.1f %-10.1f %.1f\n", sr_s.mean, sr_s.std, sr_s.min,
           sr_s.max);
    printf("  flatness         %-10.4f %-10.4f %-10.4f %.4f\n", sf.mean, sf.std, sf.min, sf.max);
    printf("  zcr              %-10.4f %-10.4f %-10.4f %.4f\n", sz.mean, sz.std, sz.min, sz.max);
    printf("  rms              %-10.4f %-10.4f %-10.4f %.4f\n", se.mean, se.std, se.min, se.max);
  }
  return 0;
}

int cmd_pitch(const CliArgs& args, const Audio& audio) {
  // Every domain this command has -- the frequency and threshold ranges, the
  // algorithm name, and fmax > fmin -- is declared in the registry and enforced
  // before dispatch. Reading the values here means reading values that already
  // passed, so an out-of-range frequency is no longer substituted with the
  // default it happens to sit next to.
  PitchConfig config;
  config.fmin = args.get_float("f0-min-hz", config.fmin);
  config.fmax = args.get_float("f0-max-hz", config.fmax);
  config.threshold = args.get_float("threshold", config.threshold);
  config.hop_length = args.get_int("hop-length", config.hop_length);

  const std::string algo = args.get_string("algorithm", "pyin");
  PitchResult result = (algo == "yin") ? yin_track(audio, config) : pyin(audio, config);

  int voiced = std::count(result.voiced_flag.begin(), result.voiced_flag.end(), true);
  const int n_frames = result.n_frames();
  // A zero-frame analysis has no voiced fraction: NaN (not measured), so the key is omitted.
  const float ratio = n_frames > 0 ? static_cast<float>(voiced) / static_cast<float>(n_frames)
                                   : std::numeric_limits<float>::quiet_NaN();

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("algorithm", algo)
        .kv("n_frames", n_frames)
        .kv("voiced_count", voiced)
        .kv("voiced_ratio", ratio)
        .kv("median_f0", result.median_f0())
        .kv("mean_f0", result.mean_f0());
    // The contour is opt-in: it is one value per frame, so the summary stays the
    // default. An unvoiced frame has no f0, so f0 lists the voiced frames' values beside
    // their frame indices.
    if (args.has("frames")) {
      std::vector<double> times(result.f0.size());
      for (size_t i = 0; i < times.size(); ++i) {
        times[i] = static_cast<double>(i) * config.hop_length / audio.sample_rate();
      }
      json.key("times").begin_array();
      for (double time : times) json.value(time);
      json.end_array();
      json.measured_array("f0", "f0_frame", result.f0.data(), result.f0.size());
      json.key("voiced_flag").begin_array();
      for (bool flag : result.voiced_flag) json.value(flag);
      json.end_array();
      json.key("voiced_prob").float_array(result.voiced_prob);
    }
    json.end_object().print();
  } else {
    std::cout << "Pitch Tracking (" << algo << "):\n";
    printf("  Frames:    %d\n", n_frames);
    const std::string voiced_text =
        std::isnan(ratio) ? "-" : measured_text(ratio * 100.0f, 1) + "%";
    printf("  Voiced:    %d (%s)\n", voiced, voiced_text.c_str());
    printf("  Median F0: %s\n", measured_text(result.median_f0(), 1, "Hz").c_str());
    printf("  Mean F0:   %s\n", measured_text(result.mean_f0(), 1, "Hz").c_str());
  }
  return 0;
}

int cmd_tempogram(const CliArgs& args, const Audio& audio) {
  MelConfig mel_config;
  mel_config.n_fft = args.get_int("n-fft", mel_config.n_fft);
  mel_config.hop_length = args.get_int("hop-length", mel_config.hop_length);
  mel_config.n_mels = args.get_int("n-mels", mel_config.n_mels);

  OnsetConfig onset_config;
  onset_config.center = true;
  const auto envelope = compute_onset_strength(audio, mel_config, onset_config);

  TempogramConfig config;
  config.hop_length = args.get_int("hop-length", config.hop_length);
  config.win_length = args.get_int("win-length", config.win_length);
  auto values = detail::tempogram_of_envelope(envelope, config);
  const int n_frames =
      config.win_length > 0 ? static_cast<int>(values.size()) / config.win_length : 0;
  Stats stats = Stats::compute(values);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("win_length", config.win_length)
        .kv("n_frames", n_frames)
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Tempogram:\n";
    printf("  Shape: %d lags x %d frames\n", config.win_length, n_frames);
    printf("  Stats: mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std, stats.min,
           stats.max);
  }
  return 0;
}

int cmd_plp(const CliArgs& args, const Audio& audio) {
  MelConfig mel_config;
  mel_config.n_fft = args.get_int("n-fft", mel_config.n_fft);
  mel_config.hop_length = args.get_int("hop-length", mel_config.hop_length);
  mel_config.n_mels = args.get_int("n-mels", mel_config.n_mels);

  OnsetConfig onset_config;
  onset_config.center = true;
  const auto envelope = compute_onset_strength(audio, mel_config, onset_config);

  PlpConfig config;
  config.sr = audio.sample_rate();
  config.hop_length = args.get_int("hop-length", config.hop_length);
  // An omitted bound keeps the library's search range.
  if (args.has("bpm-min")) config.tempo_min = args.get_float("bpm-min", config.tempo_min);
  if (args.has("bpm-max")) config.tempo_max = args.get_float("bpm-max", config.tempo_max);
  config.win_length = args.get_int("win-length", config.win_length);
  auto values = detail::plp_of_envelope(envelope, config);
  Stats stats = Stats::compute(values);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_frames", values.size())
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Predominant Local Pulse:\n";
    printf("  Frames: %zu\n", values.size());
    printf("  Stats:  mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std, stats.min,
           stats.max);
  }
  return 0;
}

int cmd_cqt(const CliArgs& args, const Audio& audio) {
  CqtConfig config;
  config.hop_length = args.get_int("hop-length", config.hop_length);
  if (args.has("fmin-hz")) config.fmin = args.fmin;
  config.n_bins = args.get_int("n-bins", config.n_bins);
  config.bins_per_octave = args.get_int("bins-per-octave", config.bins_per_octave);

  CqtResult result = cqt(audio, config);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_bins", result.n_bins())
        .kv("n_frames", result.n_frames())
        .kv("duration", result.duration())
        .kv("fmin_hz", config.fmin)
        .kv("bins_per_octave", config.bins_per_octave)
        .end_object()
        .print();
  } else {
    float fmax = config.fmin * std::pow(2.0f, static_cast<float>(config.n_bins) /
                                                  static_cast<float>(config.bins_per_octave));
    int octaves = config.n_bins / config.bins_per_octave;
    std::cout << "Constant-Q Transform:\n";
    printf("  Shape:           %d bins x %d frames\n", result.n_bins(), result.n_frames());
    printf("  Frequency Range: %.1f - %.1f Hz (%d octaves)\n", config.fmin, fmax, octaves);
    printf("  Duration:        %.2fs\n", result.duration());
  }
  return 0;
}

int cmd_vqt(const CliArgs& args, const Audio& audio) {
  VqtConfig config;
  config.hop_length = args.get_int("hop-length", config.hop_length);
  if (args.has("fmin-hz")) config.fmin = args.fmin;
  config.n_bins = args.get_int("n-bins", config.n_bins);
  config.bins_per_octave = args.get_int("bins-per-octave", config.bins_per_octave);
  // Left out, the core derives gamma from the ERB bandwidths; a given value is never negative.
  if (args.has("gamma")) config.gamma = args.get_float("gamma", 0.0f);
  config.filter_scale = args.get_float("filter-scale", config.filter_scale);

  VqtResult result = vqt(audio, config);
  // Report the gamma the bandwidths were built from, derived or given.
  const float gamma = resolve_vqt_gamma(config);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_bins", result.n_bins())
        .kv("n_frames", result.n_frames())
        .kv("duration", result.duration())
        .kv("fmin_hz", config.fmin)
        .kv("bins_per_octave", config.bins_per_octave)
        .kv("gamma", gamma)
        .end_object()
        .print();
  } else {
    float fmax = config.fmin * std::pow(2.0f, static_cast<float>(config.n_bins) /
                                                  static_cast<float>(config.bins_per_octave));
    int octaves = config.n_bins / config.bins_per_octave;
    std::cout << "Variable-Q Transform:\n";
    printf("  Shape:           %d bins x %d frames\n", result.n_bins(), result.n_frames());
    printf("  Frequency Range: %.1f - %.1f Hz (%d octaves)\n", config.fmin, fmax, octaves);
    printf("  Gamma:           %.2f\n", gamma);
    printf("  Duration:        %.2fs\n", result.duration());
  }
  return 0;
}

int cmd_mel_to_audio(const CliArgs& args, const Audio& audio) {
  MelConfig config;
  config.n_mels = args.get_int("n-mels", config.n_mels);
  config.n_fft = args.get_int("n-fft", config.n_fft);
  config.hop_length = args.get_int("hop-length", config.hop_length);
  config.fmin = args.get_float("fmin-hz", config.fmin);
  config.fmax = args.has("fmax-hz") ? args.fmax : static_cast<float>(audio.sample_rate()) / 2.0f;

  const int n_iter = args.get_int("n-iter", sonare::constants::kDefaultGriffinLimIterations);
  MelSpectrogram mel =
      with_mel_cli_subjects([&] { return MelSpectrogram::compute(audio, config); });
  Audio out = mel_to_audio(mel.power_data(), mel.n_mels(), mel.n_frames(), config, n_iter,
                           audio.sample_rate());
  const OutputFit fit = OutputFit().include(out.data(), out.size());
  write_wav(args.output_file, out.data(), out.size(), audio.sample_rate(), 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
        .kv("n_mels", mel.n_mels())
        .kv("n_frames", mel.n_frames())
        .kv("n_iter", n_iter)
        .kv("duration", out.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_mfcc_to_audio(const CliArgs& args, const Audio& audio) {
  MelConfig config;
  config.n_mels = args.get_int("n-mels", config.n_mels);
  config.n_fft = args.get_int("n-fft", config.n_fft);
  config.hop_length = args.get_int("hop-length", config.hop_length);
  config.fmin = args.get_float("fmin-hz", config.fmin);
  config.fmax = args.has("fmax-hz") ? args.fmax : static_cast<float>(audio.sample_rate()) / 2.0f;

  const int n_mfcc = args.get_int("n-mfcc", kMfccDefaultCount);
  const int n_iter = args.get_int("n-iter", sonare::constants::kDefaultGriffinLimIterations);
  MelSpectrogram mel =
      with_mel_cli_subjects([&] { return MelSpectrogram::compute(audio, config); });
  std::vector<float> mfcc = mel.mfcc(n_mfcc);
  Audio out =
      mfcc_to_audio(mfcc.data(), n_mfcc, mel.n_frames(), config, n_iter, audio.sample_rate());
  const OutputFit fit = OutputFit().include(out.data(), out.size());
  write_wav(args.output_file, out.data(), out.size(), audio.sample_rate(), 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
        .kv("n_mfcc", n_mfcc)
        .kv("n_frames", mel.n_frames())
        .kv("n_iter", n_iter)
        .kv("duration", out.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_acoustic(const CliArgs& args, const Audio& audio) {
  AcousticConfig config;
  // --ir is this command's mode selector, so the branch it does not take must
  // stay blind rather than letting AcousticConfig's Auto default re-route an
  // impulse-like input to IR analysis behind the user's back. Blind is also
  // what the C ABI's sonare_detect_acoustic forces, which is what the Python
  // CLI and every binding run, so the two shipped CLIs agree on the mode.
  config.mode = AcousticConfig::Mode::Blind;
  config.n_octave_bands = args.get_int("n-octave-bands", config.n_octave_bands);
  config.min_decay_db = args.get_float("min-decay-db", config.min_decay_db);
  config.noise_floor_margin_db =
      args.get_float("noise-floor-margin-db", config.noise_floor_margin_db);

  const bool use_ir = args.has("ir");
  AcousticParameters p =
      use_ir ? analyze_impulse_response(audio, config) : detect_acoustic(audio, config);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("rt60", p.rt60);
    // EDT and the clarity measures need a direct-sound arrival, which only an
    // impulse response has, so a blind estimate omits them.
    if (!p.is_blind) {
      json.kv("edt", p.edt).kv("c50", p.c50).kv("c80", p.c80).kv("d50", p.d50);
    }
    // A band that could not be measured is left out; each list carries its band indices.
    json.kv("confidence", p.confidence)
        .measured_array("rt60_bands", "rt60_band", p.rt60_bands.data(), p.rt60_bands.size());
    if (!p.is_blind) {
      json.measured_array("edt_bands", "edt_band", p.edt_bands.data(), p.edt_bands.size())
          .measured_array("c50_bands", "c50_band", p.c50_bands.data(), p.c50_bands.size())
          .measured_array("c80_bands", "c80_band", p.c80_bands.data(), p.c80_bands.size());
    }
    json.end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Acoustic Analysis ("
              << (p.is_blind ? "blind" : "impulse response") << "):" << color::reset << "\n";
    printf("    RT60:       %s\n", measured_text(p.rt60, 3, "s").c_str());
    if (p.is_blind) {
      std::cout << "    EDT:        -\n    C50:        -\n    C80:        -\n    D50:        -\n";
    } else {
      printf("    EDT:        %s\n", measured_text(p.edt, 3, "s").c_str());
      printf("    C50:        %s\n", measured_text(p.c50, 2, "dB").c_str());
      printf("    C80:        %s\n", measured_text(p.c80, 2, "dB").c_str());
      printf("    D50:        %s\n", measured_text(p.d50, 3).c_str());
    }
    printf("    Confidence: %.2f\n", p.confidence);
    if (!p.rt60_bands.empty()) {
      std::cout << "    RT60 bands: ";
      for (size_t i = 0; i < p.rt60_bands.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << measured_text(p.rt60_bands[i], 3);
      }
      std::cout << "\n";
    }
    std::cout << "\n";
  }
  return 0;
}

#ifdef SONARE_WITH_ACOUSTIC_SIM
namespace {

// Builds a uniform-absorption shoebox + placement from CLI flags.
sonare::acoustic::ShoeboxRoom cli_room(const CliArgs& args) {
  const sonare::RoomDimensions& room = sonare::acoustic::kDefaultRoomDims;
  return sonare::acoustic::uniform_shoebox(
      {args.get_float("length", room.length), args.get_float("width", room.width),
       args.get_float("height", room.height)},
      args.get_float("absorption", sonare::acoustic::kDefaultWallAbsorption));
}

// The source and listener coordinates the caller gave; the validator has refused a partly given
// role, so a role left out is left out whole and takes the placement scaled to the room.
sonare::acoustic::RoomPlacementRequest cli_placement_request(const CliArgs& args) {
  const auto given = [&](const char* name) -> std::optional<float> {
    if (!args.has(name)) return std::nullopt;
    return args.get_float(name, 0.0f);
  };
  sonare::acoustic::RoomPlacementRequest request;
  request.source = {given("source-x"), given("source-y"), given("source-z")};
  request.listener = {given("listener-x"), given("listener-y"), given("listener-z")};
  return request;
}

}  // namespace

int cmd_synthesize_rir(const CliArgs& args, const Audio&) {
  const int sample_rate = args.get_int("sample-rate", 48000);
  sonare::acoustic::RirSynthConfig cfg;
  cfg.ism_order = args.get_int("ism-order", cfg.ism_order);
  // Read at the C field's width: get_int cannot express the upper half of a
  // uint32, which put every seed above 2^31-1 out of reach here while the other
  // surfaces took it. Zero keeps the library default, as it does everywhere.
  if (const unsigned seed = args.get_uint32("seed", cfg.seed); seed > 0) cfg.seed = seed;
  cfg.max_seconds = args.get_float("max-sec", cfg.max_seconds);
  // --sabine selects the Sabine late-reverb model (default Eyring), matching the
  // core/C-ABI/Python-CLI selection so every surface exposes the same choice.
  cfg.late_model = args.has("sabine") ? sonare::acoustic::ReverbModel::Sabine
                                      : sonare::acoustic::ReverbModel::Eyring;

  const sonare::acoustic::ShoeboxRoom room = cli_room(args);
  const auto result = sonare::acoustic::synthesize_rir(
      room,
      sonare::acoustic::resolve_room_placement(room.dims, cli_placement_request(args)).placement,
      sample_rate, cfg);
  // Reported like every other refusal: the message alone, exiting with the invalid-parameter code.
  if (sonare::has_error(result.diagnostics)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  sonare::acoustic::first_error_text(result.diagnostics));
  }
  // Warnings survive a successful synthesis and describe a RIR the caller did
  // not ask for -- a tail cut against --max-sec, a cap raised to fit the
  // direct sound, a request reduced to early reflections alone. Dropping them
  // made a truncated RIR indistinguishable from a complete one. They go to
  // stderr in every mode so the --json document on stdout stays exactly the
  // payload both CLIs publish.
  for (const sonare::Diagnostic& diagnostic : result.diagnostics) {
    if (diagnostic.severity != sonare::Diagnostic::Severity::Warning) continue;
    std::cerr << color::yellow << "warning: " << diagnostic.code << ": " << diagnostic.message
              << color::reset << "\n";
  }
  // 24-bit, not the write_wav default. A synthesized RIR carries its physical
  // 1/(4*pi*d) attenuation, so its peak sits far below full scale and 16-bit
  // quantization would cost the tail roughly 36 dB of the headroom it needs;
  // half the reported samples came back exactly zero.
  const OutputFit fit = OutputFit().include(result.rir.data(), result.rir.size());
  write_wav(args.output_file, result.rir, 24, fit);
  note_output_fit(args, fit);
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
        .kv("samples", result.rir.size())
        .kv("sample_rate", result.rir.sample_rate())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved RIR (" << result.rir.size() << " samples) to "
              << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_estimate_room(const CliArgs& args, const Audio& audio) {
  sonare::RoomEstimateConfig cfg;
  // Match the C ABI: an explicit 0 aspect hint means "use the default 1.0".
  cfg.aspect_hint_lw = args.get_float("aspect-lw", cfg.aspect_hint_lw);
  if (cfg.aspect_hint_lw == 0.0f) cfg.aspect_hint_lw = 1.0f;
  cfg.aspect_hint_lh = args.get_float("aspect-lh", cfg.aspect_hint_lh);
  if (cfg.aspect_hint_lh == 0.0f) cfg.aspect_hint_lh = 1.0f;
  cfg.reference_absorption = args.get_float("reference-absorption", cfg.reference_absorption);
  cfg.prefer_eyring = !args.has("sabine");
  cfg.acoustic.n_octave_bands = args.get_int("n-octave-bands", cfg.acoustic.n_octave_bands);

  const sonare::RoomEstimate est = sonare::estimate_room(audio, cfg);
  if (args.json_output) {
    // The two band vectors are independent estimates and either can come back short or empty
    // on its own; each list carries the indices of the bands it measured.
    JsonBuilder()
        .begin_object()
        .kv("volume", est.volume)
        .kv("length", est.dims.length)
        .kv("width", est.dims.width)
        .kv("height", est.dims.height)
        .kv("drr_db", est.drr_db)
        .kv("confidence", est.confidence)
        .measured_array("rt60_bands", "rt60_band", est.rt60_bands.data(), est.rt60_bands.size())
        .measured_array("band_absorption", "band_absorption_band", est.absorption_bands.data(),
                        est.absorption_bands.size())
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Room Estimate:" << color::reset << "\n";
    printf("    Volume:     %s\n", measured_text(est.volume, 1, "m^3").c_str());
    if (std::isnan(est.dims.length) || std::isnan(est.dims.width) || std::isnan(est.dims.height)) {
      std::cout << "    Dimensions: -\n";
    } else {
      printf("    Dimensions: %.2f x %.2f x %.2f m\n", est.dims.length, est.dims.width,
             est.dims.height);
    }
    printf("    DRR:        %s\n", measured_text(est.drr_db, 2, "dB").c_str());
    printf("    Confidence: %.2f\n", est.confidence);
    std::cout << "\n";
  }
  return 0;
}

int cmd_room_morph(const CliArgs& args, const Audio& audio) {
  sonare::effects::acoustic::RoomMorphConfig cfg;
  cfg.target = cli_room(args);
  // An omitted spacing is fitted to the room and placement, as the room insert fits it.
  sonare::acoustic::RoomPlacementRequest request = cli_placement_request(args);
  if (args.has("receiver-spacing"))
    request.receiver_spacing_m = args.get_float("receiver-spacing", 0.0f);
  const sonare::acoustic::ResolvedRoomPlacement resolved =
      sonare::acoustic::resolve_room_placement(cfg.target.dims, request);
  cfg.placement = resolved.placement;
  cfg.receiver_spacing_m = request.receiver_spacing_m;
  cfg.supplied = resolved.supplied;
  cfg.source_tail_suppression = args.get_float("suppression", cfg.source_tail_suppression);
  cfg.wet = args.get_float("wet", cfg.wet);
  cfg.ism_order = args.get_int("ism-order", cfg.ism_order);
  // Read at the C field's width: get_int cannot express the upper half of a
  // uint32, which put every seed above 2^31-1 out of reach here while the other
  // surfaces took it. Zero keeps the library default, as it does everywhere.
  if (const unsigned seed = args.get_uint32("seed", cfg.seed); seed > 0) cfg.seed = seed;
  cfg.max_seconds = args.get_float("max-sec", cfg.max_seconds);
  // --sabine selects the Sabine late-reverb model (default Eyring), matching the
  // core/C-ABI/Python-CLI selection so every surface exposes the same choice.
  cfg.late_model = args.has("sabine") ? sonare::acoustic::ReverbModel::Sabine
                                      : sonare::acoustic::ReverbModel::Eyring;

  // A two-channel source is morphed as a pair heard by two receivers, so the
  // written file keeps the image; a mono source takes the single-receiver path.
  const bool stereo = args.source_channels == 2;
  StereoPlanes planes;
  sonare::effects::acoustic::RoomMorphStereoResult stereo_morph;
  sonare::effects::acoustic::RoomMorphResult mono_morph;
  try {
    if (stereo) {
      planes = load_stereo_planes(args, audio);
      const int rate = audio.sample_rate();
      stereo_morph = sonare::effects::acoustic::room_morph_stereo(
          Audio::from_vector(planes.left, rate), Audio::from_vector(planes.right, rate), cfg);
    } else {
      mono_morph = sonare::effects::acoustic::room_morph(audio, cfg);
    }
  } catch (const SonareParameterRefusal& refusal) {
    if (std::find(refusal.subjects().begin(), refusal.subjects().end(), "receiver_spacing_m") ==
        refusal.subjects().end()) {
      throw;
    }
    throw refusal.respelled(
        refusal.code(),
        [](const std::string& subject) {
          return subject == "receiver_spacing_m" ? std::string("--receiver-spacing") : subject;
        },
        true);
  }
  const auto& diagnostics = stereo ? stereo_morph.diagnostics : mono_morph.diagnostics;
  // The same loop synthesize-rir runs, for the same reason: a warning describes a
  // target room the caller did not ask for, and it goes to stderr in every mode so
  // the --json document on stdout stays the payload both CLIs publish.
  for (const sonare::Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity != sonare::Diagnostic::Severity::Warning) continue;
    std::cerr << color::yellow << "warning: " << diagnostic.code << ": " << diagnostic.message
              << color::reset << "\n";
  }
  size_t out_samples = 0;
  OutputFit fit;
  if (stereo) {
    const Audio& left = stereo_morph.left;
    const Audio& right = stereo_morph.right;
    out_samples = left.size();
    fit.include(left).include(right);
    write_stereo_wav(args.output_file, left, right, left.sample_rate(), 16, fit);
  } else {
    const Audio& result = mono_morph.audio;
    out_samples = result.size();
    fit.include(result);
    write_wav(args.output_file, result, 16, fit);
  }
  note_output_fit(args, fit);
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
        .kv("samples", out_samples)
        .kv("stereo", stereo)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved morphed audio to " << args.output_file << color::reset
              << "\n";
  }
  return 0;
}
#endif  // SONARE_WITH_ACOUSTIC_SIM

int cmd_onset_strength(const CliArgs& args, const Audio& audio) {
  MelConfig mel_config;
  mel_config.n_fft = args.get_int("n-fft", mel_config.n_fft);
  mel_config.hop_length = args.get_int("hop-length", mel_config.hop_length);
  mel_config.n_mels = args.get_int("n-mels", mel_config.n_mels);

  auto envelope = compute_onset_strength(audio, mel_config, OnsetConfig());
  Stats stats = Stats::compute(envelope);
  // The time of the first frame holding stats.max; 0 for an empty envelope, as the stats are.
  const size_t peak_frame =
      envelope.empty() ? 0
                       : static_cast<size_t>(std::distance(
                             envelope.begin(), std::max_element(envelope.begin(), envelope.end())));
  const double peak_time =
      static_cast<double>(peak_frame) * mel_config.hop_length / audio.sample_rate();

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("count", envelope.size())
        .kv("hop_length", mel_config.hop_length)
        .kv("duration", audio.duration())
        .kv("peak_time", peak_time)
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .key("values")
        .float_array(envelope)
        .end_object()
        .print();
  } else {
    std::cout << "Onset Strength Envelope:\n";
    printf("  Frames: %zu\n", envelope.size());
    printf("  Stats:  mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std, stats.min,
           stats.max);
    printf("  Peak:   %.2fs\n", peak_time);
  }
  return 0;
}

int cmd_fourier_tempogram(const CliArgs& args, const Audio& audio) {
  TempogramConfig config;
  config.hop_length = args.get_int("hop-length", config.hop_length);
  config.win_length = args.get_int("win-length", config.win_length);

  auto values = fourier_tempogram(audio, config);
  const int n_bins = config.win_length / 2 + 1;
  const int n_frames = n_bins > 0 ? static_cast<int>(values.size()) / n_bins : 0;
  Stats stats = Stats::compute(values);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("win_length", config.win_length)
        .kv("n_bins", n_bins)
        .kv("n_frames", n_frames)
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Fourier Tempogram:\n";
    printf("  Shape: %d bins x %d frames\n", n_bins, n_frames);
    printf("  Stats: mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std, stats.min,
           stats.max);
  }
  return 0;
}

int cmd_tempogram_ratio(const CliArgs& args, const Audio& audio) {
  TempogramConfig config;
  config.hop_length = args.get_int("hop-length", config.hop_length);
  config.win_length = args.get_int("win-length", config.win_length);

  auto tempogram_data = tempogram(audio, config);
  static const std::vector<float> factors = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
  auto ratios = tempogram_ratio(tempogram_data, config.win_length, audio.sample_rate(),
                                config.hop_length, factors);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("win_length", config.win_length).key("ratios").begin_array();
    for (size_t i = 0; i < ratios.size(); ++i) {
      json.begin_object().kv("factor", factors[i]).kv("value", ratios[i]).end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "Tempogram Ratio:\n";
    for (size_t i = 0; i < ratios.size(); ++i) {
      printf("  factor %.1f: %.4f\n", factors[i], ratios[i]);
    }
  }
  return 0;
}

int cmd_nnls_chroma(const CliArgs& args, const Audio& audio) {
  NnlsChromaConfig config;
  config.cqt.bins_per_octave = 12;
  config.cqt.n_bins = 84;
  config.cqt.hop_length = args.get_int("hop-length", config.cqt.hop_length);
  config.midi_min = 24;
  config.n_pitches = 60;
  config.n_harmonics = 4;
  config.max_iter = 25;
  config.tolerance = 1.0e-3f;

  Chroma chroma = nnls_chroma(audio, config);
  auto mean_energy = chroma.mean_energy();
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("n_chroma", chroma.n_chroma())
        .kv("n_frames", chroma.n_frames())
        .kv("duration", chroma.duration())
        .key("mean_energy")
        .begin_array();
    // One pitch-class vector, one representation: `chroma` publishes this array
    // and a consumer must not need a second reader for the NNLS variant.
    for (float value : mean_energy) json.value(value);
    json.end_array().end_object().print();
  } else {
    std::cout << "NNLS Chromagram:\n";
    printf("  Shape:    %d bins x %d frames\n", chroma.n_chroma(), chroma.n_frames());
    printf("  Duration: %.2fs\n", chroma.duration());
    std::cout << "\nMean Energy by Pitch Class:\n";
    float max_e = *std::max_element(mean_energy.begin(), mean_energy.end());
    if (max_e <= 0.0f) max_e = 1.0f;
    for (int i = 0; i < 12; ++i) {
      int bar = static_cast<int>(mean_energy[i] / max_e * 20);
      printf("  %-2s: %.3f ", names[i], mean_energy[i]);
      for (int j = 0; j < bar; ++j) std::cout << "*";
      std::cout << "\n";
    }
  }
  return 0;
}

int cmd_lufs(const CliArgs& args, const Audio&) {
  using namespace sonare::metering;
  const LufsConfig config;
  const std::vector<float>& interleaved = cached_interleaved_audio(args);
  const int channels = args.decoded_input->retained_channels;
  SONARE_CHECK_MSG(channels > 0 && interleaved.size() % static_cast<size_t>(channels) == 0,
                   ErrorCode::DecodeFailed, "decoded audio has an inconsistent channel count");
  const bool series = args.has("series");
  std::vector<float> momentary_series;
  std::vector<float> short_term_series;
  const LufsResult result =
      lufs_interleaved(interleaved.data(), interleaved.size() / static_cast<size_t>(channels),
                       channels, args.decoded_input->sample_rate, config,
                       series ? &momentary_series : nullptr, series ? &short_term_series : nullptr);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("integrated_lufs", result.integrated_lufs)
        .kv("momentary_lufs", result.momentary_lufs)
        .kv("short_term_lufs", result.short_term_lufs)
        .kv("loudness_range_lu", result.loudness_range_lu);
    if (series) {
      json.key("momentary_series").float_array(momentary_series);
      json.key("short_term_series").float_array(short_term_series);
    }
    json.end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Loudness (LUFS):" << color::reset << "\n";
    printf("    Integrated:    %s\n", measured_text(result.integrated_lufs, 2, "LUFS").c_str());
    printf("    Momentary:     %s\n", measured_text(result.momentary_lufs, 2, "LUFS").c_str());
    printf("    Short-term:    %s\n", measured_text(result.short_term_lufs, 2, "LUFS").c_str());
    printf("    Loudness Range: %s\n", measured_text(result.loudness_range_lu, 2, "LU").c_str());
    std::cout << "\n";
  }
  return 0;
}

int cmd_meter(const CliArgs& args, const Audio& audio) {
  const float clip_threshold = metering::clipping_threshold_from_db(
      args.get_float("clip-threshold-db", std::numeric_limits<float>::quiet_NaN()));
  const int oversample = args.get_int("oversample", metering::kDefaultTruePeakOversample);

  const float peak = metering::peak_db(audio);
  const float rms = metering::rms_db(audio);
  const float crest = metering::crest_factor_db(audio);
  const float clip_ratio = metering::clipping_ratio(audio, clip_threshold);
  const float silence = metering::silence_ratio(audio);
  const float dc = metering::dc_offset(audio);
  const float tp = metering::true_peak_db(audio, oversample);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("peak_db", peak)
        .kv("rms_db", rms)
        .kv("crest_factor_db", crest)
        .kv("true_peak_db", tp)
        .kv("clipping_ratio", clip_ratio)
        .kv("silence_ratio", silence)
        .kv("dc_offset", dc)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Meters:" << color::reset << "\n";
    printf("    Peak:           %s\n", measured_text(peak, 2, "dB").c_str());
    printf("    RMS:            %s\n", measured_text(rms, 2, "dB").c_str());
    printf("    Crest Factor:   %s\n", measured_text(crest, 2, "dB").c_str());
    printf("    True Peak:      %s\n", measured_text(tp, 2, "dBTP").c_str());
    printf("    Clipping Ratio: %s\n", measured_text(clip_ratio, 6).c_str());
    printf("    Silence Ratio:  %s\n", measured_text(silence, 6).c_str());
    printf("    DC Offset:      %s\n", measured_text(dc, 6).c_str());
    std::cout << "\n";
  }
  return 0;
}

int cmd_clipping(const CliArgs& args, const Audio& audio) {
  const float threshold = metering::clipping_threshold_from_db(
      args.get_float("threshold-db", std::numeric_limits<float>::quiet_NaN()));
  const auto min_region = static_cast<size_t>(
      args.get_int("min-region", static_cast<int>(metering::kDefaultClippingMinRegionSamples)));

  metering::ClippingResult result = metering::detect_clipping(audio, threshold, min_region);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("clipped_samples", result.clipped_samples)
        .kv("clipping_ratio", result.clipping_ratio)
        .kv("max_clipped_peak", result.max_clipped_peak)
        .key("regions")
        .begin_array();
    for (const auto& region : result.regions) {
      json.begin_object()
          .kv("start_sample", region.start_sample)
          .kv("end_sample", region.end_sample)
          .kv("length", region.length)
          .kv("peak", region.peak)
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "Clipping Detection:\n";
    printf("  Clipped Samples: %zu\n", result.clipped_samples);
    printf("  Clipping Ratio:  %s\n", measured_text(result.clipping_ratio, 6).c_str());
    printf("  Max Peak:        %.4f\n", result.max_clipped_peak);
    printf("  Regions:         %zu\n", result.regions.size());
    const size_t shown = std::min<size_t>(result.regions.size(), 20);
    for (size_t i = 0; i < shown; ++i) {
      const auto& region = result.regions[i];
      printf("    [%zu] %zu - %zu (len %zu, peak %.4f)\n", i, region.start_sample,
             region.end_sample, region.length, region.peak);
    }
    if (result.regions.size() > shown) {
      printf("    ... (%zu more regions)\n", result.regions.size() - shown);
    }
  }
  return 0;
}

int cmd_dynamic_range(const CliArgs& args, const Audio& audio) {
  metering::DynamicRangeConfig config;
  config.window_sec = args.get_float("window-sec", config.window_sec);
  config.hop_sec = args.get_float("hop-sec", config.hop_sec);
  config.low_percentile = args.get_float("low-percentile", config.low_percentile);
  config.high_percentile = args.get_float("high-percentile", config.high_percentile);

  metering::DynamicRangeResult result = metering::dynamic_range(audio, config);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("dynamic_range_db", result.dynamic_range_db)
        .kv("low_percentile_db", result.low_percentile_db)
        .kv("high_percentile_db", result.high_percentile_db)
        .key("window_rms_db")
        .float_array(result.window_rms_db)
        .end_object()
        .print();
  } else {
    std::cout << "Dynamic Range:\n";
    printf("  Dynamic Range:   %s\n", measured_text(result.dynamic_range_db, 2, "dB").c_str());
    printf("  Low Percentile:  %s\n", measured_text(result.low_percentile_db, 2, "dB").c_str());
    printf("  High Percentile: %s\n", measured_text(result.high_percentile_db, 2, "dB").c_str());
  }
  return 0;
}

int cmd_stereo(const CliArgs& args, const Audio& audio) {
  const Audio right = load_reference_audio(args, audio.sample_rate(), audio.size());
  const float corr = metering::correlation(audio.data(), right.data(), audio.size());
  const float width = metering::stereo_width(audio.data(), right.data(), audio.size());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("correlation", corr)
        .kv("stereo_width", width)
        .end_object()
        .print();
  } else {
    std::cout << "Stereo Image:\n";
    printf("  Correlation:  %s\n", measured_text(corr, 4).c_str());
    printf("  Stereo Width: %s\n", measured_text(width, 4).c_str());
  }
  return 0;
}

int cmd_phase(const CliArgs& args, const Audio& audio) {
  const Audio right = load_reference_audio(args, audio.sample_rate(), audio.size());
  metering::PhaseScopeResult result =
      metering::phase_scope(audio.data(), right.data(), audio.size());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("correlation", result.correlation)
        .kv("average_abs_angle_rad", result.average_abs_angle_rad)
        .kv("max_radius", result.max_radius)
        .end_object()
        .print();
  } else {
    std::cout << "Phase Scope:\n";
    printf("  Correlation:        %s\n", measured_text(result.correlation, 4).c_str());
    printf("  Avg Abs Angle:      %s\n",
           measured_text(result.average_abs_angle_rad, 4, "rad").c_str());
    printf("  Max Radius:         %.4f\n", result.max_radius);
  }
  return 0;
}
