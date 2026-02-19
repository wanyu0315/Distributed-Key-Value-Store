#include "raft_rpc.h"
#include <thread>
#include <mutex>
#include <iostream>
#include <future>
#include <memory>

// 引入协程库头文件 (根据你的项目结构调整路径)
#include "scheduler.h" 
#include "fiber.h"

namespace raft {

// =========================================================
//  PART 1: 异步 RPC 核心引擎 (Poller & Type Erasure)
//  
//  这里的核心难点在于：gRPC 的 CompletionQueue::Next 返回的是 void* tag。
//  我们需要一种机制，把这个 void* 还原成具体的 RequestVote 或 AppendEntries 上下文，
//  并执行对应的回调和协程唤醒逻辑。
// =========================================================

/**
 * @brief 异步调用基类接口 (Type Erasure / 类型擦除)
 * * 为什么需要这个类？
 * AsyncClientCall<T> 是模板类，RequestVote 和 AppendEntries 的类型不同。
 * CompletionQueue 获取到的 tag 是 void*。
 * 我们需要一个非模板的基类指针，通过虚函数 Proceed() 来多态地处理不同类型的 RPC 完成事件。
 */
struct AsyncCallBase {
    virtual ~AsyncCallBase() = default;
    
    /**
     * @brief 处理 RPC 完成事件
     * @param ok gRPC 系统通知的成功/失败标志
     */
    virtual void Proceed(bool ok) = 0;
};

/**
 * @brief 具体的异步调用包装器
 * * 这是一个“胶水”类，连接了：
 * 1. gRPC 的 void* tag
 * 2. 具体的业务数据 AsyncClientCall<Reply>
 * 3. 协程调度器
 */
template<typename Reply>
struct AsyncCallWrapper : public AsyncCallBase {
    // 持有实际的调用上下文数据
    AsyncClientCall<Reply>* call_data;
    
    explicit AsyncCallWrapper(AsyncClientCall<Reply>* ptr) : call_data(ptr) {}

    // 当 gRPC 后台线程收到回包时，会调用此函数
    void Proceed(bool ok) override {
        // [Thread: gRPC Poller Thread] 
        // 注意：这里运行在后台轮询线程中，绝对不能做耗时操作，也不能直接跑 Raft 业务逻辑

        // 1. 检查 gRPC 状态
        if (!ok) {
            call_data->status = grpc::Status(grpc::StatusCode::CANCELLED, "RPC failed or cancelled (gRPC level)");
        }

        // 2. 执行用户注册的回调 (通常用于简单的状态更新)
        if (call_data->callback) {
            call_data->callback(call_data->status.ok(), call_data->reply);
        }

        // 3. 核心步骤：唤醒挂起的协程
        // 如果调用方在发起 RPC 时传入了协程句柄 (fiber_tag)，我们需要把它重新加入调度器
        if (call_data->fiber_tag) {
             // 获取全局调度器，将挂起的协程指针强转回来，并 Schedule
             monsoon::Scheduler::GetThis()->scheduler((monsoon::Fiber*)call_data->fiber_tag);
        }

        // 4. 资源清理 (自杀式生命周期管理)
        // 这一步非常重要！因为 call_data 和 wrapper 都是 new 出来的
        // 既然 RPC 已经结束，回调也已执行，必须在这里释放内存，否则内存泄漏
        delete call_data; // 释放 Protocol Buffer 消息和 Context
        delete this;      // 释放当前的包装器
    }
};

/**
 * @brief 全局 RPC 系统单例
 * * 负责管理唯一的 CompletionQueue 和后台轮询线程 (Poller Thread)。
 * 所有的 gRPC 客户端请求都共享这个队列和线程。
 */
class RpcSystem {
public:
    // 获取单例的 CompletionQueue 指针
    static grpc::CompletionQueue* GetCQ() {
        static RpcSystem instance;
        return &instance.cq_;
    }

private:
    RpcSystem() {
        // 启动后台线程，不断从队列中取出完成的事件
        poller_ = std::thread([this](){
            void* tag;  // 这是我们在 CallMethod 时传进去的 AsyncCallWrapper 指针
            bool ok;
            
            // 阻塞等待，直到有 RPC 完成
            while (cq_.Next(&tag, &ok)) {
                // 1. 将 void* 还原为基类指针
                auto* call = static_cast<AsyncCallBase*>(tag);
                // 2. 多态调用 Proceed，处理具体类型的 RPC
                call->Proceed(ok);
            }
        });
        poller_.detach(); // 简单起见，随进程退出而退出。生产环境建议妥善管理 join。
    }

