#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "filter.h"
#include "hid_locker.h"
#include "ble_scanner.h"

// ==========================================
// 全局配置与状态变量
// ==========================================
static Preferences      g_prefs;
static OperatingSystem  g_osMode = OS_WINDOWS;
static ProximityState   g_state  = STATE_UNPAIRED;

static int              g_lockRSSI   = DEFAULT_LOCK_RSSI;
static int              g_unlockRSSI = DEFAULT_UNLOCK_RSSI;
static uint32_t         g_leaveTimeoutMs = DEFAULT_LEAVE_TIMEOUT_MS;

static uint32_t         g_stateEnterMs = 0;
static uint32_t         g_pauseStartMs = 0;
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

void setup() {
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

    // 初始化硬件子系统
    Locker.begin();
    Tracker.begin();

    // 触摸基准采样校准
    delay(100);
    g_touchBaselineOS = touchRead(PIN_TOUCH_OS);
    g_touchBaselinePause = touchRead(PIN_TOUCH_PAUSE);

    if (Tracker.isPaired()) {
        g_state = STATE_PRESENT;
    } else {
        g_state = STATE_UNPAIRED;
    }

    Serial.println("\n=======================================================");
    Serial.println("  ESP32-S3-SuperMini: ProximityLock 物理锁屏保镖 (v1.0) ");
    Serial.println("=======================================================");
    printStatus();
    Serial.println("\n输入 'help' 获取所有串口控制命令.\n");
}

void loop() {
    // 1. 处理串口控制台指令
    handleSerialCLI();

    // 2. BLE 扫描器周期维持
    Tracker.loop();

    // 3. 硬件按键与裸板电容触控
    handleBootButton();
    handleTouchPins();

    // 4. 核心状态机运转
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
            bool signalAlive = Tracker.isSignalAlive(1500);
            float rssi = Tracker.getFilteredRSSI();

            // 如果在超时时间内信号恢复（带 3dBm 迟滞），返回在场状态
            if (signalAlive && rssi >= (float)(g_lockRSSI + 3)) {
                g_state = STATE_PRESENT;
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
            bool signalAlive = Tracker.isSignalAlive(2000);
            float rssi = Tracker.getFilteredRSSI();

            // 当回到工位，且信号高于解锁阈值
            if (signalAlive && rssi >= (float)g_unlockRSSI) {
                Serial.printf("\n✨ [欢迎归位] 检测到目标靠近 (RSSI: %.1f dBm)，自动唤醒屏幕!\n", rssi);
                Locker.wakeScreen();
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

    // 5. 实时 ASCII 信号柱状图打印
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

    // 6. 板载 LED 视觉反馈
    updateLedFeedback();
    delay(20);
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

void handleTouchPins() {
    uint32_t now = millis();

    // 触摸 PIN 1 (GPIO 1): 切换目标操作系统
    if (now - g_lastTouchOSMs > 600) {
        uint32_t val = touchRead(PIN_TOUCH_OS);
        // S3 触摸时读数通常会有明显的跳变
        if (abs((int)val - (int)g_touchBaselineOS) > TOUCH_SENSITIVITY_DELTA) {
            g_lastTouchOSMs = now;
            OperatingSystem nextOS = (OperatingSystem)((g_osMode + 1) % 3);
            setOSMode(nextOS);
        }
    }

    // 触摸 PIN 2 (GPIO 2): 切换免打扰/会议模式
    if (now - g_lastTouchPauseMs > 600) {
        uint32_t val = touchRead(PIN_TOUCH_PAUSE);
        if (abs((int)val - (int)g_touchBaselinePause) > TOUCH_SENSITIVITY_DELTA) {
            g_lastTouchPauseMs = now;
            if (g_state == STATE_PAUSED) {
                g_state = STATE_PRESENT;
                Serial.println("\n[触控] 取消免打扰模式，恢复工作.");
            } else {
                g_state = STATE_PAUSED;
                g_pauseStartMs = now;
                Serial.println("\n[触控] 进入会议免打扰模式 (15分钟内不锁屏).");
            }
        }
    }
}

void setOSMode(OperatingSystem os) {
    g_osMode = os;
    g_prefs.putUChar("os_mode", (uint8_t)g_osMode);
    const char* osNames[] = {"Windows (Win+L)", "macOS (Cmd+Ctrl+Q)", "Linux (Ctrl+Alt+L)"};
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
        Tracker.setBoundMAC(arg);
        if (Tracker.isPaired()) g_state = STATE_PRESENT;
    } else if (cmd == "lock") {
        Serial.println("[测试] 发送锁屏击键测试.");
        Locker.lock(g_osMode);
    } else if (cmd == "wake") {
        Serial.println("[测试] 发送唤醒击键测试.");
        Locker.wakeScreen();
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
            g_prefs.putInt("lock_rssi", g_lockRSSI);
            g_prefs.putInt("unlock_rssi", g_unlockRSSI);
            Serial.printf("锁定阈值已更新: 离开 <= %d dBm, 归位 >= %d dBm\n", g_lockRSSI, g_unlockRSSI);
        } else {
            Serial.println("用法: thresh <rssi_value> (例如: thresh -82)");
        }
    } else if (cmd == "timeout") {
        uint32_t t = arg.toInt();
        if (t >= 1000 && t <= 30000) {
            g_leaveTimeoutMs = t;
            g_prefs.putULong("timeout_ms", g_leaveTimeoutMs);
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
    const char* osNames[] = {"Windows (Win+L)", "macOS (Cmd+Ctrl+Q)", "Linux (Ctrl+Alt+L)"};
    const char* stateNames[] = {"未配对", "配对中", "在场(工作中)", "离开预警", "已锁定", "会议免打扰"};

    Serial.println("\n----------------- 当前运行状态 -----------------");
    Serial.printf("绑定设备 MAC : %s\n", Tracker.isPaired() ? Tracker.getBoundMAC().c_str() : "未绑定");
    Serial.printf("当前运行状态 : %s\n", stateNames[g_state]);
    Serial.printf("目标操作系统 : %s\n", osNames[g_osMode]);
    Serial.printf("当前过滤信号 : %.1f dBm (最后收到: %lu 毫秒前)\n", 
                  Tracker.getFilteredRSSI(), Tracker.getLastSeenElapsed());
    Serial.printf("距离判定阈值 : 离开 <= %d dBm | 回位 >= %d dBm\n", g_lockRSSI, g_unlockRSSI);
    Serial.printf("锁屏超时判定 : %lu 毫秒\n", g_leaveTimeoutMs);
    Serial.println("------------------------------------------------");
}

void printHelp() {
    Serial.println("\n--- ProximityLock 串口指令菜单 ---");
    Serial.println("  status           : 显示当前设备状态、距离与配置");
    Serial.println("  pair             : 启动贴近自动配对模式 (限时8秒)");
    Serial.println("  mac <MAC>        : 手动指定绑定设备的蓝牙 MAC");
    Serial.println("  clear            : 清除已绑定的设备");
    Serial.println("  os <win|mac|linux>: 切换当前操作系统锁屏组合键");
    Serial.println("  thresh <RSSI>    : 设置离开锁屏阈值 (如 thresh -82)");
    Serial.println("  timeout <毫秒>   : 设置离开超时等待时长 (如 timeout 4000)");
    Serial.println("  lock             : 立即测试一次锁屏按键发送");
    Serial.println("  wake             : 立即测试一次屏幕唤醒");
    Serial.println("  monitor <on|off> : 开启/关闭控制台 ASCII 实时信号柱状图");
    Serial.println("-----------------------------------");
}
