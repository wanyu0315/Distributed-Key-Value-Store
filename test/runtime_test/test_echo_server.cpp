#include "monsoon.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

using namespace monsoon;

void handle_client(int client_fd) {
    // 设置 Hook 状态（虽然 Scheduler::run 里已经设了，但确保万无一失）
    set_hook_enable(true);

    std::cout << "Handle client: " << client_fd << " in Thread: " << GetThreadId() << std::endl;

    char buf[1024];
    while (true) {
        // 【关键】这里的 recv 会被 Hook 拦截
        // 如果没数据：Hook 会把当前协程 yield，注册 epoll 事件
        // 如果有数据：epoll 唤醒协程，recv 返回
        int n = recv(client_fd, buf, sizeof(buf), 0);

        if (n > 0) {
            // Echo 回去
            send(client_fd, buf, n, 0);
        } else if (n == 0) {
            std::cout << "Client closed: " << client_fd << std::endl;
            break;
        } else {
            if (errno == EAGAIN) {
                // 正常情况，但在 Hook 模式下通常不会返回 EAGAIN 给上层，除非超时
                continue;
            }
            std::cout << "Recv error: " << strerror(errno) << std::endl;
            break;
        }
    }
    close(client_fd);
}

void run_server() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    // 设为非阻塞（Hook 的前提）
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;

    int rt = bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    if (rt != 0) {
        perror("bind");
        return;
    }

    rt = listen(listen_fd, 1024);
    if (rt != 0) {
        perror("listen");
        return;
    }

    std::cout << "Server listening on 8080..." << std::endl;

    IOManager iom(2); // 开启2个IO线程进行处理

    while (true) {
        // accept 本身也是阻塞的，需要 Hook 支持
        // 或者你可以在这里手动 yield，但通常我们 Hook accept
        sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);

        if (client_fd > 0) {
            // 设为非阻塞
            fcntl(client_fd, F_SETFL, O_NONBLOCK);
            
            // 【关键】将新连接分配给 IOManager 调度
            // 调度器会自动把这个任务分配给某个 Worker 线程
            // 实现负载均衡
            iom.schedule(std::bind(handle_client, client_fd));
        } else {
             if (errno == EAGAIN) {
                 // 没人连，让出 CPU 等一会（或者这里应该用 IOManager::addEvent 监听 listen_fd）
                 // 简易写法：
                 usleep(1000); 
             }
        }
    }
}

int main() {
    run_server();
    return 0;
}