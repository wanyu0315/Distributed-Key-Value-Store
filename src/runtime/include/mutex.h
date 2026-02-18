#ifndef __MONSOON_MUTEX_H_
#define __MONSOON_MUTEX_H_

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <stdexcept>

#include "noncopyable.h"
#include "util.h" // 确保包含 util.h 以使用 CondPanic

namespace monsoon {

/**
 * @brief 信号量封装
 * * 场景： 线程间同步，例如 Thread 类中等待线程初始化完成。
 */
class Semaphore : Nonecopyable {
 public:
  /**
   * @brief 构造函数
   * @param count 初始计数
   */
  Semaphore(uint32_t count = 0) {
    if (sem_init(&m_semaphore, 0, count)) {
      throw std::logic_error("sem_init error");
    }
  }

  /**
   * @brief 析构函数
   */
  ~Semaphore() {
    sem_destroy(&m_semaphore);
  }

  /**
   * @brief 等待信号量 (P操作)
   */
  void wait() {
    if (sem_wait(&m_semaphore)) {
      throw std::logic_error("sem_wait error");
    }
  }

  /**
   * @brief 发送信号量 (V操作)
   */
  void notify() {
    if (sem_post(&m_semaphore)) {
      throw std::logic_error("sem_post error");
    }
  }

 private:
  sem_t m_semaphore;
};

/**
 * @brief 局部锁 RAII 模板 (Scoped Lock)
 * @tparam T 锁类型
 * * 场景： 自动管理锁的生命周期，防止异常导致死锁。
 */
template <class T>
struct ScopedLockImpl {
 public:
  /**
   * @brief 构造时自动上锁
   * @param mutex 锁对象的引用
   */
  ScopedLockImpl(T &mutex) : m_mutex(mutex) {
    m_mutex.lock();
    m_locked = true;
  }

  /**
   * @brief 析构时自动解锁
   */
  ~ScopedLockImpl() {
    unlock();
  }

  /**
   * @brief 手动上锁
   * * 注意：通常不需要手动调用，构造函数已处理
   */
  void lock() {
    if (!m_locked) {
      m_mutex.lock();
      m_locked = true;
    }
  }

  /**
   * @brief 手动解锁
   * * 场景： 需要提前释放锁以缩小临界区时使用
   */
  void unlock() {
    if (m_locked) {
      m_mutex.unlock();
      m_locked = false;
    }
  }

 private:
  T &m_mutex;
  bool m_locked;
};

/**
 * @brief 局部读锁 RAII 模板
 */
template <class T>
struct ReadScopedLockImpl {
 public:
  ReadScopedLockImpl(T &mutex) : m_mutex(mutex) {
    m_mutex.rdlock();
    m_locked = true;
  }

  ~ReadScopedLockImpl() {
    unlock();
  }

  void lock() {
    if (!m_locked) {
      m_mutex.rdlock();
      m_locked = true;
    }
  }

  void unlock() {
    if (m_locked) {
      m_mutex.unlock();
      m_locked = false;
    }
  }

 private:
  T &m_mutex;
  bool m_locked;
};

/**
 * @brief 局部写锁 RAII 模板
 */
template <class T>
struct WriteScopedLockImpl {
 public:
  WriteScopedLockImpl(T &mutex) : m_mutex(mutex) {
    m_mutex.wrlock();
    m_locked = true;
  }

  ~WriteScopedLockImpl() {
    unlock();
  }

  void lock() {
    if (!m_locked) {
      m_mutex.wrlock();
      m_locked = true;
    }
  }

  void unlock() {
    if (m_locked) {
      m_mutex.unlock();
      m_locked = false;
    }
  }

 private:
  T &m_mutex;
  bool m_locked;
};

/**
 * @brief 互斥锁 (封装 pthread_mutex_t)
 * * 场景： 线程级互斥。
 * * 注意： 在协程调度器内部可以使用，但不要在协程业务逻辑中使用（会导致线程挂起）。
 */
class Mutex : Nonecopyable {
 public:
  typedef ScopedLockImpl<Mutex> Lock;

  Mutex() {
    pthread_mutex_init(&m_mutex, nullptr);
  }

  ~Mutex() {
    pthread_mutex_destroy(&m_mutex);
  }

  void lock() {
    pthread_mutex_lock(&m_mutex);
  }

  void unlock() {
    pthread_mutex_unlock(&m_mutex);
  }

 private:
  pthread_mutex_t m_mutex;
};

/**
 * @brief 读写锁 (封装 pthread_rwlock_t)
 * * 场景： 读多写少的配置更新场景。
 */
class RWMutex : Nonecopyable {
 public:
  typedef ReadScopedLockImpl<RWMutex> ReadLock;
  typedef WriteScopedLockImpl<RWMutex> WriteLock;

  RWMutex() {
    pthread_rwlock_init(&m_lock, nullptr);
  }

  ~RWMutex() {
    pthread_rwlock_destroy(&m_lock);
  }

  void rdlock() {
    pthread_rwlock_rdlock(&m_lock);
  }

  void wrlock() {
    pthread_rwlock_wrlock(&m_lock);
  }

  void unlock() {
    pthread_rwlock_unlock(&m_lock);
  }

 private:
  pthread_rwlock_t m_lock;
};

/**
 * @brief 自旋锁 (封装 pthread_spinlock_t)
 * * 场景： 高性能短临界区保护，如 Scheduler 任务队列入队。
 * * 优势： 忙等待，不切换线程上下文，在竞争不激烈时性能远高于 Mutex。
 */
class Spinlock : Nonecopyable {
 public:
  typedef ScopedLockImpl<Spinlock> Lock;

  Spinlock() {
    pthread_spin_init(&m_mutex, 0);
  }

  ~Spinlock() {
    pthread_spin_destroy(&m_mutex);
  }

  void lock() {
    pthread_spin_lock(&m_mutex);
  }

  void unlock() {
    pthread_spin_unlock(&m_mutex);
  }

 private:
  pthread_spinlock_t m_mutex;
};

/**
 * @brief 原子锁 (CAS Lock)
 * * 场景： 极轻量级的标志位保护。
 */
class CASLock : Nonecopyable {
 public:
  typedef ScopedLockImpl<CASLock> Lock;

  CASLock() {
    m_mutex.clear();
  }

  ~CASLock() {}

  void lock() {
    // 自旋直到获取锁 (TAS: Test And Set)
    while (std::atomic_flag_test_and_set_explicit(&m_mutex, std::memory_order_acquire)) {
    }
  }

  void unlock() {
    std::atomic_flag_clear_explicit(&m_mutex, std::memory_order_release);
  }

 private:
  volatile std::atomic_flag m_mutex = ATOMIC_FLAG_INIT;
};

}  // namespace monsoon

#endif