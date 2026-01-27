#include "timer.h"
#include "utils.h"

namespace monsoon {

// =======================================================
// Timer 类实现
// =======================================================

// 仿函数，专门用于定义 std::set 中定时器的排序规则。它决定了红黑树中哪个定时器排在前面（即最先被触发）。
// 排序规则：
// 树的左侧：时间早的（即将过期的）。
// 树的右侧：时间晚的。
// 节点相同时间：通过内存地址区分，依然挂在树上。
bool Timer::Comparator::operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const {
    if (!lhs && !rhs) return false;
    if (!lhs) return true;
    if (!rhs) return false;
    // 核心逻辑：时间早的排前面
    if (lhs->next_ < rhs->next_) {
        return true;
    }
    if (rhs->next_ < lhs->next_) {
        return false;
    }
    // 时间相同时，比较指针地址，保证 set 能够存储多个时间相同的定时器
    return lhs.get() < rhs.get();
}

// 定时器的初始化入口，负责将用户传入的“相对时间”转换为系统能理解的“绝对时间”
// 存绝对时间（在第10000秒触发）：无论过了多久，只需要用 next_ - 当前时间，就能算出还要等多久。数据本身不需要频繁修改。
Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager *manager)
    : recurring_(recurring), ms_(ms), cb_(cb), manager_(manager) {
    next_ = GetElapsedMS() + ms_;
}

Timer::Timer(uint64_t next) : next_(next) {}

/**
 * @brief 取消定时器
 * 细节：
 * 1. 必须加锁，因为 timers_ 是共享资源。
 * 2. 只有在 timers_ 中找到自己才算成功。
 */
bool Timer::cancel() {
    TimerManager::RWMutexType::WriteLock lock(manager_->mutex_);
    if (cb_) {
        cb_ = nullptr;
        // 用“当前 Timer 自己对应的 shared_ptr”作为 key，在 TimerManager 的 timers_ 容器中查找该 Timer，并得到它在容器中的位置（迭代器）
        auto it = manager_->timers_.find(shared_from_this());
        if (it != manager_->timers_.end()) {
            manager_->timers_.erase(it);  // 从容器中移除该 Timer
        }
        return true;
    }
    return false;
}

/**
 * @brief 刷新定时器
 * 细节：
 * 1. std::set 的 key 是 const 的，不能直接修改 next_。
 * 2. 必须先 erase，更新 next_，再 insert。
 */
bool Timer::refresh() {
    TimerManager::RWMutexType::WriteLock lock(manager_->mutex_);
    if (!cb_) {
        return false;
    }
    
    // 用“当前 Timer 自己对应的 shared_ptr”作为 key，在 TimerManager 的 timers_ 容器中查找该 Timer，并得到它在容器中的位置（迭代器）
    auto it = manager_->timers_.find(shared_from_this());
    if (it == manager_->timers_.end()) {
        return false;
    }
    
    manager_->timers_.erase(it);  // 先从容器中移除该 Timer
    next_ = GetElapsedMS() + ms_; // 更新 next_ 为新的触发时间
    manager_->timers_.insert(shared_from_this()); // 重新插入容器，保持有序性
    return true;
}

/**
 * @brief 重置定时器
 * 细节：
 * 1. 如果使用了 from_now=false，会基于上次的理论触发时间推算，防止误差累积。
 * 2. 重新 addTimer 可能会触发 OnTimerInsertedAtFront，这是必要的。
 */
bool Timer::reset(uint64_t ms, bool from_now) {
    if (ms == ms_ && !from_now) {
        return true;
    }
    
    TimerManager::RWMutexType::WriteLock lock(manager_->mutex_);
    if (!cb_) {
        return false;
    }
    
    // 用“当前 Timer 自己对应的 shared_ptr”作为 key，在 TimerManager 的 timers_ 容器中查找该 Timer，并得到它在容器中的位置（迭代器）
    auto it = manager_->timers_.find(shared_from_this());
    if (it == manager_->timers_.end()) {
        return false;
    }
    
    manager_->timers_.erase(it);
    
    uint64_t start = 0;
    if (from_now) {
        start = GetElapsedMS();
    } else {
        start = next_ - ms_; // 恢复出上次的理论触发时间
    }
    
    ms_ = ms;
    next_ = start + ms_;
    
    manager_->addTimer(shared_from_this(), lock);
    return true;
}

// =======================================================
// TimerManager 类实现
// =======================================================

TimerManager::TimerManager() {
    previousTime_ = GetElapsedMS();
}

TimerManager::~TimerManager() {}

/**
 * @brief 添加定时器
 * 触发场景：
 * 1. 用户希望在未来某个时间点执行任务时调用。
 * 2. 这里是 TimerManager 提供给外部的接口，负责创建 Timer 对象，不需要关心内部Timer的创建细节
 * 细节：
 * 1. 创建 Timer 对象并插入 set。
 * 2. 如果新插入的定时器排在最前面，触发 OnTimerInsertedAtFront()。
 */
Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) {
    Timer::ptr timer(new Timer(ms, cb, recurring, this));
    RWMutexType::WriteLock lock(mutex_);
    addTimer(timer, lock);
    return timer;
}

