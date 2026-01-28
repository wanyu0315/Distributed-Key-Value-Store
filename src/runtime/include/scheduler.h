#ifndef __MONSOON_SCHEDULER_H__
#define __MONSOON_SCHEDULER_H__

#include <atomic>
#include <boost/type_index.hpp>
#include <functional>
#include <list>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <iostream>

#include "fiber.h"
#include "mutex.h"
#include "thread.h"
#include "utils.h"

namespace monsoon {

/**
 * @brief N-M 协程调度器 (Raft 优化版)
 * * 场景：作为 IOManager 的基类，管理线程池和任务队列
 * * 架构变更：
 * 从“全局队列”改为“Thread-per-Core”模型，包含线程私有队列和公共窃取队列。
 */
class Scheduler {
public:
    typedef std::shared_ptr<Scheduler> ptr;
    typedef Mutex MutexType;

    /**
     * @brief 构造函数
     * @param threads 线程数量
     * @param use_caller 是否将当前线程纳入调度
     * @param name 调度器名称
     */
    Scheduler(size_t threads = 1, bool use_caller = true, const std::string &name = "Scheduler");
    
    virtual ~Scheduler();
    
    const std::string &getName() const { return name_; }
    
    // 获取当前线程的调度器
    static Scheduler *GetThisScheduler();
    // 获取当前线程的调度协程 (MainFiber)
    static Fiber *GetMainFiber();

    /**
     * @brief 启动调度器
     */
    void start();
    
    /**
     * @brief 停止调度器
     */
    void stop();

    /**
     * @brief [核心接口] 调度协程
     * @param fiber 协程对象
     * @param thread 指定执行线程ID，-1为不指定(负载均衡/窃取)
     * * 场景：Raft Leader 核心循环应指定 thread 以保证 CPU 亲和性
     */
    void schedule(Fiber::ptr fiber, int thread = -1);

    /**
     * @brief [核心接口] 调度回调函数
     * @param cb 回调函数
     * @param thread 指定执行线程ID，-1为不指定
     */
    void schedule(std::function<void()> cb, int thread = -1);

    /**
     * @brief 批量调度 (模板实现，为了方便批量添加)
     */
    template <class InputIterator>
    void schedule(InputIterator begin, InputIterator end) {
        for (auto it = begin; it != end; ++it) {
            schedule(&*it);
        }
    }

protected:
    // 通知调度器有任务到达 (虚函数，IOManager会重写使用pipe)
    virtual void tickle();
    
    // 协程调度主循环
    void run();
    
    // 无任务时执行的 idle 协程 (虚函数，IOManager会重写使用epoll_wait)
    virtual void idle();
    
    // 返回是否可以停止
    virtual bool stopping();
    
    // 设置当前线程调度器
    void setThis();
    
    // 是否有空闲线程
    bool isHasIdleThreads() { return idleThreadCnt_ > 0; }

private:
    /**
     * @brief 调度任务封装 (设为私有内部类)
     * * 细节：既可以存储协程(Fiber)，也可以存储回调函数(std::function)
     * * 修改：将其放入 private，因为外部不需要感知此结构
     */
    struct SchedulerTask {
        Fiber::ptr fiber_;           // 任务对应的协程
        std::function<void()> cb_;   // 任务对应的回调函数
        int thread_;                 // 指定运行的线程ID，-1表示任意线程

        SchedulerTask() { thread_ = -1; }
        SchedulerTask(Fiber::ptr f, int t) : fiber_(f), thread_(t) {}
        SchedulerTask(std::function<void()> f, int t) : cb_(f), thread_(t) {}

        // 清空任务
        void reset() {
            fiber_ = nullptr;
            cb_ = nullptr;
            thread_ = -1;
        }
    };

    /**
     * @brief [Raft优化] 线程局部上下文
     * * 细节：每个线程拥有独立的上下文，包含私有队列和公共队列
     */
    struct ThreadContext {
        // [私有队列]：仅当前线程可访问，用于 Raft Leader 等核心逻辑 (无锁/极低开销)
        // 注意：.h 中定义结构，具体无锁逻辑依赖 run 方法实现
        // 这里为了简化实现，暂统一放在结构体中，逻辑上区分对待
        std::deque<SchedulerTask> private_queue; 

        // [公共队列]：允许被窃取 (Work Stealing)，需加锁
        std::deque<SchedulerTask> public_queue;
        MutexType mutex; // 保护 public_queue
    };

protected:
    // 线程池
    std::vector<Thread::ptr> threadPool_;
    
    // [修改] 替换原有的全局 tasks_，改为每个线程独立的上下文容器
    // 索引对应线程 ID (或映射后的 ID)
    std::vector<ThreadContext*> threadContexts_;

    // 互斥锁 (保护全局 metadata，不再保护任务队列)
    MutexType mutex_;
    
    // 调度器名称
    std::string name_;
    
    // 线程池id数组
    std::vector<int> threadIds_;
    // 工作线程数量
    size_t threadCnt_ = 0;
    // 活跃线程数
    std::atomic<size_t> activeThreadCnt_ = {0};
    // IDLE线程数
    std::atomic<size_t> idleThreadCnt_ = {0};
    
    // 是否是 use caller
    bool isUseCaller_;
    // use caller = true 时，主线程的调度协程
    Fiber::ptr Caller_Schedule_Fiber_;
    // use caller = true 时，主线程的ID
    int rootThread_ = 0;

    // 是否正在停止
    bool stopping_ = false;
};

}  // namespace monsoon

#endif