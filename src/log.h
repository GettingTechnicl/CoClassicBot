#pragma once

#include <spdlog/spdlog.h>
#include <string>

namespace Log {

// Initialize the rotating file logger. Call once from InitThread.
// logPath: full path to the log file (e.g. next to the DLL)
// maxSizeMb: max file size before rotation (default 5 MB)
// maxFiles: number of rotated files to keep (default 3)
void Init(const char* logPath = nullptr, size_t maxSizeMb = 5, size_t maxFiles = 3);

// Shut down spdlog (flush + drop all loggers). Call on DLL_PROCESS_DETACH.
void Shutdown();

// Change the active log level at runtime. `level` is a raw
// spdlog::level::level_enum value (trace=0 .. off=6); clamped to that range.
// Safe to call before Init() — applied once the logger exists.
void SetLevel(int level);

// Full path to the active log file, e.g. for display in a debug UI.
// Empty if Init() hasn't run yet.
const std::string& GetLogPath();

}  // namespace Log
