#include "sonare_cli.h"

#ifdef SONARE_WITH_ARRANGEMENT

struct ProjectHandle {
  SonareProject* ptr = nullptr;
  ~ProjectHandle() { sonare_project_destroy(ptr); }
  ProjectHandle() = default;
  ProjectHandle(const ProjectHandle&) = delete;
  ProjectHandle& operator=(const ProjectHandle&) = delete;
};

std::string project_error_string(SonareError err) {
  const char* msg = sonare_error_message(err);
  return msg != nullptr ? std::string(msg) : ("error " + std::to_string(static_cast<int>(err)));
}

void project_report_error(const std::string& what, SonareError err) {
  std::cerr << color::red << "Error: " << what << ": " << project_error_string(err) << color::reset
            << "\n";
}

// Reads an arbitrary file into a byte buffer (binary-safe). The CLI owns file
// I/O; the core / C ABI exchange in-memory buffers only.
bool read_binary_file(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

bool write_binary_file(const std::string& path, const uint8_t* data, size_t len) {
  std::ofstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  if (len > 0) file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
  return file.good();
}

// Loads a project JSON file from --in (or the second positional) into a fresh
// handle. Returns true on success. On failure prints an error and leaves the
// handle empty.
bool load_project_from_args(const CliArgs& args, ProjectHandle* handle) {
  const std::string in_path =
      args.has("in") ? args.get_string("in") : args.get_string("project", args.input_file);
  if (in_path.empty()) {
    std::cerr << color::red << "Error: missing project JSON (use --in <project.json>)"
              << color::reset << "\n";
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!read_binary_file(in_path, &bytes)) {
    std::cerr << color::red << "Error: cannot open project file: " << in_path << color::reset
              << "\n";
    return false;
  }
  char* diag = nullptr;
  SonareError err = sonare_project_deserialize(reinterpret_cast<const char*>(bytes.data()),
                                               bytes.size(), &handle->ptr, &diag);
  if (err != SONARE_OK) {
    std::cerr << color::red << "Error: failed to parse project JSON: " << project_error_string(err);
    if (diag != nullptr) std::cerr << " (" << diag << ")";
    std::cerr << color::reset << "\n";
    sonare_free_string(diag);
    return false;
  }
  sonare_free_string(diag);
  return true;
}

// `project abi` — print the runtime project ABI version (0 when the arrangement
// subsystem was compiled out).
int cmd_project_abi(const CliArgs& args) {
  const uint32_t version = sonare_project_abi_version();
  if (args.json_output) {
    JsonBuilder().begin_object().kv("abi_version", static_cast<int>(version)).end_object().print();
  } else {
    std::cout << version << "\n";
  }
  return 0;
}

// `project new -o out.json` — create an empty project and serialize it to disk.
int cmd_project_new(const CliArgs& args) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: project new requires output file (-o out.json)"
              << color::reset << "\n";
    return 1;
  }
  ProjectHandle handle;
  SonareError err = sonare_project_create(&handle.ptr);
  if (err != SONARE_OK) {
    project_report_error("create project", err);
    return 1;
  }
  const double sample_rate = args.get_float("sample-rate", 0.0f);
  if (sample_rate > 0.0) {
    err = sonare_project_set_sample_rate(handle.ptr, sample_rate);
    if (err != SONARE_OK) {
      project_report_error("set sample rate", err);
      return 1;
    }
  }
  char* json = nullptr;
  size_t len = 0;
  err = sonare_project_serialize(handle.ptr, &json, &len);
  if (err != SONARE_OK) {
    project_report_error("serialize project", err);
    return 1;
  }
  const bool ok = write_binary_file(args.output_file, reinterpret_cast<const uint8_t*>(json), len);
  sonare_free_string(json);
  if (!ok) {
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Wrote empty project to " << args.output_file << color::reset
              << "\n";
  }
  return 0;
}

