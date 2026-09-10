#pragma once

/// @file sample_bank_internal.h
/// @brief Definition of the opaque SonareSampleBank handle, shared by the bank
///        surface and the bounce that binds one to a synth instrument.
///
/// The handle owns a shared_ptr rather than the bank itself so a bounce can take
/// a reference for the length of the render: a caller that destroys its handle
/// while audio is still being produced then frees nothing a voice is reading.

#include <memory>

#include "midi/synth/sample_bank.h"

struct SonareSampleBank {
  std::shared_ptr<sonare::midi::synth::SampleBank> bank =
      std::make_shared<sonare::midi::synth::SampleBank>();
};
