#include "iomanager.h"
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <algorithm> // for std::min
#include <sched.h> // [新增] for pthread_setaffinity_np

namespace monsoon {

/**
 * @brief 获取事件对应的上下文引用
 * @param event 待获取的事件类型 (READ/WRITE)
 * @return EventContext& 事件上下文引用
 */
EventContext &FdContext::getEveContext(Event event) {
    switch (event) {
        case READ:
            return read;
        case WRITE:
            return write;
        default:
            CondPanic(false, "getContext error: unknow event");
    }
    throw std::invalid_argument("getContext invalid event");
}

/**
 * @brief 重置事件上下文
 * @param ctx 待重置的上下文引用
 */
void FdContext::resetEveContext(EventContext &ctx) {
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

/**
 * @brief 触发事件
 * @param event 待触发的事件类型
 * * 细节：
 * 1. 验证事件是否已注册。
 * 2. 清除该事件的注册状态（因为是边缘触发+一次性触发机制）。
 * 3. 将对应的回调或协程加入调度器等待执行。
 */
void FdContext::triggerEvent(Event event) {
    CondPanic(events & event, "event hasn't been registed");
    // FdContext 中移除 event，清除该事件标记，表示已触发
    events = (Event)(events & ~event);  // events & ~event —— 位清零的经典写法。语法含义：从 events 中移除 event事件
    EventContext &ctx = getEveContext(event); // 获取对应事件的上下文
    
    if (ctx.cb) {
        // 注册事件的调度回调函数
        ctx.scheduler->schedule(ctx.cb);
    } else {
        // 注册事件的调度协程
        ctx.scheduler->schedule(ctx.fiber);
    }
    // 事件触发后重置上下文，以便下次复用
    resetEveContext(ctx);
    return;
}

/**
 * @brief 构造函数：初始化IO调度器
 * @param threads 线程池包含的线程数量
 * @param use_caller 是否将当前的调用线程（Caller Thread）纳入调度体系
 * @param name 调度器的名称，用于日志和调试
 * * 场景： 在服务器启动时调用，构建基于Epoll的协程调度中心。
 * 细节：
 * 1. 创建 Epoll 实例。
 * 2. 创建用于通知调度协程的管道 (Pipe)，并注册到 Epoll 中。
 * 3. 预分配 Socket 上下文容器。
 * 4. 启动调度器。
 * 5. [Raft优化] 根据传入的 core_offset 参数，设置线程亲和性 (CPU Affinity)。
 */
IOManager::IOManager(size_t threads, bool use_caller, const std::string &name, int core_offset)
    : Scheduler(threads, use_caller, name) {
    // 创建epoll实例，size参数在Linux 2.6.8之后被忽略，但必须大于0
    epfd_ = epoll_create(5000);
    CondPanic(epfd_ > 0, "epoll_create error");

    // 创建管道，用于tickle（唤醒）idle协程
    int ret = pipe(tickleFds_);
    CondPanic(ret == 0, "pipe error");

    // 注册管道读端的可读事件
    epoll_event event{};
    memset(&event, 0, sizeof(epoll_event));
    event.events = EPOLLIN | EPOLLET; // 边缘触发
    event.data.fd = tickleFds_[0];

    // 设置管道读端为非阻塞，配合ET模式
    ret = fcntl(tickleFds_[0], F_SETFL, O_NONBLOCK);
    CondPanic(ret == 0, "set fd nonblock error");

    // 将管道加入epoll监听
    ret = epoll_ctl(epfd_, EPOLL_CTL_ADD, tickleFds_[0], &event);
    CondPanic(ret == 0, "epoll_ctl error");

    // 预分配一定的FdContext大小
    contextResize(32);

    // 启动 Scheduler::run 进行调度，会创建线程池并让每个工作线程运行调度循环
    start();

    // =======================================================
    // [Raft优化] CPU 亲和性设置 (Thread Affinity)
    // =======================================================
    
    // 获取当前系统的逻辑核心数
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    
    // [策略配置]
    // stride: 步长。设为 1 表示紧凑绑定 (0,1,2,3)
    // 设为 2 表示隔核绑定 (0,2,4,6)，有助于避开超线程(HT)争抢 ALU 资源
    // 对于计算密集型的 Raft Leader，建议 stride = 1 (独占物理核) 或根据具体 CPU 拓扑调整
    int stride = 1; 

    // A. 绑定 Worker 线程池
    {
        Mutex::Lock lock(Scheduler::mutex_); // 保护 threadPool_
        for (size_t i = 0; i < threadPool_.size(); ++i) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);

            // 计算目标核心：(偏移量 + 索引 * 步长) % 总核数
            // 举例：offset=0, stride=1 -> 0, 1, 2, 3
            // 举例：offset=4, stride=1 -> 4, 5, 6, 7 (用于 IO 分离)
            int core_id = (core_offset + i * stride) % num_cores;
            CPU_SET(core_id, &cpuset);

            // [关键修复] 获取原生线程句柄并调用系统 API
            // 假设你的 Thread 类封装了 pthread_t，并提供了 native_handle()
            // 如果没有，你需要去 Thread 类里加一个 getId() 返回 pthread_t
            pthread_t native_thread = threadPool_[i]->native_handle();
            
            int rc = pthread_setaffinity_np(native_thread, sizeof(cpu_set_t), &cpuset);
            if (rc != 0) {
                std::cerr << "[WARNING] Bind worker_" << i << " to core " << core_id 
                          << " failed: " << strerror(rc) << std::endl;
            } else {
                // std::cout << "[INFO] Bound worker_" << i << " to core " << core_id << std::endl;
            }
        }
    }

