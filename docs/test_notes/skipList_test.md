# dump_file和load_file函数功能测试
## 测试用例

这个测试包含三个关键部分：
1. 基础正确性：写入 -> Dump -> Load -> 验证数据一致。
2. 状态替换测试：验证 load_file 是否真能清空旧数据（Raft 关键需求）。
3. 性能/大数据测试：插入 10万+ 数据，测试二进制序列化的速度和压缩后的正确性

## 测试目的与分析
为什么这样设计测试？
1. TestBasicRoundTrip (基础验证)

目的：验证最基本的序列化和反序列化功能是否工作。
场景：插入少量数据，确保数据在转换成二进制字符串并还原后，key 和 value 依然匹配。这是“冒烟测试”。

2. TestStateReplacement (状态替换 / Raft Snapshot 逻辑)

目的：这是最重要的测试，直接针对我们刚才修复的 load_file Bug（状态清除）。
场景：Raft 节点滞后时，会接收 Leader 的快照。此时节点内存中可能包含旧的、未提交的甚至错误的日志应用结果。
验证点：必须确认 load_file 在加载快照前，彻底清除了内存中的旧数据。如果测试失败（比如 search(1) 仍然返回 true），说明你的节点在安装快照后会产生数据不一致，导致脑裂或数据损坏。

3. TestPerformanceAndIntegrity (性能与大数据)

目的：验证 boost::archive::binary_oarchive 带来的性能提升，以及验证在大规模数据下 std::shared_mutex 是否引入了过大的锁开销。
场景：模拟 10万 条数据（接近生产环境的一个小型快照）。
验证点：
耗时：观察 Dump 和 Load 的时间。二进制格式通常比文本格式快 5-10 倍。
大小：打印快照大小，二进制格式应该比纯文本更紧凑。
完整性：确保 10万 条数据在经过“内存->二进制->内存”的转换后，一条不少，且首尾数据正确。

## 测试信息
    测试源文件：test/skipList_test/test_dump_load.cpp

    调试方法：
        cmake --build . --target skiplist_dump_load_test
        ./test/skiplist/skiplist_dump_load_test
    期望输出：
        === Starting SkipList Dump/Load Tests ===
        [Test 1] Basic Round-Trip (Insert -> Dump -> Load)... dump_file-----------------
        PASSED
        [Test 2] State Replacement (Raft Snapshot Logic)... dump_file-----------------
        PASSED
        [Test 3] Performance & Integrity (100k elements)... 
            -> Insert 100k items: 0.044207s
        dump_file-----------------
            -> Dump (Binary Serialize): 0.0114536s
            -> Snapshot Size: 0 MB
            -> Load (Binary Deserialize & Rebuild): 0.0396221s
            -> Integrity Check: PASSED
        [Test 3] PASSED
        === All Tests Passed ===
        测试1通过说明能够正常进行基本的跳表序列化和反序列化功能工作。
        测试2通过说明load_file 在加载快照前，彻底清除了内存中的旧数据。
        测试3通过说明完整进行大规模数据的序列化和反序列化不出错。

# 跳表功能测试

## 测试目的
验证核心的增删改查逻辑，并包含一个多线程并发测试，以确保引入的 std::shared_mutex (读写锁) 能够正常工作且无死锁。

## 测试用例

测试文件包含三个部分：
1. 基础功能测试：验证 insert, search, delete 的基本逻辑。
2. Upsert (覆盖写入) 测试：专门验证 insert_set_element 是否能正确处理“不存在则插入，存在则更新”。
3. 并发压力测试：启动多个读者线程和一个写者线程，验证读写锁是否导致死锁或数据竞争崩溃。

## 测试信息
    测试源文件：test/skipList_test/test_skiplist_ops.cpp
    调试指令：cmake --build . --target skiplist_ops_test

    期望输出：
        === Starting SkipList Operations Tests ===
        [Test 1] Basic Operations (Insert/Search/Delete)... PASSED
        [Test 2] Upsert Logic (Insert or Update)... PASSED
        [Test 3] Concurrency Sanity Check (Readers + Writer)... PASSED
        === All Tests Passed ===
        三个测试全部通过，说明跳表的核心增删改查功能能够正常实现。且在多线程并发时也能够正常工作无死锁。


