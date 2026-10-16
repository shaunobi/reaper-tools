#pragma once
// Background job model. A Job does its work in run() on a worker thread and
// reports through Progress. The host decides how jobs are scheduled and how
// results are applied; JobRunner is a simple thread-per-job scheduler that
// both the CLI and the REAPER extension use.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace srt {

class Progress {
public:
    // fraction in [0,1]. Thread-safe.
    void set(double fraction, const std::string& message = std::string());
    void setFraction(double fraction);
    void setMessage(const std::string& message);
    double fraction() const { return fraction_.load(std::memory_order_relaxed); }
    std::string message() const;

    void requestCancel() { cancel_.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return cancel_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> fraction_{0.0};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    std::string message_;
};

// Thrown by a job when it notices cancellation. JobRunner turns it into
// JobState::Cancelled.
struct JobCancelled {};

class Job {
public:
    virtual ~Job() = default;
    virtual std::string name() const = 0;
    // Do the work. Check progress.cancelled() regularly and throw JobCancelled
    // to stop early. Throw std::exception for failures.
    virtual void run(Progress& progress) = 0;
};

enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };

const char* toString(JobState state);

using JobId = uint64_t;

struct JobStatus {
    JobId id = 0;
    std::string name;
    JobState state = JobState::Queued;
    double fraction = 0.0;
    std::string message;
    std::string error;
};

class JobRunner {
public:
    // Called on whichever thread calls poll() once the job has ended.
    using Completion = std::function<void(const JobStatus&, Job&)>;

    JobRunner() = default;
    ~JobRunner();
    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    JobId submit(std::shared_ptr<Job> job, Completion onDone = nullptr);
    void cancel(JobId id);
    void cancelAll();

    // Delivers completions for finished jobs and prunes them. Call from the
    // thread that owns the results (REAPER's main thread, or the CLI loop).
    void poll();

    std::vector<JobStatus> statuses() const;
    size_t activeCount() const;
    bool hasActive() const { return activeCount() > 0; }

    // Blocks until every job has finished, polling as it goes.
    void waitAll();

private:
    struct Entry {
        JobId id = 0;
        std::shared_ptr<Job> job;
        Progress progress;
        Completion onDone;
        std::thread thread;
        std::atomic<JobState> state{JobState::Queued};
        std::string error;
        std::mutex errorMutex;
    };

    void threadMain(Entry* e);

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
    JobId nextId_ = 1;
};

} // namespace srt
