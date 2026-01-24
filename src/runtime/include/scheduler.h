#ifndef __MONSOON_SCHEDULER_H__
#define __MONSOON_SCHEDULER_H__

#include <atomic>
#include <boost/type_index.hpp>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <iostream> // 补充 cout 支持

#include "fiber.h"
#include "mutex.h"
#include "thread.h"
#include "utils.h"

namespace monsoon {

/**
 * @brief 调度任务封装
 * * 细节：既可以存储协程(Fiber)，也可以存储回调函数(std::function)
 */
class SchedulerTask {
 public:
  friend class Scheduler;
  SchedulerTask() { thread_ = -1; }
  SchedulerTask(Fiber::ptr f, int t) : fiber_(f), thread_(t) {}
  SchedulerTask(Fiber::ptr *f, int t) {
    fiber_.swap(*f);
    thread_ = t;
  }
  SchedulerTask(std::function<void()> f, int t) {
    cb_ = f;
    thread_ = t;
  }
  
  // 清空任务
  void reset() {
    fiber_ = nullptr;
    cb_ = nullptr;
    thread_ = -1;
  }

 private:
  Fiber::ptr fiber_;            // 任务对应的协程
  std::function<void()> cb_;    // 任务对应的回调函数
  int thread_;                  // 指定运行的线程ID，-1表示任意线程
};

/**
 * @brief N-M 协程调度器
 * * 场景：作为 IOManager 的基类，管理线程池和任务队列
 */
class Scheduler {
 public:
  typedef std::shared_ptr<Scheduler> ptr;
  typedef Mutex MutexType; // 统一锁类型定义

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
   * @brief 添加调度任务
   * @tparam TaskType 任务类型
   * @param task 任务
   * @param thread 指定执行线程，-1为不指定
   */
  template <class TaskType>
  void scheduler(TaskType task, int thread = -1) {
    bool isNeedTickle = false;
    {
      MutexType::Lock lock(mutex_);
      isNeedTickle = schedulerNoLock(task, thread);
    }

    if (isNeedTickle) {
      tickle();  // 任务队列从空变非空，唤醒idle协程
    }
  }

  // 启动调度器
  void start();
  // 停止调度器
  void stop();

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

 /* * 修改：将 private 改为 protected
  * 原因：IOManager 继承后需要直接访问 mutex_, tasks_, stopping_ 等成员
  * 以实现高效的 epoll 调度和 idle 逻辑
  */
 protected:
  // 线程池
  std::vector<Thread::ptr> threadPool_;
  // 任务队列
  std::list<SchedulerTask> tasks_;
  // 互斥锁
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
  int rootThread_ = 0; // 改名：Schedule_Fiber_ThreadID -> rootThread_

  // 是否正在停止
  bool stopping_ = false;
  // 是否自动停止 (当任务做完时)
  bool autoStop_ = false; 

 private:
  // 无锁下添加任务，返回是否需要唤醒
  template <class TaskType>
  bool schedulerNoLock(TaskType t, int thread) {
    bool isNeedTickle = tasks_.empty();
    SchedulerTask task(t, thread);
    if (task.fiber_ || task.cb_) {
      tasks_.push_back(task);
    }
    return isNeedTickle;
  }
};

}  // namespace monsoon

#endif