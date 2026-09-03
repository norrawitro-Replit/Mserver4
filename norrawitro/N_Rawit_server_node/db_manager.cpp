#include "db_manager.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

// ─── Constructor ─────────────────────────────────────────
DBManager::DBManager()
    : _mounted(false), _seqCounter(0) {
    _lastError[0] = '\0';
}

// ─── begin: mount SD ─────────────────────────────────────
bool DBManager::begin() {
    // SDMMC 1-bit mode (ปรับเป็น 4-bit ได้ถ้า hardware support)
    if (!SD_MMC.begin("/sdcard", true)) {
        _setError("SD_MMC mount failed");
        return false;
    }
    _mounted = true;
    Serial.printf("[DB] SD mounted. Card size: %llu MB\n",
                  SD_MMC.cardSize() / (1024 * 1024));

    // สร้าง folder ถ้ายังไม่มี
    if (!SD_MMC.exists(DB_FOLDER)) {
        SD_MMC.mkdir(DB_FOLDER);
        Serial.printf("[DB] Created folder: %s\n", DB_FOLDER);
    }

    scanAll();
    return true;
}

// ─── scanAll: scan folder และ build index ────────────────
void DBManager::scanAll() {
    _entries.clear();
    _seqCounter = 0;

    File root = SD_MMC.open(DB_FOLDER);
    if (!root || !root.isDirectory()) {
        _setError("Cannot open databox folder");
        return;
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const char* name = file.name();
            // รับเฉพาะ .db files
            int len = strlen(name);
            if (len > 3 && strcmp(name + len - 3, ".db") == 0) {
                char fullPath[200];
                snprintf(fullPath, sizeof(fullPath), "%s/%s", DB_FOLDER, name);
                DBEntry entry;
                memset(&entry, 0, sizeof(entry));
                if (_parseHeader(fullPath, entry)) {
                    _entries.push_back(entry);
                    _seqCounter++;
                    Serial.printf("[DB] Indexed: %s (%s) %u bytes\n",
                                  entry.id, typeName(entry.type), entry.payload_size);
                } else {
                    Serial.printf("[DB] Skip invalid: %s\n", fullPath);
                }
            }
        }
        file = root.openNextFile();
    }
    root.close();

    Serial.printf("[DB] Total indexed: %d files\n", (int)_entries.size());
}

// ─── findById ────────────────────────────────────────────
const DBEntry* DBManager::findById(const char* id) const {
    for (auto& e : _entries) {
        if (strcmp(e.id, id) == 0) return &e;
    }
    return nullptr;
}

// ─── getByIndex ──────────────────────────────────────────
const DBEntry* DBManager::getByIndex(int i) const {
    if (i < 0 || i >= (int)_entries.size()) return nullptr;
    return &_entries[i];
}

// ─── readMeta: อ่าน metadata JSON ───────────────────────
String DBManager::readMeta(const DBEntry& e) {
    if (e.meta_size == 0) return "{}";

    File f = SD_MMC.open(e.path, FILE_READ);
    if (!f) return "{}";

    // skip header
    f.seek(DB_HEADER_SIZE);

    String result = "";
    uint32_t remaining = e.meta_size;
    uint8_t buf[128];
    while (remaining > 0) {
        size_t toRead = min((uint32_t)sizeof(buf), remaining);
        size_t n = f.read(buf, toRead);
        if (n == 0) break;
        result += String((char*)buf).substring(0, n);
        remaining -= n;
    }
    f.close();
    return result;
}

// ─── readPayload ─────────────────────────────────────────
int DBManager::readPayload(const DBEntry& e, uint8_t* buf, size_t maxLen) {
    File f = SD_MMC.open(e.path, FILE_READ);
    if (!f) return -1;

    uint32_t offset = DB_HEADER_SIZE + e.meta_size;
    f.seek(offset);

    size_t toRead = min((size_t)e.payload_size, maxLen);
    size_t n = f.read(buf, toRead);
    f.close();
    return (int)n;
}

