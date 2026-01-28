#ifndef __MONSOON_IOMANAGER_H__
#define __MONSOON_IOMANAGER_H__

#include <sys/epoll.h>
#include <fcntl.h>
#include <memory>
#include <atomic>
#include <functional>
#include <vector>

#include "scheduler.h"
#include "timer.h"

namespace monsoon {

/**
 * @brief IO事件类型枚举
 * * 细节：使用位掩码，支持同时注册读写事件
 */
enum Event {
  NONE  = 0x0, // 无事件
  READ  = 0x1, // 读事件 (EPOLLIN)
  WRITE = 0x4, // 写事件 (EPOLLOUT)
};

/**
 * @brief 事件上下文
 * * 细节：保存事件触发时需要执行的回调或协程
 */
struct EventContext {
  Scheduler *scheduler = nullptr; // 执行该事件的调度器
  Fiber::ptr fiber;               // 事件对应的协程
  std::function<void()> cb;       // 事件对应的回调函数
};

/**
 * @brief 文件句柄上下文
 * * 细节：管理一个 FD 关注的所有事件（读/写）
 */
class FdContext {
  friend class IOManager;

 public:
  /**
   * @brief 获取事件上下文
   * @param event 事件类型
   * @return EventContext& 对应事件的上下文引用
   */
  EventContext &getEveContext(Event event);

  /**
   * @brief 重置事件上下文
   * @param ctx 待重置的上下文
   */
  void resetEveContext(EventContext &ctx);

  /**
   * @brief 触发事件
   * @param event 触发的事件类型
   */
  void triggerEvent(Event event);

 private:
  EventContext read;      // 读事件上下文
  EventContext write;     // 写事件上下文
  int fd = 0;             // 文件句柄
  Event events = NONE;    // 当前已注册的事件
  Mutex mutex;            // 互斥锁，保护 FdContext 的修改
};

/**
 * @brief 基于 Epoll 的 IO 协程调度器
 * * 场景：服务器的核心 IO 调度模块，负责监听 socket 事件并唤醒协程。
 */
class IOManager : public Scheduler, public TimerManager {
 public:
  typedef std::shared_ptr<IOManager> ptr;
  typedef RWMutex RWMutexType;

  /**
   * @brief 构造函数：初始化 IO 调度器
   * @param threads 线程池包含的线程数量
   * @param use_caller 是否将当前的调用线程（Caller Thread）纳入调度体系
   * @param name 调度器的名称，用于日志和调试
   * @param core_offset CPU绑定偏移量，用于设置线程亲和性
   * * 细节：
   * 1. 初始化 Epoll 实例和 Tickle 管道。
   * 2. 启动调度器线程池。
   */
  IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager", int core_offset = 0);

  /**
   * @brief 析构函数
   * * 细节：停止调度器，释放 Epoll 资源和 FdContext。
   */
  ~IOManager();

  /**
   * @brief 添加事件
   * @param fd 文件描述符
   * @param event 关注的事件类型 (READ / WRITE)
   * @param cb 事件触发后的回调函数 (若为空，则默认回调当前协程)
   * @return int 0 success, -1 error
   * * 场景：当 socket 读写返回 EAGAIN 时调用。
   */
  int addEvent(int fd, Event event, std::function<void()> cb = nullptr);

  /**
   * @brief 删除事件
   * @param fd 文件描述符
   * @param event 待删除的事件类型
   * @return bool 是否删除成功
   * * 细节：仅从 Epoll 中移除，不会触发回调。
   */
  bool delEvent(int fd, Event event);

  /**
   * @brief 取消事件
   * @param fd 文件描述符
   * @param event 待取消的事件类型
   * @return bool 是否取消成功
   * * 细节：从 Epoll 中移除，且**强制触发一次回调**（通常用于超时或取消任务）。
   */
  bool cancelEvent(int fd, Event event);

  /**
   * @brief 取消指定 fd 下的所有事件
   * @param fd 文件描述符
   * @return bool 是否成功
   */
  bool cancelAll(int fd);

  /**
   * @brief 获取当前的 IOManager 对象
   */
  static IOManager *GetThis();

 protected:
  /**
   * @brief 通知调度器有任务到来
   * * 细节：写 pipe 唤醒从 epoll_wait 中阻塞的 idle 协程。
   */
  void tickle() override;

  /**
   * @brief 判断调度器是否可以停止
   */
  bool stopping() override;

  /**
   * @brief 核心调度等待循环 (Idle 协程)
   * * 细节：执行 epoll_wait，处理 IO 事件和定时器。
   */
  void idle() override;

  /**
   * @brief 判断是否可以停止，同时获取最近一个定时超时时间
   * @param timeout 传出参数，距离最近定时器触发的时间
   */
  bool stopping(uint64_t &timeout);

  /**
   * @brief 当有新的定时器插入到链表最前端时调用
   * * 细节：立即 tickle 唤醒 idle，更新 epoll_wait 超时时间。
   */
  void OnTimerInsertedAtFront() override;

  /**
   * @brief 扩展 FdContext 容器大小
   */
  void contextResize(size_t size);

 private:
  /// Epoll 文件句柄
  int epfd_ = 0;
  /// Pipe 文件句柄 [0]读 [1]写，用于唤醒 idle
  int tickleFds_[2];
  /// 正在等待执行的 IO 事件数量
  std::atomic<size_t> pendingEventCnt_ = {0};
  /// 读写锁，保护 fdContexts_ 的扩容
  RWMutexType mutex_;
  /// socket 事件上下文容器
  std::vector<FdContext *> fdContexts_;
};

}  // namespace monsoon

#endif