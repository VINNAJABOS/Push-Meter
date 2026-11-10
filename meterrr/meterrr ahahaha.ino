/*
  Push Meter - 2 Load Cell HX711 + LCD 20x4 I2C (ESP32)
  Data dikirim lewat Bluetooth LE saja (tanpa WiFi dan Firebase): dashboard web memakai
  Web Bluetooth (Chrome/Edge di laptop, Chrome di Android) -> realtime + tare + kalibrasi.

  Library:
    - "HX711" by Rob Tillaart
    - "LiquidCrystal_I2C" by Frank de Brabander

  Wiring:
    Load Cell 1 (KIRI)  : DT -> GPIO 4  , SCK -> GPIO 5
    Load Cell 2 (KANAN) : DT -> GPIO 25 , SCK -> GPIO 26
    LCD 20x4 I2C        : SDA -> GPIO 21, SCL -> GPIO 22, VCC -> 5V, GND -> GND

  Bluetooth:
    Nama perangkat : "Pluto AI"
    DATA   (notify): 11 byte little-endian = float kiri(g), float kanan(g), uint8 flag (bit0 = sedang Record),
                     uint16 detik Record berjalan
    CMD    (write) : teks, salah satu:
        T                      tare            C1:<gram> / C2:<gram>   kalibrasi kiri / kanan
        START_RECORD:<unix>    mulai Record (unix = detik epoch dari web, untuk timestamp)
        STOP_RECORD            selesai, hitung Peak/Average/Duration, simpan ke flash
        GET_HISTORY            kirim ringkasan semua sesi     GET_RECORD:<id>   kirim detail satu sesi
        DELETE_RECORD:<id>     hapus satu sesi                CLEAR_HISTORY     hapus semua sesi
    STATUS (notify): teks hasil perintah, mis. "OK:T", "OK:START:7", "OK:STOP:7", "ERR:..."
    XFER   (notify): aliran pesan berbingkai [tipe u8][panjang u16][payload][checksum u8], dipotong
                     per 20 byte. Tipe 1 item riwayat, 2 akhir riwayat, 3 header rekaman,
                     4 sampel rekaman, 5 hasil sesi terakhir. Header = id u32, ts u32, durasi u16,
                     jumlah sampel u16, puncak total u16, rata-rata u16, puncak kiri u16, puncak kanan u16
                     (semua nilai kg dalam satuan 0,01 kg).

  Record: ESP32 membaca terus, mencatat 1 nilai per detik (kg total kiri+kanan), maksimal
  600 detik per sesi dan 50 sesi tersimpan di flash (LittleFS); sesi terlama dihapus otomatis.
  Auto-zero dimatikan selama Record supaya tekanan yang ditahan tidak dianggap 0.

  Faktor kalibrasi yang dibuat lewat web/Serial disimpan di flash (NVS) dan menggantikan
  nilai awal calFactor di bawah setelah restart.

  Board: ESP32 Dev Module. Tools > Partition Scheme > "Huge APP (3MB No OTA/1MB SPIFFS)"
  (arduino-cli: --fqbn esp32:esp32:esp32:PartitionScheme=huge_app)

  Cara pakai (Serial Monitor 115200, line ending "Newline"):
    t      -> tare (nol-kan) kedua load cell (kosongkan timbangan dulu)
    1      -> kalibrasi load cell 1 (letakkan beban yang diketahui beratnya)
    2      -> kalibrasi load cell 2
    +/-    -> naik/turunkan faktor kalibrasi LC1 secara manual (x10: a/z)
    ]/[    -> naik/turunkan faktor kalibrasi LC2 secara manual (x10: s/x)
    p      -> tampilkan faktor kalibrasi saat ini
    r      -> mulai / selesai Record dari tombol Serial (tanpa web)
    h      -> cetak riwayat sesi yang tersimpan di flash
*/

#include <Wire.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>                      // deskriptor CCCD: tanpa ini browser tidak bisa mengaktifkan notifikasi
#include <LiquidCrystal_I2C.h>
#include "HX711.h"

// ---------- Bluetooth LE ----------
#define BLE_NAME         "Pluto AI"
#define BLE_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_DATA_UUID    "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // notify: bacaan realtime
#define BLE_CMD_UUID     "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // write : perintah dari web
#define BLE_STATUS_UUID  "6e400004-b5a3-f393-e0a9-e50e24dcca9e"   // notify: hasil perintah
#define BLE_XFER_UUID    "6e400005-b5a3-f393-e0a9-e50e24dcca9e"   // notify: riwayat dan rekaman tersimpan

