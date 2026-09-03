// ============================================================
//  RHEA_v0.2.0_Full.ino
//  Platform  : ESP32_S3_N16R8_16MB_Flash_8MB_PSRAM
//  WebServer : Built_in_WebServer_h
//  WiFi      : AP_Mode_RHEA_Brain
//  SD        : SDMMC_1bit_GPIO40(DATA)_GPIO39(CLK)_GPIO38(CMD)
//  Flash     : Preferences_NVS
//
//  NEW v0.2.1:
//    ChainUnit — linked chunk system สำหรับข้อมูลที่ล้น PU
//    - แยก struct ChainUnit เก็บใน PSRAM array ต่างหาก
//    - PatternUnit เพิ่ม chainHeadId (0 = ไม่มี chain)
//    - Manual read: ตอบ PU content ก่อน แจ้งว่ามีต่อ
//    - คำสั่ง: chain:add / chain:read / chain:list / more:<tag>
//
//  Commands:
//    hello/status/battery/move/stop/memory/pu/memreport/goal/reset/help
//    backup:sd / backup:flash / restore:sd / restore:flash / backup:status
//    chain:add=<tag>|<data>  → เพิ่ม chunk ต่อท้าย PU
//    chain:read=<tag>        → อ่านทุก chunk ของ PU นั้น
//    chain:list              → แสดง ChainUnit ทั้งหมด
//    chain:del=<tag>         → ลบ chain ทั้งหมดของ PU นั้น
//    more:<tag>              → อ่าน chunk ถัดไปของ tag นั้น
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include "driver/temperature_sensor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ── WiFi AP ───────────────────────────────────────────────────
#define AP_SSID     "RHEA_Brain"
#define AP_PASSWORD "rhea1234"

// ── SD Card Pin (SDMMC 1-bit) ─────────────────────────────────
#define PIN_SD_DATA  40
#define PIN_SD_CLK   39
#define PIN_SD_CMD   38

// ── Backup Config ─────────────────────────────────────────────
#define BACKUP_SD_PATH        "/rhea_pu_backup.json"
#define BACKUP_SD_PATH_PREV   "/rhea_pu_backup_prev.json"
#define FLASH_NVS_NAMESPACE   "rhea_pu"
#define FLASH_NVS_KEY_COUNT   "pu_count"
#define FLASH_PU_MAX          50
#define PSRAM_AUTO_BACKUP_PCT 30
#define BACKUP_COOLDOWN_MS    30000UL

// ── Forward Declarations ──────────────────────────────────────
void taskSafety(void* p);
void taskPerception(void* p);
void taskCognition(void* p);
void taskLearning(void* p);
void taskWebServer(void* p);

// ── Config ────────────────────────────────────────────────────
namespace RheaConfig {
  constexpr uint16_t PU_LIMIT         = 1200;
  constexpr uint16_t CHAIN_LIMIT      = 2400;  // ChainUnit สูงสุด (2x PU)
  constexpr uint8_t  CHAIN_DATA_SIZE  = 120;   // bytes ต่อ 1 chunk
  constexpr uint8_t  CHAIN_MAX_DEPTH  = 32;    // chain ยาวสุด 32 chunks ต่อ PU
  constexpr uint8_t  PRIORITY_SAFETY  = 10;
  constexpr uint8_t  PRIORITY_PERCEPT = 6;
  constexpr uint8_t  PRIORITY_COGN    = 5;
  constexpr uint8_t  PRIORITY_LEARN   = 3;
  constexpr uint8_t  PRIORITY_WEBSRV  = 4;
  constexpr uint16_t STACK_SAFETY     = 4096;
  constexpr uint16_t STACK_PERCEPT    = 8192;
  constexpr uint16_t STACK_COGN       = 8192;
  constexpr uint16_t STACK_LEARN      = 6144;
  constexpr uint16_t STACK_WEBSRV     = 8192;
  constexpr uint8_t  BATTERY_LOW_PCT  = 15;
  constexpr uint8_t  PIN_BATT_ADC     = 4;
  constexpr uint8_t  FUZZY_THRESHOLD  = 40;
  constexpr uint16_t LOG_CHAT_MAX     = 2000;
  constexpr uint16_t LOG_EVENT_MAX    = 3000;
}

// ── struct QAHistory ──────────────────────────────────────────
struct QAHistory {
  char     question[64];
  char     answer[64];
  uint32_t askedAt;
};

// ── struct AliasEntry ─────────────────────────────────────────
struct AliasEntry {
  char     question[64];
  uint32_t firstSeenAt;
  uint16_t hitCount;
};

// ── struct ChainUnit ─────────────────────────────────────────
// เก็บข้อมูลที่ล้น PU เป็น chunk
// sizeof ≈ 4+32+1+1+120+4+4 = 166 bytes
// CHAIN_LIMIT=2400 → 2400×166 ≈ 384KB ใน PSRAM
struct ChainUnit {
  uint32_t id;                              // unique id
  uint32_t headPuId;                        // PU ต้นสังกัด (PatternUnit.id)
  char     puTag[32];                       // tag ของ PU ต้นสังกัด (ค้นหาเร็ว)
  uint8_t  chunkIndex;                      // เริ่มจาก 1
  uint8_t  chunkTotal;                      // จำนวน chunk ทั้งหมด (อัปเดตทุก append)
  char     data[RheaConfig::CHAIN_DATA_SIZE]; // เนื้อหา chunk นี้
  uint32_t nextChainId;                     // 0 = จบ chain, >0 = id ของ ChainUnit ถัดไป
  uint32_t createdAt;
};

// ── struct PatternUnit (เพิ่ม chainHeadId) ───────────────────
struct PatternUnit {
  uint32_t   id;
  char       tag[32];
  char       source[24];
  char       content[64];
  uint8_t    rank;
  uint32_t   accessCount;
  uint32_t   createdAt;
  uint32_t   lastAccessed;
  bool       isActive;
  AliasEntry aliases[8];
  uint8_t    aliasCount;
  QAHistory  history[3];
  uint8_t    historyCount;
  uint8_t    historyHead;
  uint32_t   chainHeadId;   // NEW: 0=ไม่มี chain, >0=id ของ ChainUnit แรก
  uint8_t    chainCount;    // NEW: จำนวน chunk ทั้งหมด (0=ไม่มี)
};

// ── struct PUSnapshot ─────────────────────────────────────────
struct PUSnapshot {
  uint32_t id;
  char     tag[32];
  char     source[24];
  char     content[64];
  uint8_t  rank;
  uint32_t accessCount;
  uint32_t createdAt;
  uint32_t lastAccessed;
  uint8_t  aliasCount;
  struct { char question[64]; uint16_t hitCount; } aliases[8];
  uint8_t  historyCount;
  struct { char question[64]; char answer[64]; } history[3];
  uint32_t chainHeadId;
  uint8_t  chainCount;
};

// ── struct Goal ───────────────────────────────────────────────
struct Goal {
  uint8_t id; char description[64];
  uint8_t priority; bool isActive; bool isCompleted; float progress;
};

// ── struct WorldState ─────────────────────────────────────────
struct WorldState {
  float batteryPercent; float mcuTemp;
  bool  isMoving;       uint32_t capturedAt;
};

// ── struct QAPair ─────────────────────────────────────────────
struct QAPair {
  char question[64]; char answer[128];
  uint32_t askedAt;  uint8_t askedCount;
};

// ── enum Emotion ──────────────────────────────────────────────
enum Emotion { NEUTRAL, HAPPY, CURIOUS, ALERT, TIRED };
const char* emotionName[] = {"Neutral","Happy","Curious","Alert","Tired"};

// ============================================================
//  BackupManager
// ============================================================
class BackupManager {
public:
  BackupManager() {
    _sdReady=false; _lastAutoTime=0;
    _mutex=xSemaphoreCreateMutex();
  }

  bool init() {
    _sdReady=_mountSD();
    if(_sdReady){Serial.println("[Backup] SD_OK");_printSDInfo();}
    else        {Serial.println("[Backup] SD_not_found_flash_only");}
    return true;
  }

  bool isSDReady(){ return _sdReady; }

  bool backupToSDDirect(PUSnapshot* snapshots, uint16_t count, bool isAuto=false){
    if(!_sdReady){ _sdReady=_mountSD(); if(!_sdReady)return false; }
    xSemaphoreTake(_mutex,portMAX_DELAY);
    if(SD_MMC.exists(BACKUP_SD_PATH)){
      if(SD_MMC.exists(BACKUP_SD_PATH_PREV))SD_MMC.remove(BACKUP_SD_PATH_PREV);
      SD_MMC.rename(BACKUP_SD_PATH,BACKUP_SD_PATH_PREV);
    }
    File f=SD_MMC.open(BACKUP_SD_PATH,FILE_WRITE);
    if(!f){xSemaphoreGive(_mutex);return false;}
    f.printf("{\"rhea_version\":\"0.2.1\",\"backup_type\":\"%s\","
             "\"timestamp_ms\":%lu,\"pu_count\":%u,"
             "\"psram_free\":%u,\"ram_free\":%u,\"items\":[\n",
             isAuto?"auto":"manual",millis(),count,
             (unsigned)ESP.getFreePsram(),(unsigned)ESP.getFreeHeap());
    for(uint16_t i=0;i<count;i++){
      PUSnapshot& p=snapshots[i];
      if(i>0)f.printf(",\n");
      f.printf("{\"id\":%u,\"tag\":\"%s\",\"source\":\"%s\","
               "\"content\":\"%s\",\"rank\":%u,\"access_count\":%u,"
               "\"alias_count\":%u,\"chain_head_id\":%u,\"chain_count\":%u,\"aliases\":[",
               p.id,_esc(p.tag).c_str(),_esc(p.source).c_str(),
               _esc(p.content).c_str(),p.rank,p.accessCount,
               p.aliasCount,p.chainHeadId,p.chainCount);
      for(uint8_t j=0;j<p.aliasCount;j++){
        if(j>0)f.printf(",");
        f.printf("{\"q\":\"%s\",\"hits\":%u}",
          _esc(p.aliases[j].question).c_str(),p.aliases[j].hitCount);
      }
      f.printf("]}");
    }
    f.printf("\n]}\n");
    f.close();
    Serial.printf("[Backup] SD_OK:%u_PU\n",count);
    xSemaphoreGive(_mutex);
    return true;
  }

  bool backupToFlash(PUSnapshot* snapshots, uint16_t count){
    if(count==0)return false;
    xSemaphoreTake(_mutex,portMAX_DELAY);
    Preferences prefs; prefs.begin(FLASH_NVS_NAMESPACE,false);
    uint16_t saveCount=min(count,(uint16_t)FLASH_PU_MAX);
    prefs.clear(); prefs.putUShort(FLASH_NVS_KEY_COUNT,saveCount);
    for(uint16_t i=0;i<saveCount;i++){
      PUSnapshot& p=snapshots[i]; char key[16]; String tmp;
      snprintf(key,16,"p%u_id",  i); prefs.putUInt(key,p.id);
      snprintf(key,16,"p%u_tag", i); prefs.putString(key,p.tag);
      snprintf(key,16,"p%u_src", i); prefs.putString(key,p.source);
      snprintf(key,16,"p%u_con", i); prefs.putString(key,p.content);
      snprintf(key,16,"p%u_rnk", i); prefs.putUChar(key,p.rank);
      snprintf(key,16,"p%u_acc", i); prefs.putUInt(key,p.accessCount);
      snprintf(key,16,"p%u_acnt",i); prefs.putUChar(key,p.aliasCount);
      snprintf(key,16,"p%u_chn", i); prefs.putUInt(key,p.chainHeadId);
      snprintf(key,16,"p%u_cnt", i); prefs.putUChar(key,p.chainCount);
      uint8_t ac=min(p.aliasCount,(uint8_t)4);
      for(uint8_t j=0;j<ac;j++){
        snprintf(key,16,"p%u_aq%u",i,j); prefs.putString(key,p.aliases[j].question);
        snprintf(key,16,"p%u_ah%u",i,j); prefs.putUShort(key,p.aliases[j].hitCount);
      }
    }
    prefs.end();
    xSemaphoreGive(_mutex);
    Serial.printf("[Backup] Flash_OK:%u_PU\n",saveCount);
    return true;
  }

