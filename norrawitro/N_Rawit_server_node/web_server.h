#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "db_manager.h"
#include "config.h"

// ─── Session ─────────────────────────────────────────────
struct Session {
    char  token[64];
    bool  active;
    uint32_t loginTime;
};

// ─── NRWebServer ─────────────────────────────────────────
class NRWebServer {
public:
    NRWebServer(DBManager& dbm);

    // เริ่ม WiFi AP + WebServer
    bool begin();

    // ต้องเรียกใน loop หรือ task
    void handle();

    // ตรวจว่า session token ถูกต้องและยังไม่ expired
    bool isAuthenticated(const String& token);

private:
    WebServer  _server;
    DBManager& _db;
    Session    _session;

    // ─── Route Handlers ─────────────────────────────────
    void _handleRoot();
    void _handleLogin();
    void _handleLogout();
    void _handleDashboard();
    void _handleApiList();
    void _handleApiMeta();
    void _handleApiDelete();
    void _handleApiUpload();
    void _handleNotFound();

    // Helper
    void   _sendUnauthorized();
    void   _generateToken(char* out);
    bool   _checkAuth();
    String _getSessionToken();

    // HTML page generators
    String _pageLogin();
    String _pageDashboard();
    String _htmlHead(const char* title);
    String _htmlFoot();
};
