#include "wifi_manager.h"

WiFiManager WiFiMgr;

static const IPAddress apIP(192, 168, 4, 1);
static const IPAddress netMsk(255, 255, 255, 0);

WiFiManager::WiFiManager()
    : _apActive(false),
      _connecting(false),
      _connectStartMs(0),
      _lastCheckMs(0),
      _staConnectedMs(0),
      _staWasConnected(false),
      _apNoClientStartMs(0) {}

void WiFiManager::begin() {
    _prefs.begin("prox_wifi", false);
    _ssid = _prefs.getString("sta_ssid", "");
    _pass = _prefs.getString("sta_pass", "");

    // 自动根据 ESP32-S3 MAC 生成独立 AP 名称，例如 ProximityLock-70F8
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[32];
    snprintf(buf, sizeof(buf), "ProximityLock-%02X%02X", mac[4], mac[5]);
    _apName = String(buf);

    // 开启 AP+STA 双模
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(WIFI_PS_MIN_MODEM); // 启用 Modem-Sleep 深度节能模式，无流量时关闭高频锁相环，温度直降 15~20°C
    WiFi.setTxPower(WIFI_POWER_11dBm); // 发射功率调谐至 11dBm (约 12mW)，比原 15dBm/19dBm 功耗降低 60% 以上，桌面与工位穿透稳定

    // 启动热点
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(_apName.c_str(), NULL, 1, 0, 4); // 默认无密码，方便首次免输连入
    _dnsServer.start(53, "*", apIP);
    _apActive = true;
    _apNoClientStartMs = millis(); // 启动 30 秒无连接倒计时

    Serial.printf("[WiFi] 板载 AP 热点已就绪: %s (IP: 192.168.4.1，若30秒内无设备连入将自动关闭节能)\n", _apName.c_str());

    // 若有已保存的 Wi-Fi 凭证，异步尝试连接路由器
    if (_ssid.length() > 0) {
        Serial.printf("[WiFi] 正在后台连接已配置的路由器 Wi-Fi: %s ...\n", _ssid.c_str());
        WiFi.begin(_ssid.c_str(), _pass.c_str());
        _connecting = true;
        _connectStartMs = millis();
    } else {
        Serial.println("[WiFi] 暂无已保存的路由器 Wi-Fi，请连接板载热点配网.");
    }
}

void WiFiManager::loop() {
    uint32_t now = millis();

    // 维持 Captive Portal DNS 请求转发
    if (_apActive) {
        _dnsServer.processNextRequest();
    }

    // 异步连接超时监控与自动重连检测 (每秒巡检一次)
    if (now - _lastCheckMs >= 1000) {
        _lastCheckMs = now;

        if (_connecting) {
            if (WiFi.status() == WL_CONNECTED) {
                _connecting = false;
                _staWasConnected = true;
                _staConnectedMs = now;
                Serial.printf("\n🎉 [WiFi] 成功连接路由器 Wi-Fi: %s!\n", _ssid.c_str());
                Serial.printf("   • 内网访问地址: http://%s\n", WiFi.localIP().toString().c_str());
                Serial.printf("   • 信号强度: %d dBm\n", WiFi.RSSI());
                Serial.println("   • 提示: 路由器稳定连接 30 秒后将自动关闭 AP 热点以节省能耗与降温。\n");

                // 启动 mDNS 域名: http://proximitylock.local
                if (MDNS.begin("proximitylock")) {
                    MDNS.addService("http", "tcp", 80);
                    Serial.println("   • mDNS 服务已上线: http://proximitylock.local\n");
                }
            } else if (now - _connectStartMs >= 15000) {
                _connecting = false;
                Serial.printf("\n⚠️ [WiFi] 连接路由器 [%s] 超时，请检查密码或信号. 可通过直连热点 http://192.168.4.1 重新配网.\n", _ssid.c_str());
            }
        }

        // 状态巡检与 AP 自动关闭逻辑 (满足以下任一条件满 30 秒即彻底关闭 AP 射频以降低发热)
        // 1. 成功连通路由器 Wi-Fi 满 30 秒
        // 2. 无任何客户端接入满 30 秒
        if (_apActive) {
            bool shouldClose = false;
            String reason = "";

            if (isConnected() && _staConnectedMs > 0 && (now - _staConnectedMs >= 30000)) {
                shouldClose = true;
                reason = "路由器 Wi-Fi 已稳定连通满 30 秒";
            } else {
                int clientCount = WiFi.softAPgetStationNum();
                if (clientCount == 0) {
                    if (_apNoClientStartMs == 0) {
                        _apNoClientStartMs = now;
                    } else if (now - _apNoClientStartMs >= 30000) {
                        shouldClose = true;
                        reason = "板载 AP 在过去 30 秒内无设备连接";
                    }
                } else {
                    // 有设备连接中，重置计时器，保证配置时不被中断
                    _apNoClientStartMs = 0;
                }
            }

            if (shouldClose) {
                Serial.printf("\n🌿 [WiFi] %s，已自动关闭 AP 射频以降低发热与干扰.\n", reason.c_str());
                disableAP();
            }
        }

        if (!isConnected()) {
            // 如果 Wi-Fi 连接中断，且 AP 处于关闭状态，自动重新唤醒 AP，防止用户失联
            if (_staWasConnected) {
                _staWasConnected = false;
                Serial.println("\n⚠️ [WiFi] 路由器 Wi-Fi 连通中断，正在重新唤醒 AP 热点以供排查恢复...");
                enableAP();
            }
        }
    }
}

