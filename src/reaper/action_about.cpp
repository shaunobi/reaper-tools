// "SRT: About" doubles as the diagnostic for the machine-learning runtime.
// It walks through the risky steps one at a time on the main thread, printing
// a line before each so the console shows exactly where a failure happens,
// with every step wrapped in the crash guard. Only when all of that passes
// does it run the smoke model as a background job, which also exercises the
// job runner and the jobs window.

#include "actions.h"
#include "crash_guard.h"
#include "host.h"
#include "runtime_loader.h"
#include "srt/onnx.h"
#include "srt/version.h"

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace srt::reaper {

namespace {

struct SmokeJob : srt::Job {
    std::string report;
    std::string name() const override { return "Checking ONNX Runtime (background)"; }
    void run(srt::Progress& p) override {
        p.set(0.1, "creating session");
        std::string fault;
        if (!runGuarded([&] { report = srt::runEmbeddedSmokeTest(); }, fault)) throw std::runtime_error(fault);
        for (int i = 0; i < 10; ++i) {
            if (p.cancelled()) throw srt::JobCancelled{};
            p.set(0.1 + 0.09 * i, "verifying");
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        p.set(1.0, "done");
    }
};

// Runs one diagnostic step on the main thread. Returns false if it failed.
bool step(const char* label, const std::function<void()>& fn) {
    console(std::string("about: ") + label + " ...");
    std::string fault;
    try {
        if (!runGuarded(fn, fault)) {
            console(std::string("about: FAILED with ") + fault);
            return false;
        }
    } catch (const std::exception& ex) {
        console(std::string("about: FAILED: ") + ex.what());
        return false;
    }
    return true;
}

} // namespace

void actionAbout() {
    console(std::string("Shaunobi's Reaper Tools ") + SRT_VERSION_STRING + ", REAPER " + GetAppVersion());
    console(std::string("resource path: ") + GetResourcePath());
    for (const auto& line : runtimeModuleReport()) console("  " + line);

    std::string version;
    if (!step("reading the runtime version", [&] { version = srt::OnnxRuntime::version(); })) return;
    console("  ONNX Runtime " + version);

    std::vector<srt::Provider> providers;
    if (!step("listing execution providers", [&] { providers = srt::OnnxRuntime::availableProviders(); })) return;
    std::ostringstream list;
    for (srt::Provider p : providers) list << ' ' << srt::toString(p);
    console("  providers:" + list.str());

    std::string report;
    if (!step("running the smoke model on the CPU", [&] { report = srt::runEmbeddedSmokeTest({srt::Provider::Cpu}); })) return;
    console("  " + report);

    if (providers.size() > 1) {
        const srt::Provider best = providers.front();
        if (!step((std::string("running the smoke model on ") + srt::toString(best)).c_str(),
                  [&] { report = srt::runEmbeddedSmokeTest({best}); }))
            return;
        console("  " + report);
    }

    console("about: main-thread checks passed; starting the background check (opens the jobs window)");
    jobs().submit(std::make_shared<SmokeJob>(), [](const srt::JobStatus& st, srt::Job& job) {
        auto& smoke = static_cast<SmokeJob&>(job);
        if (st.state == srt::JobState::Succeeded) console("background check OK: " + smoke.report);
        else if (st.state == srt::JobState::Cancelled) console("background check cancelled");
        else console("background check FAILED: " + st.error);
    });
}

} // namespace srt::reaper
