#ifndef RPCCLIENTOLD_H
#define RPCCLIENTOLD_H

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// ============================================================================
// RPC 客户端配置
// ============================================================================
struct RpcClientConfig {
  int connect_timeout_ms = 3000;      // 连接超时 (毫秒)
  int rpc_timeout_ms = 5000;          // RPC 调用超时 (毫秒)
  int max_retry_times = 3;            // 最大重试次数
  int max_message_size = 10 * 1024 * 1024;  // 最大消息 10MB

    // 新增：连接池配置
  int connection_pool_size = 4;        // 连接池大小（建议 = CPU 核数）
  int io_thread_pool_size = 2;         // IO 线程数（接收线程）

  bool enable_auto_reconnect = true;  // 是否自动重连
  bool enable_heartbeat = false;      // 是否启用心跳 (预留)
};

// ============================================================================
// 待处理的 RPC 请求上下文
// ============================================================================
struct PendingRpcContext {
  uint64_t request_id;                        // 请求 ID
  google::protobuf::Message* response;        // 响应对象指针 (外部持有)
  google::protobuf::RpcController* controller;// 控制器
  google::protobuf::Closure* done;            // 完成回调
  
  std::mutex mutex;                     // 同步等待锁
  std::condition_variable cv;           // 条件变量
  bool finished = false;                // 是否完成
  
  std::chrono::steady_clock::time_point start_time; // 请求开始时间
  std::chrono::steady_clock::time_point send_time;  // 发送时间（用于 RTT 统计）
};

// ============================================================================
// MprpcChannel: RPC 客户端核心通道
// ============================================================================
/**
 * @brief RPC 客户端通道，负责请求的发送和响应的接收
 * @details
 * 1. 支持同步和异步 RPC 调用
 * 2. 实现 TCP 拆包/粘包处理
 * 3. 支持超时和重试机制
 * 4. 通过 request_id 匹配异步响应
 */
class MprpcChannel : public google::protobuf::RpcChannel {
 public:
  /**
   * @brief 构造函数
   * @param ip 服务端 IP 地址
   * @param port 服务端端口
   * @param config 客户端配置
   * @param connect_now 是否立即连接 (false 则延迟连接)
   */
  MprpcChannel(const std::string& ip, uint16_t port, 
               const RpcClientConfig& config = RpcClientConfig(),
               bool connect_now = true);
  
  ~MprpcChannel();

  // 禁止拷贝
  MprpcChannel(const MprpcChannel&) = delete;
  MprpcChannel& operator=(const MprpcChannel&) = delete;

  /**
   * @brief 核心 RPC 调用接口 (由 Protobuf Stub 自动调用)
   * @details
   * 流程：
   * 1. 序列化请求 -> 2. 发送请求 -> 3. 接收响应 -> 4. 反序列化响应
   */
  void CallMethod(const google::protobuf::MethodDescriptor* method,
                  google::protobuf::RpcController* controller,
                  const google::protobuf::Message* request,
                  google::protobuf::Message* response,
                  google::protobuf::Closure* done) override;

  /**
   * @brief 手动关闭连接
   */
  void Close();

  /**
   * @brief 检查连接状态
   */
  bool IsConnected() const { return m_clientFd != -1; }

 private:
  // ========== 网络相关 ==========
  
  int m_clientFd;                    // 客户端 socket fd
  const std::string m_ip;            // 服务端 IP
  const uint16_t m_port;             // 服务端端口
  RpcClientConfig m_config;          // 客户端配置

  // ========== 请求管理 ==========
  
  std::atomic<uint64_t> m_next_request_id{1};  // 请求 ID 生成器
  
  mutable std::mutex m_pending_mutex;          // 保护待处理请求表
  std::unordered_map<uint64_t, std::shared_ptr<PendingRpcContext>> m_pending_requests;

  // ========== 接收缓冲区 ==========
  
  mutable std::mutex m_recv_mutex;             // 保护接收缓冲区
  std::string m_recv_buffer;                   // 接收缓冲区 (处理粘包/半包)

  // ========== 连接管理 ==========
  
  /**
   * @brief 建立 TCP 连接
   * @param ip IP 地址
   * @param port 端口
   * @param err_msg [输出] 错误信息
   * @return true 成功, false 失败
   */
  bool Connect(const char* ip, uint16_t port, std::string* err_msg);

  /**
   * @brief 设置 socket 超时选项
   * @param timeout_ms 超时时间 (毫秒)
   */
  bool SetSocketTimeout(int timeout_ms);

  /**
   * @brief 重连机制 (带重试)
   */
  bool Reconnect(std::string* err_msg);

  // ========== 请求发送 ==========
  
  /**
   * @brief 发送 RPC 请求 (带帧头)
   * @details 
   * 协议格式: [Varint32: header_size] + [RpcHeader] + [Args]
   * @param request_id 请求 ID
   * @param service_name 服务名
   * @param method_name 方法名
   * @param request 请求对象
   * @param controller 控制器 (用于错误报告)
   * @return true 发送成功, false 失败
   */
  bool SendRequest(uint64_t request_id,
                   const std::string& service_name,
                   const std::string& method_name,
                   const google::protobuf::Message* request,
                   google::protobuf::RpcController* controller);

  // ========== 响应接收 ==========
  
  /**
   * @brief 接收响应数据 (处理拆包/粘包)
   * @details
   * 1. 循环读取 socket 数据到 m_recv_buffer
   * 2. 尝试从 buffer 解析完整消息
   * 3. 通过 request_id 匹配待处理请求
   * 4. 反序列化响应并触发回调
   */
  bool ReceiveResponse(google::protobuf::RpcController* controller);

  /**
   * @brief 尝试从接收缓冲区解析一个完整的响应包
   * @param request_id [输出] 请求 ID
   * @param error_code [输出] 错误码
   * @param error_msg [输出] 错误信息
   * @param response_data [输出] 响应数据
   * @return true 解析成功, false 数据不完整
   */
  bool TryParseResponse(uint64_t& request_id,
                       int32_t& error_code,
                       std::string& error_msg,
                       std::string& response_data);

  /**
   * @brief 从 socket 读取数据到内部缓冲区
   * @return true 读取成功, false 失败或对端关闭
   */
  bool ReadToBuffer();

  // ========== 辅助函数 ==========
  
  /**
   * @brief 生成唯一的请求 ID
   */
  uint64_t GenerateRequestId() {
    return m_next_request_id.fetch_add(1);
  }

  /**
   * @brief 注册待处理请求
   */
  void RegisterPendingRequest(uint64_t request_id, 
                             std::shared_ptr<PendingRpcContext> ctx);

  /**
   * @brief 完成待处理请求 (通知等待线程或触发回调)
   */
  void CompletePendingRequest(uint64_t request_id,
                             int32_t error_code,
                             const std::string& error_msg,
                             const std::string& response_data);

  /**
   * @brief 清理超时请求
   */
  void CleanupTimeoutRequests();
};

#endif  // RPCCLIENTOLD_H