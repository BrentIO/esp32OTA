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

## Configuration

The following timeouts can be overridden at build time:

| Define | Default | Description |
|---|---|---|
| `ESP32OTA_CONNECT_TIMEOUT_MS` | `10000` | Milliseconds to wait for the server to begin responding |
| `ESP32OTA_STALL_TIMEOUT_MS` | `5000` | Milliseconds to wait between data chunks before aborting |

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --build-property "build.extra_flags=-DESP32OTA_CONNECT_TIMEOUT_MS=20000 -DESP32OTA_STALL_TIMEOUT_MS=10000" \
  your_sketch.ino
```

---

## Requirements

- ESP32 Arduino core 3.x+
- [ArduinoJson](https://arduinojson.org/) 7.x
- App version and name embedded by the build system (see below)
- sdkconfig: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`

---

## Embedding Version and Application Name

The library reads version and application name at runtime from `esp_ota_get_app_description()`. These are standard ESP-IDF fields populated by the build system — set them using the mechanisms below, not preprocessor defines.

**Arduino CLI:**
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --build-property "build.extra_flags=-DPROJECT_VER=\"2026.05.01\" -DPROJECT_NAME=\"My Device\"" \
  your_sketch.ino
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

Add a `sha256` field to any binary entry to validate the download before activation:

```json
{"partition": "app", "url": "https://...", "sha256": "abc123..."}
```

The same verification is available for direct partition flashes via the three-argument form of `flashPartition` — see [Force a single partition directly](#force-a-single-partition-directly).

---

## Quick Start

```cpp
#include <esp32OTA.h>
#include <WiFi.h>

esp32OTA ota;

void setup() {
    WiFi.begin("ssid", "password");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    // Always call first in setup() — cancels rollback if this is a post-OTA boot.
    ota.markAppValid();

    ota.useBundledCerts();  // HTTPS via ESP-IDF built-in Mozilla CA bundle; no client object needed
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
// With SHA-256 verification (recommended) — aborts before activation on digest mismatch
bool ok = ota.flashPartition("ui", "https://firmware.example.com/ui.bin",
                             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
if (!ok) {
    // onError fired with the error code; partition unchanged, no restart needed
    return;
}
ESP.restart();

// Without SHA-256 (skips digest check) — does not reboot, caller decides when to restart
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

**HTTPS with bundled Mozilla CA certificates (recommended):**
```cpp
ota.useBundledCerts();
// No client object needed — TLS is handled internally via esp_http_client.
// Works with any transport (WiFi, W5500 Ethernet, etc.).
```

**HTTPS with a custom PEM certificate (self-signed or private CA):**
```cpp
WiFiClientSecure client;
client.setCACert(my_root_ca_pem);
ota.setClient(&client);
```

**Plain HTTP:**
```cpp
WiFiClient client;
ota.setClient(&client);
```

---

## W5500 Ethernet

Use `ETH.h` (ESP32 Arduino core v3+), **not** WIZnet's standalone `Ethernet.h` library.

With `ETH.h`, the W5500 is used as a MAC/PHY only — the TCP/IP stack (lwIP) runs on the ESP32 via the ESP-IDF `esp_eth` SPI driver. This keeps the library on the same lwIP stack as WiFi and makes `useBundledCerts()` work correctly, since `esp_http_client` uses lwIP sockets directly.

WIZnet's `Ethernet.h` runs TCP/IP inside the W5500 chip and bypasses lwIP entirely. That path is not supported.

```cpp
#include <ETH.h>

// Adjust pins for your board
#define W5500_CS    5
#define W5500_INT   4
#define W5500_RST   -1
#define W5500_SCK   18
#define W5500_MISO  19
#define W5500_MOSI  23

Network.onEvent([](arduino_event_id_t event) {
    if (event == ARDUINO_EVENT_ETH_GOT_IP) Serial.println("Ethernet up");
});

ETH.begin(ETH_PHY_W5500, 1, W5500_CS, W5500_INT, W5500_RST,
          SPI3_HOST, W5500_SCK, W5500_MISO, W5500_MOSI);

// Then use useBundledCerts() exactly as you would over WiFi:
ota.useBundledCerts();
```

See `examples/periodicOTA` for a complete W5500 sketch.

