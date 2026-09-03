// ═══════════════════════════════════════════════════════════
//  EASY CODE OS v0.1 - ESP32-C3
//  Single File Version
//  
//  Libraries needed:
//    - U8g2  (OLED driver)
//    - SPIFFS (built-in ESP32)
//
//  Board: ESP32C3 Dev Module
// ═══════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPIFFS.h>
#include <U8g2lib.h>
#include <Wire.h>

// ╔═══════════════════════════════════════════════════════════╗
// ║  CONFIG                                                   ║
// ╚═══════════════════════════════════════════════════════════╝

// ── OLED ──────────────────────────────────────────────────
#define OLED_SDA     8
#define OLED_SCL     9
#define OLED_WIDTH  72
#define OLED_HEIGHT 40

// ── BUTTONS (ADC Resistor Ladder) ─────────────────────────
#define BTN_LEFT_PIN   0    // GPIO0 = ADC1_CH0
#define BTN_RIGHT_PIN  1    // GPIO1 = ADC1_CH1

#define BTN_NONE    0
#define BTN_L       1
#define BTN_UP      2
#define BTN_DOWN    3
#define BTN_LEFT    4
#define BTN_RIGHT   5
#define BTN_PAUSE   6
#define BTN_R       7
#define BTN_SELECT  8
#define BTN_CANCEL  9
#define BTN_PLAY   10

// ADC thresholds (12-bit 0-4095)
#define ADC_NONE  3800
#define ADC_BTN1  3000
#define ADC_BTN2  2200
#define ADC_BTN3  1400
#define ADC_BTN4   600

// ── SCRIPT ENGINE ─────────────────────────────────────────
#define MAX_ROWS       16
#define SCRIPT_SIZE   100
#define MAX_VARS      256
#define CACHE_SIZE     16
#define MAX_CALLSTACK 1611

// ── MEMORY WARNING ────────────────────────────────────────
#define MEM_WARN_KB  50000
#define MEM_STOP_KB  20000

// ── SPIFFS ────────────────────────────────────────────────
#define SCRIPT_DIR  "/s/"

// ── DISPLAY ───────────────────────────────────────────────
#define MAX_CHARS_PER_LINE  15
#define MAX_LINES            6

// ── SYSTEM ────────────────────────────────────────────────
#define OS_VERSION  "0.1"
#define OS_NAME     "ECOS"

// ╔═══════════════════════════════════════════════════════════╗
// ║  VARIABLE TYPES                                           ║
// ╚═══════════════════════════════════════════════════════════╝

#define VTYPE_INT16  0
#define VTYPE_FLOAT  1
#define VTYPE_BOOL   2

#define VREF_VAR    0
#define VREF_CONST  1
#define VREF_GPIO   2
#define VREF_STR    3

// ── Variable struct (8 bytes) ──────────────────────────────
struct Variable {
  uint32_t nameEnc;
  union {
    int16_t  asInt;
    float    asFloat;
    uint8_t  asBool;
  } val;
  uint8_t type;
  uint8_t pad;
};

// ── Encode var name → uint32 ───────────────────────────────
// หลัก 1: A-Z (5 bits)  หลัก 2-4: A-Z+0-9 (6 bits each)
uint32_t encodeVarName(const char* name) {
  uint8_t c0 = (name[0] >= 'A' && name[0] <= 'Z')
               ? name[0] - 'A' : 0;
  auto enc = [](char c) -> uint8_t {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    return 35;
  };
  uint8_t c1 = name[1] ? enc(name[1]) : 35;
  uint8_t c2 = name[2] ? enc(name[2]) : 35;
  uint8_t c3 = name[3] ? enc(name[3]) : 35;
  return ((uint32_t)c0 << 18) | ((uint32_t)c1 << 12) |
         ((uint32_t)c2 <<  6) | ((uint32_t)c3);
}

void decodeVarName(uint32_t enc, char* out) {
  const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  out[0] = 'A' + ((enc >> 18) & 0x1F);
  out[1] = alpha[(enc >> 12) & 0x3F];
  out[2] = alpha[(enc >>  6) & 0x3F];
  out[3] = alpha[(enc      ) & 0x3F];
  out[4] = '\0';
}

// ── Variable Pool ──────────────────────────────────────────
class VarPool {
public:
  Variable pool[MAX_VARS];
  uint16_t count = 0;

  void clear() { count = 0; }

  int16_t find(uint32_t nameEnc) {
    for (uint16_t i = 0; i < count; i++)
      if (pool[i].nameEnc == nameEnc) return i;
    return -1;
  }

