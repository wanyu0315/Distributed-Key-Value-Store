#ifndef __MONSOON_THREAD_H_
#define __MONSOON_THREAD_H_

#include <pthread.h>
#include <semaphore.h>  // 用于线程启动同步
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace monsoon {

/**
 * @brief 简单的信号量封装，用于线程启动同步
 * * 细节：
 * 1. 封装 sem_init, sem_wait, sem_post。
 * 2. 用于保证 Thread 构造函数返回前，线程已经运行并完成了初始化（如绑核）。
 */
class Semaphore {
 public:
  Semaphore(uint32_t count = 0) {
    sem_init(&m_sem, 0, count);
  }
  ~Semaphore() {
    sem_destroy(&m_sem);
  }

  void wait() {
    sem_wait(&m_sem);
  }

  void notify() {
    sem_post(&m_sem);
  }

 private:
  sem_t m_sem;
};

/**
 * @brief 线程封装类
 * * 场景：作为协程调度器(Scheduler)的底层载体，实现 Thread-per-Core 架构。
 * * 简历对应：
 * 1. 支持 CPU 亲和性绑定 (CPU Affinity)，减少 Cache 抖动。
 * 2. 提供确定性的线程启动机制。
 */
class Thread {
 public:
  typedef std::shared_ptr<Thread> ptr;

  /**
   * @brief 构造函数，启动线程
   * @param cb 线程执行的回调函数
   * @param name 线程名称
   * @param cpu_id 绑定的 CPU 核心 ID (-1 表示不绑定)
   * * 细节：
   * 1. 创建 pthread。
   * 2. 使用信号量等待线程完全启动（包括绑核成功）后才返回。
   */
  Thread(std::function<void()> cb, const std::string &name, int cpu_id = -1);

  /**
   * @brief 析构函数
   * * 细节：
   * 1. 如果线程还在运行且未 join，则 detach，释放内核资源。
   */
  ~Thread();

  pid_t getId() const { return id_; }
  const std::string &getName() const { return name_; }

  /**
   * @brief 等待线程结束
   * * 场景： 优雅退出 Scheduler 时使用。
   */
  void join();

  /**
   * @brief 获取当前线程指针
   * @return Thread* 当前线程对象的指针
   * * 注意：如果是主线程（非 Thread 类创建的），可能返回 nullptr。
   */
  static Thread *GetThis();

  /**
   * @brief 获取当前线程名称
   */
  static const std::string &GetName();

  /**
   * @brief 设置当前线程名称
   * @param name 新名称
   */
  static void SetName(const std::string &name);

 private:
  // 禁止拷贝
  Thread(const Thread &) = delete;
  Thread(const Thread &&) = delete;
  Thread operator=(const Thread &) = delete;

  /**
   * @brief 线程执行入口函数
   * @param args Thread 对象指针
   */
  static void *run(void *args);

 private:
  pid_t id_ = -1;              // 线程真实 PID (gettid)
  pthread_t thread_ = 0;       // pthread 句柄
  std::function<void()> cb_;   // 线程回调
  std::string name_;           // 线程名
  int cpu_id_ = -1;            // 绑定的 CPU 核心
  Semaphore m_semaphore;       // 启动同步信号量
};

}  // namespace monsoon

#endif