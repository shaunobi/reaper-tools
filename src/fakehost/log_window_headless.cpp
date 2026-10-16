// Headless replacement for the log window: every line goes straight into
// the fake host's console capture, which the scenarios check.
#include "log_window.h"

#include "fake_reaper.h"

#include <cstdio>

namespace srt::reaper {

void logWindowAppend(const std::string& line) { fake::host().logLine(line); }
void logWindowToggle() { std::printf("[log window] toggled\n"); }
void logWindowShow() { std::printf("[log window] shown\n"); }
void logWindowDestroy() {}

} // namespace srt::reaper
