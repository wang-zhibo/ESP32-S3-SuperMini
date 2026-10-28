#pragma once
#include <Arduino.h>
#include "version.h"

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
#define DEFAULT_LEAVE_TIMEOUT_MS  6000  // 毫秒: 连续无信号或低信号超过此时长触发锁屏 (约2个3秒周期)
#define DEFAULT_PAUSE_DURATION_MS (15 * 60 * 1000) // 15分钟临时免打扰挂起

// 周边扫描列表范围限制：仅收录约 1 米以内设备 (RSSI >= -68 dBm)，超过 1 米的远距离设备一律不扫描收录
#define BLE_DISCOVERY_RSSI_THRESHOLD -68

#define ENABLE_TOUCH_PINS         0     // 裸引脚电容触摸开关: 0=关闭(默认, 防止裸板悬空引脚杂讯误触导致循环切换/重启), 1=开启
#define TOUCH_SENSITIVITY_DELTA   2000  // ESP32-S3 电容触控基准变化敏感度 (S3基数在数万，需设为数千)

// Wi-Fi 与 BLE 共存射频优化调度：低发热省电调度 (每 2.5 秒扫描一次，扫描窗口 900ms，占空比 36%，抓包率提升 35%)
#define BLE_SCAN_INTERVAL_MS      2500  // BLE 扫描间隔: 2500 毫秒
#define BLE_SCAN_WINDOW_MS        900   // BLE 扫描窗口: 900 毫秒 (捕获率高且发热极低)

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

// 全局状态与配置共享声明
extern OperatingSystem  g_osMode;
extern ProximityState   g_state;
extern int              g_lockRSSI;
extern int              g_unlockRSSI;
extern uint32_t         g_leaveTimeoutMs;
extern uint32_t         g_pauseStartMs;
extern String           g_unlockPassword;
extern bool             g_autoUnlockEnabled;

void saveRuntimeSettings(int lockRssi, int unlockRssi, uint32_t timeoutMs, OperatingSystem os, const String& unlockPwd, bool autoUnlock);
void saveRuntimeSettings(int lockRssi, int unlockRssi, uint32_t timeoutMs, OperatingSystem os, const String& unlockPwd);
void saveRuntimeSettings(int lockRssi, int unlockRssi, uint32_t timeoutMs, OperatingSystem os);
