#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "filter.h"
#include "hid_locker.h"
#include "ble_scanner.h"
#include "wifi_manager.h"
#include "web_server.h"

// ==========================================
// 全局配置与状态变量
// ==========================================
static Preferences      g_prefs;
OperatingSystem         g_osMode = OS_WINDOWS;
ProximityState          g_state  = STATE_UNPAIRED;

int                     g_lockRSSI   = DEFAULT_LOCK_RSSI;
int                     g_unlockRSSI = DEFAULT_UNLOCK_RSSI;
uint32_t                g_leaveTimeoutMs = DEFAULT_LEAVE_TIMEOUT_MS;
String                  g_unlockPassword = "";
bool                    g_autoUnlockEnabled = false;

uint32_t                g_stateEnterMs = 0;
uint32_t                g_pauseStartMs = 0;
static bool             g_monitorEnabled = false;

// 触摸引脚基准值 (自动校准)
static uint32_t         g_touchBaselineOS = 0;
static uint32_t         g_touchBaselinePause = 0;
static uint32_t         g_lastTouchOSMs = 0;
static uint32_t         g_lastTouchPauseMs = 0;

// 按键长按检测
static uint32_t         g_bootPressStartMs = 0;
static bool             g_bootPressed = false;

// LED 效果计时
static uint32_t         g_lastLedToggleMs = 0;
static bool             g_ledState = false;

// ==========================================
// 辅助函数声明
// ==========================================
void handleSerialCLI();
void updateLedFeedback();
void handleTouchPins();
void handleBootButton();
void printStatus();
void printHelp();
void setOSMode(OperatingSystem os);

void saveRuntimeSettings(int lockRssi, int unlockRssi, uint32_t timeoutMs, OperatingSystem os, const String& unlockPwd, bool autoUnlock) {
    // 关键稳定性防护：强制保证唤醒阈值至少比锁屏阈值大 6dBm，杜绝死区重叠导致的反复横跳
    if (unlockRssi < lockRssi + 6) {
        unlockRssi = lockRssi + 6;
    }

    g_lockRSSI = lockRssi;
    g_unlockRSSI = unlockRssi;
    g_leaveTimeoutMs = timeoutMs;
    g_osMode = os;
    g_unlockPassword = unlockPwd;
    g_autoUnlockEnabled = autoUnlock;

    g_prefs.begin("prox_cfg", false);
    g_prefs.putInt("lock_rssi", g_lockRSSI);
    g_prefs.putInt("unlock_rssi", g_unlockRSSI);
    g_prefs.putULong("timeout_ms", g_leaveTimeoutMs);
    g_prefs.putUChar("os_mode", (uint8_t)g_osMode);
    g_prefs.putString("unlock_pwd", g_unlockPassword);
    g_prefs.putBool("auto_unlock", g_autoUnlockEnabled);
    g_prefs.end();

    Serial.printf("[配置] 参数已更新: 锁屏 <= %d dBm, 唤醒 >= %d dBm, 超时 = %lu ms, OS = %d, 自动解锁=%s, 密码=%s\n",
                  g_lockRSSI, g_unlockRSSI, g_leaveTimeoutMs, (int)g_osMode,
                  g_autoUnlockEnabled ? "开启" : "仅亮屏",
                  g_unlockPassword.length() > 0 ? "已设" : "无");
}

void saveRuntimeSettings(int lockRssi, int unlockRssi, uint32_t timeoutMs, OperatingSystem os, const String& unlockPwd) {
    saveRuntimeSettings(lockRssi, unlockRssi, timeoutMs, os, unlockPwd, g_autoUnlockEnabled);
}

void saveRuntimeSettings(int lockRssi, int unlockRssi, uint32_t timeoutMs, OperatingSystem os) {
    saveRuntimeSettings(lockRssi, unlockRssi, timeoutMs, os, g_unlockPassword, g_autoUnlockEnabled);
}

