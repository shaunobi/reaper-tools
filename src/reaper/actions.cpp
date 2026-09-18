#include "actions.h"

#include "host.h"

#include <cstring>

namespace srt::reaper {

namespace {

ActionDef g_actions[] = {
    {"SRT_ABOUT", "SRT: About (version, runtime check)", &actionAbout},
    {"SRT_SEPARATE_STEMS", "SRT: Separate stems (drums, bass, other, vocals from selected items)", &actionSeparateStems},
    {"SRT_FIT_DRUMS_TO_GRID", "SRT: Fit drums to grid (first and last bar of selected items)", &actionFitDrumsToGrid},
    {"SRT_CONFORM_DRUM_BARS", "SRT: Conform drum bars (stretch marker on every downbeat of selected items)",
     &actionConformDrumBars},
};

bool hookCommand(int command, int) {
    if (command == 0) return false;
    for (ActionDef& a : g_actions) {
        if (a.commandId == command) {
            a.run();
            return true;
        }
    }
    return false;
}

} // namespace

void registerActions(reaper_plugin_info_t* rec) {
    for (ActionDef& a : g_actions) {
        a.commandId = rec->Register("command_id", const_cast<char*>(a.idString));
        if (a.commandId == 0) {
            console(std::string("could not register action ") + a.idString);
            continue;
        }
        std::memset(&a.accel, 0, sizeof(a.accel));
        a.accel.accel.cmd = static_cast<WORD>(a.commandId);
        a.accel.desc = a.description;
        rec->Register("gaccel", &a.accel);
    }
    rec->Register("hookcommand", reinterpret_cast<void*>(&hookCommand));
}

void unregisterActions() {
    plugin_register("-hookcommand", reinterpret_cast<void*>(&hookCommand));
    for (ActionDef& a : g_actions)
        if (a.commandId) plugin_register("-gaccel", &a.accel);
}

} // namespace srt::reaper
