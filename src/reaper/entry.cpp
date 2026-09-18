// REAPER loads this library at startup and calls ReaperPluginEntry once with
// a non-null rec (load) and once with a null rec (unload).

#include "actions.h"
#include "host.h"
#include "srt/version.h"

namespace {

void onTimer() { srt::reaper::onMainThreadTick(); }

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance,
                                                                  reaper_plugin_info_t* rec) {
    if (!rec) {
        plugin_register("-timer", reinterpret_cast<void*>(&onTimer));
        srt::reaper::unregisterActions();
        srt::reaper::hostShutdown();
        return 0;
    }
    if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc) return 0;

    const int missing = REAPERAPI_LoadAPI(rec->GetFunc);
    if (missing != 0) {
        // Every function in reaper_api.h is old enough that this only happens
        // on a REAPER version far below what SRT supports.
        if (rec->GetFunc("ShowConsoleMsg")) {
            auto show = reinterpret_cast<void (*)(const char*)>(rec->GetFunc("ShowConsoleMsg"));
            show("SRT: this REAPER version is too old, extension not loaded\n");
        }
        return 0;
    }

    srt::reaper::hostStartup(hInstance, rec);
    srt::reaper::registerActions(rec);
    rec->Register("timer", reinterpret_cast<void*>(&onTimer));
    srt::reaper::console(std::string("loaded version ") + SRT_VERSION_STRING);
    return 1;
}
