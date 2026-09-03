#pragma once
#include <Arduino.h>
#include <SD_MMC.h>
#include <vector>
#include <mbedtls/sha256.h>
#include "config.h"

// ─── DataBox Header (64 bytes fixed) ─────────────────────
struct DBHeader {
    uint8_t  magic[3];          // 0xDB, 0x4E, 0x52
    uint8_t  type;              // DB_TYPE_*
    uint32_t created;           // unix timestamp
    uint16_t version;           // file version
    uint32_t node_id;           // origin node id
    float    gps_lat;           // origin GPS lat
    float    gps_lon;           // origin GPS lon
    uint16_t price_token;       // ราคา token (0 = free)
    uint32_t meta_size;         // metadata JSON size (bytes)
    uint32_t payload_size;      // payload size (bytes)
    uint8_t  sha256[32];        // hash of payload
    // pad to 64 bytes: 3+1+4+2+4+4+4+2+4+4+32 = 64 ✓
} __attribute__((packed));

// ─── DataBox Index Entry (in RAM) ────────────────────────
struct DBEntry {
    char     id[32];            // "IMG_20260405_001_v1"
    char     path[128];         // full SD path
    uint8_t  type;
    uint32_t created;
    uint16_t version;
    uint32_t node_id;
    float    gps_lat;
    float    gps_lon;
    uint16_t price_token;
    uint32_t meta_size;
    uint32_t payload_size;
    uint32_t rank_total;        // แร้งค์สะสม (download count)
    bool     valid;             // header valid + hash ok
};

// ─── DB Manager Class ─────────────────────────────────────
class DBManager {
public:
    DBManager();

    // Init: mount SD + scan folder
    bool begin();

    // Scan /sdcard/databox/ และ index ไฟล์ทั้งหมด
    void scanAll();

    // Get index
    const std::vector<DBEntry>& getEntries() const { return _entries; }
    int  count() const { return _entries.size(); }

    // Get entry by id string
    const DBEntry* findById(const char* id) const;

    // Get entry by index
    const DBEntry* getByIndex(int i) const;

    // Read metadata JSON of an entry (caller must free)
    String readMeta(const DBEntry& e);

    // Read payload into buffer (returns bytes read, -1 on error)
    int readPayload(const DBEntry& e, uint8_t* buf, size_t maxLen);

    // Write new DataBox to SD
    bool writeDB(uint8_t type, const char* metaJson,
                 const uint8_t* payload, uint32_t payloadLen,
                 uint16_t priceToken, float lat, float lon);

    // Delete a DataBox by id
    bool deleteById(const char* id);

    // Increment rank (on download)
    void incrementRank(const char* id);

    // Human-readable type string
    static const char* typeName(uint8_t type);

    // Generate DataBox ID string
    static void makeId(char* out, uint8_t type, uint32_t ts, uint16_t seq, uint16_t ver);

    // SD mounted?
    bool isMounted() const { return _mounted; }

    // Last error
    const char* lastError() const { return _lastError; }

private:
    std::vector<DBEntry> _entries;
    bool   _mounted;
    char   _lastError[128];
    uint16_t _seqCounter;

    bool   _parseHeader(const char* path, DBEntry& out);
    bool   _validateHash(const char* path, const DBHeader& hdr);
    void   _computeHash(const uint8_t* data, size_t len, uint8_t* out32);
    void   _setError(const char* msg);
};
