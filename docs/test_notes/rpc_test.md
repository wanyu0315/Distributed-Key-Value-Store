# CMake编译文件的注意事项
CMake是从根目录的CMakeListes.txt文件一层一层链接下去的，因此要保证父目录的CMakeListes.txt文件中一定要add_subdirectory其所有子目录，否则无法链接到子目录下的源文件。

# rpc服务器的测试
为了确保测试的合理性和全面性，我们将测试分为三个部分：
单元测试 (Unit Test)：测试 RpcProvider 的内部辅助逻辑（如配置校验、本地 IP 获取）。
集成测试 - 服务注册 (Integration Test - Registry)：测试 NotifyService 能否正确解析 Protobuf Service 并构建路由表。
系统测试 - 模拟通信 (System Test - Mock Network)：这是最核心的测试，模拟 TCP 客户端发送粘包、半包数据，验证 OnMessage 的协议解析和业务分发逻辑。

## 第一部分：基础功能与配置测试
### 测试目的：
    确保 RpcProvider 在启动前的参数校验逻辑正确，对于合法的配置能够予以通过；确保服务器初始化时能正确获取本机环境信息。
### 测试文件：
    test/test_rpc_config.cpp
### 调试方法：
     cmake --build . --target rpc_config_test
     /home/developer/MyDistributedStore/build/test/rpc/rpc_config_test

### 期望输出：
    --- Testing Config Validation ---
    20251204 12:01:48.414966Z 732463 INFO  RpcProvider initialized: ip=, port=8000, threads=4, max_msg_size=10485760 - rpcprovider.cpp:66
    [PASS] Valid config construction
    20251204 12:01:48.415001Z 732463 INFO  RpcProvider shutting down... - rpcprovider.cpp:106
    20251204 12:01:48.415004Z 732463 INFO  RpcProvider shutdown complete. Stats - Total: 0, Failed: 0, Partial: 0 - rpcprovider.cpp:125
    --- Testing Local IP Detection ---
    20251204 12:01:48.415045Z 732463 INFO  RpcProvider initialized: ip=, port=8000, threads=4, max_msg_size=10485760 - rpcprovider.cpp:66
    Detected Local IP: 127.0.1.1
    [PASS] !ip.empty()
    [PASS] ip != "0.0.0.0"
    20251204 12:01:48.415961Z 732463 INFO  RpcProvider shutting down... - rpcprovider.cpp:106
    20251204 12:01:48.415968Z 732463 INFO  RpcProvider shutdown complete. Stats - Total: 0, Failed: 0, Partial: 0 - rpcprovider.cpp:125
    All Config Tests Passed!

输出说明：
    --- Testing Config Validation ---
    ip= 空字符串 ""：表示你还没有调用 GetLocalIP() 或者 provider 没有在构造阶段自动设置 IP。
    port=8000：测试里设置的端口。
    threads=4：RpcProvider 默认的线程数。
    max_msg_size=10485760：默认最大消息 10MB。
    日志文件位置表明代码在：rpcprovider.cpp 第 66 行

    --- Testing Local IP Detection ---
    ip 仍然为空，因为构造函数没有自动计算本地 IP。
    GetLocalIP() 返回了： 127.0.1.1
    断言通过：IP 非空，IP 不是无效默认地址 0.0.0.0，符合预期。

## 第二部分：服务注册测试 (NotifyService)
### 测试目的
    验证 Protobuf 的 ServiceDescriptor 和 MethodDescriptor 是否能被 NotifyService 正确解析并存入 service_map_。
### 前置依赖
    需要定义一个简单的 Protobuf 业务协议文件。

### 测试信息
    Proto文件：src/proto/test_service.proto
    源码文件：test/rpc_test/test_rpc_protocol.cpp
    调试方法
        cmake --build . --target rpc_registry_test
        /home/developer/MyDistributedStore/build/test/rpc/rpc_registry_test

### 期望输出
    --- Testing Service Registry ---
    20251205 07:01:56.584557Z 1005666 INFO  RpcProvider initialized: ip=, port=8000, threads=4, max_msg_size=10485760 - rpcprovider.cpp:66  // rpc服务器初始化的打印信息
    20251205 07:01:56.584701Z 1005666 INFO  Registered method: TestService.TestMethod - rpcprovider.cpp:162 // 业务服务对象初始化的打印信息
    20251205 07:01:56.584716Z 1005666 INFO  Service registered: TestService with 1 methods - rpcprovider.cpp:166  // 在方法注册完成后，NotifyService 报告已注册的服务名及其方法数
    [PASS] NotifyService returned true
    20251205 07:01:56.584751Z 1005666 WARN  Service already registered: TestService - rpcprovider.cpp:149
    [PASS] Duplicate registration rejected
    20251205 07:01:56.584760Z 1005666 ERROR Null service pointer - rpcprovider.cpp:136
    [PASS] Nullptr registration rejected
    20251205 07:01:56.584770Z 1005666 INFO  RpcProvider shutting down... - rpcprovider.cpp:106
    20251205 07:01:56.584777Z 1005666 INFO  RpcProvider shutdown complete. Stats - Total: 0, Failed: 0, Partial: 0 - rpcprovider.cpp:125
