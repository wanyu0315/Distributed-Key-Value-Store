#include "monsoon.h" // 包含你所有的头文件

void test_sleep() {
    monsoon::IOManager iom(1, false, "test_sleep"); // 1个线程

    // 任务 A：睡 2 秒
    iom.schedule([](){
        monsoon::IOManager::GetThis()->addTimer(2000, [](){
             std::cout << "Timer 2s triggered (via Timer)" << std::endl;
        });
        
        std::cout << "Task A: Begin sleep(3)" << std::endl;
        sleep(3); // 【关键】如果 Hook 成功，这里会 yield，线程去跑 Task B
        std::cout << "Task A: Wake up after 3s" << std::endl;
    });

    // 任务 B：立即执行
    iom.schedule([](){
        std::cout << "Task B: Running immediately (Hook works!)" << std::endl;
    });
}

int main() {
    test_sleep();
    return 0;
}