  bool setInt(const char* name, int16_t value) {
    uint32_t enc = encodeVarName(name);
    int16_t idx = find(enc);
    if (idx < 0) {
      if (count >= MAX_VARS) return false;
      idx = count++;
      pool[idx].nameEnc = enc;
    }
    pool[idx].val.asInt = value;
    pool[idx].type = VTYPE_INT16;
    return true;
  }

  bool setFloat(const char* name, float value) {
    uint32_t enc = encodeVarName(name);
    int16_t idx = find(enc);
    if (idx < 0) {
      if (count >= MAX_VARS) return false;
      idx = count++;
      pool[idx].nameEnc = enc;
    }
    pool[idx].val.asFloat = value;
    pool[idx].type = VTYPE_FLOAT;
    return true;
  }

  bool setBool(const char* name, bool value) {
    uint32_t enc = encodeVarName(name);
    int16_t idx = find(enc);
    if (idx < 0) {
      if (count >= MAX_VARS) return false;
      idx = count++;
      pool[idx].nameEnc = enc;
    }
    pool[idx].val.asBool = value ? 1 : 0;
    pool[idx].type = VTYPE_BOOL;
    return true;
  }

  float getFloat(const char* name, float defVal = 0.0f) {
    uint32_t enc = encodeVarName(name);
    int16_t idx = find(enc);
    if (idx < 0) return defVal;
    switch (pool[idx].type) {
      case VTYPE_INT16: return (float)pool[idx].val.asInt;
      case VTYPE_FLOAT: return pool[idx].val.asFloat;
      case VTYPE_BOOL:  return (float)pool[idx].val.asBool;
    }
    return defVal;
  }

  int16_t getInt(const char* name, int16_t defVal = 0) {
    return (int16_t)getFloat(name, (float)defVal);
  }
};

VarPool gVars;

// ╔═══════════════════════════════════════════════════════════╗
// ║  SCRIPT STRUCT & OPCODES                                  ║
// ╚═══════════════════════════════════════════════════════════╝

enum OpCode : uint8_t {
  // Math
  OP_SET   = 0x01,  // VAR = val
  OP_ADD   = 0x02,  // VAR = v1 + v2
  OP_SUB   = 0x03,  // VAR = v1 - v2
  OP_MUL   = 0x04,  // VAR = v1 * v2
  OP_DIV   = 0x05,  // VAR = v1 / v2
  OP_MOD   = 0x06,  // VAR = v1 % v2
  // Logic
  OP_AND   = 0x10,
  OP_OR    = 0x11,
  OP_NOT   = 0x12,
  // Compare → skip next row if true
  OP_IF_GT = 0x20,
  OP_IF_LT = 0x21,
  OP_IF_EQ = 0x22,
  OP_IF_NE = 0x23,
  // Flow
  OP_JUMP  = 0x24,  // PC = v1
  OP_CALL  = 0x25,  // call script name in v1
  OP_END   = 0x26,  // return to caller
  OP_HALT  = 0x27,  // stop
  // GPIO
  OP_GPOW  = 0x30,  // gpio_write(pin, val)
  OP_GPOR  = 0x31,  // VAR = gpio_read(pin)
  OP_GPOM  = 0x32,  // gpio_mode(pin, IN/OUT)
  OP_PWM   = 0x33,  // pwm(pin, duty)
  OP_ADC   = 0x34,  // VAR = adc_read(pin)
  // Time
  OP_WAIT  = 0x40,  // delay(ms)
  OP_TIME  = 0x41,  // VAR = millis()
  // Display
  OP_PRINT = 0x50,  // print to OLED
  OP_CLS   = 0x51,  // clear OLED
  // Serial
  OP_SOUT  = 0x60,
  OP_SIN   = 0x61,
  // WiFi
  OP_WGET  = 0x70,
};

// ── Row (6 bytes) ──────────────────────────────────────────
struct Row {
  uint8_t  op;      // OpCode
  uint8_t  vtype;   // [3:0]=type v1  [7:4]=type v2
  uint16_t v1;
  uint16_t v2;
};

// ── Script Header (4 bytes) ───────────────────────────────
struct ScriptHeader {
  uint8_t  rowCount;
  uint8_t  version;
  uint16_t checksum;
};

// ── Script (100 bytes total) ──────────────────────────────
struct Script {
  ScriptHeader hdr;           //  4 bytes
  Row          rows[MAX_ROWS]; // 96 bytes
};                             // = 100 bytes ✅

// ── Cache Entry (106 bytes) ───────────────────────────────
struct CacheEntry {
  char   name[5];
  Script data;
  bool   valid;
};

