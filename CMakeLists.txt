# MyDistributedStore/CMakeLists.txt (根文件)

# 1. CMake 最低版本要求
cmake_minimum_required(VERSION 3.16)

# 2. 定义项目名称
project(MyDistributedStore CXX)

# 3. 设置C++标准 (例如 C++17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. 告诉CMake进入 "src" 目录
add_subdirectory(src)

# 5. 告诉 CMake 加载我们的测试程序 (假设 'test' 目录在根目录)
#    (如果 'test' 目录在 'src' 下面，请改为 add_subdirectory(src/test) )
add_subdirectory(test)