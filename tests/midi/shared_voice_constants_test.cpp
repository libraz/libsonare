#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "midi/controller_numbers.h"
#include "midi/synth/voice_random.h"
#include "util/constants.h"

namespace {

constexpr uint64_t kSeed = 0x0123456789abcdefull;

static_assert(sonare::midi::cc::kDataEntryMsb == 6);
static_assert(sonare::midi::cc::kDataEntryLsb == 38);
static_assert(sonare::midi::cc::kNrpnLsb == 98);
static_assert(sonare::midi::cc::kNrpnMsb == 99);
static_assert(sonare::midi::cc::kRpnLsb == 100);
static_assert(sonare::midi::cc::kRpnMsb == 101);
static_assert(sonare::constants::kDefaultDawSampleRate == 48000.0);
static_assert(static_cast<float>(sonare::constants::kLn1000D) == 6.907755279f);
static_assert(sonare::constants::kLn1000D == 6.9077552789821370521);
static_assert(sonare::constants::kCentsPerOctave == 1200.0f);

}  // namespace

TEST_CASE("Voice hashing retains the historical SplitMix64 bit patterns", "[midi][synth]") {
  using sonare::midi::synth::voice_hash;

  REQUIRE(voice_hash(0) == 0xe220a8397b1dcdafull);
  REQUIRE(voice_hash(1) == 0x910a2dec89025cc1ull);
  REQUIRE(voice_hash(kSeed) == 0x157a3807a48faa9dull);
  REQUIRE(voice_hash(UINT64_MAX) == 0xe4d971771b652c20ull);
}

TEST_CASE("Voice random sequences retain historical draw order and values", "[midi][synth]") {
  sonare::midi::synth::VoiceRandomSequence sequence(kSeed);
  REQUIRE(sequence.next_unipolar() == 0x1.57a38p-4f);
  REQUIRE(sequence.next_unipolar() == 0x1.d043dcp-1f);
  REQUIRE(sequence.next_unipolar() == 0x1.8a774p-2f);
  REQUIRE(sequence.next_unipolar() == 0x1.369842p-1f);
}