// ── Call Stack Frame (8 bytes) ────────────────────────────
struct CallFrame {
  char    scriptName[5];
  uint8_t PC;
  uint8_t pad[2];
};

// ── Op name for editor ────────────────────────────────────
const char* opName(OpCode op) {
  switch(op) {
    case OP_SET:   return "SET";
    case OP_ADD:   return "ADD";
    case OP_SUB:   return "SUB";
    case OP_MUL:   return "MUL";
    case OP_DIV:   return "DIV";
    case OP_MOD:   return "MOD";
    case OP_AND:   return "AND";
    case OP_OR:    return "OR";
    case OP_NOT:   return "NOT";
    case OP_IF_GT: return "IF>";
    case OP_IF_LT: return "IF<";
    case OP_IF_EQ: return "IF=";
    case OP_IF_NE: return "IF!";
    case OP_JUMP:  return "JMP";
    case OP_CALL:  return "CALL";
    case OP_END:   return "END";
    case OP_HALT:  return "HALT";
    case OP_GPOW:  return "GOUT";
    case OP_GPOR:  return "GIN";
    case OP_GPOM:  return "GMOD";
    case OP_PWM:   return "PWM";
    case OP_ADC:   return "ADC";
    case OP_WAIT:  return "WAIT";
    case OP_TIME:  return "TIME";
    case OP_PRINT: return "PRNT";
    case OP_CLS:   return "CLS";
    case OP_SOUT:  return "SOUT";
    case OP_SIN:   return "SIN";
    case OP_WGET:  return "WGET";
    default:       return "???";
  }
}

const OpCode OP_LIST[] = {
  OP_SET, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
  OP_AND, OP_OR,  OP_NOT,
  OP_IF_GT, OP_IF_LT, OP_IF_EQ, OP_IF_NE,
  OP_JUMP, OP_CALL, OP_END, OP_HALT,
  OP_GPOW, OP_GPOR, OP_GPOM, OP_PWM, OP_ADC,
  OP_WAIT, OP_TIME, OP_PRINT, OP_CLS,
  OP_SOUT, OP_SIN, OP_WGET
};
const uint8_t OP_COUNT = sizeof(OP_LIST) / sizeof(OP_LIST[0]);

// ╔═══════════════════════════════════════════════════════════╗
// ║  ENGINE                                                   ║
// ╚═══════════════════════════════════════════════════════════╝

CacheEntry gCache[CACHE_SIZE];
uint8_t    gCacheCount = 0;

CallFrame  gCallStack[64];
uint16_t   gStackTop  = 0;
Script     gCurrentScript;
uint8_t    gPC        = 0;
bool       gRunning   = false;
bool       gHalted    = false;

// ── Load script (cache หรือ SPIFFS) ──────────────────────
bool engineLoadScript(const char* name) {
  // 1. ค้นหา cache
  for (uint8_t i = 0; i < gCacheCount; i++) {
    if (gCache[i].valid && strcmp(gCache[i].name, name) == 0) {
      memcpy(&gCurrentScript, &gCache[i].data, sizeof(Script));
      return true;
    }
  }
  // 2. cache MISS → อ่าน SPIFFS
  char path[20];
  snprintf(path, sizeof(path), "%s%s", SCRIPT_DIR, name);
  File f = SPIFFS.open(path, "r");
  if (!f) return false;
  f.read((uint8_t*)&gCurrentScript, sizeof(Script));
  f.close();
  // 3. เก็บลง cache (circular)
  uint8_t slot = gCacheCount % CACHE_SIZE;
  if (gCacheCount < CACHE_SIZE) gCacheCount++;
  strncpy(gCache[slot].name, name, 4);
  gCache[slot].name[4] = '\0';
  memcpy(&gCache[slot].data, &gCurrentScript, sizeof(Script));
  gCache[slot].valid = true;
  return true;
}

// ── Save script ────────────────────────────────────────────
bool engineSaveScript(const char* name, Script* s) {
  char path[20];
  snprintf(path, sizeof(path), "%s%s", SCRIPT_DIR, name);
  File f = SPIFFS.open(path, "w");
  if (!f) return false;
  f.write((uint8_t*)s, sizeof(Script));
  f.close();
  // update cache
  for (uint8_t i = 0; i < gCacheCount; i++)
    if (strcmp(gCache[i].name, name) == 0)
      memcpy(&gCache[i].data, s, sizeof(Script));
  return true;
}