bool WiFiManager::isConnected() const {
    return (WiFi.status() == WL_CONNECTED);
}

String WiFiManager::getSTAIP() const {
    if (isConnected()) {
        return WiFi.localIP().toString();
    }
    return "0.0.0.0";
}

String WiFiManager::getAPIP() const {
    return apIP.toString();
}

int WiFiManager::getRSSI() const {
    if (isConnected()) {
        return WiFi.RSSI();
    }
    return -999;
}

uint32_t WiFiManager::getAPShutdownRemainSec() const {
    if (!_apActive) return 0;
    uint32_t now = millis();

    if (isConnected() && _staConnectedMs > 0) {
        uint32_t elapsed = now - _staConnectedMs;
        if (elapsed >= 30000) return 0;
        return (30000 - elapsed) / 1000;
    }

    if (WiFi.softAPgetStationNum() > 0) return 999; // 正在使用中

    if (_apNoClientStartMs == 0) return 30;
    uint32_t elapsed = now - _apNoClientStartMs;
    if (elapsed >= 30000) return 0;
    return (30000 - elapsed) / 1000;
}

void WiFiManager::disableAP() {
    if (!_apActive) return;
    _dnsServer.stop();
    WiFi.softAPdisconnect(true);
    if (isConnected()) {
        WiFi.mode(WIFI_STA);
    } else {
        WiFi.mode(WIFI_OFF);
    }
    _apActive = false;
    _apNoClientStartMs = 0;
    _staConnectedMs = 0;
    Serial.println("[WiFi] 板载 AP 热点已关闭 (已彻底关闭 AP 射频，大幅降低发热).");
}

void WiFiManager::enableAP() {
    if (_apActive) return;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(_apName.c_str(), NULL, 1, 0, 4);
    _dnsServer.start(53, "*", apIP);
    _apActive = true;
    _apNoClientStartMs = millis();
    _staConnectedMs = millis();
    Serial.printf("[WiFi] 板载 AP 热点已重新开启: %s (IP: 192.168.4.1，30秒后将自动关闭)\n", _apName.c_str());
}

void WiFiManager::saveWiFiConfig(const String& ssid, const String& pass) {
    _ssid = ssid;
    _pass = pass;
    _prefs.putString("sta_ssid", _ssid);
    _prefs.putString("sta_pass", _pass);

    Serial.printf("[WiFi] 写入新 Wi-Fi 凭证: %s, 正在发起连接...\n", _ssid.c_str());
    WiFi.disconnect();
    WiFi.begin(_ssid.c_str(), _pass.c_str());
    _connecting = true;
    _connectStartMs = millis();
}

void WiFiManager::resetWiFiConfig() {
    _prefs.remove("sta_ssid");
    _prefs.remove("sta_pass");
    _ssid = "";
    _pass = "";
    WiFi.disconnect();
    _connecting = false;
    enableAP();
    Serial.println("[WiFi] 已重置路由器 Wi-Fi 凭证，恢复纯 AP 热点直连模式.");
}

void WiFiManager::processDNS() {
    if (_apActive) {
        _dnsServer.processNextRequest();
    }
}