  PUSnapshot* restoreFromFlash(uint16_t& count){
    count=0; Preferences prefs; prefs.begin(FLASH_NVS_NAMESPACE,true);
    uint16_t saveCount=prefs.getUShort(FLASH_NVS_KEY_COUNT,0);
    if(saveCount==0||saveCount>FLASH_PU_MAX){prefs.end();return nullptr;}
    PUSnapshot* arr=(PUSnapshot*)ps_malloc(sizeof(PUSnapshot)*saveCount);
    if(!arr){prefs.end();return nullptr;}
    memset(arr,0,sizeof(PUSnapshot)*saveCount);
    for(uint16_t i=0;i<saveCount;i++){
      PUSnapshot& p=arr[i]; char key[16]; String tmp;
      snprintf(key,16,"p%u_id",  i); p.id=prefs.getUInt(key,0);
      snprintf(key,16,"p%u_tag", i); tmp=prefs.getString(key,""); strncpy(p.tag,tmp.c_str(),31);
      snprintf(key,16,"p%u_src", i); tmp=prefs.getString(key,""); strncpy(p.source,tmp.c_str(),23);
      snprintf(key,16,"p%u_con", i); tmp=prefs.getString(key,""); strncpy(p.content,tmp.c_str(),63);
      snprintf(key,16,"p%u_rnk", i); p.rank=prefs.getUChar(key,64);
      snprintf(key,16,"p%u_acc", i); p.accessCount=prefs.getUInt(key,0);
      snprintf(key,16,"p%u_acnt",i); p.aliasCount=prefs.getUChar(key,0);
      snprintf(key,16,"p%u_chn", i); p.chainHeadId=prefs.getUInt(key,0);
      snprintf(key,16,"p%u_cnt", i); p.chainCount=prefs.getUChar(key,0);
      p.aliasCount=min(p.aliasCount,(uint8_t)4);
      for(uint8_t j=0;j<p.aliasCount;j++){
        snprintf(key,16,"p%u_aq%u",i,j); tmp=prefs.getString(key,"");
        strncpy(p.aliases[j].question,tmp.c_str(),63);
        snprintf(key,16,"p%u_ah%u",i,j); p.aliases[j].hitCount=prefs.getUShort(key,0);
      }
    }
    prefs.end(); count=saveCount;
    Serial.printf("[Restore] Flash_OK:%u_PU\n",saveCount);
    return arr;
  }

  String getStatusString(){
    char buf[180];
    if(_sdReady)
      snprintf(buf,sizeof(buf),"SD:Ready_%.1fMB/%.1fMB|Flash:NVS_ready|LastAuto:%lus_ago",
        SD_MMC.usedBytes()/1048576.0f,SD_MMC.totalBytes()/1048576.0f,
        (millis()-_lastAutoTime)/1000UL);
    else
      snprintf(buf,sizeof(buf),"SD:NOT_mounted|Flash:NVS_ready|LastAuto:%lus_ago",
        (millis()-_lastAutoTime)/1000UL);
    return String(buf);
  }

  void setLastAutoTime(uint32_t t){_lastAutoTime=t;}
  uint32_t getLastAutoTime(){return _lastAutoTime;}
  bool remountSD(){SD_MMC.end();delay(200);_sdReady=_mountSD();return _sdReady;}

private:
  bool _sdReady; uint32_t _lastAutoTime; SemaphoreHandle_t _mutex;

  bool _mountSD(){
    SD_MMC.setPins(PIN_SD_CLK,PIN_SD_CMD,PIN_SD_DATA);
    if(!SD_MMC.begin("/sdcard",true,false)){return false;}
    if(SD_MMC.cardType()==CARD_NONE){SD_MMC.end();return false;}
    return true;
  }
  void _printSDInfo(){
    uint8_t t=SD_MMC.cardType();
    const char* ts=(t==CARD_MMC)?"MMC":(t==CARD_SD)?"SDSC":(t==CARD_SDHC)?"SDHC":"UNK";
    Serial.printf("[Backup] SD_%s total:%.1fMB\n",ts,SD_MMC.totalBytes()/1048576.0f);
  }
  String _esc(const char* s){
    String o="";
    while(*s){
      if(*s=='"')o+="\\\"";
      else if(*s=='\\')o+="\\\\";
      else if(*s=='\n')o+="\\n";
      else o+=*s;
      s++;
    }
    return o;
  }
};

// ============================================================
//  RHEA_BRAIN
// ============================================================
class RheaBrain {
public:
  WebServer _server;

  RheaBrain():_server(80){
    _puCount=0; _puStore=nullptr;
    _chainCount=0; _chainStore=nullptr;
    _qaCount=0;
    _learningRate=0.5f; _explorationRate=0.3f; _thinkingScore=0.5f;
    _emergencyFlag=false; _isMoving=false;
    _nextPuId=1; _nextChainId=1; _lastLoopTime=0;
    _emotion=NEUTRAL; _goalDesc="Idle";
    _chatLog=""; _eventLog=""; _tempHandle=NULL;
    memset(_goals,0,sizeof(_goals));
    memset(_qaMemory,0,sizeof(_qaMemory));
    memset(&_world,0,sizeof(_world));
    memset(_moreState,0,sizeof(_moreState));
    _mutexPU   =xSemaphoreCreateMutex();
    _mutexChain=xSemaphoreCreateMutex();
    _mutexQA   =xSemaphoreCreateMutex();
    _mutexLog  =xSemaphoreCreateMutex();
  }

  // ─── Init System ─────────────────────────────────────────
  bool initSystem(){
    Serial.begin(115200); delay(300);
    _log("INFO","RHEA_booting_v0.2.1");
    if(!_initPSRAM()){_log("ERR","PSRAM_failed");return false;}
    _initTempSensor();
    _printMemoryReport();
    _startAP();
    _startWebServer();
    _seedDefaultKnowledge();
    _backup.init();
    if(!initTasks()){_log("ERR","Tasks_failed");return false;}
    _log("INFO","RHEA_ready");
    return true;
  }

  // ─── Init PSRAM: alloc PU + ChainUnit arrays ─────────────
  bool _initPSRAM(){
    if(!psramFound()){_log("ERR","PSRAM_not_found");return false;}

    // PU array
    _puStore=(PatternUnit*)ps_malloc(sizeof(PatternUnit)*RheaConfig::PU_LIMIT);
    if(!_puStore){_log("ERR","PU_alloc_failed");return false;}
    memset(_puStore,0,sizeof(PatternUnit)*RheaConfig::PU_LIMIT);

    // ChainUnit array (แยกต่างหาก)
    _chainStore=(ChainUnit*)ps_malloc(sizeof(ChainUnit)*RheaConfig::CHAIN_LIMIT);
    if(!_chainStore){_log("ERR","Chain_alloc_failed");return false;}
    memset(_chainStore,0,sizeof(ChainUnit)*RheaConfig::CHAIN_LIMIT);

    _log("INFO","PSRAM_OK_PU+Chain_allocated");
    Serial.printf("[INFO] PU_array  : %u bytes\n",(unsigned)(sizeof(PatternUnit)*RheaConfig::PU_LIMIT));
    Serial.printf("[INFO] Chain_array: %u bytes\n",(unsigned)(sizeof(ChainUnit)*RheaConfig::CHAIN_LIMIT));
    return true;
  }

  void _initTempSensor(){
    temperature_sensor_config_t cfg={.range_min=50,.range_max=125};
    temperature_sensor_install(&cfg,&_tempHandle);
    temperature_sensor_enable(_tempHandle);
  }

  void _printMemoryReport(){
    Serial.println("=== Memory_Report_v0.2.1 ===");
    Serial.printf("RAM_free       : %u bytes\n",  ESP.getFreeHeap());
    Serial.printf("PSRAM_total    : %u bytes\n",  ESP.getPsramSize());
    Serial.printf("PSRAM_free     : %u bytes\n",  ESP.getFreePsram());
    Serial.printf("PU_limit       : %u slots\n",  RheaConfig::PU_LIMIT);
    Serial.printf("PU_struct_size : %u bytes\n",  (unsigned)sizeof(PatternUnit));
    Serial.printf("PU_array_total : %u bytes\n",  (unsigned)(sizeof(PatternUnit)*RheaConfig::PU_LIMIT));
    Serial.printf("Chain_limit    : %u slots\n",  RheaConfig::CHAIN_LIMIT);
    Serial.printf("Chain_size     : %u bytes\n",  (unsigned)sizeof(ChainUnit));
    Serial.printf("Chain_array    : %u bytes\n",  (unsigned)(sizeof(ChainUnit)*RheaConfig::CHAIN_LIMIT));
    Serial.println("============================");
  }

