#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <Preferences.h>

class WebService {
public:
    WebService();
    void begin();
    void loop();

    // 凭证管理 (支持串口一键重置)
    void resetAuth();
    String getUsername() const { return _authUser; }

private:
    WebServer   _server;
    Preferences _prefs;
    String      _authUser;
    String      _authPass;
    String      _sessionToken;

    void loadCredentials();
    bool checkAuth();
    void generateSessionToken();

    void registerRoutes();

    void handleUpdate();
    void handleUpdateDone();
    void handleUpdateUpload();
};

extern WebService WebServ;
