#include "plaintext_log.h"
#include "msg_types.h"
#include <windows.h>
#include <spdlog/spdlog.h>
#include <fstream>
#include <mutex>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <string>

extern HMODULE g_hModule;

namespace PlaintextLog {
namespace {

std::mutex g_mutex;
std::ofstream g_stream;
std::atomic<uint64_t> g_outSeq{0};
std::atomic<uint64_t> g_inSeq{0};

std::string TimestampPrefix()
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char buf[32];
    snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03d]",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return buf;
}

// Same 16-bytes-per-line hex+ASCII layout as RelayLogger's LogChunk, so a plaintext
// entry and a relay_packets.log ciphertext entry can be visually diffed side by side
// without a format translation step.
void WriteEntry(const char* direction, uint64_t seq, const uint8_t* data, uint32_t size)
{
    std::ostringstream out;
    out << TimestampPrefix() << " [" << direction << " #" << seq << "] wireSize=" << size;

    if (data && size >= 4) {
        const uint16_t declaredSize = static_cast<uint16_t>(data[0] | (data[1] << 8));
        const uint16_t msgType = static_cast<uint16_t>(data[2] | (data[3] << 8));
        out << " declaredSize=" << declaredSize << " type=0x" << std::hex << std::setw(4)
            << std::setfill('0') << msgType << std::dec;
        if (const char* name = MsgTypeName(msgType))
            out << " (" << name << ")";
        // Flag exactly the divergence LogOutbound's own header comment calls out: the wire
        // size SendMsg was called with does not match what its own header claims -- worth
        // noticing rather than silently trusting one or the other.
        if (declaredSize != size)
            out << " [SIZE MISMATCH: header says " << declaredSize << ", wire is " << size << "]";
    } else {
        out << " (too short for a [size][type] header)";
    }
    out << "\n";

    if (data) {
        for (uint32_t offset = 0; offset < size; offset += 16) {
            out << "  " << std::hex << std::setw(6) << std::setfill('0') << offset << "  " << std::dec;
            for (uint32_t i = 0; i < 16; ++i) {
                if (offset + i < size)
                    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[offset + i]) << ' ' << std::dec;
                else
                    out << "   ";
            }
            out << " ";
            for (uint32_t i = 0; i < 16 && offset + i < size; ++i) {
                const uint8_t b = data[offset + i];
                out << static_cast<char>(b >= 32 && b <= 126 ? b : '.');
            }
            out << "\n";
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stream.is_open()) {
        g_stream << out.str();
        g_stream.flush();
    }
}

} // namespace

void Init()
{
    char buf[MAX_PATH];
    GetModuleFileNameA(g_hModule, buf, MAX_PATH);
    std::string path = buf;
    const auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos)
        path = path.substr(0, pos + 1);
    path += "plaintext_" + std::to_string(GetCurrentProcessId()) + ".log";

    std::lock_guard<std::mutex> lock(g_mutex);
    g_stream.open(path, std::ios::out | std::ios::trunc);
    if (!g_stream.is_open()) {
        spdlog::error("[plaintext] failed to open {}", path);
        return;
    }
    spdlog::info("[plaintext] logging plaintext SendMsg calls to {}", path);
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stream.is_open()) {
        g_stream.flush();
        g_stream.close();
    }
}

void LogOutbound(const uint8_t* data, uint32_t wireSize)
{
    WriteEntry("OUT", g_outSeq.fetch_add(1) + 1, data, wireSize);
}

void LogInbound(const uint8_t* data, uint32_t size)
{
    WriteEntry("IN", g_inSeq.fetch_add(1) + 1, data, size);
}

} // namespace PlaintextLog
