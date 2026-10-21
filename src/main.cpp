#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"

const char* ssid = "FPV-Rover";
const char* password = "password123";

// Motor Driver Pins (L298N)
#define IN1 14
#define IN2 15
#define IN3 13
#define IN4 12

// Wi-Fi channel (1-11). If video stutters, try 6 or 11.
#define WIFI_CHANNEL 6

// Wi-Fi transmit power. Lower = fewer current spikes on a weak power supply.
// Options: WIFI_POWER_19_5dBm (max), 15dBm, 11dBm, 8_5dBm, 5dBm. If the signal gets too weak, go up.
#define WIFI_TX_POWER WIFI_POWER_19_5dBm

// Video size and quality. Smaller / blockier = more FPS on a slow Wi-Fi link.
// Sizes: FRAMESIZE_QQVGA (160x120), FRAMESIZE_HQVGA (240x176), FRAMESIZE_QVGA (320x240)
// Quality: lower number = sharper but bigger (10 sharp ... 30 very blocky)
#define VIDEO_FRAME_SIZE FRAMESIZE_QQVGA
#define VIDEO_JPEG_QUALITY 15

// Camera clock (20 MHz is the standard value)
#define CAMERA_XCLK_HZ 20000000

// ---- Diagnostic switches ----
// ENABLE_CAMERA 0              -> run Wi-Fi only, no camera (tests if the camera/power is what breaks Wi-Fi)
// DISABLE_BROWNOUT_DETECTOR 0  -> the board reboots and prints "Brownout detector was triggered" when power sags
#define ENABLE_CAMERA 1
#define DISABLE_BROWNOUT_DETECTOR 0

// Motors stop by themselves if no command arrives for this long (ms)
#define CMD_TIMEOUT_MS 500

// CAMERA_MODEL_AI_THINKER Pin Definition
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

WebServer server(80);        // web page + motor commands (runs in loop())
WebServer streamServer(81);  // video stream (runs in its own task so it never blocks the above)

bool cameraOk = false;
float streamFps = 0;          // latest stream speed, shown on the web page
unsigned int streamKb = 0;
bool motorsRunning = false;
unsigned long lastCmdTime = 0;

void stopMotors() {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW);
    digitalWrite(IN4, LOW);
    motorsRunning = false;
}

void drive(int a, int b, int c, int d) {
    digitalWrite(IN1, a);
    digitalWrite(IN2, b);
    digitalWrite(IN3, c);
    digitalWrite(IN4, d);
    motorsRunning = true;
    lastCmdTime = millis();
}

// Send ALL bytes, retrying when the Wi-Fi stalls. (client.write() can return fewer bytes than asked;
// the built-in streamFile() ignores that and silently drops data, which broke the page CSS.)
// Returns false if the client left or got stuck for more than timeoutMs.
bool sendAll(WiFiClient& client, const uint8_t* data, size_t len, unsigned long timeoutMs = 3000) {
    size_t sent = 0;
    unsigned long lastProgress = millis();
    while (sent < len) {
        if (!client.connected()) return false;
        size_t n = client.write(data + sent, len - sent);
        if (n > 0) {
            sent += n;
            lastProgress = millis();
        } else {
            if (millis() - lastProgress > timeoutMs) return false;
            delay(2);
        }
    }
    return true;
}

// Send a file from LittleFS
void serveFile(const char* path, const char* contentType) {
    File file = LittleFS.open(path, "r");
    if (!file) {
        server.send(404, "text/plain",
                    String(path) + " not found on LittleFS. Run: PlatformIO > Upload Filesystem Image");
        return;
    }

    WiFiClient client = server.client();
    char header[192];
    int headerLen = snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                             "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                             contentType, (unsigned int)file.size());

    bool ok = sendAll(client, (const uint8_t*)header, headerLen);
    uint8_t buf[1024];
    while (ok) {
        size_t n = file.read(buf, sizeof(buf));
        if (n == 0) break;
        ok = sendAll(client, buf, n);
    }
    if (!ok) Serial.println("File send interrupted (Wi-Fi too slow or client left)");
    file.close();
}

// Rover movement control
void handleControl() {
    String action = server.hasArg("action") ? server.arg("action") : "";
    static String lastAction = "";

    if (action == "forward") {
        drive(HIGH, LOW, HIGH, LOW);
    } else if (action == "backward") {
        drive(LOW, HIGH, LOW, HIGH);
    } else if (action == "left") {
        drive(LOW, HIGH, HIGH, LOW);
    } else if (action == "right") {
        drive(HIGH, LOW, LOW, HIGH);
    } else if (action == "stop") {
        stopMotors();
    }

    // Only print when the action changes (the page repeats commands while a button is held)
    if (action != "ping" && action != lastAction) {
        Serial.println("Rover: " + action);
        lastAction = action;
    }

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain", "OK");
}

// Stream speed for the web page
void handleStats() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain", String(streamFps, 1) + "," + String(streamKb));
}

