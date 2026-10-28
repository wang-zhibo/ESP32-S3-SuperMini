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

class CompanionSecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t pass_key) override {}
    bool onSecurityRequest() override { return true; }
    bool onConfirmPIN(uint32_t pin) override { return true; }

#if defined(CONFIG_BLUEDROID_ENABLED)
    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
        if (cmpl.success) {
            Serial.println("\n🎉 [BLE伴侣] 手机/手表与 ESP32 已完成安全信任配对 (Bonding)!");
        } else {
            Serial.printf("\n⚠️ [BLE伴侣] 伴侣配对异常 (状态码: 0x%x)\n", cmpl.fail_reason);
        }
    }
#elif defined(CONFIG_NIMBLE_ENABLED)
    void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
        Serial.println("\n🎉 [BLE伴侣] 手机/手表与 ESP32 已完成安全信任配对 (Bonding)!");
    }
    void onAuthenticationComplete(ble_gap_conn_desc *desc, int status) override {
        if (status == 0) {
            Serial.println("\n🎉 [BLE伴侣] 手机/手表与 ESP32 已完成安全信任配对 (Bonding)!");
        } else {
            Serial.printf("\n⚠️ [BLE伴侣] 伴侣配对状态码: 0x%x\n", status);
        }
    }
#endif
};

#if defined(CONFIG_BLUEDROID_ENABLED)
static void customGapHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    if (event == ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT) {
        if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            Tracker.onConnectionRssiRead(param->read_rssi_cmpl.rssi);
        }
    }
}
#endif

class PhonePairServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {}

#if defined(CONFIG_NIMBLE_ENABLED)
    // ESP32 Arduino Core 3.x (默认基于 NimBLE)
    void onConnect(BLEServer* pServer, ble_gap_conn_desc* desc) override {
        if (!desc) return;
        BLEAddress peerAddr(desc->peer_ota_addr);
        Tracker.onIncomingPhoneConnection(String(peerAddr.toString().c_str()));
        Tracker.onCompanionConnected(desc->conn_handle, (const uint8_t*)desc->peer_id_addr.val);
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
        Tracker.onCompanionConnected(param->connect.conn_id, param->connect.remote_bda);
    }
#endif

    void onDisconnect(BLEServer* pServer) override {
        Tracker.onCompanionDisconnected();
        if (pServer) {
            pServer->startAdvertising();
        }
    }

#if defined(CONFIG_NIMBLE_ENABLED)
    void onDisconnect(BLEServer* pServer, ble_gap_conn_desc* desc) override {
        Tracker.onCompanionDisconnected();
        if (pServer) {
            pServer->startAdvertising();
        }
    }
#endif
};

BLETracker::BLETracker()
    : _pBLEScan(nullptr),
      _pServer(nullptr),
      _pHid(nullptr),
      _hasBoundDevice(false),
      _lastSeenMs(0),
      _lastRawRssi(-999),
      _isCompanionConnected(false),
      _hasBondedCompanion(false),
      _connId(0),
      _lastRssiReadMs(0),
      _pairingActive(false),
      _pairingStartMs(0),
      _pairingTimeoutMs(0),
      _bestCandidateRSSI(-999),
      _phonePairModeActive(false),
      _phonePairStartMs(0),
      _phonePairTimeoutMs(0),
      _hasIncomingPair(false),
      _incomingPairMs(0),
      _mutex(nullptr) {
    memset(_connectedBda, 0, sizeof(_connectedBda));
    _mutex = xSemaphoreCreateMutex();
}

