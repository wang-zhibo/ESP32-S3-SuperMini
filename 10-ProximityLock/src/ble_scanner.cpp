#include "ble_scanner.h"

BLETracker Tracker;

class AdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        Tracker.onDeviceScanned(&advertisedDevice);
    }
};

BLETracker::BLETracker()
    : _pBLEScan(nullptr),
      _hasBoundDevice(false),
      _lastSeenMs(0),
      _lastRawRssi(-999),
      _pairingActive(false),
      _pairingStartMs(0),
      _pairingTimeoutMs(0),
      _bestCandidateRSSI(-999) {}

void BLETracker::begin() {
    _prefs.begin("prox_lock", false);
    _boundMAC = _prefs.getString("target_mac", "");
    _hasBoundDevice = (_boundMAC.length() == 17); // 标准 MAC 长度如 11:22:33:44:55:66

    BLEDevice::init("S3-ProximityLock");
    _pBLEScan = BLEDevice::getScan();
    _pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), true);
    _pBLEScan->setActiveScan(true);
    _pBLEScan->setInterval(BLE_SCAN_INTERVAL_MS);
    _pBLEScan->setWindow(BLE_SCAN_WINDOW_MS);

    Serial.printf("[BLE] 初始化完成. 绑定状态: %s (%s)\n", 
                  _hasBoundDevice ? "已绑定" : "未绑定", 
                  _hasBoundDevice ? _boundMAC.c_str() : "无");
}

void BLETracker::clearBond() {
    _prefs.remove("target_mac");
    _boundMAC = "";
    _hasBoundDevice = false;
    _filter.reset();
    Serial.println("[BLE] 绑定的设备已解绑清除.");
}

void BLETracker::setBoundMAC(const String& mac) {
    if (mac.length() == 17) {
        _boundMAC = mac;
        _boundMAC.toLowerCase();
        _prefs.putString("target_mac", _boundMAC);
        _hasBoundDevice = true;
        _filter.reset();
        Serial.printf("[BLE] 手动设置目标设备 MAC: %s\n", _boundMAC.c_str());
    } else {
        Serial.println("[BLE] 错误: MAC 地址格式不正确 (应为 xx:xx:xx:xx:xx:xx)");
    }
}

void BLETracker::startPairing(uint32_t timeoutMs) {
    _pairingActive = true;
    _pairingStartMs = millis();
    _pairingTimeoutMs = timeoutMs;
    _bestCandidateMAC = "";
    _bestCandidateRSSI = -999;
    Serial.printf("[BLE] 进入贴近配对模式 (限时 %lu 秒). 请将手机/手表/手环贴近板子...\n", timeoutMs / 1000);
}

bool BLETracker::checkPairingFinished() {
    if (!_pairingActive) return false;

    if (millis() - _pairingStartMs >= _pairingTimeoutMs) {
        _pairingActive = false;
        if (_bestCandidateRSSI > -65 && _bestCandidateMAC.length() == 17) {
            setBoundMAC(_bestCandidateMAC);
            Serial.printf("[BLE] 配对成功! 捕获到最近目标: %s (最高 RSSI: %d dBm)\n",
                          _bestCandidateMAC.c_str(), _bestCandidateRSSI);
            return true;
        } else {
            Serial.println("[BLE] 配对超时: 未在近距离 (<-65dBm) 检测到明显的主力设备.");
            return false;
        }
    }
    return false;
}

void BLETracker::onDeviceScanned(BLEAdvertisedDevice* device) {
    String devMac = device->getAddress().toString().c_str();
    devMac.toLowerCase();
    int rssi = device->getRSSI();

    // 配对模式：寻找 RSSI 最强的设备 (贴在旁边的设备信号通常高于 -55dBm)
    if (_pairingActive) {
        if (rssi > _bestCandidateRSSI) {
            _bestCandidateRSSI = rssi;
            _bestCandidateMAC = devMac;
            Serial.printf("[BLE-配对候选] %s | RSSI: %d dBm\n", devMac.c_str(), rssi);
        }
        return;
    }

    // 追踪模式：匹配已绑定的 MAC
    if (_hasBoundDevice && devMac.equalsIgnoreCase(_boundMAC)) {
        _lastSeenMs = millis();
        _lastRawRssi = rssi;
        _filter.update((float)rssi);
    }
}

void BLETracker::loop() {
    // 定期重启扫描避免缓存堆积溢出，确保高响应度
    if (!_pBLEScan->isScanning()) {
        _pBLEScan->clearResults();
        _pBLEScan->start(1, false);
    }
}
