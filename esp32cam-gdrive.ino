// ESP32-CAM -> Google Drive on a timer.
// Wakes up every UPLOAD_INTERVAL_SEC seconds, takes a photo, uploads it, goes back to deep sleep.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "mbedtls/base64.h"
#include "esp_camera.h"
#include "img_converters.h"
#include <time.h>

#include "secrets.h"   // copy secrets.h.example -> secrets.h and edit

// ---------- Settings ----------
#define UPLOAD_INTERVAL_SEC 300      // time between photos (5 min)
#define WIFI_TIMEOUT_MS     20000
#define RESPONSE_TIMEOUT_MS 30000    // wait for Google's reply
#define FRAME_SIZE          FRAMESIZE_VGA  // UXGA|SXGA|XGA|SVGA|VGA|CIF|QVGA...
#define JPEG_QUALITY        80       // 0-100, higher = better quality/bigger file
#define STAMP_SWAP_BYTES    1        // fixes garbled colors from RGB565 byte order; set 0 if colors look worse
#define TIMEZONE            "EST5EDT,M3.2.0,M11.1.0"  // US Eastern; other zones: https://github.com/nayarsystems/posix_tz_db

// ---------- Pick ONE board (see pin tables below) ----------
//#define CAMERA_MODEL_AI_THINKER        // ESP32-CAM (the classic one on GitHub)
//#define CAMERA_MODEL_WROVER_KIT      // Freenove ESP32-WROVER, ESP-WROVER-KIT
#define CAMERA_MODEL_ESP32S3_EYE     // ESP32-S3-EYE, Freenove ESP32-S3-WROOM
//#define CAMERA_MODEL_XIAO_ESP32S3    // Seeed XIAO ESP32S3 Sense

#if defined(CAMERA_MODEL_AI_THINKER)
  #define PWDN_GPIO_NUM  32
  #define RESET_GPIO_NUM -1
  #define XCLK_GPIO_NUM   0
  #define SIOD_GPIO_NUM  26
  #define SIOC_GPIO_NUM  27
  #define Y9_GPIO_NUM    35
  #define Y8_GPIO_NUM    34
  #define Y7_GPIO_NUM    39
  #define Y6_GPIO_NUM    36
  #define Y5_GPIO_NUM    21
  #define Y4_GPIO_NUM    19
  #define Y3_GPIO_NUM    18
  #define Y2_GPIO_NUM     5
  #define VSYNC_GPIO_NUM 25
  #define HREF_GPIO_NUM  23
  #define PCLK_GPIO_NUM  22
#elif defined(CAMERA_MODEL_WROVER_KIT)
  #define PWDN_GPIO_NUM  -1
  #define RESET_GPIO_NUM -1
  #define XCLK_GPIO_NUM  21
  #define SIOD_GPIO_NUM  26
  #define SIOC_GPIO_NUM  27
  #define Y9_GPIO_NUM    35
  #define Y8_GPIO_NUM    34
  #define Y7_GPIO_NUM    39
  #define Y6_GPIO_NUM    36
  #define Y5_GPIO_NUM    19
  #define Y4_GPIO_NUM    18
  #define Y3_GPIO_NUM     5
  #define Y2_GPIO_NUM     4
  #define VSYNC_GPIO_NUM 25
  #define HREF_GPIO_NUM  23
  #define PCLK_GPIO_NUM  22
#elif defined(CAMERA_MODEL_ESP32S3_EYE)
  #define PWDN_GPIO_NUM  -1
  #define RESET_GPIO_NUM -1
  #define XCLK_GPIO_NUM  15
  #define SIOD_GPIO_NUM   4
  #define SIOC_GPIO_NUM   5
  #define Y9_GPIO_NUM    16
  #define Y8_GPIO_NUM    17
  #define Y7_GPIO_NUM    18
  #define Y6_GPIO_NUM    12
  #define Y5_GPIO_NUM    10
  #define Y4_GPIO_NUM     8
  #define Y3_GPIO_NUM     9
  #define Y2_GPIO_NUM    11
  #define VSYNC_GPIO_NUM  6
  #define HREF_GPIO_NUM   7
  #define PCLK_GPIO_NUM  13
