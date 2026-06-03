#include "sonare_cli.h"

int cmd_mel(const CliArgs& args, const Audio& audio) {
  MelConfig config;
  config.n_mels = args.n_mels;
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
  config.fmin = args.fmin;
  config.fmax = args.fmax > 0 ? args.fmax : static_cast<float>(audio.sample_rate()) / 2.0f;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);
  const float* data = mel.power_data();
  size_t total = static_cast<size_t>(mel.n_mels()) * static_cast<size_t>(mel.n_frames());

  float min_v = *std::min_element(data, data + total);
  float max_v = *std::max_element(data, data + total);
  float mean_v = std::accumulate(data, data + total, 0.0f) / static_cast<float>(total);

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
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;

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
        .begin_object();
    for (int i = 0; i < 12; ++i) json.kv(names[i], mean_energy[i]);
    json.end_object().end_object().print();
  } else {
    std::cout << "Chromagram:\n";
    printf("  Shape:    %d bins x %d frames\n", chroma.n_chroma(), chroma.n_frames());
    printf("  Duration: %.2fs\n", chroma.duration());
    std::cout << "\nMean Energy by Pitch Class:\n";
    float max_e = *std::max_element(mean_energy.begin(), mean_energy.end());
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
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
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
  StftConfig stft{args.n_fft, args.hop_length};
  Spectrogram spec = Spectrogram::compute(audio, stft);
  int sr = audio.sample_rate();

  auto centroid = spectral_centroid(spec, sr);
  auto bandwidth = spectral_bandwidth(spec, sr);
  auto rolloff = spectral_rolloff(spec, sr);
  auto flatness = spectral_flatness(spec);
  auto zcr = zero_crossing_rate(audio, args.n_fft, args.hop_length);
  auto rms = rms_energy(audio, args.n_fft, args.hop_length);

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
        .end_object()
        .key("bandwidth")
        .begin_object()
        .kv("mean", sb.mean)
        .kv("std", sb.std)
        .end_object()
        .key("rolloff")
        .begin_object()
        .kv("mean", sr_s.mean)
        .kv("std", sr_s.std)
        .end_object()
        .key("flatness")
        .begin_object()
        .kv("mean", sf.mean)
        .kv("std", sf.std)
        .end_object()
        .key("zcr")
        .begin_object()
        .kv("mean", sz.mean)
        .kv("std", sz.std)
        .end_object()
        .key("rms")
        .begin_object()
        .kv("mean", se.mean)
        .kv("std", se.std)
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
  PitchConfig config;
  config.fmin = args.get_float("fmin", 65.0f);
  config.fmax = args.get_float("fmax", 2093.0f);
  config.threshold = args.get_float("threshold", 0.3f);
  config.hop_length = args.hop_length;

  std::string algo = args.get_string("algorithm", "pyin");
  PitchResult result = (algo == "yin") ? yin_track(audio, config) : pyin(audio, config);

  int voiced = std::count(result.voiced_flag.begin(), result.voiced_flag.end(), true);
  float ratio = static_cast<float>(voiced) / static_cast<float>(result.n_frames());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("algorithm", algo)
        .kv("n_frames", result.n_frames())
        .kv("voiced_count", voiced)
        .kv("voiced_ratio", ratio)
        .kv("median_f0", result.median_f0())
        .kv("mean_f0", result.mean_f0())
        .end_object()
        .print();
  } else {
    std::cout << "Pitch Tracking (" << algo << "):\n";
    printf("  Frames:    %d\n", result.n_frames());
    printf("  Voiced:    %d (%.1f%%)\n", voiced, ratio * 100.0f);
    printf("  Median F0: %.1f Hz\n", result.median_f0());
    printf("  Mean F0:   %.1f Hz\n", result.mean_f0());
  }
  return 0;
}

int cmd_onset_env(const CliArgs& args, const Audio& audio) {
  MelConfig mel_config;
  mel_config.n_fft = args.n_fft;
  mel_config.hop_length = args.hop_length;
  mel_config.n_mels = args.n_mels;

  auto envelope = compute_onset_strength(audio, mel_config);

  auto max_it = std::max_element(envelope.begin(), envelope.end());
  float peak = *max_it;
  int peak_frame = static_cast<int>(std::distance(envelope.begin(), max_it));
  float peak_time = static_cast<float>(peak_frame * args.hop_length) / audio.sample_rate();
  float mean =
      std::accumulate(envelope.begin(), envelope.end(), 0.0f) / static_cast<float>(envelope.size());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_frames", envelope.size())
        .kv("duration", audio.duration())
        .kv("hop_length", args.hop_length)
        .kv("peak_time", peak_time)
        .kv("peak_strength", peak)
        .kv("mean", mean)
        .end_object()
        .print();
  } else {
    std::cout << "Onset Strength Envelope:\n";
    printf("  Frames:        %zu\n", envelope.size());
    printf("  Duration:      %.2fs\n", audio.duration());
    printf("  Peak Time:     %.2fs\n", peak_time);
    printf("  Peak Strength: %.3f\n", peak);
    printf("  Mean:          %.3f\n", mean);
  }
  return 0;
}

