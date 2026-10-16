#pragma once
// Host services shared by all actions: the job runner, console output, the
// per-tick main-thread pump, and the instance handle.

#include "srt/jobs.h"

#include <string>
#include <vector>

#include "reaper_api.h"

namespace srt::reaper {

extern REAPER_PLUGIN_HINSTANCE g_hInstance;

// Log output, always prefixed "SRT: ". Safe from any thread: lines from
// worker threads are queued and flushed on the next main-thread tick. Every
// line goes to the in-memory history, the SRT log window (if open) and the
// log file. Nothing goes to REAPER's ReaScript console. An error line
// (prefix "error:" or "failed") also opens the log window.
void console(const std::string& line);

// Timestamped lines logged so far this session, plus the tail of the log
// file from earlier sessions, oldest first. Main thread only.
const std::vector<std::string>& logHistory();
void clearLogHistory();
std::string logFilePath();

srt::JobRunner& jobs();

// Called from REAPER's timer on the main thread.
void onMainThreadTick();

void hostStartup(REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t* rec);
void hostShutdown();

} // namespace srt::reaper