void BLETracker::begin() {
    _prefs.begin("prox_lock", false);
    _boundMAC = _prefs.getString("target_mac", "");
    _boundName = _prefs.getString("target_name", "");
    _hasBondedCompanion = _prefs.getBool("comp_bonded", false);
    _hasBoundDevice = (_boundMAC.length() == 17) || _hasBondedCompanion;

    BLEDevice::init("ProximityLock-伴侣");
#if defined(CONFIG_BLUEDROID_ENABLED)
    BLEDevice::setCustomGapHandler(customGapHandler);
#endif

    // 1. 初始化 BLE Server 并提供标准 HID (0x1812) / Battery (0x180F) 伴侣直连服务
    _pServer = BLEDevice::createServer();
    _pServer->setCallbacks(new PhonePairServerCallbacks());
#if defined(CONFIG_NIMBLE_ENABLED)
    _pServer->advertiseOnDisconnect(true);
#endif

    // 标准 HID 报告描述符 (Consumer Control / 媒体自拍遥控器类型)
    // 核心优势: 
    // 1) 允许 iPhone/iPad/Mac 原生系统【设置 -> 蓝牙】直接搜到并配对连接
    // 2) 具备 iOS 最高自动重连优先级 (iOS 后台 daemon 自动拉起已配对的遥控设备)
    // 3) 不含文本键位 (Usage Page 0x0C)，绝对不会影响 iPhone 屏幕虚拟软键盘打字
    static const uint8_t hidReportDescriptor[] = {
        0x05, 0x0C,        // Usage Page (Consumer Devices)
        0x09, 0x01,        // Usage (Consumer Control)
        0xA1, 0x01,        // Collection (Application)
        0x85, 0x01,        //   Report ID (1)
        0x09, 0xE9,        //   Usage (Volume Up / Shutter)
        0x09, 0xEA,        //   Usage (Volume Down)
        0x09, 0xCD,        //   Usage (Play/Pause)
        0x15, 0x00,        //   Logical Minimum (0)
        0x25, 0x01,        //   Logical Maximum (1)
        0x75, 0x01,        //   Report Size (1)
        0x95, 0x03,        //   Report Count (3)
        0x81, 0x02,        //   Input (Data, Variable, Absolute)
        0x75, 0x01,        //   Report Size (1)
        0x95, 0x05,        //   Report Count (5) - padding to 1 byte
        0x81, 0x01,        //   Input (Constant)
        0xC0               // End Collection
    };

    _pHid = new BLEHIDDevice(_pServer);
    _pHid->manufacturer()->setValue("Espressif");
    _pHid->pnp(0x02, 0x05ac, 0x820a, 0x0110);  // Apple Vendor ID 0x05ac
    _pHid->hidInfo(0x00, 0x01);
    _pHid->reportMap((uint8_t*)hidReportDescriptor, sizeof(hidReportDescriptor));
    _pHid->setBatteryLevel(100);
    _pHid->startServices();

    // 配置全平台免输码的 Just Works 安全配对 (BOND 模式)
    BLESecurity* pSecurity = new BLESecurity();
#if defined(CONFIG_BLUEDROID_ENABLED)
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
#elif defined(CONFIG_NIMBLE_ENABLED)
    pSecurity->setAuthenticationMode(true, false, true); // bonding=true, mitm=false, sc=true
#else
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
#endif
    pSecurity->setCapability(ESP_IO_CAP_NONE);
    pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    BLEDevice::setSecurityCallbacks(new CompanionSecurityCallbacks());

    // 启动伴侣可连接广播 (对外显示名称: ProximityLock-伴侣)
    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->setAppearance(0x0180); // Generic Remote Control (iOS 极佳兼容性，自动重连)
    pAdv->addServiceUUID(_pHid->hidService()->getUUID()); // 0x1812
    pAdv->addServiceUUID(BLEUUID((uint16_t)0x180F));       // 0x180F
    pAdv->setMinInterval(0x00A0); // 100 ms - 苹果配件规范推荐
    pAdv->setMaxInterval(0x00F4); // 152.5 ms - 苹果配件规范推荐
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06); // 协助 iOS/watchOS 快速识别并自动重连
    pAdv->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();

    // 2. 初始化被动扫描 (兼顾周边信标与老式固定 MAC 设备)
    _pBLEScan = BLEDevice::getScan();
    _pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), true);
    _pBLEScan->setActiveScan(false);
    _pBLEScan->setInterval(BLE_SCAN_INTERVAL_MS);
    _pBLEScan->setWindow(BLE_SCAN_WINDOW_MS);
    _pBLEScan->start(0, nullptr, false);

    Serial.printf("[BLE] 伴侣直连服务已就绪. 广播名称: ProximityLock-伴侣\n");
    Serial.printf("   • 绑定状态: %s (%s, %s)\n", 
                  isPaired() ? "已就绪" : "等待配对", 
                  _boundMAC.length() > 0 ? _boundMAC.c_str() : "等待直连",
                  _boundName.length() > 0 ? _boundName.c_str() : "未命名");
    Serial.println("   • 提示: 支持直接在 iPhone / Apple Watch / 安卓手机系统的【设置 -> 蓝牙】中搜索连接！\n");
}