// ── Resolve value ─────────────────────────────────────────
float resolveVal(uint16_t v, uint8_t vtype) {
  if (vtype == VREF_CONST) return (float)(int16_t)v;
  if (vtype == VREF_GPIO)  return (float)digitalRead(v);
  if (vtype == VREF_VAR) {
    char name[5];
    decodeVarName((uint32_t)v, name);
    if (name[0] == 'G') {
      uint8_t pin = (name[1]-'0')*10 + (name[2]-'0');
      return (float)digitalRead(pin);
    }
    if (name[0] == 'A') return (float)analogRead(name[1]-'0');
    if (name[0] == 'S') {
      switch(name[3]) {
        case '1': return (float)ESP.getFreeHeap();
        case '2': return (float)millis();
        case '3': return (float)gStackTop;
      }
    }
    return gVars.getFloat(name);
  }
  return 0;
}

// ── Store result ──────────────────────────────────────────
void storeResult(uint16_t dest, uint8_t dtype, float val) {
  if (dtype != VREF_VAR) return;
  char name[5];
  decodeVarName((uint32_t)dest, name);
  if (name[0] == 'G') {
    uint8_t pin = (name[1]-'0')*10 + (name[2]-'0');
    digitalWrite(pin, val ? HIGH : LOW);
    return;
  }
  if (val == (float)(int16_t)val)
    gVars.setInt(name, (int16_t)val);
  else
    gVars.setFloat(name, val);
}

// ── Execute 1 row ─────────────────────────────────────────
void engineStep() {
  if (!gRunning || gHalted) return;

  // จบ script
  if (gPC >= gCurrentScript.hdr.rowCount) {
    if (gStackTop > 0) {
      gStackTop--;
      CallFrame& f = gCallStack[gStackTop];
      engineLoadScript(f.scriptName);
      gPC = f.PC;
    } else {
      gPC = 0; // วนกลับ row 0
    }
    return;
  }

  Row& r  = gCurrentScript.rows[gPC];
  uint8_t v1t = r.vtype & 0x0F;
  uint8_t v2t = (r.vtype >> 4) & 0x0F;
  float a = resolveVal(r.v1, v1t);
  float b = resolveVal(r.v2, v2t);
  float result = 0;

  switch ((OpCode)r.op) {
    // Math
    case OP_SET:  result = a; break;
    case OP_ADD:  result = a + b; break;
    case OP_SUB:  result = a - b; break;
    case OP_MUL:  result = a * b; break;
    case OP_DIV:  result = (b != 0) ? a / b : 0; break;
    case OP_MOD:  result = (b != 0) ? fmod(a,b) : 0; break;
    // Logic
    case OP_AND:  result = (a && b) ? 1 : 0; break;
    case OP_OR:   result = (a || b) ? 1 : 0; break;
    case OP_NOT:  result = (!a) ? 1 : 0; break;
    // Compare → skip
    case OP_IF_GT: gPC += (a >  b) ? 2 : 1; return;
    case OP_IF_LT: gPC += (a <  b) ? 2 : 1; return;
    case OP_IF_EQ: gPC += (a == b) ? 2 : 1; return;
    case OP_IF_NE: gPC += (a != b) ? 2 : 1; return;
    // Flow
    case OP_JUMP:
      gPC = (uint8_t)a; return;
    case OP_CALL: {
      if (ESP.getFreeHeap() < MEM_STOP_KB) {
        gHalted = true; return;
      }
      if (gStackTop < MAX_CALLSTACK) {
        gCallStack[gStackTop].PC = gPC + 1;
        strncpy(gCallStack[gStackTop].scriptName,
                gCallStack[gStackTop > 0 ? gStackTop-1 : 0].scriptName, 5);
        gStackTop++;
        char name[5];
        decodeVarName(r.v1, name);
        engineLoadScript(name);
        gPC = 0; return;
      }
      break;
    }
    case OP_END:
      if (gStackTop > 0) {
        gStackTop--;
        CallFrame& f = gCallStack[gStackTop];
        engineLoadScript(f.scriptName);
        gPC = f.PC; return;
      }
      gRunning = false; return;
    case OP_HALT:
      gRunning = false; gHalted = true; return;
    // GPIO
    case OP_GPOW:
      digitalWrite((uint8_t)a, b ? HIGH : LOW);
      gPC++; return;
    case OP_GPOR:
      result = digitalRead((uint8_t)b); break;
    case OP_GPOM:
      pinMode((uint8_t)a, b ? OUTPUT : INPUT);
      gPC++; return;
    case OP_PWM:
      analogWrite((uint8_t)a, (uint8_t)b);
      gPC++; return;
    case OP_ADC:
      result = analogRead((uint8_t)b); break;
    // Time
    case OP_WAIT:
      delay((uint32_t)a); gPC++; return;
    case OP_TIME:
      result = (float)millis(); break;
    // Serial
    case OP_SOUT:
      Serial.println(a); gPC++; return;
    default:
      gPC++; return;
  }

  storeResult(r.v1, v1t, result);
  gPC++;
}

