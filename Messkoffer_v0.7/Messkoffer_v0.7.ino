// ================================================================
//  ADXL345 Vibrations-Logger — Vollversion v0.7
// ================================================================
//  Hardware: Arduino UNO R4 WiFi (or Mega), ADXL345, DS3231 RTC, SD
//
//  Befehle (Serial Monitor, 115200 Baud):
//    s = Aufnahme starten       x = Aufnahme stoppen
//    c = SD-Karte testen          i = Info/Einstellungen
//    t = Sensor-Test            z = Aktuelle Zeit
//    r = RTC auf Compile-Zeit   w = RTC manuell (DD.MM.YY HH:MM:SS)
//    n = Session-Notiz setzen
// ================================================================

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>

// ===================== EINSTELLUNGEN ============================
#define PIN_SD_CS           10

// Abtastrate [Hz]: 100 / 200 / 400 / 800 / 1600 / 3200
#define CFG_RATE            1600

// Messbereich [g]: 2 / 4 / 8 / 16
#define CFG_RANGE           16

// 1 = Binaer (.BIN, fuer hohe Raten)   0 = CSV (.CSV, nur niedrige Raten)
#define CFG_BINARY          1

#define CFG_DURATION        0       // 0 = unbegrenzt
#define CFG_ROTATION        10      // Minuten pro Datei (0 = aus)

// 1 = Aufnahme direkt nach Boot   0 = manuell mit 's'
#define CFG_AUTOSTART       1

// 1 = SD-Test beim Boot (Schreibtest) — empfohlen
#define CFG_SD_CHECK_BOOT   1

// I2C-Takt: 100000 = stabiler bei langen Kabeln, 400000 = schneller
#define CFG_I2C_CLOCK_HZ    400000

// Wie oft SD.flush() [Sekunden] — weniger Verlust bei Stromausfall
#define CFG_FLUSH_SEC       4

// Flush nur wenn FIFO nicht voll (sonst OVF waehrend flush)
#define CFG_FLUSH_FIFO_MAX  12

// Zusaetzlich alle N ms puffern schreiben (0 = nur RAM-voll)
#define CFG_WRITE_MS        300

// Schreiben wenn mindestens so viele Samples im RAM (kleinere Bloecke = weniger SD-Fehler)
#define CFG_WRITE_SAMPLES   256

// Serial-Fortschritt alle N s (0 = aus, z.B. Powerbank; 1 = USB-Test)
#define CFG_PROGRESS_SEC    1

// RAM-FIFOS: 32 Samples pro FIFO-Lesung
#define RAM_FIFOS           22

#define I2C_RETRY_MAX       3
#define SD_WRITE_RETRIES    3

// Stoppt Aufnahme nach N fehlgeschlagenen SD-Schreibversuchen hintereinander
#define SD_FAIL_STOP        10
// ===================== ENDE EINSTELLUNGEN =======================


#define ADXL_ADDR   0x53
#define RTC_ADDR    0x68

#define REG_DEVID       0x00
#define REG_BW_RATE     0x2C
#define REG_POWER_CTL   0x2D
#define REG_INT_SOURCE  0x30
#define REG_DATA_FORMAT 0x31
#define REG_DATAX0      0x32
#define REG_FIFO_CTL    0x38
#define REG_FIFO_STATUS 0x39

static const uint8_t BW_RATE_VAL = (CFG_RATE == 100  ? 0x0A :
                                    CFG_RATE == 200  ? 0x0B :
                                    CFG_RATE == 400  ? 0x0C :
                                    CFG_RATE == 800  ? 0x0D :
                                    CFG_RATE == 1600 ? 0x0E : 0x0F);

static const uint8_t DATA_FMT_VAL = 0x08 | (CFG_RANGE == 2  ? 0 :
                                             CFG_RANGE == 4  ? 1 :
                                             CFG_RANGE == 8  ? 2 : 3);

static const int FIFO_SAMPLES    = 32;
static const int RAM_BUF_SAMPLES = RAM_FIFOS * FIFO_SAMPLES;
int16_t ramBuf[RAM_BUF_SAMPLES * 3];
int     ramIdx = 0;

static const unsigned long ROTATION_MS   = (unsigned long)CFG_ROTATION * 60000UL;
static const unsigned long DURATION_MS   = (unsigned long)CFG_DURATION * 1000UL;
static const unsigned long FLUSH_MS      = (unsigned long)CFG_FLUSH_SEC * 1000UL;
static const unsigned long WRITE_MS      = (unsigned long)CFG_WRITE_MS;
static const unsigned long PROGRESS_MS   = (unsigned long)CFG_PROGRESS_SEC * 1000UL;

