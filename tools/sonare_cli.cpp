/// @file sonare_cli.cpp
/// @brief Command-line interface for sonare audio analysis.

#include "cli/sonare_cli.h"

// ============================================================================
// Command Registry
// ============================================================================

const std::vector<CommandInfo>& get_commands() {
  static std::vector<CommandInfo> commands = {
      // Analysis
      {"analyze", "Full music analysis", cmd_analyze, true},
      {"bpm", "Detect BPM only", cmd_bpm, true},
      {"key", "Detect key only", cmd_key, true},
      {"beats", "Detect beat times", cmd_beats, true},
      {"downbeats", "Detect downbeat times", cmd_downbeats, true},
      {"onsets", "Detect onset times", cmd_onsets, true},
      {"chords", "Detect chord progression", cmd_chords, true},
      {"sections", "Detect song structure", cmd_sections, true},
      {"timbre", "Analyze timbral characteristics", cmd_timbre, true},
      {"dynamics", "Analyze dynamics/loudness", cmd_dynamics, true},
      {"rhythm", "Analyze rhythm features", cmd_rhythm, true},
      {"melody", "Track melody/pitch contour", cmd_melody, true},
      {"boundaries", "Detect structural boundaries", cmd_boundaries, true},
      {"acoustic", "Analyze room acoustics (RT60/EDT/clarity)", cmd_acoustic, true},
#ifdef SONARE_WITH_ACOUSTIC_SIM
      {"estimate-room", "Estimate equivalent room (volume/dims/absorption/DRR)", cmd_estimate_room,
       true},
      {"synthesize-rir", "Synthesize a room impulse response from geometry (-o out.wav)",
       cmd_synthesize_rir, false},
      {"room-morph", "Morph reverberation toward a target room (-o out.wav)", cmd_room_morph, true},
#endif
      {"lufs", "Measure loudness (LUFS / loudness range)", cmd_lufs, true},
      {"meter", "Measure basic level meters (peak/RMS/crest/true-peak)", cmd_meter, true},
      {"clipping", "Detect clipped sample regions", cmd_clipping, true},
      {"dynamic-range", "Measure dynamic range (percentile RMS)", cmd_dynamic_range, true},
      {"stereo", "Measure stereo correlation/width (needs --reference)", cmd_stereo, true},
      {"phase", "Measure phase scope summary (needs --reference)", cmd_phase, true},
      // Processing
      {"pitch-shift", "Shift pitch by semitones", cmd_pitch_shift, true},
      {"time-stretch", "Time stretch audio", cmd_time_stretch, true},
      {"pitch-correct", "Correct pitch to target MIDI note", cmd_pitch_correct, true},
      {"note-stretch", "Stretch a note region", cmd_note_stretch, true},
      {"voice-change", "Apply pitch and formant voice change", cmd_voice_change, true},
      {"voice-presets", "List realtime voice changer presets", cmd_voice_presets, false},
      {"voice-preset", "Print a realtime voice changer preset JSON", cmd_voice_preset, false},
      {"voice-preset-validate", "Normalize a realtime voice changer preset JSON",
       cmd_voice_preset_validate, false},
      {"hpss", "Harmonic-percussive separation", cmd_hpss, true},
      {"preemphasis", "Apply pre-emphasis filtering", cmd_preemphasis, true},
      {"deemphasis", "Apply de-emphasis filtering", cmd_deemphasis, true},
      {"trim-silence", "Trim leading/trailing silence", cmd_trim_silence, true},
      {"split-silence", "List non-silent intervals", cmd_split_silence, true},
      {"normalize", "Normalize audio (peak or rms)", cmd_normalize, true},
      {"gain", "Apply gain in dB", cmd_gain, true},
      {"fade", "Apply fade in/out", cmd_fade, true},
      {"filter", "Apply biquad filter (hp/lp/bp/notch)", cmd_filter, true},
      {"resample", "Resample audio to a target sample rate", cmd_resample, true},
      {"tone", "Generate a pure tone", cmd_tone, false},
      {"chirp", "Generate a frequency sweep", cmd_chirp, false},
      {"clicks", "Generate a click track", cmd_clicks, false},
#ifdef SONARE_WITH_MASTERING
      {"mastering", "Apply mastering loudness/true-peak processing", cmd_mastering, true},
      {"eq", "Apply the unified mastering equalizer", cmd_eq, true},
      {"mastering-processor", "Apply a named mastering processor", cmd_mastering_processor, true},
      {"mastering-pair-processor", "Apply a two-input mastering processor",
       cmd_mastering_pair_processor, true},
      {"mastering-pair-analyze", "Run a two-input mastering analysis", cmd_mastering_pair_analyze,
       true},
      {"mastering-stereo-analyze", "Run a stereo mastering analysis", cmd_mastering_stereo_analyze,
       true},
      {"mastering-processors", "List named mastering processors", cmd_mastering_processors, false},
      {"mastering-pair-processors", "List two-input mastering processors",
       cmd_mastering_pair_processors, false},
      {"mastering-pair-analyses", "List two-input mastering analyses", cmd_mastering_pair_analyses,
       false},
      {"mastering-stereo-analyses", "List stereo mastering analyses", cmd_mastering_stereo_analyses,
       false},
#endif
#ifdef SONARE_WITH_MIXING
      {"mix", "Apply mixer strip processing", cmd_mix, true},
      {"mixing-presets", "List built-in mixer scene presets", cmd_mixing_presets, false},
      {"mixing-preset", "Print a built-in mixer scene preset JSON", cmd_mixing_preset, false},
#endif
      // Features
      {"mel", "Compute mel spectrogram", cmd_mel, true},
      {"chroma", "Compute chromagram", cmd_chroma, true},
      {"tonnetz", "Compute tonal centroid features", cmd_tonnetz, true},
      {"spectral", "Compute spectral features", cmd_spectral, true},
      {"pitch", "Track pitch (YIN/pYIN)", cmd_pitch, true},
      {"onset-env", "Compute onset strength envelope", cmd_onset_env, true},
      {"onset-envelope", "Compute onset strength envelope (full array)", cmd_onset_envelope, true},
      {"tempogram", "Compute onset tempogram", cmd_tempogram, true},
      {"fourier-tempogram", "Compute Fourier tempogram", cmd_fourier_tempogram, true},
      {"tempogram-ratio", "Compute tempogram ratio features", cmd_tempogram_ratio, true},
      {"plp", "Compute predominant local pulse", cmd_plp, true},
      {"nnls-chroma", "Compute NNLS chromagram", cmd_nnls_chroma, true},
      {"cqt", "Compute Constant-Q Transform", cmd_cqt, true},
      {"vqt", "Compute Variable-Q Transform", cmd_vqt, true},
      {"mel-to-audio", "Reconstruct audio from a mel spectrogram", cmd_mel_to_audio, true},
      {"mfcc-to-audio", "Reconstruct audio from MFCC", cmd_mfcc_to_audio, true},
      // Utility
      {"frames-to-samples", "Convert frame index to sample index", cmd_frames_to_samples, false},
      {"samples-to-frames", "Convert sample index to frame index", cmd_samples_to_frames, false},
      {"power-to-db", "Convert power values to dB", cmd_power_to_db, false},
      {"amplitude-to-db", "Convert amplitude values to dB", cmd_amplitude_to_db, false},
      {"db-to-power", "Convert dB values to power", cmd_db_to_power, false},
      {"db-to-amplitude", "Convert dB values to amplitude", cmd_db_to_amplitude, false},
      {"frame-signal", "Frame a value sequence", cmd_frame_signal, false},
      {"pad-center", "Pad a value sequence symmetrically", cmd_pad_center, false},
      {"fix-length", "Pad or trim a value sequence", cmd_fix_length, false},
      {"fix-frames", "Pad and clamp frame indices", cmd_fix_frames, false},
      {"peak-pick", "Pick local peaks from a value sequence", cmd_peak_pick, false},
      {"vector-normalize", "Normalize a value sequence", cmd_vector_normalize, false},
      {"pcen", "Apply per-channel energy normalization", cmd_pcen, false},
      {"info", "Show audio file information", cmd_info, true},
#ifdef SONARE_WITH_ARRANGEMENT
      {"project", "Headless arrangement / DAW project (abi/new/validate/compile/bounce/...)",
       cmd_project, false},
#endif
  };
  return commands;
}

