#include "srt/log.h"

#include <mutex>

namespace srt {
namespace {
std::mutex g_mutex;
LogSink g_sink;
} // namespace

void setLogSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

void log(LogLevel level, const std::string& message) {
    LogSink sink;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        sink = g_sink;
    }
    if (sink) sink(level, message);
}

} // namespace srt
