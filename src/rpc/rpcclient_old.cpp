#include "rpcclient_old.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "rpccontroller.h"
#include "rpcheader.pb.h"

// ============================================================================
// 辅助工具：从缓冲区中 Peek Varint32
// ============================================================================
/**
 * @brief 从字符串缓冲区中偷看（不消费）一个 varint32 值
 * @details
 * Varint 是 Protobuf 的变长整数编码：
 * - 小于 128 的数用 1 字节编码
 * - 大数用 2-5 字节编码
 * 这个函数只读取但不移动缓冲区指针，用于判断数据是否完整
 * 
 * @param buffer 数据缓冲区
 * @param offset 从哪个位置开始读取
 * @param value [输出] 解析出的 varint32 值
 * @param varint_size [输出] 这个 varint 占用了多少字节
 * @return true 成功读取, false 数据不完整
 */
static bool PeekVarint32FromString(const std::string& buffer, 
                                   size_t offset,
                                   uint32_t& value, 
                                   size_t& varint_size) {
  // 边界检查
  if (offset >= buffer.size()) return false;

  const char* data = buffer.data() + offset;
  size_t readable = buffer.size() - offset;
  
  if (readable == 0) return false;

  // Varint32 最多占用 5 字节，我们只读取当前可用的数据
  google::protobuf::io::ArrayInputStream array_input(
      data, std::min(readable, size_t(5)));
  google::protobuf::io::CodedInputStream coded_input(&array_input); //把coded_input指向array_input，设置为只能读取长度大于5字节的数据
  
  // 尝试读取 varint32
  if (!coded_input.ReadVarint32(&value)) {
    return false;  // 数据不够，无法完整读取 varint
  }
  
  // 记录这个 varint 实际占用的字节数
  varint_size = coded_input.CurrentPosition();
  return true;
}

// ============================================================================
// MprpcChannel 实现
// ============================================================================

/**
 * @brief 构造函数：初始化 RPC 客户端通道
 * @param ip 服务端 IP 地址
 * @param port 服务端端口
 * @param config 客户端配置（超时、重试等）
 * @param connect_now 是否立即连接（false 则延迟到第一次 RPC 调用时连接）
 */
MprpcChannel::MprpcChannel(const std::string& ip, uint16_t port,
                          const RpcClientConfig& config,
                          bool connect_now)
    : m_clientFd(-1), // 初始化成员变量（初始化列表）
      m_ip(ip),       // 保存远程 RPC 服务地址
      m_port(port),   // 保存远程 RPC 服务端口
      m_config(config) {
  
  // 如果设置为立即连接则立即连接（否则延迟到第一次调用时连接）
  if (connect_now) {  
    std::string err_msg;
    if (!Connect(ip.c_str(), port, &err_msg)) {
      std::cerr << "[MprpcChannel] Failed to connect: " << err_msg << std::endl;
      
      // 如果启用了自动重连，尝试重连
      if (m_config.enable_auto_reconnect) {
        std::cerr << "[MprpcChannel] Retrying connection..." << std::endl;
        Reconnect(&err_msg);
      }
    } else {
      std::cout << "[MprpcChannel] Connected to " << ip << ":" << port << std::endl;
    }
  }
}

/**
 * @brief 析构函数：确保资源释放
 */
MprpcChannel::~MprpcChannel() {
  Close();
}

/**
 * @brief 关闭连接
 */
void MprpcChannel::Close() {
  if (m_clientFd != -1) {
    close(m_clientFd);
    m_clientFd = -1;
    std::cout << "[MprpcChannel] Connection closed" << std::endl;
  }
}

// ============================================================================
// 连接管理
// ============================================================================

/**
 * @brief 建立 TCP 连接到服务端
 * @details
 * 实现要点：
 * 1. 使用非阻塞 socket + select 实现连接超时控制
 * 2. 连接成功后恢复阻塞模式
 * 3. 设置读写超时，防止 recv/send 无限阻塞
 * 
 * @param ip IP 地址（点分十进制字符串）
 * @param port 端口号
 * @param err_msg [输出] 错误信息
 * @return true 连接成功, false 连接失败
 */
