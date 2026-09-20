/// @file bowed_string_body_test.cpp
/// @brief The bowed family's corpus stage: the tilt of the body resonator's dry
///        floor and the scaling of its mode bank. Cases carry [bowed][body] and
///        are invoked as an AND, because [body] alone reaches other tests.

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "midi/bowed_string_probe.h"
#include "midi/synth/body_resonator.h"
#include "support/golden_hash.h"
