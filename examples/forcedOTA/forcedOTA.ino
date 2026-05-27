/*
 * forcedOTA — demonstrates two forced-update paths with no version check:
 *   execOTA(doc)              — full manifest JSON, flashes all listed partitions
 *   flashPartition(label, url) — single partition directly from a URL
 *
 * Demonstrates loading a self-signed CA certificate from SPIFFS into PSRAM
 * so it survives the OTA session without consuming internal heap. Falls back
 * to internal heap if PSRAM is not available.
 *
 * The cert must be stored at /cert.pem on the SPIFFS partition before running.
 *
 * How the forced paths are triggered is an application concern (MQTT, HTTP
 * server, serial command, etc.) — this sketch shows the library calls only.
 *
 * Version and app name must be embedded by the build system:
 *   arduino-cli: --build-property "build.extra_flags=-DPROJECT_VER=\"2026.05.01\" -DPROJECT_NAME=\"My Device\""
 *   ESP-IDF:    set(PROJECT_VER "2026.05.01") in CMakeLists.txt
 *
 * sdkconfig:
 *   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
 */

#include <esp32OTA.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>

const char* WIFI_SSID     = "your-ssid";
const char* WIFI_PASSWORD = "your-password";

esp32OTA         ota;
WiFiClientSecure client;

char* certBuf = nullptr;

bool loadCertFromSPIFFS(const char* path) {
    File f = SPIFFS.open(path, "r");
    if (!f) return false;

    size_t len = f.size();

#if defined(CONFIG_SPIRAM) || defined(BOARD_HAS_PSRAM)
    certBuf = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
#endif
    if (!certBuf) certBuf = (char*)malloc(len + 1);
    if (!certBuf) { f.close(); return false; }

    f.readBytes(certBuf, len);
    certBuf[len] = '\0';
    f.close();
    return true;
}

void setup() {
    Serial.begin(115200);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");

    ota.markAppValid();

    if (!SPIFFS.begin()) {
        Serial.println("SPIFFS mount failed");
        return;
    }

    if (!loadCertFromSPIFFS("/cert.pem")) {
        Serial.println("Failed to load cert — check /cert.pem on SPIFFS");
        return;
    }

    client.setCACert(certBuf);  // certBuf lives in PSRAM for the application lifetime
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
}

void loop() {
    // Trigger these from your application logic (MQTT callback, HTTP handler,
    // serial command, etc.) as needed.

    // --- Forced full manifest update (no version check) ---
    // const char* manifestJson = R"({
    //     "application_name": "My Device",
    //     "version": "2026.06.01",
    //     "binaries": [
    //         {"partition": "app", "url": "https://firmware.example.com/firmware.bin"},
    //         {"partition": "ui",  "url": "https://firmware.example.com/ui.bin"}
    //     ]
    // })";
    // JsonDocument doc;
    // deserializeJson(doc, manifestJson);
    // ota.execOTA(doc);

    // --- Forced single-partition flash (no restart — caller decides when) ---
    // ota.flashPartition("ui", "https://firmware.example.com/ui.bin");
    // ESP.restart();
}
