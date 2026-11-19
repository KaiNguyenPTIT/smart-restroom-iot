#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <esp_timer.h>

// ================= THINGSBOARD =================

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

const char* TB_SERVER = "mqtt.thingsboard.cloud";
const int   TB_PORT = 1883;

// Token ThingsBoard KHÔNG hardcode trong repo.
// Truyền lúc build:  pio run -t upload --project-option 'build_flags=-DTB_TOKEN="<device-token>"'
// hoặc đặt biến môi trường TB_TOKEN khi chạy trên Wokwi.
#ifndef TB_TOKEN
#define TB_TOKEN "REPLACE_WITH_THINGSBOARD_DEVICE_TOKEN"
#endif
const char* TOKEN = TB_TOKEN;

// ================= PIN CẢM BIẾN =================

// HC-SR04 quét khu vực trong phòng / hướng cửa
#define TRIG_1 32
#define ECHO_1 33

// Cảm biến tiệm cận lavabo: LOW = đang phát hiện
#define PROX_PIN 25
const int PROX_ACTIVE_LEVEL = LOW;

// ================= ĐẦU RA =================

#define MIRROR_LIGHT  18 // GPIO18 -> IN2 (K2): den guong
#define FAN_PIN        5 // GPIO5 -> IN1 (K1): den tran va quat

// ================= NGƯỠNG VÀ THỜI GIAN =================

// Giữ ngưỡng 110 cm của HC-SR04 được giữ lại.
// Cần chỉnh theo số đo thực tế sau khi đổi vị trí lắp cảm biến.
#define ROOM_DISTANCE 250

// Cả U và P không phát hiện liên tục 10 giây:
// tắt đồng thời đèn trần và quạt trong chế độ AUTO.
const unsigned long EMPTY_TIME = 10000;

// P không phát hiện liên tục 3 giây: tắt đèn gương.
const unsigned long MIRROR_OFF_DELAY = 3000;

// ================= BIẾN =================

WiFiClient espClient;
PubSubClient client(espClient);

float roomDistance = 400;

// Trạng thái suy luận từ cảm biến, theo dõi ở mọi chế độ.
bool U = false;
bool P = false;
bool roomOccupied = false;
bool autoMirrorState = false;

// Trạng thái đầu ra thực tế, có thể bị chế độ thủ công ghi đè.
bool ceilingState = false;
bool mirrorState = false;
bool fanState = false;

bool roomOffTimerRunning = false;
bool mirrorOffTimerRunning = false;
unsigned long roomEmptySince = 0;
unsigned long mirrorEmptySince = 0;
unsigned long usageStartTime = 0;

unsigned long lastSensorRead = 0;
unsigned long lastSend = 0;
unsigned long lastWifiTry = 0;
unsigned long lastMqttTry = 0;

// ================= QUEUE CHUNG: TELEMETRY + ENTRY/EXIT =================
// Queue RAM: reset/mat dien se mat du lieu chua gui.
// 1800 ban ghi ~ 30 phut neu chi co telemetry 1 giay/ban ghi.
// Moi ENTRY/EXIT chiem them 1 phan tu. Day: giu cu, bo ban ghi moi va bao Serial.
const uint16_t DATA_QUEUE_CAPACITY = 1800;
const unsigned long QUEUE_SEND_INTERVAL_MS = 250; // Toi da 4 ban tin/giay.
const unsigned long QUEUE_RETRY_INTERVAL_MS = 1000;
const int64_t MIN_VALID_EPOCH_MS = 1704067200000LL; // 2024-01-01 UTC.

enum RecordKind : uint8_t { RECORD_TELEMETRY, RECORD_ENTRY, RECORD_EXIT };
enum RecordFlags : uint8_t {
  FLAG_U = 1 << 0, FLAG_P = 1 << 1, FLAG_ROOM = 1 << 2,
  FLAG_CEILING = 1 << 3, FLAG_MIRROR = 1 << 4, FLAG_FAN = 1 << 5
};

