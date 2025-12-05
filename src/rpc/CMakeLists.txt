# src/rpc/CMakeLists.txt

# 1. 收集源文件
set(RPC_SRCS
    rpcprovider.cpp
    # 如果有其他 .cpp 文件（如 mprpcconfig.cpp 等）也加在这里
)

# 2. 定义库目标 (rpc_provider_lib)
add_library(rpc_provider_lib ${RPC_SRCS})

# 3. 【核心配置】导出头文件路径
# PUBLIC 关键字意味着：
#   (1) 编译这个库自己时，能找到 include/ 下的头文件
#   (2) 任何链接了这个库的其他目标 (如 test_rpc_config)，也会自动把 include/ 加入搜索路径
target_include_directories(rpc_provider_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 4. 为rpc_provider_lib链接依赖库
# 注意：因为 rpcprovider.h 里面引用了 muduo 和 protobuf 的头文件，
# 所以这里的链接属性也建议用 PUBLIC，让依赖传递下去
target_link_libraries(rpc_provider_lib
    PUBLIC
        muduo_net
        muduo_base
        protobuf::libprotobuf
        rpc_proto_lib  # 你之前编译 proto 生成的库
        Threads::Threads
)
