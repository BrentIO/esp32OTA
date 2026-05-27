/*
 * periodicOTA — checks for an update once per day and flashes automatically.
 *
 * Version and app name must be embedded by the build system:
 *   PlatformIO: board_build.cmake_extra_args = -DPROJECT_VER="2026.05.01" -DPROJECT_NAME="My Device"
 *   ESP-IDF:    set(PROJECT_VER "2026.05.01") in CMakeLists.txt
 *
 * sdkconfig:
 *   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
 */

#include <esp32OTA.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ESP-IDF bundled Mozilla CA certificate bundle — attach in platformio.ini:
//   board_build.embed_files = ${COMPONENT_EMBED_FILES}
// or via Arduino IDE board support.
extern const uint8_t x509_crt_imported_bundle_bin_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_imported_bundle_bin_end[]   asm("_binary_x509_crt_bundle_end");

const char* WIFI_SSID     = "your-ssid";
const char* WIFI_PASSWORD = "your-password";
const char* MANIFEST_URL  = "https://firmware.example.com/manifest.json";
const char* DEVICE_UUID   = "your-device-uuid";

esp32OTA       ota;
WiFiClientSecure client;

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

    client.setCACertBundle(x509_crt_imported_bundle_bin_start,
                           x509_crt_imported_bundle_bin_end - x509_crt_imported_bundle_bin_start);
    ota.setClient(&client);
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
