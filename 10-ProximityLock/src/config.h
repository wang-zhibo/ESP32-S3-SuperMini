#pragma once
#include <Arduino.h>

// ==========================================
// ESP32-S3-SuperMini 硬件引脚分配
// ==========================================
#define PIN_LED              48    // 板载用户状态指示灯 (GPIO 48)
#define PIN_BOOT             0     // 板载 BOOT 按键 (GPIO 0, 低电平有效)
#define PIN_TOUCH_OS         1     // 裸铜触摸引脚 1: 切换操作系统 (Win/Mac/Linux)
#define PIN_TOUCH_PAUSE      2     // 裸铜触摸引脚 2: 开会/摸鱼临时暂停锁定 (15分钟)

// ==========================================
// 默认工作参数与距离阈值
// ==========================================
#define DEFAULT_LOCK_RSSI         -82   // dBm: 当滤波后 RSSI 低于此值，判定为远离离开
#define DEFAULT_UNLOCK_RSSI       -70   // dBm: 当滤波后 RSSI 高于此值，判定为回到工位
#define DEFAULT_LEAVE_TIMEOUT_MS  4000  // 毫秒: 连续无信号或低信号超过此时长触发锁屏
#define DEFAULT_PAUSE_DURATION_MS (15 * 60 * 1000) // 15分钟临时免打扰挂起

#define TOUCH_SENSITIVITY_DELTA   12    // 电容触控相对基准的变化敏感度阈值
#define BLE_SCAN_INTERVAL_MS      100   // BLE 扫描间隔
#define BLE_SCAN_WINDOW_MS        99    // BLE 扫描窗口 (近乎 100% 占空比持续捕获)

// ==========================================
// 支持的操作系统目标
// ==========================================
enum OperatingSystem : uint8_t {
    OS_WINDOWS = 0,
    OS_MACOS   = 1,
    OS_LINUX   = 2
};

// ==========================================
// 运行状态机枚举
// ==========================================
enum ProximityState : uint8_t {
    STATE_UNPAIRED = 0,   // 未绑定设备，等待配对
    STATE_PAIRING  = 1,   // 配对模式中（搜寻贴近的设备）
    STATE_PRESENT  = 2,   // 正常在场（已解锁/工作中）
    STATE_LEAVING  = 3,   // 信号变弱/倒计时中
    STATE_LOCKED   = 4,   // 远离工位，已执行物理锁屏
    STATE_PAUSED   = 5    // 临时暂停锁定（会议/演示模式）
};