File          dataFile;
char          filename[13];
char          sessionNote[48] = "none";

bool          isRecording    = false;
bool          rtcAvail       = false;
bool          sdOk           = false;

unsigned long recStartMs     = 0;
unsigned long fileStartMs    = 0;
unsigned long sampleCount    = 0;
unsigned long totalSamples   = 0;   // nur erfolgreich auf SD geschriebene Samples
unsigned long samplesFromSensor = 0;
unsigned long samplesLostSD     = 0;
unsigned long samplesLostOVF    = 0;
unsigned long fifoReads      = 0;
unsigned long overflowCount  = 0; // OVF in aktueller Datei
unsigned long overflowTotal  = 0; // OVF gesamt (Session)
unsigned long sdWriteFailCount  = 0;
unsigned long sdCreateFailCount = 0;
unsigned long i2cErrorCount  = 0;
unsigned int  consecutiveSdWriteFails = 0;
unsigned long lastProgressMs = 0;
unsigned long lastFlushMs    = 0;
unsigned long lastWriteMs    = 0;
String        startTimeStr   = "";

static const char* MONTHS[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};


// ================================================================
//  RTC
// ================================================================

byte bcd2dec(byte v) { return (v / 16 * 10) + (v % 16); }
byte dec2bcd(byte v) { return (v / 10 * 16) + (v % 10); }

bool checkRTC() {
  Wire.beginTransmission(RTC_ADDR);
  return Wire.endTransmission() == 0;
}

bool readRTC(byte &dy, byte &mo, byte &yr, byte &hr, byte &mn, byte &sc) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7) != 7) return false;
  if (Wire.available() < 7) return false;
  sc = bcd2dec(Wire.read());
  mn = bcd2dec(Wire.read());
  hr = bcd2dec(Wire.read());
  Wire.read();
  dy = bcd2dec(Wire.read());
  mo = bcd2dec(Wire.read());
  yr = bcd2dec(Wire.read());
  return true;
}

String getDateTime() {
  byte d, m, y, h, mn, s;
  if (!readRTC(d, m, y, h, mn, s)) return "RTC Fehler";
  char buf[20];
  snprintf(buf, 20, "%02d.%02d.20%02d %02d:%02d:%02d", d, m, y, h, mn, s);
  return String(buf);
}

bool parseCompileTime(int &dy, int &mo, int &yr, int &hr, int &mn, int &sc) {
  char cd[] = __DATE__;
  char ct[] = __TIME__;
  char ms[4];
  sscanf(cd, "%s %d %d", ms, &dy, &yr);
  sscanf(ct, "%d:%d:%d", &hr, &mn, &sc);
  mo = 0;
  for (int i = 0; i < 12; i++) {
    if (strcmp(ms, MONTHS[i]) == 0) { mo = i + 1; break; }
  }
  return mo > 0;
}

void printCompileStamp() {
  Serial.print("  Sketch compile stamp: ");
  Serial.print(__DATE__);
  Serial.print(" ");
  Serial.println(__TIME__);
}

bool writeRTC(byte dy, byte mo, byte yr, byte hr, byte mn, byte sc) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  Wire.write(dec2bcd(sc));
  Wire.write(dec2bcd(mn));
  Wire.write(dec2bcd(hr));
  Wire.write(dec2bcd(1));
  Wire.write(dec2bcd(dy));
  Wire.write(dec2bcd(mo));
  Wire.write(dec2bcd(yr));
  return Wire.endTransmission() == 0;
}

void setRTCCompileTime() {
  int dy, mo, yr, hr, mn, sc;
  if (!parseCompileTime(dy, mo, yr, hr, mn, sc)) {
    Serial.println("RTC FEHLER: Compile-Zeit nicht lesbar");
    return;
  }
  printCompileStamp();
  Serial.println("  Hinweis: 'r' nutzt Compile-Zeit — Sketch neu kompilieren + hochladen!");
  Serial.println("  Fuer exakte Zeit jetzt: w DD.MM.YY HH:MM:SS");

  if (!writeRTC((byte)dy, (byte)mo, (byte)(yr - 2000), (byte)hr, (byte)mn, (byte)sc)) {
    Serial.println("RTC FEHLER: Schreiben fehlgeschlagen");
    return;
  }
  Serial.print("RTC gesetzt: ");
  Serial.println(getDateTime());
}

