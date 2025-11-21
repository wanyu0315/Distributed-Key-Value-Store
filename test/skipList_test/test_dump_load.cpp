#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <chrono>
#include <map>
#include "skipList.h" 

// 简单的测试辅助宏
#define ASSERT_EQ(val1, val2) \
    if ((val1) != (val2)) { \
        std::cerr << "Assertion failed at line " << __LINE__ << ": " \
                  << (val1) << " != " << (val2) << std::endl; \
        std::exit(1); \
    }

#define ASSERT_TRUE(condition) \
    if (!(condition)) { \
        std::cerr << "Assertion failed at line " << __LINE__ << ": " \
                  << #condition << " is false" << std::endl; \
        std::exit(1); \
    }

void TestBasicRoundTrip() {
    std::cout << "[Test 1] Basic Round-Trip (Insert -> Dump -> Load)... ";
    
    // 1. 准备原始数据
    SkipList<int, std::string> list1(6);
    list1.insert_element(1, "one");
    list1.insert_element(2, "two");
    list1.insert_element(10, "ten");

    // 2. 执行 Dump
    std::string snapshot = list1.dump_file();
    ASSERT_TRUE(!snapshot.empty());

    // 3. 创建一个新的空跳表并 Load
    SkipList<int, std::string> list2(6);
    list2.load_file(snapshot);

    // 4. 验证 list2 是否与 list1 一致
    ASSERT_EQ(list1.size(), list2.size());
    
    std::string val;
    ASSERT_TRUE(list2.search_element(1, val));
    ASSERT_EQ(val, "one");
    
    ASSERT_TRUE(list2.search_element(10, val));
    ASSERT_EQ(val, "ten");
    
    // 验证不存在的元素
    ASSERT_TRUE(!list2.search_element(3, val));

    std::cout << "PASSED" << std::endl;
}

// 验证load_file 在加载快照前，是否彻底清除了内存中的旧数据。
void TestStateReplacement() {
    std::cout << "[Test 2] State Replacement (Raft Snapshot Logic)... ";

    // 1. 准备快照数据 (SnapA)
    SkipList<int, std::string> listA(6);
    listA.insert_element(100, "old_100");
    listA.insert_element(200, "old_200");
    std::string snapshotA = listA.dump_file();

    // 2. 准备一个已经有脏数据的跳表 (ListB)
    SkipList<int, std::string> listB(6);
    listB.insert_element(1, "dirty_1");
    listB.insert_element(999, "dirty_999");
    
    // 确保 ListB 目前有数据
    ASSERT_EQ(listB.size(), 2);

    // 3. 在 ListB 上加载 SnapA
    // 预期：ListB 的旧数据 (1, 999) 被清空，只保留 (100, 200)
    listB.load_file(snapshotA);

    // 4. 验证
    ASSERT_EQ(listB.size(), 2); // 只有两个元素
    
    std::string val;
    // 旧数据应该消失
    ASSERT_TRUE(!listB.search_element(1, val));
    ASSERT_TRUE(!listB.search_element(999, val));
    
    // 新数据应该存在
    ASSERT_TRUE(listB.search_element(100, val));
    ASSERT_EQ(val, "old_100");

    std::cout << "PASSED" << std::endl;
}

// 验证 boost::archive::binary_oarchive 带来的性能提升，以及验证在大规模数据下 std::shared_mutex 是否引入了过大的锁开销
void TestPerformanceAndIntegrity() {
    std::cout << "[Test 3] Performance & Integrity (100k elements)... " << std::endl;

    int element_count = 100000;
    SkipList<int, int> perfList(18); // 使用更高的层级以适应大数据

    // 1. 插入数据
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < element_count; ++i) {
        perfList.insert_element(i, i * 2);
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    std::cout << "    -> Insert 100k items: " << elapsed.count() << "s" << std::endl;

    // 2. Dump (序列化性能)
    start = std::chrono::high_resolution_clock::now();
    std::string snapshot = perfList.dump_file();
    end = std::chrono::high_resolution_clock::now();
    elapsed = end - start;
    std::cout << "    -> Dump (Binary Serialize): " << elapsed.count() << "s" << std::endl;
    std::cout << "    -> Snapshot Size: " << snapshot.size() / 1024 / 1024 << " MB" << std::endl;

    // 3. Load (反序列化性能)
    SkipList<int, int> recoverList(18);
    start = std::chrono::high_resolution_clock::now();
    recoverList.load_file(snapshot);
    end = std::chrono::high_resolution_clock::now();
    elapsed = end - start;
    std::cout << "    -> Load (Binary Deserialize & Rebuild): " << elapsed.count() << "s" << std::endl;

    // 4. 随机验证数据完整性
    ASSERT_EQ(perfList.size(), recoverList.size());
    ASSERT_EQ(recoverList.size(), element_count);

    int val;
    // 验证头部
    ASSERT_TRUE(recoverList.search_element(0, val));
    ASSERT_EQ(val, 0);
    // 验证尾部
    ASSERT_TRUE(recoverList.search_element(element_count - 1, val));
    ASSERT_EQ(val, (element_count - 1) * 2);
    // 验证中间某个值
    ASSERT_TRUE(recoverList.search_element(50000, val));
    ASSERT_EQ(val, 100000);

    std::cout << "    -> Integrity Check: PASSED" << std::endl;
    std::cout << "[Test 3] PASSED" << std::endl;
}

int main() {
    std::cout << "=== Starting SkipList Dump/Load Tests ===" << std::endl;
    
    TestBasicRoundTrip();
    TestStateReplacement();
    TestPerformanceAndIntegrity();

    std::cout << "=== All Tests Passed ===" << std::endl;
    return 0;
}