#pragma once
#include <Arduino.h>
#include <WebServer.h>

class WebService {
public:
    WebService();
    void begin();
    void loop();

private:
    WebServer _server;
    void registerRoutes();

    void handleUpdate();
    void handleUpdateDone();
    void handleUpdateUpload();
};

extern WebService WebServ;
