#ifndef UTIL_H
#define UTIL_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/access.hpp>
#include <condition_variable>  // pthread_condition_t
#include <functional>
#include <iostream>
#include <mutex>  // pthread_mutex_t
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>
#include "config.h"

#ifndef KVRAFTCPP_DEFER_H
#define KVRAFTCPP_DEFER_H
#include <functional>
#include <utility>

template <class F>
class DeferClass {
 public:
  DeferClass(F&& f) : m_func(std::forward<F>(f)), m_dismissed(false) {}  //增加dismissed成员变量，从而取消延迟执行
  DeferClass(const F& f) : m_func(f) {}
  ~DeferClass() noexcept{    
    if (!m_dismissed) {
      m_func(); 
    }
  }

// 取消延迟执行函数
  void dismiss() { m_dismissed = true; }

// 禁用拷贝
  DeferClass(const DeferClass& e) = delete;
  DeferClass& operator=(const DeferClass& e) = delete;

// 支持移动
  //移动构造函数（创建新对象，直接接管资源，原对象不再执行。）
  DeferClass(DeferClass&& other) noexcept 
    : m_func(std::move(other.m_func))
    , m_dismissed(other.m_dismissed) {
    other.m_dismissed = true;
  }

  //移动赋值运算符（将资源从一个对象转移到另一个已存在的对象。）
  DeferClass& operator=(DeferClass&& other) noexcept {
    if (this != &other) {
      if (!m_dismissed) {    //如果当前对象还未取消延迟执行，则先执行它自己的延迟操作，保证语义正确。
        try {
          m_func();
        } catch (...) {}
      }
      m_func = std::move(other.m_func);
      m_dismissed = other.m_dismissed;
      other.m_dismissed = true;
    }
    return *this;
  }

 private:
  F m_func;
  bool m_dismissed;
};

#define _CONCAT(a, b) a##b
#define _MAKE_DEFER_(line) DeferClass _CONCAT(defer, line) = [&]()  // 宏定义lambda表达式,固定使用引用捕获方式

#undef DEFER
#define DEFER _MAKE_DEFER_(__LINE__)  // 宏定义

// 新增有名变量的DEFER宏，方便调用dismiss方法，需要使用者设置变量的生命周期和捕获方式
#undef DEFER_VAR
#define DEFER_VAR(name) auto name = Defer  // 展开为：auto x = Defer

#endif  // KVRAFTCPP_DEFER_H

// 使用方法：
// {
//     `DEFER { ... }` 实际上是创建了一个匿名的 `DeferClass` 实例。括号里的代码会在该实例析构时执行。
// void example() {
//    基本使用：
//    DEFER { std::cout << "cleanup\n"; };

//    可取消的defer：
//    DEFER_VAR(d) [&]() { file.close(); };
//    
//    // ... do work ...
//    
//    if (some_condition) {
//       d.dismiss();       // 取消 defer
//    }
// }

// 改进内容：
// 1. 添加 noexcept 关键字,以确保在析构中不会抛出异常，防止程序异常终止。
// 2. 支持取消延迟执行：通过添加 `dismiss()` 方法，可以在需要时取消延迟执行的代码块。
// 3. 新增有名变量的DEFER宏，方便调用dismiss方法，此宏在创建实例时需要使用者设置变量的生命周期和捕获方式。
// 4. 支持移动语义：通过实现移动构造函数和移动赋值运算符，允许 `Defer` 对象被移动，而不会导致多次执行延迟代码块。


void DPrintf(const char* format, ...);

void myAssert(bool condition, std::string message = "Assertion failed!");

template <typename... Args>
std::string format(const char* format_str, Args... args) {
    int size_s = std::snprintf(nullptr, 0, format_str, args...) + 1; // "\0"
    if (size_s <= 0) { throw std::runtime_error("Error during formatting."); }
    auto size = static_cast<size_t>(size_s);
    std::vector<char> buf(size);
    std::snprintf(buf.data(), size, format_str, args...);
    return std::string(buf.data(), buf.data() + size - 1);  // remove '\0'
}

std::chrono::_V2::system_clock::time_point now();

std::chrono::milliseconds getRandomizedElectionTimeout(); //返回一个随机的选举超时时间
void sleepNMilliseconds(int N);

// ////////////////////////异步写日志的日志队列
// read is blocking!!! LIKE  go chan
template <typename T>
class LockQueue {
public:
    // 构造函数:支持设置最大容量
    explicit LockQueue(size_t max_capacity = 0) 
        : m_max_capacity(max_capacity), m_is_shutdown(false) {}
    
    // 禁止拷贝和赋值
    LockQueue(const LockQueue&) = delete;
    LockQueue& operator=(const LockQueue&) = delete;
    