void setup() {
    setCpuFrequencyMhz(80); // 调频至 80MHz 极速节能档 (伴侣直连与USB完全绰绰有余，功耗发热直降约60%)

    // 优先初始化 USB HID 键盘复合总线 (抢先向主机发起总线物理重枚举)
    Locker.begin();

    Serial.begin(115200);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    pinMode(PIN_BOOT, INPUT_PULLUP);

    // 载入持久化配置
    g_prefs.begin("prox_cfg", false);
    g_osMode = (OperatingSystem)g_prefs.getUChar("os_mode", OS_WINDOWS);
    g_lockRSSI = g_prefs.getInt("lock_rssi", DEFAULT_LOCK_RSSI);
    g_unlockRSSI = g_prefs.getInt("unlock_rssi", DEFAULT_UNLOCK_RSSI);
    g_leaveTimeoutMs = g_prefs.getULong("timeout_ms", DEFAULT_LEAVE_TIMEOUT_MS);
    g_unlockPassword = g_prefs.getString("unlock_pwd", "");
    g_autoUnlockEnabled = g_prefs.getBool("auto_unlock", false);
    g_prefs.end();

    // 初始化蓝牙追踪扫描器
    Tracker.begin();

    // 初始化 Wi-Fi 管理器 (AP 配网 + STA 局域网访问 + Captive Portal)
    WiFiMgr.begin();

    // 启动 Web 服务
    WebServ.begin();

#if ENABLE_TOUCH_PINS
    // 触摸基准采样校准
    delay(100);
    g_touchBaselineOS = touchRead(PIN_TOUCH_OS);
    g_touchBaselinePause = touchRead(PIN_TOUCH_PAUSE);
#endif

    if (Tracker.isPaired()) {
        g_state = STATE_PRESENT;
    } else {
        g_state = STATE_UNPAIRED;
    }

    Serial.println("\n=======================================================");
    Serial.println("  ESP32-S3-SuperMini: ProximityLock 物理锁屏保镖 (v2.0) ");
    Serial.println("=======================================================");
    printStatus();
    Serial.println("\n输入 'help' 获取所有串口控制命令.\n");
}