bool MprpcChannel::Connect(const char* ip, uint16_t port, std::string* err_msg) {
  // 1. 创建 TCP socket（创建的是一个socket描述符）
  int client_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (client_fd == -1) {
    *err_msg = std::string("socket() failed: ") + strerror(errno);
    return false;
  }

  // 2. 设置非阻塞模式（用于连接超时控制）
  int flags = fcntl(client_fd, F_GETFL, 0);
  fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

  // 3. 连接服务器
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = inet_addr(ip);

  int ret = connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)); // Linux 提供的全局函数 connect()
  
  // 非阻塞 connect 会立即返回 -1，errno 为 EINPROGRESS
  if (ret == -1 && errno != EINPROGRESS) { 
    // 真失败（不是正在连接中）
    *err_msg = std::string("connect() failed: ") + strerror(errno);
    close(client_fd);
    return false;
  }

  // 4. 使用 select 等待连接完成（带超时）
  if (ret == -1) {  // EINPROGRESS，连接正在进行中
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(client_fd, &write_fds);

    // 设置超时时间
    struct timeval timeout;
    timeout.tv_sec = m_config.connect_timeout_ms / 1000;    // 配置时间（毫秒）转换为秒部分
    timeout.tv_usec = (m_config.connect_timeout_ms % 1000) * 1000;  // 将毫秒的余数转换为微秒

    // select 监听 socket 可写（表示connect连接完成）
    ret = select(client_fd + 1, nullptr, &write_fds, nullptr, &timeout);
    
    if (ret <= 0) {
      *err_msg = ret == 0 ? "connect timeout" : std::string("select failed: ") + strerror(errno);
      close(client_fd);
      return false;
    }

    // 5. 检查连接是否真正成功（可能连接失败但 socket 可写）
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(client_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
      *err_msg = std::string("connect failed: ") + strerror(error);
      close(client_fd);
      return false;
    }
  }

  // 6. 恢复阻塞模式（后续 recv/send 使用阻塞方式）
  fcntl(client_fd, F_SETFL, flags);

  // 7. 设置读写超时（防止 recv/send 无限阻塞）
  if (!SetSocketTimeout(m_config.rpc_timeout_ms)) {
    std::cerr << "[MprpcChannel] Warning: failed to set socket timeout" << std::endl;
  }

  m_clientFd = client_fd;
  return true;
}

/**
 * @brief 设置当前这一个 MprpcChannel 实例所持有的那个特定 Socket 连接（m_clientFd）的读写超时时间。
 * @param timeout_ms 超时时间（毫秒）
 * @return true 设置成功, false 设置失败
 */
bool MprpcChannel::SetSocketTimeout(int timeout_ms) {
  if (m_clientFd == -1) return false;

  struct timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;

  // 设置接收超时
  if (setsockopt(m_clientFd, SOL_SOCKET, SO_RCVTIMEO, 
                 &timeout, sizeof(timeout)) < 0) {
    return false;
  }

  // 设置发送超时
  if (setsockopt(m_clientFd, SOL_SOCKET, SO_SNDTIMEO, 
                 &timeout, sizeof(timeout)) < 0) {
    return false;
  }

  return true;
}

/**
 * @brief 重连机制（带指数退避重试）
 * @details
 * 重试策略：
 * - 第 1 次重试：等待 100ms
 * - 第 2 次重试：等待 200ms
 * - 第 3 次重试：等待 400ms
 * 
 * @param err_msg [输出] 错误信息
 * @return true 重连成功, false 重连失败
 */
bool MprpcChannel::Reconnect(std::string* err_msg) {
  Close();  // 先关闭旧连接

  for (int i = 0; i < m_config.max_retry_times; ++i) {
    std::cout << "[MprpcChannel] Reconnect attempt " << (i + 1) 
              << "/" << m_config.max_retry_times << std::endl;
    
    if (Connect(m_ip.c_str(), m_port, err_msg)) {
      std::cout << "[MprpcChannel] Reconnected successfully" << std::endl;
      return true;
    }
    
    // 重试前等待（指数退避）
    if (i < m_config.max_retry_times - 1) {
      usleep(100000 * (1 << i));  // 100ms, 200ms, 400ms...
    }
  }

  return false;
}

// ============================================================================
// 核心 RPC 调用逻辑
// ============================================================================

/**
 * @brief RPC 调用的统一入口（由 Protobuf Stub 自动调用）
 * @details
 * 流程：
 * 1. 检查连接状态，必要时重连
 * 2. 生成唯一的 request_id
 * 3. 序列化请求并发送
 * 4. 接收并解析响应
 * 5. 匹配 request_id 并反序列化响应
 * 6. 触发回调（异步）或唤醒等待线程（同步）
 * 
 * @param method 方法描述符
 * @param controller 控制器（用于错误报告）
 * @param request 请求对象
 * @param response 响应对象
 * @param done 完成回调（nullptr 表示同步调用）
 */
void MprpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                              google::protobuf::RpcController* controller,
                              const google::protobuf::Message* request,
                              google::protobuf::Message* response,
                              google::protobuf::Closure* done) {
  // 0. 顺便清理一下之前的僵尸请求
  CleanupTimeoutRequests();
  
  // 1. 检查连接状态，必要时重连
  if (m_clientFd == -1) {
    std::string err_msg;
    if (!Reconnect(&err_msg)) {
      std::string error = "[CallMethod] Connection failed: " + err_msg;
      std::cerr << error << std::endl;
      controller->SetFailed(error);
      if (done) done->Run();
      return;
    }
  }

  // 2. 获取服务名和方法名
  const google::protobuf::ServiceDescriptor* service_desc = method->service();
  std::string service_name = service_desc->name();
  std::string method_name = method->name();

  // 3. 生成唯一的请求 ID（用于异步响应匹配）
  uint64_t request_id = GenerateRequestId();

  // 4. 创建请求上下文（用于异步匹配和同步等待）
  // ctx 存放着一次 RPC 调用的所有相关信息
  auto ctx = std::make_shared<PendingRpcContext>();
  ctx->request_id = request_id;
  ctx->response = response;
  ctx->controller = controller;
  ctx->done = done;
  ctx->start_time = std::chrono::steady_clock::now();

  // 5. 注册待处理请求（加入待处理队列）
  RegisterPendingRequest(request_id, ctx);

  // 6. 发送请求
  if (!SendRequest(request_id, service_name, method_name, request, controller)) {
    // 发送失败，移除待处理请求
    {
      std::lock_guard<std::mutex> lock(m_pending_mutex);
      m_pending_requests.erase(request_id);
    }
    // 接收失败分支也会调用 done->Run()，也属于“RPC 已结束”状态，必须通知上层调用者
    if (done) done->Run();
    return;
  }

  // 7. 接收服务端的响应（ReceiveResponse将循环读取直到解析出至少一个完整响应）
  if (!ReceiveResponse(controller)) {
    // 接收失败，移除待处理请求
    {
      std::lock_guard<std::mutex> lock(m_pending_mutex);
      m_pending_requests.erase(request_id);
    }
    if (done) done->Run();
    return;
  }

  // 8. 如果是同步调用（done == nullptr），同步 RPC要用条件变量等待结果
  if (done == nullptr) {
    std::unique_lock<std::mutex> lock(ctx->mutex);
    // 超时等待
    auto timeout = std::chrono::milliseconds(m_config.rpc_timeout_ms);
    // 等待条件变量唤醒，但最多等 timeout 毫秒
    if (!ctx->cv.wait_for(lock, timeout, [&]{ return ctx->finished; })) {
      controller->SetFailed("RPC timeout");
      std::cerr << "[CallMethod] Request " << request_id << " timeout" << std::endl;
    }
  }

  // 9. 如果done != nullptr，是异步RPC，等待网络线程触发异步回调 done->Run()，也就是一次RPC调用生命周期结束后触发的回调
  if (done) {
    done->Run();
  }
}

// ============================================================================
// 请求发送
// ============================================================================

/**
 * @brief 发送 RPC 请求（带帧头）
 * @details
 * 协议格式：[Varint32: header_size] + [RpcHeader] + [Args]
 * 
 * RpcHeader 包含：
 * - service_name: 服务名
 * - method_name: 方法名
 * - args_size: 参数长度
 * - request_id: 请求 ID（用于异步匹配）
 * 
 * @param request_id 请求 ID
 * @param service_name 服务名
 * @param method_name 方法名
 * @param request 请求对象
 * @param controller 控制器
 * @return true 发送成功, false 发送失败
 */