    grpc::CompletionQueue cq_;
    std::thread poller_;
};

// =========================================================
//  PART 2: RaftRpcClient 实现 (发送端)
// =========================================================

RaftRpcClient::RaftRpcClient(const std::string& target) {
    // 配置 gRPC Channel 参数
    grpc::ChannelArguments args;
    
    // [关键参数配置] 针对分布式系统的高可用优化
    // 1. 10秒发一次心跳包检测连接
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
    // 2. 心跳发送后 5秒没回应则认为连接断开
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
    // 3. 即使没有 RPC 调用，也允许发送心跳 (这对 Raft 很重要，因为可能长期空闲)
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    
    channel_ = grpc::CreateCustomChannel(
        target,
        grpc::InsecureChannelCredentials(),
        args
    );
    stub_ = raftRpcProctoc::RaftRpcService::NewStub(channel_);
}

RaftRpcClient::~RaftRpcClient() = default;

// 设置 RPC 的超时时间，防止网络分区时协程永久挂起
void RaftRpcClient::SetDeadline(grpc::ClientContext* context, int timeout_ms) {
    auto deadline = std::chrono::system_clock::now() + 
                    std::chrono::milliseconds(timeout_ms);
    context->set_deadline(deadline);
}

// --- 1. 异步 RequestVote ---
void RaftRpcClient::AsyncRequestVote(
    const raftRpcProctoc::RequestVoteArgs& args,
    RpcCallback<raftRpcProctoc::RequestVoteReply> callback,
    void* fiber_tag,
    int timeout_ms
) {
    // 1. 创建调用上下文 (Call Data)
    auto* call = new AsyncClientCall<raftRpcProctoc::RequestVoteReply>();
    call->callback = callback;
    call->fiber_tag = fiber_tag;
    SetDeadline(&call->context, timeout_ms);

    // 2. 创建类型擦除包装器 (Wrapper)
    // 这个 wrapper 会被作为 tag 传给 gRPC，并在 Poller 线程中被恢复
    auto* wrapper = new AsyncCallWrapper<raftRpcProctoc::RequestVoteReply>(call);

    // 3. 发起异步调用 (Prepare -> Start -> Finish)
    // 注意：我们将全局单例的 CQ 传入
    call->response_reader = stub_->PrepareAsyncRequestVote(&call->context, args, RpcSystem::GetCQ());
    call->response_reader->StartCall();
    
    // 4. 绑定 tag
    // 当 RPC 完成时，gRPC 会把 wrapper 指针放入 CQ 返回给我们
    call->response_reader->Finish(&call->reply, &call->status, (void*)wrapper);
}

// --- 2. 异步 AppendEntries ---
void RaftRpcClient::AsyncAppendEntries(
    const raftRpcProctoc::AppendEntriesArgs& args,
    RpcCallback<raftRpcProctoc::AppendEntriesReply> callback,
    void* fiber_tag,
    int timeout_ms
) {
    // 逻辑同上
    auto* call = new AsyncClientCall<raftRpcProctoc::AppendEntriesReply>();
    call->callback = callback;
    call->fiber_tag = fiber_tag;
    SetDeadline(&call->context, timeout_ms);

    auto* wrapper = new AsyncCallWrapper<raftRpcProctoc::AppendEntriesReply>(call);

    call->response_reader = stub_->PrepareAsyncAppendEntries(&call->context, args, RpcSystem::GetCQ());
    call->response_reader->StartCall();
    call->response_reader->Finish(&call->reply, &call->status, (void*)wrapper);
}

// --- 3. 异步 InstallSnapshot ---
void RaftRpcClient::AsyncInstallSnapshot(
    const raftRpcProctoc::InstallSnapshotArgs& args,
    RpcCallback<raftRpcProctoc::InstallSnapshotReply> callback,
    void* fiber_tag,
    int timeout_ms
) {
    // 逻辑同上
    auto* call = new AsyncClientCall<raftRpcProctoc::InstallSnapshotReply>();
    call->callback = callback;
    call->fiber_tag = fiber_tag;
    SetDeadline(&call->context, timeout_ms);

    auto* wrapper = new AsyncCallWrapper<raftRpcProctoc::InstallSnapshotReply>(call);

    call->response_reader = stub_->PrepareAsyncInstallSnapshot(&call->context, args, RpcSystem::GetCQ());
    call->response_reader->StartCall();
    call->response_reader->Finish(&call->reply, &call->status, (void*)wrapper);
}

// --- 同步辅助方法 (仅用于单元测试或简单调试) ---
bool RaftRpcClient::RequestVote(
    const raftRpcProctoc::RequestVoteArgs& args,
    raftRpcProctoc::RequestVoteReply* reply,
    int timeout_ms
) {
    grpc::ClientContext context;
    SetDeadline(&context, timeout_ms);
    // 这里会阻塞物理线程，生产环境协程中慎用！
    grpc::Status status = stub_->RequestVote(&context, args, reply);
    return status.ok();
}

bool RaftRpcClient::AppendEntries(
    const raftRpcProctoc::AppendEntriesArgs& args,
    raftRpcProctoc::AppendEntriesReply* reply,
    int timeout_ms
) {
    grpc::ClientContext context;
    SetDeadline(&context, timeout_ms);
    grpc::Status status = stub_->AppendEntries(&context, args, reply);
    return status.ok();
}

bool RaftRpcClient::IsAvailable() const {
    auto state = channel_->GetState(false);
    return state != GRPC_CHANNEL_SHUTDOWN;
}

// =========================================================
//  PART 3: RaftRpcServiceImpl 实现 (服务端接收)
//  
//  架构模式：Sync Server + Async Logic
//  1. gRPC 线程池收到请求 (阻塞)
//  2. 将任务扔给协程调度器 (异步)
//  3. 等待协程处理完成信号 (Future/Promise)
//  4. gRPC 线程返回结果
// =========================================================

RaftRpcServiceImpl::RaftRpcServiceImpl(void* raft_node)
    : raft_node_(raft_node) {}

// [占位符] 假设 Raft 核心类的接口定义。
// 在实际编译时，你需要包含 "raft.h"，并确保 Raft 类有这些 ProcessXXX 方法。
/*
class Raft {
public:
    void ProcessRequestVote(const RequestVoteArgs* args, RequestVoteReply* reply);
    void ProcessAppendEntries(const AppendEntriesArgs* args, AppendEntriesReply* reply);
    void ProcessInstallSnapshot(const InstallSnapshotArgs* args, InstallSnapshotReply* reply);
};
*/

grpc::Status RaftRpcServiceImpl::RequestVote(
    grpc::ServerContext* context,
    const raftRpcProctoc::RequestVoteArgs* request,
    raftRpcProctoc::RequestVoteReply* reply
) {
    // 1. 获取全局单例协程调度器
    auto scheduler = monsoon::Scheduler::GetThis();
    if (!scheduler) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "No Scheduler available");
    }

    // 2. 创建同步原语 (Promise/Future)
    // 作用：让 gRPC 的处理线程在这里“停车”，等待协程在另一个线程“把活干完”
    std::promise<void> prom;
    auto fut = prom.get_future();

    // 3. 将任务打包提交给协程调度器
    // Lambda 捕获 this, request, reply 指针是安全的，因为 gRPC 在此函数返回前不会销毁它们
    scheduler->scheduler([this, request, reply, &prom]() {
        // ============= 这里是协程上下文 (Fiber Context) =============
        
        // 1. 将 void* 转回 Raft* (实际项目中建议使用接口类 IRaftNode* 增加安全性)
        // auto raft = static_cast<Raft*>(raft_node_);
        
        // 2. 调用核心逻辑 (这是无锁或协程锁环境，安全！)
        // raft->ProcessRequestVote(request, reply);
        
        // 3. 模拟打印 (调试用)
        // std::cout << "[RaftRpc] Handled RequestVote in Fiber" << std::endl;
        
        // 4. 通知 gRPC 线程：活干完了，可以返回了
        prom.set_value();
        
        // ==========================================================
    });

    // 4. gRPC 线程在此阻塞等待
    // 设置 2 秒超时保护，防止协程死锁导致 gRPC 线程无限挂起
    if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
        return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Raft Core Timeout / Deadlock");
    }

    return grpc::Status::OK;
}