struct QueuedRecord {
  uint64_t uptimeMs;        // Thoi diem lay mau, dong ho don dieu 64-bit.
  int64_t timestampMs;      // Unix milliseconds UTC, 0 neu chua co gio NTP.
  float distance;
  uint32_t usageSeconds;
  RecordKind kind;
  uint8_t flags;
  uint8_t savedMode;        // Chup mode luc lay mau, khong doc lai luc gui.
};
static_assert(sizeof(QueuedRecord) <= 32, "Queue vuot ngan sach RAM");

QueuedRecord dataQueue[DATA_QUEUE_CAPACITY];
uint16_t dataQueueHead = 0;
uint16_t dataQueueCount = 0;
uint32_t droppedRecords = 0;
unsigned long lastQueueAttempt = 0;
unsigned long queueAttemptInterval = QUEUE_SEND_INTERVAL_MS;
bool clockReady = false;
int64_t bootEpochMs = 0; // Moc quy doi mau truoc lan dong bo gio dau tien.
unsigned long lastClockWarning = 0;

// ================= MANUAL =================

enum Mode {
  AUTO_MODE,
  MANUAL_ON,
  MANUAL_OFF
};

Mode mode = AUTO_MODE;

// =================================================
// ĐỌC HC-SR04
// =================================================

float readDistance(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  unsigned long duration = pulseIn(echo, HIGH, 25000);

  // 999 là không nhận được Echo, không phải khoảng cách thật.
  if (duration == 0)
    return 999;

  return duration / 58.0;
}

bool validDistance(float d) {
  return d >= 2 && d <= 400;
}

// =================================================
// DIEU KHIEN HAI RELAY KICH MUC LOW
// =================================================

void updateOutputs() {
  // K2: LOW = bat den guong; HIGH = tat.
  digitalWrite(MIRROR_LIGHT, mirrorState ? LOW : HIGH);

  // K1: LOW = bat dong thoi den tran va quat; HIGH = tat.
  digitalWrite(FAN_PIN, fanState ? LOW : HIGH);
}

// =================================================
// RPC TỪ THINGSBOARD: setMode -> AUTO / ON / OFF
// =================================================

void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, length))
    return;

  String topicStr = topic;
  if (!topicStr.startsWith("v1/devices/me/rpc/request/"))
    return;

  String requestId = topicStr.substring(
    strlen("v1/devices/me/rpc/request/")
  );
  String method = doc["method"].as<String>();

  if (method == "setMode") {
    String value;
    if (doc["params"].is<const char*>())
      value = doc["params"].as<String>();
    else
      value = doc["params"]["mode"].as<String>();

    if (value == "AUTO")
      mode = AUTO_MODE;
    else if (value == "ON")
      mode = MANUAL_ON;
    else if (value == "OFF")
      mode = MANUAL_OFF;

    String responseTopic = "v1/devices/me/rpc/response/" + requestId;
    client.publish(responseTopic.c_str(), "true");
  }
}

// =================================================
// WIFI
// =================================================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED)
    return;
  if (millis() - lastWifiTry < 5000)
    return;

  lastWifiTry = millis();
  Serial.println("Dang ket noi WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// =================================================
// MQTT
// =================================================

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (client.connected())
    return;
  if (millis() - lastMqttTry < 5000)
    return;

  lastMqttTry = millis();
  Serial.print("Dang ket noi ThingsBoard...");

  if (client.connect("ESP32_Restroom", TOKEN, NULL)) {
    Serial.println(" Thanh cong");
    client.subscribe("v1/devices/me/rpc/request/+");
  } else {
    Serial.print(" That bai, rc=");
    Serial.println(client.state());
  }
}

// =================================================
// THOI DIEM LAY MAU VA HANG DOI FIFO
// =================================================

