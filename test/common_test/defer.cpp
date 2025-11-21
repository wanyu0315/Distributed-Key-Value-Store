#include "defer.h"
#include <iostream>
#include <string>

using namespace std;

void testFun1(const string& name) 
{ 
    cout << name; 
}
void testFun2(const string& name) 
{ 
    cout << name; 
}
void testFun3(const string& name) 
{ 
    cout << name; 
}


int main() 
{ 
    cout << "Testing DEFER macro:\n";
    DEFER { 
        testFun1("defer 1\n"); 
        testFun2("defer 2\n");
    };

    DEFER_VAR(d1)([&]() {
        testFun3("This d1 will be dismissed\n");
    });
    d1.dismiss();  // 取消执行
    
    DEFER_VAR(d2) ([&]() {
        testFun3("This d2 can be dismissed\n");
    });

    cout << "Main function is done\n";
    return 0; 
}