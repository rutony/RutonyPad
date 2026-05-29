/*
 * RutonyPad — combo config firmware, reboot-separated modes
 *
 * Почему так:
 *   На Windows нельзя надежно превратить уже запущенную BLE HID-клавиатуру
 *   в чистое config-устройство без перезапуска BLE/GATT. Поэтому вход в настройку
 *   теперь сохраняет флаг режима и делает программный reset. Питание/аккумулятор
 *   отключать не нужно.
 *
 * Порядок клавиш:
 *   1 D33, 2 D2, 3 D20, 4 D21, 5 D23, 6 D1, 7 D12, 8 D11
 *
 * Служебная комбинация:
 *   NORMAL: D33 + D11 удерживать 5 сек -> перезагрузка в CONFIG ONLY, HID не создается
 *   CONFIG: D33 + D11 удерживать 2 сек -> перезагрузка в NORMAL, HID создается
 *   На веб-странице кнопка "Вернуться в режим клавиатуры" делает то же самое.
 */

#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;

// ─── ПИНЫ ───────────────────────────────────────────────────────────────────
const int SWITCH_PINS[8] = {33, 2, 20, 21, 23, 1, 12, 11};
const int BATT_PIN = 18;  // D18 — делитель батареи

const int COMBO_LEFT_INDEX  = 0; // D33
const int COMBO_RIGHT_INDEX = 7; // D11
const unsigned long ENTER_CONFIG_MS = 5000UL;
const unsigned long EXIT_CONFIG_MS  = 2000UL;

// ─── ФАЙЛЫ ──────────────────────────────────────────────────────────────────
File fsFile(InternalFS);
#define CONFIG_FILE "/rutonypad.cfg"
#define BOOTMODE_FILE "/rutonypad.mode"
#define MODE_NORMAL 0x00
#define MODE_CONFIG 0xC3

// ─── КОНФИГ КЛАВИШ ──────────────────────────────────────────────────────────
struct KeyConfig {
  uint8_t modifier;
  uint8_t keycode;
};

const KeyConfig DEFAULT_CONFIGS[8] = {
  {0x07, 0x1E}, {0x07, 0x1F}, {0x07, 0x20}, {0x07, 0x21},
  {0x07, 0x22}, {0x07, 0x23}, {0x07, 0x24}, {0x07, 0x25},
};

KeyConfig configs[8];

// ─── BLE ────────────────────────────────────────────────────────────────────
BLEDis         bledis;
BLEHidAdafruit blehid;
BLEBas         blebas;

// UUID RutonyPad config service.
// В Bluefruit 128-bit UUID задаётся байтами в little-endian порядке.
const uint8_t UUID_SERVICE[] = {0xA1,0xC9,0x43,0x4F,0x7A,0x2A,0xB0,0xBB,0xE9,0x4C,0x56,0x0E,0x20,0x9A,0x1D,0x6F}; // 6f1d9a20-0e56-4ce9-bbb0-2a7a4f43c9a1
const uint8_t UUID_READ[]    = {0x73,0x8E,0xF0,0x8C,0x79,0x6D,0xE1,0xA5,0x9B,0x4F,0xD6,0x13,0xD8,0xD4,0xF3,0xB3}; // b3f3d4d8-13d6-4f9b-a5e1-6d798cf08e73
const uint8_t UUID_WRITE[]   = {0xBF,0xA3,0xC8,0x03,0xF7,0xE0,0x6C,0x9E,0x5C,0x4D,0xD0,0x71,0xD5,0xF6,0xDC,0x4A}; // 4adcf6d5-71d0-4d5c-9e6c-e0f703c8a3bf
const uint8_t UUID_CONTROL[] = {0x0E,0x3C,0x79,0xF2,0x49,0x4B,0x13,0xB6,0xE6,0x4C,0xA2,0x8E,0x7B,0x22,0xF1,0x5A}; // 7b22f15a-a28e-4ce6-b613-4b49f2793c0e

BLEService        cfgService(UUID_SERVICE);
BLECharacteristic cfgRead(UUID_READ);
BLECharacteristic cfgWrite(UUID_WRITE);
BLECharacteristic cfgControl(UUID_CONTROL);

// ─── СОСТОЯНИЕ ──────────────────────────────────────────────────────────────
bool configMode = false;
bool hidStarted = false;
uint16_t currentConn = BLE_CONN_HANDLE_INVALID;

