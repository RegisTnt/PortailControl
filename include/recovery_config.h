#pragma once

constexpr const char *FIRMWARE_VERSION = "1.0.1-recovery";

#ifndef FIRMWARE_GIT_HASH
#define FIRMWARE_GIT_HASH "unknown"
#endif

constexpr unsigned long RELAY_DELAY_DEFAULT_MS = 500;
constexpr unsigned long RELAY_DELAY_MIN_MS = 100;
constexpr unsigned long RELAY_DELAY_MAX_MS = 5000;
