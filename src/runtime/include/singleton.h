#ifndef __MONSOON_SINGLETON_H__
#define __MONSOON_SINGLETON_H__

#include <memory>

namespace monsoon {

/**
 * @brief 全局单例模式封装类 (返回引用/裸指针)
 * @details T 类型
 * X 为了创造多个实例对应的 Tag
 * N 同一个 Tag 创造多个实例索引
 * * 场景： 用于全局唯一的无状态工具类或全局管理器 (如 FdMgr, Logger)。
 * * 细节：
 * 1. 利用 C++11 静态局部变量初始化线程安全的特性 (Magic Static)。
 * 2. 相比双重检查锁 (DCL)，编译器生成的汇编代码更精简，性能更高。
 * 3. 返回引用或裸指针，避免 shared_ptr 的原子计数开销，适合热路径调用。
 */
template <class T, class X = void, int N = 0>
class Singleton {
 public:
  /**
   * @brief 获取单例的裸指针
   * @return T* 单例对象的指针
   */
  static T *GetInstance() {
    static T v;
    return &v;
  }
};

/**
 * @brief 全局单例模式智能指针封装类
 * @details T 类型
 * X 为了创造多个实例对应的 Tag
 * N 同一个 Tag 创造多个实例索引
 * * 场景： 需要生命周期管理的全局对象，或者需要传递所有权的场景。
 * * 细节：
 * 1. 同样基于 Magic Static 保证初始化安全。
 * 2. 使用 shared_ptr 管理，会带来原子引用计数的微小开销。
 * 3. [注意] 在 Thread-per-Core 的核心 IO 循环中，建议优先使用 Singleton 裸指针版本。
 */
template <class T, class X = void, int N = 0>
class SingletonPtr {
 public:
  /**
   * @brief 获取单例的智能指针
   * @return std::shared_ptr<T>
   */
  static std::shared_ptr<T> GetInstance() {
    static std::shared_ptr<T> v(new T);
    return v;
  }
};

/**
 * @brief [新增] 线程局部单例模式 (Thread Local Singleton)
 * @details T 类型
 * X Tag
 * N Index
 * * 场景： 专为 Thread-per-Core 架构设计。
 * 例如：每个 IO 线程独立的内存池 (Arena)、独立的统计指标、独立的协程调度器引用。
 * * 细节：
 * 1. 使用 thread_local 关键字，保证每个线程有一份独立的实例。
 * 2. 完全无锁 (Lock-free)，因为数据只属于当前线程，不存在竞争。
 * 3. 实现了 Share-Nothing 架构的核心数据隔离。
 */
template <class T, class X = void, int N = 0>
class ThreadLocalSingleton {
 public:
  /**
   * @brief 获取当前线程的单例实例
   */
  static T *GetInstance() {
    static thread_local T v;
    return &v;
  }
};

}  // namespace monsoon

#endif