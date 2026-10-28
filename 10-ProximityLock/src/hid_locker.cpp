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
static SemaphoreHandle_t s_usbMutex = NULL;
static volatile bool s_isUnlocking = false;
static OperatingSystem s_targetOS = OS_WINDOWS;
static String s_targetPassword = "";

static bool takeUsb(TickType_t waitTicks = pdMS_TO_TICKS(200)) {
    if (s_usbMutex) {
        return (xSemaphoreTake(s_usbMutex, waitTicks) == pdTRUE);
    }
    return true;
}

static void giveUsb() {
    if (s_usbMutex) {
        xSemaphoreGive(s_usbMutex);
    }
}

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

        if (takeUsb(pdMS_TO_TICKS(500))) {
            Keyboard.releaseAll();
            giveUsb();
        }

        // ==============================================================
        // 核心优化阶段 1: USB 总线唤醒与显示器冷唤醒握手 (解决长时间休眠黑屏盲打)
        // ==============================================================
        // 现代显示器通过 HDMI / DisplayPort 熄屏休眠后，从 D3 到 D0 点亮背光需 2.0 ~ 3.5 秒
        // 先发送一次轻点 Shift 击键作为 USB Remote Wakeup 与点屏信号
        if (takeUsb()) {
            Keyboard.press(KEY_LEFT_SHIFT);
            vTaskDelay(pdMS_TO_TICKS(50));
            Keyboard.releaseAll();
            giveUsb();
        }

        // 关键硬件冷启动延时：给显示器与显卡充分的 EDID / DP Link Training 握手点亮时间
        vTaskDelay(pdMS_TO_TICKS(2000));

        // 第二次发送轻点 Shift，确保总线活跃与操作系统彻底就绪
        if (takeUsb()) {
            Keyboard.press(KEY_LEFT_SHIFT);
            vTaskDelay(pdMS_TO_TICKS(50));
            Keyboard.releaseAll();
            giveUsb();
        }
        vTaskDelay(pdMS_TO_TICKS(350));

        // ==============================================================
        // 核心优化阶段 2: 唤醒锁屏并滑出凭据输入框 (Dismiss Lock Screen)
        // ==============================================================
        // 发送 Space 敲起锁屏壁纸
        if (takeUsb()) {
            Keyboard.press(' ');
            vTaskDelay(pdMS_TO_TICKS(60));
            Keyboard.releaseAll();
            giveUsb();
        }

        // 等待 Windows / Linux / macOS 锁屏壁纸上滑动画结束、密码框完全加载并获取焦点
        vTaskDelay(pdMS_TO_TICKS(800));

        if (pwdCopy.length() == 0) {
            // 无密码模式：直接回车进入桌面
            if (takeUsb()) {
                Keyboard.press(KEY_RETURN);
                vTaskDelay(pdMS_TO_TICKS(60));
                Keyboard.releaseAll();
                giveUsb();
            }
        } else {
            // ==============================================================
            // 核心优化阶段 3: 密码输入框脏数据强力清洗 (彻底清空残留的单个 'l' 或历史误触)
            // ==============================================================
            // 连发 6 次 Backspace，安全抹除一切残留字符且绝不触发任何误操作快捷键
            if (takeUsb()) {
                for (int b = 0; b < 6; b++) {
                    Keyboard.press(KEY_BACKSPACE);
                    vTaskDelay(pdMS_TO_TICKS(25));
                    Keyboard.release(KEY_BACKSPACE);
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                Keyboard.releaseAll();
                giveUsb();
            }
            vTaskDelay(pdMS_TO_TICKS(100));

            // ==============================================================
            // 核心优化阶段 4: 逐字符平滑键入密码并回车提交
            // ==============================================================
            if (takeUsb(pdMS_TO_TICKS(500))) {
                for (size_t i = 0; i < pwdCopy.length(); i++) {
                    Keyboard.write(pwdCopy[i]);
                    vTaskDelay(pdMS_TO_TICKS(30)); // 30ms 击键间隔确保各种操作系统按键缓冲区平滑接收，绝不丢键
                }
                vTaskDelay(pdMS_TO_TICKS(80));
                Keyboard.press(KEY_RETURN);
                vTaskDelay(pdMS_TO_TICKS(60));
                Keyboard.releaseAll();
                giveUsb();
            }
        }

        if (takeUsb()) {
            Keyboard.releaseAll();
            giveUsb();
        }
        s_isUnlocking = false;
    }
}
#endif

HIDLocker Locker;

HIDLocker::HIDLocker() {}

void HIDLocker::begin() {
#if ARDUINO_USB_MODE == 0
    Serial.println("✅ [HID] TinyUSB 物理键盘总线初始化...");
    if (!s_usbMutex) {
        s_usbMutex = xSemaphoreCreateMutex();
    }
    if (!s_taskMutex) {
        s_taskMutex = xSemaphoreCreateMutex();
    }
    Keyboard.begin();
    Keyboard.releaseAll(); // 启动时强制释放全部按键，防暖重启卡键
    USB.begin();

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
    if (takeUsb()) {
        Keyboard.releaseAll();
        giveUsb();
    }
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
    // 关键安全互斥：如果后台正在执行开屏/打密码，绝对禁止触发锁屏快捷键！
    // 杜绝锁屏按键 (如 Win+L) 被错误打入已激活的密码输入框
    if (s_isUnlocking) {
        Serial.println("⚠️ [HID] 正在执行开屏解锁，已拦截并丢弃锁屏快捷键调用.");
        return;
    }

    if (!takeUsb(pdMS_TO_TICKS(200))) {
        Serial.println("⚠️ [HID] 获取 USB 互斥锁超时，跳过本次锁屏快捷键.");
        return;
    }

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
    Keyboard.releaseAll();
    giveUsb();
#endif
}

void HIDLocker::wakeScreen() {
#if ARDUINO_USB_MODE == 0
    if (s_isUnlocking) return;
    if (takeUsb()) {
        // 发送一次无害的 Left Shift 击键以唤醒休眠的屏幕
        Keyboard.press(KEY_LEFT_SHIFT);
        delay(40);
        Keyboard.release(KEY_LEFT_SHIFT);
        giveUsb();
    }
#endif
}

void HIDLocker::typePassword(const String& password) {
#if ARDUINO_USB_MODE == 0
    if (takeUsb(pdMS_TO_TICKS(500))) {
        for (size_t i = 0; i < password.length(); i++) {
            Keyboard.write(password[i]);
            delay(30); // 30ms 击键间隔确保各种操作系统按键缓冲区平滑接收，绝不丢键
        }
        giveUsb();
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
