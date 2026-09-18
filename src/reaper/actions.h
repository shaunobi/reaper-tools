#pragma once
// Action registry. Each action is registered as "SRT: <description>" in the
// main section with a stable command ID string, SWS-style.

#include "reaper_api.h"

namespace srt::reaper {

struct ActionDef {
    const char* idString; // e.g. "SRT_ABOUT" -> shown as _SRT_ABOUT
    const char* description; // e.g. "SRT: About"
    void (*run)();
    int commandId = 0;
    gaccel_register_t accel{};
};

void registerActions(reaper_plugin_info_t* rec);
void unregisterActions();

// Individual actions, one per file.
void actionAbout();

} // namespace srt::reaper
