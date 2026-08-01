#include <cstdlib>
#include <iostream>

#include "sonare.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " INPUT_AUDIO\n";
    return EXIT_FAILURE;
  }

  const auto audio = sonare::Audio::from_file(argv[1]);
  const auto result = sonare::MusicAnalyzer(audio).analyze();
  std::cout << "BPM: " << result.bpm << "\nKey: " << result.key.to_string() << "\n";
  return EXIT_SUCCESS;
}
