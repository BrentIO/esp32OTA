/*
 * periodicOTA — checks for an update once per day and flashes automatically.
 *
 * Uses W5500 SPI Ethernet for connectivity and the ESP-IDF built-in Mozilla CA
 * bundle via useBundledCerts(). No client object or certificate management is
 * required; the library handles HTTPS internally through esp_http_client with
 * crt_bundle_attach.
 *
 * Pin assignments below are examples — adjust for your hardware.
 * Requires ESP32 Arduino core v3.x for ETH.h W5500 SPI support.
 *
 * Version and app name must be embedded by the build system:
 *   arduino-cli: --build-property "build.extra_flags=-DPROJECT_VER=\"2026.05.01\" -DPROJECT_NAME=\"My Device\""
 *   ESP-IDF:    set(PROJECT_VER "2026.05.01") in CMakeLists.txt
 *
 * sdkconfig:
 *   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
 */

#include <esp32OTA.h>
#include <ETH.h>

// W5500 SPI wiring — adjust for your board
#define W5500_CS    5
#define W5500_INT   4
#define W5500_RST   -1   // -1 if reset is not wired
#define W5500_SCK   18
#define W5500_MISO  19
#define W5500_MOSI  23

const char* MANIFEST_URL = "https://firmware.example.com/manifest.json";
const char* DEVICE_UUID  = "your-device-uuid";

esp32OTA    ota;
static bool eth_connected = false;

void onEthEvent(arduino_event_id_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.printf("Ethernet up — IP: %s\n", ETH.localIP().toString().c_str());
            eth_connected = true;
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("Ethernet disconnected");
            eth_connected = false;
            break;
        default:
            break;
    }
}

void setup() {
    Serial.begin(115200);

    Network.onEvent(onEthEvent);
    ETH.begin(ETH_PHY_W5500, 1, W5500_CS, W5500_INT, W5500_RST,
              SPI3_HOST, W5500_SCK, W5500_MISO, W5500_MOSI);

    while (!eth_connected) delay(100);

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
