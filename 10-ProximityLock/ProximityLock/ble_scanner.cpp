#include "ble_scanner.h"
#include <algorithm>

#if !defined(CONFIG_NIMBLE_ENABLED)
#include <esp_gatts_api.h>
#endif

BLETracker Tracker;

class AdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        Tracker.onDeviceScanned(&advertisedDevice);
    }
};

class PhonePairServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {}

#if defined(CONFIG_NIMBLE_ENABLED)
    // ESP32 Arduino Core 3.x (默认基于 NimBLE)
    void onConnect(BLEServer* pServer, ble_gap_conn_desc* desc) override {
        if (!desc) return;
        BLEAddress peerAddr(desc->peer_ota_addr);
        Tracker.onIncomingPhoneConnection(String(peerAddr.toString().c_str()));
    }
#else
    // ESP32 Arduino Core 2.x (基于 Bluedroid)
    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
        if (!param) return;
        char buf[18];
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 param->connect.remote_bda[0], param->connect.remote_bda[1],
                 param->connect.remote_bda[2], param->connect.remote_bda[3],
                 param->connect.remote_bda[4], param->connect.remote_bda[5]);
        Tracker.onIncomingPhoneConnection(String(buf));
    }
#endif

    void onDisconnect(BLEServer* pServer) override {}
};

BLETracker::BLETracker()
    : _pBLEScan(nullptr),
      _pServer(nullptr),
      _hasBoundDevice(false),
      _lastSeenMs(0),
      _lastRawRssi(-999),
      _pairingActive(false),
      _pairingStartMs(0),
      _pairingTimeoutMs(0),
      _bestCandidateRSSI(-999),
      _phonePairModeActive(false),
      _phonePairStartMs(0),
      _phonePairTimeoutMs(0),
      _hasIncomingPair(false),
      _incomingPairMs(0),
      _mux(portMUX_INITIALIZER_UNLOCKED) {}

void BLETracker::begin() {
    _prefs.begin("prox_lock", false);
    _boundMAC = _prefs.getString("target_mac", "");
    _boundName = _prefs.getString("target_name", "");
    _hasBoundDevice = (_boundMAC.length() == 17); // 标准 MAC 长度如 11:22:33:44:55:66

    BLEDevice::init("ProximityLock-配对");

    // 1. 必须先初始化 BLE Server (GATT/GAP 必须在 Scan 启动前就绪，否则在 NimBLE 中会触发 ble_svc_gap_init 断言崩溃)
    _pServer = BLEDevice::createServer();
    _pServer->setCallbacks(new PhonePairServerCallbacks());
    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID("180D");
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);

    // 2. 再初始化并启动 BLE Scan
    _pBLEScan = BLEDevice::getScan();
    _pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), true);
    _pBLEScan->setActiveScan(true);
    _pBLEScan->setInterval(BLE_SCAN_INTERVAL_MS);
    _pBLEScan->setWindow(BLE_SCAN_WINDOW_MS);
    _pBLEScan->start(0, nullptr, false);

    Serial.printf("[BLE] 初始化完成. 绑定状态: %s (%s, %s)\n", 
                  _hasBoundDevice ? "已绑定" : "未绑定", 
                  _hasBoundDevice ? _boundMAC.c_str() : "无",
                  _boundName.length() > 0 ? _boundName.c_str() : "未命名");
}

void BLETracker::clearBond() {
    _prefs.remove("target_mac");
    _prefs.remove("target_name");
    _boundMAC = "";
    _boundName = "";
    _hasBoundDevice = false;
    _filter.reset();
    Serial.println("[BLE] 绑定的设备已解绑清除.");
}

void BLETracker::setBoundDevice(const String& mac, const String& name) {
    if (mac.length() == 17) {
        _boundMAC = mac;
        _boundMAC.toLowerCase();
        _boundName = name;
        _prefs.putString("target_mac", _boundMAC);
        _prefs.putString("target_name", _boundName);
        _hasBoundDevice = true;
        _filter.reset();
        Serial.printf("[BLE] 绑定目标设备: %s (%s)\n", _boundMAC.c_str(), _boundName.c_str());
    } else {
        Serial.println("[BLE] 错误: MAC 地址格式不正确 (应为 xx:xx:xx:xx:xx:xx)");
    }
}

