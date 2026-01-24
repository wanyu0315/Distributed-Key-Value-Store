#ifndef __MONSOON_FIBER_H__
#define __MONSOON_FIBER_H__

#include <stdio.h>
#include <ucontext.h>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <memory>
#include "utils.h"
#include "thread.h" // 引用 Thread 以确保线程安全操作

namespace monsoon {

class Scheduler; // 前置声明

class Fiber : public std::enable_shared_from_this<Fiber> {
 public:
  friend class Scheduler; // 允许 Scheduler 访问私有成员
  typedef std::shared_ptr<Fiber> ptr; // 协程智能指针类型定义

  // Fiber状态机
  enum State {
    // 就绪态，刚创建后者yield后状态
    READY,
    // 运行态，resume之后的状态
    RUNNING,
    // 结束态，协程的回调函数执行完之后的状态
    TERM,
    // 异常态，协程的回调函数抛出异常时的状态
    EXCEPT,
  };

 private:
  // 初始化当前线程的协程功能，构造线程主协程对象
  Fiber();

 public:
  /**
   * @brief 构造子协程
   * @param cb 协程执行函数
   * @param stackSz 栈大小
   * @param run_in_scheduler 是否参与调度器调度 (关键参数)
   */
  Fiber(std::function<void()> cb, size_t stackSz = 0, bool run_in_scheduler = true);
  ~Fiber();

  // 重置协程状态，复用栈空间
  void reset(std::function<void()> cb);
  
  // 切换协程到运行态 (执行)
  void resume();
  
  // 让出协程执行权 (挂起)
  void yield();
  
  // 类似于 resume，但专门用于 Caller 线程的主协程切换 (可选语义区分)
  // 在当前实现中，resume 已包含此逻辑，可直接调用 resume
  void call() { resume(); }

  // 获取协程Id
  uint64_t getId() const { return id_; }
  // 获取协程状态
  State getState() const { return state_; }

  // 设置当前正在运行的协程
  static void SetThis(Fiber *f);
  
  // 获取当前线程中的执行线程
  // 如果当前线程没有创建协程，则创建第一个协程，且该协程为当前线程的
  // 主协程，其他协程通过该协程来调度
  static Fiber::ptr GetThis();
  
  // 协程总数
  static uint64_t TotalFiberNum();
  
  // 协程回调函数
  static void MainFunc();
  
  // 获取当前协程Id
  static uint64_t GetCurFiberID();

 private:
  // 协程ID
  uint64_t id_ = 0;
  // 协程栈大小
  uint32_t stackSize_ = 0;
  // 协程状态
  State state_ = READY;
  // 协程上下文
  ucontext_t ctx_;
  // 协程栈地址
  void *stack_ptr = nullptr;
  // 协程回调函数
  std::function<void()> cb_;
  
  // 本协程是否参与调度器调度
  // true: resume/yield 时与 Scheduler::GetMainFiber() 交换上下文
  // false: resume/yield 时与 线程主协程 交换上下文
  bool isRunInScheduler_;
  
  // Valgrind 注册 ID
  uint32_t valgrind_stack_id_ = 0;
};
}  // namespace monsoon

#endif