bool MprpcChannel::SendRequest(uint64_t request_id,
                               const std::string& service_name,
                               const std::string& method_name,
                               const google::protobuf::Message* request,
                               google::protobuf::RpcController* controller) {
  // 1. 序列化请求参数
  std::string args_str;
  if (!request->SerializeToString(&args_str)) {
    controller->SetFailed("Failed to serialize request");
    return false;
  }

  // 2. 构造 RpcHeader
  RPC::RpcHeader rpc_header;
  rpc_header.set_service_name(service_name);
  rpc_header.set_method_name(method_name);
  rpc_header.set_args_size(args_str.size());
  rpc_header.set_request_id(request_id);  // ✅ 关键：设置请求 ID

  // 3. 序列化 RpcHeader
  std::string header_str;
  if (!rpc_header.SerializeToString(&header_str)) {
    controller->SetFailed("Failed to serialize RPC header");
    return false;
  }

  // 4. 构造完整的请求帧：[Varint32: header_size] + [Header] + [Args]
  std::string send_buffer;
  {
    google::protobuf::io::StringOutputStream string_output(&send_buffer); // 将 string 包装成 protobuf 支持的输出流
    google::protobuf::io::CodedOutputStream coded_output(&string_output); // 给“流”增加了“编码能力”和“缓冲能力”
    
    // 写入 header 长度（varint 编码，占 1-5 字节）
    coded_output.WriteVarint32(header_str.size());
    // CodedOutputStream 析构时会自动 flush，coded_output会让适配器string_output赶紧取走数据（header长度）
  }
  
  // 追加 header 和 args
  send_buffer.append(header_str);
  send_buffer.append(args_str);

  // 5. 发送数据（循环发送，处理部分发送的情况）
  size_t total_sent = 0;
  while (total_sent < send_buffer.size()) {
    ssize_t sent = send(m_clientFd, 
                       send_buffer.data() + total_sent,
                       send_buffer.size() - total_sent, 
                       0);
    
    if (sent <= 0) {
      std::string error = std::string("send() failed: ") + strerror(errno);
      std::cerr << "[SendRequest] " << error << std::endl;
      controller->SetFailed(error);
      
      // 发送失败时关闭连接（下次会自动重连）
      Close();
      return false;
    }
    
    total_sent += sent;
  }

  std::cout << "[SendRequest] Sent request " << request_id 
            << " (" << send_buffer.size() << " bytes)" << std::endl;
  return true;
}

// ============================================================================
// 响应接收 (核心：处理拆包/粘包)
// ============================================================================

/**
 * @brief 接收 RPC 响应（处理 TCP 拆包/粘包）
 * @details
 * 核心逻辑：
 * 1. 循环从 socket 读取数据到内部缓冲区
 * 2. 尝试从缓冲区解析完整的响应包
 * 3. 解析成功则完成对应的请求，解析失败则继续读取
 * 
 * 处理场景：
 * - 粘包：一次 recv 收到多个响应 → 循环解析
 * - 拆包：一个响应分多次 recv → 累积到缓冲区
 * - 半包：Header 或 Data 不完整 → 等待更多数据
 * 
 * @param controller 控制器
 * @return true 至少成功处理了一个响应, false 接收失败
 */
bool MprpcChannel::ReceiveResponse(google::protobuf::RpcController* controller) {
  // 循环读取数据，直到解析出至少一个完整的响应
  while (true) {
    // 1. 尝试从现有缓冲区解析响应
    uint64_t request_id = 0;
    int32_t error_code = 0;
    std::string error_msg;
    std::string response_data;

    {
      std::lock_guard<std::mutex> lock(m_recv_mutex);
      if (TryParseResponse(request_id, error_code, error_msg, response_data)) {
        // 解析成功，完成该请求
        CompletePendingRequest(request_id, error_code, error_msg, response_data);
        return true;  // 至少成功处理了一个响应
      }
    }

    // 2. 缓冲区数据不足，从 socket 读取更多数据
    if (!ReadToBuffer()) {
      controller->SetFailed("Failed to read response from server");
      return false;
    }
  }
}

/**
 * @brief 从 socket 读取数据到内部缓冲区
 * @details
 * 每次读取最多 4KB 数据，追加到 m_recv_buffer
 * 
 * @return true 读取成功, false 读取失败或连接关闭
 */
bool MprpcChannel::ReadToBuffer() {
  char temp_buf[4096];
  ssize_t n = recv(m_clientFd, temp_buf, sizeof(temp_buf), 0);
  
  if (n > 0) {
    // 读取成功，追加到缓冲区
    std::lock_guard<std::mutex> lock(m_recv_mutex);
    m_recv_buffer.append(temp_buf, n);
    std::cout << "[ReadToBuffer] Read " << n << " bytes, buffer size: " 
              << m_recv_buffer.size() << std::endl;
    return true;
  } else if (n == 0) {
    // 连接被对端关闭
    std::cerr << "[ReadToBuffer] Connection closed by peer" << std::endl;
    Close();
    return false;
  } else {
    // 读取错误或超时
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      std::cerr << "[ReadToBuffer] Recv timeout" << std::endl;
    } else {
      std::cerr << "[ReadToBuffer] Recv error: " << strerror(errno) << std::endl;
    }
    return false;
  }
}

