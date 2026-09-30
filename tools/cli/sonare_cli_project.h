#pragma once

/// @file sonare_cli_project.h
/// @brief State and helpers shared by the `project` command family's translation units.

#include <cstdint>
#include <string>

#include "sonare_cli.h"

#ifdef SONARE_WITH_ARRANGEMENT

// Invalid-parameter exit code, mirroring kExitInvalidParameter in
// tools/cli/sonare_cli.cpp. This is what a plain `1` from any handler normalizes
// to, so a branch only spells it out when it is returning the code alongside
// others (see project_exit_code).
constexpr int kExitInvalidParameter = 3;

// Invalid-state exit code used when `project validate --strict` finds loader
// diagnostics after still writing the canonical artifact and JSON payload, and
// when the C ABI itself reports an invalid state.
constexpr int kExitInvalidState = 9;

struct ProjectHandle {
  SonareProject* ptr = nullptr;
  ~ProjectHandle() { sonare_project_destroy(ptr); }
  ProjectHandle() = default;
  ProjectHandle(const ProjectHandle&) = delete;
  ProjectHandle& operator=(const ProjectHandle&) = delete;
};

void project_report_error(const std::string& what, SonareError err);
int project_exit_code(SonareError err);
int report_output_write_failure(const std::string& path);
bool write_binary_file(const std::string& path, const uint8_t* data, size_t len);
std::string project_input_path(const CliArgs& args);
bool load_project_from_args(const CliArgs& args, ProjectHandle* handle,
                            std::string* diagnostics = nullptr, SonareError* load_error = nullptr);

int cmd_project_bounce(const CliArgs& args);
int cmd_project_align_takes(const CliArgs& args);
int cmd_project_export_smf(const CliArgs& args);
int cmd_project_export_midi2(const CliArgs& args);
int cmd_project_import_smf(const CliArgs& args);
int cmd_project_import_midi2(const CliArgs& args);

#endif  // SONARE_WITH_ARRANGEMENT