grpc::Status RaftRpcServiceImpl::AppendEntries(
    grpc::ServerContext* context,
    const raftRpcProctoc::AppendEntriesArgs* request,
    raftRpcProctoc::AppendEntriesReply* reply
) {
    auto scheduler = monsoon::Scheduler::GetThis();
    std::promise<void> prom;
    auto fut = prom.get_future();

    scheduler->scheduler([this, request, reply, &prom]() {
        // auto raft = static_cast<Raft*>(raft_node_);
        // raft->ProcessAppendEntries(request, reply);
        prom.set_value();
    });

    // 等待结果
    fut.wait();
    return grpc::Status::OK;
}

grpc::Status RaftRpcServiceImpl::InstallSnapshot(
    grpc::ServerContext* context,
    const raftRpcProctoc::InstallSnapshotArgs* request,
    raftRpcProctoc::InstallSnapshotReply* reply
) {
    auto scheduler = monsoon::Scheduler::GetThis();
    std::promise<void> prom;
    auto fut = prom.get_future();

    scheduler->scheduler([this, request, reply, &prom]() {
        // auto raft = static_cast<Raft*>(raft_node_);
        // raft->ProcessInstallSnapshot(request, reply);
        prom.set_value();
    });

    fut.wait();
    return grpc::Status::OK;
}

