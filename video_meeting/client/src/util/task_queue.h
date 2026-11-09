// =============================================================================
// 单线程任务队列
//
// 设计文档 2.7 的关键约束：所有信令/mediasoup 操作必须串行（newConsumer 可能
// 在 join 响应后立即到来，但其处理必须晚于 RecvTransport 的创建）。
// 本队列即 mediasoup-demo 浏览器端 async 任务队列的原生等价物：
//   - WS 接收线程只做“解析 + 投递”，不做业务；
//   - Task 线程串行执行全部状态机动作；
//   - mediasoupclient 的 OnConnect/OnProduce 回调发生在本线程的 Produce/Consume
//     调用栈内，在其中发同步 protoo 请求不会与 WS 接收线程互锁（无循环等待）。
// =============================================================================
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace vm {

class TaskQueue {
public:
    using Task = std::function<void()>;

    TaskQueue() = default;
    ~TaskQueue();

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // 启动内部线程（幂等）
    void Start();

    // 停止并回收线程；队列中未执行的任务被丢弃（用于退出）
    void Stop();

    // 当前调用是否就在队列线程（调试/断言用）
    bool IsCurrent() const {
        return thread_.joinable() && std::this_thread::get_id() == thread_.get_id();
    }

    // 投递立即执行的任务（线程安全）
    void Post(Task task);

    // 投递延时任务（线程安全）：delayMs 毫秒后执行，用于重连退避等
    void PostDelayed(int delayMs, Task task);

private:
    struct Item {
        std::chrono::steady_clock::time_point due;  // 到期时刻
        Task                                  task;
    };

    void Run();

    std::thread             thread_;
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::deque<Item>        queue_;   // 就绪任务放队首（due<=now），延时任务按到期时间有序
    bool                    running_ = false;
};

}  // namespace vm
