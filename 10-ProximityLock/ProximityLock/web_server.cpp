#include "web_server.h"
#include "web_html.h"
#include "wifi_manager.h"
#include "ble_scanner.h"
#include "hid_locker.h"
#include "config.h"
#include <Update.h>

WebService WebServ;

static String s_otaErrorMsg = "";
static bool   s_otaHeaderChecked = false;

WebService::WebService() : _server(80) {}

static void sendJsonHeaders(WebServer& server) {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.sendHeader("Access-Control-Allow-Origin", "*");
}

void WebService::begin() {
    registerRoutes();
    _server.begin();
    Serial.println("[Web] HTTP Web 管理后台已启动在端口 80.");
}

void WebService::loop() {
    _server.handleClient();
}

void WebService::registerRoutes() {
    // 1. 首页 (Gzip 高度压缩 PROGMEM 直出)
    _server.on("/", HTTP_GET, [this]() {
        _server.sendHeader("Content-Encoding", "gzip");
        _server.send_P(200, "text/html; charset=utf-8", (const char*)htmlPageGz, htmlPageGz_len);
    });

    // 2. 移动端强制门户认证探针重定向 (Captive Portal)
    const char* captiveEndpoints[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html", 
        "/connecttest.txt", "/ncsi.txt", "/canonical.html"
    };
    for (const char* endpoint : captiveEndpoints) {
        _server.on(endpoint, HTTP_GET, [this]() {
            _server.sendHeader("Location", "http://192.168.4.1/", true);
            _server.send(302, "text/plain", "");
        });
    }

    // 3. 设备状态与实时信号接口
    _server.on("/api/status", HTTP_GET, [this]() {
        sendJsonHeaders(_server);

        uint32_t now = millis();
        uint32_t pauseRemainSec = 0;
        if (g_state == STATE_PAUSED) {
            uint32_t elapsed = now - g_pauseStartMs;
            if (elapsed < DEFAULT_PAUSE_DURATION_MS) {
                pauseRemainSec = (DEFAULT_PAUSE_DURATION_MS - elapsed) / 1000;
            }
        }

        const char* osNames[] = {"Windows (Win+L)", "macOS (Cmd+Ctrl+Q)", "Linux (Ctrl+Alt+L)"};
        const char* stateNames[] = {"未绑定", "贴近配对中", "在场安全监护", "离开预警", "远离已锁屏", "临时暂停免打扰"};

        String json;
        json.reserve(1024); // 内存性能优化：单次预分配，杜绝 1 秒一次的频繁 realloc 堆碎片
        json = "{";
        json += "\"state\":" + String((int)g_state) + ",";
        json += "\"state_str\":\"" + String(stateNames[g_state]) + "\",";
        json += "\"os_mode\":" + String((int)g_osMode) + ",";
        json += "\"os_str\":\"" + String(osNames[g_osMode]) + "\",";
        json += "\"bound_mac\":\"" + Tracker.getBoundMAC() + "\",";
        json += "\"bound_name\":\"" + Tracker.getBoundName() + "\",";
        json += "\"current_rssi\":" + String(Tracker.getFilteredRSSI(), 1) + ",";
        json += "\"raw_rssi\":" + String(Tracker.getLastRawRSSI()) + ",";
        json += "\"last_seen_sec\":" + String(Tracker.getLastSeenElapsed() / 1000) + ",";
        json += "\"lock_rssi\":" + String(g_lockRSSI) + ",";
        json += "\"unlock_rssi\":" + String(g_unlockRSSI) + ",";
        json += "\"timeout_ms\":" + String(g_leaveTimeoutMs) + ",";
        json += "\"is_paused\":" + String(g_state == STATE_PAUSED ? "true" : "false") + ",";
        json += "\"pause_remain_sec\":" + String(pauseRemainSec) + ",";
        json += "\"uptime_sec\":" + String(now / 1000) + ",";
        json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\",";
        json += "\"cpu_temp\":" + String(temperatureRead(), 1) + ",";
        json += "\"free_heap_kb\":" + String(ESP.getFreeHeap() / 1024) + ",";
        json += "\"has_unlock_pwd\":" + String(g_unlockPassword.length() > 0 ? "true" : "false") + ",";
        json += "\"auto_unlock\":" + String(g_autoUnlockEnabled ? "true" : "false") + ",";
        json += "\"usb_hid_ready\":" + String(Locker.isTinyUSBEnabled() ? "true" : "false") + ",";
        json += "\"companion_connected\":" + String(Tracker.isCompanionConnected() ? "true" : "false") + ",";
        json += "\"companion_bonded\":" + String(Tracker.hasBondedCompanion() ? "true" : "false") + ",";
        
        json += "\"incoming_pair\":{";
        json += "\"active\":" + String(Tracker.isPhonePairModeActive() ? "true" : "false") + ",";
        json += "\"has_request\":" + String(Tracker.hasIncomingPairRequest() ? "true" : "false") + ",";
        json += "\"mac\":\"" + Tracker.getIncomingPairMAC() + "\",";
        json += "\"remain_sec\":" + String(Tracker.getPhonePairRemainSec());
        json += "},";

        json += "\"wifi\":{";
        json += "\"connected\":" + String(WiFiMgr.isConnected() ? "true" : "false") + ",";
        json += "\"connecting\":" + String(WiFiMgr.isConnecting() ? "true" : "false") + ",";
        json += "\"sta_ip\":\"" + WiFiMgr.getSTAIP() + "\",";
        json += "\"ap_ip\":\"" + WiFiMgr.getAPIP() + "\",";
        json += "\"ssid\":\"" + WiFiMgr.getSSID() + "\",";
        json += "\"rssi\":" + String(WiFiMgr.getRSSI()) + ",";
        json += "\"ap_name\":\"" + WiFiMgr.getAPName() + "\",";
        json += "\"ap_active\":" + String(WiFiMgr.isAPActive() ? "true" : "false") + ",";
        json += "\"ap_remain_sec\":" + String(WiFiMgr.getAPShutdownRemainSec());
        json += "}}";

        _server.send(200, "application/json", json);
    });

    // 3.1 主控芯片实时温度独立接口
    auto handleTemp = [this]() {
        sendJsonHeaders(_server);
        float temp = temperatureRead();
        String json;
        json.reserve(64);
        json = "{\"ok\":true,\"temperature\":";
        json += String(temp, 1);
        json += ",\"unit\":\"°C\"}";
        _server.send(200, "application/json", json);
    };
    _server.on("/api/temperature", HTTP_GET, handleTemp);
    _server.on("/api/temp", HTTP_GET, handleTemp);

    // 4. 周边扫描到的 BLE 设备列表
    _server.on("/api/ble/devices", HTTP_GET, [this]() {
        sendJsonHeaders(_server);
        auto list = Tracker.getDiscoveredDevices();
        String boundMac = Tracker.getBoundMAC();
        uint32_t now = millis();

        String json;
        json.reserve(list.size() * 128 + 32);
        json = "[";
        for (size_t i = 0; i < list.size(); ++i) {
            if (i > 0) json += ",";
            bool isBound = (boundMac.length() > 0 && list[i].mac.equalsIgnoreCase(boundMac));
            json += "{";
            json += "\"mac\":\"" + list[i].mac + "\",";
            json += "\"name\":\"" + list[i].name + "\",";
            json += "\"rssi\":" + String(list[i].rssi) + ",";
            json += "\"age_sec\":" + String((now - list[i].lastSeenMs) / 1000) + ",";
            json += "\"is_bound\":" + String(isBound ? "true" : "false");
            json += "}";
        }
        json += "]";
        _server.send(200, "application/json", json);
    });

    // 4.1 清空周边扫描缓存
    _server.on("/api/ble/clear", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        Tracker.clearScanCache();
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已清空扫描缓存\"}");
    });

    // 5. 绑定目标 BLE 设备
    _server.on("/api/ble/bind", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        String mac = _server.arg("mac");
        String name = _server.arg("name");

        if (mac.length() == 17) {
            Tracker.setBoundDevice(mac, name);
            if (g_state == STATE_UNPAIRED) {
                g_state = STATE_PRESENT;
            }
            _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"绑定成功！\"}");
        } else {
            _server.send(400, "application/json", "{\"ok\":false,\"msg\":\"MAC地址无效\"}");
        }
    });

    // 6. 解除当前绑定
    _server.on("/api/ble/unbind", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        Tracker.clearBond();
        g_state = STATE_UNPAIRED;
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已成功解除绑定\"}");
    });

    // 7. 贴近雷达快速配对
    _server.on("/api/ble/start_pair", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        Tracker.startPairing(8000);
        g_state = STATE_PAIRING;
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"贴近配对已启动，请将手表/手环贴近板子！\"}");
    });

    // 7.1 手机主动连接配对管理
    _server.on("/api/ble/phone_pair/start", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        Tracker.startPhonePairMode(60000);
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"手机连接配对广播已开启 (限时60秒)\"}");
    });

    _server.on("/api/ble/phone_pair/stop", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        Tracker.stopPhonePairMode();
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已退出手机连接配对广播\"}");
    });

    _server.on("/api/ble/phone_pair/confirm", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        String name = _server.arg("name");
        Tracker.confirmIncomingPair(name);
        if (g_state == STATE_UNPAIRED) {
            g_state = STATE_PRESENT;
        }
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"手机绑定成功！\"}");
    });

    _server.on("/api/ble/phone_pair/reject", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        Tracker.rejectIncomingPair();
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已忽略本次手机连接\"}");
    });

    // 8. 保存工作参数
    _server.on("/api/settings", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        int lockVal = DEFAULT_LOCK_RSSI;
        int unlockVal = DEFAULT_UNLOCK_RSSI;
        uint32_t timeoutVal = DEFAULT_LEAVE_TIMEOUT_MS;
        int osVal = (int)g_osMode;

        String unlockPwd = g_unlockPassword;
        bool autoUnlock = g_autoUnlockEnabled;

        if (_server.hasArg("lock_rssi")) {
            lockVal = _server.arg("lock_rssi").toInt();
            unlockVal = _server.arg("unlock_rssi").toInt();
            timeoutVal = _server.arg("timeout_ms").toInt();
            osVal = _server.arg("os_mode").toInt();
            if (_server.hasArg("clear_pwd") && _server.arg("clear_pwd") == "1") {
                unlockPwd = "";
            } else if (_server.hasArg("unlock_pwd")) {
                String p = _server.arg("unlock_pwd");
                if (p.length() > 0) {
                    unlockPwd = p;
                }
            }
            if (_server.hasArg("auto_unlock")) {
                String au = _server.arg("auto_unlock");
                autoUnlock = (au == "1" || au == "true");
            }
        } else if (_server.hasArg("plain")) {
            String b = _server.arg("plain");
            int p1 = b.indexOf("lock_rssi=");
            if (p1 != -1) {
                lockVal = b.substring(p1 + 10).toInt();
                int p2 = b.indexOf("unlock_rssi=");
                if (p2 != -1) unlockVal = b.substring(p2 + 12).toInt();
                int p3 = b.indexOf("timeout_ms=");
                if (p3 != -1) timeoutVal = b.substring(p3 + 11).toInt();
                int p4 = b.indexOf("os_mode=");
                if (p4 != -1) osVal = b.substring(p4 + 8).toInt();
                if (b.indexOf("clear_pwd=1") != -1) {
                    unlockPwd = "";
                } else {
                    int p5 = b.indexOf("unlock_pwd=");
                    if (p5 != -1) {
                        int endP = b.indexOf('&', p5);
                        String p = (endP == -1) ? b.substring(p5 + 11) : b.substring(p5 + 11, endP);
                        if (p.length() > 0) {
                            unlockPwd = p;
                        }
                    }
                }
                int p6 = b.indexOf("auto_unlock=");
                if (p6 != -1) {
                    int endP = b.indexOf('&', p6);
                    String s = (endP == -1) ? b.substring(p6 + 12) : b.substring(p6 + 12, endP);
                    autoUnlock = (s == "1" || s == "true");
                }
            }
        }

        if (timeoutVal < 500 || timeoutVal > 30000) timeoutVal = DEFAULT_LEAVE_TIMEOUT_MS;

        if (lockVal >= -100 && lockVal <= -40 && unlockVal >= -100 && unlockVal <= -40) {
            saveRuntimeSettings(lockVal, unlockVal, timeoutVal, (OperatingSystem)osVal, unlockPwd, autoUnlock);
            Serial.printf("[Web] 保存配置成功: 锁屏=%d, 唤醒=%d, 超时=%lu, OS=%d, 自动解锁=%s, 密码=%s\n", 
                          lockVal, unlockVal, timeoutVal, osVal, autoUnlock ? "开" : "关", unlockPwd.length() > 0 ? "已设" : "空");
            _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"工作参数及解锁设置保存成功！\"}");
        } else {
            Serial.printf("[Web] 参数校验未通过: lock=%d, unlock=%d\n", lockVal, unlockVal);
            _server.send(400, "application/json", "{\"ok\":false,\"msg\":\"参数数值不合法\"}");
        }
    });

    // 9. 快速动作：立即锁屏
    _server.on("/api/action/lock", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        Locker.lock(g_osMode);
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已发送物理锁屏快捷键！\"}");
    });

    // 10. 快速动作：唤醒屏幕
    _server.on("/api/action/wake", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        Locker.wakeScreen();
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已发送屏幕唤醒击键！\"}");
    });

    // 10.1 快速动作：测试开屏解锁 (自动输入密码)
    _server.on("/api/action/unlock", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        Serial.printf("[Web] 收到开屏解锁测试指令: 密码=%s\n", g_unlockPassword.length() > 0 ? "已设" : "无");
        Locker.unlock(g_osMode, g_unlockPassword);
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已执行开屏解锁测试！\"}");
    });

    // 11. 快速动作：切换免打扰
    _server.on("/api/action/pause", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        if (g_state == STATE_PAUSED) {
            g_state = STATE_PRESENT;
            _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已退出免打扰，恢复自动锁屏监护.\"}");
        } else {
            g_state = STATE_PAUSED;
            g_pauseStartMs = millis();
            _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已开启15分钟免打扰挂起模式！\"}");
        }
    });

    // 12. Wi-Fi 周边扫描 (优化射频共存与信道驻留)
    _server.on("/api/wifi/scan", HTTP_GET, [this]() {
        sendJsonHeaders(_server);

        // 1. 临时暂停 BLE 扫描，释放 2.4GHz 射频资源给 Wi-Fi 全信道扫描
        Tracker.pauseScan();
        delay(50);

        // 2. 执行快速扫描：常规网络，单信道 80ms 快速捕获 (总耗时约 1 秒，杜绝超时)
        int16_t n = WiFi.scanNetworks(false, false, false, 80);

        // 3. 扫描结束立即恢复 BLE 持续扫描
        Tracker.resumeScan();

        struct ScannedNet {
            String ssid;
            int rssi;
            bool secure;
        };
        std::vector<ScannedNet> list;

        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                String s = WiFi.SSID(i);
                s.trim();
                if (s.length() == 0) continue;
                int r = WiFi.RSSI(i);
                bool sec = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

                // 去重，同名 SSID 保留信号最佳节点 (适配 Mesh 组网)
                bool exists = false;
                for (auto& item : list) {
                    if (item.ssid == s) {
                        exists = true;
                        if (r > item.rssi) {
                            item.rssi = r;
                            item.secure = sec;
                        }
                        break;
                    }
                }
                if (!exists) {
                    list.push_back({s, r, sec});
                }
            }
            std::sort(list.begin(), list.end(), [](const ScannedNet& a, const ScannedNet& b) {
                return a.rssi > b.rssi;
            });
            WiFi.scanDelete();
        }

        String json = "[";
        for (size_t i = 0; i < list.size(); ++i) {
            if (i > 0) json += ",";
            json += "{";
            json += "\"ssid\":\"" + list[i].ssid + "\",";
            json += "\"rssi\":" + String(list[i].rssi) + ",";
            json += "\"secure\":" + String(list[i].secure ? "true" : "false");
            json += "}";
        }
        json += "]";
        _server.send(200, "application/json", json);
    });

    // 13. 保存路由器 Wi-Fi
    _server.on("/api/wifi/save", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        String ssid = _server.arg("ssid");
        String pass = _server.arg("pass");
        if (ssid.length() > 0) {
            WiFiMgr.saveWiFiConfig(ssid, pass);
            _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Wi-Fi配置已保存，正在后台尝试连接...\"}");
        } else {
            _server.send(400, "application/json", "{\"ok\":false,\"msg\":\"SSID不能为空\"}");
        }
    });

    // 14. 重置 Wi-Fi
    _server.on("/api/wifi/reset", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        WiFiMgr.resetWiFiConfig();
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已恢复纯 AP 热点直连模式\"}");
    });

    // 14.1 手动切换/唤醒 AP 热点
    _server.on("/api/wifi/ap/toggle", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        if (WiFiMgr.isAPActive()) {
            WiFiMgr.disableAP();
            _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已主动关闭板载 AP 热点以省电降温\"}");
        } else {
            WiFiMgr.enableAP();
            _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已唤醒板载 AP 热点 (30秒内无连接将自动关闭节能)\"}");
        }
    });

    // 15. 系统软重启
    _server.on("/api/system/reboot", HTTP_POST, [this]() {
        sendJsonHeaders(_server);
        _server.send(200, "application/json", "{\"ok\":true,\"msg\":\"系统正在重启...\"}");
        delay(600);
        ESP.restart();
    });

    // 16. 固件在线升级 (OTA Update)
    _server.on("/update", HTTP_GET, [this]() { handleUpdate(); });
    _server.on("/update", HTTP_POST, 
        [this]() { handleUpdateDone(); }, 
        [this]() { handleUpdateUpload(); }
    );

    // 404 / 门户探测自动重定向
    _server.onNotFound([this]() {
        if (!WiFiMgr.isConnected()) {
            _server.sendHeader("Location", "http://192.168.4.1/", true);
            _server.send(302, "text/plain", "");
        } else {
            _server.send(404, "text/plain", "Not Found");
        }
    });
}