void setRTCFromSerialInput() {
  Serial.println("RTC setzen — Format: DD.MM.YY HH:MM:SS  (Beispiel: 06.07.26 11:45:00)");
  char buf[32];
  int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 30000) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') break;
      if (n < 31) buf[n++] = c;
    }
  }
  buf[n] = '\0';
  if (n == 0) {
    Serial.println("Abgebrochen (leer).");
    return;
  }

  int dy, mo, yr, hr, mn, sc;
  if (sscanf(buf, "%d.%d.%d %d:%d:%d", &dy, &mo, &yr, &hr, &mn, &sc) != 6) {
    Serial.print("Format ungueltig: ");
    Serial.println(buf);
    return;
  }
  if (yr >= 2000) yr -= 2000;
  if (dy < 1 || dy > 31 || mo < 1 || mo > 12 || hr > 23 || mn > 59 || sc > 59) {
    Serial.println("Werte ausserhalb Bereich.");
    return;
  }
  if (!writeRTC((byte)dy, (byte)mo, (byte)yr, (byte)hr, (byte)mn, (byte)sc)) {
    Serial.println("RTC FEHLER: Schreiben fehlgeschlagen");
    return;
  }
  Serial.print("RTC gesetzt: ");
  Serial.println(getDateTime());
}


// ================================================================
//  ADXL345 (mit I2C-Wiederholungen)
// ================================================================

bool i2cWrite(uint8_t addr, const uint8_t* data, uint8_t len) {
  for (int attempt = 0; attempt < I2C_RETRY_MAX; attempt++) {
    Wire.beginTransmission(addr);
    for (uint8_t i = 0; i < len; i++) Wire.write(data[i]);
    if (Wire.endTransmission() == 0) return true;
    delay(1);
  }
  i2cErrorCount++;
  return false;
}

bool i2cRead(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t len) {
  for (int attempt = 0; attempt < I2C_RETRY_MAX; attempt++) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) continue;
    if (Wire.requestFrom(addr, len) != len) continue;
    for (uint8_t i = 0; i < len; i++) {
      if (!Wire.available()) break;
      buf[i] = Wire.read();
    }
    return true;
  }
  i2cErrorCount++;
  return false;
}

void adxlWrite(uint8_t reg, uint8_t val) {
  uint8_t d[2] = {reg, val};
  i2cWrite(ADXL_ADDR, d, 2);
}

uint8_t adxlRead(uint8_t reg) {
  uint8_t v = 0;
  i2cRead(ADXL_ADDR, reg, &v, 1);
  return v;
}

bool adxlInit() {
  if (adxlRead(REG_DEVID) != 0xE5) return false;
  adxlWrite(REG_POWER_CTL,   0x00); delay(10);
  adxlWrite(REG_DATA_FORMAT, DATA_FMT_VAL); delay(5);
  adxlWrite(REG_BW_RATE,     BW_RATE_VAL);  delay(5);
  adxlWrite(REG_FIFO_CTL,    0x00);         delay(5);
  adxlWrite(REG_FIFO_CTL,    0x80 | 31);    delay(5);
  adxlRead(REG_INT_SOURCE);
  adxlWrite(REG_POWER_CTL,   0x08);         delay(10);
  return true;
}

void adxlReset() {
  adxlWrite(REG_POWER_CTL, 0x00); delay(10);
  adxlWrite(REG_FIFO_CTL,  0x00); delay(10);
  adxlRead(REG_INT_SOURCE);         delay(5);
}

uint8_t adxlFifoEntries() {
  return adxlRead(REG_FIFO_STATUS) & 0x3F;
}

void adxlResetFifo() {
  adxlWrite(REG_FIFO_CTL, 0x00);      delay(2);
  adxlWrite(REG_FIFO_CTL, 0x80 | 31); delay(5);
}

bool readFifoToRam() {
  if (ramIdx + FIFO_SAMPLES > RAM_BUF_SAMPLES) return false;

  for (int i = 0; i < FIFO_SAMPLES; i++) {
    uint8_t b[6];
    if (!i2cRead(ADXL_ADDR, REG_DATAX0, b, 6)) return false;
    int p = ramIdx * 3;
    ramBuf[p]     = (int16_t)((b[1] << 8) | b[0]);
    ramBuf[p + 1] = (int16_t)((b[3] << 8) | b[2]);
    ramBuf[p + 2] = (int16_t)((b[5] << 8) | b[4]);
    ramIdx++;
  }
  return true;
}


