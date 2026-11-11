#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

class WiFiManager {
public:
    WiFiManager();
    void begin();
    void loop();

    bool isConnected() const;
    bool isConnecting() const { return _connecting; }
    bool isAPActive() const { return _apActive; }
    uint32_t getAPShutdownRemainSec() const;

    String getSTAIP() const;
    String getAPIP() const;
    String getSSID() const { return _ssid; }
    int    getRSSI() const;
    String getAPName() const { return _apName; }
    String getMDNSName() const { return _mdnsName; }

    void saveWiFiConfig(const String& ssid, const String& pass);
    void resetWiFiConfig();
    void processDNS();
    void enableAP();
    void disableAP();

private:
    Preferences _prefs;
    DNSServer   _dnsServer;
    String      _ssid;
    String      _pass;
    String      _apName;
    String      _mdnsName;
    bool        _apActive;
    bool        _connecting;
    uint32_t    _connectStartMs;
    uint32_t    _lastCheckMs;
    uint32_t    _staConnectedMs;
    bool        _staWasConnected;
    uint32_t    _apNoClientStartMs;
    uint32_t    _nextRetryMs;

    // Fast-Connect 快速直连缓存 (免扫 1~13 全信道，秒级直达路由器)
    int32_t     _savedChannel;
    uint8_t     _savedBSSID[6];
    bool        _hasSavedBSSID;
    bool        _useFastConnect;

    void        startConnectSTA();
};

extern WiFiManager WiFiMgr;