// `project validate --in in.json` — round-trip a project JSON through the
// deserializer + serializer; with -o, writes the canonical JSON back out.
int cmd_project_validate(const CliArgs& args) {
  ProjectHandle handle;
  if (!load_project_from_args(args, &handle)) return 1;
  char* json = nullptr;
  size_t len = 0;
  SonareError err = sonare_project_serialize(handle.ptr, &json, &len);
  if (err != SONARE_OK) {
    project_report_error("serialize project", err);
    return 1;
  }
  if (!args.output_file.empty()) {
    const bool ok =
        write_binary_file(args.output_file, reinterpret_cast<const uint8_t*>(json), len);
    if (!ok) {
      sonare_free_string(json);
      std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
      return 1;
    }
  }
  sonare_free_string(json);
  if (args.json_output) {
    JsonBuilder().begin_object().kv("valid", true).kv("bytes", len).end_object().print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Project JSON is valid (" << len << " bytes canonical)"
              << color::reset << "\n";
  }
  return 0;
}

// `project compile --in in.json` — compile the project into a renderable
// timeline and surface diagnostics.
int cmd_project_compile(const CliArgs& args) {
  ProjectHandle handle;
  if (!load_project_from_args(args, &handle)) return 1;
  SonareProjectCompileResult result{};
  SonareError err = sonare_project_compile(handle.ptr, &result);
  if (err != SONARE_OK) {
    sonare_project_free_compile_result(&result);
    project_report_error("compile project", err);
    return 1;
  }
  const bool has_timeline = result.has_timeline != 0;
  if (args.json_output) {
    JsonBuilder builder;
    builder.begin_object()
        .kv("has_timeline", has_timeline)
        .kv("diagnostic_count", result.diagnostic_count)
        .key("diagnostics")
        .begin_array();
    for (size_t i = 0; i < result.diagnostic_count; ++i) {
      builder.begin_object()
          .kv("code", static_cast<int>(result.diagnostics[i].code))
          .kv("severity", static_cast<int>(result.diagnostics[i].severity))
          .kv("target_id", static_cast<int>(result.diagnostics[i].target_id))
          .end_object();
    }
    builder.end_array();
    if (result.messages != nullptr) builder.kv("messages", std::string(result.messages));
    builder.end_object().print();
  } else if (!args.quiet) {
    std::cerr << (has_timeline ? color::green : color::yellow)
              << (has_timeline ? "Compiled (renderable timeline)" : "Compiled with errors")
              << color::reset << ", " << result.diagnostic_count << " diagnostic(s)\n";
    if (result.messages != nullptr && result.messages[0] != '\0') {
      std::cerr << result.messages << "\n";
    }
  }
  sonare_project_free_compile_result(&result);
  return has_timeline ? 0 : 1;
}

// `project bounce --in in.json -o out.wav` — compile + render the project
// offline to an interleaved WAV file.
int cmd_project_bounce(const CliArgs& args) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: project bounce requires output file (-o out.wav)"
              << color::reset << "\n";
    return 1;
  }
  ProjectHandle handle;
  if (!load_project_from_args(args, &handle)) return 1;

  SonareProjectBounceOptions options{};
  options.total_frames = static_cast<int64_t>(args.get_int("frames", 0));
  options.block_size = args.get_int("block-size", 0);
  options.num_channels = args.get_int("channels", 0);
  options.sample_rate = args.get_int("sample-rate", 0);
  options.instrument_latency_samples = args.get_int("instrument-latency", 0);

  float* interleaved = nullptr;
  size_t total = 0;
  SonareError err = sonare_project_bounce(handle.ptr, &options, &interleaved, &total);
  if (err != SONARE_OK) {
    project_report_error("bounce project", err);
    return 1;
  }
  const int channels = options.num_channels > 0 ? options.num_channels : 2;
  const int sample_rate = options.sample_rate > 0 ? options.sample_rate : 44100;
  const size_t frames = channels > 0 ? total / static_cast<size_t>(channels) : total;
  // The CLI WAV writer is mono; downmix the interleaved render to mono (matching
  // the rest of the CLI, which is mono-centric). The full multichannel buffer is
  // available via the C ABI / Node / Python bindings for callers that need it.
  std::vector<float> mono(frames, 0.0f);
  for (size_t f = 0; f < frames; ++f) {
    float sum = 0.0f;
    for (int ch = 0; ch < channels; ++ch) {
      sum += interleaved[f * static_cast<size_t>(channels) + static_cast<size_t>(ch)];
    }
    mono[f] = channels > 0 ? sum / static_cast<float>(channels) : 0.0f;
  }
  sonare_free_floats(interleaved);
  save_wav(args.output_file, mono, sample_rate);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("frames", frames)
        .kv("channels", channels)
        .kv("sample_rate", sample_rate)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Bounced " << frames << " frames (" << channels << " ch @ "
              << sample_rate << " Hz) to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

// `project export-smf --in in.json -o out.mid` — export the project's tempo map
// + MIDI clips to a Standard MIDI File.
int cmd_project_export_smf(const CliArgs& args) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: project export-smf requires output file (-o out.mid)"
              << color::reset << "\n";
    return 1;
  }
  ProjectHandle handle;
  if (!load_project_from_args(args, &handle)) return 1;

  uint8_t* bytes = nullptr;
  size_t len = 0;
  SonareError err = sonare_project_export_smf(handle.ptr, &bytes, &len);
  if (err != SONARE_OK) {
    project_report_error("export SMF", err);
    return 1;
  }
  const bool ok = write_binary_file(args.output_file, bytes, len);
  sonare_free_bytes(bytes);
  if (!ok) {
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Exported SMF (" << len << " bytes) to " << args.output_file
              << color::reset << "\n";
  }
  return 0;
}