// ================================================================
//  SD — Test, Dateien, Schreiben
// ================================================================

void stopRecording();

bool sdCardSelfTest() {
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  SPI.begin();

  Serial.println("\n--- SD-Karten-Test ---");

  Serial.print("  SD.begin: ");
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("FEHLER (Verkabelung / Karte / FAT32?)");
    sdOk = false;
    return false;
  }
  Serial.println("OK");

  File tf = SD.open("SDTEST.TXT", FILE_WRITE);
  if (!tf) {
    Serial.println("  Schreibtest: FEHLER");
    sdOk = false;
    return false;
  }
  tf.println("Messkoffer SD test OK");
  tf.close();
  SD.remove("SDTEST.TXT");
  Serial.println("  Schreibtest: OK");
  Serial.println("--- SD bereit ---\n");
  sdOk = true;
  return true;
}

void makeFilename() {
#if CFG_BINARY
  const char* ext = "BIN";
#else
  const char* ext = "CSV";
#endif

  if (rtcAvail) {
    byte dy, mo, yr, hr, mn, sc;
    if (readRTC(dy, mo, yr, hr, mn, sc)) {
      snprintf(filename, 13, "%02d%02d%02d%02d.%s", dy, mo, hr, mn, ext);
      if (SD.exists(filename)) {
        char base[7];
        snprintf(base, 7, "%02d%02d%02d", dy, mo, hr);
        for (int i = 0; i <= 99; i++) {
          snprintf(filename, 13, "%s%02d.%s", base, i, ext);
          if (!SD.exists(filename)) return;
        }
      }
      return;
    }
  }

  for (int i = 1; i <= 9999; i++) {
    snprintf(filename, 13, "DAT%04d.%s", i, ext);
    if (!SD.exists(filename)) return;
  }
}

void writeFileHeader() {
#if CFG_BINARY
  dataFile.println("# ADXL345 Vibrations-Logger v0.7 (Binaer)");
  dataFile.print("# Start: ");       dataFile.println(startTimeStr);
  dataFile.print("# Session: ");     dataFile.println(sessionNote);
  dataFile.print("# Rate: ");        dataFile.print(CFG_RATE); dataFile.println(" Hz");
  dataFile.print("# Bereich: +/-");  dataFile.print(CFG_RANGE); dataFile.println(" g");
  dataFile.println("# Format: int16_t X,Y,Z interleaved, little-endian");
  dataFile.println("# Skalierung: 3.9 mg/LSB (Full Resolution)");
  dataFile.println("# Hinweis: Externe Anregungsfrequenz in Session-Notiz (Befehl n)");
  dataFile.println("---DATA---");
#else
  dataFile.println("# ADXL345 Vibrations-Logger v0.7 (CSV)");
  dataFile.print("# Start: ");       dataFile.println(startTimeStr);
  dataFile.print("# Session: ");     dataFile.println(sessionNote);
  dataFile.print("# Rate: ");        dataFile.print(CFG_RATE); dataFile.println(" Hz");
  dataFile.print("# Bereich: +/-");  dataFile.print(CFG_RANGE); dataFile.println(" g");
  dataFile.println("Sample,time_ms,X_raw,Y_raw,Z_raw");
#endif
}

void createFile() {
  makeFilename();
  dataFile = SD.open(filename, FILE_WRITE);
  if (!dataFile) {
    sdCreateFailCount++;
    Serial.print("FEHLER: Datei nicht erstellt! (");
    Serial.print(filename);
    Serial.print(") #");
    Serial.println(sdCreateFailCount);
    return;
  }
  writeFileHeader();
  dataFile.flush();
  Serial.print("Datei: ");
  Serial.println(filename);
}

