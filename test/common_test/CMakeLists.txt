###########################################################
# 测试 1: 测试 "common" 模块
###########################################################


# --- defer_test ---

# 添加可执行测试文件目标，名字可以自定义
# CMake 会在 build 目录中，按照你的源目录结构来存放这个可执行文件。
# 可执行文件是在 test 这个目录下生成的，因此编译后的可执行文件路径是: MyDistributedStore/build/test/xxx.o
add_executable(defer_test defer.cpp)

# 头文件路径设置，测试代码只需要看到自己的头文件路径，不需要暴露给其他模块
target_include_directories(defer_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include  # 找 defer.h
)

# 将可执行文件注册为的测试用例
add_test(NAME DeferTest COMMAND defer_test)


# --- op_serialization_test ---

add_executable(op_serialization_test test_op_serialization.cpp)
target_link_libraries(op_serialization_test
    PRIVATE
        common   
        # boost_serialization   # 链接 Boost 序列化库
)
add_test(NAME SerializationTest COMMAND op_serialization_test)

# ----- LockQueue Test -----

add_executable(lock_queue_test test_lock_queue.cpp)
target_link_libraries(lock_queue_test
    PRIVATE
        common
)
add_test(NAME LockQueueTest COMMAND lock_queue_test)


# --- random timeout test ---
add_executable(random_timeout_test test_random_timeout.cpp)
target_link_libraries(random_timeout_test
    PRIVATE
        common
)
add_test(NAME RandomTimeoutTest COMMAND random_timeout_test)