#include "hid_locker.h"
#include "USB.h"
#include "USBHIDKeyboard.h"

static USBHIDKeyboard Keyboard;
HIDLocker Locker;

HIDLocker::HIDLocker() {}

void HIDLocker::begin() {
    Keyboard.begin();
    USB.begin();
}

void HIDLocker::lock(OperatingSystem os) {
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
            // Linux: Ctrl + Alt + L
            Keyboard.press(KEY_LEFT_CTRL);
            Keyboard.press(KEY_LEFT_ALT);
            Keyboard.press('l');
            delay(60);
            Keyboard.releaseAll();
            break;
    }
}

void HIDLocker::wakeScreen() {
    // 发送一次无害的 Left Shift 击键以唤醒休眠的屏幕
    Keyboard.press(KEY_LEFT_SHIFT);
    delay(40);
    Keyboard.release(KEY_LEFT_SHIFT);
}

void HIDLocker::testCurrent(OperatingSystem os) {
    lock(os);
}