// ---------- Record ----------
#define REC_MAX_SECONDS  600     // batas satu sesi
#define REC_MAX_SESSIONS 50      // sesi terlama dihapus kalau lewat batas ini

struct __attribute__((packed)) RecHeader {
  uint32_t id;
  uint32_t ts;        // detik epoch dari web saat START_RECORD (0 = tidak diketahui)
  uint16_t dur;       // detik
  uint16_t count;     // jumlah sampel (1 per detik)
  uint16_t peak;      // puncak total kiri+kanan, 0,01 kg
  uint16_t avg;       // rata-rata total, 0,01 kg
  uint16_t peakL;     // puncak load cell kiri, 0,01 kg
  uint16_t peakR;     // puncak load cell kanan, 0,01 kg
};

const int LC1_DOUT = 4;
const int LC1_SCK  = 5;
const int LC2_DOUT = 25;
const int LC2_SCK  = 26;

// alamat I2C LCD dicari otomatis saat startup (biasanya 0x27 atau 0x3F)
const int LCD_SDA = 21;
const int LCD_SCL = 22;
LiquidCrystal_I2C *lcd = nullptr;
bool lcdOk = false;                            // false = LCD tidak ditemukan, program tetap jalan tanpa LCD

HX711 scale1;
HX711 scale2;

float calFactor1 = 6;
float calFactor2 = 6;
Preferences prefs;                             // penyimpanan faktor kalibrasi di flash

BLEServer *bleServer = nullptr;
BLECharacteristic *bleData = nullptr;
BLECharacteristic *bleStatus = nullptr;
BLECharacteristic *bleXfer = nullptr;
volatile bool bleConnected = false;
volatile bool bleCmdPending = false;           // perintah diterima di task BLE, dijalankan di loop()
String bleCmd;

// bacaan mentah dalam rentang ini dari titik nol dianggap 0 (menghilangkan noise HX711 ±30 count)
const long ZERO_BAND = 500;

// auto-zero: jika bacaan stabil (selisih max-min < STABLE_SPREAD) selama STABLE_TIME_MS,
// titik nol otomatis dipindah ke bacaan tersebut sehingga tampil 0 g
const long STABLE_SPREAD = 1000;                // count mentah (contoh: -1137..-1184 = 47 -> stabil)
const unsigned long STABLE_TIME_MS = 5000;

struct AutoZero {
  unsigned long startMs = 0;
  long minRaw = 0, maxRaw = 0;
  long long sum = 0;
  int count = 0;
};
AutoZero az1, az2;

void autoZero(HX711 &scale, AutoZero &az, long raw, int no) {
  if (az.count == 0 || raw - az.minRaw >= STABLE_SPREAD || az.maxRaw - raw >= STABLE_SPREAD) {
    // mulai jendela baru (awal, atau bacaan berubah terlalu jauh = tidak stabil)
    az.startMs = millis();
    az.minRaw = az.maxRaw = raw;
    az.sum = raw;
    az.count = 1;
    return;
  }
  if (raw < az.minRaw) az.minRaw = raw;
  if (raw > az.maxRaw) az.maxRaw = raw;
  az.sum += raw;
  az.count++;

  if (millis() - az.startMs >= STABLE_TIME_MS) {
    long avg = (long)(az.sum / az.count);
    if (labs(avg - scale.get_offset()) >= ZERO_BAND) {
      scale.set_offset(avg);
      Serial.print(">> Auto-zero LC"); Serial.print(no); Serial.println(": stabil 5 detik, direset ke 0 g");
    }
    az.count = 0;                               // mulai hitung ulang
  }
}

// ======================= LCD =======================

// scan bus I2C, pakai alamat pertama yang merespons; cetak hasilnya di Serial
void lcdDetect() {
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setTimeOut(20);
  uint8_t found = 0;
  Serial.print("Scan I2C:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(" 0x"); Serial.print(addr, HEX);
      if (!found) found = addr;
    }
  }
  if (!found) {
    Serial.println(" tidak ada perangkat -> LCD dilewati (cek SDA=21, SCL=22, VCC, GND)");
    return;
  }
  Serial.println();
  Serial.print("LCD dipakai di alamat 0x"); Serial.println(found, HEX);
  lcd = new LiquidCrystal_I2C(found, 20, 4);
  lcdOk = true;
}

// karakter custom 0..4 = blok berisi 1..5 kolom piksel (untuk progress bar halus)
void lcdLoadBarChars() {
  if (!lcdOk) return;
  for (uint8_t level = 1; level <= 5; level++) {
    uint8_t rowBits = (0x1F << (5 - level)) & 0x1F;
    uint8_t ch[8];
    for (int i = 0; i < 8; i++) ch[i] = rowBits;
    lcd->createChar(level - 1, ch);
  }
}