bool lastState[8];
bool keyPressed[8];
unsigned long lastDebounce[8];
const unsigned long DEBOUNCE_MS = 50;

bool comboWasDown = false;
bool comboConsumed = false;
unsigned long comboStart = 0;

// ─── БАТАРЕЯ ────────────────────────────────────────────────────────────────
const float BATT_FULL_MV  = 4200.0f;
const float BATT_EMPTY_MV = 3000.0f;
const float DIVIDER_RATIO = 2.0f;
const float ADC_REF_MV    = 3000.0f;  // AR_INTERNAL_3_0
const int   ADC_BITS      = 12;

unsigned long lastBattCheck = 0;
const unsigned long BATT_INTERVAL_MS = 60000UL;
uint8_t lastBattPercent = 255;

void startNormalAdv();
void startConfigAdv();
void updateCfgReadChar();
void rebootToMode(uint8_t modeByte);

// ─── BOOT MODE ──────────────────────────────────────────────────────────────
uint8_t readBootMode() {
  uint8_t mode = MODE_NORMAL;
  if (fsFile.open(BOOTMODE_FILE, FILE_O_READ)) {
    if (fsFile.read(&mode, 1) != 1) mode = MODE_NORMAL;
    fsFile.close();
  }
  return mode;
}

void saveBootMode(uint8_t modeByte) {
  InternalFS.remove(BOOTMODE_FILE);
  if (fsFile.open(BOOTMODE_FILE, FILE_O_WRITE)) {
    fsFile.write(&modeByte, 1);
    fsFile.close();
  }
}

void rebootToMode(uint8_t modeByte) {
  saveBootMode(modeByte);
  if (Bluefruit.connected() && currentConn != BLE_CONN_HANDLE_INVALID) {
    if (hidStarted) blehid.keyRelease();
    delay(30);
    Bluefruit.disconnect(currentConn);
    delay(250);
  }
  Serial.println(modeByte == MODE_CONFIG ? "Rebooting to CONFIG ONLY..." : "Rebooting to NORMAL keyboard...");
  Serial.flush();
  delay(100);
  NVIC_SystemReset();
}

// ─── BATTERY ────────────────────────────────────────────────────────────────
uint8_t readBatteryPercent() {
  int raw = analogRead(BATT_PIN);
  float pinMV = (float)raw / ((1 << ADC_BITS) - 1) * ADC_REF_MV;
  float battMV = pinMV * DIVIDER_RATIO;

  Serial.print("BAT raw="); Serial.print(raw);
  Serial.print(" pin="); Serial.print(pinMV, 1); Serial.print(" mV");
  Serial.print(" batt="); Serial.print(battMV, 1); Serial.println(" mV");

  if (battMV >= BATT_FULL_MV) return 100;
  if (battMV <= BATT_EMPTY_MV) return 0;
  return (uint8_t)((battMV - BATT_EMPTY_MV) / (BATT_FULL_MV - BATT_EMPTY_MV) * 100.0f);
}

void updateBattery(bool force = false) {
  uint8_t pct = readBatteryPercent();
  if (force || pct != lastBattPercent) {
    lastBattPercent = pct;
    blebas.write(pct);
    Serial.print("Battery level updated: "); Serial.print(pct); Serial.println("%");
  }
}

// ─── CONFIG FILE ────────────────────────────────────────────────────────────
void resetDefaultsInRam() {
  memcpy(configs, DEFAULT_CONFIGS, sizeof(configs));
}

void saveConfig() {
  InternalFS.remove(CONFIG_FILE);
  if (fsFile.open(CONFIG_FILE, FILE_O_WRITE)) {
    fsFile.write((uint8_t*)configs, sizeof(configs));
    fsFile.close();
    Serial.println("Config saved to flash.");
  } else {
    Serial.println("ERROR: Cannot open config file for write!");
  }
}

void loadConfig() {
  resetDefaultsInRam();
  if (fsFile.open(CONFIG_FILE, FILE_O_READ)) {
    size_t n = fsFile.read((uint8_t*)configs, sizeof(configs));
    fsFile.close();
    if (n == sizeof(configs)) {
      Serial.println("Config loaded from flash.");
    } else {
      Serial.println("Config file size mismatch, using defaults.");
      resetDefaultsInRam();
    }
  } else {
    Serial.println("No config file, using defaults.");
  }
}