  void _startAP(){
    WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID,AP_PASSWORD); delay(500);
    _logEvent("AP_"+String(AP_SSID)+"_IP:"+WiFi.softAPIP().toString());
  }

  // ─── WebServer ────────────────────────────────────────────
  void _startWebServer(){
    _server.on("/",HTTP_GET,[this](){_server.send(200,"text/html",_buildHTML());});
    _server.on("/api/status",HTTP_GET,[this](){_server.send(200,"application/json",_buildStatusJSON());});
    _server.on("/api/cmd",HTTP_POST,[this](){
      if(_server.hasArg("plain")){processCommand(_server.arg("plain"));_server.send(200,"application/json","{\"ok\":true}");}
      else _server.send(400,"application/json","{\"ok\":false}");
    });
    _server.on("/api/chat",HTTP_GET,[this](){
      xSemaphoreTake(_mutexLog,portMAX_DELAY);String l=_chatLog;xSemaphoreGive(_mutexLog);
      _server.send(200,"text/plain; charset=utf-8",l);
    });
    _server.on("/api/events",HTTP_GET,[this](){
      xSemaphoreTake(_mutexLog,portMAX_DELAY);String l=_eventLog;xSemaphoreGive(_mutexLog);
      _server.send(200,"text/plain; charset=utf-8",l);
    });
    _server.on("/api/pu",HTTP_GET,[this](){
      uint16_t page=0,size=20;
      if(_server.hasArg("page"))page=_server.arg("page").toInt();
      if(_server.hasArg("size"))size=_server.arg("size").toInt();
      _server.send(200,"application/json",_buildPUJson(page,size));
    });
    _server.on("/api/chain",HTTP_GET,[this](){
      uint16_t page=0,size=20;
      if(_server.hasArg("page"))page=_server.arg("page").toInt();
      if(_server.hasArg("size"))size=_server.arg("size").toInt();
      _server.send(200,"application/json",_buildChainJson(page,size));
    });
    _server.begin();
    _logEvent("WebServer_http://"+WiFi.softAPIP().toString());
  }

  void handleWebServer(){ _server.handleClient(); }

  bool initTasks(){
    xTaskCreatePinnedToCore(taskSafety,   "Safety",RheaConfig::STACK_SAFETY, this,RheaConfig::PRIORITY_SAFETY, NULL,0);
    xTaskCreatePinnedToCore(taskPerception,"Percept",RheaConfig::STACK_PERCEPT,this,RheaConfig::PRIORITY_PERCEPT,NULL,1);
    xTaskCreatePinnedToCore(taskCognition,"Cogn",  RheaConfig::STACK_COGN,  this,RheaConfig::PRIORITY_COGN,  NULL,1);
    xTaskCreatePinnedToCore(taskLearning, "Learn", RheaConfig::STACK_LEARN, this,RheaConfig::PRIORITY_LEARN, NULL,0);
    xTaskCreatePinnedToCore(taskWebServer,"WebSrv",RheaConfig::STACK_WEBSRV,this,RheaConfig::PRIORITY_WEBSRV,NULL,0);
    return true;
  }

  void processPerception(){
    uint16_t raw=analogRead(RheaConfig::PIN_BATT_ADC);
    float v=(raw/4095.0f)*3.3f*2.0f;
    _world.batteryPercent=constrain((v-3.0f)/1.2f*100.0f,0.0f,100.0f);
    if(_tempHandle)temperature_sensor_get_celsius(_tempHandle,&_world.mcuTemp);
    if(_world.mcuTemp>85.0f){_logEvent("WARN_OVERHEAT_"+String(_world.mcuTemp,1)+"C");_emotion=ALERT;}
    _world.capturedAt=millis();
  }

  void monitorSafety(){
    if(ESP.getFreeHeap()<20000){_logEvent("WARN_Low_RAM");_emotion=ALERT;}
    if(_world.batteryPercent<RheaConfig::BATTERY_LOW_PCT&&_world.batteryPercent>0){
      speak("แบตเตอรี่เหลือน้อย_กรุณาชาร์จด้วยครับ");_emotion=TIRED;
    }
    uint32_t now=millis();
    if(_lastLoopTime>0&&(now-_lastLoopTime)<50)vTaskDelay(pdMS_TO_TICKS(200));
    _lastLoopTime=now;
  }

  void emergencyStop(){_emergencyFlag=true;_isMoving=false;_emotion=ALERT;_logEvent("EMERGENCY_STOP");}
  bool isSafeToOperate(){return !_emergencyFlag;}
  void activateGoal(uint8_t id){if(id<8)_goals[id].isActive=true;}

  void prioritizeGoals(){
    for(uint8_t i=0;i<7;i++)
      for(uint8_t j=0;j<7-i;j++)
        if(_goals[j].priority<_goals[j+1].priority){Goal t=_goals[j];_goals[j]=_goals[j+1];_goals[j+1]=t;}
  }

  void runAutoBackup(){
    uint32_t psramFree=ESP.getFreePsram(),psramTotal=ESP.getPsramSize();
    if(psramTotal==0)return;
    uint8_t pct=(uint8_t)((float)psramFree/psramTotal*100.0f);
    if(pct>=PSRAM_AUTO_BACKUP_PCT)return;
    uint32_t now=millis();
    if(now-_backup.getLastAutoTime()<BACKUP_COOLDOWN_MS)return;
    _logEvent("AUTO_BACKUP_PSRAM_"+String(pct)+"%");
    uint16_t count=0;
    PUSnapshot* snaps=_buildSnapshots(count);
    if(!snaps)return;
    bool ok=_backup.backupToSDDirect(snaps,count,true);
    free(snaps);
    if(ok){_backup.setLastAutoTime(now);speak("Auto_backup_เสร็จ_PSRAM_"+String(pct)+"%");}
  }

  // ============================================================
  //  CHAIN UNIT SYSTEM
  // ============================================================

  // ─── เพิ่ม chunk ต่อท้าย PU ─────────────────────────────
  // tag = tag ของ PU ที่ต้องการ append, data = เนื้อหา chunk ใหม่
  bool chainAppend(const char* tag, const char* data){
    // หา PU ก่อน
    int16_t puIdx=_findPUIndexByTag(tag);
    if(puIdx<0){
      speak("ไม่พบ_PU_tag:["+String(tag)+"]_ต้องสร้าง_PU_ก่อนด้วย_สอน:"+String(tag)+"=<คำตอบหลัก>");
      return false;
    }
    if(_chainCount>=RheaConfig::CHAIN_LIMIT){
      speak("CHAIN_array_เต็ม_("+String(RheaConfig::CHAIN_LIMIT)+"_chunks)");
      return false;
    }

    xSemaphoreTake(_mutexChain,portMAX_DELAY);

    // สร้าง ChainUnit ใหม่
    ChainUnit& cu=_chainStore[_chainCount++];
    memset(&cu,0,sizeof(ChainUnit));
    cu.id        =_nextChainId++;
    cu.headPuId  =_puStore[puIdx].id;
    cu.createdAt =millis();
    strncpy(cu.puTag,tag,31);
    strncpy(cu.data,data,RheaConfig::CHAIN_DATA_SIZE-1);

    // หา tail ของ chain ปัจจุบัน และอัปเดต chunkIndex / chunkTotal
    xSemaphoreTake(_mutexPU,portMAX_DELAY);

    if(_puStore[puIdx].chainHeadId==0){
      // chain แรก
      cu.chunkIndex=1;
      cu.chunkTotal=1;
      _puStore[puIdx].chainHeadId=cu.id;
      _puStore[puIdx].chainCount=1;
    } else {
      // เดิน chain จนถึง tail แล้วต่อ
      uint32_t curId=_puStore[puIdx].chainHeadId;
      uint8_t  depth=0;
      int16_t  tailIdx=-1;
      while(curId!=0&&depth<RheaConfig::CHAIN_MAX_DEPTH){
        int16_t ci=_findChainIndexById(curId);
        if(ci<0)break;
        tailIdx=ci;
        curId=_chainStore[ci].nextChainId;
        depth++;
      }
      if(tailIdx<0){xSemaphoreGive(_mutexPU);xSemaphoreGive(_mutexChain);return false;}
      if(depth>=RheaConfig::CHAIN_MAX_DEPTH){
        speak("Chain_ยาวเกิน_MAX_DEPTH_("+String(RheaConfig::CHAIN_MAX_DEPTH)+")");
        xSemaphoreGive(_mutexPU);xSemaphoreGive(_mutexChain);return false;
      }
      _chainStore[tailIdx].nextChainId=cu.id;
      cu.chunkIndex=depth+1;
      // อัปเดต chunkTotal ในทุก node + PU
      _updateChainTotal(_puStore[puIdx].chainHeadId,depth+1);
      cu.chunkTotal=depth+1;
      _puStore[puIdx].chainCount=depth+1;
    }

    xSemaphoreGive(_mutexPU);
    xSemaphoreGive(_mutexChain);

    char buf[80];
    snprintf(buf,sizeof(buf),"Chain_เพิ่มแล้ว_PU:[%s]_chunk_%u",tag,cu.chunkIndex);
    speak(buf);
    _logEvent("CHAIN_APPEND_PU:["+String(tag)+"]_chunk:"+String(cu.chunkIndex)+
              "_id:"+String(cu.id));
    return true;
  }

  // ─── อ่าน chunk ถัดไป (manual mode) ─────────────────────
  // more:<tag> → อ่าน chunk ถัดจาก cursor ปัจจุบัน
  void chainReadNext(const char* tag){
    int16_t puIdx=_findPUIndexByTag(tag);
    if(puIdx<0){speak("ไม่พบ_PU_tag:["+String(tag)+"]");return;}
    if(_puStore[puIdx].chainHeadId==0||_puStore[puIdx].chainCount==0){
      speak("PU_["+String(tag)+"]_ไม่มีข้อมูลต่อ_เพิ่มด้วย_chain:add="+String(tag)+"|<ข้อมูล>");
      return;
    }

    // หา cursor ปัจจุบันของ tag นี้
    uint8_t cursor=_getMoreCursor(tag); // 0-based index ของ chunk ที่จะอ่านต่อไป

    // เดิน chain ไปถึง cursor
    uint32_t curId=_puStore[puIdx].chainHeadId;
    uint8_t  step=0;
    int16_t  ci=-1;
    while(curId!=0&&step<=cursor){
      ci=_findChainIndexById(curId);
      if(ci<0)break;
      if(step==cursor)break;
      curId=_chainStore[ci].nextChainId;
      step++;
    }

    if(ci<0){speak("Chain_read_error_for_["+String(tag)+"]");return;}

    uint8_t total=_puStore[puIdx].chainCount;
    uint8_t current=cursor+1;

    char buf[220];
    snprintf(buf,sizeof(buf),
      "[chunk_%u/%u] %s",
      current,total,_chainStore[ci].data);
    speak(buf);

    // อัปเดต cursor
    if(current<total){
      _setMoreCursor(tag,cursor+1);
      snprintf(buf,sizeof(buf),
        "→ มีต่ออีก %u chunk พิมพ์ 'more:%s' เพื่ออ่านต่อ",
        total-current,tag);
      speak(buf);
    } else {
      // จบแล้ว reset cursor
      _setMoreCursor(tag,0);
      speak("→ จบข้อมูลทั้งหมดของ ["+String(tag)+"] แล้วครับ");
    }
    _logEvent("CHAIN_READ_["+String(tag)+"]_chunk:"+String(current)+"/"+String(total));
  }

  // ─── อ่าน chain ทั้งหมดทีเดียว ───────────────────────────
  void chainReadAll(const char* tag){
    int16_t puIdx=_findPUIndexByTag(tag);
    if(puIdx<0){speak("ไม่พบ_PU_tag:["+String(tag)+"]");return;}
    if(_puStore[puIdx].chainHeadId==0){speak("PU_["+String(tag)+"]_ไม่มี_chain");return;}

    uint32_t curId=_puStore[puIdx].chainHeadId;
    uint8_t  total=_puStore[puIdx].chainCount;
    uint8_t  idx=0;
    String   out="=== Chain_all_["+String(tag)+"]_("+String(total)+"_chunks) ===\n";

    while(curId!=0&&idx<RheaConfig::CHAIN_MAX_DEPTH){
      int16_t ci=_findChainIndexById(curId);
      if(ci<0)break;
      out+="["+String(idx+1)+"/"+String(total)+"] "+String(_chainStore[ci].data)+"\n";
      curId=_chainStore[ci].nextChainId;
      idx++;
    }
    Serial.print(out); _appendChat(out);
    _setMoreCursor(tag,0); // reset cursor หลังอ่านทั้งหมด
  }

  // ─── ลบ chain ทั้งหมดของ PU ─────────────────────────────
  void chainDelete(const char* tag){
    int16_t puIdx=_findPUIndexByTag(tag);
    if(puIdx<0){speak("ไม่พบ_PU_tag:["+String(tag)+"]");return;}
    if(_puStore[puIdx].chainHeadId==0){speak("PU_["+String(tag)+"]_ไม่มี_chain_อยู่แล้ว");return;}

    xSemaphoreTake(_mutexChain,portMAX_DELAY);
    uint32_t curId=_puStore[puIdx].chainHeadId;
    uint8_t  deleted=0;
    while(curId!=0){
      int16_t ci=_findChainIndexById(curId);
      if(ci<0)break;
      uint32_t nextId=_chainStore[ci].nextChainId;
      // ลบโดย swap กับ tail แล้ว decrement count
      if(ci<(int16_t)_chainCount-1){
        _chainStore[ci]=_chainStore[_chainCount-1];
      }
      memset(&_chainStore[_chainCount-1],0,sizeof(ChainUnit));
      _chainCount--;
      deleted++;
      curId=nextId;
    }
    xSemaphoreGive(_mutexChain);

    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    _puStore[puIdx].chainHeadId=0;
    _puStore[puIdx].chainCount=0;
    xSemaphoreGive(_mutexPU);

    _setMoreCursor(tag,0);
    char buf[80];
    snprintf(buf,sizeof(buf),"ลบ_chain_[%s]_เสร็จ_%u_chunks_ถูกลบ",tag,deleted);
    speak(buf);
    _logEvent("CHAIN_DEL_["+String(tag)+"]_"+String(deleted)+"_chunks");
  }

  // ─── แสดง ChainUnit list ─────────────────────────────────
  void chainList(){
    xSemaphoreTake(_mutexChain,portMAX_DELAY);
    String out="=== ChainUnits_("+String(_chainCount)+"/"+
               String(RheaConfig::CHAIN_LIMIT)+") ===\n";
    uint16_t show=min((uint16_t)30,_chainCount);
    for(uint16_t i=0;i<show;i++){
      ChainUnit& c=_chainStore[i];
      out+="CU#"+String(c.id)+
           "_PU:["+String(c.puTag)+"]"+
           "_chunk:"+String(c.chunkIndex)+"/"+String(c.chunkTotal)+
           "_next:"+String(c.nextChainId)+"\n";
      out+="  data:"+String(c.data).substring(0,40)+"...\n";
    }
    if(_chainCount>30)out+="...และอีก "+String(_chainCount-30)+" chunk\n";
    xSemaphoreGive(_mutexChain);
    Serial.print(out); _appendChat(out);
  }

  // ─── Learn QA ────────────────────────────────────────────
  void learnQA(const char* question, const char* answer){
    xSemaphoreTake(_mutexQA,portMAX_DELAY);
    for(uint8_t i=0;i<_qaCount;i++){
      if(_strcmpi(_qaMemory[i].question,question)==0){
        strncpy(_qaMemory[i].answer,answer,127);
        xSemaphoreGive(_mutexQA);
        _boostPURankByTag(question,20);
        return;
      }
    }
    if(_qaCount<32){
      uint8_t idx=_qaCount++;
      strncpy(_qaMemory[idx].question,question,63);
      strncpy(_qaMemory[idx].answer,  answer,  127);
      _qaMemory[idx].askedAt=millis();
      _emotion=HAPPY;
    }
    xSemaphoreGive(_mutexQA);
    _createPU(question,180,"teach",answer);
  }

  void speak(const char* text){
    String msg=String("[RHEA]: ")+text;
    Serial.println(msg); _appendChat(msg);
  }
  void speak(String s){speak(s.c_str());}

  // ============================================================
  //  COMMAND TABLE
  // ============================================================
  struct Command{ const char* text; void(RheaBrain::*handler)(); const char* desc; };

  void cmdHello()  {speak("สวัสดีครับ_ผม_RHEA_v0.2.1_พร้อมรับคำสั่งครับ");_emotion=HAPPY;}

  void cmdStatus(){
    char buf[320];
    snprintf(buf,sizeof(buf),
      "RAM:%uB PSRAM:%uB\n"
      "PU:%u/%u QA:%u Chain:%u/%u\n"
      "Temp:%.1fC Batt:%.0f%%\n"
      "Score:%.2f %s",
      (unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram(),
      (unsigned)_puCount,RheaConfig::PU_LIMIT,(unsigned)_qaCount,
      (unsigned)_chainCount,RheaConfig::CHAIN_LIMIT,
      _world.mcuTemp,_world.batteryPercent,
      _thinkingScore,_emergencyFlag?"STOPPED":"OK");
    speak(buf);
  }

  void cmdBattery(){
    char buf[64];
    snprintf(buf,sizeof(buf),"Battery_%.0f%%_MCU_Temp_%.1fC",
      _world.batteryPercent,_world.mcuTemp);
    speak(buf);
  }

  void cmdMove(){
    if(_emergencyFlag){speak("ระบบหยุดฉุกเฉิน");return;}
    _isMoving=true;_world.isMoving=true;_goalDesc="Moving";_emotion=CURIOUS;
    speak("Robot_moving");_logEvent("Motor_MOVE");
  }

  void cmdStop(){
    _isMoving=false;_world.isMoving=false;_emergencyFlag=false;
    _goalDesc="Idle";_emotion=NEUTRAL;
    speak("Robot_stopped");_logEvent("Motor_STOP");
  }

  void cmdMemory(){
    String out="=== QA_("+String(_qaCount)+") ===\n";
    for(uint8_t i=0;i<_qaCount;i++)
      out+=String(i+1)+". Q:"+_qaMemory[i].question+
           " A:"+_qaMemory[i].answer+
           " ("+String(_qaMemory[i].askedCount)+"x)\n";
    if(!_qaCount)out+="ยังไม่มีครับ\n";
    Serial.print(out);_appendChat(out);
  }

  void cmdPU(){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    String out="=== PU_("+String(_puCount)+"/"+String(RheaConfig::PU_LIMIT)+") ===\n";
    uint16_t show=min((uint16_t)20,_puCount);
    for(uint16_t i=0;i<show;i++){
      PatternUnit& p=_puStore[i];
      out+="PU#"+String(p.id)+"_["+String(p.tag)+"]"
          +" rank:"+String(p.rank)
          +" chain:"+String(p.chainCount)
          +" alias:"+String(p.aliasCount)+"\n";
    }
    xSemaphoreGive(_mutexPU);
    Serial.print(out);_appendChat(out);
  }

  void cmdMemReport(){
    char buf[320];
    snprintf(buf,sizeof(buf),
      "=== Memory_v0.2.1 ===\n"
      "RAM_free    : %u bytes\n"
      "PSRAM_total : %u bytes\n"
      "PSRAM_free  : %u bytes\n"
      "PU_used     : %u/%u (%u bytes)\n"
      "Chain_used  : %u/%u (%u bytes)\n"
      "QA_pairs    : %u/32\n"
      "MCU_Temp    : %.1f C\n",
      (unsigned)ESP.getFreeHeap(),
      (unsigned)ESP.getPsramSize(),
      (unsigned)ESP.getFreePsram(),
      (unsigned)_puCount,RheaConfig::PU_LIMIT,
      (unsigned)(sizeof(PatternUnit)*RheaConfig::PU_LIMIT),
      (unsigned)_chainCount,RheaConfig::CHAIN_LIMIT,
      (unsigned)(sizeof(ChainUnit)*RheaConfig::CHAIN_LIMIT),
      (unsigned)_qaCount,_world.mcuTemp);
    Serial.print(buf);_appendChat(String(buf));
  }

  void cmdGoal(){speak("Goal:"+_goalDesc);}

  void cmdReset(){
    _emergencyFlag=false;_isMoving=false;
    _thinkingScore=0.5f;_goalDesc="Idle";_emotion=NEUTRAL;
    speak("System_reset");_logEvent("RESET");
  }

  void cmdHelp(){
    String h="=== Commands ===\n";
    for(uint8_t i=0;i<_cmdCount;i++)
      h+=String(_commands[i].text)+" : "+String(_commands[i].desc)+"\n";
    h+="Q=A / Q=Q2 / Q (สอน/ผูก/ถาม)\n";
    h+="chain:add=<tag>|<data>  เพิ่ม chunk\n";
    h+="chain:read=<tag>        อ่านทั้งหมด\n";
    h+="chain:del=<tag>         ลบ chain\n";
    h+="more:<tag>              อ่าน chunk ถัดไป\n";
    Serial.print(h);_appendChat(h);
  }

  // ── Backup commands ───────────────────────────────────────
  void cmdBackupSD(){
    speak("กำลัง_backup_ลง_SD...");
    uint16_t count=0; PUSnapshot* snaps=_buildSnapshots(count);
    if(!snaps){speak("ไม่มี_PU");return;}
    bool ok=_backup.backupToSDDirect(snaps,count,false); free(snaps);
    if(ok){char b[64];snprintf(b,sizeof(b),"Backup_SD_OK_%u_PU",count);speak(b);_logEvent(String(b));}
    else  {speak("Backup_SD_failed");_logEvent("BACKUP_SD_FAILED");}
  }

  void cmdBackupFlash(){
    speak("กำลัง_backup_ลง_Flash...");
    uint16_t count=0; PUSnapshot* snaps=_buildSnapshots(count);
    if(!snaps){speak("ไม่มี_PU");return;}
    bool ok=_backup.backupToFlash(snaps,count); free(snaps);
    uint16_t saved=min(count,(uint16_t)FLASH_PU_MAX);
    if(ok){char b[64];snprintf(b,sizeof(b),"Backup_Flash_OK_%u_PU",saved);speak(b);}
    else  speak("Backup_Flash_failed");
  }

  void cmdRestoreFlash(){
    speak("กำลัง_restore_จาก_Flash...");
    uint16_t count=0; PUSnapshot* snaps=_backup.restoreFromFlash(count);
    if(!snaps||!count){speak("ไม่พบ_backup");if(snaps)free(snaps);return;}
    _loadSnapshots(snaps,count); free(snaps);
    char b[64];snprintf(b,sizeof(b),"Restore_Flash_OK_%u_PU",count);speak(b);
  }

  void cmdBackupStatus(){speak(_backup.getStatusString());}

  // ── Command Table 15 ──────────────────────────────────────
  Command _commands[15]={
    {"hello",         &RheaBrain::cmdHello,        "ทักทาย"},
    {"status",        &RheaBrain::cmdStatus,       "สถานะระบบ"},
    {"battery",       &RheaBrain::cmdBattery,      "แบต+Temp"},
    {"move",          &RheaBrain::cmdMove,         "เดินหน้า"},
    {"stop",          &RheaBrain::cmdStop,         "หยุด"},
    {"memory",        &RheaBrain::cmdMemory,       "ดู_QA"},
    {"pu",            &RheaBrain::cmdPU,           "ดู_PU"},
    {"memreport",     &RheaBrain::cmdMemReport,    "รายงาน_memory"},
    {"goal",          &RheaBrain::cmdGoal,         "goal_ปัจจุบัน"},
    {"reset",         &RheaBrain::cmdReset,        "รีเซ็ต"},
    {"backup:sd",     &RheaBrain::cmdBackupSD,     "backup→SD"},
    {"backup:flash",  &RheaBrain::cmdBackupFlash,  "backup→Flash"},
    {"restore:flash", &RheaBrain::cmdRestoreFlash, "restore←Flash"},
    {"backup:status", &RheaBrain::cmdBackupStatus, "สถานะ_backup"},
    {"chain:list",    &RheaBrain::cmdChainList,    "แสดง_ChainUnit"},
  };
  const uint8_t _cmdCount=15;

  void cmdChainList(){chainList();}

  // ─── Process Command ──────────────────────────────────────
  void processCommand(String input){
    input.trim(); if(!input.length())return;
    _appendChat("> "+input); Serial.println("> "+input);
    String lc=input; lc.toLowerCase();

    if(lc=="help"){cmdHelp();return;}

    // ── more:<tag> ────────────────────────────────────────
    if(lc.startsWith("more:")){
      String tag=input.substring(5); tag.trim();
      chainReadNext(tag.c_str()); return;
    }

    // ── chain:add=<tag>|<data> ────────────────────────────
    if(lc.startsWith("chain:add=")){
      String val=input.substring(10);
      int sep=val.indexOf('|');
      if(sep>0){
        String tag=val.substring(0,sep);  tag.trim();
        String dat=val.substring(sep+1);  dat.trim();
        chainAppend(tag.c_str(),dat.c_str());
      } else {
        speak("รูปแบบ: chain:add=<tag>|<data>");
      }
      return;
    }

    // ── chain:read=<tag> ──────────────────────────────────
    if(lc.startsWith("chain:read=")){
      String tag=input.substring(11); tag.trim();
      chainReadAll(tag.c_str()); return;
    }

    // ── chain:del=<tag> ───────────────────────────────────
    if(lc.startsWith("chain:del=")){
      String tag=input.substring(10); tag.trim();
      chainDelete(tag.c_str()); return;
    }

    // ── = ─────────────────────────────────────────────────
    int eqPos=input.indexOf('=');
    if(eqPos>0){
      String left=input.substring(0,eqPos); left.trim();
      String right=input.substring(eqPos+1); right.trim();
      if(left.startsWith("teach:")||left.startsWith("TEACH:"))left=left.substring(6);
      if(left.length()>0&&right.length()>0){
        String lPU=_resolveToPUTag(left),rPU=_resolveToPUTag(right);
        if(lPU.length()>0&&lPU==rPU){speak("\""+left+"\" และ \""+right+"\" อยู่กลุ่มเดียวกันแล้วครับ");return;}
        if(!lPU.length()&&rPU.length()){_addAlias(rPU.c_str(),left.c_str());speak("ผูก \""+left+"\" → \""+right+"\" แล้วครับ คำตอบ: \""+_getAnswerByTag(rPU)+"\"");return;}
        if(lPU.length()&&!rPU.length()){_addAlias(lPU.c_str(),right.c_str());speak("ผูก \""+right+"\" → \""+left+"\" แล้วครับ คำตอบ: \""+_getAnswerByTag(lPU)+"\"");return;}
        if(lPU.length()&&rPU.length()){_addAlias(rPU.c_str(),left.c_str());_mergeAliases(lPU,rPU);speak("รวม \""+left+"\" เข้ากลุ่ม \""+right+"\" แล้วครับ");return;}
        learnQA(left.c_str(),right.c_str());
        speak("จำแล้วครับ \""+left+"\" = \""+right+"\"");
        return;
      }
    }

    // ── ask: / ? ─────────────────────────────────────────
    String q="";
    if(lc.startsWith("ask:"))q=input.substring(4);
    else if(lc.startsWith("?"))q=input.substring(1);
    if(q.length()){_answerQuestion(q);return;}

    // ── command table ─────────────────────────────────────
    for(uint8_t i=0;i<_cmdCount;i++)
      if(lc==_commands[i].text){(this->*_commands[i].handler)();return;}

    _answerQuestion(input);
  }

private:
  WorldState  _world;
  Goal        _goals[8];
  QAPair      _qaMemory[32];
  PatternUnit* _puStore;
  ChainUnit*   _chainStore;

  uint16_t  _puCount, _chainCount;
  uint8_t   _qaCount;
  float     _learningRate, _explorationRate, _thinkingScore;
  bool      _emergencyFlag, _isMoving;
  uint32_t  _nextPuId, _nextChainId, _lastLoopTime;
  Emotion   _emotion;
  String    _goalDesc, _chatLog, _eventLog;

  SemaphoreHandle_t _mutexPU, _mutexChain, _mutexQA, _mutexLog;
  temperature_sensor_handle_t _tempHandle;
  BackupManager _backup;

  // ── more cursor: จำว่า tag นี้อ่านถึง chunk ไหนแล้ว ──────
  // เก็บใน RAM ไม่เกิน 16 entries (ใช้บ่อยพอ)
  struct MoreState{ char tag[32]; uint8_t cursor; };
  MoreState _moreState[16];

  uint8_t _getMoreCursor(const char* tag){
    for(uint8_t i=0;i<16;i++)
      if(_strcmpi(_moreState[i].tag,tag)==0)return _moreState[i].cursor;
    return 0;
  }
  void _setMoreCursor(const char* tag, uint8_t cursor){
    for(uint8_t i=0;i<16;i++){
      if(_strcmpi(_moreState[i].tag,tag)==0){_moreState[i].cursor=cursor;return;}
    }
    // หา slot ว่าง
    for(uint8_t i=0;i<16;i++){
      if(_moreState[i].tag[0]==0){
        strncpy(_moreState[i].tag,tag,31);_moreState[i].cursor=cursor;return;
      }
    }
    // เต็ม ใช้ slot 0
    strncpy(_moreState[0].tag,tag,31);_moreState[0].cursor=cursor;
  }

  // ── หา index ของ PU จาก tag ───────────────────────────────
  int16_t _findPUIndexByTag(const char* tag){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    for(uint16_t i=0;i<_puCount;i++)
      if(_strcmpi(_puStore[i].tag,tag)==0){xSemaphoreGive(_mutexPU);return(int16_t)i;}
    // ลอง QA memory
    for(uint8_t i=0;i<_qaCount;i++)
      if(_strcmpi(_qaMemory[i].question,tag)==0){
        xSemaphoreGive(_mutexPU);
        // หา PU ที่ match
        xSemaphoreTake(_mutexPU,portMAX_DELAY);
        for(uint16_t j=0;j<_puCount;j++)
          if(_strcmpi(_puStore[j].tag,tag)==0){xSemaphoreGive(_mutexPU);return(int16_t)j;}
        xSemaphoreGive(_mutexPU);
        return -1;
      }
    xSemaphoreGive(_mutexPU);
    return -1;
  }

  // ── หา index ของ ChainUnit จาก id ─────────────────────────
  // (เรียกภายใน mutex chain เท่านั้น)
  int16_t _findChainIndexById(uint32_t id){
    for(uint16_t i=0;i<_chainCount;i++)
      if(_chainStore[i].id==id)return(int16_t)i;
    return -1;
  }

  // ── อัปเดต chunkTotal ทุก node ในสาย ─────────────────────
  void _updateChainTotal(uint32_t headId, uint8_t total){
    uint32_t curId=headId; uint8_t depth=0;
    while(curId!=0&&depth<RheaConfig::CHAIN_MAX_DEPTH){
      int16_t ci=_findChainIndexById(curId);
      if(ci<0)break;
      _chainStore[ci].chunkTotal=total;
      curId=_chainStore[ci].nextChainId;
      depth++;
    }
  }

  // ─── Build/Load Snapshots ─────────────────────────────────
  PUSnapshot* _buildSnapshots(uint16_t& count){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    count=_puCount; if(!count){xSemaphoreGive(_mutexPU);return nullptr;}
    PUSnapshot* snaps=(PUSnapshot*)ps_malloc(sizeof(PUSnapshot)*count);
    if(!snaps){xSemaphoreGive(_mutexPU);return nullptr;}
    memset(snaps,0,sizeof(PUSnapshot)*count);
    for(uint16_t i=0;i<count;i++){
      PatternUnit& s=_puStore[i]; PUSnapshot& d=snaps[i];
      d.id=s.id;d.rank=s.rank;d.accessCount=s.accessCount;
      d.createdAt=s.createdAt;d.lastAccessed=s.lastAccessed;
      d.aliasCount=s.aliasCount;d.historyCount=s.historyCount;
      d.chainHeadId=s.chainHeadId;d.chainCount=s.chainCount;
      strncpy(d.tag,s.tag,31);strncpy(d.source,s.source,23);strncpy(d.content,s.content,63);
      for(uint8_t j=0;j<s.aliasCount;j++){
        strncpy(d.aliases[j].question,s.aliases[j].question,63);
        d.aliases[j].hitCount=s.aliases[j].hitCount;
      }
      for(uint8_t j=0;j<s.historyCount;j++){
        strncpy(d.history[j].question,s.history[j].question,63);
        strncpy(d.history[j].answer,  s.history[j].answer,  63);
      }
    }
    xSemaphoreGive(_mutexPU); return snaps;
  }

  void _loadSnapshots(PUSnapshot* snaps, uint16_t count){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    uint16_t added=0;
    for(uint16_t i=0;i<count&&_puCount<RheaConfig::PU_LIMIT;i++){
      PUSnapshot& s=snaps[i];
      bool dup=false;
      for(uint16_t j=0;j<_puCount;j++)if(strcmp(_puStore[j].tag,s.tag)==0){dup=true;break;}
      if(dup)continue;
      PatternUnit& d=_puStore[_puCount++];
      memset(&d,0,sizeof(PatternUnit));
      d.id=s.id;d.rank=s.rank;d.accessCount=s.accessCount;
      d.createdAt=s.createdAt;d.lastAccessed=s.lastAccessed;
      d.aliasCount=s.aliasCount;d.historyCount=s.historyCount;
      d.chainHeadId=s.chainHeadId;d.chainCount=s.chainCount;
      d.isActive=true;
      strncpy(d.tag,s.tag,31);strncpy(d.source,s.source,23);strncpy(d.content,s.content,63);
      for(uint8_t j=0;j<s.aliasCount;j++){
        strncpy(d.aliases[j].question,s.aliases[j].question,63);
        d.aliases[j].hitCount=s.aliases[j].hitCount;
      }
      added++;
    }
    xSemaphoreGive(_mutexPU);
    Serial.printf("[Restore] %u_PU_loaded\n",added);
  }

  // ─── Create PU ───────────────────────────────────────────
  void _createPU(const char* tag,uint8_t rank,const char* source="",const char* content=""){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    PatternUnit* slot=nullptr;
    if(_puCount>=RheaConfig::PU_LIMIT){
      uint16_t evict=0; float low=999999.0f; uint32_t now=millis();
      for(uint16_t i=0;i<_puCount;i++){
        PatternUnit& p=_puStore[i];
        float s=p.rank*2.0f+p.accessCount*10.0f-(now-p.lastAccessed)/1000.0f*0.3f;
        if(s<low){low=s;evict=i;}
      }
      _logEvent("PU_evict_["+String(_puStore[evict].tag)+"]");
      slot=&_puStore[evict];
    } else {
      slot=&_puStore[_puCount++];
    }
    memset(slot,0,sizeof(PatternUnit));
    slot->id=_nextPuId++;slot->rank=rank;slot->isActive=true;
    slot->createdAt=slot->lastAccessed=millis();
    strncpy(slot->tag,tag,31);strncpy(slot->source,source,23);strncpy(slot->content,content,63);
    xSemaphoreGive(_mutexPU);
    _logEvent("PU_CREATED_["+String(tag)+"]_rank:"+String(rank));
  }

  // ─── Answer + chain notice ────────────────────────────────
  void _answerQuestion(String q){
    q.trim();
    // Exact
    for(uint8_t i=0;i<_qaCount;i++)
      if(_strcmpi(_qaMemory[i].question,q.c_str())==0){
        _qaMemory[i].askedCount++;_qaMemory[i].askedAt=millis();
        _boostPURankByTag(_qaMemory[i].question,5);
        speak(_qaMemory[i].answer);
        _notifyChainIfAny(_qaMemory[i].question); // แจ้งว่ามี chain
        _thinkingScore=min(1.0f,_thinkingScore+0.05f);
        return;
      }
    // Builtin
    String ans=_builtinAnswer(q);
    if(ans.length()){speak(ans);return;}
    // Alias
    int16_t ai=_searchAlias(q);
    if(ai>=0){
      _qaMemory[ai].askedCount++;_qaMemory[ai].askedAt=millis();
      _boostPURankByTag(_qaMemory[ai].question,4);
      speak(_qaMemory[ai].answer);
      _notifyChainIfAny(_qaMemory[ai].question);
      _thinkingScore=min(1.0f,_thinkingScore+0.04f);
      return;
    }
    // Fuzzy
    int8_t bestIdx=-1;uint8_t bestScore=0;
    for(uint8_t i=0;i<_qaCount;i++){
      uint8_t sc=_fuzzyScore(q,String(_qaMemory[i].question));
      if(sc>bestScore){bestScore=sc;bestIdx=i;}
    }
    if(bestIdx>=0&&bestScore>=RheaConfig::FUZZY_THRESHOLD){
      _qaMemory[bestIdx].askedCount++;_qaMemory[bestIdx].askedAt=millis();
      _boostPURankByTag(_qaMemory[bestIdx].question,3);
      _recordHistory(_qaMemory[bestIdx].question,q.c_str(),_qaMemory[bestIdx].answer);
      _addAlias(_qaMemory[bestIdx].question,q.c_str());
      speak("มีข้อมูลว่า \""+String(_qaMemory[bestIdx].question)+
            "\" คือ \""+String(_qaMemory[bestIdx].answer)+"\"");
      _notifyChainIfAny(_qaMemory[bestIdx].question);
      _thinkingScore=min(1.0f,_thinkingScore+0.02f);
      return;
    }
    _createPU(q.c_str(),64,"unknown","");
    speak("ไม่รู้จักครับ สอนด้วย: "+q+"=<คำตอบ>");
    _thinkingScore=max(0.0f,_thinkingScore-0.02f);
  }

  // ── แจ้งว่า PU มี chain ──────────────────────────────────
  void _notifyChainIfAny(const char* tag){
    int16_t puIdx=_findPUIndexByTag(tag);
    if(puIdx<0)return;
    if(_puStore[puIdx].chainCount>0){
      char buf[80];
      snprintf(buf,sizeof(buf),
        "→ มีข้อมูลเพิ่มเติม %u chunk พิมพ์ 'more:%s' เพื่ออ่านต่อ",
        _puStore[puIdx].chainCount, tag);
      speak(buf);
      _setMoreCursor(tag,0); // reset cursor เมื่อตอบจาก PU ใหม่
    }
  }

  // ─── Resolve / Alias / Merge ──────────────────────────────
  String _resolveToPUTag(const String& kw){
    String kl=kw; kl.toLowerCase(); kl.trim();
    for(uint8_t i=0;i<_qaCount;i++){
      String ql=String(_qaMemory[i].question);ql.toLowerCase();
      if(ql==kl)return String(_qaMemory[i].question);
    }
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    for(uint16_t i=0;i<_puCount;i++)
      for(uint8_t j=0;j<_puStore[i].aliasCount;j++){
        String al=String(_puStore[i].aliases[j].question);al.toLowerCase();al.trim();
        if(al==kl){String t=String(_puStore[i].tag);xSemaphoreGive(_mutexPU);return t;}
      }
    xSemaphoreGive(_mutexPU);return "";
  }

  String _getAnswerByTag(const String& t){
    for(uint8_t i=0;i<_qaCount;i++)
      if(_strcmpi(_qaMemory[i].question,t.c_str())==0)return String(_qaMemory[i].answer);
    return "-";
  }

  void _addAlias(const char* puTag,const char* question){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    for(uint16_t i=0;i<_puCount;i++)
      if(_strcmpi(_puStore[i].tag,puTag)==0){
        _writeAlias(_puStore[i],question);xSemaphoreGive(_mutexPU);return;
      }
    xSemaphoreGive(_mutexPU);
  }

  void _writeAlias(PatternUnit& pu,const char* q){
    for(uint8_t i=0;i<pu.aliasCount;i++)
      if(_strcmpi(pu.aliases[i].question,q)==0){pu.aliases[i].hitCount++;return;}
    if(pu.aliasCount<8){
      uint8_t idx=pu.aliasCount++;
      strncpy(pu.aliases[idx].question,q,63);
      pu.aliases[idx].firstSeenAt=millis();pu.aliases[idx].hitCount=1;
      return;
    }
    uint8_t lo=0;
    for(uint8_t i=1;i<8;i++)if(pu.aliases[i].hitCount<pu.aliases[lo].hitCount)lo=i;
    strncpy(pu.aliases[lo].question,q,63);
    pu.aliases[lo].firstSeenAt=millis();pu.aliases[lo].hitCount=1;
  }

  void _mergeAliases(const String& from,const String& to){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    for(uint16_t i=0;i<_puCount;i++)
      if(_strcmpi(_puStore[i].tag,from.c_str())==0){
        uint8_t cnt=_puStore[i].aliasCount;
        char tmp[8][64];
        for(uint8_t j=0;j<cnt;j++)strncpy(tmp[j],_puStore[i].aliases[j].question,63);
        xSemaphoreGive(_mutexPU);
        _addAlias(to.c_str(),from.c_str());
        for(uint8_t j=0;j<cnt;j++)_addAlias(to.c_str(),tmp[j]);
        return;
      }
    xSemaphoreGive(_mutexPU);
  }

  int16_t _searchAlias(const String& q){
    String ql=q;ql.toLowerCase();ql.trim();
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    for(uint16_t i=0;i<_puCount;i++)
      for(uint8_t j=0;j<_puStore[i].aliasCount;j++){
        String al=String(_puStore[i].aliases[j].question);al.toLowerCase();al.trim();
        if(al==ql){
          char pt[32];strncpy(pt,_puStore[i].tag,31);
          _puStore[i].aliases[j].hitCount++;
          _puStore[i].accessCount++;_puStore[i].lastAccessed=millis();
          xSemaphoreGive(_mutexPU);
          for(uint8_t k=0;k<_qaCount;k++)
            if(_strcmpi(_qaMemory[k].question,pt)==0)return k;
          return -1;
        }
      }
    xSemaphoreGive(_mutexPU);return -1;
  }

  void _recordHistory(const char* tag,const char* q,const char* a){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    for(uint16_t i=0;i<_puCount;i++)
      if(_strcmpi(_puStore[i].tag,tag)==0){
        uint8_t idx=_puStore[i].historyHead%3;
        strncpy(_puStore[i].history[idx].question,q,63);
        strncpy(_puStore[i].history[idx].answer,  a,63);
        _puStore[i].history[idx].askedAt=millis();
        _puStore[i].historyHead=(_puStore[i].historyHead+1)%3;
        if(_puStore[i].historyCount<3)_puStore[i].historyCount++;
        break;
      }
    xSemaphoreGive(_mutexPU);
  }

  void _boostPURankByTag(const char* tag,uint8_t amt){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    for(uint16_t i=0;i<_puCount;i++)
      if(_strcmpi(_puStore[i].tag,tag)==0){
        _puStore[i].rank=min(255,(int)_puStore[i].rank+amt);
        _puStore[i].accessCount++;_puStore[i].lastAccessed=millis();break;
      }
    xSemaphoreGive(_mutexPU);
  }

  bool _boostPURankById(uint32_t id,uint8_t amt){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    for(uint16_t i=0;i<_puCount;i++)
      if(_puStore[i].id==id){
        _puStore[i].rank=min(255,(int)_puStore[i].rank+amt);
        _puStore[i].accessCount++;_puStore[i].lastAccessed=millis();
        xSemaphoreGive(_mutexPU);return true;
      }
    xSemaphoreGive(_mutexPU);return false;
  }

  uint8_t _fuzzyScore(String a,String b){
    a.toLowerCase();a.trim();b.toLowerCase();b.trim();
    if(a==b)return 100;
    uint8_t sc=0;
    if(b.indexOf(a)>=0)sc=max(sc,(uint8_t)85);
    if(a.indexOf(b)>=0)sc=max(sc,(uint8_t)85);
    sc=max(sc,_countTokenMatch(a,b));
    sc=max(sc,_bigramScore(a,b));
    return sc;
  }

  uint8_t _countTokenMatch(String a,String b){
    if(!a.length()||!b.length())return 0;
    uint8_t total=0,matched=0;String tok="";
    for(uint16_t i=0;i<=a.length();i++){
      char c=(i<a.length())?a[i]:' ';
      if(c==' '){if(tok.length()>=2){total++;if(b.indexOf(tok)>=0)matched++;}tok="";}
      else tok+=c;
    }
    if(!total){
      for(uint8_t len=min(a.length(),b.length());len>=2;len--)
        if(b.indexOf(a.substring(0,len))>=0)return(uint8_t)((float)len/a.length()*70.0f);
      return 0;
    }
    return(uint8_t)((float)matched/total*80.0f);
  }

  uint8_t _bigramScore(String a,String b){
    if(a.length()<2||b.length()<2)return 0;
    uint8_t total=a.length()-1,matched=0;
    for(uint8_t i=0;i<total;i++)if(b.indexOf(a.substring(i,i+2))>=0)matched++;
    return(uint8_t)((float)matched/total*60.0f);
  }

  String _builtinAnswer(String q){
    q.toLowerCase();char buf[80];
    if(q.indexOf("battery")>=0||q.indexOf("แบต")>=0){snprintf(buf,sizeof(buf),"Battery_%.0f%%_Temp_%.1fC",_world.batteryPercent,_world.mcuTemp);return String(buf);}
    if(q.indexOf("temp")>=0||q.indexOf("ร้อน")>=0){snprintf(buf,sizeof(buf),"MCU_Temp_%.1fC",_world.mcuTemp);return String(buf);}
    if(q.indexOf("ram")>=0){snprintf(buf,sizeof(buf),"RAM:%uB_PSRAM:%uB",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());return String(buf);}
    if(q.indexOf("uptime")>=0){snprintf(buf,sizeof(buf),"Uptime_%lu_s",millis()/1000);return String(buf);}
    if(q.indexOf("moving")>=0||q.indexOf("เดิน")>=0)return _isMoving?"กำลังเดินอยู่":"หยุดนิ่งอยู่";
    if(q.indexOf("ขอบคุณ")>=0||q.indexOf("thank")>=0)return "ยินดีครับ";
    return "";
  }

  void _appendChat(String msg){
    xSemaphoreTake(_mutexLog,portMAX_DELAY);
    _chatLog+=msg+"\n";
    if(_chatLog.length()>RheaConfig::LOG_CHAT_MAX)
      _chatLog=_chatLog.substring(_chatLog.length()-RheaConfig::LOG_CHAT_MAX+500);
    xSemaphoreGive(_mutexLog);
  }

  void _logEvent(String ev){
    String msg="["+String(millis()/1000)+"s]_"+ev;
    Serial.println(msg);
    xSemaphoreTake(_mutexLog,portMAX_DELAY);
    _eventLog+=msg+"\n";
    if(_eventLog.length()>RheaConfig::LOG_EVENT_MAX)
      _eventLog=_eventLog.substring(_eventLog.length()-RheaConfig::LOG_EVENT_MAX+500);
    xSemaphoreGive(_mutexLog);
  }

  void _log(const char* lv,const char* msg){Serial.printf("[%8lu][%-5s]_%s\n",millis(),lv,msg);}

  // ─── Status JSON ──────────────────────────────────────────
  String _buildStatusJSON(){
    char buf[400];
    snprintf(buf,sizeof(buf),
      "{\"ram\":%u,\"psram_free\":%u,"
      "\"pu\":%u,\"pu_max\":%u,"
      "\"chain\":%u,\"chain_max\":%u,"
      "\"batt\":%.1f,\"mcu_temp\":%.1f,"
      "\"lr\":%.2f,\"score\":%.2f,\"qa\":%u,"
      "\"moving\":%s,\"emergency\":%s,"
      "\"emotion\":\"%s\",\"goal\":\"%s\","
      "\"uptime\":%lu,\"sd_ready\":%s}",
      (unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram(),
      (unsigned)_puCount,RheaConfig::PU_LIMIT,
      (unsigned)_chainCount,RheaConfig::CHAIN_LIMIT,
      _world.batteryPercent,_world.mcuTemp,
      _learningRate,_thinkingScore,(unsigned)_qaCount,
      _isMoving?"true":"false",_emergencyFlag?"true":"false",
      emotionName[_emotion],_goalDesc.c_str(),
      millis()/1000,_backup.isSDReady()?"true":"false");
    return String(buf);
  }

  // ─── PU JSON paginated ─────────────────────────────────────
  String _buildPUJson(uint16_t page,uint16_t size){
    xSemaphoreTake(_mutexPU,portMAX_DELAY);
    uint16_t start=page*size,end=min((uint16_t)(start+size),_puCount);
    String j="{\"total\":"+String(_puCount)+",\"page\":"+String(page)+",\"items\":[";
    for(uint16_t i=start;i<end;i++){
      if(i>start)j+=",";
      PatternUnit& p=_puStore[i];
      j+="{\"id\":"+String(p.id)+
         ",\"tag\":\""+String(p.tag)+"\""+
         ",\"rank\":"+String(p.rank)+
         ",\"source\":\""+String(p.source)+"\""+
         ",\"content\":\""+String(p.content)+"\""+
         ",\"accessed\":"+String(p.accessCount)+
         ",\"age\":"+String((millis()-p.createdAt)/1000)+
         ",\"alias_count\":"+String(p.aliasCount)+
         ",\"chain_count\":"+String(p.chainCount)+
         ",\"chain_head\":"+String(p.chainHeadId)+"}";
    }
    j+="]}"; xSemaphoreGive(_mutexPU); return j;
  }

  // ─── Chain JSON paginated ─────────────────────────────────
  String _buildChainJson(uint16_t page,uint16_t size){
    xSemaphoreTake(_mutexChain,portMAX_DELAY);
    uint16_t start=page*size,end=min((uint16_t)(start+size),_chainCount);
    String j="{\"total\":"+String(_chainCount)+
              ",\"chain_max\":"+String(RheaConfig::CHAIN_LIMIT)+
              ",\"page\":"+String(page)+",\"items\":[";
    for(uint16_t i=start;i<end;i++){
      if(i>start)j+=",";
      ChainUnit& c=_chainStore[i];
      String dataStr=String(c.data);
      dataStr.replace("\"","\\\"");
      j+="{\"id\":"+String(c.id)+
         ",\"pu_tag\":\""+String(c.puTag)+"\""+
         ",\"chunk\":"+String(c.chunkIndex)+
         ",\"total\":"+String(c.chunkTotal)+
         ",\"next\":"+String(c.nextChainId)+
         ",\"age\":"+String((millis()-c.createdAt)/1000)+
         ",\"data\":\""+dataStr+"\"}";
    }
    j+="]}"; xSemaphoreGive(_mutexChain); return j;
  }

  void _seedDefaultKnowledge(){
    learnQA("ชื่ออะไร",      "ผมชื่อ_RHEA_ครับ_Robot_Humanoid_Evolving_Autonomously");
    learnQA("คุณคือใคร",     "ผมคือ_RHEA_หุ่นยนต์ฮิวแมนนอยด์ที่เรียนรู้ได้ครับ");
    learnQA("ทำอะไรได้บ้าง","เดิน_หยิบของ_คุย_เรียนรู้จากคุณได้ครับ");
    learnQA("สวัสดี",        "สวัสดีครับ_มีอะไรให้ช่วยไหมครับ");
    learnQA("hello",         "Hello_I_am_RHEA_How_can_I_help");
    _addAlias("สวัสดี","hi");_addAlias("สวัสดี","hey");
    _addAlias("hello","hi"); _addAlias("hello","hey");
    _log("INFO","Default_knowledge_loaded");
  }

  int _strcmpi(const char* a,const char* b){
    while(*a&&*b){
      char ca=(*a>='A'&&*a<='Z')?*a+32:*a;
      char cb=(*b>='A'&&*b<='Z')?*b+32:*b;
      if(ca!=cb)return ca-cb;a++;b++;
    }
    return *a-*b;
  }

  // ─── HTML ─────────────────────────────────────────────────
  String _buildHTML(){
    return R"rawhtml(
<!DOCTYPE html><html lang="th">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RHEA_v0.2.1</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3;padding:12px;font-size:14px}
h1{font-size:18px;font-weight:500;margin-bottom:12px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:8px;margin-bottom:12px}
.card{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:10px 12px}
.lbl{font-size:11px;color:#8b949e;margin-bottom:4px}.val{font-size:15px;font-weight:500;color:#fff}
.ok{color:#56d364}.warn{color:#f0883e}.err{color:#ff7b72}.blue{color:#79c0ff}.purple{color:#d2a8ff}
.tabs{display:flex;gap:4px;margin-bottom:8px;flex-wrap:wrap}
.tab{background:#21262d;border:1px solid #30363d;border-radius:6px;color:#8b949e;font-size:12px;padding:5px 12px;cursor:pointer}
.tab.active{background:#1f6feb;border-color:#1f6feb;color:#fff}
.pane{display:none}.pane.active{display:block}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.box{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:10px;margin-bottom:8px}
.box-t{font-size:11px;color:#8b949e;margin-bottom:6px;font-weight:500}
textarea{width:100%;background:#0d1117;border:1px solid #30363d;border-radius:6px;color:#e6edf3;font-family:monospace;font-size:11.5px;resize:none;padding:8px;line-height:1.6}
.irow{display:flex;gap:6px;margin-top:8px}
input{flex:1;background:#0d1117;border:1px solid #30363d;border-radius:6px;color:#e6edf3;font-size:13px;padding:7px 10px;outline:none}
input:focus{border-color:#388bfd}
button{background:#1f6feb;border:none;border-radius:6px;color:#fff;font-size:13px;padding:7px 14px;cursor:pointer}
button:hover{background:#388bfd}
.quick{display:flex;flex-wrap:wrap;gap:5px;margin-top:8px}
.qbtn{background:#21262d;border:1px solid #30363d;border-radius:20px;color:#8b949e;font-size:11px;padding:4px 10px;cursor:pointer}
.qbtn:hover{border-color:#388bfd;color:#79c0ff}
.qbtn.grn{border-color:#238636;color:#56d364}.qbtn.grn:hover{background:#238636;color:#fff}
.qbtn.ora{border-color:#9e6a03;color:#f0883e}.qbtn.ora:hover{background:#9e6a03;color:#fff}
.qbtn.pur{border-color:#6e40c9;color:#d2a8ff}.qbtn.pur:hover{background:#6e40c9;color:#fff}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px}
.pu-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:6px}
.pu-card{border:1px solid #30363d;border-radius:8px;padding:8px 10px;font-size:12px;background:#0d1117}
.pu-card.has-chain{border-color:#6e40c9}
.pu-id{font-weight:500;margin-bottom:4px;color:#79c0ff}
.pu-row{color:#8b949e;margin-bottom:2px}.pu-row span{color:#e6edf3}
.rank-bar{height:4px;border-radius:2px;background:#30363d;margin-top:5px}
.rank-fill{height:4px;border-radius:2px;background:#388bfd}
.chain-badge{background:#3d1f7a;color:#d2a8ff;font-size:10px;padding:1px 6px;border-radius:10px;margin-left:4px}
.page-ctrl{display:flex;gap:6px;align-items:center;margin-bottom:8px}
.hint{font-size:11px;color:#8b949e;margin-top:6px;line-height:1.7}
.backup-bar{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:8px;padding:8px;background:#161b22;border:1px solid #30363d;border-radius:8px}
.backup-label{font-size:11px;color:#8b949e;align-self:center}
@media(max-width:500px){.row2{grid-template-columns:1fr}}
</style>
</head>
<body>
<h1><span class="dot ok" id="dot"></span>RHEA_v0.2.1_PSRAM+Chain+Backup</h1>

<div class="grid">
  <div class="card"><div class="lbl">RAM_Free</div><div class="val blue" id="ram">-</div></div>
  <div class="card"><div class="lbl">PSRAM_Free</div><div class="val purple" id="psram">-</div></div>
  <div class="card"><div class="lbl">PU_Total</div><div class="val blue" id="pu">-</div></div>
  <div class="card"><div class="lbl">Chain_Units</div><div class="val purple" id="chain">-</div></div>
  <div class="card"><div class="lbl">QA_Pairs</div><div class="val blue" id="qa">-</div></div>
  <div class="card"><div class="lbl">Battery</div><div class="val ok" id="batt">-</div></div>
  <div class="card"><div class="lbl">MCU_Temp</div><div class="val ok" id="mcu_temp">-</div></div>
  <div class="card"><div class="lbl">Emotion</div><div class="val" id="emotion">-</div></div>
  <div class="card"><div class="lbl">Score</div><div class="val" id="score">-</div></div>
  <div class="card"><div class="lbl">Status</div><div class="val ok" id="estat">-</div></div>
  <div class="card"><div class="lbl">SD_Card</div><div class="val" id="sdstat">-</div></div>
</div>

<div class="backup-bar">
  <span class="backup-label">💾 Backup:</span>
  <button style="background:#238636;font-size:12px;padding:5px 12px" onclick="sc('backup:sd')">↑ SD</button>
  <button style="background:#1f6feb;font-size:12px;padding:5px 12px" onclick="sc('backup:flash')">↑ Flash</button>
  <button style="background:#6e40c9;font-size:12px;padding:5px 12px" onclick="sc('restore:flash')">↓ Flash</button>
  <button style="background:#21262d;border:1px solid #30363d;font-size:12px;padding:5px 12px" onclick="sc('backup:status')">Status</button>
  <span class="backup-label" style="margin-left:8px">⛓ Chain:</span>
  <button style="background:#3d1f7a;font-size:12px;padding:5px 12px" onclick="sc('chain:list')">List</button>
</div>

<div class="tabs">
  <div class="tab active" onclick="showTab('chat',this)">Chat</div>
  <div class="tab" onclick="showTab('events',this)">Events</div>
  <div class="tab" onclick="showTab('pu',this)">Pattern_Units</div>
  <div class="tab" onclick="showTab('chain',this)">Chain_Units</div>
</div>

<div class="pane active" id="tab-chat">
  <div class="row2">
    <div class="box">
      <div class="box-t">Chat</div>
      <textarea id="chat" rows="12" readonly></textarea>
      <div class="irow">
        <input id="cmd" placeholder="Q=A | chain:add=tag|data | more:tag"
          onkeydown="if(event.key==='Enter')send()"/>
        <button onclick="send()">ส่ง</button>
      </div>
      <div class="hint">
        สอน: <b>q=a</b> | ผูก: <b>hi=hello</b> | เพิ่ม chunk: <b>chain:add=tag|data</b> | อ่านต่อ: <b>more:tag</b>
      </div>
      <div class="quick">
        <span class="qbtn" onclick="sc('hello')">hello</span>
        <span class="qbtn" onclick="sc('status')">status</span>
        <span class="qbtn" onclick="sc('memreport')">memreport</span>
        <span class="qbtn" onclick="sc('move')">move</span>
        <span class="qbtn" onclick="sc('stop')">stop</span>
        <span class="qbtn" onclick="sc('memory')">memory</span>
        <span class="qbtn" onclick="sc('pu')">pu</span>
        <span class="qbtn" onclick="sc('reset')">reset</span>
        <span class="qbtn" onclick="sc('help')">help</span>
        <span class="qbtn grn" onclick="sc('backup:sd')">backup:sd</span>
        <span class="qbtn grn" onclick="sc('backup:flash')">backup:flash</span>
        <span class="qbtn ora" onclick="sc('restore:flash')">restore:flash</span>
        <span class="qbtn pur" onclick="sc('chain:list')">chain:list</span>
      </div>
    </div>
    <div class="box">
      <div class="box-t">Event_Log</div>
      <textarea id="events2" rows="16" readonly></textarea>
    </div>
  </div>
</div>

<div class="pane" id="tab-events">
  <div class="box"><div class="box-t">Event_Log_Full</div>
    <textarea id="events" rows="22" readonly></textarea>
  </div>
</div>

<div class="pane" id="tab-pu">
  <div class="box">
    <div class="box-t">Pattern_Units <span id="pu-total-label" style="color:#79c0ff"></span></div>
    <div class="page-ctrl">
      <button onclick="prevPage('pu')">← Prev</button>
      <span id="pu-page-label" style="font-size:12px;color:#8b949e">Page_1</span>
      <button onclick="nextPage('pu')">Next →</button>
    </div>
    <div id="pu-grid" class="pu-grid"></div>
  </div>
</div>

<div class="pane" id="tab-chain">
  <div class="box">
    <div class="box-t">Chain_Units <span id="chain-total-label" style="color:#d2a8ff"></span></div>
    <div class="page-ctrl">
      <button onclick="prevPage('chain')">← Prev</button>
      <span id="chain-page-label" style="font-size:12px;color:#8b949e">Page_1</span>
      <button onclick="nextPage('chain')">Next →</button>
    </div>
    <div id="chain-grid" class="pu-grid"></div>
  </div>
</div>

<script>
let lastChat='',lastEvent='';
let puPage=0,chainPage=0;
const pageSize=20;

function showTab(name,el){
  document.querySelectorAll('.pane').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  document.getElementById('tab-'+name).classList.add('active');
  el.classList.add('active');
  if(name==='pu')refreshPU();
  if(name==='chain')refreshChain();
}

function sc(c){sendCmd(c);}
async function sendCmd(c){
  appendChat('> '+c);
  await fetch('/api/cmd',{method:'POST',body:c});
  await refreshChat();await refreshEvents();
}
async function send(){
  const c=document.getElementById('cmd').value.trim();
  if(!c)return;
  document.getElementById('cmd').value='';
  await sendCmd(c);
}

async function refreshStatus(){
  try{
    const d=await fetch('/api/status').then(r=>r.json());
    document.getElementById('ram').textContent=(d.ram/1024).toFixed(1)+' KB';
    document.getElementById('psram').textContent=(d.psram_free/1024/1024).toFixed(2)+' MB';
    document.getElementById('pu').textContent=d.pu+'/'+d.pu_max;
    document.getElementById('chain').textContent=d.chain+'/'+d.chain_max;
    document.getElementById('qa').textContent=d.qa;
    document.getElementById('batt').textContent=d.batt.toFixed(0)+'%';
    const te=document.getElementById('mcu_temp');
    te.textContent=d.mcu_temp.toFixed(1)+' C';
    te.className=d.mcu_temp<60?'val ok':d.mcu_temp<80?'val warn':'val err';
    document.getElementById('emotion').textContent=d.emotion;
    document.getElementById('score').textContent=d.score.toFixed(2);
    const es=document.getElementById('estat');
    if(d.emergency){es.textContent='STOPPED';es.className='val err';}
    else if(d.moving){es.textContent='MOVING';es.className='val warn';}
    else{es.textContent='IDLE';es.className='val ok';}
    document.getElementById('dot').style.background=d.emergency?'#ff7b72':'#56d364';
    const sd=document.getElementById('sdstat');
    sd.textContent=d.sd_ready?'Ready':'No_SD';
    sd.className=d.sd_ready?'val ok':'val warn';
    if(d.psram_free<2500000)document.getElementById('psram').className='val warn';
  }catch(e){}
}

async function refreshChat(){
  try{
    const t=await fetch('/api/chat').then(r=>r.text());
    if(t!==lastChat){lastChat=t;const el=document.getElementById('chat');el.value=t;el.scrollTop=el.scrollHeight;}
  }catch(e){}
}
async function refreshEvents(){
  try{
    const t=await fetch('/api/events').then(r=>r.text());
    if(t!==lastEvent){lastEvent=t;
      ['events','events2'].forEach(id=>{const el=document.getElementById(id);if(el){el.value=t;el.scrollTop=el.scrollHeight;}});
    }
  }catch(e){}
}

async function refreshPU(){
  try{
    const d=await fetch('/api/pu?page='+puPage+'&size='+pageSize).then(r=>r.json());
    document.getElementById('pu-total-label').textContent='('+d.total+'/1200)';
    document.getElementById('pu-page-label').textContent='Page_'+(puPage+1)+'/'+Math.ceil(d.total/pageSize||1);
    const grid=document.getElementById('pu-grid');
    if(!d.items.length){grid.innerHTML='<p style="color:#8b949e">ยังไม่มี_PU</p>';return;}
    grid.innerHTML=d.items.map(p=>{
      const hasChain=p.chain_count>0;
      return '<div class="pu-card'+(hasChain?' has-chain':'')+'">'+
        '<div class="pu-id">PU#'+p.id+
        (hasChain?'<span class="chain-badge">⛓'+p.chain_count+' chunks</span>':'')+
        '</div>'+
        '<div class="pu-row">tag:<span>'+p.tag+'</span></div>'+
        '<div class="pu-row">content:<span>'+(p.content||'-')+'</span></div>'+
        '<div class="pu-row">rank:<span>'+p.rank+'/255</span></div>'+
        '<div class="pu-row">alias:<span>'+p.alias_count+'</span></div>'+
        '<div class="pu-row">age:<span>'+p.age+'s</span></div>'+
        '<div class="rank-bar"><div class="rank-fill" style="width:'+(p.rank/255*100)+'%"></div></div>'+
        (hasChain?'<div style="margin-top:5px"><button style="font-size:10px;padding:2px 8px;background:#3d1f7a" onclick="sc(\'more:'+p.tag+'\')">more:'+p.tag+'</button></div>':'')+
        '</div>';
    }).join('');
  }catch(e){}
}

async function refreshChain(){
  try{
    const d=await fetch('/api/chain?page='+chainPage+'&size='+pageSize).then(r=>r.json());
    document.getElementById('chain-total-label').textContent='('+d.total+'/'+d.chain_max+')';
    document.getElementById('chain-page-label').textContent='Page_'+(chainPage+1)+'/'+Math.ceil(d.total/pageSize||1);
    const grid=document.getElementById('chain-grid');
    if(!d.items.length){grid.innerHTML='<p style="color:#8b949e">ยังไม่มี_ChainUnit</p>';return;}
    grid.innerHTML=d.items.map(c=>'<div class="pu-card" style="border-color:#6e40c9">'+
      '<div class="pu-id" style="color:#d2a8ff">CU#'+c.id+'</div>'+
      '<div class="pu-row">PU:<span>'+c.pu_tag+'</span></div>'+
      '<div class="pu-row">chunk:<span>'+c.chunk+'/'+c.total+'</span></div>'+
      '<div class="pu-row">next:<span>'+(c.next?'CU#'+c.next:'END')+'</span></div>'+
      '<div class="pu-row">age:<span>'+c.age+'s</span></div>'+
      '<div style="margin-top:6px;background:#0d1117;border-radius:4px;padding:4px 6px;font-size:11px;color:#e6edf3;word-break:break-all">'+c.data+'</div>'+
      '</div>'
    ).join('');
  }catch(e){}
}

function prevPage(t){if(t==='pu'&&puPage>0){puPage--;refreshPU();}if(t==='chain'&&chainPage>0){chainPage--;refreshChain();}}
function nextPage(t){if(t==='pu'){puPage++;refreshPU();}if(t==='chain'){chainPage++;refreshChain();}}
function appendChat(t){const el=document.getElementById('chat');if(el){el.value+=t+'\n';el.scrollTop=el.scrollHeight;}}

setInterval(refreshStatus,1000);
setInterval(()=>{refreshChat();refreshEvents();},1500);
refreshStatus();refreshChat();refreshEvents();
</script>
</body></html>
)rawhtml";
  }
};

// ── FreeRTOS Tasks ────────────────────────────────────────────
void taskSafety(void* p){RheaBrain* r=static_cast<RheaBrain*>(p);for(;;){r->monitorSafety();vTaskDelay(pdMS_TO_TICKS(500));}}
void taskPerception(void* p){RheaBrain* r=static_cast<RheaBrain*>(p);for(;;){if(r->isSafeToOperate())r->processPerception();vTaskDelay(pdMS_TO_TICKS(100));}}
void taskCognition(void* p){RheaBrain* r=static_cast<RheaBrain*>(p);for(;;){if(r->isSafeToOperate())r->prioritizeGoals();vTaskDelay(pdMS_TO_TICKS(200));}}
void taskLearning(void* p){
  RheaBrain* r=static_cast<RheaBrain*>(p);
  vTaskDelay(pdMS_TO_TICKS(5000));
  for(;;){r->runAutoBackup();vTaskDelay(pdMS_TO_TICKS(5000));}
}
void taskWebServer(void* p){RheaBrain* r=static_cast<RheaBrain*>(p);for(;;){r->handleWebServer();vTaskDelay(pdMS_TO_TICKS(10));}}

// ── Entry Point ───────────────────────────────────────────────
RheaBrain rhea;
String    input="";

void setup(){
  if(!rhea.initSystem()){Serial.println("[FATAL]_Restarting");delay(3000);ESP.restart();}
  rhea.activateGoal(0);
  Serial.println("================================================");
  Serial.println("   RHEA_v0.2.1_Full_PSRAM+Chain+Backup");
  Serial.println("   WiFi  : RHEA_Brain  Pass: rhea1234");
  Serial.println("   Web   : http://192.168.4.1");
  Serial.println("   สอน   : Q=A  |  ผูก: Q=Q2  |  ถาม: Q");
  Serial.println("   Chain : chain:add=<tag>|<data>");
  Serial.println("           chain:read=<tag>");
  Serial.println("           chain:del=<tag>");
  Serial.println("           more:<tag>  ← อ่านต่อ chunk ถัดไป");
  Serial.println("   Backup: backup:sd | backup:flash");
  Serial.println("           restore:flash");
  Serial.println("================================================");
}

void loop(){
  while(Serial.available()){
    char c=Serial.read();
    if(c=='\n'||c=='\r'){if(input.length()){rhea.processCommand(input);input="";}}
    else input+=c;
  }
  delay(10);
}
