#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

const uint8_t PIN_TEMP   = A0;
const uint8_t PIN_LDR    = A1;
const uint8_t PIN_BTN1   = 2;
const uint8_t PIN_BTN2   = 3;
const uint8_t PIN_BUZZ   = 4;
const uint8_t PIN_NIGHT  = 5;
const uint8_t PIN_STATUS = 13;

LiquidCrystal_I2C lcd(0x27, 16, 2);

enum State { BOOT, SHOW_TEMP, SHOW_LIGHT, SET_THRESHOLD, CALIBRATE };
State state = BOOT;

// interump flags
volatile bool sampleFlag = false;
volatile bool btn1Flag   = false;
volatile bool btn2Flag   = false;
volatile uint32_t lastBtn1 = 0, lastBtn2 = 0;


///eeprom
struct Settings {
    uint8_t  magic;
    uint8_t  version;
    int16_t  tempOffsetX10;
    uint16_t darkThreshold;
    uint8_t  checksum;
};

const uint8_t MAGIC   = 0xA5;
const uint8_t VERSION = 1;

Settings cfg;
bool eepromWasValid = false;
bool dirty = false;

//matavimai
uint16_t tBuf[8], lBuf[8];
uint8_t  idx = 0;
uint16_t tempAvg = 0, lightAvg = 0;
int16_t  tempX10 = 0;
uint8_t  lightPct = 0;

//led
const uint16_t HYST = 30;
uint8_t targetPwm = 0;
uint8_t curPwm = 0;

//laik
uint32_t bootStart = 0, lastFade = 0, lastBlink = 0, lastLcd = 0, lastSerial = 0;
uint32_t lcdTimeUs = 0, sensTimeUs = 0;





ISR(TIMER1_COMPA_vect) {
    sampleFlag = true;
}

void isrBtn1() {
    uint32_t now = millis();
    if (now - lastBtn1 > 50) {
        btn1Flag = true;
        lastBtn1 = now;
    }
}

void isrBtn2() {
    uint32_t now = millis();
    if (now - lastBtn2 > 50) {
        btn2Flag = true;
        lastBtn2 = now;
    }
}

///eprom funk

uint8_t calcChecksum(const Settings &s) {
    const uint8_t *p = (const uint8_t *)&s;
    uint8_t c = 0;
    for (uint8_t i = 0; i < sizeof(Settings) - 1; i++) c ^= p[i];
    return c;
}

void setDefaults() {
    cfg.magic = MAGIC;
    cfg.version = VERSION;
    cfg.tempOffsetX10 = 0;
    cfg.darkThreshold = 400;
}

void saveSettings() {
    cfg.checksum = calcChecksum(cfg);
    EEPROM.put(0, cfg);
}


bool loadSettings() {
    EEPROM.get(0, cfg);
    bool ok = cfg.magic == MAGIC &&
              cfg.version == VERSION &&
              cfg.checksum == calcChecksum(cfg) &&
              cfg.darkThreshold <= 1023;
    if (!ok) {
        setDefaults();
        saveSettings();
    }
    return ok;
}

//garsas
void beepOk()   { tone(PIN_BUZZ, 2000, 100); }
void beepSame() { tone(PIN_BUZZ, 1000, 40);  }

//matavimai
uint16_t readAdc(uint8_t pin) {
    analogRead(pin);
    return analogRead(pin);
}

void calcValues() {
    uint32_t ts = 0, ls = 0;
    for (uint8_t i = 0; i < 8; i++) { ts += tBuf[i]; ls += lBuf[i]; }
    tempAvg  = ts >> 3; // /8
    lightAvg = ls >> 3;

    // LM35: 10 mV/C, todel 1 mV = 0.1 C
    int32_t mV = (int32_t)tempAvg * 5000L / 1023;
    tempX10  = (int16_t)mV + cfg.tempOffsetX10;
    lightPct = map(lightAvg, 0, 1023, 0, 100);
}

void readSensors() {
    uint32_t t0 = micros();
    tBuf[idx] = readAdc(PIN_TEMP);
    lBuf[idx] = readAdc(PIN_LDR);
    idx = (idx + 1) & 7;   /// (idx + 1) % 8
    calcValues();
    sensTimeUs = micros() - t0;
}

// nak led
void updateNightTarget() {
    if (lightAvg + HYST < cfg.darkThreshold)      targetPwm = 255;  // tamsu
    else if (lightAvg > cfg.darkThreshold + HYST) targetPwm = 0;    // sviesu
}

void updateFade() {
    if (millis() - lastFade < 10) return;
    lastFade = millis();
    if (curPwm < targetPwm)      curPwm = min(255, curPwm + 5);
    else if (curPwm > targetPwm) curPwm = max(0, curPwm - 5);
    analogWrite(PIN_NIGHT, curPwm);
}

//status led
void updateStatusLed() {
    if (millis() - lastBlink < 500) return;
    lastBlink = millis();
    digitalWrite(PIN_STATUS, !digitalRead(PIN_STATUS));
}

//mygtukai
void saveIfChanged() {
    if (dirty) {
        saveSettings();
        dirty = false;
        beepOk();
    } else {
        beepSame();
    }
}