// ─── CONFIG SERVICE ─────────────────────────────────────────────────────────
void updateCfgReadChar() {
  uint8_t buf[16];
  for (int i = 0; i < 8; i++) {
    buf[i * 2]     = configs[i].modifier;
    buf[i * 2 + 1] = configs[i].keycode;
  }
  cfgRead.write(buf, 16);
}

void onCfgWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  (void)conn_hdl;
  (void)chr;

  if (len != 16) {
    Serial.print("Wrong config length: "); Serial.println(len);
    return;
  }

  for (int i = 0; i < 8; i++) {
    configs[i].modifier = data[i * 2];
    configs[i].keycode  = data[i * 2 + 1];
  }

  saveConfig();
  updateCfgReadChar();
  Serial.println("Config received via BLE and saved.");
}

void onControlWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  (void)conn_hdl;
  (void)chr;
  if (len < 1) return;

  if (data[0] == 0xA1) {
    Serial.println("CONTROL: return to keyboard mode");
    rebootToMode(MODE_NORMAL);
  } else if (data[0] == 0xA2) {
    Serial.println("CONTROL: reset defaults");
    resetDefaultsInRam();
    saveConfig();
    updateCfgReadChar();
  } else {
    Serial.print("CONTROL: unknown command 0x"); Serial.println(data[0], HEX);
  }
}

void setupConfigService() {
  cfgService.begin();

  cfgRead.setProperties(CHR_PROPS_READ);
  cfgRead.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  cfgRead.setFixedLen(16);
  cfgRead.begin();
  updateCfgReadChar();

  cfgWrite.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  cfgWrite.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  cfgWrite.setFixedLen(16);
  cfgWrite.setWriteCallback(onCfgWrite);
  cfgWrite.begin();

  cfgControl.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  cfgControl.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  cfgControl.setFixedLen(1);
  cfgControl.setWriteCallback(onControlWrite);
  cfgControl.begin();
}

// ─── BLE CALLBACKS ──────────────────────────────────────────────────────────
void onConnect(uint16_t conn_hdl) {
  currentConn = conn_hdl;
  Serial.print("BLE connected, handle: "); Serial.println(conn_hdl);
  updateCfgReadChar();
  updateBattery(true);
}

void onDisconnect(uint16_t conn_hdl, uint8_t reason) {
  (void)conn_hdl;
  currentConn = BLE_CONN_HANDLE_INVALID;
  Serial.print("BLE disconnected, reason: 0x"); Serial.println(reason, HEX);

  if (configMode) startConfigAdv();
  else startNormalAdv();
}

// ─── ADVERTISING ────────────────────────────────────────────────────────────
void commonAdvSetup() {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
}

void startNormalAdv() {
  commonAdvSetup();
  Bluefruit.Advertising.addAppearance(BLE_APPEARANCE_HID_KEYBOARD);
  Bluefruit.Advertising.addService(blehid);
  Bluefruit.ScanResponse.addService(cfgService);
  Bluefruit.ScanResponse.addService(blebas);
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.start(0);
  Serial.println("Advertising started: NORMAL as RutonyPad: HID + Config + Battery.");
}

void startConfigAdv() {
  commonAdvSetup();
  Bluefruit.Advertising.addService(cfgService);
  Bluefruit.ScanResponse.addService(blebas);
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.start(0);
  Serial.println("Advertising started: CONFIG ONLY as RutonyCfg. HID service was NOT created on this boot.");
}

// ─── HID HELPERS ────────────────────────────────────────────────────────────
void sendKeyDown(int i) {
  if (!hidStarted || !Bluefruit.connected() || configMode) return;
  uint8_t keycodes[6] = {configs[i].keycode, 0, 0, 0, 0, 0};
  blehid.keyboardReport(configs[i].modifier, keycodes);
}

void sendKeyTap(int i) {
  if (!hidStarted || !Bluefruit.connected() || configMode) return;
  uint8_t keycodes[6] = {configs[i].keycode, 0, 0, 0, 0, 0};
  blehid.keyboardReport(configs[i].modifier, keycodes);
  delay(20);
  blehid.keyRelease();
}

void releaseHidIfNeeded() {
  if (hidStarted && Bluefruit.connected() && !configMode) blehid.keyRelease();
}