const CommandInfo* find_command(const std::string& name) {
  for (const auto& cmd : get_commands()) {
    if (cmd.name == name) return &cmd;
  }
  return nullptr;
}

// ============================================================================
// Usage
// ============================================================================

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " <command> [options] <audio_file> [-o output]\n\n";

  std::cerr << "ANALYSIS COMMANDS:\n";
  for (const auto& cmd : get_commands()) {
    if (cmd.name == "pitch-shift") std::cerr << "\nPROCESSING COMMANDS:\n";
    if (cmd.name == "mel") std::cerr << "\nFEATURE COMMANDS:\n";
    if (cmd.name == "info") std::cerr << "\nUTILITY COMMANDS:\n";
    fprintf(stderr, "  %-14s %s\n", cmd.name.c_str(), cmd.description.c_str());
  }
  std::cerr << "  version        Show library version\n";
  std::cerr << "  system-info    Show system and parallel configuration\n";

  std::cerr << "\nGLOBAL OPTIONS:\n"
            << "  --json             Output results in JSON format\n"
            << "  --quiet, -q        Suppress progress output\n"
            << "  --help, -h         Show help\n"
            << "  -o, --output       Output file path\n"
            << "  --n-fft <int>      FFT size (default: 2048)\n"
            << "  --hop-length <int> Hop length (default: 512)\n"
            << "\nExamples:\n"
            << "  " << prog << " analyze music.mp3\n"
            << "  " << prog << " bpm music.wav --json\n"
            << "  " << prog << " pitch-shift --semitones 3 input.wav -o output.wav\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  CliArgs args = ArgParser::parse(argc, argv);

  if (args.help) {
    print_usage(argv[0]);
    return 0;
  }

  if (args.command.empty()) {
    std::cerr << color::red << "Error: No command specified" << color::reset << "\n\n";
    print_usage(argv[0]);
    return 1;
  }

  // Version command (no audio needed)
  if (args.command == "version") {
    return cmd_version(args);
  }

  if (args.command == "system-info") {
    return cmd_system_info(args);
  }

  // Find command
  const CommandInfo* cmd = find_command(args.command);
  if (!cmd) {
    std::cerr << color::red << "Error: Unknown command '" << args.command << "'" << color::reset
              << "\n\n";
    print_usage(argv[0]);
    return 1;
  }

  // Check for audio file
  if (cmd->requires_audio && args.input_file.empty()) {
    std::cerr << color::red << "Error: Missing audio file" << color::reset << "\n\n";
    print_usage(argv[0]);
    return 1;
  }

  try {
    if (!cmd->requires_audio) {
      return cmd->handler(args, Audio{});
    }

    if (!args.quiet && !args.json_output) {
      std::cerr << color::blue << "Loading " << basename(args.input_file) << "..." << color::reset
                << std::flush;
    }

    auto [samples, sample_rate] = load_audio(args.input_file);
    if (samples.empty()) {
      std::cerr << "\n" << color::red << "Error: Failed to load audio file" << color::reset << "\n";
      return 1;
    }

    Audio audio = Audio::from_vector(std::move(samples), sample_rate);

    if (!args.quiet && !args.json_output) {
      std::cerr << "\r" << color::green << "Loaded " << std::fixed << std::setprecision(1)
                << audio.duration() << "s @ " << sample_rate << "Hz" << color::reset
                << "                    \n";
    }

    return cmd->handler(args, audio);

  } catch (const std::exception& e) {
    std::cerr << "\n" << color::red << "Error: " << e.what() << color::reset << "\n";
    return 1;
  }
}