void WebService::handleUpdate() {
    sendJsonHeaders(_server);
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>固件无线升级 (OTA Update)</title>";
    html += "<style>";
    html += "body{background:#0b0f19;color:#f8fafc;font-family:-apple-system,BlinkMacSystemFont,sans-serif;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;padding:20px;}";
    html += ".card{background:rgba(23,32,54,0.85);backdrop-filter:blur(16px);border:1px solid rgba(255,255,255,0.1);border-radius:20px;padding:32px;max-width:440px;width:100%;box-shadow:0 25px 50px -12px rgba(0,0,0,0.5);}";
    html += "h2{margin-top:0;color:#38bdf8;font-size:22px;display:flex;align-items:center;gap:10px;}";
    html += ".meta{background:rgba(15,23,42,0.6);border-radius:12px;padding:12px 16px;margin:18px 0;font-size:13px;line-height:1.8;color:#94a3b8;}";
    html += ".dropzone{border:2px dashed rgba(56,189,248,0.4);border-radius:14px;padding:28px 16px;text-align:center;cursor:pointer;transition:all 0.3s;background:rgba(56,189,248,0.03);}";
    html += ".dropzone:hover{border-color:#38bdf8;background:rgba(56,189,248,0.08);}";
    html += "input[type=file]{display:none;}";
    html += ".btn{background:linear-gradient(135deg,#0284c7,#0369a1);color:#fff;border:none;border-radius:10px;padding:12px;width:100%;font-size:15px;font-weight:bold;cursor:pointer;margin-top:16px;transition:opacity 0.2s;}";
    html += ".btn:hover{opacity:0.9;}.btn:disabled{background:#475569;cursor:not-allowed;}";
    html += ".progress{height:10px;background:#1e293b;border-radius:5px;overflow:hidden;margin-top:16px;display:none;position:relative;}";
    html += ".progress-bar{height:100%;background:linear-gradient(90deg,#0ea5e9,#38bdf8);width:0%;transition:width 0.2s ease;}";
    html += "@keyframes pulseBar{0%{opacity:0.6;}50%{opacity:1;}100%{opacity:0.6;}}";
    html += ".status-box{margin-top:14px;font-size:13px;color:#cbd5e1;line-height:1.6;display:none;}";
    html += ".back{display:block;text-align:center;margin-top:16px;color:#64748b;text-decoration:none;font-size:13px;}";
    html += ".back:hover{color:#94a3b8;}";
    html += "</style></head><body>";
    html += "<div class='card'>";
    html += "<h2>🚀 固件无线 OTA 升级</h2>";
    html += "<div class='meta'>";
    html += "• 硬件平台: ESP32-S3 (USB 棒/SuperMini)<br>";
    html += "• 当前版本: " + String(FIRMWARE_VERSION) + "<br>";
    html += "• 空闲内存: " + String(ESP.getFreeHeap() / 1024) + " KB";
    html += "</div>";
    html += "<form id='f' method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<div class='dropzone' id='dz' onclick=\"document.getElementById('fi').click()\">";
    html += "<span id='fn' style='font-size:14px;color:#cbd5e1;'>📁 点击或拖拽固件 (.bin) 至此处</span>";
    html += "<input id='fi' type='file' name='update' accept='.bin' onchange=\"onFileChosen(this)\">";
    html += "</div>";
    html += "<div class='progress' id='prg'><div class='progress-bar' id='bar'></div></div>";
    html += "<div class='status-box' id='stBox'></div>";
    html += "<button type='button' class='btn' id='btn' disabled onclick='doUpload()'>⚡ 开始无线升级</button>";
    html += "</form>";
    html += "<a href='/' class='back'>[← 返回主控制台]</a>";
    html += "</div>";
    html += "<script>";
    html += "function onFileChosen(el){";
    html += "  if(el.files&&el.files[0]){";
    html += "    var f=el.files[0];";
    html += "    var mb=(f.size/1048576).toFixed(2);";
    html += "    document.getElementById('fn').innerHTML='已就绪: <b>'+f.name+'</b> ('+mb+' MB)';";
    html += "    document.getElementById('btn').disabled=false;";
    html += "  }";
    html += "}";
    html += "function doUpload(){";
    html += "  var f=document.getElementById('fi').files[0];";
    html += "  if(!f)return;";
    html += "  if(!f.name.endsWith('.bin')){alert('请选择以 .bin 结尾的固件镜像！');return;}";
    html += "  var btn=document.getElementById('btn');";
    html += "  var bar=document.getElementById('bar');";
    html += "  var prg=document.getElementById('prg');";
    html += "  var dz=document.getElementById('dz');";
    html += "  var st=document.getElementById('stBox');";
    html += "  btn.disabled=true;btn.innerText='固件传输中...';";
    html += "  dz.style.pointerEvents='none';";
    html += "  prg.style.display='block';";
    html += "  st.style.display='block';";
    html += "  st.innerHTML='正在通过 Wi-Fi 推送固件包...';";
    html += "  var fd=new FormData();fd.append('update',f);";
    html += "  var xhr=new XMLHttpRequest();";
    html += "  xhr.timeout=180000;";
    html += "  xhr.upload.addEventListener('progress',function(e){";
    html += "    if(e.lengthComputable){";
    html += "      var p=Math.round((e.loaded/e.total)*100);";
    html += "      bar.style.width=p+'%';";
    html += "      var cur=(e.loaded/1048576).toFixed(2);";
    html += "      var tot=(e.total/1048576).toFixed(2);";
    html += "      if(p<100){";
    html += "        st.innerHTML='已发送: <b>'+cur+' MB</b> / '+tot+' MB ('+p+'%)';";
    html += "        btn.innerText='传输中: '+p+'%';";
    html += "      }else{";
    html += "        st.innerHTML='⚡ <b>固件传输完毕！</b><br>ESP32 正在校验并烧录 Flash (约需 3~6 秒，请勿断电)...';";
    html += "        btn.innerText='⚙️ 正在写入 Flash...';";
    html += "        bar.style.animation='pulseBar 1.2s infinite ease-in-out';";
    html += "        bar.style.background='linear-gradient(90deg,#38bdf8,#818cf8,#38bdf8)';";
    html += "      }";
    html += "    }";
    html += "  });";
    html += "  xhr.onload=function(){";
    html += "    if(xhr.status===200){";
    html += "      document.body.innerHTML=xhr.responseText;";
    html += "    }else{";
    html += "      showError('固件写入失败 (HTTP '+xhr.status+')：<br>'+(xhr.responseText||'网络断开或固件格式不兼容'));";
    html += "    }";
    html += "  };";
    html += "  xhr.onerror=function(){";
    html += "    showError('传输异常中断！可能是烧录完成自动重启或网络闪断。请检查板子运行状态。');";
    html += "  };";
    html += "  xhr.ontimeout=function(){";
    html += "    showError('上传或写入超时，请刷新页面重新尝试。');";
    html += "  };";
    html += "  xhr.open('POST','/update');xhr.send(fd);";
    html += "}";
    html += "function showError(msg){";
    html += "  document.getElementById('prg').style.display='none';";
    html += "  var st=document.getElementById('stBox');";
    html += "  st.style.display='block';";
    html += "  st.innerHTML='<div style=\"background:rgba(239,68,68,0.15);border:1px solid rgba(239,68,68,0.4);border-radius:10px;padding:12px;color:#fca5a5;line-height:1.6;\">❌ '+msg+'</div>';";
    html += "  var btn=document.getElementById('btn');";
    html += "  btn.disabled=false;btn.innerText='🔄 重新选择固件';";
    html += "  document.getElementById('dz').style.pointerEvents='auto';";
    html += "}";
    html += "</script></body></html>";
    _server.send(200, "text/html; charset=utf-8", html);
}

