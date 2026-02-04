#include "hook.h"
#include <dlfcn.h>
#include <cstdarg>
#include <iostream>
#include <string.h>

#include "fiber.h"
#include "iomanager.h"
#include "fd_manager.h"
#include "scheduler.h"

namespace monsoon {

static thread_local bool t_hook_enable = false;
static int g_tcp_connect_timeout = 5000;

// 宏定义：批量声明 Hook 函数，请对 sleep 做一次 XX 操作，其他函数类推
#define HOOK_FUN(XX) \
  XX(sleep)          \
  XX(usleep)         \
  XX(nanosleep)      \
  XX(socket)         \
  XX(connect)        \
  XX(accept)         \
  XX(read)           \
  XX(readv)          \
  XX(recv)           \
  XX(recvfrom)       \
  XX(recvmsg)        \
  XX(write)          \
  XX(writev)         \
  XX(send)           \
  XX(sendto)         \
  XX(sendmsg)        \
  XX(close)          \
  XX(fcntl)          \
  XX(ioctl)          \
  XX(getsockopt)     \
  XX(setsockopt)     \
  XX(dup)            \
  XX(dup2)

/**
 * @brief Hook 初始化函数：宏定义原生函数地址
 * * 场景： 在程序启动或首次调用 Hook 接口时执行。
 * 细节：
 * 1. 使用 dlsym(RTLD_NEXT, name) 获取系统原生的函数地址。
 * 2. 保证只初始化一次。
 */
void hook_init() {
    static bool is_inited = false;
    if (is_inited) {
        return;
    }
// 定义 XX 具体要做什么：把所有需要 Hook 的系统函数的原始地址找到，并保存到对应的 xxx_f 变量中
#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);    // dlsym(RTLD_NEXT, #name) 获取 #name 的地址（即系统原生函数地址）
    HOOK_FUN(XX);   // 执行清单，让清单中的每一个系统调用函数都执行一次 XX
#undef XX   // 用完即清理宏定义，保持代码整洁
    is_inited = true;
}

static uint64_t s_connect_timeout = -1;
// Hook 初始化结构体，确保程序启动时执行 hook_init
struct _HOOKIniter {
    _HOOKIniter() {
        hook_init();
        s_connect_timeout = g_tcp_connect_timeout;
    }
};
static _HOOKIniter s_hook_initer;

// Hook 开关控制函数
bool is_hook_enable() { return t_hook_enable; }

// 设置 Hook 是否开启
void set_hook_enable(const bool flag) { t_hook_enable = flag; }

struct timer_info {
    int cancelled = 0;
};

/**
 * @brief 通用 IO 协程调度模板函数
 * @param fd 文件描述符
 * @param fun 原系统函数
 * @param hook_fun_name Hook 函数名
 * @param event 关注的事件
 * @param timeout_so 超时选项类型
 * @param args 参数列表
 * @return ssize_t IO 结果
 * * 场景： 拦截 read/write 等可能阻塞的 IO 调用。
 * 细节：
 * 1. 尝试执行一次系统调用，如果返回 EAGAIN 则挂起协程。
 * 2. 设置定时器（如果配置了超时）。
 * 3. 注册 IO 事件并 yield。
 * 4. 恢复后重试。
 */
template <typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char *hook_fun_name, uint32_t event, int timeout_so, Args &&...args) {
    // 1. 未开启 Hook，直接调用原函数
    if (!t_hook_enable) {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 2. 获取此系统调用文件描述符的文件句柄上下文
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
    if (!ctx) {
        return fun(fd, std::forward<Args>(args)...);
    }

    if (ctx->isClose()) {   // 文件已关闭，返回错误
        errno = EBADF;
        return -1;
    }

    // 如果 FdCtx 记录它不是 Socket，或者是用户显式设置的非阻塞
    if (!ctx->isSocket() || ctx->getUserNonblock()) {   // 非 socket 或用户设置非阻塞，直接调用原系统函数
        return fun(fd, std::forward<Args>(args)...);
    }

    uint64_t to = ctx->getTimeout(timeout_so);  // 从 FdCtx 获取 Hook 的超时时间
    std::shared_ptr<timer_info> tinfo(new timer_info);

    // 3. 尝试执行系统调用：
        // 如果成功或失败非 EAGAIN，直接返回结果
        // 如果失败且 errno 是 EAGAIN，进入协程调度逻辑
retry:
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    while (n == -1 && errno == EINTR) {
        n = fun(fd, std::forward<Args>(args)...);
    }

    if (n == -1 && errno == EAGAIN) {
        IOManager *iom = IOManager::GetThis();
        Timer::ptr timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        if (to != (uint64_t)-1) {
            timer = iom->addConditionTimer(
                to,
                [winfo, fd, iom, event]() {
                    auto t = winfo.lock();
                    if (!t || t->cancelled) {
                        return;
                    }
                    t->cancelled = ETIMEDOUT;
                    // 【修正】使用 monsoon::Event，不加 IOManager:: 前缀
                    iom->cancelEvent(fd, (monsoon::Event)(event));
                },
                winfo);
        }

        // 使用 monsoon::Event
        int rt = iom->addEvent(fd, (monsoon::Event)(event));
        if (rt) {
            std::cout << hook_fun_name << " addEvent(" << fd << ", " << event << ")";
            if (timer) {
                timer->cancel();
            }
            return -1;
        } else {
            Fiber::GetThis()->yield();
            if (timer) {
                timer->cancel();
            }
            if (tinfo->cancelled) {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    }

    return n;
}

extern "C" {

#define XX(name) name##_fun name##_f = nullptr;
HOOK_FUN(XX);
#undef XX

/**
 * @brief 睡眠 Hook (秒级)
 * @param seconds 秒
 * @return 0
 * * 场景： 协程让出 CPU，指定时间后自动唤醒。
 * 细节：
 * 1. 使用定时器触发 Scheduler 重新调度当前 Fiber。
 * 2. [修正] 使用 Lambda 替代 std::bind，解决重载函数 schedule 的匹配问题。
 */
unsigned int sleep(unsigned int seconds) {
    if (!t_hook_enable) {
        return sleep_f(seconds);
    }
    Fiber::ptr fiber = Fiber::GetThis();
    IOManager *iom = IOManager::GetThis();
    
    // [修复点] 使用 Lambda 表达式，清晰调用 iom->schedule(fiber)
    // 避免了 std::bind 对重载函数取地址的歧义
    iom->addTimer(seconds * 1000, [iom, fiber]() {
        iom->schedule(fiber);
    });
    
    Fiber::GetThis()->yield();
    return 0;
}

/**
 * @brief 睡眠 Hook (微秒级)
 * @param usec 微秒
 * @return 0
 * * 场景： 高精度协程休眠。
 */
int usleep(useconds_t usec) {
    if (!t_hook_enable) {
        return usleep_f(usec);
    }
    Fiber::ptr fiber = Fiber::GetThis();
    IOManager *iom = IOManager::GetThis();
    
    // 使用 Lambda
    iom->addTimer(usec / 1000, [iom, fiber]() {
        iom->schedule(fiber);
    });
    
    Fiber::GetThis()->yield();
    return 0;
}

/**
 * @brief 睡眠 Hook (纳秒级)
 * * 场景： 系统调用 nanosleep 的封装。
 */
int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!t_hook_enable) {
        return nanosleep_f(req, rem);
    }
    Fiber::ptr fiber = Fiber::GetThis();
    IOManager *iom = IOManager::GetThis();
    int timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000;
    
    // [修复点] 使用 Lambda
    iom->addTimer(timeout_ms, [iom, fiber]() {
        iom->schedule(fiber);
    });
    
    Fiber::GetThis()->yield();
    return 0;
}

/**
 * @brief 创建 Socket Hook
 * * 场景： 当hook启用的时候，创建套接字并接管。
 * 细节：
 * 1. 调用原系统 socket。
 * 2. 将 fd 注册到 FdMgr，以便后续 Hook 识别。
 */
int socket(int domain, int type, int protocol) {
    if (!t_hook_enable) {
        return socket_f(domain, type, protocol);
    }
    int fd = socket_f(domain, type, protocol);
    if (fd == -1) {
        return fd;
    }
    FdMgr::GetInstance()->get(fd, true);
    return fd;
}

/**
 * @brief 带超时控制的连接，使用同步代码风格实现异步非阻塞的高性能网络连接，并且精确控制连接超时时间。
 * * 场景： 异步连接服务器。
 * 细节：
 * 1. 发起非阻塞连接。
 * 2. 如果 EINPROGRESS（非阻塞连接正在握手），则注册 WRITE 事件和定时器。
 * 3. 挂起协程，等待连接成功或超时。
 */
int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms) {
    if (!t_hook_enable) {
        return connect_f(fd, addr, addrlen);
    }

    // 获取文件句柄上下文，依次检查文件描述符有效性、是否关闭、是否为 Socket 以及是否为用户非阻塞
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
    if (!ctx || ctx->isClose()) {
        errno = EBADF;
        return -1;
    }

    if (!ctx->isSocket()) {
        return connect_f(fd, addr, addrlen);
    }

    if (ctx->getUserNonblock()) {
        return connect_f(fd, addr, addrlen);
    }
    // 检查完毕，开始发起连接

    int n = connect_f(fd, addr, addrlen);   // Hook connet
    if (n == 0) {
        return 0;
    } else if (n != -1 || errno != EINPROGRESS) {
        return n;
    }

    // 操作系统正在进行非阻塞连接（正在三次握手），注册写事件和定时器
    IOManager *iom = IOManager::GetThis();
    Timer::ptr timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);

    if (timeout_ms != (uint64_t)-1) {
        timer = iom->addConditionTimer(
            timeout_ms,
            [winfo, fd, iom]() {
                auto t = winfo.lock();
                if (!t || t->cancelled) {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                // 【修正】使用 monsoon::WRITE
                iom->cancelEvent(fd, monsoon::WRITE);
            },
            winfo);
    }

    // 使用 monsoon::WRITE注册写事件，等待连接完成
    int rt = iom->addEvent(fd, monsoon::WRITE);
    if (rt == 0) {
        Fiber::GetThis()->yield();
        if (timer) {
            timer->cancel();
        }
        if (tinfo->cancelled) {
            errno = tinfo->cancelled;
            return -1;
        }
    } else {
        if (timer) {
            timer->cancel();
        }
        std::cout << "connect addEvent(" << fd << ", WRITE) error" << std::endl;
    }

    int error = 0;
    socklen_t len = sizeof(int);
    if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) {
        return -1;
    }
    if (!error) {
        return 0;
    } else {
        errno = error;
        return -1;
    }
}

