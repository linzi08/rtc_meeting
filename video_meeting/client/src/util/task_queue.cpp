#include "task_queue.h"

#include "../util/log.h"

namespace vm {

static const char* TAG = "task";

TaskQueue::~TaskQueue() {
    Stop();
}

void TaskQueue::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    // 注意：thread_ 在持锁状态下启动，Run() 里会再抢锁，安全（锁先释放）
    thread_ = std::thread([this] { Run(); });
}

void TaskQueue::Stop() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
        cv_.notify_all();
        t = std::move(thread_);
    }
    if (t.joinable()) {
        t.join();
        LOGT(TAG, "TaskQueue 已停止");
    }
}

void TaskQueue::Post(Task task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    // 就绪任务 due 置为最小值，保证排在所有延时任务之前
    queue_.push_back(Item{std::chrono::steady_clock::time_point::min(), std::move(task)});
    cv_.notify_all();
}

void TaskQueue::PostDelayed(int delayMs, Task task) {
    auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    // 按到期时间插入，保持队列按 due 有序（延时任务间）
    bool inserted = false;
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->due > due) {
            queue_.insert(it, Item{due, std::move(task)});
            inserted = true;
            break;
        }
    }
    if (!inserted) queue_.push_back(Item{due, std::move(task)});
    cv_.notify_all();
}

void TaskQueue::Run() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!running_) return;

            if (queue_.empty()) {
                cv_.wait(lock);  // 无任务：睡到有新任务
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            if (queue_.front().due > now) {
                // 队首是延时任务且未到期：睡到到期或新任务到来
                cv_.wait_until(lock, queue_.front().due);
                continue;
            }

            task = std::move(queue_.front().task);
            queue_.pop_front();
        }

        if (task) {
            // 任务异常不允许杀死队列线程：捕获并记录
            try {
                task();
            } catch (const std::exception& e) {
                LOGT("task", "任务异常: %s", e.what());
            } catch (...) {
                LOGT("task", "任务未知异常");
            }
        }
    }
}

}  // namespace vm