void WebService::handleUpdateDone() {
    _server.sendHeader("Connection", "close");
    sendJsonHeaders(_server);
    bool success = !Update.hasError() && (s_otaErrorMsg.length() == 0);
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>body{background:#0b0f19;color:#fff;font-family:-apple-system,BlinkMacSystemFont,sans-serif;text-align:center;padding:60px 20px;}";
    html += ".card{background:rgba(23,32,54,0.85);backdrop-filter:blur(16px);max-width:440px;margin:0 auto;padding:36px;border-radius:20px;border:1px solid rgba(255,255,255,0.1);}";
    html += "h2{color:#38bdf8;margin-bottom:15px;}</style></head><body><div class='card'>";
    if (success) {
        html += "<h2>🎉 固件升级成功！</h2><p style='color:#94a3b8;'>新固件已成功刷入 Flash，ESP32-S3 正在自动重启，约 12 秒后自动刷新回到控制台...</p>";
        html += "<p id='cnt' style='font-size:32px;color:#4ade80;font-weight:bold;margin:20px 0;'>12</p>";
        html += "<script>var c=12;setInterval(function(){c--;document.getElementById('cnt').innerText=c;if(c<=0)location.href='/';},1000);</script>";
    } else {
        String reason = (s_otaErrorMsg.length() > 0) ? s_otaErrorMsg : "固件大小校验不匹配或 Flash 写入中断";
        html += "<h2 style='color:#f87171;'>❌ 固件升级已被系统拦截</h2><p style='color:#fca5a5;line-height:1.6;font-size:14px;'>" + reason + "</p>";
        html += "<a href='/update' style='color:#38bdf8;text-decoration:none;display:inline-block;margin-top:20px;font-weight:600;'>← 返回重试</a>";
    }
    html += "</div></body></html>";
    _server.send(200, "text/html; charset=utf-8", html);
    if (_server.client()) {
        _server.client().flush();
    }
    if (success) {
        Serial.println("✓ [OTA] 固件升级成功，1.2 秒后重启 ESP32-S3...");
        delay(1200);
        ESP.restart();
    } else {
        Tracker.resumeScan();
    }
}

void WebService::handleUpdateUpload() {
    HTTPUpload& upload = _server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        s_otaErrorMsg = "";
        s_otaHeaderChecked = false;
        // 关键点：暂停 BLE 扫描！释放 2.4GHz 射频资源给 Wi-Fi，防止共存机制导致网络丢包/卡死
        Tracker.pauseScan();
        Serial.printf("🚀 [OTA] 正在接收固件文件: %s，已暂停 BLE 扫描以确保 Wi-Fi 独享带宽\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (s_otaErrorMsg.length() > 0) return; // 拦截后续写入

        // 校验 Magic Byte (0xE9) 与 Chip ID (防呆防变砖)
        if (!s_otaHeaderChecked && upload.currentSize >= 16) {
            s_otaHeaderChecked = true;
            if (upload.buf[0] != 0xE9) {
                s_otaErrorMsg = "固件格式非法 (Magic Byte 非 0xE9)，该文件并非有效的 ESP 固件二进制文件！";
                Serial.printf("🚨 [OTA] %s\n", s_otaErrorMsg.c_str());
                Update.abort();
                Tracker.resumeScan();
                return;
            }
            uint16_t chipId = (uint16_t)upload.buf[12] | ((uint16_t)upload.buf[13] << 8);
            // ESP32-S3 的 Chip ID 为 0x0009
            if (chipId != 0x0009) {
                if (chipId == 0x0005) {
                    s_otaErrorMsg = "芯片架构不匹配: 该固件针对 ESP32-C3 (Chip ID: 0x0005)，当前设备为 ESP32-S3，已拦截写入以防变砖！";
                } else if (chipId == 0x0000) {
                    s_otaErrorMsg = "芯片架构不匹配: 该固件针对经典 ESP32 (Chip ID: 0x0000)，当前设备为 ESP32-S3，已拦截写入以防变砖！";
                } else {
                    s_otaErrorMsg = "芯片架构不匹配: 固件 Chip ID (0x" + String(chipId, HEX) + ") 与当前 ESP32-S3 (0x0009) 不匹配，已拦截写入以防变砖！";
                }
                Serial.printf("🚨 [OTA] %s\n", s_otaErrorMsg.c_str());
                Update.abort();
                Tracker.resumeScan();
                return;
            }
            Serial.println("✓ [OTA] 固件头校验通过: 有效 ESP32-S3 架构二进制镜像 (Magic: 0xE9, Chip: 0x0009)");
        }

        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
        yield(); // 喂狗并调度底层任务
    } else if (upload.status == UPLOAD_FILE_END) {
        if (s_otaErrorMsg.length() > 0) {
            Update.abort();
            Tracker.resumeScan();
            return;
        }
        if (Update.end(true)) {
            Serial.printf("✓ [OTA] 固件写入完毕 (%u 字节)\n", upload.totalSize);
        } else {
            Update.printError(Serial);
            Tracker.resumeScan();
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Tracker.resumeScan();
        Serial.println("🚨 [OTA] 客户端中止固件上传");
    }
}
