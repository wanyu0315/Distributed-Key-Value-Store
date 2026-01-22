# MyDistributedStore/CMakeLists.txt (根文件)

# 1. CMake 最低版本要求
cmake_minimum_required(VERSION 3.16)

# 2. 定义项目名称
project(MyDistributedStore CXX)

# 3. 启用测试功能
enable_testing()

# 4. 设置C++标准 (例如 C++17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 5. 查找外部库或包
find_package(Boost REQUIRED COMPONENTS serialization system)  #  使用 Boost 库的序列化和系统组件
find_package(Threads REQUIRED) # 查找线程库 (pthread)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
# gRPC 通常会自动拉取 Abseil，但有些环境需要显式查找
find_package(absl CONFIG REQUIRED)

# 6. 告诉CMake进入各个子目录
add_subdirectory(proto)
add_subdirectory(src)
add_subdirectory(test)  # (如果 'test' 目录在 'src' 下面，请改为 add_subdirectory(src/test) )