    // B. 绑定 Caller 线程 (如果当前线程也参与调度)
    if (use_caller) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        
        // 策略：Caller 线程通常承担 Main Loop 或 Accept 职责
        // 将其绑定到 offset 之前的最后一个核，或者紧接着 Worker 的下一个核
        // 这里简单策略：绑定到分配给 Worker 之后的下一个核，避免冲突
        int core_id = (core_offset + threadPool_.size() * stride) % num_cores;
        CPU_SET(core_id, &cpuset);

        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
             std::cerr << "[WARNING] Bind caller thread to core " << core_id 
                       << " failed: " << strerror(rc) << std::endl;
        }
    }
}

/**
 * @brief 析构函数：销毁IO调度器
 * * 场景： 服务器关闭或对象销毁时。
 * 细节：
 * 1. 停止调度器。
 * 2. 关闭 Epoll 文件句柄和管道句柄。
 * 3. 释放所有的 FdContext 内存。
 */
IOManager::~IOManager() {
    stop();
    close(epfd_);   // 关闭 epoll 文件句柄
    close(tickleFds_[0]);   // 关闭管道读端
    close(tickleFds_[1]);   // 关闭管道写端

    // 释放 fdContexts 内存中的每个 FdContext 对象(一个FdContext对应一个socket fd)
    for (size_t i = 0; i < fdContexts_.size(); i++) {
        if (fdContexts_[i]) {
            delete fdContexts_[i];
        }
    }
}

/**
 * @brief 扩展 FdContext 容器大小，使fdContexts_能容纳指定数量的fd
 * @param size 扩容后的目标大小
 * * 细节：
 * 1. 只有在 fd 超过当前大小时才需要调用。
 * 2. 初始化新分配的 FdContext 对象。
 */
void IOManager::contextResize(size_t size) {
    fdContexts_.resize(size);
    // 初始化新分配的 FdContext 对象
    for (size_t i = 0; i < fdContexts_.size(); i++) {
        if (!fdContexts_[i]) {
            fdContexts_[i] = new FdContext;
            fdContexts_[i]->fd = i;
        }
    }
}

