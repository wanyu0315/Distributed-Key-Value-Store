#include "util.h" // 假设你的新 LockQueue 在这里
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory> // 用于 std::unique_ptr

// 全局互斥锁，仅用于保护 std::cout，防止测试日志交错
std::mutex g_print_mutex; 

// 一个简单的测试报告辅助函数
bool check(bool condition, const std::string& test_name, const std::string& message = "") {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    if (!condition) {
        std::cerr << "[Faild] " << test_name << ": " << message << std::endl;
        return false;
    }
    return true;
}

/**
 * @brief 测试用例 1: FIFO, Size, Empty (无界模式下的基本功能)
 */
bool test_fifo_and_size() {
    const std::string test_name = "test_fifo_and_size";
    std::cout << "Running: " << test_name << "..." << std::endl;
    LockQueue<int> queue; // max_capacity = 0 (无界)

    bool passed = true;
    passed &= check(queue.Empty(), test_name, "New queue should be empty");
    
    queue.Push(1);
    queue.Push(2);
    passed &= check(queue.Size() == 2, test_name, "Size should be 2");
    passed &= check(!queue.Empty(), test_name, "Queue should not be empty");

    int out;
    queue.Pop(out);
    passed &= check(out == 1, test_name, "FIFO order failed, expected 1");
    queue.Pop(out);
    passed &= check(out == 2, test_name, "FIFO order failed, expected 2");
    passed &= check(queue.Empty(), test_name, "Queue should be empty after popping all");
    
    return passed;
}

/**
 * @brief 测试用例 2: 移动语义 (零拷贝)
 * @details 使用 std::unique_ptr (一个只能移动不能拷贝的对象) 来验证
 */
bool test_move_semantics() {
    const std::string test_name = "test_move_semantics";
    std::cout << "Running: " << test_name << "..." << std::endl;
    LockQueue<std::unique_ptr<int>> queue;

    auto ptr_in = std::make_unique<int>(123);
    queue.Push(std::move(ptr_in)); // 移动 Push

    // 此时 ptr_in 应该变为 nullptr
    bool passed = true;
    passed &= check(ptr_in == nullptr, test_name, "Push(T&&) did not move the object");

    std::unique_ptr<int> ptr_out;
    queue.Pop(ptr_out); // Pop 到 out_data
    passed &= check(ptr_out != nullptr, test_name, "Pop() failed to retrieve object");
    if (ptr_out) {
        passed &= check(*ptr_out == 123, test_name, "Pop() retrieved wrong data");
    }
    return passed;
}

/**
 * @brief 测试用例 3: 有界队列 - 阻塞 Push
 */
