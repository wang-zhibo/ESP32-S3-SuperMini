#include "wifi_manager.h"
#include <esp_wifi.h>
#include <esp_mac.h>

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
      _apNoClientStartMs(0),
      _nextRetryMs(0),
      _savedChannel(0),
      _hasSavedBSSID(false),
      _useFastConnect(false) {
    memset(_savedBSSID, 0, sizeof(_savedBSSID));
}

void WiFiManager::begin() {
    _prefs.begin("prox_wifi", false);
    _ssid = _prefs.getString("sta_ssid", "");
    _pass = _prefs.getString("sta_pass", "");
    _savedChannel = _prefs.getInt("sta_chan", 0);
    size_t bssidLen = _prefs.getBytes("sta_bssid", _savedBSSID, 6);
    _hasSavedBSSID = (bssidLen == 6);

    // 核心优化：直接通过 ESP-IDF 硬件层读取芯片物理 MAC
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[32];
    snprintf(buf, sizeof(buf), "ProximityLock-%02X%02X", mac[4], mac[5]);
    _apName = String(buf);
    snprintf(buf, sizeof(buf), "proximitylock-%02x%02x", mac[4], mac[5]);
    _mdnsName = String(buf);

    // 配置中国区国家码 (CN)，开放信道 1~13，完美支持工作在 12/13 高信道的双频合一/现代路由器
    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_AUTO
    };
    esp_wifi_set_country(&country);

    WiFi.setAutoReconnect(true);

    // 关键架构优化：智能区分「配网模式」与「常态开机直连模式」
    // 若已有保存的 Wi-Fi 凭证：开机进入纯 STA 极速直连模式（不开启 SoftAP，100% 释放天线，杜绝信道跳频撕裂）
    // 若未配置 Wi-Fi：直接启动板载 AP 热点供用户首次免输配网
    if (_ssid.length() > 0) {
        WiFi.mode(WIFI_STA);
        _apActive = false;
        startConnectSTA();
    } else {
        Serial.println("[WiFi] 暂无已保存的路由器 Wi-Fi，启动板载热点配网模式.");
        enableAP();
    }
}