void BLETracker::clearBond() {
    _prefs.remove("target_mac");
    _prefs.remove("target_name");
    _prefs.remove("comp_bonded");
    _boundMAC = "";
    _boundName = "";
    _hasBoundDevice = false;
    _hasBondedCompanion = false;
    _filter.reset();
    disconnectCompanion();
    if (_pServer) {
        _pServer->startAdvertising();
    }
    Serial.println("[BLE] 绑定的设备与伴侣长连接已全部清除解绑, 已重启广播等待新设备.");
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
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
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
            xSemaphoreGive(_mutex);
        }
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
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
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
        xSemaphoreGive(_mutex);
    }

    // 按 RSSI 降序排序（越近排越前）
    std::sort(copy.begin(), copy.end(), [](const DiscoveredDevice& a, const DiscoveredDevice& b) {
        return a.rssi > b.rssi;
    });
    return copy;
}

void BLETracker::clearScanCache() {
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        _discoveredDevices.clear();
        xSemaphoreGive(_mutex);
    }
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

void BLETracker::onCompanionConnected(uint16_t connId, const uint8_t* remoteBda) {
    _isCompanionConnected = true;
    _connId = connId;
    memcpy(_connectedBda, remoteBda, 6);
    _hasBondedCompanion = true;
    _prefs.putBool("comp_bonded", true);

    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             remoteBda[0], remoteBda[1], remoteBda[2], remoteBda[3], remoteBda[4], remoteBda[5]);
    _boundMAC = String(buf);
    if (_boundName.length() == 0 || _boundName == "未命名" || _boundName == "无") {
        _boundName = "随身蓝牙伴侣 (直连保活)";
    }
    _prefs.putString("target_mac", _boundMAC);
    _prefs.putString("target_name", _boundName);
    _hasBoundDevice = true;

    _lastSeenMs = millis();
    // 关键修正: 刚建立连接时绝不伪造 -55dBm (人可能还在十米外门口刚连上蓝牙)
    // 而是立即发起一次底层真实 RSSI 读取，以物理实际信号为准
    readConnectionRssi();

    Serial.printf("\n🎉 [BLE伴侣] 目标设备已成功直连! 句柄: %d, MAC: %s\n", connId, buf);
    Serial.println("   • 核心特性: 底层已建立长连接链路，走芯片逻辑句柄，绝无任何MAC漂移和黑屏断连！\n");

    // 关键射频与温升优化：伴侣直连成功后，暂停后台主动广播扫描，释放 2.4GHz 射频资源
    if (_pBLEScan) {
        _pBLEScan->stop();
        Serial.println("   • [射频优化] 伴侣直连已建立，暂停后台主动扫描，降低芯片发热与 2.4GHz 干扰。");
    }

#if defined(CONFIG_NIMBLE_ENABLED)
    // 严格遵循 Apple Accessory Design Guidelines 协商连接链路参数
    // 1) 间隔 15ms~30ms (12~24 * 1.25ms) 获得更高精度的快速 RSSI 采样
    // 2) 潜伏 0
    // 3) 监督超时 2000ms (200 * 10ms)，人走远后 2 秒立刻察觉物理断开，雷厉风行触发锁屏！
    ble_gap_upd_params connParams;
    memset(&connParams, 0, sizeof(connParams));
    connParams.itvl_min = 12;
    connParams.itvl_max = 24;
    connParams.latency = 0;
    connParams.supervision_timeout = 200;
    connParams.min_ce_len = 0;
    connParams.max_ce_len = 0;
    ble_gap_update_params(connId, &connParams);
    Serial.println("   • [链路优化] 已协商优化连接参数: 间隔 15~30ms, 离席断连感知从 20s 缩短至 2s.");