/**
 * @brief 标准 Connect Hook
 * * 场景： 默认超时时间的连接。
 */
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return monsoon::connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

/**
 * @brief Accept Hook
 * * 场景： 接受新连接。
 * 细节：
 * 1. 委托 do_io 处理 READ 事件。
 * 2. 成功后将返回的新 fd 注册到 FdMgr。
 */
int accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
    // 【修正】使用 monsoon::READ
    int fd = do_io(s, accept_f, "accept", monsoon::READ, SO_RCVTIMEO, addr, addrlen);
    if (fd >= 0) {
        FdMgr::GetInstance()->get(fd, true);
    }
    return fd;
}

/**
 * @brief Read Hook
 * * 场景： 读数据。
 */
ssize_t read(int fd, void *buf, size_t count) { 
    return do_io(fd, read_f, "read", monsoon::READ, SO_RCVTIMEO, buf, count); 
}

/**
 * @brief Readv Hook
 * * 场景： 分散读。
 */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return do_io(fd, readv_f, "readv", monsoon::READ, SO_RCVTIMEO, iov, iovcnt);
}

/**
 * @brief Recv Hook
 * * 场景： 接收数据。
 */
ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return do_io(sockfd, recv_f, "recv", monsoon::READ, SO_RCVTIMEO, buf, len, flags);
}