    // 析构函数:确保优雅关闭
    ~LockQueue() {
        Shutdown();
    }

    // 1. 改进的Push - 支持移动语义和容量限制
    bool Push(T&& data) {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        // 等待队列有空间(如果设置了容量限制)
        if (m_max_capacity > 0) {
            m_not_full.wait(lock, [this] {
                return m_queue.size() < m_max_capacity || m_is_shutdown;
            });
        }
        
        //检测关闭状态
        if (m_is_shutdown) {
            return false;
        }
        
        m_queue.push(std::move(data));
        m_not_empty.notify_one();
        return true;
    }
    
    // 拷贝版本的Push(兼容性)
    bool Push(const T& data) {
        T temp(data);
        return Push(std::move(temp));
    }
    
    // 2. 超时Push - 避免生产者永久阻塞
    bool timeOutPush(T&& data, int timeout_ms) {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        if (m_max_capacity > 0) {
            auto timeout_time = std::chrono::steady_clock::now() + 
                               std::chrono::milliseconds(timeout_ms);
            
            if (!m_not_full.wait_until(lock, timeout_time, [this] {
                return m_queue.size() < m_max_capacity || m_is_shutdown;
            })) {
                return false;  // 超时
            }
        }
        
        if (m_is_shutdown) {
            return false;
        }
        
        m_queue.push(std::move(data));
        m_not_empty.notify_one();
        return true;
    }

    // 3. 批量Push - 减少锁竞争次数
    bool PushBatch(std::vector<T>&& items) {
        if (items.empty()) return true;
        
        std::unique_lock<std::mutex> lock(m_mutex);
        
        if (m_max_capacity > 0) {
            m_not_full.wait(lock, [this, &items] {
                return m_queue.size() + items.size() <= m_max_capacity || m_is_shutdown;
            });
        }
        
        if (m_is_shutdown) {
            return false;
        }
        
        for (auto& item : items) {
            m_queue.push(std::move(item));
        }
        
        // 如果有多个等待线程,唤醒多个
        if (items.size() > 1) {
            m_not_empty.notify_all();
        } else {
            m_not_empty.notify_one();
        }
        return true;
    }

    // 4. 改进的Pop - 使用输出参数避免拷贝,增加异常安全性
    bool Pop(T& out_data) {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        m_not_empty.wait(lock, [this] {
            return !m_queue.empty() || m_is_shutdown;
        });
        
        if (m_is_shutdown && m_queue.empty()) {
            return false;
        }
        
        // 使用移动语义,异常安全
        out_data = std::move(m_queue.front());   //把队首元素移动给输出参数out_data
        m_queue.pop();
        
        if (m_max_capacity > 0) {
            m_not_full.notify_one();
        }
        
        return true;
    }

    // 5. 改进的超时Pop
    bool timeOutPop(int timeout_ms, T& out_data) {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        // 获取当前时间点，并计算出超时时刻
        auto timeout_time = std::chrono::steady_clock::now() + 
                           std::chrono::milliseconds(timeout_ms);
        
        if (!m_not_empty.wait_until(lock, timeout_time, [this] {
            return !m_queue.empty() || m_is_shutdown;
        })) {
            return false;  // 超时
        }
        
        if (m_is_shutdown && m_queue.empty()) {
            return false;
        }
        
        out_data = std::move(m_queue.front());
        m_queue.pop();
        
        if (m_max_capacity > 0) {
            m_not_full.notify_one();
        }
        
        return true;
    }

    // 6. 批量Pop - 提高吞吐量
    size_t PopBatch(std::vector<T>& out_items, size_t max_count) {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        m_not_empty.wait(lock, [this] {
            return !m_queue.empty() || m_is_shutdown;
        });
        
        if (m_is_shutdown && m_queue.empty()) {
            return 0;
        }
        
        size_t count = 0;
        while (!m_queue.empty() && count < max_count) {
            out_items.push_back(std::move(m_queue.front()));
            m_queue.pop();
            ++count;
        }
        
        if (m_max_capacity > 0 && count > 0) {
            m_not_full.notify_all();
        }
        
        return count;
    }

    // 7. 非阻塞尝试Pop
    bool TryPop(T& out_data) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (m_queue.empty()) {
            return false;
        }
        
        out_data = std::move(m_queue.front());
        m_queue.pop();
        
        if (m_max_capacity > 0) {
            m_not_full.notify_one();
        }
        
