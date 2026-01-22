#include "fiber.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <unistd.h>     // for sysconf
#include <sys/mman.h>   // for mmap, mprotect, munmap
#include "scheduler.h" 

// 引入 Valgrind 头文件以支持内存分析
#if __has_include(<valgrind/valgrind.h>)
    #include <valgrind/valgrind.h>
    #define HAS_VALGRIND 1
#else
    #define HAS_VALGRIND 0
    #define VALGRIND_STACK_REGISTER(start, end) 0
    #define VALGRIND_STACK_DEREGISTER(id) do {} while(0)
#endif

namespace monsoon {

// =======================================================
// 全局/线程局部变量 (Global & TLS)
// =======================================================

// 当前线程正在运行的协程
static thread_local Fiber* t_fiber = nullptr;

// 当前线程的主协程 (Thread Main Fiber)
// 每个线程启动时，会隐式创建一个主协程，用于保存线程原本的上下文
static thread_local Fiber::ptr t_threadFiber = nullptr;

// 全局协程 ID 生成器
static std::atomic<uint64_t> s_fiber_id{0};

// 全局协程计数器
static std::atomic<uint64_t> s_fiber_count{0};

// 默认栈大小: 128KB
static uint32_t g_fiber_stack_size = 128 * 1024;

// =======================================================
// 内存管理模块 (Memory Management) - Modified for mmap
// =======================================================

class StackAllocator {
public:
    /**
     * @brief 使用 mmap 分配栈内存
     * 1. 使用 mmap 实现 Lazy Allocation (虚拟内存很大，物理内存按需分配)
     * 2. 在低地址端设置 Guard Page (保护页)
     */
    static void* Alloc(size_t size) {
        // 获取系统页大小 (通常 4KB)
        size_t page_size = sysconf(_SC_PAGESIZE);
        
        // 实际申请大小 = 用户需求 + 1个保护页
        size_t real_size = size + page_size;

        // 使用 mmap 申请匿名私有映射
        // PROT_READ | PROT_WRITE: 可读写
        // MAP_PRIVATE | MAP_ANONYMOUS: 私有匿名内存(不关联文件)
        char* base = (char*)mmap(nullptr, real_size, 
                                 PROT_READ | PROT_WRITE, 
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (base == MAP_FAILED) {
            std::cerr << "StackAllocator::Alloc mmap failed" << std::endl;
            return nullptr;
        }

        // 设置 Guard Page: 将最低地址的一页设为不可读写 (PROT_NONE)
        // 这样如果栈溢出，程序会收到 SIGSEGV，便于调试，而不是静默破坏堆内存
        if (mprotect(base, page_size, PROT_NONE) != 0) {
            std::cerr << "StackAllocator::Alloc mprotect failed" << std::endl;
            munmap(base, real_size); // 失败则回滚
            return nullptr;
        }

        // 返回的栈底指针需要跳过保护页
        return base + page_size;
    }

    static void Dealloc(void* vp, size_t size) {
        if (!vp) return;
        
        size_t page_size = sysconf(_SC_PAGESIZE);
        // 计算回原始的 mmap 起始地址 (减去保护页偏移)
        char* start = (char*)vp - page_size;
        size_t real_size = size + page_size;

        // 释放整个内存块
        munmap(start, real_size);
    }
};

// =======================================================
// 协程核心实现 (Fiber Implementation)
// =======================================================

/**
 * @brief 获取当前协程
 * 如果当前线程没有协程，则创建主协程
 */
Fiber::ptr Fiber::GetThis() {
    if (t_fiber) {
        return t_fiber->shared_from_this();
    }
    // 创建主协程 (无参构造)
    Fiber::ptr main_fiber(new Fiber);
    assert(t_fiber == main_fiber.get());
    t_threadFiber = main_fiber;
    return t_fiber->shared_from_this();
}

/**
 * @brief 设置当前协程
 */
void Fiber::SetThis(Fiber* f) {
    t_fiber = f;
}

/**
 * @brief 获取协程总数
 */
uint64_t Fiber::TotalFiberNum() {
    return s_fiber_count;
}

/**
 * @brief 协程入口函数 (Trampoline)
 * 用于封装用户回调，处理异常及生命周期
 * 当调度器切换到一个新协程时，CPU 实际上是从这个函数开始执行的。它的核心任务是“包裹”用户的业务逻辑。
 */
void Fiber::MainFunc() {
    // 获取当前协程的智能指针，增加引用计数，防止执行期间被销毁
    Fiber::ptr cur = GetThis();
    assert(cur);

    try {
        cur->cb_(); // 执行用户任务
        cur->cb_ = nullptr;
        cur->state_ = TERM;
    } catch (std::exception& e) {
        cur->state_ = EXCEPT;
        std::cerr << "Fiber Except: " << e.what() << " fiber_id=" << cur->getId() << std::endl;
    } catch (...) {
        cur->state_ = EXCEPT;
        std::cerr << "Fiber Except: Unknown fiber_id=" << cur->getId() << std::endl;
    }

    // 任务结束，切出协程
    // 注意：不能使用 cur->yield()，因为 cur 是智能指针，这里需要裸指针操作
    // 并且需要让出引用计数
    auto raw_ptr = cur.get();   // 1.获取裸指针
    cur.reset(); // 2.释放 shared_ptr，引用计数 -1
    raw_ptr->yield(); // 3.切回调度器或主线程
}

// -------------------------------------------------------
// 构造与析构
// -------------------------------------------------------

/**
 * @brief 私有构造：创建线程主协程
 * 场景： 当一个线程（Thread）首次启用协程功能时调用。
 * 主协程不需要分配栈，因为它使用当前线程的栈
 */
Fiber::Fiber() {
    state_ = RUNNING;
    SetThis(this);

    // 获取当前上下文 (保存到 ctx_)
    if (getcontext(&ctx_) == -1) {
        assert(false && "getcontext error");
    }

    ++s_fiber_count;
    id_ = s_fiber_id++;
    // std::cout << "[Fiber] Main fiber created, id=" << id_ << std::endl;
}

/**
 * @brief 公有构造：创建子协程
 * 场景： 用户创建一个并发任务时调用。
 * 分配独立栈空间，绑定 MainFunc
 */
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_inscheduler)
    : id_(s_fiber_id++), cb_(cb), isRunInScheduler_(run_inscheduler) {
    ++s_fiber_count;    // 子协程数量+1
    stackSize_ = stacksize > 0 ? stacksize : g_fiber_stack_size;    // 设置栈大小
    
    // 1. 分配栈内存 (使用 mmap)
    stack_ptr = StackAllocator::Alloc(stackSize_);

    // 2. 注册到 Valgrind (Added Feature)
    // 告诉 Valgrind 这块堆内存被当作栈使用，避免未初始化内存误报
#if HAS_VALGRIND
    valgrind_stack_id_ = VALGRIND_STACK_REGISTER(stack_ptr, (char*)stack_ptr + stackSize_);
#endif

    // 3. 获取当前上下文作为模板
    if (getcontext(&ctx_) == -1) {
        assert(false && "getcontext error");
    }

    // 4. 修改上下文
    ctx_.uc_link = nullptr;          // 执行完后不自动跳转，由 MainFunc 手动 yield
    ctx_.uc_stack.ss_sp = stack_ptr; // 设置栈顶
    ctx_.uc_stack.ss_size = stackSize_; // 将指令指针(IP)指向 MainFunc

    // 5. 绑定入口函数
    makecontext(&ctx_, &Fiber::MainFunc, 0);
}

Fiber::~Fiber() {
    --s_fiber_count;
    // 如果有栈，说明是子协程
    if (stack_ptr) {
        // 子协程析构：确保状态为 TERM 或 EXCEPT (未执行完析构是危险的)
        assert(state_ == TERM || state_ == EXCEPT || state_ == READY); 

        // 1. 从 Valgrind 注销 (Added Feature)
#if HAS_VALGRIND
        if (valgrind_stack_id_ > 0) {
            VALGRIND_STACK_DEREGISTER(valgrind_stack_id_);
        }
#endif
        // 2. 释放内存
        StackAllocator::Dealloc(stack_ptr, stackSize_);
    } else {
        // 主协程析构：确保当前协程就是自己 (逻辑正确性检查)
        assert(!cb_);
        assert(state_ == RUNNING);
        
        Fiber* cur = t_fiber;
        if (cur == this) {
            SetThis(nullptr);
        }
    }
}

// -------------------------------------------------------
// 核心调度原语：Resume & Yield
// -------------------------------------------------------

/**
 * @brief 换入：激活当前协程，开始（或继续）执行。
 * * 逻辑分支：
 * 1. 如果该协程参与调度器调度 (isRunInScheduler_):
 * 与 调度器主协程 (Scheduler::GetMainFiber()) 交换上下文。
 * 2. 如果该协程是线程独立协程 (!isRunInScheduler_):
 * 与 线程主协程 (t_threadFiber) 交换上下文。
 */
void Fiber::resume() {
    assert(state_ != TERM && state_ != RUNNING);
    SetThis(this);
    state_ = RUNNING;

    if (isRunInScheduler_) {
        // 核心：保存调度器的上下文，恢复本协程的上下文
        if (swapcontext(&(Scheduler::GetMainFiber()->ctx_), &ctx_) == -1) {
            assert(false && "swapcontext error (scheduler->fiber)");
        }
    } else {
        // 保存线程主协程上下文，恢复本协程上下文
        if (swapcontext(&(t_threadFiber->ctx_), &ctx_) == -1) {
            assert(false && "swapcontext error (thread->fiber)");
        }
    }
}

/**
 * @brief 换出：挂起当前协程，让出 CPU 执行权。
 * * 逻辑分支：
 * 1. 参与调度器：切回 调度器主协程。
 * 2. 独立协程：切回 线程主协程。
 */
void Fiber::yield() {
    assert(state_ == RUNNING || state_ == TERM);
    
    // 切出时，需要恢复当前线程的全局指针指向“调用者”
    if (isRunInScheduler_) {
        SetThis(Scheduler::GetMainFiber()); // 告诉系统：现在是调度器在掌管 CPU 了
    } else {
        SetThis(t_threadFiber.get());
    }

    if (state_ != TERM && state_ != EXCEPT) {
        state_ = READY;     // 标记：我还没写完，只是“就绪”等待下次被叫
    }

    if (isRunInScheduler_) {
        // 核心：保存本协程上下文，恢复调度器上下文
        // 1. 把我现在的状态（&ctx_）冻结保存。
        // 2. 把调度员的状态（Scheduler::ctx_）解冻加载。
        // 这一行代码执行完，CPU 就瞬间“传送”回了 Scheduler::run() 的循环里。
        if (swapcontext(&ctx_, &(Scheduler::GetMainFiber()->ctx_)) == -1) {
            assert(false && "swapcontext error (fiber->scheduler)");
        }
    } else {
        if (swapcontext(&ctx_, &(t_threadFiber->ctx_)) == -1) {
            assert(false && "swapcontext error (fiber->thread)");
        }
    }
}

/**
 * @brief 重置协程 (对象池复用)
 * 让一个已经结束的协程对象焕发新生，去执行新的任务
 */
void Fiber::reset(std::function<void()> cb) {
    assert(stack_ptr);
    assert(state_ == TERM || state_ == EXCEPT || state_ == READY); // 允许 Ready 状态重置(针对未执行的任务)
    
    cb_ = cb;   // 绑定新的任务函数
    
    // getcontext + makecontext 是 Linux 的“魔法组合”
    // 它们并没有分配新内存，而是修改了现有的 ctx_ 结构体
    // 告诉它：下次被激活时，指令指针(IP) 指向 MainFunc，栈顶指针(SP) 指向 stack_ptr
    if (getcontext(&ctx_) == -1) {
        assert(false && "getcontext error");
    }

    ctx_.uc_link = nullptr;
    ctx_.uc_stack.ss_sp = stack_ptr;
    ctx_.uc_stack.ss_size = stackSize_;

    makecontext(&ctx_, &Fiber::MainFunc, 0);
    state_ = READY;
}

} // namespace monsoon