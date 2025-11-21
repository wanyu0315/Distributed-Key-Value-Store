// test_op_serialization.cpp
#include "util.h" // 包含 Op 类定义和序列化方法
#include <iostream>
#include <string>

// 一个简单的测试报告辅助函数
// 返回 true 表示通过，false 表示失败
bool check(bool condition, const std::string& test_name, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAILED] " << test_name << ": " << message << std::endl;
        return false;
    }
    return true;
}

/**
 * @brief 测试用例 1: 完整数据的往返测试
 * @details 填充所有字段，序列化后再反序列化，检查所有字段是否一致。
 */
bool test_op_roundtrip_full() {
    const std::string test_name = "test_op_roundtrip_full";
    std::cout << "Running: " << test_name << "..." << std::endl;

    Op op_in;
    op_in.Operation = "Put";
    op_in.Key = "myKey";
    op_in.Value = "myValue_!@#$_123";
    op_in.ClientId = "client-uuid-abc-123";
    op_in.RequestId = 999;

    std::cout << "\t序列化前的 Op 对象键值对: " << "{ Key：" << op_in.Key << ", Value：" << op_in.Value << " }" << std::endl;

    // 1. 序列化
    std::string payload;
    try {
        payload = op_in.asString();
    } catch (const std::exception& e) {
        return !check(true, test_name, "asString() 抛出异常: " + std::string(e.what()));
    }

    if (!check(!payload.empty(), test_name, "序列化后的 payload 不应为空")) return false;

    // 2. 反序列化
    Op op_out;
    // !! 注意：这里我们假设 parseFromString 已经被修复了 !!
    // !! 如果使用你原来的代码，这里在解析失败时会崩溃 !!
    if (!check(op_out.parseFromString(payload), test_name, "parseFromString() 应该返回 true")) {
        return false;
    }

    std::cout << "\t反序列化后的 Op 对象键值对: " << "{ key：" << op_out.Key << ", value：" << op_out.Value << " }" << std::endl;

    // 3. 断言（ &= 运算符会在当前的 passed 基础上与（AND） check() 的结果）
    bool passed = true;
    passed &= check(op_in.Operation == op_out.Operation, test_name, "Operation 字段不匹配");
    passed &= check(op_in.Key == op_out.Key, test_name, "Key 字段不匹配");
    passed &= check(op_in.Value == op_out.Value, test_name, "Value 字段不匹配");
    passed &= check(op_in.ClientId == op_out.ClientId, test_name, "ClientId 字段不匹配");
    passed &= check(op_in.RequestId == op_out.RequestId, test_name, "RequestId 字段不匹配");

    return passed;
}

/**
 * @brief 测试用例 2: 边界情况（空字符串）
 * @details 测试 Value 为空字符串时是否能正确往返。
 */
bool test_op_roundtrip_empty_value() {
    const std::string test_name = "test_op_roundtrip_empty_value";
    std::cout << "Running: " << test_name << "..." << std::endl;

    Op op_in;
    op_in.Operation = "Get";
    op_in.Key = "key_for_empty_value";
    op_in.Value = ""; // 边界情况
    op_in.ClientId = "client-789";
    op_in.RequestId = 101;

    std::string payload = op_in.asString();
    Op op_out;
    if (!check(op_out.parseFromString(payload), test_name, "parseFromString() 应该返回 true")) {
        return false;
    }

    // 关键断言
    return check(op_in.Value == op_out.Value, test_name, "Value (空字符串) 字段不匹配");
}

/**
 * @brief 测试用例 3: 失败情况（解析损坏的数据）
 * @details 测试 parseFromString 是否能安全地处理无效输入，而不崩溃。
 */
bool test_parse_failure() {
    const std::string test_name = "test_parse_failure";
    std::cout << "Running: " << test_name << "..." << std::endl;

    Op op_out;
    std::string corrupted_payload = "this is definitely not boost archive data";  // 不是合法的序列化内容

    // !! 关键：如果你没有修复 parseFromString，下面这行会使程序崩溃 !!
    // !! 修复后，它应该安全地返回 false !!
    bool success = op_out.parseFromString(corrupted_payload);

    return check(success == false, test_name, "parseFromString() 应该在解析损坏数据时返回 false");
}


int main() {
    int passed = 0;
    const int total = 3;


    if (test_op_roundtrip_full()) passed++;
    if (test_op_roundtrip_empty_value()) passed++;
    if (test_parse_failure()) passed++;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "Test Summary: " << passed << " / " << total << " tests passed." << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // Google Test 和其他框架会返回 0 表示成功
    return (passed == total) ? 0 : 1; 
}