/**
 * @brief Recvfrom Hook
 * * 场景： 接收数据并获取源地址 (UDP)。
 */
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    return do_io(sockfd, recvfrom_f, "recvfrom", monsoon::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
}

/**
 * @brief Recvmsg Hook
 * * 场景： 通用接收消息。
 */
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    return do_io(sockfd, recvmsg_f, "recvmsg", monsoon::READ, SO_RCVTIMEO, msg, flags);
}

/**
 * @brief Write Hook
 * * 场景： 写数据。
 */
ssize_t write(int fd, const void *buf, size_t count) {
    return do_io(fd, write_f, "write", monsoon::WRITE, SO_SNDTIMEO, buf, count);
}

/**
 * @brief Writev Hook
 * * 场景： 聚集写。
 */
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return do_io(fd, writev_f, "writev", monsoon::WRITE, SO_SNDTIMEO, iov, iovcnt);
}

/**
 * @brief Send Hook
 * * 场景： 发送数据。
 */
ssize_t send(int s, const void *msg, size_t len, int flags) {
    return do_io(s, send_f, "send", monsoon::WRITE, SO_SNDTIMEO, msg, len, flags);
}

/**
 * @brief Sendto Hook
 * * 场景： 发送数据到指定地址 (UDP)。
 */
ssize_t sendto(int s, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
    return do_io(s, sendto_f, "sendto", monsoon::WRITE, SO_SNDTIMEO, msg, len, flags, to, tolen);
}

/**
 * @brief Sendmsg Hook
 * * 场景： 通用发送消息。
 */
ssize_t sendmsg(int s, const struct msghdr *msg, int flags) {
    return do_io(s, sendmsg_f, "sendmsg", monsoon::WRITE, SO_SNDTIMEO, msg, flags);
}

/**
 * @brief Close Hook
 * * 场景： 关闭文件描述符。
 * 细节：
 * 1. 取消 IOManager 中该 fd 的所有事件监听。
 * 2. 从 FdMgr 中移除 fd。
 */
int close(int fd) {
    if (!t_hook_enable) {
        return close_f(fd);
    }
    FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
    if (ctx) {
        auto iom = IOManager::GetThis();
        if (iom) {
            iom->cancelAll(fd);
        }
        FdMgr::GetInstance()->del(fd);
    }
    return close_f(fd);
}

