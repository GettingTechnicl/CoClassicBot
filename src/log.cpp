#include "log.h"

#include <windows.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <algorithm>
#include <string>

extern HMODULE g_hModule;

namespace Log {

namespace {
std::string g_logPath;
}

void Init(const char* logPath, size_t maxSizeMb, size_t maxFiles)
{
    std::string path;
    if (logPath && logPath[0]) {
        path = logPath;
    } else {
        char buf[MAX_PATH];
        GetModuleFileNameA(g_hModule, buf, MAX_PATH);
        path = buf;
        auto pos = path.find_last_of("\\/");
        if (pos != std::string::npos)
            path = path.substr(0, pos + 1);
        // Session 14 [PER-PROCESS LOG SEPARATION]: was a fixed "coclassic.log"
        // regardless of which account/process wrote it. Live-confirmed this
        // session: running two ImConquer.exe instances (two accounts)
        // simultaneously interleaves BOTH characters' log lines into the
        // identical file, since rotating_file_sink_mt opens with shared
        // read/write and both processes happily append to the same path.
        // That made a melee-vs-archer live-behavior investigation require
        // filtering by content (plugin-specific state-reason phrases)
        // instead of just reading the file for the account being debugged.
        // PID is unique per running process and needs no account-name
        // plumbing this early in init (well before login/account selection
        // is known) — spdlog's own rotation then appends ".1"/".2"/etc. to
        // THIS name, so "coclassic_<pid>.1.log" stays unambiguous against
        // this suffix rather than colliding with it.
        path += "coclassic_" + std::to_string(GetCurrentProcessId()) + ".log";
    }

    try {
        auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path, maxSizeMb * 1024 * 1024, maxFiles);

        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        auto logger = std::make_shared<spdlog::logger>("cc",
            spdlog::sinks_init_list{fileSink, consoleSink});

        logger->set_level(spdlog::level::trace);
        logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
        logger->flush_on(spdlog::level::debug);

        spdlog::set_default_logger(logger);
        g_logPath = path;
        spdlog::info("Logger initialized — file: {}", path);
    } catch (const spdlog::spdlog_ex& ex) {
        // Fallback: if file logging fails, just use console
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("cc", consoleSink);
        logger->set_level(spdlog::level::trace);
        logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(logger);
        spdlog::warn("File logger failed ({}), using console only", ex.what());
    }
}

void Shutdown()
{
    spdlog::info("Logger shutting down");
    spdlog::default_logger()->flush();
    spdlog::shutdown();
}

void SetLevel(int level)
{
    const int clamped = std::clamp(level, (int)spdlog::level::trace, (int)spdlog::level::off);
    if (auto logger = spdlog::default_logger())
        logger->set_level((spdlog::level::level_enum)clamped);
}

const std::string& GetLogPath()
{
    return g_logPath;
}

}  // namespace Log