// tulis teks tepat 20 kolom (sisa diisi spasi) supaya tidak perlu lcd.clear() -> tidak berkedip
void lcdLine(uint8_t row, const char *text) {
  if (!lcdOk) return;
  char buf[21];
  snprintf(buf, sizeof(buf), "%-20s", text);
  lcd->setCursor(0, row);
  lcd->print(buf);
}

void lcdCenter(uint8_t row, const char *text) {
  char buf[21];
  int len = strlen(text);
  int pad = len < 20 ? (20 - len) / 2 : 0;
  snprintf(buf, sizeof(buf), "%*s%s", pad, "", text);
  lcdLine(row, buf);
}

// progress bar 20 kolom x 5 piksel = 100 langkah
void lcdProgress(uint8_t row, int percent) {
  if (!lcdOk) return;
  percent = constrain(percent, 0, 100);
  lcd->setCursor(0, row);
  for (int col = 0; col < 20; col++) {
    int fill = percent - col * 5;
    if (fill >= 5)     lcd->write(byte(4));
    else if (fill > 0) lcd->write(byte(fill - 1));
    else               lcd->print(' ');
  }
}

// baris judul + status Bluetooth (diperbarui tiap siklus)
void lcdHeader(bool bt, bool rec, unsigned long recSeconds) {
  char line[21];
  if (rec) snprintf(line, sizeof(line), "PUSHMETER REC %02lu:%02lu", recSeconds / 60, recSeconds % 60);
  else     snprintf(line, sizeof(line), "PUSHMETER %10s", bt ? "BT TERHUBUNG" : "BT MENUNGGU");
  lcdLine(0, line);
}

void introAnimation() {
  if (!lcdOk) return;
  lcd->clear();
  lcdCenter(0, "PUSH METER");
  lcdCenter(1, "Tarikan Kiri-Kanan");
  delay(1500);
}

// tampilkan progress bar selama HX711 menstabilkan diri (pengganti delay biasa)
void stabilizeWithProgress(unsigned long durationMs) {
  lcdCenter(2, "Stabilisasi sensor");
  unsigned long start = millis();
  while (millis() - start < durationMs) {
    lcdProgress(3, (millis() - start) * 100 / durationMs);
    delay(50);
  }
  lcdProgress(3, 100);
}

// format berat ke teks rata kanan 12 karakter; >= 1000 g otomatis ditampilkan dalam kg
void formatWeight(float g, char *out, size_t size) {
  if (fabs(g) >= 1000) snprintf(out, size, "%9.2f kg", g / 1000.0);
  else                 snprintf(out, size, "%9.1f g ", g);
}

void lcdShowWeights(float kiri, float kanan) {
  char val[16], line[21];
  float rata = (kiri + kanan) / 2.0;

  formatWeight(kiri, val, sizeof(val));
  snprintf(line, sizeof(line), "Kiri :%14s", val);
  lcdLine(1, line);

  formatWeight(kanan, val, sizeof(val));
  snprintf(line, sizeof(line), "Kanan:%14s", val);
  lcdLine(2, line);

  formatWeight(rata, val, sizeof(val));
  snprintf(line, sizeof(line), "Rata2:%14s", val);
  lcdLine(3, line);
}

// ===================================================

// ======================= Kalibrasi tersimpan =======================

// pakai nilai tersimpan kalau pernah dikalibrasi, kalau belum tetap nilai awal di atas
void loadFactors() {
  prefs.begin("pushmeter", true);
  if (prefs.isKey("cal1")) calFactor1 = prefs.getFloat("cal1", calFactor1);
  if (prefs.isKey("cal2")) calFactor2 = prefs.getFloat("cal2", calFactor2);
  prefs.end();
  if (calFactor1 == 0) calFactor1 = 1;
  if (calFactor2 == 0) calFactor2 = 1;
}

void saveFactors() {
  prefs.begin("pushmeter", false);
  prefs.putFloat("cal1", calFactor1);
  prefs.putFloat("cal2", calFactor2);
  prefs.end();
}

// ======================= Bluetooth LE =======================

// Tetap menyiarkan sinyal setelah ada yang tersambung: Windows sering menahan satu sambungan ke alat
// yang pernah di-pair lewat Pengaturan, dan tanpa ini browser tidak bisa menemukan alat lagi.
// Dengan begitu HP dan laptop juga bisa tersambung bersamaan.
class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleConnected = true;
    BLEDevice::startAdvertising();
  }
  void onDisconnect(BLEServer *server) override {
    bleConnected = server->getConnectedCount() > 0;
    BLEDevice::startAdvertising();
  }
};

class CmdCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *ch) override {
    String v = ch->getValue();
    v.trim();
    if (v.length() == 0 || bleCmdPending) return;
    bleCmd = v;
    bleCmdPending = true;
  }
};

void bleBegin() {
  BLEDevice::init(BLE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCb());
  BLEService *svc = bleServer->createService(BLE_SERVICE_UUID);
  bleData = svc->createCharacteristic(BLE_DATA_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleStatus = svc->createCharacteristic(BLE_STATUS_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleXfer = svc->createCharacteristic(BLE_XFER_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleData->addDescriptor(new BLE2902());
  bleStatus->addDescriptor(new BLE2902());
  bleXfer->addDescriptor(new BLE2902());
  BLECharacteristic *cmd = svc->createCharacteristic(
      BLE_CMD_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  cmd->setCallbacks(new CmdCb());
  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("Bluetooth LE aktif: " BLE_NAME);
}

// ======================= Record + penyimpanan sesi =======================

bool recording = false;
unsigned long recStartMs = 0;
uint32_t recId = 0, recTs = 0;
uint16_t recSamples[REC_MAX_SECONDS];
uint16_t recCount = 0;
float recTickSum = 0;  int recTickN = 0;      // rata-rata nilai dalam detik yang sedang berjalan
double recAllSum = 0;  long recAllN = 0;      // untuk rata-rata seluruh sesi
float recPeak = 0;                            // gram
float recPeakL = 0, recPeakR = 0;             // gram, puncak per sisi

uint16_t toCentiKg(float grams) {
  float v = grams / 10.0f + 0.5f;             // gram -> 0,01 kg
  if (v < 0) v = 0;
  if (v > 65535) v = 65535;
  return (uint16_t)v;
}

String recPath(uint32_t id) { return "/r" + String(id) + ".rec"; }

// daftar id sesi tersimpan, urut naik. Mengembalikan jumlahnya.
int listRecords(uint32_t *ids, int maxN) {
  int n = 0;
  File root = LittleFS.open("/");
  if (!root) return 0;
  for (File f = root.openNextFile(); f && n < maxN; f = root.openNextFile()) {
    const char *nm = f.name();
    const char *base = strrchr(nm, '/');
    base = base ? base + 1 : nm;
    unsigned long id;
    if (sscanf(base, "r%lu.rec", &id) == 1) ids[n++] = (uint32_t)id;
    f.close();
  }
  root.close();
  for (int i = 1; i < n; i++) {               // urutkan (insertion sort)
    uint32_t v = ids[i];
    int j = i - 1;
    while (j >= 0 && ids[j] > v) { ids[j + 1] = ids[j]; j--; }
    ids[j + 1] = v;
  }
  return n;
}

bool readRecHeader(uint32_t id, RecHeader &h) {
  File f = LittleFS.open(recPath(id), "r");
  if (!f) return false;
  bool ok = f.read((uint8_t *)&h, sizeof(h)) == sizeof(h) &&
            f.size() == sizeof(h) + (size_t)h.count * 2;       // ukuran tak cocok = format lama / rusak
  f.close();
  if (!ok) {
    LittleFS.remove(recPath(id));
    Serial.print("Sesi #"); Serial.print(id); Serial.println(" format lama/rusak, dihapus.");
  }
  return ok;
}

uint32_t nextSessionId() {
  prefs.begin("pushmeter", false);
  uint32_t id = prefs.getUInt("nextId", 1);
  prefs.putUInt("nextId", id + 1);
  prefs.end();
  return id;
}

// kirim satu pesan berbingkai lewat XFER, dipotong per 20 byte (aman untuk MTU default)
void xferSend(uint8_t type, const uint8_t *payload, uint16_t len) {
  if (!bleConnected) return;
  uint8_t chunk[20];
  int fill = 0;
  uint8_t sum = 0;
  uint32_t total = 3 + (uint32_t)len + 1;
  for (uint32_t i = 0; i < total; i++) {
    uint8_t b;
    if (i == 0)            b = type;
    else if (i == 1)       b = len & 0xFF;
    else if (i == 2)       b = len >> 8;
    else if (i < 3u + len) { b = payload[i - 3]; sum += b; }
    else                   b = sum;
    chunk[fill++] = b;
    if (fill == 20 || i == total - 1) {
      bleXfer->setValue(chunk, fill);
      bleXfer->notify();
      fill = 0;
      delay(20);                              // beri waktu stack BLE mengosongkan antrean
    }
  }
}

void sendHistory() {
  static uint32_t ids[REC_MAX_SESSIONS + 5];
  int n = listRecords(ids, REC_MAX_SESSIONS + 5);
  uint16_t sent = 0;
  for (int i = 0; i < n; i++) {
    RecHeader h;
    if (!readRecHeader(ids[i], h)) continue;
    xferSend(1, (uint8_t *)&h, sizeof(h));
    sent++;
  }
  xferSend(2, (uint8_t *)&sent, sizeof(sent));
}

bool sendRecord(uint32_t id) {
  File f = LittleFS.open(recPath(id), "r");
  if (!f) return false;
  RecHeader h;
  if (f.read((uint8_t *)&h, sizeof(h)) != sizeof(h)) { f.close(); return false; }
  static uint8_t buf[REC_MAX_SECONDS * 2];
  int bytes = f.read(buf, h.count * 2 <= (int)sizeof(buf) ? h.count * 2 : sizeof(buf));
  f.close();
  xferSend(3, (uint8_t *)&h, sizeof(h));
  xferSend(4, buf, bytes > 0 ? bytes : 0);
  return true;
}

void printHistory() {
  static uint32_t ids[REC_MAX_SESSIONS + 5];
  int n = listRecords(ids, REC_MAX_SESSIONS + 5);
  Serial.print("Riwayat tersimpan: "); Serial.print(n); Serial.println(" sesi");
  for (int i = 0; i < n; i++) {
    RecHeader h;
    if (!readRecHeader(ids[i], h)) continue;
    Serial.print("  #"); Serial.print(h.id);
    Serial.print(" ts="); Serial.print(h.ts);
    Serial.print(" dur="); Serial.print(h.dur);
    Serial.print("s sampel="); Serial.print(h.count);
    Serial.print(" peak="); Serial.print(h.peak / 100.0f, 2);
    Serial.print("kg avg="); Serial.print(h.avg / 100.0f, 2);
    Serial.print("kg kiri="); Serial.print(h.peakL / 100.0f, 2);
    Serial.print("kg kanan="); Serial.print(h.peakR / 100.0f, 2); Serial.println("kg");
  }
}

void pruneOldRecords() {
  static uint32_t ids[REC_MAX_SESSIONS + 5];
  int n = listRecords(ids, REC_MAX_SESSIONS + 5);
  for (int i = 0; i < n - REC_MAX_SESSIONS; i++) LittleFS.remove(recPath(ids[i]));
}

void recPush(float grams) {
  if (recCount < REC_MAX_SECONDS) recSamples[recCount++] = toCentiKg(grams);
}

uint32_t recordStart(uint32_t unixTs) {
  if (recording) return 0;
  recId = nextSessionId();
  recTs = unixTs;
  recCount = 0;
  recTickSum = 0; recTickN = 0;
  recAllSum = 0;  recAllN = 0;
  recPeak = 0;
  recPeakL = recPeakR = 0;
  recStartMs = millis();
  az1.count = az2.count = 0;
  recording = true;
  Serial.print("Record mulai, sesi #"); Serial.println(recId);
  return recId;
}

// dipanggil tiap pembacaan (500 ms) selama Record; kiri/kanan dalam gram
void recordTick(float leftGrams, float rightGrams) {
  float totalGrams = leftGrams + rightGrams;
  uint32_t sec = (millis() - recStartMs) / 1000;
  if (sec > recCount && recTickN > 0) {       // detik sebelumnya selesai -> catat rata-ratanya
    float v = recTickSum / recTickN;
    recPush(v);
    while (recCount < sec && recCount < REC_MAX_SECONDS) recPush(v);   // kalau sempat terhenti > 1 detik
    recTickSum = 0; recTickN = 0;
  }
  recTickSum += totalGrams; recTickN++;
  recAllSum += totalGrams;  recAllN++;
  if (totalGrams > recPeak) recPeak = totalGrams;
  if (leftGrams > recPeakL) recPeakL = leftGrams;
  if (rightGrams > recPeakR) recPeakR = rightGrams;
}

// akhiri Record dan simpan ke flash. Mengembalikan id sesi, atau 0 jika terlalu singkat / gagal.
uint32_t recordStop(RecHeader &h) {
  if (!recording) return 0;
  recording = false;
  if (recTickN > 0) recPush(recTickSum / recTickN);           // sisa detik terakhir
  if (recCount == 0 || recAllN == 0) return 0;

  unsigned long el = millis() - recStartMs;
  h.id = recId;
  h.ts = recTs;
  h.dur = (uint16_t)((el + 500) / 1000 > 65535UL ? 65535UL : (el + 500) / 1000);
  if (h.dur < 1) h.dur = 1;
  h.count = recCount;
  h.peak = toCentiKg(recPeak);
  h.avg = toCentiKg((float)(recAllSum / recAllN));
  h.peakL = toCentiKg(recPeakL);
  h.peakR = toCentiKg(recPeakR);

  File f = LittleFS.open(recPath(recId), "w");
  if (!f) { Serial.println("Gagal membuka file rekaman."); return 0; }
  f.write((uint8_t *)&h, sizeof(h));
  f.write((uint8_t *)recSamples, recCount * 2);
  f.close();
  pruneOldRecords();
  Serial.print("Record selesai, sesi #"); Serial.print(h.id);
  Serial.print(" dur="); Serial.print(h.dur);
  Serial.print("s peak="); Serial.print(h.peak / 100.0f, 2);
  Serial.print("kg avg="); Serial.print(h.avg / 100.0f, 2);
  Serial.print("kg kiri="); Serial.print(h.peakL / 100.0f, 2);
  Serial.print("kg kanan="); Serial.print(h.peakR / 100.0f, 2); Serial.println("kg");
  return h.id;
}

void bleSendReading(float w1, float w2) {
  if (!bleConnected) return;
  uint8_t buf[11];
  memcpy(buf, &w1, 4);
  memcpy(buf + 4, &w2, 4);
  buf[8] = recording ? 1 : 0;
  uint16_t el = recording ? (uint16_t)((millis() - recStartMs) / 1000) : 0;
  memcpy(buf + 9, &el, 2);
  bleData->setValue(buf, sizeof(buf));
  bleData->notify();
}

void bleSendStatus(const String &msg) {
  Serial.println("BLE status: " + msg);
  if (!bleConnected) return;
  bleStatus->setValue(msg.c_str());
  bleStatus->notify();
}

// tare hanya jika HX711 merespons, supaya program tidak macet
bool safeTare(HX711 &scale, int no) {
  if (scale.wait_ready_timeout(1000)) {
    for (int i = 0; i < 10; i++) scale.read();       // buang bacaan awal yang belum stabil
    scale.tare(50);
    return true;
  }
  Serial.print("HX711 #"); Serial.print(no); Serial.println(" tidak merespons, tare dilewati.");
  return false;
}

void tareBoth() {
  lcdCenter(2, "Tare... jangan");
  lcdCenter(3, "disentuh!");
  bool ok1 = safeTare(scale1, 1);
  bool ok2 = safeTare(scale2, 2);
  az1.count = az2.count = 0;
  if (!ok1 || !ok2) {
    lcdCenter(2, "HX711 ERROR:");
    lcdCenter(3, !ok1 && !ok2 ? "Kiri & Kanan" : (!ok1 ? "Kiri (LC1)" : "Kanan (LC2)"));
    delay(2000);
  }
}

void printFactors() {
  Serial.println("----------------------------------");
  Serial.print("calFactor1 = "); Serial.println(calFactor1, 4);
  Serial.print("calFactor2 = "); Serial.println(calFactor2, 4);
  Serial.println("Salin nilai ini ke program utama: scale.set_scale(calFactor);");
  Serial.println("----------------------------------");
}
// (kalibrasi Serial di bawah memakai alur Satria_Push_Meter_Ino; calibrateWith() tetap ada untuk perintah Bluetooth)

float readKnownWeight() {
  Serial.println("Masukkan berat beban yang diketahui (gram), lalu Enter:");
  while (Serial.available()) Serial.read();          // bersihkan buffer
  while (!Serial.available()) delay(10);
  float w = Serial.parseFloat();
  while (Serial.available()) Serial.read();
  return w;
}

// inti kalibrasi: beban "known" gram sudah di atas load cell "no". true jika berhasil.
bool calibrateWith(HX711 &scale, float &calFactor, int no, float known) {
  if (known <= 0) {
    Serial.println("Berat tidak valid, dibatalkan.");
    return false;
  }
  if (!scale.wait_ready_timeout(1000)) {
    Serial.println("HX711 tidak terdeteksi! Cek wiring.");
    return false;
  }
  long raw = scale.get_value(20);                   // rata-rata 20 bacaan (sudah dikurangi offset tare)
  float factor = raw / known;
  if (factor == 0) {
    Serial.println("Bacaan 0, beban belum terpasang? Dibatalkan.");
    return false;
  }
  calFactor = factor;
  scale.set_scale(calFactor);
  saveFactors();
  az1.count = az2.count = 0;
  Serial.print("Raw = "); Serial.print(raw);
  Serial.print("  -> calFactor"); Serial.print(no); Serial.print(" = "); Serial.println(calFactor, 4);
  return true;
}

void calibrate(HX711 &scale, float &calFactor, int no) {
  Serial.print("\n=== Kalibrasi Load Cell "); Serial.print(no); Serial.println(" ===");
  Serial.println("Pastikan sudah di-tare (kosong), lalu letakkan beban yang diketahui.");
  lcdCenter(1, "== KALIBRASI ==");
  lcdCenter(2, no == 1 ? "Load Cell KIRI" : "Load Cell KANAN");
  lcdCenter(3, "Isi berat di Serial");
  float known = readKnownWeight();
  if (known <= 0) {
    Serial.println("Berat tidak valid, dibatalkan.");
    return;
  }
  if (!scale.wait_ready_timeout(1000)) {
    Serial.println("HX711 tidak terdeteksi! Cek wiring.");
    return;
  }
  long raw = scale.get_value(20);                   // rata-rata 20 bacaan (sudah dikurangi offset tare)
  calFactor = raw / known;
  if (calFactor == 0) calFactor = 1;                // hindari pembagian nol
  scale.set_scale(calFactor);
  saveFactors();                                    // simpan ke flash (sama seperti kalibrasi dari web)
  az1.count = az2.count = 0;
  Serial.print("Raw = "); Serial.print(raw);
  Serial.print("  -> calFactor"); Serial.print(no); Serial.print(" = "); Serial.println(calFactor, 4);
}

// akhiri Record (dari web, Serial, atau batas waktu), simpan, dan kabari web
void finishRecord() {
  RecHeader h;
  uint32_t id = recordStop(h);
  if (id == 0) {
    bleSendStatus("ERR:STOP:singkat");
    return;
  }
  char msg[24];
  snprintf(msg, sizeof(msg), "OK:STOP:%lu", (unsigned long)id);
  bleSendStatus(msg);
  xferSend(5, (uint8_t *)&h, sizeof(h));
}

// jalankan perintah dari web (Bluetooth)
void handleBleCommand() {
  if (!bleCmdPending) return;
  String cmd = bleCmd;
  bleCmdPending = false;
  Serial.println("BLE perintah: " + cmd);

  if (cmd == "T" || cmd.startsWith("C1:") || cmd.startsWith("C2:")) {
    if (recording) { bleSendStatus("ERR:sedang-record"); return; }   // tare/kalibrasi merusak rekaman
  }

  if (cmd == "T") {
    tareBoth();
    bleSendStatus("OK:T");
  } else if (cmd.startsWith("C1:") || cmd.startsWith("C2:")) {
    int no = cmd.charAt(1) - '0';
    float known = cmd.substring(3).toFloat();
    lcdCenter(2, "Kalibrasi...");
    lcdCenter(3, no == 1 ? "Load Cell KIRI" : "Load Cell KANAN");
    bool ok = no == 1 ? calibrateWith(scale1, calFactor1, 1, known)
                      : calibrateWith(scale2, calFactor2, 2, known);
    char msg[32];
    if (ok) snprintf(msg, sizeof(msg), "OK:C%d:%.2f", no, no == 1 ? calFactor1 : calFactor2);
    else    snprintf(msg, sizeof(msg), "ERR:C%d", no);
    bleSendStatus(msg);
  } else if (cmd.startsWith("START_RECORD")) {
    uint32_t ts = cmd.length() > 13 ? (uint32_t)strtoul(cmd.substring(13).c_str(), nullptr, 10) : 0;
    uint32_t id = recordStart(ts);
    char msg[24];
    if (id) snprintf(msg, sizeof(msg), "OK:START:%lu", (unsigned long)id);
    else    snprintf(msg, sizeof(msg), "ERR:START:sibuk");
    bleSendStatus(msg);
  } else if (cmd == "STOP_RECORD") {
    if (!recording) bleSendStatus("ERR:STOP:diam");
    else finishRecord();
  } else if (cmd == "GET_HISTORY") {
    sendHistory();
  } else if (cmd.startsWith("GET_RECORD:")) {
    if (!sendRecord((uint32_t)strtoul(cmd.substring(11).c_str(), nullptr, 10))) bleSendStatus("ERR:GET:tidak-ada");
  } else if (cmd.startsWith("DELETE_RECORD:")) {
    uint32_t id = (uint32_t)strtoul(cmd.substring(14).c_str(), nullptr, 10);
    bool ok = LittleFS.remove(recPath(id));
    bleSendStatus(ok ? "OK:DEL" : "ERR:DEL");
  } else if (cmd == "CLEAR_HISTORY") {
    static uint32_t ids[REC_MAX_SESSIONS + 5];
    int n = listRecords(ids, REC_MAX_SESSIONS + 5);
    for (int i = 0; i < n; i++) LittleFS.remove(recPath(ids[i]));
    bleSendStatus("OK:CLEAR");
  } else {
    bleSendStatus("ERR:perintah");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nPush Meter - 2 Load Cell HX711 + Bluetooth");

  loadFactors();
  lcdDetect();
  if (lcdOk) {
    lcd->init();
    lcd->backlight();
  }
  lcdLoadBarChars();
  introAnimation();

  scale1.begin(LC1_DOUT, LC1_SCK);
  scale2.begin(LC2_DOUT, LC2_SCK);
  scale1.set_scale(calFactor1);
  scale2.set_scale(calFactor2);

  Serial.println("Kosongkan timbangan... tare dalam 5 detik (menunggu HX711 stabil)");
  stabilizeWithProgress(5000);
  tareBoth();
  Serial.println("Tare selesai. Ketik '1' atau '2' untuk kalibrasi.");

  if (!LittleFS.begin(true)) Serial.println("LittleFS gagal dimulai: sesi tidak bisa disimpan.");
  bleBegin();

  lcdLine(2, "");
  lcdCenter(3, "*** SIAP ***");
  delay(1000);
  if (lcdOk) lcd->clear();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 't': tareBoth(); Serial.println("Tare OK"); break;
      case '1': calibrate(scale1, calFactor1, 1); break;
      case '2': calibrate(scale2, calFactor2, 2); break;
      case '+': calFactor1 += 1;  break;
      case '-': calFactor1 -= 1;  break;
      case 'a': calFactor1 += 10; break;
      case 'z': calFactor1 -= 10; break;
      case ']': calFactor2 += 1;  break;
      case '[': calFactor2 -= 1;  break;
      case 's': calFactor2 += 10; break;
      case 'x': calFactor2 -= 10; break;
      case 'p': printFactors(); break;
      case 'r':
        if (recording) finishRecord();
        else recordStart(0);
        break;
      case 'h': printHistory(); break;
    }
    if (calFactor1 == 0) calFactor1 = 1;   // hindari pembagian nol
    if (calFactor2 == 0) calFactor2 = 1;
    scale1.set_scale(calFactor1);
    scale2.set_scale(calFactor2);
    if (c != '\0' && strchr("+-azs]x[", c)) saveFactors();   // penyesuaian manual ikut disimpan
  }
  handleBleCommand();

  static unsigned long last = 0;
  if (millis() - last >= 500) {
    last = millis();
    // raw = bacaan mentah HX711; harus berubah-ubah & berubah besar saat diberi beban
    // tunggu HX711 siap (bukan cek sesaat), kalau tetap tidak siap pakai bacaan terakhir
    static long raw1 = 0, raw2 = 0;
    bool ok1 = scale1.wait_ready_timeout(300);
    if (ok1) raw1 = (long)scale1.read_average(5);
    bool ok2 = scale2.wait_ready_timeout(300);
    if (ok2) raw2 = (long)scale2.read_average(5);
    if (!recording) {                            // jangan auto-zero saat Record: tekanan ditahan bukan "kosong"
      if (ok1) autoZero(scale1, az1, raw1, 1);
      if (ok2) autoZero(scale2, az2, raw2, 2);
    }
    long net1 = raw1 - scale1.get_offset();
    long net2 = raw2 - scale2.get_offset();
    if (labs(net1) < ZERO_BAND) net1 = 0;
    if (labs(net2) < ZERO_BAND) net2 = 0;
    float w1 = net1 / scale1.get_scale();
    float w2 = net2 / scale2.get_scale();
    if (w1 < 0) w1 = 0;                          // saat load cell diangkat/dilepas, jangan tampil negatif
    if (w2 < 0) w2 = 0;

    if (recording) {
      recordTick(w1, w2);
      if (recCount >= REC_MAX_SECONDS) finishRecord();           // batas waktu tercapai
    }
    lcdHeader(bleConnected, recording, recording ? (millis() - recStartMs) / 1000 : 0);
    lcdShowWeights(w1, w2);

    Serial.print("KIRI: "); Serial.print(w1, 2); Serial.print(" g (raw=");
    Serial.print(raw1); Serial.print(", f="); Serial.print(calFactor1, 2);
    Serial.print(")   |   KANAN: "); Serial.print(w2, 2); Serial.print(" g (raw=");
    Serial.print(raw2); Serial.print(", f="); Serial.print(calFactor2, 2);
    Serial.print(")   |   RATA2: "); Serial.print((w1 + w2) / 2.0, 2); Serial.println(" g");

    bleSendReading(w1, w2);
  }
}