/**
 * @brief 添加事件
 * @param fd 文件描述符
 * @param event 关注的事件类型 (READ/WRITE)
 * @param cb 事件触发后的回调函数 (若为空则默认回调当前协程)
 * @return int 0 success, -1 error
 * * 场景： 协程执行 read/write 遇到 EAGAIN（资源暂时不可用） 时，将 fd 注册到 IOManager 并挂起当前协程。
 * 细节：
 * 1. 检查并扩展 fd 上下文容器 (Double-Checked Locking)。
 * 2. 更新 Epoll 监听状态 (EPOLL_CTL_ADD 或 EPOLL_CTL_MOD)。
 * 3. 保存回调对象或当前协程上下文。
 */
int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
    FdContext *fd_ctx = nullptr;
    
    // 1. 获取读锁，尝试获取 FdContext
    RWMutex::ReadLock lock(mutex_);
    if ((int)fdContexts_.size() > fd) {
        fd_ctx = fdContexts_[fd];
        lock.unlock();
    } else {
        lock.unlock();
        // 2. 读锁获取失败（fd超出范围），升级为写锁进行扩容，扩容后获取 FdContext
        RWMutex::WriteLock lock2(mutex_);
        // Double-Check: 再次判断大小，防止在获取写锁等待期间已被其他线程扩容
        if ((int)fdContexts_.size() <= fd) {
            contextResize(fd * 1.5);    // 扩容的时候，fdContexts_ 大小变 150，fdContexts_[100]~[150] 的对象全被 new 出来了。
        }
        fd_ctx = fdContexts_[fd];
    }

    // 3. 对具体的 FdContext 加锁
    Mutex::Lock ctxLock(fd_ctx->mutex);
    
    // 检查事件是否重复添加，如果事件已存在则报错
    if (fd_ctx->events & event) {
        std::cerr << "addEvent assert error: fd=" << fd << " event=" << event
                  << " fd_ctx.events=" << fd_ctx->events << std::endl;
        CondPanic(!(fd_ctx->events & event), "addEvent error");
    }

    // 判断是新增还是修改 (若已有其他事件则为MOD，否则为ADD)
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    // 构建增加了 event 的事件集合（events）给 epoll_ctl 使用（给内核监听）
    epoll_event epevent;
    epevent.events = EPOLLET | fd_ctx->events | event; // 新的事件集合如下：保持原有事件，添加新事件，开启ET模式
    epevent.data.ptr = fd_ctx; // 携带 fd 的上下文指针（用于触发事件时找到对应的 FdContext）

    int ret = epoll_ctl(epfd_, op, fd, &epevent);
    if (ret) {
        std::cerr << "addEvent: epoll_ctl error, fd=" << fd << " op=" << op << " err=" << strerror(errno) << std::endl;
        return -1;
    }

    // 待执行IO事件数量 +1
    ++pendingEventCnt_;

    // 更新 FdContext 事件状态，把新事件加入到 fd_ctx->events 中
    fd_ctx->events = (Event)(fd_ctx->events | event);
    EventContext &event_ctx = fd_ctx->getEveContext(event);
    CondPanic(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb, "event_ctx is dirty");

    // 设置事件触发时的调度器
    event_ctx.scheduler = Scheduler::GetThisScheduler();
    if (cb) {
        // 设置了回调函数
        event_ctx.cb.swap(cb);
    } else {
        // 未设置回调函数，则将当前协程设置为回调任务
        event_ctx.fiber = Fiber::GetThis();
        CondPanic(event_ctx.fiber->getState() == Fiber::RUNNING, "state=" + std::to_string(event_ctx.fiber->getState()));
    }
    // std::cout << "add event success, fd = " << fd << std::endl;
    return 0;
}

/**
 * @brief 取消事件
 * @param fd 文件描述符
 * @param event 待取消的事件类型
 * @return bool 是否取消成功
 * * 场景： 任务超时或强制退出时。
 * 细节：
 * 1. 从 Epoll 中移除监听。
 * 2. **主动触发**一次该事件（执行回调），以确保协程能从 yield 状态恢复并退出。
 */
