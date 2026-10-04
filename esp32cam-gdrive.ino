// ESP32-CAM -> Google Drive on a timer.
// Wakes up every UPLOAD_INTERVAL_SEC seconds, takes a photo, uploads it, goes back to deep sleep.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "mbedtls/base64.h"
#include "esp_camera.h"

#include "secrets.h"   // copy secrets.h.example -> secrets.h and edit

// ---------- Settings ----------
#define UPLOAD_INTERVAL_SEC 300      // time between photos (5 min)
#define WIFI_TIMEOUT_MS     20000
#define RESPONSE_TIMEOUT_MS 30000    // wait for Google's reply
#define FRAME_SIZE          FRAMESIZE_VGA  // UXGA|SXGA|XGA|SVGA|VGA|CIF|QVGA...
#define JPEG_QUALITY        10       // 0-63, lower = better quality/bigger file

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
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to " WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println("\nWiFi failed");
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
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAME_SIZE;
  config.jpeg_quality = JPEG_QUALITY;
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
static size_t streamImage(camera_fb_t* fb, WiFiClientSecure* client) {
  const size_t CHUNK = 192;
  char out[3 * 4 * 64 + 4];
  size_t total = 0;
  for (size_t i = 0; i < fb->len; i += CHUNK) {
    size_t n = min(CHUNK, fb->len - i);
    size_t o = b64UrlEncode(fb->buf + i, n, out);
    if (client) client->write((const uint8_t*)out, o);
    total += o;
  }
  return total;
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

  WiFiClientSecure client;
  client.setInsecure();   // skip certificate check (simple; fine for a hobby project)
  Serial.println("Connecting to " + String(myDomain));
  if (!client.connect(myDomain, 443)) {
    Serial.println("Connection failed");
    esp_camera_fb_return(fb);
    return false;
  }

  String head = "filename=ESP32-CAM.jpg&mimetype=image/jpeg&data=";
  size_t bodyLen = head.length() + streamImage(fb, nullptr);

  client.print("POST /macros/s/" GOOGLE_SCRIPT_ID "/exec HTTP/1.1\r\n");
  client.print("Host: " + String(myDomain) + "\r\n");
  client.print("Content-Length: " + String(bodyLen) + "\r\n");
  client.print("Content-Type: application/x-www-form-urlencoded\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(head);
  streamImage(fb, &client);
  esp_camera_fb_return(fb);

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
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // disable brownout reset
  Serial.begin(115200);
  delay(100);

  if (connectWifi() && initCamera()) {
    Serial.println(uploadPhoto() ? "Upload OK" : "Upload failed");
    esp_camera_deinit();
  }
  goToSleep();
}

void loop() {}  // never reached: setup() ends in deep sleep
