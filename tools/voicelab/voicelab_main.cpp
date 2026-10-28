/// @file voicelab_main.cpp
/// @brief sonare_voicelab: engine measurement and external-device loopback analysis.

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void print_usage(std::FILE* out) {
  std::fprintf(out,
               "Usage: sonare_voicelab <command> [options]\n"
               "\n"
               "Commands:\n"
               "  measure   Run the quality metrics on the voice engines\n"
               "              --engines <list>   comma-separated engine names\n"
               "              --out <path>       output JSON path\n"
               "  analyze   Analyze a loopback recording from an external device\n"
               "              --dry <wav>        recording taken before processing\n"
               "              --wet <wav>        recording taken after processing\n"
               "              --out <path>       output JSON path\n"
               "\n"
               "Options:\n"
               "  --help    Show this message\n");
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

  std::fprintf(stderr, "sonare_voicelab %s: not implemented\n", command.c_str());
  return 2;
}
