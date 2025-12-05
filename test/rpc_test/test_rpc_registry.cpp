#include "rpcprovider.h"
#include "test_service.pb.h" // protoc 生成的文件
#include <iostream>

// 模拟一个业务服务实现
// test::TestService 是一个 protobuf RPC service 的抽象基类（protoc 生成的）。
class TestServiceImpl : public test::TestService {
public:
    void TestMethod(::google::protobuf::RpcController* controller,
                    const ::test::TestRequest* request,
                    ::test::TestResponse* response,
                    ::google::protobuf::Closure* done) override {
        // Mock implementation
    }
};

int main() {
    std::cout << "--- Testing Service Registry ---" << std::endl;

    RpcProvider::Config config;
    config.port = 8000;
    RpcProvider provider(config);   // 创建 RpcProvider 实例，用于测试服务注册功能。

    TestServiceImpl service_impl;   // 创建一个我们刚刚实现的业务类对象，这是后面要注册的服务实体。
    
    // 测试点 1: 正常注册
    bool ret = provider.NotifyService(&service_impl);
    if (ret) {
        std::cout << "[PASS] NotifyService returned true" << std::endl;
    } else {
        std::cerr << "[FAIL] NotifyService returned false" << std::endl;
        return 1;
    }

    // 测试点 2: 重复注册应失败
    bool ret_duplicate = provider.NotifyService(&service_impl);
    if (!ret_duplicate) {
        std::cout << "[PASS] Duplicate registration rejected" << std::endl;
    } else {
        std::cerr << "[FAIL] Duplicate registration accepted" << std::endl;
        return 1;
    }

    // 测试点 3: 空指针注册
    bool ret_null = provider.NotifyService(nullptr);
    if (!ret_null) {
        std::cout << "[PASS] Nullptr registration rejected" << std::endl;
    } else {
        std::cerr << "[FAIL] Nullptr registration accepted" << std::endl;
        return 1;
    }

    return 0;
}