// ── Start / Stop ──────────────────────────────────────────
bool engineStart(const char* name) {
  if (!engineLoadScript(name)) return false;
  gStackTop = 0;
  gPC       = 0;
  gRunning  = true;
  gHalted   = false;
  strncpy(gCallStack[0].scriptName, name, 4);
  gCallStack[0].scriptName[4] = '\0';
  return true;
}

void engineStop() {
  gRunning  = false;
  gStackTop = 0;
  gPC       = 0;
}

// ╔═══════════════════════════════════════════════════════════╗
// ║  BUTTONS                                                  ║
// ╚═══════════════════════════════════════════════════════════╝

#define BTN_DEBOUNCE_MS   50
#define BTN_REPEAT_MS    200

struct BtnState {
  uint8_t  current;
  uint8_t  last;
  uint32_t pressTime;
  uint32_t repeatTime;
  bool     pressed;
  bool     held;
};

BtnState gBtnL = {0};
BtnState gBtnR = {0};

uint8_t readLeftBtn() {
  int v = analogRead(BTN_LEFT_PIN);
  if (v > ADC_NONE) return BTN_NONE;
  if (v > ADC_BTN1) return BTN_L;
  if (v > ADC_BTN2) return BTN_UP;
  if (v > ADC_BTN3) return BTN_DOWN;
  if (v > ADC_BTN4) return BTN_LEFT;
  if (v > 100)      return BTN_RIGHT;
  return BTN_PAUSE;
}

uint8_t readRightBtn() {
  int v = analogRead(BTN_RIGHT_PIN);
  if (v > ADC_NONE) return BTN_NONE;
  if (v > ADC_BTN1) return BTN_R;
  if (v > ADC_BTN2) return BTN_SELECT;
  if (v > ADC_BTN3) return BTN_CANCEL;
  if (v > ADC_BTN4) return BTN_PLAY;
  return BTN_NONE;
}

void updateBtnState(BtnState& s, uint8_t raw) {
  uint32_t now = millis();
  s.pressed = false;
  if (raw != BTN_NONE) {
    if (s.current == BTN_NONE) {
      s.pressTime  = now;
      s.repeatTime = now;
      s.pressed    = true;
    } else if (now - s.repeatTime > BTN_REPEAT_MS) {
      s.pressed    = true;
      s.repeatTime = now;
    }
    s.held = (now - s.pressTime > BTN_DEBOUNCE_MS);
  }
  s.last    = s.current;
  s.current = raw;
}

void buttonsUpdate() {
  updateBtnState(gBtnL, readLeftBtn());
  updateBtnState(gBtnR, readRightBtn());
}

bool btnPressed(uint8_t btn) {
  if (btn <= BTN_PAUSE)
    return gBtnL.pressed && gBtnL.current == btn;
  return gBtnR.pressed && gBtnR.current == btn;
}

uint8_t anyPressed() {
  if (gBtnL.pressed) return gBtnL.current;
  if (gBtnR.pressed) return gBtnR.current;
  return BTN_NONE;
}

// ╔═══════════════════════════════════════════════════════════╗
// ║  DISPLAY                                                  ║
// ╚═══════════════════════════════════════════════════════════╝

U8G2_SSD1306_72X40_ER_F_HW_I2C
  u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// ── Ticker ────────────────────────────────────────────────
char     tickerBuf[128] = "";
int16_t  tickerX        = OLED_WIDTH;
uint32_t tickerLast     = 0;
#define  TICKER_SPEED_MS  40

// ── Screens ───────────────────────────────────────────────
enum Screen {
  SCR_BOOT,
  SCR_MAIN,
  SCR_SCRIPTS,
  SCR_RUNNING,
  SCR_PAUSE,
  SCR_LOWMEM,
};
Screen gScreen = SCR_BOOT;

// ── Menu state ────────────────────────────────────────────
int8_t  gMenuSel = 0;
int8_t  gMenuTop = 0;
#define VISIBLE_ROWS 4

// ── Nav path ──────────────────────────────────────────────
char gNavPath[32] = "MAIN";

// ── Line Y positions ─────────────────────────────────────
const uint8_t LINE_Y[6] = {6, 13, 20, 27, 34, 40};

// ── Font helpers ─────────────────────────────────────────
void setFontSmall()  { u8g2.setFont(u8g2_font_4x6_tr); }
void setFontMedium() { u8g2.setFont(u8g2_font_5x8_tr); }