#elif defined(CAMERA_MODEL_XIAO_ESP32S3)
  #define PWDN_GPIO_NUM  -1
  #define RESET_GPIO_NUM -1
  #define XCLK_GPIO_NUM  10
  #define SIOD_GPIO_NUM  40
  #define SIOC_GPIO_NUM  39
  #define Y9_GPIO_NUM    48
  #define Y8_GPIO_NUM    11
  #define Y7_GPIO_NUM    12
  #define Y6_GPIO_NUM    14
  #define Y5_GPIO_NUM    16
  #define Y4_GPIO_NUM    18
  #define Y3_GPIO_NUM    17
  #define Y2_GPIO_NUM    15
  #define VSYNC_GPIO_NUM 38
  #define HREF_GPIO_NUM  47
  #define PCLK_GPIO_NUM  13
#else
  #error "Select a camera model above"
#endif

const char* myDomain = "script.google.com";

void goToSleep() {
  Serial.printf("Sleeping for %d s\n", UPLOAD_INTERVAL_SEC);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)UPLOAD_INTERVAL_SEC * 1000000ULL);
  esp_deep_sleep_start();
}

bool connectWifi() {
  Serial.println("WiFi: starting radio");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("\nDisconnect reason: %d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // Diagnostic: list visible networks so you can see if your SSID is in range (2.4 GHz)
  int n = WiFi.scanNetworks();
  Serial.printf("Scan found %d networks:\n", n);
  for (int i = 0; i < n; i++) {
    Serial.printf("  %s  RSSI %d  ch %d  enc %d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i));
  }

  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to " WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.printf("\nWiFi failed, status %d\n", WiFi.status());
      return false;
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println(WiFi.localIP());
  return true;
}

bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  // Timestamp needs a raw RGB565 frame (~600 KB), which only fits in PSRAM.
  // No PSRAM (wrong Tools > PSRAM setting) -> fall back to plain JPEG, no stamp.
  Serial.println(psramFound() ? "PSRAM found" : "No PSRAM: check Tools > PSRAM = OPI PSRAM; photos will have no stamp");
  config.pixel_format = psramFound() ? PIXFORMAT_RGB565 : PIXFORMAT_JPEG;
  config.frame_size = FRAME_SIZE;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }
  return true;
}

// Base64-encode `len` bytes, then URL-encode the result (+ / = become %XX).
// Writes into out (must hold 3*(4*ceil(len/3)) bytes). Returns encoded length.
static size_t b64UrlEncode(const uint8_t* in, size_t len, char* out) {
  unsigned char b64[4 * 64 + 4];
  size_t n = 0;
  mbedtls_base64_encode(b64, sizeof(b64), &n, in, len);
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    char c = b64[i];
    if (c == '+')      { out[o++] = '%'; out[o++] = '2'; out[o++] = 'B'; }
    else if (c == '/') { out[o++] = '%'; out[o++] = '2'; out[o++] = 'F'; }
    else if (c == '=') { out[o++] = '%'; out[o++] = '3'; out[o++] = 'D'; }
    else out[o++] = c;
  }
  return o;
}

// Encode in 192-byte chunks (a multiple of 3, so no padding mid-stream).
// If client is NULL, only measures the total length.
static size_t streamImage(const uint8_t* buf, size_t len, WiFiClientSecure* client) {
  const size_t CHUNK = 192;
  char out[3 * 4 * 64 + 4];
  size_t total = 0;
  for (size_t i = 0; i < len; i += CHUNK) {
    size_t n = min(CHUNK, len - i);
    size_t o = b64UrlEncode(buf + i, n, out);
    if (client) client->write((const uint8_t*)out, o);
    total += o;
  }
  return total;
}

