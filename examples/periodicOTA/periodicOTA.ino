/*
 * periodicOTA — checks for an update once per day and flashes automatically.
 *
 * Uses the ESP-IDF built-in Mozilla CA bundle via useBundledCerts(). No client
 * object or certificate management is required; the library handles HTTPS
 * internally through esp_http_client with crt_bundle_attach.
 *
 * Version and app name must be embedded by the build system:
 *   arduino-cli: --build-property "build.extra_flags=-DPROJECT_VER=\"2026.05.01\" -DPROJECT_NAME=\"My Device\""
 *   ESP-IDF:    set(PROJECT_VER "2026.05.01") in CMakeLists.txt
 *
 * sdkconfig:
 *   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
 */

#include <esp32OTA.h>
#include <WiFi.h>

const char* WIFI_SSID     = "your-ssid";
const char* WIFI_PASSWORD = "your-password";
const char* MANIFEST_URL  = "https://firmware.example.com/manifest.json";
const char* DEVICE_UUID   = "your-device-uuid";

esp32OTA ota;

void setup() {
    Serial.begin(115200);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");

    // Always call first in setup() — cancels rollback if this is a post-OTA boot.
    ota.markAppValid();

    ota.useBundledCerts();
    ota.setManifestURL(MANIFEST_URL);
    ota.addHeader("X-Device-UUID", DEVICE_UUID);

    ota.setBlockedPartitions({"config", "nvs"});

    ota.onAvailable([](const char* version, const char* appName, const char* releaseUrl) {
        Serial.printf("Update available: %s (%s)\n", version, appName);
        if (strlen(releaseUrl)) Serial.printf("Release notes: %s\n", releaseUrl);
    });

    ota.onProgress([](const char* partition, size_t written, size_t total) {
        if (total) Serial.printf("[%s] %u / %u bytes (%.1f%%)\n",
                                 partition, written, total, 100.0f * written / total);
        else       Serial.printf("[%s] %u bytes written\n", partition, written);
    });

    ota.onPartitionComplete([](const char* partition, bool success) {
        Serial.printf("[%s] %s\n", partition, success ? "done" : "FAILED");
    });

    ota.onComplete([](bool success) {
        Serial.println(success ? "OTA complete — rebooting" : "OTA failed");
    });

    ota.onError([](const char* partition, int code) {
        Serial.printf("OTA error [%s]: 0x%x\n", partition, code);
    });
}

void loop() {
    if (ota.checkForUpdate()) {
        ota.execOTA();
    }
    delay(86400000UL);  // 24 hours
}
