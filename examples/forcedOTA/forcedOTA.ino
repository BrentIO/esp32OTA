/*
 * forcedOTA — two forced-update paths exposed over HTTP:
 *   POST /update  body: manifest JSON  → execOTA(doc), no version check
 *   POST /flash?partition=ui&url=...   → flashPartition(), no restart
 *
 * Version and app name must be embedded by the build system:
 *   PlatformIO: board_build.cmake_extra_args = -DPROJECT_VER="2026.05.01" -DPROJECT_NAME="My Device"
 *   ESP-IDF:    set(PROJECT_VER "2026.05.01") in CMakeLists.txt
 *
 * sdkconfig:
 *   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
 */

#include <esp32OTA.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>

const char* WIFI_SSID     = "your-ssid";
const char* WIFI_PASSWORD = "your-password";

esp32OTA  ota;
WiFiClient client;
WebServer  server(80);

// POST /update
// Body: manifest JSON (object or array). Flashes immediately, no version check.
void handleForceUpdate() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    server.send(200, "text/plain", "OTA starting");
    ota.execOTA(doc);
}

// POST /flash?partition=ui&url=https://...
// Flashes a single partition; caller is responsible for restarting.
void handleFlashPartition() {
    String partition = server.arg("partition");
    String url       = server.arg("url");
    if (partition.isEmpty() || url.isEmpty()) {
        server.send(400, "text/plain", "Missing partition or url parameter");
        return;
    }
    server.send(200, "text/plain", "Flashing " + partition);
    if (ota.flashPartition(partition.c_str(), url.c_str())) {
        ESP.restart();
    }
}

void setup() {
    Serial.begin(115200);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

    ota.markAppValid();
    ota.setClient(&client);
    ota.setBlockedPartitions({"config", "nvs"});

    ota.onProgress([](const char* partition, size_t written, size_t total) {
        Serial.printf("[%s] %u / %u\n", partition, written, total);
    });
    ota.onPartitionComplete([](const char* partition, bool ok) {
        Serial.printf("[%s] %s\n", partition, ok ? "done" : "FAILED");
    });
    ota.onComplete([](bool ok) {
        Serial.println(ok ? "OTA complete — rebooting" : "OTA failed");
    });
    ota.onError([](const char* partition, int code) {
        Serial.printf("OTA error [%s]: 0x%x\n", partition, code);
    });

    server.on("/update", HTTP_POST, handleForceUpdate);
    server.on("/flash",  HTTP_POST, handleFlashPartition);
    server.begin();
    Serial.println("HTTP server started");
}

void loop() {
    server.handleClient();
}