uint64_t monotonicMs() {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

int64_t readEpochMs() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0)
    return 0;
  int64_t value = static_cast<int64_t>(tv.tv_sec) * 1000LL + tv.tv_usec / 1000;
  return value >= MIN_VALID_EPOCH_MS ? value : 0;
}

void updateTimeAnchor() {
  if (clockReady)
    return;
  const int64_t epochMs = readEpochMs();
  if (epochMs == 0)
    return;
  bootEpochMs = epochMs - static_cast<int64_t>(monotonicMs());
  clockReady = true;
  Serial.println("[TIME] Da co gio UTC; cho phep gui queue voi timestamp.");
}

void stampRecord(QueuedRecord& record) {
  record.uptimeMs = monotonicMs();
  record.timestampMs = readEpochMs();
}

bool enqueueRecord(const QueuedRecord& record) {
  if (dataQueueCount >= DATA_QUEUE_CAPACITY) {
    ++droppedRecords;
    if (droppedRecords == 1 || droppedRecords % 60 == 0)
      Serial.printf("[QUEUE FULL] Bo ban ghi MOI; tong da bo=%lu\n",
                    static_cast<unsigned long>(droppedRecords));
    return false;
  }
  const uint16_t tail = (dataQueueHead + dataQueueCount) % DATA_QUEUE_CAPACITY;
  dataQueue[tail] = record;
  ++dataQueueCount;
  return true;
}

const char* recordedModeText(uint8_t savedMode) {
  return savedMode == AUTO_MODE ? "AUTO"
      : savedMode == MANUAL_ON ? "MANUAL_ON" : "MANUAL_OFF";
}

bool formatRecord(const QueuedRecord& record, char* output, size_t capacity) {
  int written;
  if (record.kind == RECORD_TELEMETRY) {
    written = snprintf(output, capacity,
      "{\"ts\":%lld,\"values\":{"
      "\"roomDistance\":%.1f,"
      "\"ultrasonicDetected\":%s,"
      "\"sinkProximity\":%s,"
      "\"roomOccupied\":%s,"
      "\"ceilingLight\":%s,"
      "\"mirrorLight\":%s,"
      "\"fan\":%s,"
      "\"usageTime\":%lu,"
      "\"mode\":\"%s\"}}",
      static_cast<long long>(record.timestampMs), record.distance,
      (record.flags & FLAG_U) ? "true" : "false",
      (record.flags & FLAG_P) ? "true" : "false",
      (record.flags & FLAG_ROOM) ? "true" : "false",
      (record.flags & FLAG_CEILING) ? "true" : "false",
      (record.flags & FLAG_MIRROR) ? "true" : "false",
      (record.flags & FLAG_FAN) ? "true" : "false",
      static_cast<unsigned long>(record.usageSeconds),
      recordedModeText(record.savedMode));
  } else {
    written = snprintf(output, capacity,
      "{\"ts\":%lld,\"values\":{\"roomEvent\":\"%s\"}}",
      static_cast<long long>(record.timestampMs),
      record.kind == RECORD_ENTRY ? "ENTRY" : "EXIT");
  }
  return written > 0 && static_cast<size_t>(written) < capacity;
}

// Giu ten ham va hai cho goi ENTRY/EXIT trong logic cu.
// Moi su kien la mot ban ghi rieng, co thoi diem phat sinh rieng.
void queueRoomEvent(const char* event) {
  QueuedRecord record = {};
  if (strcmp(event, "ENTRY") == 0)
    record.kind = RECORD_ENTRY;
  else if (strcmp(event, "EXIT") == 0)
    record.kind = RECORD_EXIT;
  else
    return;
  stampRecord(record);
  if (enqueueRecord(record))
    Serial.printf("[QUEUE] %s uptime=%llu ms, pending=%u\n", event,
                  static_cast<unsigned long long>(record.uptimeMs), dataQueueCount);
}

