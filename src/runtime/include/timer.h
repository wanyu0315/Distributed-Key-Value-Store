#ifndef __MONSOON_TIMER_H__
#define __MONSOON_TIMER_H__

#include <memory>
#include <set>
#include <vector>
#include <functional>
#include "mutex.h"

namespace monsoon {

class TimerManager;

/**
 * @brief 定时器类
 * * 作用： 封装单个定时任务的元数据（执行时间、周期、回调等）。
 * 细节：
 * 1. 继承 enable_shared_from_this 以便在 TimerManager 中安全管理生命周期。
 * 2. 构造函数私有化，强制通过 TimerManager 创建。
 */
class Timer : public std::enable_shared_from_this<Timer> {
    friend class TimerManager;

public:
    typedef std::shared_ptr<Timer> ptr;

    /**
     * @brief 取消定时器
     * * 场景： 任务不再需要执行时调用。
     * 细节：
     * 1. 加锁从管理器中移除自身。
     * 2. 置空回调函数防止循环引用。
     */
    bool cancel();

    /**
     * @brief 刷新定时器
     * * 场景： 类似于看门狗机制，重置执行时间。
     * 细节：
     * 1. 将执行时间更新为：当前时间 + 周期 (ms_)。
     * 2. 在 TimerManager 的 set 中重新排序。
     */
    bool refresh();

    /**
     * @brief 重置定时器执行间隔
     * @param[in] ms 新的间隔时间
     * @param[in] from_now 是否从当前时刻开始计算（true: 当前+ms, false: 上次执行时间+ms）
     * * 场景： 动态调整任务频率。
     */
    bool reset(uint64_t ms, bool from_now);

private:
    /**
     * @brief 私有构造函数
     * * 作用： 仅供 TimerManager 调用。
     */
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager *manager);

    /**
     * @brief 仅用于比较的构造函数
     * * 作用： 用于 set 查找时的临时对象构建。
     */
    Timer(uint64_t next);

private:
    /// 是否是循环定时器
    bool recurring_ = false;
    /// 执行周期(ms)
    uint64_t ms_ = 0;
    /// 精确的下一次执行时间戳(ms)
    uint64_t next_ = 0;
    /// 回调函数
    std::function<void()> cb_;
    /// 管理器指针
    TimerManager *manager_ = nullptr;

private:
    /**
     * @brief 定时器比较仿函数
     * * 作用： 定义 std::set 的排序规则。
     * 细节：
     * 1. 优先比较 next_ (执行时间)，时间越早越靠前。
     * 2. 时间相同时，比较对象地址，确保 set 能存储多个同一时刻触发的定时器。
     */
    struct Comparator {
        bool operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const;
    };
};

/**
 * @brief 定时器管理器
 * * 场景： 作为 IOManager 的基类，赋予调度器处理定时任务的能力。
 * 细节：
 * 1. 维护一个按时间排序的定时器集合。
 * 2. 提供获取最近超时时间的方法，供 epoll_wait 使用。
 */
class TimerManager {
    friend class Timer;

public:
    typedef RWMutex RWMutexType;

    TimerManager();
    virtual ~TimerManager();

    /**
     * @brief 添加定时器
     * @param[in] ms 执行间隔
     * @param[in] cb 回调函数
     * @param[in] recurring 是否循环执行
     * * 细节：
     * 1. 创建 Timer 对象并插入 set。
     * 2. 如果新插入的定时器排在最前面，触发 OnTimerInsertedAtFront()。
     */
    Timer::ptr addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

    /**
     * @brief 添加条件定时器
     * @param[in] weak_cond 条件变量（弱引用）
     * * 作用： 解决 shared_ptr 循环引用问题。只有当 weak_cond 指向的对象还存活时，回调才执行。
     */
    Timer::ptr addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond,
                                 bool recurring = false);

    /**
     * @brief 获取距离下一次定时器触发的时间间隔
     * * 场景： 用于 epoll_wait 的 timeout 参数。
     * 细节：
     * 1. 如果没有定时器，返回 ~0ull。
     * 2. 如果有定时器且已过期，返回 0。
     * 3. 否则返回 (next_time - now_time)。
     */
    uint64_t getNextTimer();

    /**
     * @brief 获取所有已过期的定时器回调
     * @param[out] cbs 用于接收回调函数的数组
     * * 场景： 工作线程从 epoll_wait 醒来后调用。
     * 细节：
     * 1. 检测系统时间是否回卷（Rollover）。
     * 2. 将所有 (next_ <= now) 的定时器取出。
     * 3. 如果是循环定时器，重新计算时间放回 set；否则删除。
     */
    void listExpiredCb(std::vector<std::function<void()>> &cbs);

    /**
     * @brief 检查是否有定时器
     */
    bool hasTimer();

protected:
    /**
     * @brief 当有新的最早定时器加入时调用
     * * 场景： 纯虚函数，由 IOManager 实现。
     * 作用： 写入 Tickle 管道，唤醒正在 epoll_wait 的线程，使其重新计算超时时间。
     */
    virtual void OnTimerInsertedAtFront() = 0;

    /**
     * @brief 添加定时器的底层实现
     * * 细节： 复用锁逻辑，避免死锁。
     */
    void addTimer(Timer::ptr val, RWMutexType::WriteLock &lock);

private:
    /**
     * @brief 检测系统时间是否倒流
     * * 场景： 系统校时可能导致时间骤减。
     */
    bool detectClockRollover(uint64_t now_ms);

private:
    /// 读写锁
    RWMutexType mutex_;
    /// set（红黑树）定时器集合 (有序)
    std::set<Timer::ptr, Timer::Comparator> timers_;
    /// 是否已经触发过 Tickle (优化，避免重复唤醒)
    bool tickled_ = false;
    /// 上次检查的时间戳
    uint64_t previousTime_ = 0;
};

}  // namespace monsoon

#endif