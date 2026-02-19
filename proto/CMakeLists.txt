# proto/CMakeLists.txt

# 1. 查找 gRPC 和 Protobuf (要求必需)
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf REQUIRED)

# 2. 获取 gRPC C++ 插件的路径
get_target_property(gRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin LOCATION)

# 3. 查找所有 .proto 文件
# 添加 CONFIGURE_DEPENDS。如果你新增了 raft.proto，无需手动重新 cmake，make 会自动发现新文件！
file(GLOB PROTO_FILES CONFIGURE_DEPENDS "*.proto")

# 4. 初始化文件列表
set(PROTO_HDRS)
set(PROTO_SRCS)
set(GRPC_HDRS)
set(GRPC_SRCS)

# 5. 遍历生成编译命令
foreach(FIL ${PROTO_FILES})
    get_filename_component(ABS_FIL ${FIL} ABSOLUTE)
    get_filename_component(FIL_WE ${FIL} NAME_WE)

    # 定义生成的文件名
    set(_PROTO_H "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.h")
    set(_PROTO_SRC "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.cc")
    set(_GRPC_H "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.grpc.pb.h")
    set(_GRPC_SRC "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.grpc.pb.cc")

    list(APPEND PROTO_HDRS ${_PROTO_H})
    list(APPEND PROTO_SRCS ${_PROTO_SRC})
    list(APPEND GRPC_HDRS ${_GRPC_H})
    list(APPEND GRPC_SRCS ${_GRPC_SRC})

    # 添加自定义命令：调用 protoc
    add_custom_command(
        OUTPUT "${_PROTO_H}" "${_PROTO_SRC}" "${_GRPC_H}" "${_GRPC_SRC}"
        # 【改进】直接使用 Target protobuf::protoc，规避繁琐且易错的路径变量
        COMMAND protobuf::protoc
        ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
             --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             --plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN_EXECUTABLE}
             -I ${CMAKE_CURRENT_SOURCE_DIR}
             ${ABS_FIL}
        DEPENDS ${ABS_FIL} protobuf::protoc gRPC::grpc_cpp_plugin
        COMMENT "Running gRPC C++ compiler on ${FIL_WE}.proto"
        VERBATIM
    )
endforeach()

# 【改进】显式告诉 CMake 这些文件是生成的，防止依赖检查时报错说“找不到源文件”
set_source_files_properties(${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS} PROPERTIES GENERATED TRUE)

# 6. 定义静态库目标
add_library(grpc_proto_lib STATIC ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS})

# 7. 链接依赖
target_link_libraries(grpc_proto_lib
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
        gRPC::grpc++_reflection  # 支持 gRPCurl 调试，非常有用
)

# 8. 导出头文件路径，使得外层模块可以直接 #include "raft.pb.h"
target_include_directories(grpc_proto_lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR})