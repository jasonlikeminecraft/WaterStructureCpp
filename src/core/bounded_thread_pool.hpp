#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace water_structure::detail {

class BoundedThreadPool {
public:
    BoundedThreadPool(std::size_t worker_count, std::size_t queue_capacity)
        : mQueueCapacity(std::max<std::size_t>(queue_capacity, 1))
    {
        worker_count = std::max<std::size_t>(worker_count, 1);
        mWorkers.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            mWorkers.emplace_back([this, index](std::stop_token stop_token) {
                worker_loop(stop_token, index);
            });
        }
    }

    BoundedThreadPool(const BoundedThreadPool&) = delete;
    BoundedThreadPool& operator=(const BoundedThreadPool&) = delete;

    ~BoundedThreadPool()
    {
        {
            const std::scoped_lock lock(mMutex);
            mAccepting = false;
        }
        mTaskReady.notify_all();
        mQueueSpace.notify_all();
        for (auto& worker : mWorkers) {
            if (worker.joinable()) worker.join();
        }
    }

    template <typename Function>
    auto submit(Function&& function)
        -> std::future<std::invoke_result_t<std::decay_t<Function>>>
    {
        using Return = std::invoke_result_t<std::decay_t<Function>>;
        auto task = std::make_shared<std::packaged_task<Return()>>(
            std::forward<Function>(function));
        auto future = task->get_future();

        {
            std::unique_lock lock(mMutex);
            mQueueSpace.wait(lock, [this] {
                return !mAccepting || mTasks.size() < mQueueCapacity;
            });
            if (!mAccepting) {
                throw std::runtime_error("thread pool is shutting down");
            }
            mTasks.emplace_back([task](std::size_t) { (*task)(); });
        }
        mTaskReady.notify_one();
        return future;
    }

    template <typename Function>
    auto submit_indexed(Function&& function)
        -> std::future<std::invoke_result_t<std::decay_t<Function>, std::size_t>>
    {
        using Return = std::invoke_result_t<std::decay_t<Function>, std::size_t>;
        auto task = std::make_shared<std::packaged_task<Return(std::size_t)>>(
            std::forward<Function>(function));
        auto future = task->get_future();

        {
            std::unique_lock lock(mMutex);
            mQueueSpace.wait(lock, [this] {
                return !mAccepting || mTasks.size() < mQueueCapacity;
            });
            if (!mAccepting) {
                throw std::runtime_error("thread pool is shutting down");
            }
            mTasks.emplace_back([task](std::size_t worker_index) {
                (*task)(worker_index);
            });
        }
        mTaskReady.notify_one();
        return future;
    }

private:
    void worker_loop(std::stop_token stop_token, std::size_t worker_index)
    {
        while (!stop_token.stop_requested()) {
            std::function<void(std::size_t)> task;
            {
                std::unique_lock lock(mMutex);
                const auto ready = mTaskReady.wait(lock, stop_token, [this] {
                    return !mTasks.empty() || !mAccepting;
                });
                if (!ready || stop_token.stop_requested()) return;
                if (mTasks.empty()) {
                    if (!mAccepting) return;
                    continue;
                }
                task = std::move(mTasks.front());
                mTasks.pop_front();
            }
            mQueueSpace.notify_one();
            task(worker_index);
        }
    }

    std::size_t mQueueCapacity;
    std::mutex mMutex;
    std::condition_variable_any mTaskReady;
    std::condition_variable mQueueSpace;
    std::deque<std::function<void(std::size_t)>> mTasks;
    std::vector<std::jthread> mWorkers;
    bool mAccepting = true;
};

} // namespace water_structure::detail