void loop() {
    // 1. 处理串口控制台指令
    handleSerialCLI();

    // 2. BLE 扫描器周期维持
    Tracker.loop();

    // 3. Wi-Fi 与 Web 管理服务维护
    WiFiMgr.loop();
    WebServ.loop();

    // 4. 硬件按键与裸板电容触控
    handleBootButton();
#if ENABLE_TOUCH_PINS
    handleTouchPins();
#endif

    // 5. 核心状态机运转
    uint32_t now = millis();

    switch (g_state) {
        case STATE_UNPAIRED:
            if (Tracker.isPaired()) {
                g_state = STATE_PRESENT;
                g_stateEnterMs = now;
            }
            break;

        case STATE_PAIRING:
            if (Tracker.checkPairingFinished()) {
                g_state = STATE_PRESENT;
                g_stateEnterMs = now;
                // 成功配对后闪烁 3 次
                for (int i = 0; i < 3; i++) {
                    digitalWrite(PIN_LED, HIGH); delay(100);
                    digitalWrite(PIN_LED, LOW); delay(100);
                }
            }
            break;

        case STATE_PRESENT: {
            // 关键优化 1: 如果后台正在执行开屏/打密码，绝对禁止检测离席
            if (Locker.isUnlocking()) {
                break;
            }

            // 关键优化 2: 回座解锁后的“在场保护冷静期 (Grace Period)”
            // 刚坐下 8 秒内，人体遮挡或信标重连可能导致瞬态掉包或信号抖动，坚决不进入 LEAVING
            if (now - g_stateEnterMs < 8000) {
                break;
            }

            bool signalAlive = Tracker.isSignalAlive(g_leaveTimeoutMs);
            float rssi = Tracker.getFilteredRSSI();

            if (!signalAlive || rssi < (float)g_lockRSSI) {
                g_state = STATE_LEAVING;
                g_stateEnterMs = now;
                Serial.printf("\n[状态] 离开预警! 信号减弱 (RSSI: %.1f dBm, 信号超时: %s)\n",
                              rssi, signalAlive ? "正常" : "无包");
            }
            break;
        }

        case STATE_LEAVING: {
            // 关键互斥: 若后台正在开屏/打密码，立即打断离开状态，恢复在场
            if (Locker.isUnlocking()) {
                g_state = STATE_PRESENT;
                g_stateEnterMs = now;
                break;
            }

            bool signalAlive = Tracker.isSignalAlive(4000);
            float rssi = Tracker.getFilteredRSSI();

            // 如果在超时时间内信号恢复（带 3dBm 迟滞），返回在场状态
            if (signalAlive && rssi >= (float)(g_lockRSSI + 3)) {
                g_state = STATE_PRESENT;
                g_stateEnterMs = now;
                Serial.println("[状态] 信号恢复，取消锁屏，保持在场.");
                break;
            }

            // 持续离开超时触发锁屏
            if (now - g_stateEnterMs >= g_leaveTimeoutMs) {
                Serial.println("\n🚨 [安全触发] 远离工位超时，正在发送物理锁屏快捷键...");
                Locker.lock(g_osMode);
                g_state = STATE_LOCKED;
                g_stateEnterMs = now;
            }
            break;
        }

        case STATE_LOCKED: {
            // 关键安全防抖：锁屏后必须至少静默 6 秒，等待操作系统锁屏动画彻底完成，杜绝边缘横跳与误输密码
            if (now - g_stateEnterMs < 6000) {
                break;
            }

            // 如果正在后台异步击键输入密码，避免重复派发
            if (Locker.isUnlocking()) {
                break;
            }

            bool signalAlive = Tracker.isSignalAlive(4500);
            float rssi = Tracker.getFilteredRSSI();

            // 当回到工位，且信号高于解锁阈值
            if (signalAlive && rssi >= (float)g_unlockRSSI) {
                if (g_autoUnlockEnabled) {
                    Serial.printf("\n✨ [欢迎归位] 目标靠近 (RSSI: %.1f dBm)，自动唤醒屏幕并执行开屏解锁!\n", rssi);
                    Locker.unlock(g_osMode, g_unlockPassword);
                } else {
                    Serial.printf("\n✨ [欢迎归位] 目标靠近 (RSSI: %.1f dBm)，仅点亮唤醒屏幕 (自动解锁未开启).\n", rssi);
                    Locker.wakeScreen();
                }
                g_state = STATE_PRESENT;
                g_stateEnterMs = now;
            }
            break;
        }

        case STATE_PAUSED: {
            if (now - g_pauseStartMs >= DEFAULT_PAUSE_DURATION_MS) {
                g_state = STATE_PRESENT;
                Serial.println("[状态] 会议免打扰时间结束，恢复自动锁屏监护.");
            }
            break;
        }
    }

    // 6. 实时 ASCII 信号柱状图打印
    if (g_monitorEnabled && Tracker.isPaired() && (now % 500 < 20)) {
        float fRssi = Tracker.getFilteredRSSI();
        int bars = map(constrain((int)fRssi, -95, -45), -95, -45, 0, 20);
        Serial.printf("\rRSSI: %5.1f dBm [", fRssi);
        for (int i = 0; i < 20; i++) {
            Serial.print(i < bars ? "=" : " ");
        }
        Serial.printf("] 状态: %s (耗时: %lums)   ", 
                      g_state == STATE_PRESENT ? "在场" : 
                      g_state == STATE_LEAVING ? "离开中" :
                      g_state == STATE_LOCKED  ? "已锁定" : "暂停",
                      Tracker.getLastSeenElapsed());
    }

    // 7. 板载 LED 视觉反馈
    updateLedFeedback();
    delay(25); // 40Hz 适度降频巡检，按键消抖依然敏捷，但大幅跃升 FreeRTOS CPU 空闲休眠率
}

// ==========================================
// 硬件交互实现：按键与裸板电容触控
// ==========================================
void handleBootButton() {
    bool isDown = (digitalRead(PIN_BOOT) == LOW);
    uint32_t now = millis();

    if (isDown && !g_bootPressed) {
        g_bootPressed = true;
        g_bootPressStartMs = now;
    } else if (!isDown && g_bootPressed) {
        uint32_t duration = now - g_bootPressStartMs;
        g_bootPressed = false;

        if (duration >= 2500) {
            // 长按 2.5 秒以上：开启极速贴近配对模式
            Serial.println("\n[按键] 长按触发：启动贴近配对模式!");
            Tracker.startPairing(8000);
            g_state = STATE_PAIRING;
        } else if (duration > 50) {
            // 短按：立即手动测试执行一次锁屏
            Serial.println("\n[按键] 短按触发：立即执行手动锁屏测试!");
            Locker.lock(g_osMode);
        }
    }
}