// `project export-midi2 --in in.json -o out.midi2` — export the project's tempo
// map + MIDI clips to a MIDI 2.0 Clip File.
int cmd_project_export_midi2(const CliArgs& args) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: project export-midi2 requires output file (-o out.midi2)"
              << color::reset << "\n";
    return 1;
  }
  ProjectHandle handle;
  if (!load_project_from_args(args, &handle)) return 1;

  uint8_t* bytes = nullptr;
  size_t len = 0;
  SonareError err = sonare_project_export_clip_file(handle.ptr, &bytes, &len);
  if (err != SONARE_OK) {
    project_report_error("export MIDI2 Clip File", err);
    return 1;
  }
  const bool ok = write_binary_file(args.output_file, bytes, len);
  sonare_free_bytes(bytes);
  if (!ok) {
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
  }
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("bytes", len)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Exported MIDI2 Clip File (" << len << " bytes) to "
              << args.output_file << color::reset << "\n";
  }
  return 0;
}

// `project import-smf --smf in.mid -o out.json` — import an SMF into a new
// project and serialize it to JSON.
int cmd_project_import_smf(const CliArgs& args) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: project import-smf requires output file (-o out.json)"
              << color::reset << "\n";
    return 1;
  }
  const std::string smf_path = args.get_string("smf");
  if (smf_path.empty()) {
    std::cerr << color::red << "Error: missing SMF input (use --smf <file.mid>)" << color::reset
              << "\n";
    return 1;
  }
  std::vector<uint8_t> smf;
  if (!read_binary_file(smf_path, &smf)) {
    std::cerr << color::red << "Error: cannot open SMF file: " << smf_path << color::reset << "\n";
    return 1;
  }
  ProjectHandle handle;
  SonareError err = sonare_project_create(&handle.ptr);
  if (err != SONARE_OK) {
    project_report_error("create project", err);
    return 1;
  }
  uint32_t first_clip = 0;
  err = sonare_project_import_smf(handle.ptr, smf.data(), smf.size(), &first_clip);
  if (err != SONARE_OK) {
    project_report_error("import SMF", err);
    return 1;
  }
  char* json = nullptr;
  size_t len = 0;
  err = sonare_project_serialize(handle.ptr, &json, &len);
  if (err != SONARE_OK) {
    project_report_error("serialize project", err);
    return 1;
  }
  const bool ok = write_binary_file(args.output_file, reinterpret_cast<const uint8_t*>(json), len);
  sonare_free_string(json);
  if (!ok) {
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
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
    std::cerr << color::green << "Imported SMF to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

// `project import-midi2 --midi2 in.midi2 -o out.json` — import a MIDI 2.0 Clip
// File into a new project and serialize it to JSON.
int cmd_project_import_midi2(const CliArgs& args) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: project import-midi2 requires output file (-o out.json)"
              << color::reset << "\n";
    return 1;
  }
  const std::string midi2_path = args.get_string("midi2");
  if (midi2_path.empty()) {
    std::cerr << color::red << "Error: missing MIDI2 input (use --midi2 <file.midi2>)"
              << color::reset << "\n";
    return 1;
  }
  std::vector<uint8_t> midi2;
  if (!read_binary_file(midi2_path, &midi2)) {
    std::cerr << color::red << "Error: cannot open MIDI2 file: " << midi2_path << color::reset
              << "\n";
    return 1;
  }
  ProjectHandle handle;
  SonareError err = sonare_project_create(&handle.ptr);
  if (err != SONARE_OK) {
    project_report_error("create project", err);
    return 1;
  }
  uint32_t first_clip = 0;
  err = sonare_project_import_clip_file(handle.ptr, midi2.data(), midi2.size(), &first_clip);
  if (err != SONARE_OK) {
    project_report_error("import MIDI2 Clip File", err);
    return 1;
  }
  char* json = nullptr;
  size_t len = 0;
  err = sonare_project_serialize(handle.ptr, &json, &len);
  if (err != SONARE_OK) {
    project_report_error("serialize project", err);
    return 1;
  }
  const bool ok = write_binary_file(args.output_file, reinterpret_cast<const uint8_t*>(json), len);
  sonare_free_string(json);
  if (!ok) {
    std::cerr << color::red << "Error: cannot write " << args.output_file << color::reset << "\n";
    return 1;
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
    std::cerr << color::green << "Imported MIDI2 Clip File to " << args.output_file << color::reset
              << "\n";
  }
  return 0;
}

