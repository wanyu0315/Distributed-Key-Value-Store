#ifndef __MONSOON_UTIL_H__
#define __MONSOON_UTIL_H__

#include <assert.h>
#include <cxxabi.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

namespace monsoon {

/**
 * @brief 获取当前线程的真实物理 ID (TID)
 * * 场景： 用于 Thread-per-Core 架构中识别具体运行在哪个 LWP (Light Weight Process) 上。
 * * 细节： 
 * 1. 封装了系统调用 SYS_gettid。
 * 2. 使用 inline 减少函数调用开销。
 */
inline pid_t GetThreadId() {
    return syscall(SYS_gettid);
}

/**
 * @brief 获取当前协程 ID
 * * 注意： 此处仅声明。具体实现需在 fiber.cpp 中，
 * 防止 util.h 与 fiber.h 产生循环依赖。
 */
uint64_t GetFiberId();

/**
 * @brief 获取系统启动至今的毫秒数
 * * 场景： 用于计算 Raft 心跳超时、选举超时等单调时间。
 * * 细节： 使用 CLOCK_MONOTONIC_RAW，不受系统时间修改（NTP 对时）影响。
 */
inline uint64_t GetElapsedMS() {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * @brief C++ 符号解码 (Demangle)
 * @param str 原始符号名 (Mangled Name)
 * * 细节： 
 * 1. 将编译器生成的 _ZN7monsoon6Thread3runEPv 这种符号
 * 解析为 monsoon::Thread::run(void*) 这种可读形式。
 * 2. 改进了原有的 sscanf 解析逻辑，使用 string 操作更健壮。
 */
inline std::string demangle(const char *str) {
    size_t size = 0;
    int status = 0;
    std::string rt(str);
    
    // backtrace_symbols 返回的格式通常是: ./path/to/bin(mangled_name+offset) [address]
    // 我们需要提取括号内的 mangled_name
    
    size_t left_paren = rt.find('(');
    size_t plus_sign = rt.find('+');

    if (left_paren != std::string::npos && plus_sign != std::string::npos && left_paren < plus_sign) {
        std::string mangled_name = rt.substr(left_paren + 1, plus_sign - left_paren - 1);
        
        // 调用 abi::__cxa_demangle 进行解码
        char *v = abi::__cxa_demangle(mangled_name.c_str(), nullptr, &size, &status);
        if (status == 0 && v) {
            std::string result(v);
            free(v);
            // 重新拼接：路径(解码后的函数名+偏移) [地址]
            return rt.substr(0, left_paren + 1) + result + rt.substr(plus_sign);
        }
    }
    
    // 如果解析失败，返回原始字符串
    return rt;
}

/**
 * @brief 获取当前调用栈信息
 * @param bt 输出参数，存储栈帧字符串
 * @param size 回溯深度
 * @param skip 跳过的栈帧数 (通常跳过 Backtrace 本身)
 * * 场景： Panic 或 Error 时打印堆栈。
 */
inline void Backtrace(std::vector<std::string> &bt, int size = 64, int skip = 1) {
    // 分配用于存储调用栈信息的数组
    void **array = (void **)malloc((sizeof(void *) * size));
    // 获取栈深度
    size_t s = ::backtrace(array, size);

    // 获取符号信息 (malloc inside)
    char **strings = backtrace_symbols(array, s);
    if (strings == NULL) {
        std::cerr << "backtrace_symbols error" << std::endl;
        free(array);
        return;
    }

    // 解析并存入 vector
    for (size_t i = skip; i < s; ++i) {
        bt.push_back(demangle(strings[i]));
    }

    free(strings);
    free(array);
}

/**
 * @brief 获取调用栈的字符串形式
 * @param size 回溯深度
 * @param skip 跳过层数
 * @param prefix 每行前缀 (通常用于日志格式化)
 */
inline std::string BacktraceToString(int size = 64, int skip = 2, const std::string &prefix = "") {
    std::vector<std::string> bt;
    Backtrace(bt, size, skip);
    std::stringstream ss;
    for (size_t i = 0; i < bt.size(); ++i) {
        ss << prefix << bt[i] << std::endl;
    }
    return ss.str();
}

/**
 * @brief 带堆栈信息的断言 (Conditional Panic)
 * @param condition 断言条件 (为 false 时触发)
 * @param err 错误描述信息
 * * 场景： 核心逻辑 (如 Raft 状态机) 出现不可恢复错误时，打印堆栈并终止进程。
 * * 细节： 使用 std::cerr 无缓冲输出，防止进程崩溃时日志还在缓冲区丢失。
 */
inline void CondPanic(bool condition, std::string err) {
    if (!condition) {
        std::cerr << "================= PANIC =================" << std::endl;
        std::cerr << "[Assertion Failed] " << err << std::endl;
        std::cerr << "[Location] " << __FILE__ << ":" << __LINE__ << std::endl;
        std::cerr << "[Backtrace]:\n" << BacktraceToString(64, 2, "    ") << std::endl;
        std::cerr << "=========================================" << std::endl;
        assert(condition);
    }
}

}  // namespace monsoon

#endif