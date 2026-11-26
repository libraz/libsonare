#pragma once

/// @file sonare_c_version.h
/// @brief Release version of libsonare, usable from C and C++ alike.
/// @details sonare_c.h and the C++ umbrella sonare.h both include this file, so
///          one definition serves both APIs. sonare_version() returns
///          SONARE_VERSION_STRING as compiled into the library.

/// Major version number.
#define SONARE_VERSION_MAJOR 2
/// Minor version number.
#define SONARE_VERSION_MINOR 0
/// Patch version number.
#define SONARE_VERSION_PATCH 0
/// The three numbers as "MAJOR.MINOR.PATCH".
#define SONARE_VERSION_STRING "2.0.0"