        return true;
    }

    // 8. 状态查询接口
    size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }
    
    bool Empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }
    
    bool IsFull() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_max_capacity > 0 && m_queue.size() >= m_max_capacity;
    }

    // 9. 优雅关闭机制
    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_is_shutdown = true;
        }
        // 唤醒所有等待的线程
        m_not_empty.notify_all();
        m_not_full.notify_all();
    }
    //判断是否已关闭
    bool IsShutdown() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_is_shutdown;
    }

    // 10. 清空队列
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::queue<T> empty_queue;
        std::swap(m_queue, empty_queue);
        m_not_full.notify_all();
    }

private:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_not_empty;   // 队列非空条件变量
    std::condition_variable m_not_full;    // 队列未满条件变量
    size_t m_max_capacity;                  // 最大容量(0表示无限制)
    bool m_is_shutdown;                     // 关闭标志
};
// 两个对锁的管理用到了RAII的思想，防止中途出现问题而导致资源无法释放的问题！！！
// std::lock_guard 和 std::unique_lock 都是 C++11 中用来管理互斥锁的工具类，它们都封装了 RAII（Resource Acquisition Is
// Initialization）技术，使得互斥锁在需要时自动加锁，在不需要时自动解锁，从而避免了很多手动加锁和解锁的繁琐操作。
// std::lock_guard 是一个模板类，它的模板参数是一个互斥量类型。当创建一个 std::lock_guard
// 对象时，它会自动地对传入的互斥量进行加锁操作，并在该对象被销毁时对互斥量进行自动解锁操作。std::lock_guard
// 不能手动释放锁，因为其所提供的锁的生命周期与其绑定对象的生命周期一致。 std::unique_lock
// 也是一个模板类，同样的，其模板参数也是互斥量类型。不同的是，std::unique_lock 提供了更灵活的锁管理功能。可以通过
// lock()、unlock()、try_lock() 等方法手动控制锁的状态。当然，std::unique_lock 也支持 RAII
// 技术，即在对象被销毁时会自动解锁。另外， std::unique_lock 还支持超时等待和可中断等待的操作。

// 这个Op是kv传递给raft的command
class Op {
 public:
  // Your definitions here.
  // Field names must start with capital letters,
  // otherwise RPC will break.
  std::string Operation;  // "Get" "Put" "Append"
  std::string Key;
  std::string Value;
  std::string ClientId;  //客户端号码
  int RequestId;         //客户端号码请求的Request的序列号，为了保证线性一致性
                         // IfDuplicate bool // Duplicate command can't be applied twice , but only for PUT and APPEND

 public:
  // todo
  //为了协调raftRPC中的command只设置成了string,这个的限制就是正常字符中不能包含|
  //当然后期可以换成更高级的序列化方法，比如protobuf

  std::string asString() const {      // Op 对象序列化为 std::string（用于 RPC 传输或写入日志）。
    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);

    // write class instance to archive
    oa << *this;
    // close archive

    return ss.str();
  }

  // 反序列化（添加try...catch 块，防止崩溃）
  bool parseFromString(std::string str) {
    try {
        std::stringstream iss(str);
        boost::archive::text_iarchive ia(iss);
        // read class state from archive
        ia >> *this;
        return true; 
    } catch (const std::exception& e) {
        // 当 str 格式错误时，Boost.Serialization 会抛出异常
        // 我们捕获它，并返回 false，而不是让程序崩溃
        // 你可以在这里用 DPrintf 打印日志
        // DPrintf("Op::parseFromString failed: %s", e.what());
        return false;
    }
}

 public:
  friend std::ostream& operator<<(std::ostream& os, const Op& obj) {
    os << "[MyClass:Operation{" + obj.Operation + "},Key{" + obj.Key + "},Value{" + obj.Value + "},ClientId{" +
              obj.ClientId + "},RequestId{" + std::to_string(obj.RequestId) + "}";  // 在这里实现自定义的输出格式
    return os;
  }

 private:
  friend class boost::serialization::access;
  template <class Archive>
  void serialize(Archive& ar, const unsigned int version) {
    ar& Operation;
    ar& Key;
    ar& Value;
    ar& ClientId;
    ar& RequestId;
  }
};

///////////////////////////////////////////////kvserver reply err to clerk

const std::string OK = "OK";
const std::string ErrNoKey = "ErrNoKey";
const std::string ErrWrongLeader = "ErrWrongLeader";

////////////////////////////////////获取可用端口

bool isReleasePort(unsigned short usPort);

bool getReleasePort(short& port);

// int main(int argc, char** argv)
// {
//     short port = 9060;
//     if(getReleasePort(port)) //在port的基础上获取一个可用的port
//     {
//         std::cout << "可用的端口号为：" << port << std::endl;
//     }
//     else
//     {
//         std::cout << "获取可用端口号失败！" << std::endl;
//     }
//     return 0;
// }

#endif  //  UTIL_H