// ============================================================================
// 响应解析 (协议解析器)
// ============================================================================

/**
 * @brief 尝试从接收缓冲区解析一个完整的响应包
 * @details
 * 协议格式：[Varint32: header_size] + [RpcHeader] + [Response Data]
 * 
 * 解析策略：Peek-Check-Consume
 * 1. Peek Varint32（不移动指针）
 * 2. Check 数据完整性（Header 和 Data 是否都收齐）
 * 3. Consume 数据（解析并移除已处理的数据）
 * 
 * 注意：调用者已对 m_recv_mutex 加锁
 * 
 * @param request_id [输出] 请求 ID
 * @param error_code [输出] 错误码（0 表示成功）
 * @param error_msg [输出] 错误信息
 * @param response_data [输出] 响应数据
 * @return true 解析成功, false 数据不完整
 */
bool MprpcChannel::TryParseResponse(uint64_t& request_id,
                                   int32_t& error_code,
                                   std::string& error_msg,
                                   std::string& response_data) {
  // 注意：调用者已对 m_recv_mutex 加锁

  // ========== 阶段 1: Peek Varint32 (Header 长度) ==========
  size_t offset = 0;
  uint32_t header_size = 0;
  size_t varint_size = 0;

  if (!PeekVarint32FromString(m_recv_buffer, offset, header_size, varint_size)) {
    // 数据不够，连 varint 都读不出来
    return false;
  }

  // 校验 header 大小（防止恶意大包）
  if (header_size == 0 || header_size > m_config.max_message_size) {
    std::cerr << "[TryParseResponse] Invalid header size: " << header_size << std::endl;
    // 丢弃损坏的数据
    m_recv_buffer.erase(0, varint_size);
    throw std::runtime_error("Invalid response header size");
  }

  offset += varint_size;

  // ========== 阶段 2: 检查是否有完整的 Header ==========
  if (m_recv_buffer.size() < offset + header_size) {
    // Header 不完整，等待更多数据
    return false;
  }

  // ========== 阶段 3: 解析 RpcHeader ==========
  std::string header_str = m_recv_buffer.substr(offset, header_size);
  offset += header_size;

  RPC::RpcHeader rpc_header;
  if (!rpc_header.ParseFromString(header_str)) {
    std::cerr << "[TryParseResponse] Failed to parse RPC header" << std::endl;
    m_recv_buffer.erase(0, offset);
    throw std::runtime_error("Invalid RPC header");
  }

  // 提取 Header 中的信息
  request_id = rpc_header.request_id();
  error_code = rpc_header.error_code();
  error_msg = rpc_header.error_msg();
  uint32_t args_size = rpc_header.args_size();

  // 校验 args_size
  if (args_size > m_config.max_message_size) {
    std::cerr << "[TryParseResponse] Args size too large: " << args_size << std::endl;
    m_recv_buffer.erase(0, offset);
    throw std::runtime_error("Response too large");
  }

  // ========== 阶段 4: 检查是否有完整的 Response Data ==========
  if (m_recv_buffer.size() < offset + args_size) {
    // Response Data 不完整，等待更多数据
    return false;
  }

  // ========== 阶段 5: 读取 Response Data ==========
  response_data = m_recv_buffer.substr(offset, args_size);
  offset += args_size;

  // ========== 阶段 6: 消费已处理的数据 ==========
  m_recv_buffer.erase(0, offset);

  std::cout << "[TryParseResponse] Parsed response for request " << request_id
            << ", error_code=" << error_code 
            << ", data size=" << response_data.size() << std::endl;

  return true;
}

// ============================================================================
// 请求管理
// ============================================================================

/**
 * @brief 注册待处理请求
 * @param request_id 请求 ID
 * @param ctx 请求上下文
 */
void MprpcChannel::RegisterPendingRequest(uint64_t request_id,
                                         std::shared_ptr<PendingRpcContext> ctx) {
  std::lock_guard<std::mutex> lock(m_pending_mutex);
  m_pending_requests[request_id] = ctx;
  std::cout << "[RegisterPendingRequest] Registered request " << request_id << std::endl;
}

