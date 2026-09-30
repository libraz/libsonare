#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sonare_cli_project.h"

#ifdef SONARE_WITH_ARRANGEMENT

// A project source's serialized `kind`: 0 audio, 1 MIDI (SourceKind). Only an
// audio source carries a `uri`, so the kind decides which entries are
// addressable by --audio at all.
constexpr int kProjectSourceKindAudio = 0;

// The only URI scheme --resolve-audio opens. The core never opens a URI itself,
// so resolving one is this front-end's own file I/O, and it has no business
// fetching over a network to feed a render.
constexpr char kFileUriPrefix[] = "file://";

// Percent-decodes a URI path. A malformed escape is kept verbatim, so a path
// containing a bare '%' stays openable instead of being mangled.
std::string percent_decode(const std::string& text) {
  const auto hex_value = [](char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const int high = (text[i] == '%' && i + 2 < text.size()) ? hex_value(text[i + 1]) : -1;
    const int low = high >= 0 ? hex_value(text[i + 2]) : -1;
    if (low >= 0) {
      out.push_back(static_cast<char>((high << 4) | low));
      i += 2;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

// Resolves a file:// URI to a local path. Returns false for any other scheme
// and for an authority naming another host, neither of which this front-end can
// open. The POSIX decoding is percent-decoding alone, which is what the other
// front-end's url2pathname does on the same platforms.
bool path_from_file_uri(const std::string& uri, std::string* out) {
  const std::string prefix(kFileUriPrefix);
  if (uri.compare(0, prefix.size(), prefix) != 0) return false;
  const std::string rest = uri.substr(prefix.size());
  const size_t slash = rest.find('/');
  if (slash == std::string::npos) return false;
  const std::string authority = rest.substr(0, slash);
  if (!authority.empty() && authority != "localhost") return false;
  *out = percent_decode(rest.substr(slash));
  return true;
}

// Maps each audio source id in the document to the URI it references.
//
// Read from the document rather than from the loaded handle: the flat source
// descriptor carries the URI in a fixed-width field, so anything longer arrives
// truncated, and a truncated URI is one --resolve-audio would then try to open.
// The document is the only place the whole string survives.
std::map<uint32_t, std::string> project_audio_source_uris(const std::string& document) {
  std::map<uint32_t, std::string> uris;
  const auto root = sonare::util::json::parse(document);
  const auto* sources = root.find("sources");
  if (sources == nullptr || !sources->is_array()) return uris;
  for (const auto& source : sources->as_array()) {
    const auto* kind = source.find("kind");
    const auto* id = source.find("id");
    const auto* uri = source.find("uri");
    if (kind == nullptr || !kind->is_number() || kind->as_int() != kProjectSourceKindAudio)
      continue;
    if (id == nullptr || !id->is_number()) continue;
    uris.emplace(static_cast<uint32_t>(id->as_number()),
                 uri != nullptr && uri->is_string() ? uri->as_string() : std::string());
  }
  return uris;
}

// What `project align-takes` has to read straight out of the document because
// the C ABI exposes counts but no per-clip and no per-warp-map getter. Read the
// same way project_audio_source_uris reads the URIs, and in one pass, since the
// document may be as large as kMaxProjectOrMidiBytes.
struct ProjectAlignDocumentFacts {
  // Every source in the document, either kind, so a reference naming a MIDI
  // source is refused differently from one naming nothing.
  std::set<uint32_t> source_ids;
  // Clip ids per source, ascending, which is the order the clips are visited in.
  std::map<uint32_t, std::vector<uint32_t>> clip_ids_by_source;
  // Highest warp-map id already in the document; the first map written takes the
  // next id after it, so nothing already present is reused or renumbered.
  uint32_t max_warp_map_id = 0;
};

ProjectAlignDocumentFacts project_align_document_facts(const std::string& document) {
  ProjectAlignDocumentFacts facts;
  const auto root = sonare::util::json::parse(document);
  if (const auto* sources = root.find("sources"); sources != nullptr && sources->is_array()) {
    for (const auto& source : sources->as_array()) {
      const auto* id = source.find("id");
      if (id != nullptr && id->is_number()) {
        facts.source_ids.insert(static_cast<uint32_t>(id->as_number()));
      }
    }
  }
  if (const auto* clips = root.find("clips"); clips != nullptr && clips->is_array()) {
    for (const auto& clip : clips->as_array()) {
      const auto* id = clip.find("id");
      const auto* source_id = clip.find("source_id");
      if (id == nullptr || !id->is_number()) continue;
      if (source_id == nullptr || !source_id->is_number()) continue;
      facts.clip_ids_by_source[static_cast<uint32_t>(source_id->as_number())].push_back(
          static_cast<uint32_t>(id->as_number()));
    }
  }
  for (auto& [source_id, clip_ids] : facts.clip_ids_by_source) {
    (void)source_id;
    std::sort(clip_ids.begin(), clip_ids.end());
  }
  if (const auto* maps = root.find("warp_maps"); maps != nullptr && maps->is_array()) {
    for (const auto& map : maps->as_array()) {
      const auto* id = map.find("id");
      if (id == nullptr || !id->is_number()) continue;
      facts.max_warp_map_id =
          std::max(facts.max_warp_map_id, static_cast<uint32_t>(id->as_number()));
    }
  }
  return facts;
}

// Every audio source the loaded document has no PCM for, in the order the C ABI
// reports them. @p err carries the first failed C call, so a read that could not
// happen is not reported as an empty list.
std::vector<uint32_t> project_unresolved_audio_source_ids(const SonareProject* project,
                                                          SonareError* err) {
  std::vector<uint32_t> ids;
  size_t count = 0;
  *err = sonare_project_unresolved_audio_source_count(project, &count);
  if (*err != SONARE_OK) return ids;
  for (size_t index = 0; index < count; ++index) {
    uint32_t source_id = 0;
    *err = sonare_project_unresolved_audio_source_id_by_index(project, index, &source_id);
    if (*err != SONARE_OK) return {};
    ids.push_back(source_id);
  }
  return ids;
}

// Splits one `--audio <source_id>=FILE` assignment at the FIRST '=', so a path
// containing one stays intact. A source is addressed by id alone: its URI is
// unique too, but accepting either spelling would put two addresses on one
// binding, and the id is what the unresolved-source report already names.
void parse_audio_binding(const std::string& assignment, uint32_t* out_source_id,
                         std::string* out_path) {
  const size_t separator = assignment.find('=');
  if (separator == 0 || separator == std::string::npos || separator + 1 >= assignment.size()) {
    throw std::invalid_argument("--audio expects <source_id>=FILE, got '" + assignment + "'");
  }
  const long long source_id = parse_int64_strict("audio", assignment.substr(0, separator));
  if (source_id <= 0 || source_id > static_cast<long long>(UINT32_MAX)) {
    throw std::invalid_argument("--audio source id out of range: " +
                                assignment.substr(0, separator));
  }
  *out_source_id = static_cast<uint32_t>(source_id);
  *out_path = assignment.substr(separator + 1);
}

// Decodes an audio file and registers its PCM against one audio source.
SonareError bind_source_audio(SonareProject* project, uint32_t source_id, const std::string& path) {
  auto [interleaved, sample_rate, channels] = load_audio_interleaved(path);
  if (channels <= 0 || interleaved.empty()) {
    throw std::invalid_argument("--audio " + std::to_string(source_id) + "=" + path +
                                " decoded no samples");
  }
  const int64_t frames = static_cast<int64_t>(interleaved.size() / static_cast<size_t>(channels));
  return sonare_project_set_source_audio(project, source_id, interleaved.data(), frames, channels,
                                         sample_rate);
}

// Supplies the PCM a bounce needs for the document's audio sources.
//
// Project JSON references audio by URI / storage handle only and the core never
// opens either, so a document whose clips reference audio has nothing to render
// from until a host binds samples. This front-end is that host: `--audio` binds
// one named source to a file and `--resolve-audio` opens the file:// URIs the
// document already carries. Anything still unresolved is refused naming every
// source and its URI, rather than reaching the render as a bare invalid-state
// error that says nothing about what is missing.
//
// Returns 0 when the render may proceed, and the exit code to report otherwise.
int resolve_bounce_audio_sources(const CliArgs& args, SonareProject* project) {
  const std::vector<std::string> assignments = args.get_string_list("audio");
  const bool resolve = args.has("resolve-audio");
  SonareError unresolved_error = SONARE_OK;
  const std::vector<uint32_t> initial =
      project_unresolved_audio_source_ids(project, &unresolved_error);
  if (unresolved_error != SONARE_OK) {
    project_report_error("read unresolved audio sources", unresolved_error);
    return project_exit_code(unresolved_error);
  }
  // A document with no audio sources at all -- every MIDI-only project -- has
  // nothing to resolve and nothing to report, so it never pays for the second
  // read the URI map costs.
  if (assignments.empty() && !resolve && initial.empty()) return 0;

  const std::string in_path = project_input_path(args);
  std::vector<uint8_t> document;
  if (!read_binary_file(in_path, &document)) {
    std::cerr << color::red << "Error: cannot open project file: " << in_path << color::reset
              << "\n";
    return project_exit_code(SONARE_ERROR_FILE_NOT_FOUND);
  }
  const std::map<uint32_t, std::string> uris =
      project_audio_source_uris(std::string(document.begin(), document.end()));

  for (const auto& assignment : assignments) {
    uint32_t source_id = 0;
    std::string path;
    parse_audio_binding(assignment, &source_id, &path);
    if (uris.count(source_id) == 0) {
      std::cerr << color::red << "Error: --audio " << assignment
                << ": the project has no audio source " << source_id << color::reset << "\n";
      return kExitInvalidParameter;
    }
    const SonareError err = bind_source_audio(project, source_id, path);
    if (err != SONARE_OK) {
      project_report_error("bind audio source " + std::to_string(source_id), err);
      return project_exit_code(err);
    }
  }
  if (args.has("resolve-audio")) {
    SonareError err = SONARE_OK;
    for (const uint32_t source_id : project_unresolved_audio_source_ids(project, &err)) {
      const auto found = uris.find(source_id);
      const std::string uri = found != uris.end() ? found->second : std::string();
      std::string path;
      if (!path_from_file_uri(uri, &path)) {
        std::cerr << color::red << "Error: source " << source_id << " (" << uri
                  << "): --resolve-audio opens file:// URIs only; pass --audio " << source_id
                  << "=FILE" << color::reset << "\n";
        return kExitInvalidParameter;
      }
      const SonareError bind_err = bind_source_audio(project, source_id, path);
      if (bind_err != SONARE_OK) {
        project_report_error("bind audio source " + std::to_string(source_id), bind_err);
        return project_exit_code(bind_err);
      }
    }
    if (err != SONARE_OK) {
      project_report_error("read unresolved audio sources", err);
      return project_exit_code(err);
    }
  }
  SonareError err = SONARE_OK;
  const std::vector<uint32_t> remaining = project_unresolved_audio_source_ids(project, &err);
  if (err != SONARE_OK) {
    project_report_error("read unresolved audio sources", err);
    return project_exit_code(err);
  }
  if (remaining.empty()) return 0;
  // One line per source, each carrying its own id, because the caller has to act
  // on every one of them: stopping at the first turns a document with four
  // missing takes into four runs.
  for (const uint32_t source_id : remaining) {
    const auto found = uris.find(source_id);
    std::cerr << color::red << "Error: source " << source_id << " ("
              << (found != uris.end() ? found->second : std::string())
              << ") has no audio; pass --audio " << source_id << "=FILE or --resolve-audio"
              << color::reset << "\n";
  }
  return kExitInvalidState;
}

// `project bounce --in in.json -o out.wav` — compile + render the project
// offline to an interleaved WAV file. With `--synth [preset]` MIDI tracks are
// rendered through the NativeSynth catalog. A named preset is a fixed patch;
// the bare flag follows GM bank/program changes and routes channel 10 through
// the GM drum-kit map. Without --synth MIDI tracks render silently.
//
// `binds_source_audio` is false for `midi-render`, which declares neither
// --audio nor --resolve-audio: it has no way to supply the PCM an unresolved
// source needs, and naming those options in its refusal would advertise a flag
// the command does not accept.
int project_bounce_impl(const CliArgs& args, bool use_synth, bool binds_source_audio = true) {
  SonareSynthInstrumentBinding synth_binding{};
  if (use_synth) {
    const std::string requested = args.get_string("synth");
    const bool auto_select_gm = requested.empty() || requested == "true";
    if (auto_select_gm) {
      // GM routing replaces the patch at every note-on, so a base preset named
      // here would assert thirty field values the caller never chose and that
      // nothing ever reads. The zero-initialized patch is the init patch (an
      // empty preset name), which is what the other front-end sends: with the
      // patch inert either spelling renders the same, and this one stays the
      // same if the routing is ever asked to yield.
      synth_binding.patch.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
    } else {
      const SonareError patch_error =
          sonare_synth_preset_patch(requested.c_str(), &synth_binding.patch);
      if (patch_error != SONARE_OK) {
        std::cerr << color::red << "Error: unknown synth preset '" << requested << "'"
                  << color::reset << "\n";
        return project_exit_code(patch_error);
      }
    }
    synth_binding.destination_id = 0;
    synth_binding.use_gm_programs = auto_select_gm ? 1 : 0;
  }

  ProjectHandle handle;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, nullptr, &load_error))
    return project_exit_code(load_error);

  if (binds_source_audio) {
    const int audio_exit = resolve_bounce_audio_sources(args, handle.ptr);
    if (audio_exit != 0) return audio_exit;
  }

  double project_sample_rate = 0.0;
  SonareError sr_err = sonare_project_get_sample_rate(handle.ptr, &project_sample_rate);
  if (sr_err != SONARE_OK) {
    project_report_error("read project sample rate", sr_err);
    return project_exit_code(sr_err);
  }

  SonareProjectBounceOptions options{};
  options.total_frames = static_cast<int64_t>(args.get_int("frames", 0));
  options.block_size = args.get_int("block-size", 0);
  options.num_channels = args.get_int("channels", 0);
  options.instrument_latency_samples = args.get_int("instrument-latency", 0);
  // CRITICAL: the WAV header MUST be tagged with the SAME sample rate the render
  // actually used, or the file plays back at the wrong pitch. Default to the
  // project's own rate (queried above via sonare_project_get_sample_rate) rather
  // than a hardcoded value, so a 44.1k/96k project bounces without --sample-rate
  // at all. An explicit --sample-rate is only accepted when it matches the
  // project's rate: the C ABI rejects a genuine mismatch with a generic
  // invalid-parameter error, so the check is duplicated here to name both rates.
  int render_sample_rate = static_cast<int>(std::lround(project_sample_rate));
  if (args.has("sample-rate")) {
    render_sample_rate = args.get_int("sample-rate", render_sample_rate);
    if (std::abs(static_cast<double>(render_sample_rate) - project_sample_rate) > 1e-6) {
      std::cerr << color::red << "Error: --sample-rate " << render_sample_rate
                << " does not match the project's sample rate (" << project_sample_rate
                << " Hz); project bounce renders at the project's own rate" << color::reset << "\n";
      return kExitInvalidParameter;
    }
    options.sample_rate = render_sample_rate;
  }
  // Left at 0 when the caller did not ask for a rate, which is what the C ABI
  // reads as "the project's own". Pinning the rounded rate unconditionally made
  // a project whose rate is not an integer fail the ABI's own equality check
  // against the full-precision value the int cannot carry. The header below
  // still reports the nearest integer to the rate the engine rendered at, which
  // is the closest a RIFF header can come to a fractional rate.

  float* interleaved = nullptr;
  size_t total = 0;
  SonareError err = use_synth ? sonare_project_bounce_with_synth_instruments(
                                    handle.ptr, &options, &synth_binding, 1, &interleaved, &total)
                              : sonare_project_bounce(handle.ptr, &options, &interleaved, &total);
  if (err != SONARE_OK) {
    project_report_error("bounce project", err);
    return project_exit_code(err);
  }
  const int channels = options.num_channels > 0 ? options.num_channels : 2;
  // The WAV header sample rate equals the render rate the engine used (see above).
  const int sample_rate = render_sample_rate;
  const size_t frames = channels > 0 ? total / static_cast<size_t>(channels) : total;
  std::vector<float> rendered(interleaved, interleaved + total);
  sonare_free_floats(interleaved);
  // The WAV writer requires the count and the layout to agree, so the layout is
  // derived from the count through the shared mapping rather than by a local
  // mono-or-stereo rule that would label any other width stereo. The count is
  // restricted to one of the four speaker-layout widths (1, 2, 6, 8) by
  // validate_cli_arguments before this point; whether it also fits the
  // scene's own master is the C ABI's own check.
  const ChannelLayout layout = layout_from_channel_count(channels);
  save_wav_multichannel(args.output_file, rendered.data(), frames, channels, layout, sample_rate);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("frames", frames)
        .kv("channels", channels)
        .kv("sample_rate", sample_rate)
        .kv("synth", use_synth)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << color::green << "Bounced " << frames << " frames (" << channels << " ch @ "
              << sample_rate << " Hz" << (use_synth ? ", NativeSynth" : "") << ") to "
              << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_project_bounce(const CliArgs& args) { return project_bounce_impl(args, args.has("synth")); }

// `midi-render --in in.json -o out.wav [--synth preset]` — the same render with
// the synth pinned on, so a MIDI project reaches audio without the caller
// having to know it is a project bounce underneath. An empty `--synth` follows
// GM bank/program changes, which is what makes the bare form useful on a
// general-MIDI file.
int cmd_midi_render(const CliArgs& args, const Audio&) {
  return project_bounce_impl(args, true, false);
}

// One take's alignment, as it was measured and as it was written into the
// document. Collected for every take before anything is printed, so the text and
// the `--json` branch report the same run.
struct AlignedTake {
  uint32_t source_id = 0;
  uint32_t warp_ref_id = 0;
  size_t clip_count = 0;
  size_t anchor_count = 0;
  int32_t reference_frames = 0;
  int32_t take_frames = 0;
  float mean_residual_frames = 0.0f;
};

// The local file each source the alignment decodes is read from.
//
// `--audio` and `--resolve-audio` mean what they mean on `project bounce`, down
// to the refusals: what differs is that this command needs the path rather than
// the decoded PCM, so it never binds a source. @p needed is every source the
// alignment must decode, ascending.
//
// Returns 0 when every needed source has a path, and the exit code to report
// otherwise.
int resolve_align_source_paths(const CliArgs& args, const std::map<uint32_t, std::string>& uris,
                               const std::vector<uint32_t>& needed,
                               std::map<uint32_t, std::string>* out_paths) {
  for (const auto& assignment : args.get_string_list("audio")) {
    uint32_t source_id = 0;
    std::string path;
    parse_audio_binding(assignment, &source_id, &path);
    if (uris.count(source_id) == 0) {
      std::cerr << color::red << "Error: --audio " << assignment
                << ": the project has no audio source " << source_id << color::reset << "\n";
      return kExitInvalidParameter;
    }
    (*out_paths)[source_id] = path;
  }
  if (args.has("resolve-audio")) {
    for (const uint32_t source_id : needed) {
      if (out_paths->count(source_id) != 0) continue;
      const auto found = uris.find(source_id);
      const std::string uri = found != uris.end() ? found->second : std::string();
      std::string path;
      if (!path_from_file_uri(uri, &path)) {
        std::cerr << color::red << "Error: source " << source_id << " (" << uri
                  << "): --resolve-audio opens file:// URIs only; pass --audio " << source_id
                  << "=FILE" << color::reset << "\n";
        return kExitInvalidParameter;
      }
      (*out_paths)[source_id] = path;
    }
  }
  // One line per source, for the reason resolve_bounce_audio_sources gives: a
  // document with four unbound takes must not take four runs to diagnose.
  bool missing = false;
  for (const uint32_t source_id : needed) {
    if (out_paths->count(source_id) != 0) continue;
    missing = true;
    const auto found = uris.find(source_id);
    std::cerr << color::red << "Error: source " << source_id << " ("
              << (found != uris.end() ? found->second : std::string())
              << ") has no audio; pass --audio " << source_id << "=FILE or --resolve-audio"
              << color::reset << "\n";
  }
  return missing ? kExitInvalidState : 0;
}

// `project align-takes --in in.json --reference-source <id> -o out.json` — align
// every take in the document against one reference take and write the result in
// as first-class warp maps the takes' own clips point at.
//
// A take is an audio source other than the reference that at least one clip
// references; a source no clip references is not a take and needs no path. Takes
// are processed in ascending source-id order, which is what decides the warp-map
// ids.
int cmd_project_align_takes(const CliArgs& args) {
  ProjectHandle handle;
  SonareError load_error = SONARE_OK;
  if (!load_project_from_args(args, &handle, nullptr, &load_error))
    return project_exit_code(load_error);

  const std::string in_path = project_input_path(args);
  std::vector<uint8_t> document_bytes;
  if (!read_binary_file(in_path, &document_bytes)) {
    std::cerr << color::red << "Error: cannot open project file: " << in_path << color::reset
              << "\n";
    return project_exit_code(SONARE_ERROR_FILE_NOT_FOUND);
  }
  const std::string document(document_bytes.begin(), document_bytes.end());
  const std::map<uint32_t, std::string> uris = project_audio_source_uris(document);
  const ProjectAlignDocumentFacts facts = project_align_document_facts(document);

  const auto reference_source = static_cast<uint32_t>(args.get_int("reference-source", 0));
  if (uris.count(reference_source) == 0) {
    std::cerr << color::red << "Error: --reference-source " << reference_source << ": "
              << (facts.source_ids.count(reference_source) != 0
                      ? "that source is not an audio source"
                      : "the project has no source with that id")
              << color::reset << "\n";
    return kExitInvalidParameter;
  }

  // `uris` is ordered by source id, so this is the ascending order the warp-map
  // ids are handed out in.
  std::vector<uint32_t> take_ids;
  for (const auto& [source_id, uri] : uris) {
    (void)uri;
    if (source_id == reference_source) continue;
    if (facts.clip_ids_by_source.count(source_id) == 0) continue;
    take_ids.push_back(source_id);
  }
  // An invalid parameter rather than a success with an empty result: an aligned
  // document with no takes in it is indistinguishable from one never aligned.
  if (take_ids.empty()) {
    std::cerr << color::red << "Error: no takes to align against source " << reference_source
              << "; a take is another audio source at least one clip references" << color::reset
              << "\n";
    return kExitInvalidParameter;
  }

  std::vector<uint32_t> needed = take_ids;
  needed.push_back(reference_source);
  std::sort(needed.begin(), needed.end());
  std::map<uint32_t, std::string> paths;
  if (const int paths_exit = resolve_align_source_paths(args, uris, needed, &paths);
      paths_exit != 0) {
    return paths_exit;
  }

  // 0 is the C ABI's "use the library value" for both fields, which is what an
  // absent option means, so neither is given a value this CLI would have to keep
  // in step with the core's.
  SonareTakeAlignConfig config{};
  config.hop_length = args.get_int("hop-length", 0);
  config.bins_per_octave = args.get_int("bins-per-octave", 0);

  const auto [reference_samples, reference_rate] = load_audio(paths.at(reference_source));
  uint32_t next_warp_id = facts.max_warp_map_id + 1;
  std::vector<AlignedTake> aligned;
  for (const uint32_t source_id : take_ids) {
    const auto [take_samples, take_rate] = load_audio(paths.at(source_id));
    // Refused before the call rather than after: one chroma frame grid cannot
    // span two rates, and the alignment does no rate conversion.
    if (take_rate != reference_rate) {
      std::cerr << color::red << "Error: source " << source_id << " is " << take_rate
                << " Hz and reference source " << reference_source << " is " << reference_rate
                << " Hz; align-takes reads both at one rate and does not resample" << color::reset
                << "\n";
      return kExitInvalidParameter;
    }
    SonareProjectWarpAnchor* anchors = nullptr;
    size_t anchor_count = 0;
    SonareTakeAlignment alignment{};
    SonareError err = sonare_align_take_to_reference(
        reference_samples.data(), reference_samples.size(), take_samples.data(),
        take_samples.size(), reference_rate, &config, &anchors, &anchor_count, &alignment);
    if (err != SONARE_OK) {
      project_report_error("align source " + std::to_string(source_id), err);
      return project_exit_code(err);
    }
    // The anchors go in exactly as returned: they are already oriented for a clip
    // whose source is the take, so reordering or rescaling them here would invert
    // the map with nothing to report it.
    const std::string name = "take-" + std::to_string(source_id);
    SonareProjectWarpMapDesc desc{};
    desc.id = next_warp_id;
    desc.name = name.c_str();
    desc.anchors = anchors;
    desc.anchor_count = anchor_count;
    err = sonare_project_set_warp_map(handle.ptr, &desc);
    sonare_free_warp_anchors(anchors);
    if (err != SONARE_OK) {
      project_report_error("set warp map " + name, err);
      return project_exit_code(err);
    }
    const std::vector<uint32_t>& clip_ids = facts.clip_ids_by_source.at(source_id);
    for (const uint32_t clip_id : clip_ids) {
      err = sonare_project_set_clip_warp_ref(handle.ptr, clip_id, next_warp_id);
      if (err != SONARE_OK) {
        project_report_error("set clip " + std::to_string(clip_id) + " warp ref", err);
        return project_exit_code(err);
      }
      // A map without a mode plays unaligned, so the mode is part of the job
      // rather than something left to the caller.
      err = sonare_project_set_clip_warp_mode(handle.ptr, clip_id,
                                              SONARE_PROJECT_WARP_MODE_TIME_STRETCH);
      if (err != SONARE_OK) {
        project_report_error("set clip " + std::to_string(clip_id) + " warp mode", err);
        return project_exit_code(err);
      }
    }
    AlignedTake take{};
    take.source_id = source_id;
    take.warp_ref_id = next_warp_id;
    take.clip_count = clip_ids.size();
    take.anchor_count = anchor_count;
    take.reference_frames = alignment.reference_frames;
    take.take_frames = alignment.take_frames;
    take.mean_residual_frames = alignment.mean_residual_frames;
    aligned.push_back(take);
    ++next_warp_id;
  }

  char* json = nullptr;
  size_t len = 0;
  SonareError err = sonare_project_serialize(handle.ptr, &json, &len);
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
    // Neither path is in the payload: both are the caller's own arguments, so
    // echoing them reports nothing the caller did not just write.
    JsonBuilder builder;
    builder.begin_object()
        .kv("reference_source", static_cast<int>(reference_source))
        .kv("take_count", aligned.size())
        .key("takes")
        .begin_array();
    for (const auto& take : aligned) {
      builder.begin_object()
          .kv("source_id", static_cast<int>(take.source_id))
          .kv("warp_ref_id", static_cast<int>(take.warp_ref_id))
          .kv("clip_count", take.clip_count)
          .kv("anchor_count", take.anchor_count)
          .kv("reference_frames", static_cast<int>(take.reference_frames))
          .kv("take_frames", static_cast<int>(take.take_frames))
          .kv("mean_residual_frames", take.mean_residual_frames)
          .end_object();
    }
    builder.end_array().kv("bytes", len).end_object().print();
  } else if (!args.quiet) {
    std::cout << color::green << "Aligned " << aligned.size() << " take(s) against source "
              << reference_source << "\n";
    for (const auto& take : aligned) {
      std::cout << "  source " << take.source_id << " -> warp map " << take.warp_ref_id << ": "
                << take.anchor_count << " anchors, " << take.reference_frames << "/"
                << take.take_frames << " frames, mean residual " << std::fixed
                << std::setprecision(3) << take.mean_residual_frames << std::defaultfloat
                << " frames, " << take.clip_count << " clip(s)\n";
    }
    std::cout << "Wrote " << args.output_file << " (" << len << " bytes)" << color::reset << "\n";
  }
  return 0;
}
#endif  // SONARE_WITH_ARRANGEMENT