/**
 * @brief Fcntl Hook
 * * 场景： 控制文件描述符属性。
 * 细节：
 * 1. 拦截 F_SETFL，感知用户设置的 O_NONBLOCK 标志。
 * 2. 在底层始终保持 O_NONBLOCK，在应用层模拟用户的阻塞/非阻塞设置。
 */
int fcntl(int fd, int cmd, ... /* arg */) {
    va_list va;
    va_start(va, cmd);
    switch (cmd) {
        case F_SETFL: {
            int arg = va_arg(va, int);
            va_end(va);
            FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
            if (!ctx || ctx->isClose() || !ctx->isSocket()) {
                return fcntl_f(fd, cmd, arg);
            }
            ctx->setUserNonblock(arg & O_NONBLOCK);
            if (ctx->getSysNonblock()) {
                arg |= O_NONBLOCK;
            } else {
                arg &= ~O_NONBLOCK;
            }
            return fcntl_f(fd, cmd, arg);
        } break;
        case F_GETFL: {
            va_end(va);
            int arg = fcntl_f(fd, cmd);
            FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
            if (!ctx || ctx->isClose() || !ctx->isSocket()) {
                return arg;
            }
            if (ctx->getUserNonblock()) {
                return arg | O_NONBLOCK;
            } else {
                return arg & ~O_NONBLOCK;
            }
        } break;
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
        {
            int arg = va_arg(va, int);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        } break;
        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
        {
            va_end(va);
            return fcntl_f(fd, cmd);
        } break;
        case F_SETLK:
        case F_SETLKW:
        case F_GETLK: {
            struct flock *arg = va_arg(va, struct flock *);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        } break;
        case F_GETOWN_EX:
        case F_SETOWN_EX: {
            struct f_owner_exlock *arg = va_arg(va, struct f_owner_exlock *);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        } break;
        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }
}

/**
 * @brief Ioctl Hook
 * * 场景： 控制设备属性。
 * 细节：
 * 1. 拦截 FIONBIO，感知用户设置的阻塞标志。
 */
int ioctl(int d, unsigned long int request, ...) {
    va_list va;
    va_start(va, request);
    void *arg = va_arg(va, void *);
    va_end(va);

    if (FIONBIO == request) {
        bool user_nonblock = !!*(int *)arg;
        FdCtx::ptr ctx = FdMgr::GetInstance()->get(d);
        if (!ctx || ctx->isClose() || !ctx->isSocket()) {
            return ioctl_f(d, request, arg);
        }
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(d, request, arg);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

/**
 * @brief Setsockopt Hook
 * * 场景： 设置 socket 选项。
 * 细节：
 * 1. 拦截 SO_RCVTIMEO/SO_SNDTIMEO，将超时时间保存到 FdCtx 中。
 */
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    if (!t_hook_enable) {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            FdCtx::ptr ctx = FdMgr::GetInstance()->get(sockfd);
            if (ctx) {
                const timeval *v = (const timeval *)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}

// ================= 新增的关键系统调用 Hook =================

/**
 * @brief 复制文件描述符 (dup)
 * @param oldfd 旧的文件描述符
 * @return int 新的文件描述符
 * * 场景： 复制 socket fd。
 * 细节：
 * 1. 执行原 dup 调用。
 * 2. 如果成功，务必在 FdMgr 中注册新 fd，否则后续对新 fd 的 IO 将无法被 Hook。
 */
int dup(int oldfd) {
    if (!t_hook_enable) {
        return dup_f(oldfd);
    }
    int newfd = dup_f(oldfd);
    if (newfd >= 0) {
        // 关键步骤：在 FdMgr 中初始化新的 FdContext
        // get(newfd, true) 会自动判断该 fd 是否为 Socket 并初始化上下文
        FdMgr::GetInstance()->get(newfd, true);
    }
    return newfd;
}

/**
 * @brief 复制文件描述符到指定 fd (dup2)
 * @param oldfd 旧 fd
 * @param newfd 目标 fd
 * @return int 成功返回 newfd
 * * 场景： 重定向标准输入输出，或复用 fd。
 * 细节：
 * 1. 执行原 dup2。
 * 2. 如果成功，确保 FdMgr 能够管理 newfd。
 * 3. 注意：如果 newfd 之前是打开的，dup2 会自动关闭它，FdMgr 重新 get 时会覆盖旧的上下文。
 */
int dup2(int oldfd, int newfd) {
    if (!t_hook_enable) {
        return dup2_f(oldfd, newfd);
    }
    int ret = dup2_f(oldfd, newfd);
    if (ret >= 0) {
        // 确保新 fd 被 FdMgr 纳管
        FdMgr::GetInstance()->get(newfd, true);
    }
    return ret;
}

} // extern "C"

}  // namespace monsoon