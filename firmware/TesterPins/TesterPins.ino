/*
 * PinTester — находим рабочие пины на китайском NRF52840 Pro Micro
 *
 * Как пользоваться:
 *   1. Залей этот скетч
 *   2. Открой Serial Monitor (115200 бод)
 *   3. Замыкай пины на GND по одному
 *   4. Смотри что пишет в консоль
 *   5. Запиши какие пины реально откликаются
 *
 * Тестируем ВСЕ пины которые физически есть на плате.
 * Нам нужно найти 9 рабочих (8 свитчей + 1 кнопка питания).
 */

// Все пины которые обычно выведены на китайском Pro Micro NRF52840.
// Числа — это номера Arduino (не GPIO!), как они обычно маркируются в BSP.
// Если плата определилась как "Adafruit Feather nRF52840" — пины будут одни,
// если как "Generic nRF52840" — могут быть другие. Проверяем всё подряд.

const int TEST_PINS[] = {
  2, 3, 4, 5, 6, 7, 8, 9, 10,   // правый ряд (D2..D10)
  16, 14, 15, 18, 19, 20, 21,    // A0..A5 / дополнительные
  0, 1,                           // TX/RX — обычно Serial, но вдруг нужны
  11, 12, 13,                     // SPI / дополнительные
  22, 23, 24, 25,                 // если есть
  // GPIO напрямую — попробуем и так:
  // NRF52840 имеет P0.xx и P1.xx
  // В некоторых BSP они идут как 32+ для P1
  32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47
};

const int PIN_COUNT = sizeof(TEST_PINS) / sizeof(TEST_PINS[0]);

bool lastState[60];   // с запасом
bool initialized[60]; // какие пины успешно инициализировались
unsigned long lastTrigger[60];
const unsigned long DEBOUNCE_MS = 100;

int totalWorking = 0;
int workingPins[60];

void setup() {
  Serial.begin(115200);
  delay(2000); // ждём открытия Serial Monitor

  Serial.println();
  Serial.println("╔══════════════════════════════════════╗");
  Serial.println("║       RutonyPad — Pin Tester         ║");
  Serial.println("╠══════════════════════════════════════╣");
  Serial.println("║  Замыкай пины на GND по одному.      ║");
  Serial.println("║  Рабочие пины покажутся здесь.       ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.println();

  // Пробуем инициализировать каждый пин
  Serial.print("Инициализация пинов: ");
  for (int i = 0; i < PIN_COUNT; i++) {
    int pin = TEST_PINS[i];
    initialized[i] = false;
    lastTrigger[i] = 0;

    // Пробуем настроить пин — на некоторых китайских платах
    // обращение к несуществующему пину вызывает сбой.
    // pinMode на NRF52 не падает, просто игнорирует — но на всякий случай.
    pinMode(pin, INPUT_PULLUP);
    delay(2); // небольшая пауза чтобы подтяжка установилась

    // Читаем — если HIGH, значит пин живой (подтяжка работает)
    // Если читает LOW сразу — пин либо не существует, либо всегда замкнут
    int reading = digitalRead(pin);

    if (reading == HIGH) {
      initialized[i] = true;
      lastState[i] = HIGH;
      Serial.print("D");
      Serial.print(pin);
      Serial.print(" ");
      totalWorking++;
      workingPins[totalWorking - 1] = pin;
    }
    // Если LOW при старте — пропускаем (скорее всего не существует или GND)
  }

  Serial.println();
  Serial.println();
  Serial.print("Найдено рабочих пинов: ");
  Serial.println(totalWorking);
  Serial.print("Список: ");
  for (int i = 0; i < totalWorking; i++) {
    Serial.print("D"); Serial.print(workingPins[i]);
    if (i < totalWorking - 1) Serial.print(", ");
  }
  Serial.println();
  Serial.println();
  Serial.println("Нужно найти 9 штук (8 свитчей + кнопка питания).");
  Serial.println("Замыкай пины на GND и смотри сюда ↓");
  Serial.println("────────────────────────────────────────");
}

void loop() {
  unsigned long now = millis();

  for (int i = 0; i < PIN_COUNT; i++) {
    if (!initialized[i]) continue;

    int pin = TEST_PINS[i];
    bool cur = digitalRead(pin) == LOW; // LOW = замкнут на GND

    if (cur && !lastState[i]) {
      // Нажатие (HIGH → LOW, т.е. lastState был HIGH = не нажат)
      if (now - lastTrigger[i] > DEBOUNCE_MS) {
        lastTrigger[i] = now;

        Serial.print("▶ ЗАМКНУТ  D");
        Serial.print(pin);
        Serial.print("  (Arduino pin ");
        Serial.print(pin);
        Serial.print(", millis=");
        Serial.print(now);
        Serial.println(")");
      }
    }

    if (!cur && lastState[i]) {
      // Отпускание (LOW → HIGH)
      if (now - lastTrigger[i] > DEBOUNCE_MS) {
        Serial.print("  разомкнут D");
        Serial.println(pin);
      }
    }

    lastState[i] = cur;
  }
}