// ── Ticker ────────────────────────────────────────────────
void updateTicker() {
  snprintf(tickerBuf, sizeof(tickerBuf),
    " CPU:%dMHz MEM:%dKB STK:%d HEAP:%d ",
    (int)ESP.getCpuFreqMHz(),
    (int)(ESP.getFreeHeap() / 1024),
    (int)gStackTop,
    (int)ESP.getFreeHeap()
  );
}

void drawTicker() {
  uint32_t now = millis();
  if (now - tickerLast > TICKER_SPEED_MS) {
    tickerX--;
    tickerLast = now;
    int textW = strlen(tickerBuf) * 4;
    if (tickerX < -textW) tickerX = OLED_WIDTH;
  }
  setFontSmall();
  u8g2.setDrawColor(1);
  u8g2.drawStr(tickerX, LINE_Y[0], tickerBuf);
  u8g2.drawHLine(0, 8, OLED_WIDTH);
}

void drawNavPath() {
  setFontSmall();
  u8g2.drawStr(0, LINE_Y[1], gNavPath);
  u8g2.drawHLine(0, 15, OLED_WIDTH);
}

// ── Menu list ─────────────────────────────────────────────
void drawMenuList(const char** items, uint8_t count,
                  int8_t sel, int8_t top) {
  setFontSmall();
  for (uint8_t i = 0; i < VISIBLE_ROWS && (top+i) < count; i++) {
    uint8_t idx = top + i;
    uint8_t y   = LINE_Y[2 + i];
    if (idx == sel) {
      u8g2.setDrawColor(1);
      u8g2.drawBox(0, y-6, OLED_WIDTH, 7);
      u8g2.setDrawColor(0);
      u8g2.drawStr(2, y, items[idx]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(2, y, items[idx]);
    }
  }
  if (count > VISIBLE_ROWS) {
    if (top > 0)
      u8g2.drawStr(OLED_WIDTH-4, LINE_Y[2], "^");
    if (top + VISIBLE_ROWS < count)
      u8g2.drawStr(OLED_WIDTH-4, LINE_Y[5], "v");
  }
}

// ── Screens ───────────────────────────────────────────────
void drawBoot() {
  u8g2.clearBuffer();
  setFontMedium();
  u8g2.drawStr(18, 16, OS_NAME);
  setFontSmall();
  u8g2.drawStr(16, 26, "v" OS_VERSION);
  u8g2.drawStr(8,  36, "ESP32-C3");
  u8g2.sendBuffer();
}

const char* MAIN_ITEMS[] = {
  "1.RUN SCRIPT",
  "2.EDIT SCRIPT",
  "3.SETTINGS",
  "4.MONITOR",
};
#define MAIN_COUNT 4

void drawMain() {
  u8g2.clearBuffer();
  drawTicker();
  strncpy(gNavPath, "MAIN", sizeof(gNavPath));
  drawNavPath();
  drawMenuList(MAIN_ITEMS, MAIN_COUNT, gMenuSel, gMenuTop);
  u8g2.sendBuffer();
}

// ── Script file list ─────────────────────────────────────
#define MAX_SCRIPT_FILES 32
char    scriptFiles[MAX_SCRIPT_FILES][5];
uint8_t scriptFileCount = 0;

void scanScripts() {
  scriptFileCount = 0;
  File dir = SPIFFS.open(SCRIPT_DIR);
  if (!dir) return;
  File f = dir.openNextFile();
  while (f && scriptFileCount < MAX_SCRIPT_FILES) {
    String name = String(f.name());
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.length() >= 1 && name.length() <= 4
        && name != ".keep") {
      strncpy(scriptFiles[scriptFileCount], name.c_str(), 4);
      scriptFiles[scriptFileCount][4] = '\0';
      scriptFileCount++;
    }
    f = dir.openNextFile();
  }
}

void drawScriptList() {
  const char* items[MAX_SCRIPT_FILES + 1];
  for (uint8_t i = 0; i < scriptFileCount; i++)
    items[i] = scriptFiles[i];
  items[scriptFileCount] = "[NEW]";
  u8g2.clearBuffer();
  drawTicker();
  drawNavPath();
  drawMenuList(items, scriptFileCount + 1, gMenuSel, gMenuTop);
  u8g2.sendBuffer();
}