#if ENABLE_TOUCH_PINS
void handleTouchPins() {
    uint32_t now = millis();

    // 触摸 PIN 1 (GPIO 1): 切换目标操作系统
    if (now - g_lastTouchOSMs > 600) {
        uint32_t val = touchRead(PIN_TOUCH_OS);
        if (abs((int)val - (int)g_touchBaselineOS) > TOUCH_SENSITIVITY_DELTA) {
            g_lastTouchOSMs = now;
            OperatingSystem nextOS = (OperatingSystem)(((int)g_osMode + 1) % 3);
            setOSMode(nextOS);
        }
    }

    // 触摸 PIN 2 (GPIO 2): 会议/摸鱼临时暂停锁定 (15分钟免打扰)
    if (now - g_lastTouchPauseMs > 800) {
        uint32_t val = touchRead(PIN_TOUCH_PAUSE);
        if (abs((int)val - (int)g_touchBaselinePause) > TOUCH_SENSITIVITY_DELTA) {
            g_lastTouchPauseMs = now;
            if (g_state == STATE_PAUSED) {
                g_state = STATE_PRESENT;
                Serial.println("\n[触控 PIN 2] 退出会议模式，恢复离开自动锁屏!");
                digitalWrite(PIN_LED, LOW);
            } else {
                g_state = STATE_PAUSED;
                g_pauseStartMs = now;
                Serial.println("\n[触控 PIN 2] 开启 15 分钟会议临时免打扰，暂停锁屏!");
                for (int i = 0; i < 2; i++) {
                    digitalWrite(PIN_LED, HIGH); delay(150);
                    digitalWrite(PIN_LED, LOW); delay(150);
                }
            }
        }
    }
}
#endif

void setOSMode(OperatingSystem os) {
    if (g_osMode == os) return;
    g_osMode = os;
    g_prefs.begin("prox_cfg", false);
    g_prefs.putUChar("os_mode", (uint8_t)g_osMode);
    g_prefs.end();

    const char* osNames[] = {"Windows (Win+L)", "macOS (Cmd+Ctrl+Q)", "Linux (Ctrl+Alt+L / Super+L)"};
    Serial.printf("\n[系统切换] 当前操作系统已设为: %s\n", osNames[g_osMode]);

    // 闪烁对应次数提示: Win 1次, Mac 2次, Linux 3次
    for (int i = 0; i <= (int)g_osMode; i++) {
        digitalWrite(PIN_LED, HIGH); delay(80);
        digitalWrite(PIN_LED, LOW); delay(80);
    }
}

// ==========================================
// 状态指示灯 (GPIO 48) 视觉反馈引擎
// ==========================================
void updateLedFeedback() {
    uint32_t now = millis();
    uint32_t period = 1000;

    switch (g_state) {
        case STATE_UNPAIRED:
            period = 600; // 慢闪：未配对
            break;
        case STATE_PAIRING:
            period = 100; // 急促快闪：搜寻配对中
            break;
        case STATE_PRESENT:
            // 在场时：平时熄灭，每 2.5 秒极短微闪 1 次表示在岗守护
            if (now % 2500 < 50) {
                digitalWrite(PIN_LED, HIGH);
            } else {
                digitalWrite(PIN_LED, LOW);
            }
            return;
        case STATE_LEAVING:
            period = 150; // 快速警报闪烁
            break;
        case STATE_LOCKED:
            // 锁定状态：常亮
            digitalWrite(PIN_LED, HIGH);
            return;
        case STATE_PAUSED:
            period = 1200; // 极慢呼吸闪烁：会议免打扰
            break;
    }

    if (now - g_lastLedToggleMs >= period / 2) {
        g_lastLedToggleMs = now;
        g_ledState = !g_ledState;
        digitalWrite(PIN_LED, g_ledState ? HIGH : LOW);
    }
}