// ─── SETUP ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== RutonyPad RutonyCfg FIXED UUID boot ===");
  Serial.println("D2 is an ordinary key. No physical power button.");

  InternalFS.begin();
  configMode = (readBootMode() == MODE_CONFIG);
  loadConfig();

  analogReadResolution(ADC_BITS);
  analogReference(AR_INTERNAL_3_0);
  pinMode(BATT_PIN, INPUT);

  for (int i = 0; i < 8; i++) {
    pinMode(SWITCH_PINS[i], INPUT_PULLUP);
    lastState[i] = HIGH;
    keyPressed[i] = false;
    lastDebounce[i] = 0;
  }

  Bluefruit.begin(1, 0);
  Bluefruit.setTxPower(4);
  Bluefruit.setName(configMode ? "RutonyCfg" : "RutonyPad");
  Serial.print("BLE name: "); Serial.println(configMode ? "RutonyCfg" : "RutonyPad");
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  bledis.setManufacturer("Rutony");
  bledis.setModel(configMode ? "RutonyPad config only" : "RutonyPad keyboard");
  bledis.begin();

  blebas.begin();
  blebas.write(100);

  if (!configMode) {
    blehid.begin();
    hidStarted = true;
    setupConfigService();
    startNormalAdv();
    Serial.println("Boot mode: NORMAL, HID enabled.");
    Serial.println("Hold D33 + D11 for 5 sec to enter Web Config mode.");
  } else {
    hidStarted = false;
    setupConfigService();
    startConfigAdv();
    Serial.println("Boot mode: CONFIG ONLY, HID disabled.");
    Serial.println("Hold D33 + D11 for 2 sec or use web button to return to keyboard mode.");
  }

  lastBattCheck = millis() - BATT_INTERVAL_MS;
  Serial.println("Key order: D33, D2, D20, D21, D23, D1, D12, D11");
  Serial.println("=== RutonyPad ready ===");
}

// ─── LOOP ───────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (now - lastBattCheck >= BATT_INTERVAL_MS) {
    lastBattCheck = now;
    updateBattery();
  }

  bool cur[8];
  for (int i = 0; i < 8; i++) cur[i] = digitalRead(SWITCH_PINS[i]);

  bool comboDown = (cur[COMBO_LEFT_INDEX] == LOW && cur[COMBO_RIGHT_INDEX] == LOW);

  if (comboDown && !comboWasDown) {
    comboStart = now;
    comboConsumed = false;
    comboWasDown = true;
    Serial.println("SERVICE COMBO started: D33 + D11");
  }

  if (comboDown && !comboConsumed) {
    unsigned long held = now - comboStart;
    if (!configMode && held >= ENTER_CONFIG_MS) {
      Serial.println("MODE request: CONFIG. Saving mode and rebooting with HID disabled...");
      comboConsumed = true;
      rebootToMode(MODE_CONFIG);
    } else if (configMode && held >= EXIT_CONFIG_MS) {
      Serial.println("MODE request: NORMAL. Saving mode and rebooting with HID enabled...");
      comboConsumed = true;
      rebootToMode(MODE_NORMAL);
    }
  }

  if (!comboDown && comboWasDown) {
    comboWasDown = false;
    comboStart = 0;
    if (cur[COMBO_LEFT_INDEX] == HIGH && cur[COMBO_RIGHT_INDEX] == HIGH) comboConsumed = false;
  }

  for (int i = 0; i < 8; i++) {
    bool isComboKey = (i == COMBO_LEFT_INDEX || i == COMBO_RIGHT_INDEX);

    if (lastState[i] == HIGH && cur[i] == LOW) {
      if (now - lastDebounce[i] > DEBOUNCE_MS) {
        lastDebounce[i] = now;
        keyPressed[i] = true;

        Serial.print("KEY DOWN: slot "); Serial.print(i + 1);
        Serial.print(" D"); Serial.println(SWITCH_PINS[i]);

        // D33/D11 отправляются tap'ом на отпускании, чтобы можно было отличить служебную комбинацию.
        if (!isComboKey && !configMode) sendKeyDown(i);
      }
    }

    if (lastState[i] == LOW && cur[i] == HIGH) {
      if (keyPressed[i]) {
        keyPressed[i] = false;

        Serial.print("KEY UP: slot "); Serial.println(i + 1);

        if (!configMode) {
          if (isComboKey) {
            if (!comboConsumed) sendKeyTap(i);
          } else {
            releaseHidIfNeeded();
          }
        }
      }
    }

    lastState[i] = cur[i];
  }

  delay(2);
}
