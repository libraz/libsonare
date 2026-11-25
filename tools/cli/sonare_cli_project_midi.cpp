#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "sonare_cli_project.h"

#ifdef SONARE_WITH_ARRANGEMENT

namespace {

// The transcription default for the re-attack split; the C ABI reads NaN as "no split".
constexpr float kDefaultReattackRatio = 2.0f;

using ProjectMidiImportFn = SonareError (*)(SonareProject*, const uint8_t*, size_t, uint32_t*);

struct ProjectMidiImportSpec {
  const char* input_kind;
  const char* import_error_context;
  const char* success_format;
  ProjectMidiImportFn import;
};

int cmd_project_import_midi(const CliArgs& args, const ProjectMidiImportSpec& spec) {
  const std::string input_path = project_input_path(args);
  std::vector<uint8_t> bytes;
  if (!read_binary_file(input_path, &bytes)) {
    std::cerr << color::red << "Error: cannot open " << spec.input_kind << " file: " << input_path
              << color::reset << "\n";
    // A user-named input file that cannot be opened keeps the file-not-found
    // class here for the same reason load_project_from_args does: a caller that
    // branches on "fetch the input again" versus "the arguments are wrong" must
    // get the same answer from whichever subcommand read the file.
    return project_exit_code(SONARE_ERROR_FILE_NOT_FOUND);
  }
  ProjectHandle handle;
  SonareError err = sonare_project_create(&handle.ptr);
  if (err != SONARE_OK) {
    project_report_error("create project", err);
    return project_exit_code(err);
  }
  uint32_t first_clip = 0;
  err = spec.import(handle.ptr, bytes.data(), bytes.size(), &first_clip);
  if (err != SONARE_OK) {
    project_report_error(spec.import_error_context, err);
    return project_exit_code(err);
  }
  char* json = nullptr;
  size_t len = 0;
  err = sonare_project_serialize(handle.ptr, &json, &len);
  if (err != SONARE_OK) {
    project_report_error("serialize project", err);
    return project_exit_code(err);
  }
  const bool ok = write_binary_file(args.output_file, reinterpret_cast<const uint8_t*>(json), len);
  sonare_free_string(json);
  if (!ok) {
    return report_output_write_failure(args.output_file);
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("first_clip_id", static_cast<int>(first_clip))
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Imported " << spec.success_format << " to " << args.output_file
              << color::reset << "\n";
  }
  return 0;
}

}  // namespace

