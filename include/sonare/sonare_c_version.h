#pragma once

/// @file sonare_c_version.h
/// @brief Release version of libsonare, usable from C and C++ alike.
/// @details sonare_c.h and the C++ umbrella sonare.h both include this file, so
///          one definition serves both APIs. sonare_version() returns
///          SONARE_VERSION_STRING as compiled into the library.

#define SONARE_VERSION_MAJOR 1
#define SONARE_VERSION_MINOR 11
#define SONARE_VERSION_PATCH 0
#define SONARE_VERSION_STRING "1.11.0"