/**
 * @brief 完成待处理请求
 * @details
 * 流程：
 * 1. 从待处理队列中取出请求上下文（您的代码前半部分已完成）
 * 2. 处理错误码（如果有）
 * 3. 反序列化响应数据
 * 4. 唤醒等待线程（同步调用）
 * * @param request_id 请求 ID
 * @param error_code 错误码（0 表示成功）
 * @param error_msg 错误信息
 * @param response_data 响应数据
 */
void MprpcChannel::CompletePendingRequest(uint64_t request_id,
                                         int32_t error_code,
                                         const std::string& error_msg,
                                         const std::string& response_data) {
  std::shared_ptr<PendingRpcContext> ctx;
  
  // 1. 从待处理队列中取出请求上下文
  {
    std::lock_guard<std::mutex> lock(m_pending_mutex);
    auto it = m_pending_requests.find(request_id);
    if (it == m_pending_requests.end()) {
      std::cerr << "[CompletePendingRequest] Request " << request_id 
                << " not found (may have timed out or removed)" << std::endl;
      return;
    }
    ctx = it->second;
    m_pending_requests.erase(it); // 从 Map 中移除，防止超时检查再次处理
  }

  // 2. 锁定上下文互斥锁，准备更新状态
  // 使用 unique_lock 是为了配合条件变量通知
  std::unique_lock<std::mutex> lock(ctx->mutex);

  // 3. 判断 RPC 调用结果
  if (error_code != 0) {
    // case A: 框架层返回了错误（如服务不存在、服务端内部错误）
    std::string err_info = "RPC Failed (Code: " + std::to_string(error_code) + "): " + error_msg;
    std::cerr << "[CompletePendingRequest] " << err_info << std::endl;
    ctx->controller->SetFailed(err_info);
  } else {
    // case B: RPC 成功，尝试反序列化业务数据
    if (!ctx->response->ParseFromString(response_data)) {
      std::string err_info = "Failed to parse response data for request " + std::to_string(request_id);
      std::cerr << "[CompletePendingRequest] " << err_info << std::endl;
      ctx->controller->SetFailed(err_info);
    } else {
      // 成功！
      // std::cout << "[CompletePendingRequest] Request " << request_id << " completed successfully" << std::endl;
    }
  }

  // 4. 更新完成状态并唤醒等待线程
  ctx->finished = true;
  ctx->cv.notify_all(); // 唤醒 CallMethod 中的 wait_for

  // 注意：如果是纯异步回调模式 (done != nullptr)，
  // 通常在这里或者 CallMethod 此时被唤醒后执行 done->Run()。
  // 根据您的 CallMethod 实现，done->Run() 是在 CallMethod 尾部执行的，
  // 所以这里只需要 notify 即可。
}

/**
 * @brief 清理超时请求
 * @details
 * 遍历 pending_requests map，检查 start_time 是否超过配置的 rpc_timeout_ms。
 * 如果超时：
 * 1. 设置 controller 状态为失败
 * 2. 唤醒等待线程
 * 3. 从 map 中移除
 */
void MprpcChannel::CleanupTimeoutRequests() {
  auto now = std::chrono::steady_clock::now();
  auto timeout_duration = std::chrono::milliseconds(m_config.rpc_timeout_ms);
  
  std::vector<uint64_t> timeout_ids;

  // 1. 扫描超时请求
  // 注意：为了减小锁粒度，我们先收集 ID，再统一移除，或者在锁内直接处理
  {
    std::lock_guard<std::mutex> lock(m_pending_mutex);
    
    for (auto it = m_pending_requests.begin(); it != m_pending_requests.end(); ) {
      auto ctx = it->second;
      
      // 检查是否超时
      if (now - ctx->start_time > timeout_duration) {
        std::cout << "[CleanupTimeoutRequests] Request " << it->first << " timed out" << std::endl;
        
        // 唤醒该请求的等待线程，告知超时
        {
          // 超时线程和 IO 线程（`CompletePendingRequest`）都会去抢这把锁，谁抢到另一方就会失去执行权。
          std::unique_lock<std::mutex> ctx_lock(ctx->mutex);
          if (!ctx->finished) { // 防止重复处理
            ctx->controller->SetFailed("RPC Request timed out (client-side cleanup)");
            ctx->finished = true; // 标记为true，抢夺失败的线程不会再处理它
            ctx->cv.notify_all();
          }
        }
        
        // 移除并获取下一个迭代器
        it = m_pending_requests.erase(it);
      } else {
        ++it;
      }
    }
  }
}