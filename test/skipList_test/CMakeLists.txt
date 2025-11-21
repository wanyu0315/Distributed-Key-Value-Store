###########################################################
# 测试 2: 测试 "SkipList" 模块
###########################################################

# --- skiplist_dump_load_test ---

add_executable(skiplist_dump_load_test test_dump_load.cpp)
target_link_libraries(skiplist_dump_load_test
    PRIVATE
        skipList
        common
        Boost::system          # 通常也建议链接 system
)
add_test(NAME SkipListDumpLoadTest COMMAND skiplist_dump_load_test)

# --- skiplist_ops_test ---

add_executable(skiplist_ops_test test_skiplist_ops.cpp)
target_link_libraries(skiplist_ops_test
    PRIVATE
        skipList
        common
)
add_test(NAME SkipListOpsTest COMMAND skiplist_ops_test)
