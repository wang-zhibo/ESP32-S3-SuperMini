#pragma once
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <vector>
#include "filter.h"
#include "config.h"

struct DiscoveredDevice {
    String   mac;
    String   name;
    int      rssi;
    uint32_t lastSeenMs;
};

class BLETracker {
public:
    BLETracker();
    void begin();
    
    // 配对与绑定管理
    bool isPaired() const { return _hasBoundDevice; }
    String getBoundMAC() const { return _boundMAC; }
    String getBoundName() const { return _boundName; }
    void clearBond();
    void startPairing(uint32_t timeoutMs = 8000);
    bool checkPairingFinished();

    // 手机主动连接配对管理
    void startPhonePairMode(uint32_t timeoutMs = 60000);
    void stopPhonePairMode();
    bool isPhonePairModeActive() const { return _phonePairModeActive; }
    uint32_t getPhonePairRemainSec() const;
    bool hasIncomingPairRequest() const { return _hasIncomingPair; }
    String getIncomingPairMAC() const { return _incomingPairMAC; }
    void confirmIncomingPair(const String& name = "");
    void rejectIncomingPair();
    void onIncomingPhoneConnection(const String& mac);

    // 手动或 Web 指定绑定设备
    void setBoundDevice(const String& mac, const String& name = "");

    // 扫描结果列表查询
    std::vector<DiscoveredDevice> getDiscoveredDevices();
    void clearScanCache();

    // 临时暂停/恢复 BLE 扫描 (为 Wi-Fi 扫描让出 2.4GHz 射频资源)
    void pauseScan();
    void resumeScan();

    // 追踪与距离状态查询
    void loop();
    float getFilteredRSSI() const { return _filter.get(); }
    int   getLastRawRSSI() const { return _lastRawRssi; }
    uint32_t getLastSeenElapsed() const { return millis() - _lastSeenMs; }
    bool  isSignalAlive(uint32_t timeoutMs) const {
        return (_hasBoundDevice && (millis() - _lastSeenMs < timeoutMs));
    }

private:
    friend class AdvertisedDeviceCallbacks;
    friend class PhonePairServerCallbacks;
    void onDeviceScanned(BLEAdvertisedDevice* device);

    Preferences    _prefs;
    BLEScan*       _pBLEScan;
    BLEServer*     _pServer;
    RSSIFilter     _filter;

    bool           _hasBoundDevice;
    String         _boundMAC;
    String         _boundName;
    uint32_t       _lastSeenMs;
    int            _lastRawRssi;

    // 贴近配对模式临时状态
    bool           _pairingActive;
    uint32_t       _pairingStartMs;
    uint32_t       _pairingTimeoutMs;
    String         _bestCandidateMAC;
    String         _bestCandidateName;
    int            _bestCandidateRSSI;

    // 手机主动连接配对状态
    bool           _phonePairModeActive;
    uint32_t       _phonePairStartMs;
    uint32_t       _phonePairTimeoutMs;
    bool           _hasIncomingPair;
    String         _incomingPairMAC;
    uint32_t       _incomingPairMs;

    // 缓存周边发现的活跃外设
    std::vector<DiscoveredDevice> _discoveredDevices;
    portMUX_TYPE   _mux;
};

extern BLETracker Tracker;