void WiFiManager::startConnectSTA() {
    _nextRetryMs = 0;
    // 关键提速：连接建立期间禁用休眠，发射功率拉满，确保握手包与 ACK 100% 准时往返
    WiFi.setSleep(WIFI_PS_NONE);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    if (_savedChannel >= 1 && _savedChannel <= 13 && _hasSavedBSSID) {
        _useFastConnect = true;
        Serial.printf("[WiFi] 启用 Fast-Connect 极速直连: [%s] (定向信道 %d, BSSID %02X:%02X:%02X:%02X:%02X:%02X) ...\n",
                      _ssid.c_str(), _savedChannel,
                      _savedBSSID[0], _savedBSSID[1], _savedBSSID[2],
                      _savedBSSID[3], _savedBSSID[4], _savedBSSID[5]);
        WiFi.begin(_ssid.c_str(), _pass.c_str(), _savedChannel, _savedBSSID);
    } else {
        _useFastConnect = false;
        Serial.printf("[WiFi] 正在全信道扫描连接路由器: %s ...\n", _ssid.c_str());
        WiFi.begin(_ssid.c_str(), _pass.c_str());
    }
    _connecting = true;
    _connectStartMs = millis();
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
                uint32_t connDuration = now - _connectStartMs;

                // 极速直连优化：捕获并持久化当前连接的信道与 BSSID
                int32_t currentChan = WiFi.channel();
                uint8_t* currentBSSID = WiFi.BSSID();
                if (currentChan >= 1 && currentChan <= 13 && currentBSSID != nullptr) {
                    if (currentChan != _savedChannel || !_hasSavedBSSID || memcmp(currentBSSID, _savedBSSID, 6) != 0) {
                        _savedChannel = currentChan;
                        memcpy(_savedBSSID, currentBSSID, 6);
                        _hasSavedBSSID = true;
                        _prefs.putInt("sta_chan", _savedChannel);
                        _prefs.putBytes("sta_bssid", _savedBSSID, 6);
                        Serial.printf("   • [Fast-Connect] 已缓存 Wi-Fi 信道 (%d) 与 BSSID (%02X:%02X:%02X:%02X:%02X:%02X)，下次开机免扫全频秒连!\n",
                                      _savedChannel,
                                      _savedBSSID[0], _savedBSSID[1], _savedBSSID[2],
                                      _savedBSSID[3], _savedBSSID[4], _savedBSSID[5]);
                    }
                }

                // 连接建立后开启轻度 Modem-Sleep，并降低发射功率至 13dBm，兼顾通信并降温
                WiFi.setSleep(WIFI_PS_MIN_MODEM);
                WiFi.setTxPower(WIFI_POWER_13dBm);

                Serial.printf("\n🎉 [WiFi] 成功连接路由器 Wi-Fi: %s! (耗时: %lu ms%s)\n",
                              _ssid.c_str(), connDuration, _useFastConnect ? " [Fast-Connect 极速直连]" : "");
                Serial.printf("   • 内网访问地址: http://%s\n", WiFi.localIP().toString().c_str());
                Serial.printf("   • 信号强度: %d dBm (信道 %d)\n", WiFi.RSSI(), currentChan);
                if (_apActive) {
                    Serial.println("   • 提示: 路由器稳定连接 30 秒后将自动关闭 AP 热点以节省能耗与降温。\n");
                }

                // 启动独立尾号 mDNS 域名: 例如 http://proximitylock-70f8.local (避免同局域网多设备域名冲突)
                if (MDNS.begin(_mdnsName.c_str())) {
                    MDNS.addService("http", "tcp", 80);
                    Serial.printf("   • mDNS 服务已上线: http://%s.local\n\n", _mdnsName.c_str());
                }
            } else if (_useFastConnect && (now - _connectStartMs >= 5000)) {
                // 若路由器变更了信道或更换了 AP 导致 Fast-Connect 5 秒内未连上，立即回退到全信道扫描
                Serial.println("\n⚠️ [WiFi] Fast-Connect 定向信道超时 (路由器可能换信道)，回退至全信道扫描重试...");
                _useFastConnect = false;
                WiFi.disconnect(false);
                WiFi.begin(_ssid.c_str(), _pass.c_str());
                _connectStartMs = now; // 重新给 15s 完整超时窗口
            } else if (now - _connectStartMs >= 15000) {
                _connecting = false;
                _nextRetryMs = now + 180000; // 3 分钟退避休眠
                WiFi.disconnect(false);      // 暂停底层不断全信道狂扫天线
                Serial.printf("\n⚠️ [WiFi] 连接路由器 [%s] 超时. 已启用 3 分钟休眠退避，为 BLE 让出天线并防止发热.\n", _ssid.c_str());
                if (!_apActive) {
                    Serial.println("[WiFi] 自动唤醒 AP 热点，确保配置页面随时可访问...");
                    enableAP();
                }
            }
        } else if (!isConnected() && _ssid.length() > 0 && _nextRetryMs > 0 && now >= _nextRetryMs) {
            _nextRetryMs = 0;
            Serial.printf("\n[WiFi] 退避休眠结束，重新尝试连接路由器 Wi-Fi: %s ...\n", _ssid.c_str());
            startConnectSTA();
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
        WiFi.setSleep(WIFI_PS_MIN_MODEM);
        WiFi.setTxPower(WIFI_POWER_13dBm);
    } else {
        WiFi.mode(WIFI_OFF);
    }
    _apActive = false;
    _apNoClientStartMs = 0;
    _staConnectedMs = 0;
    Serial.println("[WiFi] 板载 AP 热点已关闭 (已彻底关闭 AP 射频，大幅降低发热与干扰).");
}

void WiFiManager::enableAP() {
    if (_apActive) return;
    if (isConnected()) {
        WiFi.mode(WIFI_AP_STA);
    } else {
        WiFi.mode(WIFI_AP);
    }
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(_apName.c_str(), NULL, 1, 0, 4);
    _dnsServer.start(53, "*", apIP);
    _apActive = true;
    _apNoClientStartMs = millis();
    _staConnectedMs = millis();
    Serial.printf("[WiFi] 板载 AP 热点已开启: %s (IP: 192.168.4.1，30秒后将自动关闭)\n", _apName.c_str());
}

void WiFiManager::saveWiFiConfig(const String& ssid, const String& pass) {
    _ssid = ssid;
    _pass = pass;
    _prefs.putString("sta_ssid", _ssid);
    _prefs.putString("sta_pass", _pass);

    // 清除已过期的旧信道缓存
    _prefs.remove("sta_chan");
    _prefs.remove("sta_bssid");
    _savedChannel = 0;
    _hasSavedBSSID = false;
    memset(_savedBSSID, 0, sizeof(_savedBSSID));

    Serial.printf("[WiFi] 写入新 Wi-Fi 凭证: %s, 正在发起连接...\n", _ssid.c_str());
    _nextRetryMs = 0; // 重置退避计时器
    WiFi.disconnect();
    startConnectSTA();
}

void WiFiManager::resetWiFiConfig() {
    _prefs.remove("sta_ssid");
    _prefs.remove("sta_pass");
    _prefs.remove("sta_chan");
    _prefs.remove("sta_bssid");
    _ssid = "";
    _pass = "";
    _savedChannel = 0;
    _hasSavedBSSID = false;
    memset(_savedBSSID, 0, sizeof(_savedBSSID));
    _nextRetryMs = 0;
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
