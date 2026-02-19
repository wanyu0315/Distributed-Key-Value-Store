#pragma once

#include <grpcpp/grpcpp.h>
#include <memory>
#include <functional>
#include <atomic>
#include "raft.grpc.pb.h"

namespace raft {

// ========== RPC 调用结果回调 ==========
template<typename Reply>
using RpcCallback = std::function<void(bool success, const Reply& reply)>;

// ========== 异步 RPC 调用上下文 ==========
template<typename Reply>
struct AsyncClientCall {
    grpc::ClientContext context;
    Reply reply;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncResponseReader<Reply>> response_reader;
    
    // 用于协程唤醒
    void* fiber_tag = nullptr;  // 指向 monsoon::Fiber::ptr
    RpcCallback<Reply> callback;
    
    AsyncClientCall() = default;
    ~AsyncClientCall() = default;
};

// ========== Raft RPC 客户端（用于发送RPC到其他节点）==========
class RaftRpcClient {
public:
    /**
     * @brief 构造函数
     * @param target 目标节点地址，格式: "ip:port"
     */
    explicit RaftRpcClient(const std::string& target);
    
    ~RaftRpcClient();
    
    /**
     * @brief 异步发送投票请求
     * @param args 请求参数
     * @param callback 完成回调
     * @param fiber_tag 协程标记（用于协程唤醒）
     * @param timeout_ms 超时时间（毫秒）
     */
    void AsyncRequestVote(
        const raftRpcProctoc::RequestVoteArgs& args,
        RpcCallback<raftRpcProctoc::RequestVoteReply> callback,
        void* fiber_tag = nullptr,
        int timeout_ms = 100
    );
    
    /**
     * @brief 异步发送日志追加请求
     */
    void AsyncAppendEntries(
        const raftRpcProctoc::AppendEntriesArgs& args,
        RpcCallback<raftRpcProctoc::AppendEntriesReply> callback,
        void* fiber_tag = nullptr,
        int timeout_ms = 100
    );
    
    /**
     * @brief 异步发送快照安装请求
     */
    void AsyncInstallSnapshot(
        const raftRpcProctoc::InstallSnapshotArgs& args,
        RpcCallback<raftRpcProctoc::InstallSnapshotReply> callback,
        void* fiber_tag = nullptr,
        int timeout_ms = 1000  // 快照传输超时时间长一些
    );
    
    /**
     * @brief 同步发送投票请求（用于测试或简单场景）
     */
    bool RequestVote(
        const raftRpcProctoc::RequestVoteArgs& args,
        raftRpcProctoc::RequestVoteReply* reply,
        int timeout_ms = 100
    );
    
    /**
     * @brief 同步发送日志追加请求
     */
    bool AppendEntries(
        const raftRpcProctoc::AppendEntriesArgs& args,
        raftRpcProctoc::AppendEntriesReply* reply,
        int timeout_ms = 100
    );
    
    /**
     * @brief 检查连接是否可用
     */
    bool IsAvailable() const;
    
private:
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<raftRpcProctoc::RaftRpcService::Stub> stub_;
    
    // 设置超时
    void SetDeadline(grpc::ClientContext* context, int timeout_ms);
};

// ========== Raft RPC 服务端（处理其他节点发来的RPC）==========
class RaftRpcServiceImpl final : public raftRpcProctoc::RaftRpcService::Service {
public:
    /**
     * @brief 构造函数
     * @param raft_node Raft节点指针（实际处理逻辑在Raft类中）
     */
    explicit RaftRpcServiceImpl(void* raft_node);
    
    // gRPC 服务接口实现
    grpc::Status RequestVote(
        grpc::ServerContext* context,
        const raftRpcProctoc::RequestVoteArgs* request,
        raftRpcProctoc::RequestVoteReply* reply
    ) override;
    
    grpc::Status AppendEntries(
        grpc::ServerContext* context,
        const raftRpcProctoc::AppendEntriesArgs* request,
        raftRpcProctoc::AppendEntriesReply* reply
    ) override;
    
    grpc::Status InstallSnapshot(
        grpc::ServerContext* context,
        const raftRpcProctoc::InstallSnapshotArgs* request,
        raftRpcProctoc::InstallSnapshotReply* reply
    ) override;
    
private:
    void* raft_node_;  // 指向 Raft* （避免头文件循环依赖）
};

// ========== Raft RPC 服务端管理器 ==========
class RaftRpcServer {
public:
    /**
     * @brief 构造函数
     * @param listen_addr 监听地址，格式: "0.0.0.0:port"
     * @param raft_node Raft节点指针
     */
    RaftRpcServer(const std::string& listen_addr, void* raft_node);
    
    ~RaftRpcServer();
    
    /**
     * @brief 启动 RPC 服务器（阻塞）
     */
    void Start();
    
    /**
     * @brief 停止 RPC 服务器
     */
    void Stop();
    
    /**
     * @brief 异步启动（在独立线程中运行）
     */
    void StartAsync();
    
private:
    std::string listen_addr_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<RaftRpcServiceImpl> service_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
};

// ========== RPC 工具函数 ==========

/**
 * @brief 序列化 Op 到 bytes
 */
std::string SerializeOp(const raftKVRpcProctoc::Op& op);

/**
 * @brief 反序列化 bytes 到 Op
 */
bool DeserializeOp(const std::string& data, raftKVRpcProctoc::Op* op);

/**
 * @brief 创建日志条目
 */
raftRpcProctoc::LogEntry MakeLogEntry(int term, int index, const std::string& command);

} // namespace raft