void print_project_usage(std::ostream& out) {
  out << "Usage: sonare project <subcommand> [options]\n\n"
      << "PROJECT SUBCOMMANDS (headless arrangement / DAW):\n"
      << "  abi                  Print the project C ABI version\n"
      << "  new                  Create an empty project (-o out.json)\n"
      << "  validate             Round-trip / validate a project (--in in.json [-o out.json])\n"
      << "  compile              Compile a project + report diagnostics (--in in.json)\n"
      << "  bounce               Render a project offline to WAV (--in in.json -o out.wav)\n"
      << "  export-smf           Export tempo map + MIDI clips to SMF (--in in.json -o out.mid)\n"
      << "  import-smf           Import an SMF into a new project (--smf in.mid -o out.json)\n"
      << "  export-midi2         Export tempo map + MIDI clips to MIDI2 Clip File (--in in.json -o "
         "out.midi2)\n"
      << "  import-midi2         Import MIDI2 Clip File into a new project (--midi2 in.midi2 -o "
         "out.json)\n"
      << "\nOPTIONS:\n"
      << "  --in <file>          Input project JSON\n"
      << "  --smf <file>         Input Standard MIDI File (import-smf)\n"
      << "  --midi2 <file>       Input MIDI 2.0 Clip File (import-midi2)\n"
      << "  -o, --output <file>  Output file\n"
      << "  --sample-rate <hz>   Sample rate (new / bounce)\n"
      << "  --frames <n>         Bounce length in frames\n"
      << "  --channels <n>       Bounce channel count (default 2)\n"
      << "  --json               Emit JSON results\n";
}

// `project <subcommand> ...` — dispatches the headless-project subcommands. The
// subcommand lands in the second positional (args.input_file).
int cmd_project(const CliArgs& args, const Audio&) {
  const std::string& sub = args.input_file;
  if (args.help || sub.empty() || sub == "help") {
    print_project_usage(sub.empty() && !args.help ? std::cerr : std::cout);
    return (sub.empty() && !args.help) ? 1 : 0;
  }
  if (sub == "abi") return cmd_project_abi(args);
  if (sub == "new") return cmd_project_new(args);
  if (sub == "validate") return cmd_project_validate(args);
  if (sub == "compile") return cmd_project_compile(args);
  if (sub == "bounce") return cmd_project_bounce(args);
  if (sub == "export-smf") return cmd_project_export_smf(args);
  if (sub == "import-smf") return cmd_project_import_smf(args);
  if (sub == "export-midi2") return cmd_project_export_midi2(args);
  if (sub == "import-midi2") return cmd_project_import_midi2(args);
  std::cerr << color::red << "Error: unknown project subcommand '" << sub << "'" << color::reset
            << "\n\n";
  print_project_usage(std::cerr);
  return 1;
}
#endif  // SONARE_WITH_ARRANGEMENT
