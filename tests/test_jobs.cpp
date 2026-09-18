#include "srt/jobs.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace {
struct CountingJob : srt::Job {
    int steps;
    bool fail;
    explicit CountingJob(int s, bool f = false) : steps(s), fail(f) {}
    std::string name() const override { return "counting"; }
    void run(srt::Progress& p) override {
        for (int i = 0; i < steps; ++i) {
            if (p.cancelled()) throw srt::JobCancelled{};
            p.set((i + 1.0) / steps, "step " + std::to_string(i + 1));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (fail) throw std::runtime_error("boom");
    }
};
} // namespace

TEST_CASE("JobRunner runs a job to completion and reports success") {
    srt::JobRunner runner;
    srt::JobStatus done;
    runner.submit(std::make_shared<CountingJob>(5), [&](const srt::JobStatus& s, srt::Job&) { done = s; });
    runner.waitAll();
    REQUIRE(done.state == srt::JobState::Succeeded);
    REQUIRE(done.fraction == 1.0);
    REQUIRE(done.message == "step 5");
    REQUIRE(done.name == "counting");
}

TEST_CASE("JobRunner reports failures with the exception text") {
    srt::JobRunner runner;
    srt::JobStatus done;
    runner.submit(std::make_shared<CountingJob>(2, true), [&](const srt::JobStatus& s, srt::Job&) { done = s; });
    runner.waitAll();
    REQUIRE(done.state == srt::JobState::Failed);
    REQUIRE(done.error == "boom");
}

TEST_CASE("JobRunner cancels a running job") {
    srt::JobRunner runner;
    srt::JobStatus done;
    auto id = runner.submit(std::make_shared<CountingJob>(1000), [&](const srt::JobStatus& s, srt::Job&) { done = s; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(runner.activeCount() == 1);
    runner.cancel(id);
    runner.waitAll();
    REQUIRE(done.state == srt::JobState::Cancelled);
    REQUIRE(done.fraction < 1.0);
}