// Continuous MJPEG camera stream (port 81)
void handleStream() {
    WiFiClient client = streamServer.client();

    if (!cameraOk) {
        client.print("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\nCamera not initialised");
        return;
    }

    Serial.println("Video client connected");
    client.setNoDelay(true);
    static const char streamHeader[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n";
    if (!sendAll(client, (const uint8_t*)streamHeader, sizeof(streamHeader) - 1)) return;

    unsigned long statStart = millis();
    unsigned int frames = 0;
    size_t bytes = 0;
    unsigned long camTime = 0;   // ms spent waiting for the camera
    unsigned long netTime = 0;   // ms spent sending over Wi-Fi

    while (client.connected()) {
        unsigned long t0 = millis();
        camera_fb_t* fb = esp_camera_fb_get();
        unsigned long t1 = millis();
        if (!fb) {
            Serial.println("Camera capture failed");
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        char partHeader[96];
        int partLen = snprintf(partHeader, sizeof(partHeader),
                               "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               (unsigned int)fb->len);
        bool sentOk = sendAll(client, (const uint8_t*)partHeader, partLen) &&
                      sendAll(client, fb->buf, fb->len);
        unsigned long t2 = millis();
        if (!sentOk) {            // client left or link stuck: close so the page reconnects cleanly
            esp_camera_fb_return(fb);
            break;
        }
        camTime += t1 - t0;
        netTime += t2 - t1;

        frames++;
        bytes += fb->len;
        esp_camera_fb_return(fb);

        if (millis() - statStart >= 2000) {
            streamFps = frames * 1000.0 / (millis() - statStart);
            streamKb = (unsigned int)(bytes / frames / 1024);
            Serial.printf("Stream: %.1f FPS, avg frame %u KB | per frame: camera %lu ms, send %lu ms\n",
                          streamFps, streamKb, camTime / frames, netTime / frames);
            statStart = millis();
            frames = 0;
            bytes = 0;
            camTime = 0;
            netTime = 0;
        }
        vTaskDelay(1);
    }

    streamFps = 0;   // stream closed
    Serial.println("Video client left");
}

void streamTask(void* param) {
    for (;;) {
        streamServer.handleClient();
        vTaskDelay(1);
    }
}

bool initCamera() {
    camera_config_t config;
    memset(&config, 0, sizeof(config));

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
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = CAMERA_XCLK_HZ;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = VIDEO_FRAME_SIZE;
    config.jpeg_quality = VIDEO_JPEG_QUALITY;

    if (psramFound()) {
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;   // always the newest frame = lower latency
    } else {
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x\n", err);
        return false;
    }
    return true;
}

void onWiFiEvent(WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
        Serial.println("Wi-Fi: a device joined the rover");
    } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
        Serial.println("Wi-Fi: a device left the rover");
    }
}

void setup() {
#if DISABLE_BROWNOUT_DETECTOR
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif
    Serial.begin(115200);
    delay(1000);

    // Motor pins
    pinMode(IN1, OUTPUT);
    pinMode(IN2, OUTPUT);
    pinMode(IN3, OUTPUT);
    pinMode(IN4, OUTPUT);
    stopMotors();

    // Camera (a failure no longer stops the rest of setup)
    Serial.println(psramFound() ? "PSRAM found" : "PSRAM NOT found (slower video)");
#if ENABLE_CAMERA
    cameraOk = initCamera();
    Serial.println(cameraOk ? "Camera initialized successfully!" : "Camera NOT available");
#else
    cameraOk = false;
    Serial.println("Camera DISABLED for testing (ENABLE_CAMERA is 0)");
#endif

    // LittleFS
    if (!LittleFS.begin(true)) {
        Serial.println("An Error has occurred while mounting LittleFS");
    } else {
        Serial.println("LittleFS mounted successfully");
    }

    // Wi-Fi access point
    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password, WIFI_CHANNEL, 0, 4);
    WiFi.setTxPower(WIFI_TX_POWER);
    WiFi.setSleep(false);
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());

    // Port 80: page + controls
    server.on("/", HTTP_GET, []() { serveFile("/index.html", "text/html"); });
    server.on("/style.css", HTTP_GET, []() { serveFile("/style.css", "text/css"); });
    server.on("/script.js", HTTP_GET, []() { serveFile("/script.js", "application/javascript"); });
    server.on("/control", HTTP_GET, handleControl);
    server.on("/stats", HTTP_GET, handleStats);
    server.on("/favicon.ico", HTTP_GET, []() { server.send(204, "text/plain", ""); });
    server.begin();

    // Port 81: video, handled in its own task on core 0
    streamServer.on("/stream", HTTP_GET, handleStream);
    streamServer.begin();
    xTaskCreatePinnedToCore(streamTask, "streamTask", 8192, NULL, 1, NULL, 0);

    Serial.println("Web Server Started!");
}

void loop() {
    server.handleClient();

    // Failsafe: stop if the controller went quiet (Wi-Fi drop, lost "stop" request, closed tab...)
    if (motorsRunning && (millis() - lastCmdTime > CMD_TIMEOUT_MS)) {
        stopMotors();
        Serial.println("Failsafe: no command received, motors stopped");
    }

    delay(1);
}