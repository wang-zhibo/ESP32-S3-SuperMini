#pragma once
#include <Arduino.h>
#include "config.h"

class HIDLocker {
public:
    HIDLocker();
    void begin();
    
    // 执行锁屏
    void lock(OperatingSystem os);
    
    // 唤醒屏幕并自动解锁（若设置了密码则输入密码+回车，若无密码则仅唤醒并确认）
    void unlock(OperatingSystem os, const String& password);

    // 唤醒屏幕（轻点无害键）
    void wakeScreen();

    // 模拟键盘平滑输入密码 (逐字符延时，防操作系统丢失按键)
    void typePassword(const String& password);

    // 发送测试组合键
    void testCurrent(OperatingSystem os);

    // 检查当前固件是否成功启用了 TinyUSB 键盘驱动 (ARDUINO_USB_MODE == 0)
    bool isTinyUSBEnabled() const;

    // 强制释放所有物理按键状态 (防卡键保底)
    void releaseAll();

    // 检查是否正在后台异步执行开屏与密码输入
    bool isUnlocking() const;
};

extern HIDLocker Locker;