bool writeBufferToSD() {
  if (!dataFile || ramIdx == 0) return true;

  bool ok = false;

#if CFG_BINARY
  size_t bytes = (size_t)ramIdx * 3 * sizeof(int16_t);
  for (int attempt = 0; attempt < SD_WRITE_RETRIES; attempt++) {
    size_t written = dataFile.write((const uint8_t*)ramBuf, bytes);
    if (written == bytes) {
      ok = true;
      break;
    }
    if (attempt + 1 < SD_WRITE_RETRIES) delay(2);
  }
#else
  ok = true;
  for (int i = 0; i < ramIdx; i++) {
    int p = i * 3;
    unsigned long idx = sampleCount + (unsigned long)i;
    float tMs = (float)idx * (1000.0f / (float)CFG_RATE);
    dataFile.print(idx);
    dataFile.print(',');
    dataFile.print(tMs, 2);
    dataFile.print(',');
    dataFile.print(ramBuf[p]);
    dataFile.print(',');
    dataFile.print(ramBuf[p + 1]);
    dataFile.print(',');
    dataFile.println(ramBuf[p + 2]);
  }
#endif

  if (ok) {
    sampleCount    += ramIdx;
    totalSamples   += ramIdx;
    consecutiveSdWriteFails = 0;
  } else {
    sdWriteFailCount++;
    samplesLostSD += ramIdx;
    consecutiveSdWriteFails++;
    Serial.print(" [SD err #");
    Serial.print(sdWriteFailCount);
    Serial.print("]");
    if (consecutiveSdWriteFails >= SD_FAIL_STOP) {
      Serial.println(" Too many SD errors - stopping.");
      stopRecording();
    }
  }

  ramIdx = 0;
  lastWriteMs = millis();
  return ok;
}

// Flush nur wenn Sensor-FIFO Luft hat — schuetzt vor OVF und Stromausfall
void tryFlushToSD() {
  if (!dataFile) return;
  if (millis() - lastFlushMs < FLUSH_MS) return;
  if (adxlFifoEntries() > CFG_FLUSH_FIFO_MAX) return;
  dataFile.flush();
  lastFlushMs = millis();
}

bool shouldWriteBuffer() {
  if (ramIdx == 0) return false;
  if (ramIdx >= RAM_BUF_SAMPLES - FIFO_SAMPLES) return true;
  if (CFG_WRITE_SAMPLES > 0 && ramIdx >= CFG_WRITE_SAMPLES
      && WRITE_MS > 0 && (millis() - lastWriteMs) >= WRITE_MS) return true;
  if (WRITE_MS > 0 && (millis() - lastWriteMs) >= WRITE_MS) return true;
  return false;
}

void closeFile() {
  if (!dataFile) return;
  if (ramIdx > 0) writeBufferToSD();
  dataFile.flush();
  dataFile.close();
}


// ================================================================
//  Aufnahme
// ================================================================

void startRecording() {
  if (isRecording) return;
  if (!sdOk) {
    Serial.println("SD nicht bereit! Befehl 'c' zum Testen.");
    return;
  }

  Serial.println("\n=== AUFNAHME STARTET ===");
  startTimeStr = rtcAvail ? getDateTime() : "keine RTC";
  Serial.print("Zeit: ");
  Serial.println(startTimeStr);
  Serial.print("Session: ");
  Serial.println(sessionNote);

  adxlResetFifo();
  delay(5);

  sampleCount = totalSamples = fifoReads = 0;
  samplesFromSensor = samplesLostSD = samplesLostOVF = 0;
  overflowCount = overflowTotal = 0;
  sdWriteFailCount = sdCreateFailCount = 0;
  consecutiveSdWriteFails = 0;
  i2cErrorCount = 0;
  ramIdx = 0;
  recStartMs = fileStartMs = millis();
  lastProgressMs = lastFlushMs = lastWriteMs = millis();

  createFile();
  if (dataFile) {
    isRecording = true;
    Serial.println("Aufnahme laeuft...");
  }
}

void rotateFile() {
  writeBufferToSD();
  dataFile.flush();
  Serial.print("\nDatei fertig: ");
  Serial.print(filename);
  Serial.print(" | ");
  Serial.print(sampleCount);
  Serial.print(" Smp | OVF: ");
  Serial.print(overflowCount);
  Serial.print("/");
  Serial.print(overflowTotal);
  Serial.print(" | SDerr: ");
  Serial.println(sdWriteFailCount);
  dataFile.close();

  startTimeStr = rtcAvail ? getDateTime() : String((millis() - recStartMs) / 1000) + "s";
  sampleCount = 0;
  overflowCount = 0;
  fileStartMs = lastFlushMs = lastWriteMs = millis();
  createFile();
  if (!dataFile) {
    sdCreateFailCount++;
    Serial.print("FEHLER: Neue Datei! Stoppe. #");
    Serial.println(sdCreateFailCount);
    isRecording = false;
  }
}

