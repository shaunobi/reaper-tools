#include "host.h"

#include "progress_window.h"
#include "srt/log.h"

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

void writeConsole(const std::string& line) {
    std::string s = "SRT: " + line + "\n";
    ShowConsoleMsg(s.c_str());
}
} // namespace

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
}

} // namespace srt::reaper
