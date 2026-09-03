#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "db_manager.h"
#include "web_server.h"

// ─── Global instances ────────────────────────────────────
DBManager  gDB;
NRWebServer* gWeb = nullptr;

// ─── Task: DB Manager ────────────────────────────────────
// รัน scan ครั้งแรก แล้วคอย rescan ทุก 30 วิ
void taskDBManager(void* param) {
    Serial.println("[TASK:DB] Started");

    if (!gDB.begin()) {
        Serial.printf("[TASK:DB] SD init failed: %s\n", gDB.lastError());
        // ยังรัน task ต่อ แต่ไม่มีข้อมูล
    }

    while (true) {
        // Rescan ทุก 30 วินาที (เผื่อมีไฟล์ใหม่ถูก copy เข้า SD ข้างนอก)
        vTaskDelay(pdMS_TO_TICKS(30000));
        Serial.println("[TASK:DB] Rescanning...");
        gDB.scanAll();
    }
}

// ─── Task: Web Server ────────────────────────────────────
void taskWebServer(void* param) {
    Serial.println("[TASK:WEB] Started");

    gWeb = new NRWebServer(gDB);
    if (!gWeb->begin()) {
        Serial.println("[TASK:WEB] Failed to start");
        vTaskDelete(nullptr);
        return;
    }

    // ต้องเรียก handle() ใน loop เพื่อรับ request
    while (true) {
        gWeb->handle();
        vTaskDelay(pdMS_TO_TICKS(1)); // yield 1ms
    }
}

// ─── setup ───────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("╔═══════════════════════════════╗");
    Serial.println("║   N_RAWIT BOX  v" NODE_VERSION "         ║");
    Serial.printf ("║   Node #%d  %-20s║\n", NODE_ID, NODE_NAME);
    Serial.println("╚═══════════════════════════════╝");

    // ─── สร้าง FreeRTOS Tasks ────────────────────────────
    // DB Task: core 0 (ใช้ SD I/O)
    xTaskCreatePinnedToCore(
        taskDBManager,
        "DB_Manager",
        TASK_DB_STACK,
        nullptr,
        TASK_DB_PRIO,
        nullptr,
        0           // core 0
    );

    // Web Task: core 1 (network)
    xTaskCreatePinnedToCore(
        taskWebServer,
        "WebServer",
        TASK_WEB_STACK,
        nullptr,
        TASK_WEB_PRIO,
        nullptr,
        1           // core 1
    );

    Serial.println("[MAIN] Tasks created");
}

// ─── loop ────────────────────────────────────────────────
// Tasks จัดการทุกอย่างแล้ว loop ว่าง
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
