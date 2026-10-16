// Headless replacement for the jobs window: prints job status changes.
#include "progress_window.h"

#include <cstdio>
#include <map>
#include <string>

namespace srt::reaper {

namespace {
std::map<srt::JobId, int> g_lastPercent;
}

void progressWindowRefresh(srt::JobRunner& runner) {
    for (const auto& st : runner.statuses()) {
        const int pct = static_cast<int>(st.fraction * 100.0 + 0.5);
        auto it = g_lastPercent.find(st.id);
        if (it == g_lastPercent.end() || pct / 10 != it->second / 10) {
            std::printf("[jobs] %s: %d%% %s\n", st.name.c_str(), pct, st.message.c_str());
            g_lastPercent[st.id] = pct;
        }
    }
}

void progressWindowDestroy() { g_lastPercent.clear(); }

} // namespace srt::reaper