bool IOManager::cancelEvent(int fd, Event event) {
    // 1. 获取读锁，尝试获取 fd 对应的 FdContext
    RWMutex::ReadLock lock(mutex_);
    if ((int)fdContexts_.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = fdContexts_[fd];
    lock.unlock();

    // 2. 对具体的 FdContext 加锁
    Mutex::Lock ctxLock(fd_ctx->mutex);
    if (!(fd_ctx->events & event)) {
        return false;
    }

    // 构建去除了 event 的事件集合（events）给 epoll_ctl 使用（给内核监听）
    Event new_events = (Event)(fd_ctx->events & ~event);    // 清理指定事件位，也就是移除 events 中的 event 事件
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;  // 携带 fd 的上下文指针（用于触发事件时找到对应的 FdContext）

    int ret = epoll_ctl(epfd_, op, fd, &epevent);
    if (ret) {
        std::cerr << "cancelEvent: epoll_ctl error, fd=" << fd << " err=" << strerror(errno) << std::endl;
        return false;
    }

    // 关键区别：取消操作会触发一次事件，让等待的协程醒来处理“被取消”的逻辑
    // 虽然 IO 事件没有发生，但必须强行唤醒协程，否则协程会永远挂死在内存里（例如等待一个已经被取消的事件）。
    fd_ctx->triggerEvent(event);
    --pendingEventCnt_;
    return true;
}

/**
 * @brief 删除事件
 * @param fd 文件描述符
 * @param event 待删除的事件类型
 * @return bool 是否删除成功
 * * 场景： 主动移除不再关心的事件。
 * 细节：
 * 1. 只是从 Epoll 中移除监听，**不会触发**事件回调。
 * 2. 如果该 fd 上没有其他事件了，则使用 EPOLL_CTL_DEL。
 */
bool IOManager::delEvent(int fd, Event event) {
    // 1. 获取读锁，尝试获取 fd 对应的 FdContext
    RWMutex::ReadLock lock(mutex_);
    if ((int)fdContexts_.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = fdContexts_[fd];
    lock.unlock();

    // 对具体的 FdContext 加锁
    Mutex::Lock ctxLock(fd_ctx->mutex);
    if (!(fd_ctx->events & event)) {
        return false;
    }

    // 2. 构建去除了 event 的事件集合（events）给 epoll_ctl 使用（给内核监听）
    Event new_events = (Event)(fd_ctx->events & ~event);    // 位清零，移除 events 中的 event 事件
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    // 3. 调用 epoll_ctl 更新监听状态
    int ret = epoll_ctl(epfd_, op, fd, &epevent);
    if (ret) {
        std::cerr << "delEvent: epoll_ctl error, fd=" << fd << " err=" << strerror(errno) << std::endl;
        return false;
    }

    --pendingEventCnt_;

    // 4. 更新 FdContext 事件状态
    fd_ctx->events = new_events;
    EventContext &event_ctx = fd_ctx->getEveContext(event);
    fd_ctx->resetEveContext(event_ctx);
    return true;
}

/**
 * @brief 取消指定fd下的所有事件
 * @param fd 文件描述符
 * @return bool 是否成功
 * * 场景： fd 关闭时，清理所有残留事件。
 */
bool IOManager::cancelAll(int fd) {
    RWMutex::ReadLock lock(mutex_);
    if ((int)fdContexts_.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = fdContexts_[fd];
    lock.unlock();

    Mutex::Lock ctxLock(fd_ctx->mutex);
    if (!fd_ctx->events) {
        return false;
    }

    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = 0;
    epevent.data.ptr = fd_ctx;

    int ret = epoll_ctl(epfd_, op, fd, &epevent);
    if (ret) {
        std::cerr << "cancelAll: epoll_ctl error, fd=" << fd << " err=" << strerror(errno) << std::endl;
        return false;
    }

    // 依次触发可能存在的读写事件
    if (fd_ctx->events & READ) {
        fd_ctx->triggerEvent(READ);
        --pendingEventCnt_;
    }
    if (fd_ctx->events & WRITE) {
        fd_ctx->triggerEvent(WRITE);
        --pendingEventCnt_;
    }
    CondPanic(fd_ctx->events == 0, "fd not totally clear");
    return true;
}


/**
 * @brief 获取当前的 IOManager 对象
 * @return IOManager* */
IOManager *IOManager::GetThis() { 
    return dynamic_cast<IOManager *>(Scheduler::GetThisScheduler()); 
}

/**
 * @brief 通知调度器有任务到来
 * * 场景： 当其他线程添加了任务，或者定时器超时，需要唤醒 idle 状态的线程。
 * 细节：
 * 1. 写入管道一个字符，使阻塞在 epoll_wait 的 idle 协程返回。
 */
void IOManager::tickle() {
    if (!isHasIdleThreads()) {
        return;
    }
    // 写pipe管道，使得idle协程从epoll_wait退出，开始调度任务
    int rt = write(tickleFds_[1], "T", 1);
    CondPanic(rt == 1, "write pipe error");
}

/**
 * @brief 核心调度等待循环 (Idle 协程)
 * * 场景： 当调度器无任务可做时，Worker 线程会执行此协程。
 * 细节：
 * 1. 负责执行 epoll_wait，等待 IO 事件发生。
 * 2. 负责管理定时器（Timer），计算最小超时时间。
 * 3. 当有 IO 事件或定时器超时，唤醒并执行对应任务。
 * 4. 如果收到 tickle 信号，说明有新任务加入，退出 idle 状态。
 */
void IOManager::idle() {
    // 每次 epoll_wait 最多检测 256 个就绪事件
    const uint64_t MAX_EVENTS = 256;
    // 使用 vector 管理内存，自动释放，避免手动 delete[]
    std::vector<epoll_event> events(MAX_EVENTS);

    while (true) {
        // 获取下一个定时器超时时间，同时判断调度器是否已经 stop
        // 如果系统要停止了（且没定时器、没 Pending 事件），直接 break 退出 idle 循环
        uint64_t next_timeout = 0;
        if (stopping(next_timeout)) {
            std::cout << "name=" << getName() << " idle stopping exit" << std::endl;
            break;
        }

        // 阻塞等待，等待事件发生 或者 定时器超时
        int ret = 0;
        do {
            static const int MAX_TIMEOUT = 5000;

            // 问一下定时器管理器：“下一个定时器啥时候触发？”
            // 如果最近的定时器是 10ms 后，那就设置 epoll_wait 超时时间为 10ms
            // 如果没有定时器，就设为默认最大值（5秒）
            if (next_timeout != ~0ull) {
                next_timeout = std::min((int)next_timeout, MAX_TIMEOUT);
            } else {
                next_timeout = MAX_TIMEOUT;
            }
            
            // 工作线程执行 epoll_wait()，阻塞等待 IO 事件或超时发生（协程的read是非阻塞的）
            // 物理动作：当前线程挂起，让出 CPU
            // &events[0] 获取 vector 底层数组指针
            ret = epoll_wait(epfd_, &events[0], MAX_EVENTS, (int)next_timeout);

            if (ret < 0) {
                if (errno == EINTR) {
                    // 系统调用被信号中断，重新等待
                    continue;
                }
                std::cerr << "epoll_wait [" << epfd_ << "] errno, err: " << strerror(errno) << std::endl;
                break; // 出错退出内层循环，重新进入外层循环检查状态
            } else {
                // 超时或 epoll_wait 有事件返回
                break;
            }
        } while (true);

        // 1. 收集定时管理器（一组内存里的数据结构）中所有超时定时器，执行回调函数
        std::vector<std::function<void()>> cbs; // 检查所有已经过期的定时器，把它们绑定的回调函数取出来。
        listExpiredCb(cbs);
        if (!cbs.empty()) {
            for (const auto &cb : cbs) {
                // [Raft优化] 定时器任务也使用新的 schedule 接口
                schedule(cb);  // 把这些定时器的任务都扔进调度器的全局任务队列 (tasks_)
            }
            cbs.clear();
        }

        // 2. 处理 Epoll 返回的 IO 事件
        // 遍历 epoll_wait 返回的 events 列表
        for (int i = 0; i < ret; i++) {
            epoll_event &event = events[i];

            // 如果是 tickle 管道的可读事件(有其他线程喊醒此线程)
            if (event.data.fd == tickleFds_[0]) {
                uint8_t dummy[256];
                // 循环 read 管道，把里面的数据读干净（必须循环读取直到 EAGAIN，防止 ET 模式下漏读导致下次无法触发）
                while (true) {
                    int read_len = read(tickleFds_[0], dummy, sizeof(dummy));
                    if (read_len < 0) {
                        if (errno == EAGAIN) break; // 读完了
                    } else if (read_len == 0) {
                        break; // 对端关闭
                    }
                }
                continue;
            }

            // 真实 IO 事件 (Socket 有动静)，处理普通 IO 事件
            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            Mutex::Lock lock(fd_ctx->mutex);

            // 错误事件 or 挂起事件(对端关闭)，转换为读写事件处理
            if (event.events & (EPOLLERR | EPOLLHUP)) {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
            
            // 解析实际发生的事件类型
            int real_events = NONE;
            if (event.events & EPOLLIN) {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) {
                real_events |= WRITE;
            }

            // 如果没有关注的事件发生，跳过
            if ((fd_ctx->events & real_events) == NONE) {
                continue;
            }

            // 剔除已经发生的事件，将剩余的事件重新加入 epoll_wait
            // 类似于手动实现 EPOLLET 的 ONESHOT 效果：触发后需要重新注册
            int left_events = (fd_ctx->events & ~real_events);
            int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events = EPOLLET | left_events;

            int ret2 = epoll_ctl(epfd_, op, fd_ctx->fd, &event);
            if (ret2) {
                std::cerr << "epoll_ctl mod error: " << strerror(errno) << std::endl;
                continue;
            }

            // 处理已就绪事件 （把该 fd 对应的 协程 (Fiber) 或 回调 扔进调度器的全局任务队列）
            if (real_events & READ) {
                fd_ctx->triggerEvent(READ);
                --pendingEventCnt_;
            }
            if (real_events & WRITE) {
                fd_ctx->triggerEvent(WRITE);
                --pendingEventCnt_;
            }
        }

        // 处理结束，idle 协程 yield，让出 CPU
        // 此时调度协程 (run) 可以去 tasklist 中检测并拿取新任务
        Fiber::ptr cur = Fiber::GetThis();
        auto raw_ptr = cur.get();
        cur.reset(); // 解除引用，防止循环引用或计数异常

        raw_ptr->yield();
    }
    // 跳出死循环，意味着idle 协程运行完毕，状态变为 TERM，此时调度器（IOManager继承了Scheduler，它就是调度器）决定退出。
}

/**
 * @brief 判断 IOManager 是否可以停止
 * @return bool 
 */
bool IOManager::stopping() {
    uint64_t timeout = 0;
    return stopping(timeout);
}

/**
 * @brief 判断 IOManager 是否可以停止 (带下一个超时时间传出)
 * @param timeout 用于传出距离下一个定时器触发的时间
 * @return bool 
 * * 细节：
 * 1. 所有待调度的 IO 事件执行结束后，才允许退出。
 * 2. 所有定时器执行结束后，才允许退出。
 */
bool IOManager::stopping(uint64_t &timeout) {
    timeout = getNextTimer();
    // 只有当没有定时器任务 (~0ull)、没有待处理 IO 事件 (pendingEventCnt_ == 0) 
    // 且 Scheduler 本身发出了停止信号时，才真正停止。
    return timeout == ~0ull && pendingEventCnt_ == 0 && Scheduler::stopping();
}

/**
 * @brief 定时器插入到最前端的回调
 * * 场景： 当插入了一个比当前最小定时器还要早触发的定时器时调用。
 * 细节：
 * 1. 立即调用 tickle，唤醒 idle 协程，使其能更新 epoll_wait 的超时时间，避免新定时器延误。
 */
void IOManager::OnTimerInsertedAtFront() { 
    tickle(); 
}

}  // namespace monsoon