bool test_bounded_blocking_push() {
    const std::string test_name = "test_bounded_blocking_push";
    std::cout << "Running: " << test_name << "..." << std::endl;
    LockQueue<int> queue(1); // 容量为 1

    queue.Push(1);
    bool passed = true;
    passed &= check(queue.IsFull(), test_name, "Queue should be full");

    std::atomic<bool> push_returned(false);
    
    // 启动一个线程，它应该在 Push(2) 时阻塞
    std::thread producer_thread([&]() {
        queue.Push(2);
        push_returned = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    passed &= check(!push_returned, test_name, "Push() should be blocked");

    int out;
    queue.Pop(out); // 消费一个, 腾出空间
    passed &= check(out == 1, test_name, "Pop() expected 1");

    producer_thread.join(); // 此时线程应该被唤醒并结束
    passed &= check(push_returned, test_name, "Producer thread did not unblock and return");

    queue.Pop(out);
    passed &= check(out == 2, test_name, "Pop() expected 2");
    
    return passed;
}

/**
 * @brief 测试用例 4: 有界队列 - 超时 Push
 */
bool test_timeout_push_on_full() {
    const std::string test_name = "test_timeout_push_on_full";
    std::cout << "Running: " << test_name << "..." << std::endl;
    LockQueue<int> queue(1); // 容量为 1
    queue.Push(1);

    auto start = std::chrono::steady_clock::now();
    // 尝试 Push, 应该在 50ms 后超时失败
    bool success = queue.timeOutPush(2, 50);
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    bool passed = true;
    passed &= check(!success, test_name, "timeOutPush should return false when full");
    passed &= check(duration >= 50 && duration < 80, test_name, "Timeout duration was not correct");
    passed &= check(queue.Size() == 1, test_name, "Queue size should not change on failed push");

    return passed;
}

/**
 * @brief 测试用例 5: Shutdown 唤醒阻塞的 Pop
 */
bool test_shutdown_unblocks_pop() {
    const std::string test_name = "test_shutdown_unblocks_pop";
    std::cout << "Running: " << test_name << "..." << std::endl;
    LockQueue<int> queue;
    std::atomic<bool> pop_returned(false);  //声明一个原子布尔变量 pop_returned，初始值 false
    std::atomic<bool> pop_success(true); // 默认为 true, 期待它变为 false

    std::thread consumer_thread([&]() {
        int out;
        pop_success = queue.Pop(out); // 应该阻塞，把Pop的返回值（bool类型）赋值给pop_success
        pop_returned = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool passed = true;
    passed &= check(!pop_returned, test_name, "Pop() should be blocked");

    queue.Shutdown(); // 关闭队列，唤醒阻塞的 Pop，此后 Pop 可以继续执行并返回false（没有可以弹出的值）
    consumer_thread.join(); //主线程会在这里“卡住”，直到 子线程consumer_thread 退出

    passed &= check(pop_returned, test_name, "Shutdown() did not unblock Pop()");
    passed &= check(!pop_success, test_name, "Pop() should return false when woken by Shutdown");
    
    // 验证 Shutdown 后 Pop 立即失败
    int out;
    passed &= check(!queue.Pop(out), test_name, "Pop() after Shutdown should return false");
    // 验证 Shutdown 后 Push 立即失败
    passed &= check(!queue.Push(1), test_name, "Push() after Shutdown should return false");
    
    return passed;
}

/**
 * @brief 测试用例 6: Shutdown 唤醒阻塞的 Push
 */
bool test_shutdown_unblocks_push() {
    const std::string test_name = "test_shutdown_unblocks_push";
    std::cout << "Running: " << test_name << "..." << std::endl;
    LockQueue<int> queue(1);
    queue.Push(1);

    std::atomic<bool> push_returned(false);
    std::atomic<bool> push_success(true);

    std::thread producer_thread([&]() {
        push_success = queue.Push(2); // 应该阻塞
        push_returned = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool passed = true;
    passed &= check(!push_returned, test_name, "Push() should be blocked");

    queue.Shutdown();
    producer_thread.join();

    passed &= check(push_returned, test_name, "Shutdown() did not unblock Push()");
    passed &= check(!push_success, test_name, "Push() should return false when woken by Shutdown");
    
    return passed;
}

/**
 * @brief 测试用例 7: 批量操作 (PushBatch / PopBatch)
 */
bool test_batch_operations() {
    const std::string test_name = "test_batch_operations";
    std::cout << "Running: " << test_name << "..." << std::endl;
    LockQueue<int> queue(10); // 容量 10

    std::vector<int> batch_in;
    batch_in.push_back(10);
    batch_in.push_back(20);
    batch_in.push_back(30);

    queue.PushBatch(std::move(batch_in));
    bool passed = true;
    passed &= check(queue.Size() == 3, test_name, "Size after PushBatch is wrong");

    std::vector<int> batch_out;
    size_t count = queue.PopBatch(batch_out, 5); // 尝试 Pop 5 个

    passed &= check(count == 3, test_name, "PopBatch should return 3");
    passed &= check(batch_out.size() == 3, test_name, "PopBatch vector size is wrong");
    passed &= check(queue.Empty(), test_name, "Queue should be empty after PopBatch");
    if (batch_out.size() == 3) {
        passed &= check(batch_out[0] == 10 && batch_out[1] == 20 && batch_out[2] == 30, test_name, "Batch data mismatch");
    }

    return passed;
}

/**
 * @brief 测试用例 8: 压力测试 (多生产者, 单消费者, 有界队列)
 * @details 验证在高并发、有界、关闭时的数据完整性
 */
bool test_stress_multi_producer_consumer() {
    const std::string test_name = "test_stress_multi_producer_consumer";
    std::cout << "Running: " << test_name << "..." << std::endl;
    
    // 使用有界队列来测试反压 (backpressure)
    LockQueue<int> queue(100); 
    
    const int NUM_PRODUCERS = 4; // 4 个生产者线程
    const int ITEMS_PER_PRODUCER = 500; // 每个生产者生产 500 个项目
    const int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER; // 2000

    std::vector<std::thread> producers;
    // 启动4个生产者线程，每个负责 500 次 Push()
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        producers.emplace_back([&queue, i]() { 
            for (int j = 0; j < ITEMS_PER_PRODUCER; ++j) {
                // 生产 1000-1499, 2000-2499, ...
                int value = (i + 1) * 1000 + j;
                if (!queue.Push(value)) {
                    // 如果 Push 失败 (因为 Shutdown), 就退出
                    break;
                }
            }
        });
    }

    std::map<int, int> counts; // 统计收到的数据
    
    // 消费者线程: 循环 Pop 直到 Pop 返回 false (即队列已关闭且为空)
    std::thread consumer([&]() {
        int val;
        while (queue.Pop(val)) {
            counts[val]++;
        }
    });

    // 等待所有生产者完成
    for (auto& t : producers) {
        t.join();
    }

    // 生产者已完成, 关闭队列以通知消费者
    queue.Shutdown();

    // 等待消费者退出循环并结束
    consumer.join();

    // 验证，如果不是 2000，说明数据丢失（某些生产者的值没被消费到）
    bool passed = true;
    passed &= check(counts.size() == TOTAL_ITEMS, test_name, 
        "Data loss! Expected " + std::to_string(TOTAL_ITEMS) + " items, but got " + std::to_string(counts.size()));

    for (auto const& [key, val] : counts) {
        if (val != 1) {
            passed &= check(false, test_name, "Duplicate item " + std::to_string(key));
            break;
        }
    }

    return passed;
}


int main() {
    int passed = 0;
    const int total = 8;

    if (test_fifo_and_size()) passed++;
    if (test_move_semantics()) passed++;
    if (test_bounded_blocking_push()) passed++;
    if (test_timeout_push_on_full()) passed++;
    if (test_shutdown_unblocks_pop()) passed++;
    if (test_shutdown_unblocks_push()) passed++;
    if (test_batch_operations()) passed++;
    if (test_stress_multi_producer_consumer()) passed++;


    std::cout << "----------------------------------" << std::endl;
    std::cout << "Test Summary: " << passed << " / " << total << " tests passed." << std::endl;
    std::cout << "----------------------------------" << std::endl;

    return (passed == total) ? 0 : 1;
}