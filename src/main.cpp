#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"

const char* ssid = "FPV-Rover";
const char* password = "password123";

// Motor Driver Pins
#define IN1 14
#define IN2 15
#define IN3 13
#define IN4 12

#define WIFI_CHANNEL 6
#define WIFI_TX_POWER WIFI_POWER_19_5dBm

#define CAMERA_XCLK_HZ 20000000 
#define VIDEO_FRAME_SIZE FRAMESIZE_QQVGA 
#define VIDEO_JPEG_QUALITY 12 

#define DISABLE_BROWNOUT_DETECTOR 1
#define CMD_TIMEOUT_MS 500

// Camera Pins
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

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
WebServer streamServer(81); 

bool cameraOk = false;
float streamFps = 0;
bool motorsRunning = false;
unsigned long lastCmdTime = 0;

void stopMotors() {
    digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
    motorsRunning = false;
}

void drive(int a, int b, int c, int d) {
    digitalWrite(IN1, a); digitalWrite(IN2, b);
    digitalWrite(IN3, c); digitalWrite(IN4, d);
    motorsRunning = true;
    lastCmdTime = millis();
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len) {
            data[len] = 0;
            String action = (char*)data;
            
            // 🚀 THE FIX BASED ON YOUR EXACT SYMPTOMS 🚀
            
            // If Left button made it go straight, then this is the STRAIGHT movement
            if (action == "left")          drive(LOW, HIGH, LOW, HIGH); 
            // Reverse of straight
            else if (action == "right")    drive(HIGH, LOW, HIGH, LOW); 
            
            // If Forward button made it turn, then this is the TURN movement
            else if (action == "forward")  drive(HIGH, LOW, LOW, HIGH); 
            // Reverse of turn
            else if (action == "backward") drive(LOW, HIGH, HIGH, LOW); 
            
            else if (action == "stop")     stopMotors();
            
            lastCmdTime = millis();
        }
    }
}

bool sendAll(WiFiClient& client, const uint8_t* data, size_t len, unsigned long timeoutMs = 8000) {
    size_t sent = 0;
    unsigned long lastProgress = millis();
    while (sent < len) {
        if (!client.connected()) return false;
        size_t n = client.write(data + sent, len - sent);
        if (n > 0) { sent += n; lastProgress = millis(); }
        else {
            if (millis() - lastProgress > timeoutMs) return false;
            vTaskDelay(1 / portTICK_PERIOD_MS); 
        }
    }
    return true;
}

void handleStreamTask() {
    WiFiClient client = streamServer.client();
    if (!cameraOk) { client.print("HTTP/1.1 503 Service Unavailable\r\n\r\n"); return; }
    
    client.setNoDelay(true);
    client.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");

    unsigned long statStart = millis();
    unsigned int frames = 0;

    while (client.connected()) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(10); continue; }

        client.printf("\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
        bool ok = sendAll(client, fb->buf, fb->len);
        esp_camera_fb_return(fb);

        if (!ok) break; 
        
        frames++;
        if (millis() - statStart >= 2000) {
            streamFps = frames * 1000.0 / (millis() - statStart);
            ws.textAll("FPS:" + String(streamFps, 1)); 
            statStart = millis(); frames = 0;
        }
    }
}

void streamTask(void* param) {
    streamServer.on("/stream", HTTP_GET, handleStreamTask);
    streamServer.begin();
    for(;;) { streamServer.handleClient(); vTaskDelay(1); }
}

bool initCamera() {
    camera_config_t config;
    memset(&config, 0, sizeof(config));
    config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM; config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = CAMERA_XCLK_HZ; config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = VIDEO_FRAME_SIZE; config.jpeg_quality = VIDEO_JPEG_QUALITY;

    if (psramFound()) {
        config.fb_count = 2; 
        config.fb_location = CAMERA_FB_IN_PSRAM; 
        config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
        config.fb_count = 1; 
        config.fb_location = CAMERA_FB_IN_DRAM; 
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }
    return esp_camera_init(&config) == ESP_OK;
}

void setup() {
#if DISABLE_BROWNOUT_DETECTOR
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif
    Serial.begin(115200); 
    delay(1000);

    pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); 
    pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
    stopMotors();

    cameraOk = initCamera();
    Serial.println(cameraOk ? "Camera OK" : "Camera FAIL");

    if (!LittleFS.begin(true)) Serial.println("LittleFS Error");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password, WIFI_CHANNEL, 0, 4);
    WiFi.setTxPower(WIFI_TX_POWER);
    WiFi.setSleep(false);

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/index.html", "text/html");
    });
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/style.css", "text/css");
    });
    server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/script.js", "application/javascript");
    });
    
    server.begin();
    xTaskCreatePinnedToCore(streamTask, "streamTask", 8192, NULL, 1, NULL, 0);
}

void loop() {
    ws.cleanupClients();
    
    if (ws.count() == 0 && motorsRunning) {
        stopMotors();
    }

    if (motorsRunning && (millis() - lastCmdTime > CMD_TIMEOUT_MS)) {
        stopMotors();
    }
    delay(10);
}