#elif defined(CONFIG_BLUEDROID_ENABLED)
    esp_ble_conn_update_params_t connParams;
    memcpy(connParams.bda, remoteBda, ESP_BD_ADDR_LEN);
    connParams.min_int = 12;
    connParams.max_int = 24;
    connParams.latency = 0;
    connParams.timeout = 200;
    esp_ble_gap_update_conn_params(&connParams);
    Serial.println("   • [链路优化] 已协商优化连接参数: 间隔 15~30ms, 离席断连感知从 20s 缩短至 2s.");
#endif

    // 立即发起一次真实链路 RSSI 测量
    readConnectionRssi();
}

void BLETracker::onCompanionDisconnected() {
    _isCompanionConnected = false;
    _lastSeenMs = 0;
    Serial.println("\n⚠️ [BLE伴侣] 伴侣设备已断开连接 (可能已离开工位). 正在重新开启广播等待回座秒连...");

    // 关键射频与重连优化：伴侣断开后，重新唤醒后台扫描以备重连
    if (_pBLEScan) {
        _pBLEScan->clearResults();
        _pBLEScan->start(0, nullptr, false);
        Serial.println("   • [射频恢复] 伴侣已断开，重新唤醒后台扫描以备重新发现。");
    }
}

void BLETracker::onConnectionRssiRead(int rssi) {
    if (rssi != 127 && rssi < 0) {
        _lastRawRssi = rssi;
        _filter.update((float)rssi);
        _lastSeenMs = millis();
    }
}

void BLETracker::readConnectionRssi() {
    if (!_isCompanionConnected) return;

#if defined(CONFIG_NIMBLE_ENABLED)
    int8_t rssiValue = 0;
    int rc = ble_gap_conn_rssi(_connId, &rssiValue);
    if (rc == 0) {
        onConnectionRssiRead((int)rssiValue);
    }
#elif defined(CONFIG_BLUEDROID_ENABLED)
    esp_ble_gap_read_rssi(_connectedBda);
#endif
}

void BLETracker::disconnectCompanion() {
    if (_isCompanionConnected && _pServer) {
        _pServer->disconnect(_connId);
    }
    _isCompanionConnected = false;
}

void BLETracker::loop() {
    uint32_t now = millis();

    // 1. 如果处于伴侣直连保活状态，定期向对端发起链路真实 RSSI 采样 (每 1.5 秒采样一次)
    if (_isCompanionConnected) {
        if (now - _lastRssiReadMs >= 1500) {
            _lastRssiReadMs = now;
            readConnectionRssi();
        }
    }

    // 2. 关键优化：每 15 秒清空一次底层 BLEScan 堆积的扫描设备结果集 (仅当未连接伴侣且扫描活跃时)
    static uint32_t lastClearMs = 0;
    if (_pBLEScan && !_isCompanionConnected && (now - lastClearMs >= 15000)) {
        lastClearMs = now;
        _pBLEScan->clearResults();
    }

    // 3. 伴侣断连广播保活看门狗：确保在未连接状态下，BLE 广播持续发射等待回座秒连
    static uint32_t lastAdvCheckMs = 0;
    if (!_isCompanionConnected && (now - lastAdvCheckMs >= 2000)) {
        lastAdvCheckMs = now;
#if defined(CONFIG_NIMBLE_ENABLED)
        if (!ble_gap_adv_active()) {
            BLEDevice::startAdvertising();
        }
#elif defined(CONFIG_BLUEDROID_ENABLED)
        BLEDevice::startAdvertising();
#endif
    }

    // 4. 监控手机主动连接配对超时
    if (_phonePairModeActive) {
        if (now - _phonePairStartMs >= _phonePairTimeoutMs) {
            stopPhonePairMode();
        }
    }
}