void stopRecording() {
  if (!isRecording) return;
  isRecording = false;
  closeFile();

  unsigned long dur = millis() - recStartMs;
  float durSec = dur / 1000.0f;
  float rateCfg = (float)CFG_RATE;
  float rateWr = durSec > 0 ? (float)totalSamples / durSec : 0;
  float rateRd = durSec > 0 ? (float)samplesFromSensor / durSec : 0;

  unsigned long expectedSamples = durSec > 0
    ? (unsigned long)(durSec * rateCfg + 0.5f) : 0;

  unsigned long lostCapture = 0;
  if (expectedSamples > samplesFromSensor) {
    lostCapture = expectedSamples - samplesFromSensor;
  }

  unsigned long lostSd = 0;
  if (samplesFromSensor > totalSamples) {
    lostSd = samplesFromSensor - totalSamples;
  }

  unsigned long lostLoop = 0;
  if (lostCapture > samplesLostOVF) {
    lostLoop = lostCapture - samplesLostOVF;
  }

  unsigned long lostTotal = 0;
  if (expectedSamples > totalSamples) {
    lostTotal = expectedSamples - totalSamples;
  }

  Serial.println("\n=== RECORDING STOPPED ===");
  Serial.print("Duration: "); Serial.print(durSec, 1); Serial.println(" s");
  Serial.print("Configured rate:        "); Serial.print((int)rateCfg); Serial.println(" Hz");
  Serial.print("Expected (config):      "); Serial.println(expectedSamples);
  Serial.print("Samples read (sensor):  "); Serial.println(samplesFromSensor);
  Serial.print("Samples on SD:          "); Serial.println(totalSamples);

  if (expectedSamples > 0) {
    float pctSaved = (float)totalSamples * 100.0f / (float)expectedSamples;
    float pctTotalLost = (float)lostTotal * 100.0f / (float)expectedSamples;
    float pctCapture = (float)lostCapture * 100.0f / (float)expectedSamples;
    float pctOvf = (float)samplesLostOVF * 100.0f / (float)expectedSamples;
    float pctLoop = (float)lostLoop * 100.0f / (float)expectedSamples;
    float pctSd = (float)lostSd * 100.0f / (float)expectedSamples;

    Serial.println("\n--- Loss (vs configured rate) ---");
    Serial.print("Total lost:             ");
    Serial.print(lostTotal);
    Serial.print(" (");
    Serial.print(pctTotalLost, 1);
    Serial.println(" %)");
    Serial.print("Saved on SD:            ");
    Serial.print(totalSamples);
    Serial.print(" (");
    Serial.print(pctSaved, 1);
    Serial.println(" %)");
    Serial.print("  Capture loss:         ");
    Serial.print(lostCapture);
    Serial.print(" (");
    Serial.print(pctCapture, 1);
    Serial.println(" %)  [config -> sensor]");
    Serial.print("    FIFO overflow:     ");
    Serial.print(samplesLostOVF);
    Serial.print(" (");
    Serial.print(pctOvf, 1);
    Serial.println(" %)");
    Serial.print("    Loop/overhead:     ");
    Serial.print(lostLoop);
    Serial.print(" (");
    Serial.print(pctLoop, 1);
    Serial.println(" %)  [Arduino timing]");
    Serial.print("  SD write loss:        ");
    Serial.print(lostSd);
    Serial.print(" (");
    Serial.print(pctSd, 1);
    Serial.println(" %)  [sensor -> SD]");
  } else {
    Serial.println("\n--- Loss ---");
    Serial.println("n/a (zero duration)");
  }

  Serial.println("\n--- Rates ---");
  Serial.print("Rate configured:        "); Serial.print((int)rateCfg); Serial.println(" Hz");
  Serial.print("Rate read (sensor):     "); Serial.print(rateRd, 0); Serial.println(" Hz");
  Serial.print("Rate on SD:             "); Serial.print(rateWr, 0); Serial.println(" Hz");

  Serial.println("\n--- Diagnostics ---");
  Serial.print("I2C errors:             "); Serial.println(i2cErrorCount);
  Serial.print("SD write errors:        "); Serial.print(sdWriteFailCount);
  Serial.print(" (");
  Serial.print(samplesLostSD);
  Serial.println(" samples)");
  Serial.print("FIFO overflow events:   "); Serial.println(overflowTotal);
  Serial.print("File create errors:     "); Serial.println(sdCreateFailCount);
  Serial.print("File: "); Serial.println(filename);
  if (totalSamples == 0) {
    Serial.println("\n*** WARNUNG: 0 Samples auf SD — nur Header? ***");
  }
  Serial.println();
}

