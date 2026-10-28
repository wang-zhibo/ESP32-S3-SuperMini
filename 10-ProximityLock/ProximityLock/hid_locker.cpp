#include "hid_locker.h"
#include "USB.h"
#include "USBHIDKeyboard.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#if ARDUINO_USB_MODE == 0
static USBHIDKeyboard Keyboard;
static TaskHandle_t s_unlockTaskHandle = NULL;
static SemaphoreHandle_t s_taskMutex = NULL;
static volatile bool s_isUnlocking = false;
static OperatingSystem s_targetOS = OS_WINDOWS;
static String s_targetPassword = "";

static void unlockWorkerTask(void* param) {
    while (true) {
        // 等待解锁唤醒事件通知
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        s_isUnlocking = true;
        String pwdCopy = "";
        OperatingSystem osCopy = OS_WINDOWS;
        if (s_taskMutex && xSemaphoreTake(s_taskMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            pwdCopy = s_targetPassword;
            osCopy = s_targetOS;
            xSemaphoreGive(s_taskMutex);
        }

        Keyboard.releaseAll();
        if (pwdCopy.length() == 0) {
            // 无密码模式：轻点 Shift 唤醒 -> 等待亮屏 -> 空格与回车直接进入
            Keyboard.press(KEY_LEFT_SHIFT);
            vTaskDelay(pdMS_TO_TICKS(40));
            Keyboard.release(KEY_LEFT_SHIFT);
            vTaskDelay(pdMS_TO_TICKS(350));

            Keyboard.press(' ');
            vTaskDelay(pdMS_TO_TICKS(50));
            Keyboard.releaseAll();
            vTaskDelay(pdMS_TO_TICKS(200));
            Keyboard.press(KEY_RETURN);
            vTaskDelay(pdMS_TO_TICKS(50));
            Keyboard.releaseAll();
        } else {
            // 有密码模式：轻点 Shift 唤醒 -> 空格滑出输入框 -> 等待焦点 -> 平滑输入密码 -> 回车确认
            Keyboard.press(KEY_LEFT_SHIFT);
            vTaskDelay(pdMS_TO_TICKS(40));
            Keyboard.release(KEY_LEFT_SHIFT);
            vTaskDelay(pdMS_TO_TICKS(350));

            Keyboard.press(' ');
            vTaskDelay(pdMS_TO_TICKS(50));
            Keyboard.releaseAll();

            vTaskDelay(pdMS_TO_TICKS(800)); // 关键硬件延时：等待输入框获取聚焦

            for (size_t i = 0; i < pwdCopy.length(); i++) {
                Keyboard.write(pwdCopy[i]);
                vTaskDelay(pdMS_TO_TICKS(25)); // 25ms 击键间隔确保各种操作系统按键缓冲区平滑接收
            }
            vTaskDelay(pdMS_TO_TICKS(60));

            Keyboard.press(KEY_RETURN);
            vTaskDelay(pdMS_TO_TICKS(50));
            Keyboard.releaseAll();
        }
        Keyboard.releaseAll();
        s_isUnlocking = false;
    }
}
#endif

HIDLocker Locker;

HIDLocker::HIDLocker() {}

void HIDLocker::begin() {
#if ARDUINO_USB_MODE == 0
    Serial.println("✅ [HID] TinyUSB 物理键盘总线初始化...");
    Keyboard.begin();
    Keyboard.releaseAll(); // 启动时强制释放全部按键，防暖重启卡键
    USB.begin();

    if (!s_taskMutex) {
        s_taskMutex = xSemaphoreCreateMutex();
    }
    if (!s_unlockTaskHandle) {
        // 创建后台独立 Worker 任务执行屏幕唤醒与密码输入，主循环零等待
        xTaskCreatePinnedToCore(unlockWorkerTask, "unlockWorker", 4096, NULL, 1, &s_unlockTaskHandle, 0);
    }
#else
    Serial.println("⚠️ [HID] 警告: 当前编译选项未启用 TinyUSB (ARDUINO_USB_MODE != 0)。若需物理锁屏按键，请在 IDE 菜单将 USB Mode 选为 'USB-OTG (TinyUSB)'。");
#endif
}

void HIDLocker::releaseAll() {
#if ARDUINO_USB_MODE == 0
    Keyboard.releaseAll();
#endif
}

bool HIDLocker::isTinyUSBEnabled() const {
#if ARDUINO_USB_MODE == 0
    return true;
#else
    return false;
#endif
}

void HIDLocker::lock(OperatingSystem os) {
#if ARDUINO_USB_MODE == 0
    Keyboard.releaseAll(); // 操作前先清零按键状态
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
    if (s_isUnlocking) return; // 正在异步解锁中，避免并发重复触发
    
    if (s_taskMutex && xSemaphoreTake(s_taskMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_targetOS = os;
        s_targetPassword = password;
        xSemaphoreGive(s_taskMutex);
    }
    if (s_unlockTaskHandle) {
        xTaskNotifyGive(s_unlockTaskHandle);
    }
#endif
}

bool HIDLocker::isUnlocking() const {
#if ARDUINO_USB_MODE == 0
    return s_isUnlocking;
#else
    return false;
#endif
}

void HIDLocker::testCurrent(OperatingSystem os) {
    lock(os);
}
