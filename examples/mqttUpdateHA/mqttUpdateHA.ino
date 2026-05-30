/*
 * mqttUpdateHA — wires esp32OTA callbacks into the Home Assistant MQTT Update integration.
 *
 * The library provides version/name/release_url via onAvailable; this sketch
 * publishes them to the HA MQTT Update entity. MQTT install commands trigger execOTA().
 *
 * Uses the ESP-IDF built-in Mozilla CA bundle via useBundledCerts() for OTA HTTPS.
 * The MQTT transport uses a plain WiFiClient on an unencrypted broker connection;
 * swap it for WiFiClientSecure if your broker requires TLS.
 *
 * HA MQTT Update integration docs:
 *   https://www.home-assistant.io/integrations/update.mqtt/
 *
 * Dependencies: PubSubClient (knolleary/pubsubclient)
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
#include <PubSubClient.h>
#include <esp_ota_ops.h>

const char* WIFI_SSID     = "your-ssid";
const char* WIFI_PASSWORD = "your-password";
const char* MANIFEST_URL  = "https://firmware.example.com/manifest.json";

const char* MQTT_BROKER = "mqtt.example.com";
const int   MQTT_PORT   = 1883;

// Used in compile-time string concatenation for topic names — must be a #define
#define MQTT_CLIENT_ID "my-device"

// Home Assistant MQTT Update entity topics
const char* HA_CONFIG_TOPIC  = "homeassistant/update/" MQTT_CLIENT_ID "/config";
const char* HA_STATE_TOPIC   = "homeassistant/update/" MQTT_CLIENT_ID "/state";
const char* HA_COMMAND_TOPIC = "homeassistant/update/" MQTT_CLIENT_ID "/set";

esp32OTA     ota;
WiFiClient   mqttTransport;
PubSubClient mqtt(mqttTransport);

void publishState(const char* installed, const char* latest,
                  const char* title, const char* releaseUrl) {
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"installed_version\":\"%s\","
        "\"latest_version\":\"%s\","
        "\"title\":\"%s\","
        "\"release_url\":\"%s\"}",
        installed, latest, title, releaseUrl);
    mqtt.publish(HA_STATE_TOPIC, payload, /*retain=*/true);
}

void publishDiscovery() {
    char config[512];
    snprintf(config, sizeof(config),
        "{\"name\":\"Firmware\","
        "\"state_topic\":\"%s\","
        "\"command_topic\":\"%s\","
        "\"payload_install\":\"install\","
        "\"unique_id\":\"%s_firmware\"}",
        HA_STATE_TOPIC, HA_COMMAND_TOPIC, MQTT_CLIENT_ID);
    mqtt.publish(HA_CONFIG_TOPIC, config, /*retain=*/true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[64] = {};
    memcpy(msg, payload, min((unsigned int)sizeof(msg) - 1, length));

    if (strcmp(topic, HA_COMMAND_TOPIC) == 0 && strcmp(msg, "install") == 0) {
        if (ota.checkForUpdate()) ota.execOTA();
    }
}

void connectMQTT() {
    while (!mqtt.connected()) {
        Serial.print("Connecting to MQTT...");
        if (mqtt.connect(MQTT_CLIENT_ID)) {
            Serial.println(" connected");
            mqtt.subscribe(HA_COMMAND_TOPIC);
            publishDiscovery();

            const esp_app_desc_t* desc = esp_ota_get_app_description();
            publishState(desc->version, desc->version, desc->project_name, "");
        } else {
            Serial.printf(" failed (rc=%d), retrying in 5s\n", mqtt.state());
            delay(5000);
        }
    }
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

    ota.useBundledCerts();
    ota.setManifestURL(MANIFEST_URL);

    ota.onAvailable([](const char* version, const char* appName, const char* releaseUrl) {
        // Publish new version info so HA shows an "update available" badge
        const esp_app_desc_t* desc = esp_ota_get_app_description();
        publishState(desc->version, version, appName, releaseUrl);
    });

    ota.onProgress([](const char* partition, size_t written, size_t total) {
        Serial.printf("[%s] %u / %u\n", partition, written, total);
        // Publish in_progress percentage so HA shows a progress bar
        const esp_app_desc_t* desc = esp_ota_get_app_description();
        int pct = (total > 0) ? (int)(100 * written / total) : 1;
        char payload[256];
        snprintf(payload, sizeof(payload),
            "{\"installed_version\":\"%s\",\"latest_version\":\"%s\",\"in_progress\":%d}",
            desc->version, ota.getAvailableVersion(), pct);
        mqtt.publish(HA_STATE_TOPIC, payload);
    });

    ota.onPartitionComplete([](const char* partition, bool success) {
        Serial.printf("[%s] %s\n", partition, success ? "done" : "FAILED");
        // App failure aborts immediately and fires onComplete(false).
        // Data partition failure continues remaining partitions; onComplete(false)
        // fires at the end — restart or not is the caller's decision.
    });

    ota.onComplete([](bool success) {
        Serial.println(success ? "OTA complete — rebooting" : "OTA failed");
        if (!success) {
            // Reset in_progress so HA removes the progress bar.
            // HA confirms a successful update on the next boot when installed_version
            // matches latest_version in the reconnect publishState() call.
            const esp_app_desc_t* desc = esp_ota_get_app_description();
            char payload[256];
            snprintf(payload, sizeof(payload),
                "{\"installed_version\":\"%s\",\"latest_version\":\"%s\",\"in_progress\":false}",
                desc->version, ota.getAvailableVersion());
            mqtt.publish(HA_STATE_TOPIC, payload, /*retain=*/true);
        }
    });

    ota.onError([](const char* partition, int code) {
        Serial.printf("OTA error [%s]: 0x%x\n", partition, code);
        // HA MQTT Update has no native error state; reset in_progress and publish
        // error details to a dedicated topic for application-level handling.
        const esp_app_desc_t* desc = esp_ota_get_app_description();
        char statePayload[256];
        snprintf(statePayload, sizeof(statePayload),
            "{\"installed_version\":\"%s\",\"latest_version\":\"%s\",\"in_progress\":false}",
            desc->version, ota.getAvailableVersion());
        mqtt.publish(HA_STATE_TOPIC, statePayload, /*retain=*/true);

        char errPayload[128];
        snprintf(errPayload, sizeof(errPayload),
            "{\"partition\":\"%s\",\"code\":%d}", partition, code);
        mqtt.publish("homeassistant/update/" MQTT_CLIENT_ID "/error", errPayload);
    });

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    connectMQTT();
}

void loop() {
    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();

    // Periodic manifest check (every hour)
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 3600000UL) {
        lastCheck = millis();
        if (ota.checkForUpdate()) ota.execOTA();
    }
}
