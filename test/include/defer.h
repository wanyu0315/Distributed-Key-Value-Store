#ifndef KVRAFTCPP_DEFER_H
#define KVRAFTCPP_DEFER_H
#include <functional>
#include <utility>

//此处完全移植common/include/util.h中的DeferClass

template <class F>
class Defer {
 public:
  Defer(F&& f) : m_func(std::forward<F>(f)), m_dismissed(false) {}  //增加dismissed成员变量，从而取消延迟执行
  Defer(const F& f) : m_func(f) {}
  ~Defer() noexcept{    
    if (!m_dismissed) {
      m_func(); 
    }
  }

// 取消延迟执行函数
  void dismiss() { m_dismissed = true; }

// 禁用拷贝
  Defer(const Defer& e) = delete;
  Defer& operator=(const Defer& e) = delete;

// 支持移动
  //移动构造函数（创建新对象，直接接管资源，原对象不再执行。）
  Defer(Defer&& other) noexcept 
    : m_func(std::move(other.m_func))
    , m_dismissed(other.m_dismissed) {
    other.m_dismissed = true;
  }

  //移动赋值运算符（将资源从一个对象转移到另一个已存在的对象。）
  Defer& operator=(Defer&& other) noexcept {
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
#define _MAKE_DEFER_(line) Defer _CONCAT(defer, line) = [&]()  // 宏定义lambda表达式

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