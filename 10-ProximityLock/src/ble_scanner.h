#pragma once
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include "filter.h"
#include "config.h"

class BLETracker {
public:
    BLETracker();
    void begin();
    
    // 配对与绑定管理
    bool isPaired() const { return _hasBoundDevice; }
    String getBoundMAC() const { return _boundMAC; }
    void clearBond();
    void startPairing(uint32_t timeoutMs = 8000);
    bool checkPairingFinished();

    // 手动指定绑定 MAC
    void setBoundMAC(const String& mac);

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
    void onDeviceScanned(BLEAdvertisedDevice* device);

    Preferences    _prefs;
    BLEScan*       _pBLEScan;
    RSSIFilter     _filter;

    bool           _hasBoundDevice;
    String         _boundMAC;
    uint32_t       _lastSeenMs;
    int            _lastRawRssi;

    // 配对模式临时状态
    bool           _pairingActive;
    uint32_t       _pairingStartMs;
    uint32_t       _pairingTimeoutMs;
    String         _bestCandidateMAC;
    int            _bestCandidateRSSI;
};

extern BLETracker Tracker;
