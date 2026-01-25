#include "scheduler.h"
#include "fiber.h"
#include "hook.h"
#include <iostream>
#include <algorithm>
#include <cassert>

namespace monsoon {

// =======================================================
// 线程局部变量 (Thread Local Storage)
// =======================================================

// 当前线程绑定的调度器实例
// 作用：允许在任意位置调用 Scheduler::GetThis() 获取当前调度器
static thread_local Scheduler *t_scheduler = nullptr;

// 当前线程正在执行的“主调度协程”
// 作用：当子协程 yield 时，会切回这个协程（即 run 方法执行的上下文）
static thread_local Fiber *t_scheduler_fiber = nullptr;

const std::string LOG_HEAD = "[scheduler] ";

/**
 * @brief 构造函数：初始化调度器
 * @param threads 调度器启动时会创建线程池，threads是线程池包含的线程数量
 * @param use_caller 是否将当前的调用线程（Caller Thread）纳入调度体系
 * @param name 调度器的名称，用于日志和调试
 * * 场景： 在服务器启动时调用，构建协程调度中心。
 * 细节：
 * 1. 如果 use_caller 为 true，当前线程也会被当作一个 "Worker" 线程。
 * 此时需要创建一个 rootFiber，作为当前线程的调度循环执行流。
 * 2. 初始化线程数、名称等基础配置。
 */
Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name)
    : name_(name) {
    assert(threads > 0);

    isUseCaller_ = use_caller;  // 是否将当前的调用线程（Caller Thread）纳入调度体系

    if (use_caller) {
        // 如果当前线程也要参与调度，则实际需要创建的新线程数减 1
        --threads;

        // 1. 初始化当前线程的主协程 (Thread Main Fiber)
        // 确保当前线程有一个 Fiber 上下文，否则无法创建子协程
        Fiber::GetThis(); // 如果当前线程没有协程，会创建主协程

        // 2. 绑定当前线程的调度器为自己
        t_scheduler = this;

        // 3. 创建 Caller 线程的调度协程 (rootFiber)
        // 注意：这个协程执行的是 Scheduler::run 方法。
        // 当 run 方法 yield 时，会切回当前线程的主协程 (即 main 函数原本的执行流)
        Caller_Schedule_Fiber_.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true)); // run 方法作为协程入口函数
        
        Thread::SetName(name_);
        
        // 设置当前线程（主线程）的调度协程
        t_scheduler_fiber = Caller_Schedule_Fiber_.get(); // 把创建的调度协程指针存到线程局部变量里，t_scheduler_fiber就指向这个调度协程了
        rootThread_ = GetThreadId();
        threadIds_.push_back(rootThread_);
    } else {
        rootThread_ = -1;
    }
    threadCnt_ = threads;   // start()时，创建的线程池包含的线程数量
    // std::cout << LOG_HEAD << "Constructed: " << name_ << " success" << std::endl;
}

/**
 * @brief 析构函数：销毁调度器
 * * 场景： 服务器关闭或模块卸载时。
 * 细节：
 * 1. 必须确保 stop() 已经被调用且完全停止，否则直接析构是不安全的。
 * 2. 清理线程局部变量 t_scheduler。
 */
Scheduler::~Scheduler() {
    assert(stopping_); // 强制要求必须先停止才能析构
    if (GetThisScheduler() == this) {
        t_scheduler = nullptr;
    }
}

/**
 * @brief 获取当前线程绑定的调度器
 * @return Scheduler* 指向当前调度器的指针
 */
Scheduler *Scheduler::GetThisScheduler() { 
    return t_scheduler; 
}

/**
 * @brief 获取当前线程的调度协程
 * @return Fiber* 调度协程指针 (即运行 run() 方法的协程)
 */
Fiber *Scheduler::GetMainFiber() { 
    return t_scheduler_fiber; 
}

/**
 * @brief 设置当前线程的调度器
 * 场景： 在 run() 方法开始时调用，标记当前线程归属。
 */
void Scheduler::setThis() { 
    t_scheduler = this; 
}

/**
 * @brief 启动调度器
 * * 场景： 初始化完成后，开始创建线程池并运行。
 * 细节：
 * 1. 锁保护，防止重复启动。
 * 2. 创建指定数量的 Thread，每个线程都绑定运行 Scheduler::run()。
 * 3. 将线程 ID 记录到 threadIds_ 数组中。
 */