输出说明:
    第一项测试通过：首次合法注册返回 true。
    第二次调用 NotifyService(&service_impl) 时，重复注册被检测到，内部选择不覆盖/不重复插入，记录一个警告。
    第三次调用 NotifyService(nullptr) 时，函数检测到传入空指针并记录为错误（rpcprovider.cpp:136）。传 nullptr 是非法用法，Provider 直接拒绝并把这类情况视为错误（而非仅警告）。


## 第三部分：系统测试 - 模拟 TCP 粘包与协议解析 (Core)
### 测试目的
    这是最关键的测试。验证 OnMessage 中的 Peek 逻辑能否正确处理：
    1. 正常包：一次收到一个完整请求。
    2. 半包：先收到头部，再收到 Body。
    3. 粘包：一次收到两个请求。

### 测试逻辑
    由于直接启动 Muduo Server 需要真实的 Socket 交互，测试代码中编写一个 Mock Buffer 的测试。在测试代码中手动构造 muduo::net::Buffer，然后手动调用 TryParseMessage（这里需要一点 Hack 技巧将 TryParseMessage 暴露出来，或者在测试中重写类似的逻辑进行验证）。
    为了方便测试，我们通过继承 RpcProvider 并公开其 private 方法来进行白盒测试。

### 测试信息
    测试源文件：test/test_rpc_protocol.cpp
    调试方法：
        cmake ..
        cmake --build . --target rpc_protocol_test
        /home/developer/MyDistributedStore/build/test/rpc/rpc_protocol_test

### 期望输出
    Test 1: Normal Packet... 20251205 12:07:23.983904Z 1078807 INFO  RpcProvider initialized: ip=, port=1234, threads=4, max_msg_size=10485760 - rpcprovider.cpp:66
    PASS
    20251205 12:07:23.983972Z 1078807 INFO  RpcProvider shutting down... - rpcprovider.cpp:106
    20251205 12:07:23.983978Z 1078807 INFO  RpcProvider shutdown complete. Stats - Total: 0, Failed: 0, Partial: 0 - rpcprovider.cpp:125
    Test 2: Partial Packet (Split)... 20251205 12:07:23.984008Z 1078807 INFO  RpcProvider initialized: ip=, port=1234, threads=4, max_msg_size=10485760 - rpcprovider.cpp:66
    PASS
    20251205 12:07:23.984029Z 1078807 INFO  RpcProvider shutting down... - rpcprovider.cpp:106
    20251205 12:07:23.984033Z 1078807 INFO  RpcProvider shutdown complete. Stats - Total: 0, Failed: 0, Partial: 0 - rpcprovider.cpp:125
    Test 3: Sticky Packets (Two in one)... 20251205 12:07:23.984057Z 1078807 INFO  RpcProvider initialized: ip=, port=1234, threads=4, max_msg_size=10485760 - rpcprovider.cpp:66
    PASS
    20251205 12:07:23.984079Z 1078807 INFO  RpcProvider shutting down... - rpcprovider.cpp:106
    20251205 12:07:23.984082Z 1078807 INFO  RpcProvider shutdown complete. Stats - Total: 0, Failed: 0, Partial: 0 - rpcprovider.cpp:125
输出说明：
    服务器构造和析构的打印不再赘述
    三个测试都完美，说明正常包、半包和粘包问题都能够正确处理。


# 总结与分析
1. Config 测试：
    通过继承 RpcProvider 访问保护成员，验证了 GetLocalIP 的逻辑和配置校验逻辑。这是白盒测试的典型手段。
2. Registry 测试：
    引入了一个真实的（虽然是空的）test_service.proto，验证了 NotifyService 中反射逻辑的正确性。如果 NotifyService 写错了（比如方法名获取错误），这个测试会失败。
3. Protocol 测试：
    这是含金量最高的。它没有启动真实的网络，而是直接操作内存中的 Buffer。
    通过 AppendInt8 和 AppendString 手动构造符合协议的二进制流。
    验证了 Peek 策略：在 test_partial_packet 中，测试故意只给 5 个字节，验证了代码不会错误地消费数据，而是等待数据补齐。这是对之前改进的 TryParseMessage 最有力的验证。