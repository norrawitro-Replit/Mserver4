#pragma once

// ─── Node Identity ───────────────────────────────────────
#define NODE_ID           1
#define NODE_NAME         "NR-BOX-001"
#define NODE_VERSION      "0.1.0"

// ─── WiFi ────────────────────────────────────────────────
#define WIFI_AP_SSID      "N_Rawit_001"
#define WIFI_AP_PASS      "nrawit1234"
#define WIFI_AP_CHANNEL   1
#define WIFI_MAX_CLIENTS  5
#define WEBSERVER_PORT    80

// ─── Login ───────────────────────────────────────────────
#define ADMIN_USERNAME    "admin"
#define ADMIN_PASSWORD    "rawit2026"
#define SESSION_TIMEOUT   3600        // seconds

// ─── SD Card (SDMMC) ─────────────────────────────────────
#define SD_MOUNT_POINT    "/sdcard"
#define DB_FOLDER         "/sdcard/databox"
#define MAX_DB_FILES      256

// ─── DataBox Format ──────────────────────────────────────
#define DB_MAGIC_0        0xDB
#define DB_MAGIC_1        0x4E        // N
#define DB_MAGIC_2        0x52        // R
#define DB_HEADER_SIZE    64

// DB Types
#define DB_TYPE_IMG       0x00
#define DB_TYPE_TXT       0x01
#define DB_TYPE_VID       0x02
#define DB_TYPE_CFG       0x03
#define DB_TYPE_PRG       0x04
#define DB_TYPE_OTHER     0xFF

// ─── Token ───────────────────────────────────────────────
#define TOKEN_INITIAL     100

// ─── FreeRTOS Tasks ──────────────────────────────────────
#define TASK_DB_STACK     8192
#define TASK_WEB_STACK    8192
#define TASK_DB_PRIO      2
#define TASK_WEB_PRIO     3
