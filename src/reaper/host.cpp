#include "host.h"

#include "log_window.h"
#include "progress_window.h"
#include "srt/log.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace srt::reaper {

REAPER_PLUGIN_HINSTANCE g_hInstance = nullptr;

namespace {
std::mutex g_consoleMutex;
std::vector<std::string> g_pending;
std::thread::id g_mainThread;
srt::JobRunner* g_jobs = nullptr;

constexpr size_t kHistoryMaxLines = 5000; // in memory
constexpr size_t kFileTailBytes = 256 * 1024; // loaded from earlier sessions
constexpr std::uintmax_t kFileMaxBytes = 4 * 1024 * 1024; // the file is trimmed past this at startup

std::vector<std::string> g_history;
std::string g_logPath;

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

bool isErrorLine(const std::string& line) {
    return line.rfind("error:", 0) == 0 || line.rfind("failed", 0) == 0 || line.find(": FAILED") != std::string::npos ||
           line.find("failed on \"") != std::string::npos;
}

void appendToFile(const std::string& line) {
    if (g_logPath.empty()) return;
    std::ofstream f(g_logPath, std::ios::app);
    if (f) f << line << '\n';
}

// Reads the end of the log file into the history, as whole lines, so the
// window shows what happened last time too. Trims the file if it grew big.
void loadFileTail() {
    if (g_logPath.empty()) return;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(g_logPath, ec);
    if (ec || size == 0) return;
    std::ifstream f(g_logPath, std::ios::binary);
    if (!f) return;
    const std::uintmax_t start = size > kFileTailBytes ? size - kFileTailBytes : 0;
    f.seekg(static_cast<std::streamoff>(start));
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    size_t pos = 0;
    if (start > 0) {
        const size_t nl = text.find('\n');
        pos = nl == std::string::npos ? text.size() : nl + 1; // drop the partial first line
    }
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) g_history.push_back(line);
        pos = nl + 1;
    }
    if (size > kFileMaxBytes) {
        std::ofstream out(g_logPath, std::ios::trunc);
        for (const auto& l : g_history) out << l << '\n';
    }
    g_history.push_back(timestamp() + " ---- REAPER started; lines above are from earlier sessions ----");
}

void writeConsole(const std::string& line) {
    const std::string stamped = timestamp() + " SRT: " + line;
    g_history.push_back(stamped);
    if (g_history.size() > kHistoryMaxLines) g_history.erase(g_history.begin(), g_history.begin() + 500);
    appendToFile(stamped);
    logWindowAppend(stamped);
    if (isErrorLine(line)) logWindowShow();
}
} // namespace

const std::vector<std::string>& logHistory() { return g_history; }

void clearLogHistory() {
    g_history.clear();
    if (!g_logPath.empty()) {
        std::ofstream out(g_logPath, std::ios::trunc);
    }
}

std::string logFilePath() { return g_logPath; }

void console(const std::string& line) {
    if (std::this_thread::get_id() == g_mainThread) {
        writeConsole(line);
        return;
    }
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    g_pending.push_back(line);
}

srt::JobRunner& jobs() { return *g_jobs; }

void onMainThreadTick() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_consoleMutex);
        lines.swap(g_pending);
    }
    for (const auto& l : lines) writeConsole(l);
    if (g_jobs) {
        g_jobs->poll();
        progressWindowRefresh(*g_jobs);
    }
}

void hostStartup(REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t*) {
    g_hInstance = instance;
    g_mainThread = std::this_thread::get_id();
    g_jobs = new srt::JobRunner();

    const char* resource = GetResourcePath();
    if (resource && *resource) {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(resource) / "SRT";
        std::filesystem::create_directories(dir, ec);
        g_logPath = (dir / "srt.log").string();
        loadFileTail();
    }
    srt::setLogSink([](srt::LogLevel level, const std::string& m) {
        const char* tag = level == srt::LogLevel::Error ? "error: " : level == srt::LogLevel::Warning ? "warning: " : "";
        console(tag + m);
    });
}

void hostShutdown() {
    srt::setLogSink(nullptr);
    if (g_jobs) {
        g_jobs->cancelAll();
        g_jobs->waitAll();
        delete g_jobs;
        g_jobs = nullptr;
    }
    progressWindowDestroy();
    logWindowDestroy();
}

void actionShowLog() { logWindowToggle(); }

} // namespace srt::reaper
