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
      _staWasConnected(false) {}

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
    WiFi.setSleep(true); // 节能模式，降低 S3 射频发热并与 BLE 良好共存
    WiFi.setTxPower(WIFI_POWER_15dBm); // 适度调谐发射功率 (15dBm)，大幅降低微型核心发热量，覆盖依然充沛

    // 启动热点
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(_apName.c_str(), NULL, 1, 0, 4); // 默认无密码，方便首次免输连入
    _dnsServer.start(53, "*", apIP);
    _apActive = true;

    Serial.printf("[WiFi] 默认直连 AP 热点已就绪: %s (IP: 192.168.4.1)\n", _apName.c_str());

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

    // 异步连接超时监控与自动重连检测
    if (now - _lastCheckMs >= 2000) {
        _lastCheckMs = now;

        if (_connecting) {
            if (WiFi.status() == WL_CONNECTED) {
                _connecting = false;
                _staWasConnected = true;
                _staConnectedMs = now;
                Serial.printf("\n🎉 [WiFi] 成功连接路由器 Wi-Fi: %s!\n", _ssid.c_str());
                Serial.printf("   • 内网访问地址: http://%s\n", WiFi.localIP().toString().c_str());
                Serial.printf("   • 信号强度: %d dBm\n", WiFi.RSSI());
                Serial.println("   • 提示: 路由器稳定连接 3 分钟后将自动关闭 AP 热点以节省能耗。\n");

                // 启动 mDNS 域名: http://proximitylock.local
                if (MDNS.begin("proximitylock")) {
                    MDNS.addService("http", "tcp", 80);
                    Serial.println("   • mDNS 服务已上线: http://proximitylock.local\n");
                }
            } else if (now - _connectStartMs >= 15000) {
                _connecting = false;
                Serial.printf("\n⚠️ [WiFi] 连接路由器 [%s] 超时，请检查密码或信号. 可通过直连热点 http://192.168.4.1 重新配网.\n", _ssid.c_str());
            }
        } else {
            // 已建立连接后的状态巡检
            if (isConnected()) {
                if (!_staWasConnected) {
                    _staWasConnected = true;
                    _staConnectedMs = now;
                }
                // 当 Wi-Fi 网络稳定达到 3 分钟 (180 秒)，且 AP 当前仍处于开启状态时，自动关闭 AP
                if (_apActive && (now - _staConnectedMs >= 180000)) {
                    disableAP();
                }
            } else {
                // 如果 Wi-Fi 连接中断，且 AP 处于关闭状态，自动重新唤醒 AP，防止用户失联
                if (_staWasConnected) {
                    _staWasConnected = false;
                    Serial.println("\n⚠️ [WiFi] 路由器 Wi-Fi 连通中断，正在重新唤醒 AP 热点以供排查恢复...");
                    enableAP();
                }
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
    if (!_apActive || !isConnected()) return 0;
    uint32_t elapsed = millis() - _staConnectedMs;
    if (elapsed >= 180000) return 0;
    return (180000 - elapsed) / 1000;
}

void WiFiManager::disableAP() {
    if (!_apActive) return;
    _dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    _apActive = false;
    Serial.println("[WiFi] 路由器 Wi-Fi 持续稳定已满 3 分钟，已自动关闭板载 AP 热点 (降低 2.4GHz 射频干扰与发热).");
}

void WiFiManager::enableAP() {
    if (_apActive) return;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(_apName.c_str(), NULL, 1, 0, 4);
    _dnsServer.start(53, "*", apIP);
    _apActive = true;
    Serial.printf("[WiFi] 板载 AP 热点已重新开启: %s (IP: 192.168.4.1)\n", _apName.c_str());
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