// ==========================================
// 串口交互命令行 (CLI)
// ==========================================
void handleSerialCLI() {
    if (!Serial.available()) return;

    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    int spaceIdx = line.indexOf(' ');
    String cmd = (spaceIdx == -1) ? line : line.substring(0, spaceIdx);
    String arg = (spaceIdx == -1) ? "" : line.substring(spaceIdx + 1);
    cmd.toLowerCase();

    if (cmd == "help" || cmd == "?") {
        printHelp();
    } else if (cmd == "status") {
        printStatus();
    } else if (cmd == "pair") {
        Tracker.startPairing(8000);
        g_state = STATE_PAIRING;
    } else if (cmd == "clear") {
        Tracker.clearBond();
        g_state = STATE_UNPAIRED;
    } else if (cmd == "mac") {
        Tracker.setBoundDevice(arg, "手动MAC");
        if (Tracker.isPaired()) g_state = STATE_PRESENT;
    } else if (cmd == "lock") {
        Serial.println("[测试] 发送锁屏击键测试.");
        Locker.lock(g_osMode);
    } else if (cmd == "wake") {
        Serial.println("[测试] 发送唤醒击键测试.");
        Locker.wakeScreen();
    } else if (cmd == "unlock") {
        Serial.printf("[测试] 发送开屏解锁测试 (密码: %s).\n", g_unlockPassword.length() > 0 ? "已设" : "无");
        Locker.unlock(g_osMode, g_unlockPassword);
    } else if (cmd == "pwd" || cmd == "pass") {
        if (arg == "clear") {
            saveRuntimeSettings(g_lockRSSI, g_unlockRSSI, g_leaveTimeoutMs, g_osMode, "");
            Serial.println("[配置] 已清空开屏解锁密码 (切换为无密码唤醒模式).");
        } else if (arg.length() > 0) {
            saveRuntimeSettings(g_lockRSSI, g_unlockRSSI, g_leaveTimeoutMs, g_osMode, arg);
            Serial.println("[配置] 开屏解锁密码已更新保存.");
        } else {
            Serial.printf("当前密码状态: %s\n用法: pwd <密码> (设置密码) | pwd clear (清除密码)\n",
                          g_unlockPassword.length() > 0 ? "已设置" : "未设置(仅唤醒)");
        }
    } else if (cmd == "autounlock") {
        arg.toLowerCase();
        if (arg == "on" || arg == "1" || arg == "true") {
            saveRuntimeSettings(g_lockRSSI, g_unlockRSSI, g_leaveTimeoutMs, g_osMode, g_unlockPassword, true);
            Serial.println("[配置] 自动解锁功能已开启 (回位时自动执行开屏与密码输入).");
        } else if (arg == "off" || arg == "0" || arg == "false") {
            saveRuntimeSettings(g_lockRSSI, g_unlockRSSI, g_leaveTimeoutMs, g_osMode, g_unlockPassword, false);
            Serial.println("[配置] 自动解锁功能已关闭 (回位时仅点亮唤醒屏幕).");
        } else {
            Serial.printf("自动解锁当前状态: %s\n用法: autounlock on | autounlock off\n",
                          g_autoUnlockEnabled ? "开启" : "关闭(仅亮屏)");
        }
    } else if (cmd == "os") {
        arg.toLowerCase();
        if (arg == "win") setOSMode(OS_WINDOWS);
        else if (arg == "mac") setOSMode(OS_MACOS);
        else if (arg == "linux") setOSMode(OS_LINUX);
        else Serial.println("用法: os win | os mac | os linux");
    } else if (cmd == "thresh") {
        int l = arg.toInt();
        if (l < -40 && l > -100) {
            g_lockRSSI = l;
            g_unlockRSSI = l + 12; // 解锁阈值自动加 12dBm 形成自然迟滞
            saveRuntimeSettings(g_lockRSSI, g_unlockRSSI, g_leaveTimeoutMs, g_osMode, g_unlockPassword);
            Serial.printf("锁定阈值已更新: 离开 <= %d dBm, 归位 >= %d dBm\n", g_lockRSSI, g_unlockRSSI);
        } else {
            Serial.println("用法: thresh <rssi_value> (例如: thresh -82)");
        }
    } else if (cmd == "timeout") {
        uint32_t t = arg.toInt();
        if (t >= 1000 && t <= 30000) {
            g_leaveTimeoutMs = t;
            saveRuntimeSettings(g_lockRSSI, g_unlockRSSI, g_leaveTimeoutMs, g_osMode, g_unlockPassword);
            Serial.printf("离开判定超时更新为: %lu 毫秒\n", g_leaveTimeoutMs);
        } else {
            Serial.println("用法: timeout <毫秒> (范围 1000 ~ 30000)");
        }
    } else if (cmd == "monitor") {
        arg.toLowerCase();
        g_monitorEnabled = (arg == "on" || arg == "1");
        Serial.printf("实时信号监控图表: %s\n", g_monitorEnabled ? "已开启" : "已关闭");
    } else {
        Serial.printf("未知指令: '%s'. 输入 'help' 查看支持的命令.\n", cmd.c_str());
    }
}