void BLETracker::startPairing(uint32_t timeoutMs) {
    _pairingActive = true;
    _pairingStartMs = millis();
    _pairingTimeoutMs = timeoutMs;
    _bestCandidateMAC = "";
    _bestCandidateName = "";
    _bestCandidateRSSI = -999;
    Serial.printf("[BLE] 进入贴近配对模式 (限时 %lu 秒). 请将手机/手表/手环贴近板子...\n", timeoutMs / 1000);
}

bool BLETracker::checkPairingFinished() {
    if (!_pairingActive) return false;

    if (millis() - _pairingStartMs >= _pairingTimeoutMs) {
        _pairingActive = false;
        if (_bestCandidateRSSI > -65 && _bestCandidateMAC.length() == 17) {
            setBoundDevice(_bestCandidateMAC, _bestCandidateName);
            Serial.printf("[BLE] 配对成功! 捕获到最近目标: %s (%s, 最高 RSSI: %d dBm)\n",
                          _bestCandidateMAC.c_str(), _bestCandidateName.c_str(), _bestCandidateRSSI);
            return true;
        } else {
            Serial.println("[BLE] 配对超时: 未在近距离 (<-65dBm) 检测到明显的主力设备.");
            return false;
        }
    }
    return false;
}

static String identifyBleDevice(BLEAdvertisedDevice* device) {
    if (device->haveName()) {
        String n = device->getName().c_str();
        n.trim();
        if (n.length() > 0) return n;
    }

    // 1. 尝试通过广播厂商数据 (Manufacturer Data) 识别品牌与品类
    if (device->haveManufacturerData()) {
        auto rawMData = device->getManufacturerData();
        const uint8_t* mData = (const uint8_t*)rawMData.c_str();
        size_t len = rawMData.length();
        if (len >= 2) {
            uint16_t companyId = mData[0] | (mData[1] << 8);
            if (companyId == 0x004C) { // Apple Inc.
                if (len >= 3) {
                    uint8_t appleType = mData[2];
                    if (appleType == 0x07) return "🍎 AirPods 耳机";
                    if (appleType == 0x12) return "🍎 AirTag 寻物标";
                    if (appleType == 0x02) return "🍎 Apple iBeacon";
                    if (appleType == 0x10) return "🍎 iPhone / Apple Watch";
                    if (appleType == 0x05) return "🍎 AirDrop 随身设备";
                    if (appleType == 0x09) return "🍎 Apple 随身设备";
                }
                return "🍎 Apple 随身设备 (iPhone/Watch)";
            }
            if (companyId == 0x0157 || companyId == 0x038F) {
                return "⌚ 小米/华米手环 (Amazfit/MiBand)";
            }
            if (companyId == 0x0006) {
                return "💻 微软设备 (Windows PC)";
            }
            if (companyId == 0x027D) {
                return "⌚ 华为智能穿戴 (Huawei)";
            }
            if (companyId == 0x0075) {
                return "📱 三星智能设备 (Samsung)";
            }
            if (companyId == 0x0087) {
                return "⌚ 佳明运动手表 (Garmin)";
            }
            if (companyId == 0x000A) {
                return "🎧 蓝牙音频外设 (Qualcomm)";
            }
            if (companyId == 0x0046) {
                return "🎧 索尼音频外设 (Sony)";
            }
            if (companyId == 0x00E0) {
                return "📶 Google 快速配对信标";
            }
            if (companyId == 0x0665) {
                return "⌚ OPPO/OnePlus 随身设备";
            }
            if (companyId == 0x0618) {
                return "⌚ Vivo 随身设备";
            }
        }
    }

    // 2. 尝试通过标准服务 UUID 识别
    if (device->haveServiceUUID()) {
        BLEUUID uuid = device->getServiceUUID();
        String uuidStr = uuid.toString().c_str();
        uuidStr.toLowerCase();
        if (uuidStr.indexOf("180d") != -1) return "💓 运动心率手环";
        if (uuidStr.indexOf("1812") != -1) return "🖱️ 蓝牙 HID 键鼠外设";
        if (uuidStr.indexOf("fe95") != -1 || uuidStr.indexOf("fe9f") != -1) return "⌚ 小米/米家穿戴设备";
        if (uuidStr.indexOf("fe2c") != -1) return "📱 Android 快速配对设备";
        if (uuidStr.indexOf("fdf7") != -1) return "⌚ 华为智能穿戴";
        if (uuidStr.indexOf("fee0") != -1 || uuidStr.indexOf("fee7") != -1) return "⌚ 智能手环/手表";
    }

    // 3. 检测是否为开启了隐私保护的随身手机/手环 (BLE RPA 随机隐私地址)
    String rawAddr = device->getAddress().toString().c_str();
    if (rawAddr.length() >= 2) {
        uint8_t topByte = (uint8_t)strtoul(rawAddr.substring(0, 2).c_str(), nullptr, 16);
        uint8_t addrType = (topByte >> 6) & 0x03;
        if (addrType == 0x01) {
            return "📱 智能随身设备 (手机/手表)";
        }
    }

    return "BLE 随身外设";
}

