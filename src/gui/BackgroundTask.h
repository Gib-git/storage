#pragma once

#include <QObject>
#include <QPointer>

#include <functional>
#include <thread>

// Runs one job on a worker thread and delivers its result on the GUI thread.
// The owner must outlive the job; the destructor waits for it to finish.
class BackgroundTask {
public:
    BackgroundTask() = default;
    ~BackgroundTask() { join(); }
    BackgroundTask(const BackgroundTask&) = delete;
    BackgroundTask& operator=(const BackgroundTask&) = delete;

    bool running() const { return running_; }

    template <typename Result>
    void start(QObject* context, std::function<Result()> work, std::function<void(Result)> done) {
        join();
        running_ = true;
        QPointer<QObject> ctx(context);
        thread_ = std::thread([this, ctx, work = std::move(work), done = std::move(done)]() mutable {
            auto result = std::make_shared<Result>(work());
            if (!ctx) return;
            QMetaObject::invokeMethod(
                ctx.data(),
                [this, result, done = std::move(done)] {
                    join();
                    done(std::move(*result));
                },
                Qt::QueuedConnection);
        });
    }

    void join() {
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
        running_ = false;
    }

private:
    std::thread thread_;
    bool running_ = false;
};
