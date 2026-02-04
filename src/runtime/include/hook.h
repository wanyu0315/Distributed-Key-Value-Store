#ifndef __MONSOON_HOOK_H__
#define __MONSOON_HOOK_H__

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

namespace monsoon {

/**
 * @brief 获取当前线程是否启用 Hook
 * @return true 启用
 * @return false 未启用
 * * 场景： 某些关键的系统日志操作可能需要临时关闭 Hook 以避免递归 Hook。
 */
bool is_hook_enable();

/**
 * @brief 设置当前线程的 Hook 状态
 * @param flag true: 开启, false: 关闭
 */
void set_hook_enable(bool flag);

}  // namespace monsoon

extern "C" {

// ================= Sleep 相关接口 =================
typedef unsigned int (*sleep_fun)(unsigned int seconds);
extern sleep_fun sleep_f;

typedef int (*usleep_fun)(useconds_t usec);
extern usleep_fun usleep_f;

typedef int (*nanosleep_fun)(const struct timespec *req, struct timespec *rem);
extern nanosleep_fun nanosleep_f;

// ================= Socket 相关接口 =================
typedef int (*socket_fun)(int domain, int type, int protocol);
extern socket_fun socket_f;

typedef int (*connect_fun)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
extern connect_fun connect_f;

typedef int (*accept_fun)(int s, struct sockaddr *addr, socklen_t *addrlen);
extern accept_fun accept_f;

// ================= Read 相关接口 =================
typedef ssize_t (*read_fun)(int fd, void *buf, size_t count);
extern read_fun read_f;

typedef ssize_t (*readv_fun)(int fd, const struct iovec *iov, int iovcnt);
extern readv_fun readv_f;

typedef ssize_t (*recv_fun)(int sockfd, void *buf, size_t len, int flags);
extern recv_fun recv_f;

typedef ssize_t (*recvfrom_fun)(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
extern recvfrom_fun recvfrom_f;

typedef ssize_t (*recvmsg_fun)(int sockfd, struct msghdr *msg, int flags);
extern recvmsg_fun recvmsg_f;

// ================= Write 相关接口 =================
typedef ssize_t (*write_fun)(int fd, const void *buf, size_t count);
extern write_fun write_f;

typedef ssize_t (*writev_fun)(int fd, const struct iovec *iov, int iovcnt);
extern writev_fun writev_f;

typedef ssize_t (*send_fun)(int s, const void *msg, size_t len, int flags);
extern send_fun send_f;

typedef ssize_t (*sendto_fun)(int s, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
extern sendto_fun sendto_f;

typedef ssize_t (*sendmsg_fun)(int s, const struct msghdr *msg, int flags);
extern sendmsg_fun sendmsg_f;

// ================= Control 相关接口 =================
typedef int (*close_fun)(int fd);
extern close_fun close_f;

typedef int (*fcntl_fun)(int fd, int cmd, ... /* arg */);
extern fcntl_fun fcntl_f;

typedef int (*ioctl_fun)(int d, unsigned long int request, ...);
extern ioctl_fun ioctl_f;

typedef int (*getsockopt_fun)(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
extern getsockopt_fun getsockopt_f;

typedef int (*setsockopt_fun)(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
extern setsockopt_fun setsockopt_f;

// ================= File Descriptor 操作接口 (新增) =================
typedef int (*dup_fun)(int oldfd);
extern dup_fun dup_f;

typedef int (*dup2_fun)(int oldfd, int newfd);
extern dup2_fun dup2_f;

/**
 * @brief 带超时的 connect
 * @details 这是一个非标准接口，但在 Hook 模块中暴露出来便于应用层控制连接超时
 */
extern int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms);

}

#endif