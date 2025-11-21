#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include "skipList.h"

// 辅助宏：用于断言测试结果
#define ASSERT_TRUE(condition, msg) \
    if (!(condition)) { \
        std::cerr << "[FAILED] " << msg << " at line " << __LINE__ << std::endl; \
        std::exit(1); \
    }

#define ASSERT_EQ(val1, val2, msg) \
    if ((val1) != (val2)) { \
        std::cerr << "[FAILED] " << msg << ": " << (val1) << " != " << (val2) << " at line " << __LINE__ << std::endl; \
        std::exit(1); \
    }

// ----------------------------------------------------------------
// 1. 基础增删查测试
// ----------------------------------------------------------------
void TestBasicOperations() {
    std::cout << "[Test 1] Basic Operations (Insert/Search/Delete)... ";
    
    SkipList<int, std::string> list(6);
    std::string val;

    // 测试插入
    ASSERT_EQ(list.insert_element(1, "one"), 0, "Insert key 1 should succeed");
    ASSERT_EQ(list.insert_element(2, "two"), 0, "Insert key 2 should succeed");
    ASSERT_EQ(list.insert_element(1, "one_again"), 1, "Insert duplicate key 1 should return 1");

    // 测试查找
    ASSERT_TRUE(list.search_element(1, val), "Search key 1 failed");
    ASSERT_EQ(val, "one", "Key 1 value mismatch");
    
    ASSERT_TRUE(list.search_element(2, val), "Search key 2 failed");
    ASSERT_EQ(val, "two", "Key 2 value mismatch");

    ASSERT_TRUE(!list.search_element(3, val), "Search key 3 should fail");

    // 测试删除
    list.delete_element(1);
    ASSERT_TRUE(!list.search_element(1, val), "Key 1 should be deleted");
    ASSERT_EQ(list.size(), 1, "Size should be 1 after deletion");

    // 删除不存在的元素（不应崩溃）
    list.delete_element(3); 
    ASSERT_EQ(list.size(), 1, "Size should not change deleting non-existent key");

    std::cout << "PASSED" << std::endl;
}

// ----------------------------------------------------------------
// 2. Upsert (Insert_Set_Element) 测试
// ----------------------------------------------------------------
void TestUpsert() {
    std::cout << "[Test 2] Upsert Logic (Insert or Update)... ";

    SkipList<int, std::string> list(6);
    std::string val;

    // Case A: 键不存在 -> 执行插入
    std::string v1 = "version_1";
    list.insert_set_element(10, v1);
    
    ASSERT_TRUE(list.search_element(10, val), "Key 10 should be inserted");
    ASSERT_EQ(val, "version_1", "Value should be version_1");
    ASSERT_EQ(list.size(), 1, "Size should be 1");

    // Case B: 键已存在 -> 执行更新
    std::string v2 = "version_2";
    list.insert_set_element(10, v2); // 应该覆盖

    ASSERT_TRUE(list.search_element(10, val), "Key 10 should exist");
    ASSERT_EQ(val, "version_2", "Value should be updated to version_2");
    ASSERT_EQ(list.size(), 1, "Size should still be 1 (update not insert)");

    std::cout << "PASSED" << std::endl;
}

// ----------------------------------------------------------------
// 3. 并发读写测试 (Sanity Check for Locks)
// ----------------------------------------------------------------
void TestConcurrency() {
    std::cout << "[Test 3] Concurrency Sanity Check (Readers + Writer)... ";

    SkipList<int, int> list(12);
    int num_elements = 1000;
    int num_readers = 4;
    std::atomic<bool> done{false};

    // 预先填充一些数据
    for(int i=0; i<num_elements; ++i) {
        list.insert_element(i, i);
    }

    // 写线程：不断执行 upsert 操作 (修改 value)
    std::thread writer([&]() {
        for(int i=0; i<num_elements; ++i) {
            int newVal = i * 10;
            list.insert_set_element(i, newVal); // 使用写锁
            std::this_thread::sleep_for(std::chrono::microseconds(10)); // 模拟负载
        }
        done = true;
    });

    // 读线程组：不断执行 search 操作
    std::vector<std::thread> readers;
    for(int t=0; t<num_readers; ++t) {
        readers.emplace_back([&]() {
            int val;
            while(!done) {
                // 随机读取一个 key
                int target = rand() % num_elements;
                // 使用读锁 (shared_lock)
                if (list.search_element(target, val)) {
                    // 简单的验证：值要么是 i，要么是 i*10，不能是脏数据
                    if (val != target && val != target * 10) {
                        std::cerr << "Data corruption detected!" << std::endl;
                        std::exit(1);
                    }
                }
            }
        });
    }

    writer.join();
    for(auto& t : readers) {
        t.join();
    }

    // 最终验证大小没变
    ASSERT_EQ(list.size(), num_elements, "Concurrent upserts changed size unexpectedly");

    std::cout << "PASSED" << std::endl;
}

int main() {
    std::cout << "=== Starting SkipList Operations Tests ===" << std::endl;

    TestBasicOperations();
    TestUpsert();
    TestConcurrency();

    std::cout << "=== All Tests Passed ===" << std::endl;
    return 0;
}