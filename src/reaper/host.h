#pragma once
// Host services shared by all actions: the job runner, console output, the
// per-tick main-thread pump, and the instance handle.

#include "srt/jobs.h"

#include <string>

#include "reaper_api.h"

namespace srt::reaper {

extern REAPER_PLUGIN_HINSTANCE g_hInstance;

// Console output, always prefixed "SRT: ". Safe from any thread: lines from
// worker threads are queued and flushed on the next main-thread tick.
void console(const std::string& line);

srt::JobRunner& jobs();

// Called from REAPER's timer on the main thread.
void onMainThreadTick();

void hostStartup(REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t* rec);
void hostShutdown();

} // namespace srt::reaper