void showInfo() {
  Serial.println("\n=== EINSTELLUNGEN v0.7 ===");
  Serial.print("  Rate:          "); Serial.print(CFG_RATE); Serial.println(" Hz");
  Serial.print("  Bereich:       +/-"); Serial.print(CFG_RANGE); Serial.println(" g");
#if CFG_BINARY
  Serial.println("  Format:        Binaer (.BIN) — fuer hohe Raten");
#else
  Serial.println("  Format:        CSV (.CSV) — nur bis ~800 Hz empfohlen");
#endif
  Serial.print("  Auto-Start:    "); Serial.println(CFG_AUTOSTART ? "Ja" : "Nein");
  Serial.print("  SD-Check Boot: "); Serial.println(CFG_SD_CHECK_BOOT ? "Ja" : "Nein");
  Serial.print("  I2C-Takt:      "); Serial.print(CFG_I2C_CLOCK_HZ); Serial.println(" Hz");
  Serial.print("  SD flush:      "); Serial.print(CFG_FLUSH_SEC); Serial.println(" s (safe)");
  Serial.print("  Flush FIFO max:"); Serial.println(CFG_FLUSH_FIFO_MAX);
  Serial.print("  SD write alle: "); Serial.print(CFG_WRITE_MS); Serial.println(" ms");
  Serial.print("  SD write ab:   "); Serial.print(CFG_WRITE_SAMPLES); Serial.println(" Samples");
  Serial.print("  Serial prog:   ");
  if (CFG_PROGRESS_SEC > 0) { Serial.print(CFG_PROGRESS_SEC); Serial.println(" s"); }
  else { Serial.println("aus"); }
  Serial.print("  RAM-Buffer:    "); Serial.print(RAM_BUF_SAMPLES); Serial.println(" Samples");
  Serial.print("  Session:       "); Serial.println(sessionNote);
  Serial.print("  SD Status:     "); Serial.println(sdOk ? "OK" : "FEHLER");
  if (rtcAvail) { Serial.print("  RTC:           "); Serial.println(getDateTime()); }
  Serial.println();
}

void readSessionNoteFromSerial() {
  Serial.println("Session-Notiz (z.B. '150Hz_sweep' oder '3000Hz_test'), Enter zum Speichern:");
  char buf[48];
  int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 30000) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') break;
      if (n < 47) buf[n++] = c;
    }
  }
  buf[n] = '\0';
  if (n > 0) {
    strncpy(sessionNote, buf, 47);
    sessionNote[47] = '\0';
    Serial.print("Session gesetzt: ");
    Serial.println(sessionNote);
  }
}


// ================================================================
//  SETUP / LOOP
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n=============================================");
  Serial.println("  ADXL345 Vibrations-Logger — v0.7");
  Serial.println("=============================================\n");

#if !CFG_BINARY && CFG_RATE > 800
  Serial.println("WARNUNG: CSV bei >800 Hz fuehrt zu Datenverlust!");
  Serial.println("         CFG_BINARY auf 1 setzen fuer 3200 Hz.\n");
#endif

  Wire.begin();
  Wire.setClock(CFG_I2C_CLOCK_HZ);
  delay(100);

  Serial.println("I2C Scan:");
  for (byte a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print("  0x"); Serial.print(a, HEX);
      if (a == ADXL_ADDR) Serial.print(" (ADXL345)");
      if (a == RTC_ADDR)  Serial.print(" (RTC)");
      Serial.println();
    }
  }
  Serial.println();

  rtcAvail = checkRTC();
  Serial.print("RTC: ");
  if (rtcAvail) {
    Serial.println("OK");
    byte dy, mo, yr, hr, mn, sc;
    if (readRTC(dy, mo, yr, hr, mn, sc) && yr < 24) {
      setRTCCompileTime();
    } else {
      Serial.print("  Zeit: ");
      Serial.println(getDateTime());
    }
  } else {
    Serial.println("NICHT GEFUNDEN");
  }
  Serial.println();

  if (CFG_SD_CHECK_BOOT) {
    if (!sdCardSelfTest()) {
      Serial.println("SD FEHLER! Befehl 'c' zum Wiederholen. Kein Auto-Start.");
    }
  } else {
    SPI.begin();
    sdOk = SD.begin(PIN_SD_CS);
    Serial.print("SD-Karte: ");
    Serial.println(sdOk ? "OK" : "FEHLER!");
  }

  Serial.print("ADXL345: ");
  adxlReset();
  if (!adxlInit()) {
    Serial.println("FEHLER!");
    while (1) delay(1000);
  }
  Serial.print("OK (");
  Serial.print(CFG_RATE);
  Serial.println(" Hz)\n");

  showInfo();
  Serial.println("BEFEHLE: s=Start x=Stop c=SD-Test i=Info t=Test n=Session z=Zeit r=RTC w=RTC manuell\n");

