# src/common/CMakeLists.txt

# 1. 定义一个名为 "common" 的 库
#    (注意：这里只添加 .cpp 文件，不添加 .h 文件)
add_library(common
    util.cpp
)

# 2. 告诉CMake这个模块的头文件在哪里
target_include_directories(common
    PUBLIC
        # "PUBLIC" 表示: 
        # 1. 任何链接了 common 的模块 (如 skipList) 
        #    会自动获得这个 include 路径。
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        # "PRIVATE" 表示:
        # 1. 只有 common 自己的 .cpp (如 util.cpp) 
        #    能看到这个 include 路径。
        ${CMAKE_CURRENT_SOURCE_DIR}
)
# 3. 链接 common 需要的外部库 ,此后链接 common 的模块也会自动链接这些库
target_link_libraries(common
    PUBLIC
        Boost::serialization
        Threads::Threads   #  LockQueue 需要它
)

# 这样设置后:
# - CMAKE_CURRENT_SOURCE_DIR 对应 src/common
# - 在 util.cpp 中，您可以这样写: #include "include/util.h"
# - 在其他模块 (如 skipList) 中，您可以直接写: #include "util.h"
# - PUBLIC 的对象：所有链接 common 的目标也能用到 include 路径的头文件，也就是 include下的头文件都能被其他.cpp文件使用
# - PRIVATE 的对象： 只有 common 自己的.cpp源文件能用，也就是 util.cpp不能被其他.cpp文件使用