// =========================================================
//  PART 4: RaftRpcServer 实现 (服务端启动管理)
// =========================================================

RaftRpcServer::RaftRpcServer(const std::string& listen_addr, void* raft_node)
    : listen_addr_(listen_addr) {
    service_ = std::make_unique<RaftRpcServiceImpl>(raft_node);
}

RaftRpcServer::~RaftRpcServer() {
    Stop();
}

void RaftRpcServer::Start() {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_addr_, grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());
    
    // [优化] gRPC 线程池配置
    // 因为我们采用了 "gRPC等待协程" 的模式，gRPC 线程会被阻塞住等待协程结果。
    // 所以需要更多的 gRPC 线程来应对并发，防止线程池耗尽。
    // 这里没有硬编码，但在高并发场景建议通过 SetSyncServerOption 调大线程数。

    // [优化] 消息大小限制 (针对 Snapshot 大包)
    builder.SetMaxReceiveMessageSize(INT_MAX); // 允许接收大快照
    builder.SetMaxSendMessageSize(INT_MAX);

    server_ = builder.BuildAndStart();
    if (server_) {
        std::cout << "[RaftRpcServer] Started listening on " << listen_addr_ << std::endl;
        running_ = true;
        server_->Wait(); // 阻塞当前线程，直到 Shutdown
    } else {
        std::cerr << "[RaftRpcServer] Failed to bind " << listen_addr_ << std::endl;
    }
}

void RaftRpcServer::Stop() {
    if (running_ && server_) {
        std::cout << "[RaftRpcServer] Stopping..." << std::endl;
        // 设置 1秒 Deadline，强制中断现有连接，防止卡死
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(1);
        server_->Shutdown(deadline);
        running_ = false;
        
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        std::cout << "[RaftRpcServer] Stopped." << std::endl;
    }
}

void RaftRpcServer::StartAsync() {
    // 启动一个独立的 std::thread 来运行 gRPC 的阻塞循环
    server_thread_ = std::thread([this]() {
        this->Start();
    });
}

// =========================================================
//  PART 5: 工具函数
// =========================================================

std::string SerializeOp(const raftKVRpcProctoc::Op& op) {
    std::string serialized;
    // Protobuf 提供的序列化方法
    op.SerializeToString(&serialized);
    return serialized;
}

bool DeserializeOp(const std::string& data, raftKVRpcProctoc::Op* op) {
    // Protobuf 提供的反序列化方法
    return op->ParseFromString(data);
}

raftRpcProctoc::LogEntry MakeLogEntry(int term, int index, const std::string& command) {
    raftRpcProctoc::LogEntry entry;
    entry.set_term(term);
    entry.set_index(index);
    entry.set_command(command);
    return entry;
}

} // namespace raft