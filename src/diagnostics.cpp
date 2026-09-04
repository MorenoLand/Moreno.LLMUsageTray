#include "diagnostics.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <atomic>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <limits.h>
#include <unistd.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

static std::mutex g_diagnostics_mutex;
static std::atomic_bool g_diagnostics_enabled{false};

static std::filesystem::path executable_directory() {
#if defined(_WIN32)
    std::vector<char> buffer(512);
    DWORD length = 0;
    while (buffer.size() < 32768 && (length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()))) >= buffer.size()) buffer.resize(buffer.size() * 2);
    if (length > 0 && length < buffer.size()) return std::filesystem::path(std::string(buffer.data(), length)).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) == -1 && size > 0) {
        std::vector<char> buffer(size + 1);
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) return std::filesystem::path(buffer.data()).parent_path();
    }
#else
    std::vector<char> buffer(PATH_MAX);
    ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length))).parent_path();
#endif
    std::error_code error;
    return std::filesystem::current_path(error);
}

void diagnostics_init(bool enabled) {
    g_diagnostics_enabled = enabled;
    if (!enabled) return;
#if defined(_WIN32)
    if (!GetConsoleWindow() && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif
    diagnostics_log("debug enabled");
}

std::string diagnostics_log_path() {
    return (executable_directory() / "app.log").string();
}

void diagnostics_log(const std::string& message) {
    if (!g_diagnostics_enabled) return;
    std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
    std::filesystem::path path = diagnostics_log_path();
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (std::filesystem::file_size(path, error) > 2 * 1024 * 1024) {
        std::filesystem::remove(path, error);
    }
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream line;
    line << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << " " << message;
    if (out) out << line.str() << "\n";
    std::cerr << line.str() << std::endl;
}

void diagnostics_log_raw(const std::string& label, const std::string& body) {
    if (!g_diagnostics_enabled) return;
    std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
    std::filesystem::path path = diagnostics_log_path();
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (std::filesystem::file_size(path, error) > 2 * 1024 * 1024) std::filesystem::remove(path, error);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream prefix;
    prefix << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << " " << label;
    std::ofstream out(path, std::ios::app);
    if (out) out << prefix.str() << "\n" << body << "\n";
    std::cerr << prefix.str() << std::endl << body << std::endl;
}