// 5x7 font (column-major, bit0 = top row) for the characters in STAMP_CHARS.
static const char STAMP_CHARS[] = "0123456789-: ";
static const uint8_t STAMP_FONT[13][5] = {
  {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
  {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
  {0x06,0x49,0x49,0x29,0x1E}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x36,0x36,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00}
};

// Draw white text on a black box, bottom-left of an RGB565 frame (black/white are byte-order safe).
static void drawStamp(uint16_t* px, int w, int h, const char* text) {
  const int S = 2;   // pixel scale
  const int len = strlen(text);
  const int boxW = len * 6 * S + 6, boxH = 7 * S + 6;
  const int x0 = 4, y0 = h - boxH - 4;
  for (int y = 0; y < boxH; y++)
    for (int x = 0; x < boxW; x++)
      px[(y0 + y) * w + x0 + x] = 0x0000;
  for (int c = 0; c < len; c++) {
    const char* p = strchr(STAMP_CHARS, text[c]);
    if (!p) continue;
    const uint8_t* g = STAMP_FONT[p - STAMP_CHARS];
    for (int col = 0; col < 5; col++)
      for (int row = 0; row < 7; row++)
        if ((g[col] >> row) & 1)
          for (int dy = 0; dy < S; dy++)
            for (int dx = 0; dx < S; dx++)
              px[(y0 + 3 + row * S + dy) * w + x0 + 3 + (c * 6 + col) * S + dx] = 0xFFFF;
  }
}

void syncTime() {
  configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  Serial.println(getLocalTime(&t, 8000) ? "Time synced" : "Time sync failed (photo will have no stamp)");
}

bool uploadPhoto() {
  // First frames after power-up are often dark/green; throw a few away.
  for (int i = 0; i < 3; i++) {
    camera_fb_t* tmp = esp_camera_fb_get();
    if (tmp) esp_camera_fb_return(tmp);
    delay(100);
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return false;
  }

  uint8_t* jpg = nullptr;
  size_t jpgLen = 0;
  bool encoded;
  if (fb->format == PIXFORMAT_JPEG) {   // no-PSRAM fallback: already a JPEG, copy it
    jpg = (uint8_t*)malloc(fb->len);
    encoded = jpg != nullptr;
    if (encoded) { memcpy(jpg, fb->buf, fb->len); jpgLen = fb->len; }
  } else {
    struct tm t;
    if (getLocalTime(&t, 0)) {
      char ts[24];
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);
      drawStamp((uint16_t*)fb->buf, fb->width, fb->height, ts);
    }
#if STAMP_SWAP_BYTES
    uint16_t* px = (uint16_t*)fb->buf;
    for (size_t i = 0; i < fb->len / 2; i++) px[i] = (px[i] >> 8) | (px[i] << 8);
#endif
    encoded = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, fb->format, JPEG_QUALITY, &jpg, &jpgLen);
  }
  esp_camera_fb_return(fb);
  if (!encoded) {
    Serial.println("JPEG encode failed");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();   // skip certificate check (simple; fine for a hobby project)
  Serial.println("Connecting to " + String(myDomain));
  if (!client.connect(myDomain, 443)) {
    Serial.println("Connection failed");
    free(jpg);
    return false;
  }

  String head = "filename=ESP32-CAM.jpg&mimetype=image/jpeg&data=";
  size_t bodyLen = head.length() + streamImage(jpg, jpgLen, nullptr);

  client.print("POST /macros/s/" GOOGLE_SCRIPT_ID "/exec HTTP/1.1\r\n");
  client.print("Host: " + String(myDomain) + "\r\n");
  client.print("Content-Length: " + String(bodyLen) + "\r\n");
  client.print("Content-Type: application/x-www-form-urlencoded\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(head);
  streamImage(jpg, jpgLen, &client);
  free(jpg);

  Serial.println("Waiting for response");
  unsigned long start = millis();
  while (!client.available() && client.connected()) {
    if (millis() - start > RESPONSE_TIMEOUT_MS) {
      Serial.println("No response");
      client.stop();
      return false;
    }
    delay(100);
  }
  // Google answers a successful POST with a 302 redirect; that still means the file was saved.
  String status = client.readStringUntil('\n');
  Serial.println(status);
  client.stop();
  return status.indexOf("302") > 0 || status.indexOf("200") > 0;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Boot");

  // Camera first, then WiFi: the serial markers show which step triggers the reset.
  if (initCamera()) Serial.println("Camera OK");
  else { goToSleep(); }
  if (connectWifi()) {
    syncTime();
    Serial.println(uploadPhoto() ? "Upload OK" : "Upload failed");
    esp_camera_deinit();
  }
  goToSleep();
}

void loop() {}  // never reached: setup() ends in deep sleep
