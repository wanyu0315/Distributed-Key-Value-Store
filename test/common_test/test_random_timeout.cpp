#include "util.h"   // 包含你要测试的函数
#include "config.h" // 包含 min/max 常量
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <set>           // 包含了 <set>
#include <numeric>       // 用于 std::accumulate
#include <cmath>         // 用于 std::abs
#include <iomanip>       // 用于 std::fixed, std::setprecision (美化输出)
#include <functional>    // 用于 std::function

// 全局互斥锁，仅用于保护 std::cout，防止测试日志交错
std::mutex g_print_mutex; 

// check 函数: 保持不变, 只在失败时打印
bool check(bool condition, const std::string& test_name, const std::string& message = "") {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    if (!condition) {
        // std::cerr 是一种好的实践, 但为了确保顺序, 我们都用 cout
        std::cout << "[FAILED] " << test_name << ": " << message << std::endl;
        return false;
    }
    return true;
}

/**
 * @brief 辅助函数, 打印信息 (线程安全)
 */
void print_info(const std::string& test_name, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << "[INFO]   " << test_name << ": " << message << std::endl;
}

/**
 * @brief 测试用例 1: 范围和随机性 (单线程)
 * @details 验证函数返回的值总是在 [min, max] 范围内，
 * 并且具有良好的随机分布（通过平均值检查）。
 */
bool test_range_and_randomness() {
    const std::string test_name = "test_range_and_randomness";
    // 移除 "Running..." 打印, 交给 main 处理

    const int iterations = 5000;
    std::map<int, int> counts;  // 用于存储 [值 -> 出现次数]
    double sum = 0;
    bool all_in_range = true;

    for (int i = 0; i < iterations; ++i) {
        int ms = getRandomizedElectionTimeout().count();
        
        if (ms < minRandomizedElectionTime || ms > maxRandomizedElectionTime) {
            // 只在第一次出错时打印
            if (all_in_range) { 
                check(false, test_name, "Value " + std::to_string(ms) + " is out of range [" + 
                       std::to_string(minRandomizedElectionTime) + ", " + 
                       std::to_string(maxRandomizedElectionTime) + "]");
            }
            all_in_range = false;
        }
        
        counts[ms]++;
        sum += ms;
    }
    if (!all_in_range) return false; // 范围错误是硬故障

    //随机值的统计信息
    double actual_mean = sum / iterations;  // 实际均值
    double expected_mean = (minRandomizedElectionTime + maxRandomizedElectionTime) / 2.0;  // 理论均值
    double tolerance = (maxRandomizedElectionTime - minRandomizedElectionTime) * 0.05;  // 5%的误差范围

    // 打印统计信息
    std::cout << std::fixed << std::setprecision(2);
    print_info(test_name, "迭代次数：" + std::to_string(iterations));
    print_info(test_name, "一共出现的随机值个数：" + std::to_string(counts.size()));
    print_info(test_name, "理论均值: " + std::to_string(expected_mean));
    print_info(test_name, "实际均值:   " + std::to_string(actual_mean));
    print_info(test_name, "允许的误差范围:     +/- " + std::to_string(tolerance));
    std::cout.unsetf(std::ios::fixed); // 恢复默认

    bool passed = true;
    passed &= check(counts.size() > 10, test_name, "Function seems to have low randomness (only " + std::to_string(counts.size()) + " unique values)");
    passed &= check(std::abs(actual_mean - expected_mean) < tolerance, 
                    test_name, "Actual mean is outside the tolerance range.");

    return passed;
}

/**
 * @brief 测试用例 2: 并发测试 (多线程)
 * @details 验证多个线程同时调用函数时，程序不崩溃 (无数据竞争)，
 * 并且每个线程都能正确获取在范围内的值。
 * 这是对 `thread_local` 实现的关键测试。
 */
bool test_concurrency() {
    const std::string test_name = "test_concurrency";
    // 移除 "Running..." 打印

    const int num_threads = 10; // 启动 10 个线程
    const int iterations_per_thread = 1000; // 每个线程调用 1000 次
    std::vector<std::thread> threads;
    std::vector<std::vector<int>> results(num_threads); // 每个线程存储自己的结果

    std::atomic<bool> threads_passed(true); // 使用 atomic bool 

    print_info(test_name, "Launching " + std::to_string(num_threads) + " threads, " + 
               std::to_string(iterations_per_thread) + " iterations each...");

    for (int i = 0; i < num_threads; ++i) {
        // vector<std::thread> threads中每个线程执行的函数
        threads.emplace_back([&, i]() { // 注意: 捕获 i (值捕获)
            bool local_passed = true;
            for (int j = 0; j < iterations_per_thread; ++j) {
                int ms = getRandomizedElectionTimeout().count();
                results[i].push_back(ms); 

                if (ms < minRandomizedElectionTime || ms > maxRandomizedElectionTime) {
                    local_passed = false;
                }
            }
            if (!local_passed) {
                // check 函数是线程安全的
                check(false, "test_concurrency (Thread " + std::to_string(i) + ")", "One or more values were out of range.");
                threads_passed = false; // 原子写入
            }
        });
    }

    // 等待所有线程结束
    for (auto& t : threads) {
        t.join();
    }
    
    print_info(test_name, "All threads joined.");

    // 主要验证：程序没有崩溃，并且所有线程的检查都通过了
    bool passed = check(threads_passed, test_name, "One or more threads generated values out of range.");
    if (!passed) return false; // 如果有线程失败，则提前退出

    // 检查不同线程是否得到了不同的随机序列
    std::set<int> first_results;
    for(int i = 0; i < num_threads; ++i) {
        if (!results[i].empty()) {
            first_results.insert(results[i][0]);
        }
    }
    
    print_info(test_name, "number of threads：" + std::to_string(num_threads) + 
               " number of threads with different random sequences: " + std::to_string(first_results.size()));
               
    passed &= check(first_results.size() > 1 || num_threads == 1, test_name, 
                    "All threads seem to have the same random sequence (unlikely, but a possible false negative)");

    return passed;
}


/**
 * @brief 新增的 main 函数, 包含一个测试运行器
 */
int main() {
    int passed = 0;
    int total = 0;

    // 定义一个运行器 lambda
    auto run_test = [&](std::function<bool()> test_func, const std::string& test_name) {
        total++;
        std::cout << "----------------------------------" << std::endl;
        std::cout << "Running Test: " << test_name << "..." << std::endl;
        
        bool success = test_func();
        
        if (success) {
            passed++;
            std::cout << "[PASSED] Test: " << test_name << " have passed " << std::endl;
        } else {
            // check() 函数已经打印了具体错误
            std::cout << "[FAILED] Test: " << test_name << " (See errors above)" << std::endl;
        }
    };

    // 运行所有测试
    run_test(test_range_and_randomness, "range and randomness test(Single-threaded)");
    run_test(test_concurrency, "concurrency test(Multi-threaded)");

    // 打印最终总结
    std::cout << "==================================" << std::endl;
    std::cout << "Test Summary: " << passed << " / " << total << " tests passed." << std::endl;
    std::cout << "==================================" << std::endl;

    return (passed == total) ? 0 : 1;
}