void drawRunning() {
  u8g2.clearBuffer();
  drawTicker();
  char line[16];
  snprintf(line, sizeof(line), ">%s PC:%d",
    gCallStack[0].scriptName, gPC);
  setFontSmall();
  u8g2.drawStr(0, LINE_Y[1], line);
  u8g2.drawHLine(0, 15, OLED_WIDTH);
  if (gPC < gCurrentScript.hdr.rowCount) {
    Row& r = gCurrentScript.rows[gPC];
    snprintf(line, sizeof(line), "OP:%s", opName((OpCode)r.op));
    u8g2.drawStr(0, LINE_Y[2], line);
  }
  if (ESP.getFreeHeap() < MEM_WARN_KB) {
    snprintf(line, sizeof(line), "!MEM:%dK",
      (int)(ESP.getFreeHeap()/1024));
    u8g2.drawStr(0, LINE_Y[5], line);
  }
  u8g2.sendBuffer();
}

const char* PAUSE_ITEMS[] = {
  "RESUME", "SAVE", "STOP", "NEW SCRIPT"
};
#define PAUSE_COUNT 4

void drawPause() {
  u8g2.clearBuffer();
  u8g2.drawFrame(4, 2, OLED_WIDTH-8, OLED_HEIGHT-4);
  setFontSmall();
  u8g2.drawStr(14, 10, "[PAUSE]");
  u8g2.drawHLine(5, 12, OLED_WIDTH-10);
  for (uint8_t i = 0; i < PAUSE_COUNT; i++) {
    uint8_t y = 19 + i * 6;
    if (i == gMenuSel) {
      u8g2.drawBox(6, y-5, OLED_WIDTH-12, 6);
      u8g2.setDrawColor(0);
      u8g2.drawStr(8, y, PAUSE_ITEMS[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(8, y, PAUSE_ITEMS[i]);
    }
  }
  u8g2.sendBuffer();
}

void drawLowMem() {
  u8g2.clearBuffer();
  u8g2.drawFrame(2, 2, OLED_WIDTH-4, OLED_HEIGHT-4);
  setFontSmall();
  u8g2.drawStr(6,  12, "! LOW MEM !");
  char line[16];
  snprintf(line, sizeof(line), "FREE:%dKB",
    (int)(ESP.getFreeHeap()/1024));
  u8g2.drawStr(8, 22, line);
  u8g2.drawStr(4, 30, "SEL=CONT");
  u8g2.drawStr(4, 38, "CNCL=STOP");
  u8g2.sendBuffer();
}

void displayRender() {
  updateTicker();
  switch (gScreen) {
    case SCR_BOOT:    drawBoot();       break;
    case SCR_MAIN:    drawMain();       break;
    case SCR_SCRIPTS: drawScriptList(); break;
    case SCR_RUNNING: drawRunning();    break;
    case SCR_PAUSE:   drawPause();      break;
    case SCR_LOWMEM:  drawLowMem();     break;
  }
}

void menuUp(int8_t count) {
  if (gMenuSel > 0) gMenuSel--;
  if (gMenuSel < gMenuTop) gMenuTop = gMenuSel;
}

void menuDown(int8_t count) {
  if (gMenuSel < count-1) gMenuSel++;
  if (gMenuSel >= gMenuTop + VISIBLE_ROWS)
    gMenuTop = gMenuSel - VISIBLE_ROWS + 1;
}

void menuReset() { gMenuSel = 0; gMenuTop = 0; }

// ╔═══════════════════════════════════════════════════════════╗
// ║  MAIN OS                                                  ║
// ╚═══════════════════════════════════════════════════════════╝

char gSelScript[5] = "";

// ── Nav path ─────────────────────────────────────────────
uint8_t gNavStack[8];
uint8_t gNavDepth = 0;

void navPush(uint8_t sel) {
  if (gNavDepth < 8) gNavStack[gNavDepth++] = sel;
  gNavPath[0] = '\0';
  for (uint8_t i = 0; i < gNavDepth; i++) {
    char tmp[5];
    snprintf(tmp, sizeof(tmp), i > 0 ? ">%d" : "%d",
             gNavStack[i] + 1);
    strncat(gNavPath, tmp, sizeof(gNavPath)-strlen(gNavPath)-1);
  }
}

void navPop() { if (gNavDepth > 0) gNavDepth--; }

// ── Input handler ────────────────────────────────────────
void handleInput() {
  buttonsUpdate();
  uint8_t btn = anyPressed();
  if (btn == BTN_NONE) return;

  switch (gScreen) {

    case SCR_MAIN:
      if (btn == BTN_UP)   menuUp(MAIN_COUNT);
      if (btn == BTN_DOWN) menuDown(MAIN_COUNT);
      if (btn == BTN_SELECT || btn == BTN_PLAY) {
        navPush(gMenuSel);
        switch (gMenuSel) {
          case 0:
          case 1:
            scanScripts();
            menuReset();
            snprintf(gNavPath, sizeof(gNavPath),
              gMenuSel == 0 ? "RUN" : "EDIT");
            gScreen = SCR_SCRIPTS;
            break;
        }
      }
      break;

    case SCR_SCRIPTS: {
      uint8_t total = scriptFileCount + 1;
      if (btn == BTN_UP)     menuUp(total);
      if (btn == BTN_DOWN)   menuDown(total);
      if (btn == BTN_CANCEL) {
        navPop(); menuReset();
        gScreen = SCR_MAIN;
      }
      if (btn == BTN_SELECT || btn == BTN_PLAY) {
        if (gMenuSel < scriptFileCount) {
          navPush(gMenuSel);
          strncpy(gSelScript, scriptFiles[gMenuSel], 4);
          gSelScript[4] = '\0';
          if (engineStart(gSelScript)) {
            menuReset();
            gScreen = SCR_RUNNING;
          }
        }
        // TODO: NEW script → editor
      }
      break;
    }

    case SCR_RUNNING:
      if (btn == BTN_PAUSE) { menuReset(); gScreen = SCR_PAUSE; }
      break;

    case SCR_PAUSE:
      if (btn == BTN_UP)   menuUp(PAUSE_COUNT);
      if (btn == BTN_DOWN) menuDown(PAUSE_COUNT);
      if (btn == BTN_CANCEL) gScreen = SCR_RUNNING;
      if (btn == BTN_SELECT || btn == BTN_PLAY) {
        switch (gMenuSel) {
          case 0: gScreen = SCR_RUNNING; break;
          case 1: // SAVE (TODO)
            gScreen = SCR_RUNNING; break;
          case 2: // STOP
            engineStop(); gVars.clear();
            navPop(); navPop();
            menuReset(); gScreen = SCR_MAIN; break;
          case 3: // NEW SCRIPT
            engineStop(); gVars.clear();
            menuReset(); gScreen = SCR_SCRIPTS; break;
        }
      }
      break;

    case SCR_LOWMEM:
      if (btn == BTN_SELECT) gScreen = SCR_RUNNING;
      if (btn == BTN_CANCEL) {
        engineStop(); gVars.clear();
        menuReset(); gScreen = SCR_MAIN;
      }
      break;

    default: break;
  }
}

// ── SETUP ────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n" OS_NAME " v" OS_VERSION " booting...");

  // SPIFFS
  if (!SPIFFS.begin(true))
    Serial.println("SPIFFS ERROR");
  if (!SPIFFS.exists(SCRIPT_DIR)) {
    File f = SPIFFS.open("/s/.keep", "w"); f.close();
  }

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.setContrast(200);

  // ADC
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // Boot screen
  gScreen = SCR_BOOT;
  drawBoot();
  delay(1500);

  // Var pool
  gVars.clear();

  // Demo script
  if (!SPIFFS.exists("/s/demo")) {
    Script demo;
    memset(&demo, 0, sizeof(demo));
    demo.hdr.rowCount = 4;
    demo.hdr.version  = 1;
    // SET V001 = 10
    demo.rows[0] = {OP_SET, (VREF_VAR)|(VREF_CONST<<4),
      (uint16_t)encodeVarName("V001"), 10};
    // SET V002 = 5
    demo.rows[1] = {OP_SET, (VREF_VAR)|(VREF_CONST<<4),
      (uint16_t)encodeVarName("V002"), 5};
    // ADD V001 + V002 (result ใน V001)
    demo.rows[2] = {OP_ADD, (VREF_VAR)|(VREF_VAR<<4),
      (uint16_t)encodeVarName("V001"),
      (uint16_t)encodeVarName("V002")};
    // HALT
    demo.rows[3] = {OP_HALT, 0, 0, 0};
    engineSaveScript("demo", &demo);
    Serial.println("Demo script created");
  }

  menuReset();
  gScreen = SCR_MAIN;
  Serial.printf("Boot OK - Free heap: %d bytes\n",
    ESP.getFreeHeap());
}

// ── LOOP ─────────────────────────────────────────────────
void loop() {
  // 1. Input
  handleInput();

  // 2. Engine step
  if (gScreen == SCR_RUNNING && gRunning) {
    if (ESP.getFreeHeap() < MEM_WARN_KB) {
      gScreen = SCR_LOWMEM;
    } else {
      engineStep();
    }
    if (!gRunning) {
      gVars.clear();
      menuReset();
      gScreen = SCR_MAIN;
    }
  }

  // 3. Render
  displayRender();

  // 4. yield
  yield();
}