void printStatus() {
    const char* osNames[] = {"Windows (Win+L)", "macOS (Cmd+Ctrl+Q)", "Linux (Ctrl+Alt+L / Super+L)"};
    const char* stateNames[] = {"未配对", "配对中", "在场(工作中)", "离开预警", "已锁定", "会议免打扰"};

    Serial.println("\n----------------- 当前运行状态 -----------------");
    Serial.printf("绑定设备 MAC : %s (%s)\n", 
                  Tracker.isPaired() ? Tracker.getBoundMAC().c_str() : "未绑定",
                  Tracker.getBoundName().length() > 0 ? Tracker.getBoundName().c_str() : "无备注");
    Serial.printf("当前运行状态 : %s\n", stateNames[g_state]);
    Serial.printf("目标操作系统 : %s\n", osNames[g_osMode]);
    Serial.printf("开屏行为模式 : %s\n", g_autoUnlockEnabled ? (g_unlockPassword.length() > 0 ? "自动输入密码解锁" : "自动滑起壁纸/免密解锁") : "仅唤醒点亮屏幕 (未开启自动解锁)");
    Serial.printf("当前过滤信号 : %.1f dBm (最后收到: %lu 毫秒前)\n", 
                  Tracker.getFilteredRSSI(), Tracker.getLastSeenElapsed());
    Serial.printf("距离判定阈值 : 离开 <= %d dBm | 回位 >= %d dBm\n", g_lockRSSI, g_unlockRSSI);
    Serial.printf("锁屏超时判定 : %lu 毫秒\n", g_leaveTimeoutMs);
    
    if (WiFiMgr.isConnected()) {
        Serial.printf("网络连接状态 : 已连接 WiFi [%s], IP: http://%s\n", 
                      WiFiMgr.getSSID().c_str(), WiFiMgr.getSTAIP().c_str());
    } else {
        Serial.printf("网络连接状态 : 纯直连 AP 热点 [%s], 管理地址: http://192.168.4.1\n", 
                      WiFiMgr.getAPName().c_str());
    }
    Serial.println("------------------------------------------------");
}

void printHelp() {
    Serial.println("\n--- ProximityLock 串口指令菜单 ---");
    Serial.println("  status           : 显示当前设备状态、距离与配置");
    Serial.println("  pair             : 启动贴近自动配对模式 (限时8秒)");
    Serial.println("  mac <MAC>        : 手动指定绑定设备的蓝牙 MAC");
    Serial.println("  clear            : 清除已绑定的设备");
    Serial.println("  os <win|mac|linux>: 切换当前操作系统锁屏组合键");
    Serial.println("  pwd <密码|clear> : 设置/清除开屏自动解锁密码");
    Serial.println("  autounlock <on|off>: 开启/关闭靠近自动开屏解锁");
    Serial.println("  thresh <RSSI>    : 设置离开锁屏阈值 (如 thresh -82)");
    Serial.println("  timeout <毫秒>   : 设置离开超时等待时长 (如 timeout 4000)");
    Serial.println("  lock             : 立即测试一次锁屏按键发送");
    Serial.println("  wake             : 立即测试一次屏幕唤醒");
    Serial.println("  unlock           : 立即测试一次开屏解锁与密码输入");
    Serial.println("  monitor <on|off> : 开启/关闭控制台 ASCII 实时信号柱状图");
    Serial.println("-----------------------------------");
}
