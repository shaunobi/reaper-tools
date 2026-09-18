#pragma once
// Logging sink. srt_core never prints on its own; the host (CLI or REAPER
// extension) installs a sink that decides where lines go.

#include <functional>
#include <string>

namespace srt {

enum class LogLevel { Info, Warning, Error };

using LogSink = std::function<void(LogLevel, const std::string&)>;

void setLogSink(LogSink sink);
void log(LogLevel level, const std::string& message);
inline void logInfo(const std::string& m) { log(LogLevel::Info, m); }
inline void logWarning(const std::string& m) { log(LogLevel::Warning, m); }
inline void logError(const std::string& m) { log(LogLevel::Error, m); }

} // namespace srt