// ─── writeDB: สร้าง DataBox ใหม่ ─────────────────────────
bool DBManager::writeDB(uint8_t type, const char* metaJson,
                        const uint8_t* payload, uint32_t payloadLen,
                        uint16_t priceToken, float lat, float lon) {
    if (!_mounted) return false;

    // สร้าง timestamp
    time_t now = time(nullptr);
    _seqCounter++;

    // สร้าง ID
    char id[32];
    makeId(id, type, (uint32_t)now, _seqCounter, 1);

    // สร้าง path
    char path[200];
    snprintf(path, sizeof(path), "%s/%s.db", DB_FOLDER, id);

    // คำนวณ hash ของ payload
    uint8_t hash[32];
    _computeHash(payload, payloadLen, hash);

    // สร้าง header
    DBHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0]     = DB_MAGIC_0;
    hdr.magic[1]     = DB_MAGIC_1;
    hdr.magic[2]     = DB_MAGIC_2;
    hdr.type         = type;
    hdr.created      = (uint32_t)now;
    hdr.version      = 1;
    hdr.node_id      = NODE_ID;
    hdr.gps_lat      = lat;
    hdr.gps_lon      = lon;
    hdr.price_token  = priceToken;
    hdr.meta_size    = (uint32_t)strlen(metaJson);
    hdr.payload_size = payloadLen;
    memcpy(hdr.sha256, hash, 32);

    // เขียน file
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        _setError("Cannot create file");
        return false;
    }

    f.write((uint8_t*)&hdr, sizeof(hdr));
    f.write((uint8_t*)metaJson, strlen(metaJson));
    if (payload && payloadLen > 0) {
        f.write(payload, payloadLen);
    }
    f.close();

    // เพิ่ม entry ใน index
    DBEntry entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.id, id, sizeof(entry.id) - 1);
    strncpy(entry.path, path, sizeof(entry.path) - 1);
    entry.type         = type;
    entry.created      = (uint32_t)now;
    entry.version      = 1;
    entry.node_id      = NODE_ID;
    entry.gps_lat      = lat;
    entry.gps_lon      = lon;
    entry.price_token  = priceToken;
    entry.meta_size    = (uint32_t)strlen(metaJson);
    entry.payload_size = payloadLen;
    entry.rank_total   = 0;
    entry.valid        = true;
    _entries.push_back(entry);

    Serial.printf("[DB] Created: %s (%u bytes payload)\n", id, payloadLen);
    return true;
}

// ─── deleteById ──────────────────────────────────────────
bool DBManager::deleteById(const char* id) {
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
        if (strcmp(it->id, id) == 0) {
            SD_MMC.remove(it->path);
            _entries.erase(it);
            Serial.printf("[DB] Deleted: %s\n", id);
            return true;
        }
    }
    return false;
}

// ─── incrementRank ───────────────────────────────────────
void DBManager::incrementRank(const char* id) {
    for (auto& e : _entries) {
        if (strcmp(e.id, id) == 0) {
            e.rank_total++;
            return;
        }
    }
}

// ─── typeName ────────────────────────────────────────────
const char* DBManager::typeName(uint8_t type) {
    switch (type) {
        case DB_TYPE_IMG:   return "IMG";
        case DB_TYPE_TXT:   return "TXT";
        case DB_TYPE_VID:   return "VID";
        case DB_TYPE_CFG:   return "CFG";
        case DB_TYPE_PRG:   return "PRG";
        default:            return "OTH";
    }
}

// ─── makeId ──────────────────────────────────────────────
void DBManager::makeId(char* out, uint8_t type, uint32_t ts, uint16_t seq, uint16_t ver) {
    // แปลง timestamp เป็น YYYYMMDD
    time_t t = (time_t)ts;
    struct tm* tm_info = localtime(&t);
    char dateStr[16];
    strftime(dateStr, sizeof(dateStr), "%Y%m%d", tm_info);

    snprintf(out, 32, "%s_%s_%03u_v%u",
             typeName(type), dateStr, seq, ver);
}

// ─── _parseHeader (private) ──────────────────────────────
bool DBManager::_parseHeader(const char* path, DBEntry& out) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    if (f.size() < DB_HEADER_SIZE) {
        f.close();
        return false;
    }

    DBHeader hdr;
    f.read((uint8_t*)&hdr, sizeof(hdr));
    f.close();

    // ตรวจ magic
    if (hdr.magic[0] != DB_MAGIC_0 ||
        hdr.magic[1] != DB_MAGIC_1 ||
        hdr.magic[2] != DB_MAGIC_2) {
        return false;
    }

    // สร้าง ID จาก header
    makeId(out.id, hdr.type, hdr.created, _seqCounter + 1, hdr.version);

    strncpy(out.path, path, sizeof(out.path) - 1);
    out.type         = hdr.type;
    out.created      = hdr.created;
    out.version      = hdr.version;
    out.node_id      = hdr.node_id;
    out.gps_lat      = hdr.gps_lat;
    out.gps_lon      = hdr.gps_lon;
    out.price_token  = hdr.price_token;
    out.meta_size    = hdr.meta_size;
    out.payload_size = hdr.payload_size;
    out.rank_total   = 0;
    out.valid        = true;

    return true;
}

// ─── _computeHash (SHA-256 via mbedtls) ──────────────────
void DBManager::_computeHash(const uint8_t* data, size_t len, uint8_t* out32) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out32);
    mbedtls_sha256_free(&ctx);
}

// ─── _setError ───────────────────────────────────────────
void DBManager::_setError(const char* msg) {
    strncpy(_lastError, msg, sizeof(_lastError) - 1);
    Serial.printf("[DB] ERROR: %s\n", msg);
}