void handleBtn1() {
    switch (state) {
        case SHOW_TEMP:     state = SHOW_LIGHT; break;
        case SHOW_LIGHT:    state = SHOW_TEMP;  break;
        case SET_THRESHOLD: saveIfChanged(); state = CALIBRATE; break;
        case CALIBRATE:     saveIfChanged(); state = SHOW_TEMP; break;
        default: break;
    }
}

void handleBtn2() {
    switch (state) {
        case SHOW_TEMP:
        case SHOW_LIGHT:
            state = SET_THRESHOLD;
            break;
        case SET_THRESHOLD:
            cfg.darkThreshold += 25;
            if (cfg.darkThreshold > 1000) cfg.darkThreshold = 50;
            dirty = true;
            break;
        case CALIBRATE:
            cfg.tempOffsetX10 += 5;
            if (cfg.tempOffsetX10 > 50) cfg.tempOffsetX10 = -50;
            calcValues();
            dirty = true;
            break;
        default: break;
    }
}


void updateBoot() {
    if (state == BOOT && millis() - bootStart >= 1500) state = SHOW_TEMP;
}

//lcd
void printLine(uint8_t row, const char *txt) {
    lcd.setCursor(0, row);
    uint8_t n = 0;
    while (txt[n] && n < 16) lcd.print(txt[n++]);
    while (n++ < 16) lcd.print(' ');
}


void fmtX10(char *out, uint8_t size, int16_t v) {
    const char *sign = (v < 0) ? "-" : "";
    if (v < 0) v = -v;
    snprintf(out, size, "%s%d.%d", sign, v / 10, v % 10);
}

void updateLcd() {
    if (millis() - lastLcd < 300) return;
    lastLcd = millis();
    uint32_t t0 = micros();

    char l1[17], l2[17], num[8];

    switch (state) {
        case BOOT:
            snprintf(l1, sizeof l1, "Stotele HW2");
            snprintf(l2, sizeof l2, eepromWasValid ? "EEPROM OK" : "EEPROM default");
            break;
        case SHOW_TEMP:
            fmtX10(num, sizeof num, tempX10);
            snprintf(l1, sizeof l1, "Temp: %s C", num);
            snprintf(l2, sizeof l2, "Sviesa: %u%%", lightPct);
            break;
        case SHOW_LIGHT:
            snprintf(l1, sizeof l1, "Sviesa: %u%%", lightPct);
            snprintf(l2, sizeof l2, "ADC:%u LED:%s", lightAvg, targetPwm ? "ON" : "OFF");
            break;
        case SET_THRESHOLD:
            snprintf(l1, sizeof l1, "Slenkstis:%u%s", cfg.darkThreshold, dirty ? "*" : "");
            snprintf(l2, sizeof l2, "Dabar ADC: %u", lightAvg);
            break;
        case CALIBRATE:
            fmtX10(num, sizeof num, cfg.tempOffsetX10);
            snprintf(l1, sizeof l1, "Kalibr:%s C%s", num, dirty ? "*" : "");
            fmtX10(num, sizeof num, tempX10);
            snprintf(l2, sizeof l2, "Temp: %s C", num);
            break;
    }

    printLine(0, l1);
    printLine(1, l2);
    lcdTimeUs = micros() - t0;
}

//serial
void updateSerial() {
    if (millis() - lastSerial < 1000) return;
    lastSerial = millis();
    Serial.print(F("T=")); Serial.print(tempX10);
    Serial.print(F(" L=")); Serial.print(lightAvg);
    Serial.print(F(" sens_us=")); Serial.print(sensTimeUs);
    Serial.print(F(" lcd_us=")); Serial.println(lcdTimeUs);
}

//timer
// 16 MHz / 1024 = 15625 Hz; 15625 / 10 = 1562.5 -> OCR1A = 1561
void setupTimer1() {
    noInterrupts();
    TCCR1A = 0;
    TCCR1B = 0;
    TCNT1  = 0;
    OCR1A  = 1561;
    TCCR1B |= (1 << WGM12);                // CTC rezimas
    TCCR1B |= (1 << CS12) | (1 << CS10);   // prescaler 1024
    TIMSK1 |= (1 << OCIE1A);               // ijungti Compare A pertraukima
    interrupts();
}


// =====================================================================
void setup() {
    Serial.begin(9600);

    pinMode(PIN_BTN1, INPUT_PULLUP);
    pinMode(PIN_BTN2, INPUT_PULLUP);
    pinMode(PIN_BUZZ, OUTPUT);
    pinMode(PIN_NIGHT, OUTPUT);
    pinMode(PIN_STATUS, OUTPUT);

    lcd.init();
    lcd.backlight();
    Wire.setClock(400000);

    eepromWasValid = loadSettings();

    uint16_t t = readAdc(PIN_TEMP), l = readAdc(PIN_LDR);
    for (uint8_t i = 0; i < 8; i++) { tBuf[i] = t; lBuf[i] = l; }
    calcValues();

    attachInterrupt(digitalPinToInterrupt(PIN_BTN1), isrBtn1, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN2), isrBtn2, FALLING);
    setupTimer1();

    bootStart = millis();
    beepOk();
}

void loop() {
    if (btn1Flag)   { btn1Flag = false;   handleBtn1(); }
    if (btn2Flag)   { btn2Flag = false;   handleBtn2(); }
    if (sampleFlag) { sampleFlag = false; readSensors(); updateNightTarget(); }

    updateBoot();
    updateFade();
    updateStatusLed();
    updateLcd();
    updateSerial();
}
