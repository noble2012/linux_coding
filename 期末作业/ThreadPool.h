#pragma once
#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

class ThreadPool {
    int threadCount;
    bool started = false;
    bool finished = false;
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mutex;
    std::condition_variable hasData;
    std::condition_variable hasSpace;
    const size_t capacity = 64;
    bool closed = false;

    void pushTask(std::function<void()> task) {
        std::unique_lock<std::mutex> lock(mutex);
        while (!closed && tasks.size() >= capacity) {
            hasSpace.wait(lock);
        }
        if (closed) throw std::runtime_error("任务队列已关闭");
        tasks.push(std::move(task));
        hasData.notify_one();
    }

    bool popTask(std::function<void()> &task) {
        std::unique_lock<std::mutex> lock(mutex);
        while (!closed && tasks.empty()) {
            hasData.wait(lock);
        }
        if (tasks.empty()) return false;
        task = std::move(tasks.front());
        tasks.pop();
        hasSpace.notify_one();
        return true;
    }

    void workerLoop() {
        std::function<void()> task;
        while (popTask(task)) {
            task();
            task = nullptr;
        }
    }

public:
    explicit ThreadPool(int count) : threadCount(count) {
        if (count <= 0) throw std::invalid_argument("线程数必须大于0");
    }

    // 调用者负责管理生命周期，shutdown后不能再次start。
    void start() {
        if (started || finished) throw std::runtime_error("线程池不能重复启动");
        started = true;
        try {
            for (int i = 0; i < threadCount; ++i) {
                workers.emplace_back(&ThreadPool::workerLoop, this);
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    // Result是任务的返回类型，例如submit<int>或submit<void>。
    // 参数由调用处的小函数保存，不需要可变参数模板。
    template <typename Result>
    std::future<Result> submit(std::function<Result()> function) {
        if (!started || finished) throw std::runtime_error("线程池未运行");

        // packaged_task把函数的返回值或异常交给future。
        std::shared_ptr<std::packaged_task<Result()>> task =
            std::make_shared<std::packaged_task<Result()>>(function);
        std::future<Result> result = task->get_future();
        pushTask([task] { (*task)(); });
        return result;
    }

    void shutdown() {
        if (finished) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true; // 队列关闭后，已有任务仍会执行完。
        }
        hasData.notify_all();
        hasSpace.notify_all();
        for (size_t i = 0; i < workers.size(); ++i) {
            if (workers[i].joinable()) {
                //回收线程
                workers[i].join();
            }
        }
        finished = true;
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ~ThreadPool() { shutdown(); }
};
