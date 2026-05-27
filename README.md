# esp32OTA

An ESP32 OTA (over-the-air) firmware update library for Arduino, supporting:

- Multi-partition updates from a single JSON manifest
- HTTP and HTTPS (WiFi and W5500 Ethernet)
- App partition rollback via ESP-IDF
- Custom HTTP headers per request
- SHA-256 binary integrity verification (optional)
- Home Assistant MQTT Update integration compatible

Loosely based on [chrisjoyce911/esp32FOTA](https://github.com/chrisjoyce911/esp32FOTA).

---

## Requirements

- ESP32 Arduino core 3.x+
- [ArduinoJson](https://arduinojson.org/) 7.x
- App version and name embedded by the build system (see below)
- sdkconfig: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`

---

## Embedding Version and Application Name

The library reads version and application name at runtime from `esp_ota_get_app_description()`. These are standard ESP-IDF fields populated by the build system — set them using the mechanisms below, not preprocessor defines.

**PlatformIO (platformio.ini):**
```ini
[env:myboard]
board_build.cmake_extra_args =
    -DPROJECT_VER="2026.05.01"
    -DPROJECT_NAME="My Device"
```

**ESP-IDF (CMakeLists.txt):**
```cmake
project(my_project)
set(PROJECT_VER "2026.05.01")
set(PROJECT_NAME "My Device")
```

If version is not embedded, `checkForUpdate()` will fire `onError` with `ESP_ERR_INVALID_VERSION` and return false.

---

## Manifest Format

```json
{
    "application_name": "My Device",
    "version": "2026.05.01",
    "binaries": [
        {"partition": "ui",  "url": "https://firmware.example.com/2026.05.01/ui.bin"},
        {"partition": "app", "url": "https://firmware.example.com/2026.05.01/firmware.bin"}
    ],
    "release_url": "https://github.com/example/device/releases/tag/2026.05.01"
}
```

The `app` partition is always flashed first, regardless of its position in `binaries`. If app flashing fails, no other partitions are touched.

`application_name` is required in all JSON manifests (object or array). Entries missing it are rejected with `onError(ESP_ERR_INVALID_ARG)`. For a forced flash with no manifest, use `flashPartition(label, url)` instead.

For multi-application manifests, use a top-level JSON array. The library matches the first entry whose `application_name` equals the running firmware's `project_name` from `esp_app_desc_t`.

### Optional SHA-256 Verification

Add a `sha256` field to any binary entry to validate the download:

```json
{"partition": "app", "url": "https://...", "sha256": "abc123..."}
```

---

## Quick Start

```cpp
#include <esp32OTA.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

esp32OTA ota;
WiFiClientSecure client;

void setup() {
    WiFi.begin("ssid", "password");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    // Always call first in setup() — cancels rollback if this is a post-OTA boot.
    ota.markAppValid();

    client.setCACertBundle(x509_crt_imported_bundle_bin_start);
    ota.setClient(&client);
    ota.setManifestURL("https://firmware.example.com/manifest.json");
    ota.addHeader("X-Device-UUID", "your-device-uuid");
    ota.setBlockedPartitions({"config", "nvs"});

    ota.onAvailable([](const char* version, const char* app, const char* url) {
        Serial.printf("Update available: %s\n", version);
    });
    ota.onProgress([](const char* partition, size_t written, size_t total) {
        Serial.printf("[%s] %u / %u\n", partition, written, total);
    });
    ota.onComplete([](bool success) {
        Serial.println(success ? "OTA complete — rebooting" : "OTA failed");
    });
}

void loop() {
    if (ota.checkForUpdate()) {
        ota.execOTA();
    }
    delay(86400000UL);
}
```

---

## Forced OTA

### Force from JSON manifest (no version check)

```cpp
JsonDocument doc;
deserializeJson(doc, manifestJsonString);
ota.execOTA(doc);
```

### Force a single partition directly

```cpp
// Does not reboot — caller decides when to restart
ota.flashPartition("ui", "https://firmware.example.com/ui.bin");
ESP.restart();
```

---

## Rollback

After an OTA update, the ESP-IDF bootloader waits for `markAppValid()` before permanently committing the new firmware. If the device reboots before this call, it reverts to the previous OTA slot.

```cpp
void setup() {
    ota.markAppValid();  // always call early in setup()

    if (!hardwareCheckPassed()) {
        ota.markAppInvalid();  // reboots immediately into previous slot
    }
}
```

Requires `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in sdkconfig.

---

## HTTP vs HTTPS

**Plain HTTP:**
```cpp
WiFiClient client;
ota.setClient(&client);
```

**HTTPS with bundled Mozilla CA certificates:**
```cpp
WiFiClientSecure client;
client.setCACertBundle(x509_crt_imported_bundle_bin_start);
ota.setClient(&client);
```

**HTTPS with a custom PEM certificate:**
```cpp
WiFiClientSecure client;
client.setCACert(my_root_ca_pem);
ota.setClient(&client);
```

**W5500 Ethernet with HTTPS:** use `SSLClient` (OPEnSLab-OSU) wrapping `EthernetClient`.

