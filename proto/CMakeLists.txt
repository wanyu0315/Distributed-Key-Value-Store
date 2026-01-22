# proto/CMakeLists.txt

# 1. 确保能找到 gRPC 和 Protobuf (虽然根目录找过了，为了保险和独立性，这里可以使用变量)
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf REQUIRED)

# 2. 获取 gRPC C++ 插件的路径 (用于生成 Service 代码)
get_target_property(gRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin LOCATION)

# 3. 查找所有 .proto 文件
file(GLOB PROTO_FILES "*.proto")

# 4. 定义生成文件的存放列表
set(PROTO_HDRS)
set(PROTO_SRCS)
set(GRPC_HDRS)
set(GRPC_SRCS)

# 5. 遍历每个 .proto 文件，生成 protobuf 代码 和 gRPC 代码
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
      COMMAND ${Protobuf_PROTOC_EXECUTABLE}
      ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
           --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
           --plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN_EXECUTABLE}
           -I ${CMAKE_CURRENT_SOURCE_DIR}
           ${ABS_FIL}
      DEPENDS ${ABS_FIL} gRPC::grpc_cpp_plugin
      COMMENT "Running gRPC C++ compiler on ${FIL}"
      VERBATIM)
endforeach()

# 6. 定义库目标 (包含 pb.cc 和 grpc.pb.cc)
add_library(grpc_proto_lib ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS})

# 7. 链接依赖 (这是重点修改！)
# 以前只链接 protobuf，现在必须链接 gRPC 库
target_link_libraries(grpc_proto_lib
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
        gRPC::grpc++_reflection  # (可选) 支持反射，调试方便
)

# 8. 导出头文件路径
target_include_directories(grpc_proto_lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR})