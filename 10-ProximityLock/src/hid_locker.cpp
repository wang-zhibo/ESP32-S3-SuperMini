#include "hid_locker.h"
#include "USB.h"
#include "USBHIDKeyboard.h"

#if ARDUINO_USB_MODE == 0
static USBHIDKeyboard Keyboard;
#endif
HIDLocker Locker;

HIDLocker::HIDLocker() {}

void HIDLocker::begin() {
#if ARDUINO_USB_MODE == 0
    Keyboard.begin();
    USB.begin();
#else
    Serial.println("⚠️ [HID] 提示: 当前编译选项未启用 TinyUSB (ARDUINO_USB_MODE != 0)。若需物理锁屏按键，请在 IDE 菜单将 USB Mode 选为 'USB-OTG (TinyUSB)'。");
#endif
}

void HIDLocker::lock(OperatingSystem os) {
#if ARDUINO_USB_MODE == 0
    switch (os) {
        case OS_WINDOWS:
            // Windows: Win + L
            Keyboard.press(KEY_LEFT_GUI);
            Keyboard.press('l');
            delay(60);
            Keyboard.releaseAll();
            break;
            
        case OS_MACOS:
            // macOS: Cmd + Ctrl + Q
            Keyboard.press(KEY_LEFT_GUI);
            Keyboard.press(KEY_LEFT_CTRL);
            Keyboard.press('q');
            delay(60);
            Keyboard.releaseAll();
            break;
            
        case OS_LINUX:
            // Linux: 先发 Super+L (Ubuntu GNOME / KDE 默认快捷键)，再发 Ctrl+Alt+L (传统桌面兼容)
            Keyboard.press(KEY_LEFT_GUI);
            Keyboard.press('l');
            delay(50);
            Keyboard.releaseAll();
            delay(50);
            Keyboard.press(KEY_LEFT_CTRL);
            Keyboard.press(KEY_LEFT_ALT);
            Keyboard.press('l');
            delay(50);
            Keyboard.releaseAll();
            break;
    }
#endif
}

void HIDLocker::wakeScreen() {
#if ARDUINO_USB_MODE == 0
    // 发送一次无害的 Left Shift 击键以唤醒休眠的屏幕
    Keyboard.press(KEY_LEFT_SHIFT);
    delay(40);
    Keyboard.release(KEY_LEFT_SHIFT);
#endif
}

void HIDLocker::typePassword(const String& password) {
#if ARDUINO_USB_MODE == 0
    for (size_t i = 0; i < password.length(); i++) {
        Keyboard.write(password[i]);
        delay(25); // 25ms 击键间隔确保各种操作系统按键缓冲区平滑接收，绝不丢键
    }
#endif
}

void HIDLocker::unlock(OperatingSystem os, const String& password) {
#if ARDUINO_USB_MODE == 0
    if (password.length() == 0) {
        // 无密码模式：
        // 1. 发送无害 Left Shift 唤醒显示器
        Keyboard.press(KEY_LEFT_SHIFT);
        delay(40);
        Keyboard.release(KEY_LEFT_SHIFT);
        delay(350);

        // 2. 发送空格与回车滑起锁屏壁纸，直接进入桌面
        Keyboard.press(' ');
        delay(50);
        Keyboard.releaseAll();
        delay(200);
        Keyboard.press(KEY_RETURN);
        delay(50);
        Keyboard.releaseAll();
    } else {
        // 有密码模式：
        // 1. 发送无害 Shift 点亮休眠屏幕
        Keyboard.press(KEY_LEFT_SHIFT);
        delay(40);
        Keyboard.release(KEY_LEFT_SHIFT);
        delay(350);

        // 2. 发送空格键滑起锁屏壁纸（Windows/GNOME 遮罩需要一次击键唤醒输入框）
        Keyboard.press(' ');
        delay(50);
        Keyboard.releaseAll();

        // 3. 关键硬件延时：等待显示器亮屏且密码输入框获得焦点 (通常需 700~900ms)
        delay(800);

        // 4. 逐字符平滑输入密码
        typePassword(password);
        delay(60);

        // 5. 敲击回车确认登录
        Keyboard.press(KEY_RETURN);
        delay(50);
        Keyboard.releaseAll();
    }
#endif
}

void HIDLocker::testCurrent(OperatingSystem os) {
    lock(os);
}