int cmd_tempogram(const CliArgs& args, const Audio& audio) {
  TempogramConfig config;
  config.hop_length = args.hop_length;
  config.win_length = args.get_int("win-length", 384);
  auto values = tempogram(audio, config);
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
  PlpConfig config;
  config.sr = audio.sample_rate();
  config.hop_length = args.hop_length;
  config.tempo_min = args.get_float("tempo-min", 30.0f);
  config.tempo_max = args.get_float("tempo-max", 300.0f);
  config.win_length = args.get_int("win-length", 384);
  auto values = plp(audio, config);
  Stats stats = Stats::compute(values);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_frames", values.size())
        .kv("tempo_min", config.tempo_min)
        .kv("tempo_max", config.tempo_max)
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
  config.hop_length = args.hop_length;
  config.fmin = args.get_float("fmin", 32.7f);
  config.n_bins = args.get_int("n-bins", 84);
  config.bins_per_octave = args.get_int("bins-per-octave", 12);

  CqtResult result = cqt(audio, config);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_bins", result.n_bins())
        .kv("n_frames", result.n_frames())
        .kv("duration", result.duration())
        .kv("fmin", config.fmin)
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
  config.hop_length = args.hop_length;
  config.fmin = args.fmin > 0.0f ? args.fmin : 32.7f;
  config.n_bins = args.get_int("n-bins", 84);
  config.bins_per_octave = args.get_int("bins-per-octave", 12);
  config.gamma = args.get_float("gamma", 0.0f);
  config.filter_scale = args.get_float("filter-scale", 1.0f);

  VqtResult result = vqt(audio, config);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_bins", result.n_bins())
        .kv("n_frames", result.n_frames())
        .kv("duration", result.duration())
        .kv("fmin", config.fmin)
        .kv("bins_per_octave", config.bins_per_octave)
        .kv("gamma", config.gamma)
        .end_object()
        .print();
  } else {
    float fmax = config.fmin * std::pow(2.0f, static_cast<float>(config.n_bins) /
                                                  static_cast<float>(config.bins_per_octave));
    int octaves = config.n_bins / config.bins_per_octave;
    std::cout << "Variable-Q Transform:\n";
    printf("  Shape:           %d bins x %d frames\n", result.n_bins(), result.n_frames());
    printf("  Frequency Range: %.1f - %.1f Hz (%d octaves)\n", config.fmin, fmax, octaves);
    printf("  Gamma:           %.2f\n", config.gamma);
    printf("  Duration:        %.2fs\n", result.duration());
  }
  return 0;
}

