#include "thread.h"
#include <sched.h>  // for sched_setaffinity
#include <stdexcept>
#include "utils.h"  // 假设 GetThreadId 在这里

namespace monsoon {

// thread_local 变量，每个线程都有一份独立的副本，不需要锁，实现 Share-Nothing 的基础
static thread_local Thread *t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOW";

/**
 * @brief 构造函数实现
 * * 场景： 创建 IO 线程或 Worker 线程。
 * * 细节：
 * 1. 初始化成员变量。
 * 2. 调用 pthread_create 创建线程。
 * 3. [关键] m_semaphore.wait() 阻塞等待，直到新线程完成绑核和初始化。
 * 这确保了构造函数返回时，线程状态是确定性的。
 */
Thread::Thread(std::function<void()> cb, const std::string &name, int cpu_id)
    : cb_(cb), name_(name), cpu_id_(cpu_id) {
  if (name.empty()) {
    name_ = "UNKNOW";
  }

  int rt = pthread_create(&thread_, nullptr, &Thread::run, this);
  if (rt) {
    std::cerr << "pthread_create error, name: " << name_ 
              << " return: " << rt << std::endl;
    throw std::logic_error("pthread_create");
  }

  // 等待线程真正运行起来（完成 ID 获取和 CPU 绑定）
  m_semaphore.wait();
}

/**
 * @brief 析构函数
 * * 场景： Thread 对象销毁时（例如 Scheduler 停止）。
 * * 细节： 
 * 如果用户没有显式调用 join，析构函数必须负责 detach，
 * 否则 pthread 资源（栈内存、描述符）不会被内核回收，导致泄漏。
 */
Thread::~Thread() {
  if (thread_) {
    // 如果没有手动 join，析构时 detach，防止资源泄漏
    pthread_detach(thread_);
  }
}

void *Thread::run(void *arg) {
  Thread *thread = (Thread *)arg;
  t_thread = thread;
  t_thread_name = thread->name_;
  
  // 1. 获取线程真实 PID
  thread->id_ = monsoon::GetThreadId();

  // 2. 设置线程名称 (Linux 限制 16 字符)
  pthread_setname_np(pthread_self(), thread->name_.substr(0, 15).c_str());

  // 3. 实现 Thread-per-Core 架构的 CPU 亲和性绑定
  // 消除 Cache 抖动，实现确定性延迟
  if (thread->cpu_id_ != -1) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(thread->cpu_id_, &cpuset);
    int rt = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rt != 0) {
      std::cerr << "pthread_setaffinity_np error, name: " << thread->name_ 
                << " cpu_id: " << thread->cpu_id_ << std::endl;
    }
  }

  std::function<void()> cb;
  cb.swap(thread->cb_);

  // 4. 初始化完成，唤醒构造函数中的等待
  thread->m_semaphore.notify();

  // 5. 执行业务回调
  cb();
  return 0;
}

/**
 * @brief 等待线程结束
 * * 场景： Server 关闭时，主线程等待所有 Worker 线程跑完任务再退出。
 */
void Thread::join() {
  if (thread_) {
    int rt = pthread_join(thread_, nullptr);
    if (rt) {
      std::cerr << "pthread_join error, name: " << name_ 
                << " return: " << rt << std::endl;
      throw std::logic_error("pthread_join");
    }
    thread_ = 0;
  }
}

/**
 * @brief 获取当前线程对象指针
 * * 场景： 在代码深处需要获取当前线程的属性（如 ID）时使用。
 * * 优势： O(1) 复杂度，无锁。
 */
Thread *Thread::GetThis() { 
  return t_thread; 
}

/**
 * @brief 获取当前线程名称
 * * 场景： 日志库在打印 Log 时，自动加上 [Worker_0] 这样的前缀。
 */
const std::string &Thread::GetName() { 
  return t_thread_name; 
}

void Thread::SetName(const std::string &name) {
  if (name.empty()) {
    return;
  }
  if (t_thread) {
    t_thread->name_ = name;
  }
  t_thread_name = name;
}

}  // namespace monsoon