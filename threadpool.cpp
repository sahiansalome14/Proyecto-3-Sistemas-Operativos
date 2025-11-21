#include "threadpool.h"

ThreadPool::ThreadPool(size_t threads) : stop(false), activeTasks(0) {
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back([this, i]() {
            {
                std::unique_lock<std::mutex> lock(this->queueMutex);
                threadIds[std::this_thread::get_id()] = i + 1;
            }
            
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queueMutex);
                    this->condition.wait(lock, [this]() { 
                        return this->stop.load() || !this->tasks.empty(); 
                    });
                    if (this->stop.load() && this->tasks.empty()) return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                activeTasks++;
                task();
                activeTasks--;
            }
        });
    }
}

int ThreadPool::getThreadId() {
    std::unique_lock<std::mutex> lock(queueMutex);
    auto it = threadIds.find(std::this_thread::get_id());
    return (it != threadIds.end()) ? it->second : 0;
}

ThreadPool::~ThreadPool() {
    stop.store(true);
    condition.notify_all();
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}