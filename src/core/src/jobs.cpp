#include "srt/jobs.h"

#include <algorithm>
#include <chrono>
#include <exception>

namespace srt {

void Progress::set(double fraction, const std::string& message) {
    setFraction(fraction);
    if (!message.empty()) setMessage(message);
}

void Progress::setFraction(double fraction) {
    fraction = fraction < 0.0 ? 0.0 : fraction > 1.0 ? 1.0 : fraction;
    fraction_.store(fraction, std::memory_order_relaxed);
}

void Progress::setMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = message;
}

std::string Progress::message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

const char* toString(JobState state) {
    switch (state) {
    case JobState::Queued: return "queued";
    case JobState::Running: return "running";
    case JobState::Succeeded: return "succeeded";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

JobRunner::~JobRunner() {
    cancelAll();
    waitAll();
}

JobId JobRunner::submit(std::shared_ptr<Job> job, Completion onDone) {
    auto entry = std::make_unique<Entry>();
    entry->job = std::move(job);
    entry->onDone = std::move(onDone);
    Entry* raw = entry.get();
    JobId id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = nextId_++;
        entry->id = id;
        entries_.push_back(std::move(entry));
    }
    raw->thread = std::thread([this, raw] { threadMain(raw); });
    return id;
}

void JobRunner::threadMain(Entry* e) {
    e->state.store(JobState::Running);
    try {
        if (e->progress.cancelled()) throw JobCancelled{};
        e->job->run(e->progress);
        e->state.store(e->progress.cancelled() ? JobState::Cancelled : JobState::Succeeded);
    } catch (const JobCancelled&) {
        e->state.store(JobState::Cancelled);
    } catch (const std::exception& ex) {
        {
            std::lock_guard<std::mutex> lock(e->errorMutex);
            e->error = ex.what();
        }
        e->state.store(JobState::Failed);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(e->errorMutex);
            e->error = "unknown error";
        }
        e->state.store(JobState::Failed);
    }
}

void JobRunner::cancel(JobId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : entries_)
        if (e->id == id) e->progress.requestCancel();
}

void JobRunner::cancelAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : entries_) e->progress.requestCancel();
}

void JobRunner::poll() {
    std::vector<std::unique_ptr<Entry>> finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::stable_partition(entries_.begin(), entries_.end(), [](const auto& e) {
            JobState s = e->state.load();
            return s == JobState::Queued || s == JobState::Running;
        });
        for (auto j = it; j != entries_.end(); ++j) finished.push_back(std::move(*j));
        entries_.erase(it, entries_.end());
    }
    for (auto& e : finished) {
        if (e->thread.joinable()) e->thread.join();
        JobStatus st;
        st.id = e->id;
        st.name = e->job->name();
        st.state = e->state.load();
        st.fraction = e->progress.fraction();
        st.message = e->progress.message();
        {
            std::lock_guard<std::mutex> lock(e->errorMutex);
            st.error = e->error;
        }
        if (e->onDone) e->onDone(st, *e->job);
    }
}

std::vector<JobStatus> JobRunner::statuses() const {
    std::vector<JobStatus> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        JobStatus st;
        st.id = e->id;
        st.name = e->job->name();
        st.state = e->state.load();
        st.fraction = e->progress.fraction();
        st.message = e->progress.message();
        out.push_back(std::move(st));
    }
    return out;
}

size_t JobRunner::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void JobRunner::waitAll() {
    while (hasActive()) {
        poll();
        if (hasActive()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace srt
