# 定义 Protobuf 生成的测试服务代码

# 指定 test_service.proto 文件路径
set(TEST_PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../../proto/test_service.proto)

#  它会在编译后自动把 生成的 .pb.h 和 .pb.cc 放到 当前 build 目录
protobuf_generate_cpp(TEST_PROTO_GEN_SRCS TEST_PROTO_GEN_HDRS ${TEST_PROTO_SRCS})

add_library(test_proto_lib ${TEST_PROTO_GEN_SRCS} ${TEST_PROTO_GEN_HDRS})
target_link_libraries(test_proto_lib PUBLIC protobuf::libprotobuf)

# 任何链接了 test_proto_lib 的目标，都会自动获得这个 include 路径
target_include_directories(test_proto_lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# 1. 基础配置测试
add_executable(rpc_config_test test_rpc_config.cpp)
target_link_libraries(rpc_config_test 
    PRIVATE 
    rpc_lib # 你的 rpcprovider 库目标名
)
add_test(NAME RpcConfigTest COMMAND rpc_config_test)

# 2. 服务注册与发现测试
add_executable(rpc_registry_test test_rpc_registry.cpp)
target_link_libraries(rpc_registry_test
    PRIVATE
    rpc_lib # rpcprovider 库
    test_proto_lib  # 测试服务的 protobuf 库
)

# 3. 协议解析测试 (核心)
add_executable(rpc_protocol_test test_rpc_protocol.cpp)
target_link_libraries(rpc_protocol_test
    PRIVATE
    rpc_lib # rpcprovider 库
)