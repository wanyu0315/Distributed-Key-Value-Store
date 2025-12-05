#include <iostream>
#include <cassert>
#include "rpcprovider.h"

// 辅助宏：简单的断言测试
#define TEST_ASSERT(cond) \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " << #cond << std::endl; \
        exit(1); \
    } else { \
        std::cout << "[PASS] " << #cond << std::endl; \
    }

void test_config_validation() {
    std::cout << "--- Testing Config Validation ---" << std::endl;

    // 1. 测试默认配置是否合法
    RpcProvider::Config config;
    config.port = 8000; // 必须设置端口
    RpcProvider provider(config);
    // 如果构造函数没有抛出 Fatal Log (exit)，说明通过
    std::cout << "[PASS] Valid config construction" << std::endl;
}

// 这是一个 Hack 技巧：通过继承访问 protected/private 方法进行测试
// 在实际工程中通常使用 friend class 或者 gtest
class RpcProviderTester : public RpcProvider {
public:
    RpcProviderTester(const Config& c) : RpcProvider(c) {}
    // 公开 GetLocalIP
    std::string PublicGetLocalIP() const { return GetLocalIP(); }
};

void test_local_ip() {
    std::cout << "--- Testing Local IP Detection ---" << std::endl;
    RpcProvider::Config config;
    config.port = 8000;
    RpcProviderTester tester(config);
    
    std::string ip = tester.PublicGetLocalIP();
    std::cout << "Detected Local IP: " << ip << std::endl;
    
    TEST_ASSERT(!ip.empty());
    TEST_ASSERT(ip != "0.0.0.0");
}

int main() {
    test_config_validation();
    test_local_ip();
    std::cout << "All Config Tests Passed!" << std::endl;
    return 0;
}