void Scheduler::start() {
    Mutex::Lock lock(mutex_);
    if (!stopping_) {
        // 已经启动，直接返回
        return;
    }
    stopping_ = false; // 复位停止标记
    
    assert(threadPool_.empty());
    threadPool_.resize(threadCnt_);
    
    for (size_t i = 0; i < threadCnt_; ++i) {
        // 创建新线程，入口函数为 Scheduler::run
        threadPool_[i].reset(new Thread(std::bind(&Scheduler::run, this), 
                                        name_ + "_" + std::to_string(i)));
        threadIds_.push_back(threadPool_[i]->getId());
    }
    std::cout << LOG_HEAD << "Start success, threads: " << threadCnt_ << std::endl;
}

/**
 * @brief 停止调度器 (优雅关闭)
 * * 场景： 程序退出或服务停止时。
 * 细节：
 * 1. 优雅关闭的关键：不仅仅是设置 flag，还要保证所有已提交的任务被执行完毕。
 * 2. use_caller 模式下，Caller 线程需要参与 "收尾工作" (执行 run)。
 * 3. 唤醒所有处于 idle 状态的线程，让它们检测到 stopping 状态并退出。
 */
void Scheduler::stop() {
    std::cout << LOG_HEAD << "Stop initiated" << std::endl;
    
    if (stopping()) {
        return;
    }
    
    // 设置停止状态：为 true 时，run 循环在处理完所有任务后将退出
    stopping_ = true;

    // 场景检查：stop指令只能由caller线程发起 (如果使用了caller模式)
    if (isUseCaller_) {
        assert(GetThisScheduler() == this);
    } else {
        assert(GetThisScheduler() != this);
    }

    // 唤醒所有等待任务的线程 (tickle)
    for (size_t i = 0; i < threadCnt_; ++i) {
        tickle();
    }
    if (Caller_Schedule_Fiber_) {
        tickle();
    }

    // use_caller 模式特判：
    // 如果当前线程就是 Caller 线程，它需要亲自切入调度循环来协助清理剩余任务
    if (Caller_Schedule_Fiber_) {
        if (!stopping()) {
            // 切入 Caller_Schedule_Fiber_ 执行 run()，直到满足 stopping() 条件
            Caller_Schedule_Fiber_->call(); // 使用 call 或 resume 均可，call 更语义化
        }
    }

    // 等待所有子线程执行完毕 (join)
    std::vector<Thread::ptr> threads;
    {
        Mutex::Lock lock(mutex_);
        threads.swap(threadPool_); // 把成员变量 threadPool_ 里的线程指针转移到局部变量 threads 中。为了尽快释放 mutex_ 锁。
    }

    for (auto &t : threads) {
        t->join();
    }
    std::cout << LOG_HEAD << "Stopped gracefully" << std::endl;
}

/**
 * @brief 核心调度循环
 * * 场景： 每一个参与调度的线程（包括 Worker 线程和 Caller 线程）的入口函数。
 * 细节：
 * 1. 这是一个死循环，直到 stopping() 返回 true。
 * 2. 状态机逻辑：
 * - 有任务 -> 执行任务 (resume)。
 * - 无任务 -> 执行 Idle 协程 (yield + wait)。
 * 3. 实现了对象复用：对于函数回调 (cb) 类型的任务，复用 cb_fiber，避免重复内存分配。
 */
