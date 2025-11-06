# src/skipList/CMakeLists.txt

# 1. 定义一个名为 "skipList" 的 INTERFACE 库
#    (因为它只有头文件，没有 .cpp 文件)
add_library(skipList INTERFACE)

# 2. 设置 "skipList" 的 "PUBLIC" (公开) 头文件路径
#    这样，任何链接了 skipList 的模块 (如 test_skiplist) 
#    都能自动找到 src/skipList/include 目录
target_include_directories(skipList
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 3. (关键) 声明 skipList (头文件) 依赖于 common
#    (例如，如果您的 skipList.h 里面 #include "util.h" 或 "config.h")
#    这会将 common 的 include 路径也 "传递" 给最终的测试程序
target_link_libraries(skipList
    INTERFACE
        common
)