/**
 * @brief 条件定时器的包装回调
 * * 作用： 在执行前检查 weak_ptr 是否有效。
 */
static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    std::shared_ptr<void> tmp = weak_cond.lock();
    if (tmp) {
        cb();
    }
}

/**
 * @brief 添加条件定时器
 * 使用场景：
 * 1. 避免 shared_ptr 循环引用。
 * 2. 只有当条件对象还存活时，才执行回调。
 * 细节：
 * 1. 使用 std::bind 绑定 OnTimer 和用户回调 cb。
 * 2. OnTimer 内部会检查 weak_ptr 是否过期。
 */
Timer::ptr TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond,
                                           bool recurring) {
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

/**
 * @brief 获取下个超时时间
 * 细节：
 * 1. 加读锁即可。
 * 2. 如果已经超时（now >= next），返回 0，告诉 epoll_wait 立即返回。
 */
uint64_t TimerManager::getNextTimer() {
    RWMutexType::ReadLock lock(mutex_);
    tickled_ = false;
    
    if (timers_.empty()) {
        return ~0ull;
    }
    
    const Timer::ptr &nextTimer = *timers_.begin();  // 获取最早的定时器
    uint64_t now_ms = GetElapsedMS(); // 获取当前启动的毫秒数：系统从启动到当前时刻的毫秒数

    // 计算距离下个定时器触发的时间，如果最早的定时器已经过期，返回0
    if (now_ms >= nextTimer->next_) {
        return 0;
    } else {
        return nextTimer->next_ - now_ms;
    }
}

/**
 * @brief 收集并处理过期定时器
 * * 改进点：
 * 1. 移除了原版代码中不安全的 lower_bound 查找方式。
 * 2. 改为直接遍历迭代器，安全且逻辑清晰。
 */
void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs) {
    uint64_t now_ms = GetElapsedMS(); // 获取当前启动的毫秒数：系统从启动到当前时刻的毫秒数
    std::vector<Timer::ptr> expired;  // 用于存储所有过期的定时器
    
    {
        RWMutexType::WriteLock lock(mutex_);
        if (timers_.empty()) {
            return;
        }
        
        // 检查时钟倒流，如果倒流则判断全部定时器过期
        bool rollover = detectClockRollover(now_ms);
        
        // 如果没倒流，且最早的一个还没到时间，直接返回，不做无用功
        if (!rollover && ((*timers_.begin())->next_ > now_ms)) {
            return;
        }
        
        Timer::ptr now_timer(new Timer(now_ms));
        
        // 查找所有过期定时器
        // 迭代器遍历直到找到一个未过期的
        auto it = timers_.begin();
        if (rollover) {
            // 时间倒流了，为了安全，将所有定时器视为过期
            it = timers_.end();
        } else {
            while (it != timers_.end() && (*it)->next_ <= now_ms) {
                ++it;
            }
        }
        
        // 批量提取并移除
        expired.insert(expired.begin(), timers_.begin(), it); // 提取所有过期定时器到 expired 列表
        timers_.erase(timers_.begin(), it); // 从定时器集合中移除所有过期定时器
        
        cbs.reserve(expired.size());
        
        // 处理提取出来的定时器，放入回调函数列表cbs中
        for (auto &timer : expired) {
            cbs.push_back(timer->cb_);
            if (timer->recurring_) {
                // 循环定时器任务：计算下次时间重新入队
                timer->next_ = now_ms + timer->ms_;
                timers_.insert(timer);
            } else {
                // 一次性定时任务：清理回调
                timer->cb_ = nullptr;
            }
        }
    } // 写锁在此释放
}

/**
 * @brief 添加定时器内部实现
 * * 关键逻辑：
 * 1. timers_.insert 返回迭代器。
 * 2. 只有当新插入的定时器排在最前面 (begin)，才需要唤醒 epoll_wait (OnTimerInsertedAtFront)。
 * 3. 使用 tickled_ 标志位防止频繁无效唤醒。
 */
void TimerManager::addTimer(Timer::ptr val, RWMutexType::WriteLock &lock) {
    auto it = timers_.insert(val).first;  // 向set<Timer::ptr, Timer::Comparator> timers_插入定时器并获取定时器的迭代器
    bool at_front = (it == timers_.begin()) && !tickled_; // 判断是否是最前面的定时器且未被触发过tickle_
    
    if (at_front) {
        tickled_ = true;
    }
    
    lock.unlock(); // 尽早解锁
    
    // 如果是排在最前面的定时器（定时最短），立刻触发唤醒操作
    if (at_front) {
        OnTimerInsertedAtFront();
    }
}

/**
 * @brief 检测时钟回卷
 * 细节：
 * 1. 简单判定：如果当前时间比上次记录的小了 1 小时以上，认为是系统时间被重置了。
 */
bool TimerManager::detectClockRollover(uint64_t now_ms) {
    bool rollover = false;
    if (now_ms < previousTime_ && now_ms < (previousTime_ - 60 * 60 * 1000)) {
        rollover = true;
    }
    previousTime_ = now_ms;
    return rollover;
}

bool TimerManager::hasTimer() {
    RWMutexType::ReadLock lock(mutex_);
    return !timers_.empty();
}

}  // namespace monsoon