// Chup TOAN BO gia tri hien tai vao queue, du co mang hay khong.
// Khong luu con tro den bien toan cuc, khong lay lai gia tri luc gui bu.
void queueTelemetry() {
  QueuedRecord record = {};
  record.kind = RECORD_TELEMETRY;
  stampRecord(record);
  record.distance = roomDistance;
  record.usageSeconds = roomOccupied ? (millis() - usageStartTime) / 1000 : 0;
  record.savedMode = static_cast<uint8_t>(mode);
  record.flags = (U ? FLAG_U : 0) | (P ? FLAG_P : 0)
      | (roomOccupied ? FLAG_ROOM : 0) | (ceilingState ? FLAG_CEILING : 0)
      | (mirrorState ? FLAG_MIRROR : 0) | (fanState ? FLAG_FAN : 0);
  if (enqueueRecord(record)) {
    char data[400];
    if (formatRecord(record, data, sizeof(data))) {
      Serial.printf("[QUEUE pending=%u uptime=%llu] ", dataQueueCount,
                    static_cast<unsigned long long>(record.uptimeMs));
      Serial.println(data); // ts=0 tren Serial: dang cho gio, CHUA gui len cloud.
    }
  }
}

void sendQueuedData() {
  if (WiFi.status() != WL_CONNECTED || !client.connected() || dataQueueCount == 0)
    return;
  updateTimeAnchor();
  if (!clockReady) {
    // Khong gui ts=0/1970, khong dung thoi gian nhan thay cho thoi gian lay mau.
    if (millis() - lastClockWarning >= 10000) {
      lastClockWarning = millis();
      Serial.println("[TIME] Dang cho NTP; van luu queue. Kiem tra Internet/UDP 123.");
    }
    return;
  }

  const unsigned long now = millis();
  if (now - lastQueueAttempt < queueAttemptInterval)
    return;
  lastQueueAttempt = now;

  QueuedRecord& record = dataQueue[dataQueueHead];
  if (record.timestampMs == 0) {
    // Quy doi moc uptime da luu khi khoi dong offline, trong CUNG mot lan chay.
    // Moc nay duoc giu lai ca khi publish that bai de retry cung timestamp.
    record.timestampMs = bootEpochMs + static_cast<int64_t>(record.uptimeMs);
  }

  char data[400];
  if (!formatRecord(record, data, sizeof(data))) {
    Serial.println("[QUEUE] JSON vuot buffer; giu nguyen ban ghi.");
    queueAttemptInterval = QUEUE_RETRY_INTERVAL_MS;
    return;
  }
  if (!client.publish("v1/devices/me/telemetry", data)) {
    Serial.println("[QUEUE] Gui that bai; giu ban ghi de thu lai.");
    queueAttemptInterval = QUEUE_RETRY_INTERVAL_MS;
    return;
  }

  // PubSubClient publish QoS 0: true la thanh cong ghi ra client,
  // KHONG phai xac nhan ThingsBoard da luu vao database.
  dataQueueHead = (dataQueueHead + 1) % DATA_QUEUE_CAPACITY;
  --dataQueueCount;
  queueAttemptInterval = QUEUE_SEND_INTERVAL_MS;
  Serial.printf("[SEND pending=%u] ", dataQueueCount);
  Serial.println(data);
  // Chi xu ly mot ban ghi moi lan goi; khong dung while rut het queue.
}

// =================================================
// CẬP NHẬT HAI BỘ ĐẾM ĐỘC LẬP Ở MỌI CHẾ ĐỘ
// =================================================