// `transcribe in.wav -o out.mid` — audio to a Standard MIDI File. The notes
// land on a PROJECT's tempo map, which is why an explicit --tempo-bpm is
// installed as that map rather than handed to the transcriber: the clip entry
// takes no tempo at all. Omitting it detects one from the take.
int cmd_transcribe(const CliArgs& args, const Audio& audio) {
  const bool tempo_given = args.has("tempo-bpm");

  ProjectHandle handle;
  SonareError err = sonare_project_create(&handle.ptr);
  if (err != SONARE_OK) {
    project_report_error("create project", err);
    return project_exit_code(err);
  }

  double tempo_bpm = 0.0;
  SonareProjectAutoTempoResult detected{};
  if (tempo_given) {
    tempo_bpm = args.get_double("tempo-bpm", 0.0);
    SonareProjectTempoSegment segment{};
    segment.start_ppq = 0.0;
    segment.bpm = tempo_bpm;
    err = sonare_project_set_tempo_segments(handle.ptr, &segment, 1);
    if (err != SONARE_OK) {
      project_report_error("set tempo", err);
      return project_exit_code(err);
    }
  } else {
    SonareProjectTempoOptions options;
    sonare_project_tempo_options_init(&options);
    err = sonare_project_auto_tempo(handle.ptr, audio.data(), audio.size(), audio.sample_rate(),
                                    &options, 0, 0, &detected);
    if (err != SONARE_OK) {
      project_report_error("detect tempo", err);
      return project_exit_code(err);
    }
    tempo_bpm = detected.bpm;
  }

  // PPQ coordinates are beats, so the take's length in beats is what the clip
  // has to span for its last note-off to fall inside it.
  const double duration =
      audio.sample_rate() > 0 ? static_cast<double>(audio.size()) / audio.sample_rate() : 0.0;
  const double length_ppq = std::max(1.0, std::ceil(duration * tempo_bpm / 60.0));
  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  err = sonare_project_add_midi_clip(handle.ptr, 0.0, length_ppq, &track_id, &clip_id);
  if (err != SONARE_OK) {
    project_report_error("add MIDI clip", err);
    return project_exit_code(err);
  }

  // An absent option keeps what sonare_transcribe_config_init wrote, so this CLI never restates
  // a core default; a given value, 0 included, is passed through for the core to check.
  SonareTranscribeConfig config;
  sonare_transcribe_config_init(&config);
  config.polyphonic = args.has("polyphonic") ? 1 : 0;
  const auto read_float = [&args](const char* name, float& field) {
    if (args.has(name)) field = args.get_float(name, field);
  };
  read_float("reference-hz", config.reference_hz);
  read_float("f0-min-hz", config.f0_min_hz);
  read_float("f0-max-hz", config.f0_max_hz);
  read_float("min-note-ms", config.min_note_ms);
  read_float("segmentation-threshold-cents", config.segmentation_threshold_cents);
  read_float("velocity-floor-db", config.velocity_floor_db);
  read_float("min-frame-peak-ratio", config.min_frame_peak_ratio);
  read_float("min-ridge-peak-ratio", config.min_ridge_peak_ratio);
  if (args.has("fixed-velocity")) config.fixed_velocity = args.get_int("fixed-velocity", 0);
  config.channel = args.get_int("channel", config.channel);
  if (args.has("max-polyphony")) config.max_polyphony = args.get_int("max-polyphony", 0);
  if (args.has("reattack-ratio")) {
    // The CLI spells "no split" as 0, which the C ABI spells as NaN.
    const float ratio = args.get_float("reattack-ratio", 0.0f);
    config.reattack_ratio = ratio == 0.0f ? std::numeric_limits<float>::quiet_NaN() : ratio;
  } else if (config.polyphonic != 0) {
    // The C ABI does not split without a ratio, so the polyphonic transcription default is given.
    config.reattack_ratio = kDefaultReattackRatio;
  }

  // Both paths read the division; giving it with --min-note-ms is refused by the
  // core, which names the field.
  if (args.has("min-note-division")) {
    config.min_note_division = args.get_int("min-note-division", 0);
  }

  // Absent leaves the C ABI's 0 (segments); the core refuses singing together
  // with --polyphonic, naming the field.
  if (args.has("monophonic-model")) {
    const std::string model = args.get_string("monophonic-model");
    if (model == "segments") {
      config.monophonic_model = 0;
    } else if (model == "singing") {
      config.monophonic_model = 1;
    } else {
      throw std::invalid_argument("argument --monophonic-model: must be one of segments, singing");
    }
  }

  size_t note_count = 0;
  err = sonare_project_transcribe_to_clip(handle.ptr, clip_id, audio.data(), audio.size(),
                                          audio.sample_rate(), &config, &note_count);
  if (err != SONARE_OK) {
    project_report_error("transcribe", err);
    return project_exit_code(err);
  }

  uint8_t* bytes = nullptr;
  size_t len = 0;
  err = sonare_project_export_smf(handle.ptr, &bytes, &len);
  if (err != SONARE_OK) {
    project_report_error("export SMF", err);
    return project_exit_code(err);
  }
  const bool ok = write_binary_file(args.output_file, bytes, len);
  sonare_free_bytes(bytes);
  if (!ok) {
    return report_output_write_failure(args.output_file);
  }

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("output", args.output_file)
        .kv("note_count", note_count)
        .kv("tempo_bpm", tempo_bpm)
        .kv("bytes", len);
    if (!tempo_given) {
      json.kv("tempo_confidence", detected.bpm_confidence)
          .key("time_signature")
          .begin_object()
          .kv("numerator", detected.time_signature.numerator)
          .kv("denominator", detected.time_signature.denominator)
          .kv("confidence", detected.time_signature.confidence)
          .end_object();
    }
    json.end_object().print();
  } else if (!args.quiet) {
    std::cout << color::green << "Transcribed " << note_count << " notes at ";
    if (tempo_bpm < 0.005) {
      std::cout << std::scientific << std::setprecision(2) << tempo_bpm;
    } else {
      std::cout << std::fixed << std::setprecision(2) << tempo_bpm;
    }
    std::cout << std::defaultfloat << " BPM to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

// `project export-smf in.json -o out.mid` — export the project's tempo map
// + MIDI clips to a Standard MIDI File.
int cmd_project_export_smf(const CliArgs& args) {
  ProjectHandle handle;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, nullptr, &load_error))
    return project_exit_code(load_error);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  SonareError err = sonare_project_export_smf(handle.ptr, &bytes, &len);
  if (err != SONARE_OK) {
    project_report_error("export SMF", err);
    return project_exit_code(err);
  }
  const bool ok = write_binary_file(args.output_file, bytes, len);
  sonare_free_bytes(bytes);
  if (!ok) {
    return report_output_write_failure(args.output_file);
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Exported SMF (" << len << " bytes) to " << args.output_file
              << color::reset << "\n";
  }
  return 0;
}

// `project export-midi2 in.json -o out.midi2` — export the project's tempo
// map + MIDI clips to a MIDI 2.0 Clip File.
int cmd_project_export_midi2(const CliArgs& args) {
  ProjectHandle handle;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, nullptr, &load_error))
    return project_exit_code(load_error);

  uint8_t* bytes = nullptr;
  size_t len = 0;
  SonareError err = sonare_project_export_clip_file(handle.ptr, &bytes, &len);
  if (err != SONARE_OK) {
    project_report_error("export MIDI2 Clip File", err);
    return project_exit_code(err);
  }
  const bool ok = write_binary_file(args.output_file, bytes, len);
  sonare_free_bytes(bytes);
  if (!ok) {
    return report_output_write_failure(args.output_file);
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Exported MIDI2 Clip File (" << len << " bytes) to "
              << args.output_file << color::reset << "\n";
  }
  return 0;
}

// `project import-smf in.mid -o out.json` — import an SMF into a new
// project and serialize it to JSON.
int cmd_project_import_smf(const CliArgs& args) {
  return cmd_project_import_midi(args, {"SMF", "import SMF", "SMF", sonare_project_import_smf});
}

// `project import-midi2 in.midi2 -o out.json` — import a MIDI 2.0 Clip
// File into a new project and serialize it to JSON.
int cmd_project_import_midi2(const CliArgs& args) {
  return cmd_project_import_midi(args, {"MIDI2", "import MIDI2 Clip File", "MIDI2 Clip File",
                                        sonare_project_import_clip_file});
}
#endif  // SONARE_WITH_ARRANGEMENT