int cmd_mel_to_audio(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: mel-to-audio requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  MelConfig config;
  config.n_mels = args.n_mels;
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
  config.fmin = args.fmin;
  config.fmax = args.fmax > 0 ? args.fmax : static_cast<float>(audio.sample_rate()) / 2.0f;

  const int n_iter = args.get_int("n-iter", 32);
  MelSpectrogram mel = MelSpectrogram::compute(audio, config);
  Audio out = mel_to_audio(mel.power_data(), mel.n_mels(), mel.n_frames(), config, n_iter,
                           audio.sample_rate());
  save_wav(args.output_file, out.data(), out.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
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
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: mfcc-to-audio requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  MelConfig config;
  config.n_mels = args.n_mels;
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
  config.fmin = args.fmin;
  config.fmax = args.fmax > 0 ? args.fmax : static_cast<float>(audio.sample_rate()) / 2.0f;

  const int n_mfcc = args.get_int("n-mfcc", 13);
  const int n_iter = args.get_int("n-iter", 32);
  MelSpectrogram mel = MelSpectrogram::compute(audio, config);
  std::vector<float> mfcc = mel.mfcc(n_mfcc);
  Audio out =
      mfcc_to_audio(mfcc.data(), n_mfcc, mel.n_frames(), config, n_iter, audio.sample_rate());
  save_wav(args.output_file, out.data(), out.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
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
  config.n_octave_bands = args.get_int("n-bands", config.n_octave_bands);
  config.min_decay_db = args.get_float("min-decay-db", config.min_decay_db);
  config.noise_floor_margin_db =
      args.get_float("noise-floor-margin-db", config.noise_floor_margin_db);

  const bool use_ir = args.has("ir");
  AcousticParameters p =
      use_ir ? analyze_impulse_response(audio, config) : detect_acoustic(audio, config);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("rt60", p.rt60)
        .kv("edt", p.edt)
        .kv("c50", p.c50)
        .kv("c80", p.c80)
        .kv("d50", p.d50)
        .kv("confidence", p.confidence)
        .kv("is_blind", p.is_blind)
        .key("rt60_bands")
        .float_array(p.rt60_bands)
        .key("edt_bands")
        .float_array(p.edt_bands)
        .key("c50_bands")
        .float_array(p.c50_bands)
        .key("c80_bands")
        .float_array(p.c80_bands)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Acoustic Analysis ("
              << (p.is_blind ? "blind" : "impulse response") << "):" << color::reset << "\n";
    printf("    RT60:       %.3f s\n", p.rt60);
    printf("    EDT:        %.3f s\n", p.edt);
    printf("    C50:        %.2f dB\n", p.c50);
    printf("    C80:        %.2f dB\n", p.c80);
    printf("    D50:        %.2f\n", p.d50);
    printf("    Confidence: %.2f\n", p.confidence);
    if (!p.rt60_bands.empty()) {
      std::cout << "    RT60 bands: ";
      for (size_t i = 0; i < p.rt60_bands.size(); ++i) {
        if (i > 0) std::cout << ", ";
        printf("%.3f", p.rt60_bands[i]);
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
sonare::acoustic::ShoeboxRoom cli_room(const CliArgs& args, float def_absorption) {
  return sonare::acoustic::uniform_shoebox(
      {args.get_float("length", 7.0f), args.get_float("width", 5.0f),
       args.get_float("height", 3.0f)},
      args.get_float("absorption", def_absorption));
}

sonare::acoustic::SourceListener cli_placement(const CliArgs& args) {
  return {{args.get_float("source-x", 1.0f), args.get_float("source-y", 1.0f),
           args.get_float("source-z", 1.2f)},
          {args.get_float("listener-x", 5.0f), args.get_float("listener-y", 4.0f),
           args.get_float("listener-z", 1.7f)}};
}

}  // namespace

int cmd_synthesize_rir(const CliArgs& args, const Audio&) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: synthesize-rir requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  const int sample_rate = args.get_int("sample-rate", 48000);
  sonare::acoustic::RirSynthConfig cfg;
  cfg.ism_order = args.get_int("ism-order", cfg.ism_order);
  cfg.seed = static_cast<unsigned>(std::max(0, args.get_int("seed", static_cast<int>(cfg.seed))));
  cfg.max_seconds = args.get_float("max-seconds", cfg.max_seconds);

  const auto result =
      sonare::acoustic::synthesize_rir(cli_room(args, 0.2f), cli_placement(args), sample_rate, cfg);
  if (sonare::has_error(result.diagnostics)) {
    std::cerr << color::red << "Error: invalid room geometry (source/listener outside the room)"
              << color::reset << "\n";
    return 1;
  }
  save_wav(args.output_file, result.rir.data(), result.rir.size(), result.rir.sample_rate());
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
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
  cfg.aspect_hint_lw = args.get_float("aspect-lw", cfg.aspect_hint_lw);
  cfg.aspect_hint_lh = args.get_float("aspect-lh", cfg.aspect_hint_lh);
  cfg.reference_absorption = args.get_float("reference-absorption", cfg.reference_absorption);
  cfg.prefer_eyring = !args.has("sabine");
  cfg.acoustic.n_octave_bands = args.get_int("n-bands", cfg.acoustic.n_octave_bands);

  const sonare::RoomEstimate est = sonare::estimate_room(audio, cfg);
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("volume", est.volume)
        .kv("length", est.dims.length)
        .kv("width", est.dims.width)
        .kv("height", est.dims.height)
        .kv("drr_db", est.drr_db)
        .kv("confidence", est.confidence)
        .key("rt60_bands")
        .float_array(est.rt60_bands)
        .key("absorption_bands")
        .float_array(est.absorption_bands)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Room Estimate:" << color::reset << "\n";
    printf("    Volume:     %.1f m^3\n", est.volume);
    printf("    Dimensions: %.2f x %.2f x %.2f m\n", est.dims.length, est.dims.width,
           est.dims.height);
    printf("    DRR:        %.2f dB\n", est.drr_db);
    printf("    Confidence: %.2f\n", est.confidence);
    std::cout << "\n";
  }
  return 0;
}

int cmd_room_morph(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: room-morph requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  sonare::effects::acoustic::RoomMorphConfig cfg;
  cfg.target = cli_room(args, 0.2f);
  cfg.placement = cli_placement(args);
  cfg.source_tail_suppression = args.get_float("suppression", cfg.source_tail_suppression);
  cfg.wet = args.get_float("wet", cfg.wet);
  cfg.ism_order = args.get_int("ism-order", cfg.ism_order);
  cfg.seed = static_cast<unsigned>(std::max(0, args.get_int("seed", static_cast<int>(cfg.seed))));
  cfg.max_seconds = args.get_float("max-seconds", cfg.max_seconds);

  const Audio result = sonare::effects::acoustic::room_morph(audio, cfg);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("samples", result.size())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved morphed audio to " << args.output_file << color::reset
              << "\n";
  }
  return 0;
}
#endif  // SONARE_WITH_ACOUSTIC_SIM

int cmd_onset_envelope(const CliArgs& args, const Audio& audio) {
  MelConfig mel_config;
  mel_config.n_fft = args.n_fft;
  mel_config.hop_length = args.hop_length;
  mel_config.n_mels = args.n_mels;

  auto envelope = compute_onset_strength(audio, mel_config, OnsetConfig());
  Stats stats = Stats::compute(envelope);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("count", envelope.size())
        .kv("hop_length", args.hop_length)
        .kv("duration", audio.duration())
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
  }
  return 0;
}

int cmd_fourier_tempogram(const CliArgs& args, const Audio& audio) {
  TempogramConfig config;
  config.hop_length = args.hop_length;
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
  config.hop_length = args.hop_length;
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
  config.cqt.hop_length = args.hop_length;
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
        .begin_object();
    for (int i = 0; i < 12; ++i) json.kv(names[i], mean_energy[i]);
    json.end_object().end_object().print();
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

int cmd_lufs(const CliArgs& args, const Audio& audio) {
  using namespace sonare::metering;
  LufsConfig config;
  LufsResult result = lufs(audio, config);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("integrated_lufs", result.integrated_lufs)
        .kv("momentary_lufs", result.momentary_lufs)
        .kv("short_term_lufs", result.short_term_lufs)
        .kv("loudness_range", result.loudness_range);
    if (args.has("series")) {
      json.key("momentary_series").float_array(momentary_lufs(audio, config));
      json.key("short_term_series").float_array(short_term_lufs(audio, config));
    }
    json.end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Loudness (LUFS):" << color::reset << "\n";
    printf("    Integrated:    %.2f LUFS\n", result.integrated_lufs);
    printf("    Momentary:     %.2f LUFS\n", result.momentary_lufs);
    printf("    Short-term:    %.2f LUFS\n", result.short_term_lufs);
    printf("    Loudness Range: %.2f LU\n", result.loudness_range);
    std::cout << "\n";
  }
  return 0;
}

int cmd_meter(const CliArgs& args, const Audio& audio) {
  const float clip_threshold = args.get_float("clip-threshold", 0.999f);
  const int oversample = args.get_int("oversample", 4);

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
    printf("    Peak:           %.2f dB\n", peak);
    printf("    RMS:            %.2f dB\n", rms);
    printf("    Crest Factor:   %.2f dB\n", crest);
    printf("    True Peak:      %.2f dBTP\n", tp);
    printf("    Clipping Ratio: %.6f\n", clip_ratio);
    printf("    Silence Ratio:  %.6f\n", silence);
    printf("    DC Offset:      %.6f\n", dc);
    std::cout << "\n";
  }
  return 0;
}

int cmd_clipping(const CliArgs& args, const Audio& audio) {
  const float threshold = args.get_float("threshold", 0.999f);
  const size_t min_region = static_cast<size_t>(args.get_int("min-region", 1));

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
    printf("  Clipping Ratio:  %.6f\n", result.clipping_ratio);
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
    printf("  Dynamic Range:   %.2f dB\n", result.dynamic_range_db);
    printf("  Low Percentile:  %.2f dB\n", result.low_percentile_db);
    printf("  High Percentile: %.2f dB\n", result.high_percentile_db);
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
    printf("  Correlation:  %.4f\n", corr);
    printf("  Stereo Width: %.4f\n", width);
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
    printf("  Correlation:        %.4f\n", result.correlation);
    printf("  Avg Abs Angle:      %.4f rad\n", result.average_abs_angle_rad);
    printf("  Max Radius:         %.4f\n", result.max_radius);
  }
  return 0;
}