void updateRoomOccupancy() {
  unsigned long now = millis();

  U = validDistance(roomDistance) && roomDistance < ROOM_DISTANCE;
  P = digitalRead(PROX_PIN) == PROX_ACTIVE_LEVEL;

  // Bộ đếm 10 giây: U hoặc P có thể giữ đèn trần và quạt.
  if (U || P) {
    roomOffTimerRunning = false;
    roomEmptySince = 0;

    if (!roomOccupied) {
      roomOccupied = true;
      usageStartTime = now;
      queueRoomEvent("ENTRY");
      Serial.println("ROOM -> OCCUPIED");
    }
  } else if (roomOccupied) {
    if (!roomOffTimerRunning) {
      roomOffTimerRunning = true;
      roomEmptySince = now;
    }

    if (now - roomEmptySince >= EMPTY_TIME) {
      roomOccupied = false;
      roomOffTimerRunning = false;
      roomEmptySince = 0;
      usageStartTime = 0;
      queueRoomEvent("EXIT");
      Serial.println("ROOM -> EMPTY");
    }
  }

  // Bộ đếm 3 giây: chỉ P giữ trạng thái tự động của đèn gương.
  if (P) {
    autoMirrorState = true;
    mirrorOffTimerRunning = false;
    mirrorEmptySince = 0;
  } else if (autoMirrorState) {
    if (!mirrorOffTimerRunning) {
      mirrorOffTimerRunning = true;
      mirrorEmptySince = now;
    }

    if (now - mirrorEmptySince >= MIRROR_OFF_DELAY) {
      autoMirrorState = false;
      mirrorOffTimerRunning = false;
      mirrorEmptySince = 0;
    }
  }
}

// =================================================
// ĐIỀU KHIỂN TỰ ĐỘNG
// =================================================

void autoControl() {
  ceilingState = roomOccupied;
  fanState = roomOccupied;
  mirrorState = autoMirrorState;
  updateOutputs();
}

// =================================================
// SETUP
// =================================================

void setup() {
  // Tat ca hai relay ngay khi vao setup(), truoc khoang cho khoi dong.
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, HIGH);
  pinMode(MIRROR_LIGHT, OUTPUT);
  digitalWrite(MIRROR_LIGHT, HIGH);

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("===============================");
  Serial.println("SERIAL ESP32 HOAT DONG");
  Serial.println("===============================");

  pinMode(TRIG_1, OUTPUT);
  digitalWrite(TRIG_1, LOW);
  pinMode(ECHO_1, INPUT);
  pinMode(PROX_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  client.setServer(TB_SERVER, TB_PORT);
  client.setCallback(callback);
  client.setBufferSize(512);
  // Giảm thời gian chờ phản hồi MQTT khi máy chủ mất kết nối.
  // TCP/DNS vẫn là lời gọi đồng bộ của WiFiClient.
  client.setSocketTimeout(1);

  // NTP chay nen, khong cho trong while. Epoch UTC khong cong them 7 gio.
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  Serial.printf("[QUEUE] capacity=%u, RAM=%u bytes; reset se mat queue.\n",
                DATA_QUEUE_CAPACITY, static_cast<unsigned int>(sizeof(dataQueue)));

  Serial.println("HE THONG NHA VE SINH KHOI DONG");
}

// =================================================
// LOOP
// =================================================

void loop() {
  unsigned long now = millis();
  updateTimeAnchor();

  // Đọc HC-SR04 mỗi 60 ms; GPIO25 được đọc ở mọi vòng lặp.
  if (now - lastSensorRead >= 60) {
    lastSensorRead = now;
    roomDistance = readDistance(TRIG_1, ECHO_1);
  }

  // Theo dõi cảm biến và hai bộ đếm ở mọi chế độ.
  updateRoomOccupancy();

  if (mode == AUTO_MODE) {
    autoControl();
  } else if (mode == MANUAL_ON) {
    ceilingState = true;
    mirrorState = true;
    fanState = true;
    updateOutputs();
  } else {
    ceilingState = false;
    mirrorState = false;
    fanState = false;
    updateOutputs();
  }

  if (now - lastSend >= 1000) {
    lastSend = now;
    queueTelemetry();
  }

  connectWiFi();
  connectMQTT();
  if (client.connected())
    client.loop();
  sendQueuedData();
}
