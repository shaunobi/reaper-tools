// "SRT: About" prints version information to the console and runs the
// embedded ONNX smoke model as a background job, so it also exercises the
// job runner and the progress window.

#include "actions.h"
#include "host.h"
#include "srt/onnx.h"
#include "srt/version.h"

#include <chrono>
#include <memory>
#include <sstream>
#include <thread>

namespace srt::reaper {

namespace {

struct SmokeJob : srt::Job {
    std::string report;
    std::string name() const override { return "Checking ONNX Runtime"; }
    void run(srt::Progress& p) override {
        p.set(0.1, "creating session");
        report = srt::runEmbeddedSmokeTest();
        // Hold the window open long enough to be seen; real jobs take far longer.
        for (int i = 0; i < 10; ++i) {
            if (p.cancelled()) throw srt::JobCancelled{};
            p.set(0.1 + 0.09 * i, "verifying");
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        p.set(1.0, "done");
    }
};

} // namespace

void actionAbout() {
    std::ostringstream info;
    info << "Shaunobi's Reaper Tools " << SRT_VERSION_STRING << ", REAPER " << GetAppVersion()
         << ", ONNX Runtime " << srt::OnnxRuntime::version() << ", providers:";
    for (srt::Provider p : srt::OnnxRuntime::availableProviders()) info << ' ' << srt::toString(p);
    console(info.str());
    console(std::string("resource path: ") + GetResourcePath());

    jobs().submit(std::make_shared<SmokeJob>(), [](const srt::JobStatus& st, srt::Job& job) {
        auto& smoke = static_cast<SmokeJob&>(job);
        if (st.state == srt::JobState::Succeeded) console(smoke.report);
        else if (st.state == srt::JobState::Cancelled) console("runtime check cancelled");
        else console("runtime check FAILED: " + st.error);
    });
}

} // namespace srt::reaper