#if CFG_AUTOSTART
  if (sdOk) {
    startRecording();
  } else {
    Serial.println("Auto-Start ausgelassen (SD nicht OK).");
  }
#endif
}

void loop() {
  // Nur bei USB-Serial — verhindert Zufallsbytes am Powerbank (x stoppt sonst)
  if (Serial && Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 's': case 'S': startRecording(); break;
      case 'x': case 'X': stopRecording();  break;
      case 'c': case 'C': sdCardSelfTest();   break;
      case 'i': case 'I': showInfo();         break;
      case 'n': case 'N': readSessionNoteFromSerial(); break;
      case 'z': case 'Z':
        if (rtcAvail) { Serial.print("Zeit: "); Serial.println(getDateTime()); }
        break;
      case 'r': case 'R':
        if (rtcAvail) setRTCCompileTime();
        break;
      case 'w': case 'W':
        if (rtcAvail) setRTCFromSerialInput();
        break;
      case 't': case 'T':
        if (isRecording) { Serial.println("Erst stoppen (x)!"); break; }
        Serial.println("--- Sensor-Test ---");
        Serial.print("FIFO: "); Serial.println(adxlFifoEntries());
        Serial.print("I2C-Fehler bisher: "); Serial.println(i2cErrorCount);
        if (adxlFifoEntries() >= 32) {
          ramIdx = 0;
          readFifoToRam();
          for (int i = 0; i < 4; i++) {
            int p = i * 3;
            Serial.print(i); Serial.print(": ");
            Serial.print(ramBuf[p]); Serial.print(", ");
            Serial.print(ramBuf[p+1]); Serial.print(", ");
            Serial.println(ramBuf[p+2]);
          }
          ramIdx = 0;
        }
        Serial.println("--- Ende ---");
        break;
    }
  }

  if (!isRecording) {
    delay(10);
    return;
  }

  if (DURATION_MS > 0 && millis() - recStartMs >= DURATION_MS) {
    stopRecording();
    return;
  }

  if (ROTATION_MS > 0 && millis() - fileStartMs >= ROTATION_MS) {
    rotateFile();
  }

  uint8_t entries = adxlFifoEntries();

  if (entries > 33) {
    overflowCount++;
    overflowTotal++;
    samplesLostOVF += 32;
    if (ramIdx > 0) writeBufferToSD();
    adxlResetFifo();
    delay(1);
    return;
  }

  if (entries >= 28 && ramIdx >= RAM_BUF_SAMPLES - FIFO_SAMPLES) {
    writeBufferToSD();
  }

  if (entries >= 32) {
    if (readFifoToRam()) {
      fifoReads++;
      samplesFromSensor += FIFO_SAMPLES;
    }
    if (shouldWriteBuffer()) writeBufferToSD();
  }

  tryFlushToSD();

  if (PROGRESS_MS > 0 && millis() - lastProgressMs >= PROGRESS_MS) {
    float rateWr = (millis() - recStartMs) > 0
      ? (float)(totalSamples + ramIdx) / ((millis() - recStartMs) / 1000.0f) : 0;
    Serial.print("  ");
    Serial.print((millis() - recStartMs) / 1000);
    Serial.print("s | Wr:");
    Serial.print(totalSamples + ramIdx);
    Serial.print(" Rd:");
    Serial.print(samplesFromSensor);
    Serial.print(" | ");
    Serial.print(rateWr, 0);
    Serial.print("Hz | OVF:");
    Serial.print(overflowCount);
    Serial.print("/");
    Serial.print(overflowTotal);
    Serial.print(" | SDerr:");
    Serial.print(sdWriteFailCount);
    Serial.print(" | I2C:");
    Serial.println(i2cErrorCount);
    lastProgressMs = millis();
  }
}
