#pragma once

#include <filesystem>

// Options are PS2X_* environment variables. A settings file can supply the same names for runs
// without a launcher: lines of NAME=value, with # or ; starting a comment. The file is the one
// PS2X_CONFIG names, else ps2x.ini in `gameDirectory`. A variable already set in the environment
// wins over the file. Call once, before anything reads an option. Returns how many were applied.
int ps2xLoadConfigFile(const std::filesystem::path &gameDirectory);