void Scheduler::run() {
    std::cout << LOG_HEAD << "Run begin in thread: " << GetThreadId() << std::endl;
    
    // 启用 Hook，确保 socket IO 操作能自动切换协程
    set_hook_enable(true);
    
    setThis(); // 绑定当前线程的调度器为自己
    
    // 初始化 thread_local 的调度协程指针
    if (GetThreadId() != rootThread_) {
        t_scheduler_fiber = Fiber::GetThis().get();
    }

    // 创建 Idle 协程：当没有任务时，运行这个协程进行休眠
    // 由于虚函数，且实际中只需要实例化 IOManager，因此实际执行的是 IOManager::idle
    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
    
    // 创建回调协程容器：用于执行 std::function 类型的任务
    Fiber::ptr cb_fiber;

    SchedulerTask task; // 临时承载取出的任务

    while (true) {
        task.reset();
        bool tickle_other_thread = false; // 是否需要唤醒其他线程
        bool is_active = false; // 本轮是否处理了任务

        // --- 临界区：从任务队列取任务 ---
        {
            Mutex::Lock lock(mutex_);
            auto it = tasks_.begin();
            while (it != tasks_.end()) {
                // 如果任务指定了特定线程执行，且不是当前线程 -> 跳过，并通知其他线程
                if (it->thread_ != -1 && it->thread_ != GetThreadId()) {
                    tickle_other_thread = true;
                    ++it;
                    continue;
                }

                // 确保任务有效
                assert(it->fiber_ || it->cb_);
                
                // 如果是协程任务，且正在执行中 -> 跳过 (不应发生，除非逻辑错误)
                if (it->fiber_ && it->fiber_->getState() == Fiber::RUNNING) {
                    ++it;
                    continue;
                }

                // 找到可用任务，取出
                task = *it;
                tasks_.erase(it);
                ++activeThreadCnt_; // 活跃线程数 +1
                is_active = true;
                break;
            }
            // 如果取走一个任务后队列仍不为空，唤醒其他线程分担压力
            tickle_other_thread |= (it != tasks_.end());
        }

        // 调用的字面上是 tickle()，但实际运行时，执行的是 IOManager::tickle()，通过 C++ 的 虚函数（Virtual Function） 机制实现的。
        if (tickle_other_thread) {
            tickle();
        }

        // --- 使用这个任务（task）持有的协程执行任务逻辑 ---
        if (task.fiber_ && (task.fiber_->getState() != Fiber::TERM && task.fiber_->getState() != Fiber::EXCEPT)) {
            // Case 1: 这是一个协程任务 (Fiber)
            task.fiber_->resume();  // 切入这个任务对应的协程执行任务
            --activeThreadCnt_; // 任务执行完毕 (或 yield)
            task.reset();       // 释放引用
        
        } else if (task.cb_) {
            // Case 2: 这是一个函数回调任务 (Function Callback)
            if (cb_fiber) {
                // [优化] 复用已有的协程对象，仅替换底层的函数指针
                // 避免了重复的 Alloc/mmap 开销
                cb_fiber->reset(task.cb_);
            } else {
                cb_fiber.reset(new Fiber(task.cb_));
            }
            task.reset();
            
            cb_fiber->resume();
            --activeThreadCnt_;
            
            // 只有当复用的协程彻底结束或异常时，才重置智能指针
            if(cb_fiber->getState() == Fiber::TERM || cb_fiber->getState() == Fiber::EXCEPT) {
                 cb_fiber->reset(nullptr); 
            }
        
        } else {
            // Case 3: 此时没有拿到任务
            
            // 再次检查 idle 协程状态，如果它结束了，说明调度器该退出了
            if (idle_fiber->getState() == Fiber::TERM) {
                std::cout << LOG_HEAD << "Idle fiber term, loop exit" << std::endl;
                break;
            }

            // 切换到 idle 协程进行等待 (会挂起当前线程)
            ++idleThreadCnt_;
            idle_fiber->resume();   
            --idleThreadCnt_;
        }
    }
    std::cout << LOG_HEAD << "Run exit in thread: " << GetThreadId() << std::endl;
}

/**
 * @brief 唤醒线程 (Tickle)
 * * 场景： 当有新任务加入，或者需要停止调度器时调用。
 * 细节：
 * 1. 在基类 Scheduler 中，由于不知道具体的唤醒机制（Condition Variable 还是 Pipe），
 * 此处留空或仅打印日志。
 * 2. 实际的唤醒逻辑由子类（如 IOManager）重写实现。
 */
void Scheduler::tickle() { 
    // std::cout << LOG_HEAD << "tickle (base)" << std::endl;
}

/**
 * @brief 检查是否可以停止
 * @return true 可以停止, false 不能停止
 * * 场景： stop() 和 idle() 中判断退出条件。
 * 细节：
 * 停止的硬性条件：
 * 1. stopping_ 标记为 true (由 stop() 触发)。
 * 2. 任务队列 tasks_ 为空。
 * 3. activeThreadCnt_(活跃线程数) 为 0 (所有任务都跑完了)。
 */
bool Scheduler::stopping() {
    Mutex::Lock lock(mutex_);
    return stopping_ && tasks_.empty() && activeThreadCnt_ == 0;
}

/**
 * @brief 空闲协程函数
 * * 场景： 当 run() 循环中没有任务可做时，切入此协程。
 * 细节：
 * 1. 这是一个死循环，只要调度器没停止，它就负责 "等待"。
 * 2. [注意] 基类 scheduler 不支持 epoll，只能通过 Yield 忙等待来模拟阻塞。
 * 真正的 IOManager 会重写此方法，使用 epoll_wait 挂起线程。
 */
void Scheduler::idle() {
    std::cout << LOG_HEAD << "Enter idle" << std::endl;
    while (true) {
        if (stopping()) {
            std::cout << LOG_HEAD << "Scheduler stopping, idle exit" << std::endl;
            break;
        }
        
        // 由于移除了条件变量，基类只能通过 yield 让出 CPU 权限
        // 子类 IOManager 会在此处使用 epoll_wait 真正阻塞线程
        Fiber::GetThis()->yield();
    }
}

} // namespace monsoon