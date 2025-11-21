# 测试说明
test模块均支持ctest测试
## 辅助工具：Defer / DEFER 宏 的测试
defer.cpp是DEFER宏的测试源码，使用cmake --build . --target defer_test生成对应可执行文件，执行即可测试。
测试的期望输出为：
Testing DEFER macro:
Main function is done
This d2 can be dismissed
defer 1
defer 2
表示析构以后才执行宏展开的内容

## Op 类的序列化的测试
这是最高优先级的测试。如果序列化/反序列化（asString / parseFromString）出现 Bug，会导致数据损坏、日志不一致、RPC 失败，是分布式系统的"绝症"。

### 测试目标：确保一个 Op 对象经过“序列化 -> 反序列化”后，所有字段保持不变。

### 测试用例：

1. 往返测试 (Round-trip Test)：

    创建一个 Op 对象 op1，并填充所有字段（Operation, Key, Value, ClientId, RequestId）。
    调用 std::string payload = op1.asString() 将其序列化。
    创建一个新的 Op 对象 op2。
    调用 op2.parseFromString(payload) 进行反序列化。
    断言 (Assert)：ASSERT(op1.Operation == op2.Operation), ASSERT(op1.Key == op2.Key), ASSERT(op1.RequestId == op2.RequestId)... 确保所有字段都完全相等。

2. 边界情况测试：

    测试 Value 为空字符串（""）时，往返测试是否依然通过。
    测试 Key 或 Value 包含特殊字符时（如果 boost::text_archive 支持）是否通过。

3. 失败情况测试：

    测试 parseFromString 是否能安全地处理无效输入，而不崩溃。

### 测试信息
    测试文件：test_op_serialization.cpp

    调试方法：
     cmake --build . --target op_serialization_test
     ./test/op_serialization_test

    期望输入输出：
    测试函数内部内定了输入。期望输出为：
    Running: test_op_roundtrip_full...
        序列化前的 Op 对象键值对: { Key：myKey, Value：myValue_!@#$_123 }
        反序列化后的 Op 对象键值对: { key：myKey, value：myValue_!@#$_123 }
    Running: test_op_roundtrip_empty_value...
    Running: test_parse_failure...
    ----------------------------------
    Test Summary: 3 / 3 tests passed.
    ----------------------------------
    表示3种测试都成功通过了，基于BOOST库的序列化和反序列化且给出了其中键值对的结果示范。


## LockQueue线程安全队列测试

### 测试用例
此测试包含了 8 个测试用例，覆盖了所有新功能：

1. test_fifo_and_size: 基础功能（无限队列下的测试）：先进先出，Size(), Empty()。
2. test_move_semantics: （性能） 验证 Push(T&&) 和 Pop(T&) 确实在“移动”数据（零拷贝）。
3. test_bounded_blocking_push: （容量） 验证 Push 在队列满时会阻塞。
4. test_timeout_push_on_full: （容量） 验证 timeOutPush 在队列满时会正确超时（如果超时还没取到，就返回 false）。
5. test_shutdown_unblocks_pop: （关闭） 验证 Shutdown 能唤醒阻塞的 Pop。
6. test_shutdown_unblocks_push: （关闭） 验证 Shutdown 能唤醒阻塞的 Push。
7. test_batch_operations: （批量） 验证 PushBatch 和 PopBatch。
8. test_stress_multi_producer_consumer: （最终压力测试） 在高并发下，使用有界队列和 Shutdown 机制，验证数据100%不丢失。

### 测试信息
    测试文件：test_lock_queue.cpp

    调试方法：
     cmake --build . --target lock_queue_test
     ./test/lock_queue_test

    期望输入输出：
    Running: test_fifo_and_size...
    Running: test_move_semantics...
    Running: test_bounded_blocking_push...
    Running: test_timeout_push_on_full...
    Running: test_shutdown_unblocks_pop...
    Running: test_shutdown_unblocks_push...
    Running: test_batch_operations...
    Running: test_stress_multi_producer_consumer...
    ----------------------------------
    Test Summary: 8 / 8 tests passed.
    
    表示所有测试用例正确通过

## random_timeout随机选举时间测试
 
### 测试目标
不仅要测试它的正确性（范围），还要测试它的并发安全性。

### 测试用例：
1. 单线程测试：验证其功能、范围和随机分布的正确性。
2. 多线程（并发）测试：（最重要的） 验证 thread_local 静态引擎在多线程同时调用时是安全的（不崩溃、不产生数据竞争），并且每个线程都有自己独立的随机序列。

### 测试信息
    测试文件：test_random_timeout.cpp

    调试方法：
     cmake --build . --target random_timeout_test
     ./test/random_timeout_test

    期望输出：
    ----------------------------------
    Running Test: range and randomness test(Single-threaded)...
    [INFO]   test_range_and_randomness: 迭代次数：5000
    [INFO]   test_range_and_randomness: 一共出现的随机值个数：201
    [INFO]   test_range_and_randomness: 理论均值: 400.000000
    [INFO]   test_range_and_randomness: 实际均值:   400.255200
    [INFO]   test_range_and_randomness: 允许的误差范围:     +/- 10.000000
    [PASSED] Test: range and randomness test(Single-threaded) have passed 
    ----------------------------------
    Running Test: concurrency test(Multi-threaded)...
    [INFO]   test_concurrency: Launching 10 threads, 1000 iterations each...
    [INFO]   test_concurrency: All threads joined.
    [INFO]   test_concurrency: number of threads：10 number of threads with different random sequences: 10
    [PASSED] Test: concurrency test(Multi-threaded) have passed 
    ==================================
    Test Summary: 2 / 2 tests passed.
    ==================================
    测试结果打印了单线程和多线程测试中随机数生成的信息，两个测试用例都通过了，表示其功能、范围和随机分布的正确性以及 thread_local 静态引擎在多线程同时调用时是安全的（不崩溃、不产生数据竞争），并且每个线程都有自己独立的随机序列。
