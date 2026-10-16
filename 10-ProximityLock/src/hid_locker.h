#pragma once
#include <Arduino.h>
#include "config.h"

class HIDLocker {
public:
    HIDLocker();
    void begin();
    
    // 执行锁屏
    void lock(OperatingSystem os);
    
    // 唤醒屏幕（轻点无害键）
    void wakeScreen();

    // 发送测试组合键
    void testCurrent(OperatingSystem os);
};

extern HIDLocker Locker;
