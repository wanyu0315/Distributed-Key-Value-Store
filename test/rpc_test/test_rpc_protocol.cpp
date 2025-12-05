#include "rpcprovider.h"
#include "rpcheader.pb.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include <muduo/net/Buffer.h>
#include <iostream>
#include <cassert>

// 辅助类：暴露 protected/private 方法
class RpcProviderTester : public RpcProvider {
public:
    RpcProviderTester(const Config& c) : RpcProvider(c) {}

    // 公开 TryParseMessage 用于测试
    bool PublicTryParseMessage(muduo::net::Buffer* buffer,
                               uint32_t& header_size,
                               std::string& service_name,
                               std::string& method_name,
                               std::string& args_str) {
        return TryParseMessage(buffer, header_size, service_name, method_name, args_str);
    }
};

// 辅助函数：构造一个标准的 RPC 请求二进制流，模拟客户端发送一个 RPC 调用请求。
void AppendRpcRequestToBuffer(muduo::net::Buffer* buffer, 
                              const std::string& service, 
                              const std::string& method, 
                              const std::string& args) {
    // 1. 构造 Header，描述服务和方法，header 将字符串信息序列化为二进制流，模拟网络传输
    RPC::RpcHeader header;
    header.set_service_name(service);
    header.set_method_name(method);
    header.set_args_size(args.size());

    std::string header_str;
    header.SerializeToString(&header_str);

    // 2. 写入长度头 (Varint)
    uint32_t header_len = header_str.size();
    // 手动模拟 WriteVarint32 行为 (简单版，假设长度 < 128，只占1字节)
    // 实际上应该用 CodedOutputStream，但为了测试简单，若长度小可以直接 push_back
    assert(header_len < 128); 
    buffer->appendInt8(static_cast<uint8_t>(header_len)); 

    // 3. 写入 Header 数据
    buffer->append(header_str);

    // 4. 写入 Args 数据
    buffer->append(args);
}

// 正常包：一次收到一个完整请求。
void test_normal_packet() {
    std::cout << "Test 1: Normal Packet... ";
    RpcProvider::Config config; 
    config.port = 1234;
    RpcProviderTester tester(config);
    muduo::net::Buffer buffer;

    // 构造并写入一个完整的 RPC 请求包，将这个请求包序列化为二进制流。
    AppendRpcRequestToBuffer(&buffer, "TestService", "Login", "user:admin");

    uint32_t h_size;
    std::string s_name, m_name, args;
    bool res = tester.PublicTryParseMessage(&buffer, h_size, s_name, m_name, args);

    assert(res == true);
    assert(s_name == "TestService");
    assert(m_name == "Login");
    assert(args == "user:admin");
    assert(buffer.readableBytes() == 0); // 数据应该被全部消费
    std::cout << "PASS" << std::endl;
}

// 半包：先收到头部，再收到 Body。
void test_partial_packet() {
    std::cout << "Test 2: Partial Packet (Split)... ";
    RpcProvider::Config config; config.port = 1234;
    RpcProviderTester tester(config);   // 测试服务类（只有 TryParseMessage 方法）
    muduo::net::Buffer buffer;

    // 构造完整包
    muduo::net::Buffer temp;
    AppendRpcRequestToBuffer(&temp, "TestService", "Login", "user:admin"); //   模拟客户端发送二进制流
    std::string all_data = temp.retrieveAllAsString(); // muduo::net::Buffer的内置函数，用于获取全部数据并转换为字符串

    // 模拟半包：只发送前 5 个字节
    buffer.append(all_data.substr(0, 5));

    uint32_t h_size;
    std::string s_name, m_name, args;
    
    // 第一次尝试：数据不够，应该返回 false，且 buffer 不动
    bool res = tester.PublicTryParseMessage(&buffer, h_size, s_name, m_name, args);
    assert(res == false);
    assert(buffer.readableBytes() == 5); 

    // 补齐剩余数据
    buffer.append(all_data.substr(5));

    // 第二次尝试：数据齐了
    res = tester.PublicTryParseMessage(&buffer, h_size, s_name, m_name, args);
    assert(res == true);
    assert(args == "user:admin");
    assert(buffer.readableBytes() == 0);
    std::cout << "PASS" << std::endl;
}

// 粘包：一次收到两个请求。
void test_sticky_packets() {
    std::cout << "Test 3: Sticky Packets (Two in one)... ";
    RpcProvider::Config config; config.port = 1234;
    RpcProviderTester tester(config);
    muduo::net::Buffer buffer;

    // 粘包：连续写入两个请求（连续生成两个客户端二进制流）
    AppendRpcRequestToBuffer(&buffer, "ServiceA", "MethodA", "ArgsA");
    AppendRpcRequestToBuffer(&buffer, "ServiceB", "MethodB", "ArgsB");

    uint32_t h_size;
    std::string s_name, m_name, args;

    // 解析第一个包（看看TryParseMessage能否正确解析出第一个请求而不粘包）
    bool res1 = tester.PublicTryParseMessage(&buffer, h_size, s_name, m_name, args);
    assert(res1 == true);
    assert(s_name == "ServiceA");
    assert(args == "ArgsA");

    // 解析第二个包
    bool res2 = tester.PublicTryParseMessage(&buffer, h_size, s_name, m_name, args);
    assert(res2 == true);
    assert(s_name == "ServiceB");
    assert(args == "ArgsB");

    assert(buffer.readableBytes() == 0);
    std::cout << "PASS" << std::endl;
}

int main() {
    test_normal_packet();
    test_partial_packet();
    test_sticky_packets();
    return 0;
}