void BLETracker::onDeviceScanned(BLEAdvertisedDevice* device) {
    String devMac = device->getAddress().toString().c_str();
    devMac.toLowerCase();
    int rssi = device->getRSSI();

    bool isBoundTarget = (_hasBoundDevice && devMac.equalsIgnoreCase(_boundMAC));

    // 性能优化：快速过滤丢弃超过 1 米的无关环境杂散设备 (不执行任何字符串解析与内存申请)
    if (!isBoundTarget && !_pairingActive && rssi < BLE_DISCOVERY_RSSI_THRESHOLD) {
        return;
    }

    String devName = identifyBleDevice(device);
    uint32_t now = millis();

    // 1. 维护周边设备扫描缓存 (Web UI 列表展示)
    // 严格限制：只收录约 1 米范围内的设备 (RSSI >= BLE_DISCOVERY_RSSI_THRESHOLD，即 -68dBm)
    if (rssi >= BLE_DISCOVERY_RSSI_THRESHOLD || isBoundTarget) {
        portENTER_CRITICAL(&_mux);
        bool found = false;
        for (auto& item : _discoveredDevices) {
            if (item.mac.equalsIgnoreCase(devMac)) {
                item.rssi = rssi;
                item.lastSeenMs = now;
                if (devName.indexOf("外设") == -1 || item.name.length() == 0) {
                    item.name = devName;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            if (_discoveredDevices.size() >= 25) {
                // 剔除超时时间最长的外设
                uint32_t oldestTime = 0xFFFFFFFF;
                size_t oldestIdx = 0;
                for (size_t i = 0; i < _discoveredDevices.size(); ++i) {
                    if (_discoveredDevices[i].lastSeenMs < oldestTime) {
                        oldestTime = _discoveredDevices[i].lastSeenMs;
                        oldestIdx = i;
                    }
                }
                _discoveredDevices.erase(_discoveredDevices.begin() + oldestIdx);
            }
            DiscoveredDevice newDev;
            newDev.mac = devMac;
            newDev.name = devName;
            newDev.rssi = rssi;
            newDev.lastSeenMs = now;
            _discoveredDevices.push_back(newDev);
        }
        portEXIT_CRITICAL(&_mux);
    }

    // 2. 配对模式：寻找 RSSI 最强的设备 (贴在旁边的设备信号通常高于 -55dBm)
    if (_pairingActive) {
        if (rssi > _bestCandidateRSSI) {
            _bestCandidateRSSI = rssi;
            _bestCandidateMAC = devMac;
            _bestCandidateName = devName;
            Serial.printf("[BLE-配对候选] %s (%s) | RSSI: %d dBm\n", devMac.c_str(), devName.c_str(), rssi);
        }
        return;
    }

    // 3. 追踪模式：匹配已绑定的 MAC
    if (_hasBoundDevice && devMac.equalsIgnoreCase(_boundMAC)) {
        _lastSeenMs = now;
        _lastRawRssi = rssi;
        _filter.update((float)rssi);
        if (_boundName.length() == 0 && devName.length() > 0) {
            _boundName = devName;
            _prefs.putString("target_name", _boundName);
        }
    }
}

std::vector<DiscoveredDevice> BLETracker::getDiscoveredDevices() {
    uint32_t now = millis();
    std::vector<DiscoveredDevice> copy;
    copy.reserve(25);
    portENTER_CRITICAL(&_mux);
    // 只返回 1 米以内 (>= -68 dBm) 且在最近 20 秒内活跃的设备
    for (auto it = _discoveredDevices.begin(); it != _discoveredDevices.end(); ) {
        bool isBound = (_hasBoundDevice && it->mac.equalsIgnoreCase(_boundMAC));
        if (!isBound && (it->rssi < BLE_DISCOVERY_RSSI_THRESHOLD || now - it->lastSeenMs > 20000)) {
            it = _discoveredDevices.erase(it);
        } else {
            copy.push_back(*it);
            ++it;
        }
    }
    portEXIT_CRITICAL(&_mux);

    // 按 RSSI 降序排序（越近排越前）
    std::sort(copy.begin(), copy.end(), [](const DiscoveredDevice& a, const DiscoveredDevice& b) {
        return a.rssi > b.rssi;
    });
    return copy;
}

void BLETracker::clearScanCache() {
    portENTER_CRITICAL(&_mux);
    _discoveredDevices.clear();
    portEXIT_CRITICAL(&_mux);
}

void BLETracker::pauseScan() {
    if (_pBLEScan) {
        _pBLEScan->stop();
    }
}

void BLETracker::resumeScan() {
    if (_pBLEScan) {
        _pBLEScan->clearResults();
        _pBLEScan->start(0, nullptr, false);
    }
}

void BLETracker::startPhonePairMode(uint32_t timeoutMs) {
    _phonePairModeActive = true;
    _phonePairStartMs = millis();
    _phonePairTimeoutMs = timeoutMs;
    _hasIncomingPair = false;
    _incomingPairMAC = "";
    BLEDevice::startAdvertising();
    Serial.printf("[BLE] 开启手机主动连接配对广播 (名称: ProximityLock-配对, 限时 %lu 秒)...\n", timeoutMs / 1000);
}

void BLETracker::stopPhonePairMode() {
    if (_phonePairModeActive) {
        _phonePairModeActive = false;
        _hasIncomingPair = false;
        _incomingPairMAC = "";
        BLEDevice::getAdvertising()->stop();
        Serial.println("[BLE] 手机连接配对广播已关闭.");
    }
}

uint32_t BLETracker::getPhonePairRemainSec() const {
    if (!_phonePairModeActive) return 0;
    uint32_t elapsed = millis() - _phonePairStartMs;
    if (elapsed >= _phonePairTimeoutMs) return 0;
    return (_phonePairTimeoutMs - elapsed) / 1000;
}

void BLETracker::onIncomingPhoneConnection(const String& mac) {
    if (!_phonePairModeActive) return;
    _hasIncomingPair = true;
    _incomingPairMAC = mac;
    _incomingPairMAC.toLowerCase();
    _incomingPairMs = millis();
    Serial.printf("\n🔔 [BLE] 收到手机主动连接请求! 物理 MAC: %s\n", _incomingPairMAC.c_str());
}

void BLETracker::confirmIncomingPair(const String& name) {
    if (_incomingPairMAC.length() == 17) {
        String finalName = name.length() > 0 ? name : "我的随身手机";
        setBoundDevice(_incomingPairMAC, finalName);
        Serial.printf("[BLE] 确认绑定手机: %s (%s)\n", _incomingPairMAC.c_str(), finalName.c_str());
    }
    stopPhonePairMode();
}

void BLETracker::rejectIncomingPair() {
    _hasIncomingPair = false;
    _incomingPairMAC = "";
    Serial.println("[BLE] 已忽略/拒绝手机连接配对请求.");
}

void BLETracker::loop() {
    uint32_t now = millis();

    // 关键优化：每 3 秒清空一次底层 BLEScan 堆积的扫描设备结果集
    // 避免长时间连续扫描时底层 m_scanResults 累积大量周边设备导致堆内存耗尽 (OOM 崩溃: operator new std::bad_alloc)
    static uint32_t lastClearMs = 0;
    if (_pBLEScan && (now - lastClearMs >= 3000)) {
        lastClearMs = now;
        _pBLEScan->stop();
        _pBLEScan->clearResults();
        _pBLEScan->start(0, nullptr, false);
    }

    // 监控手机主动连接配对超时
    if (_phonePairModeActive) {
        if (now - _phonePairStartMs >= _phonePairTimeoutMs) {
            stopPhonePairMode();
        }
    }
}
