#pragma once

// Maximum time in milliseconds to wait for the server to begin responding.
// Override at build time: -DESP32OTA_CONNECT_TIMEOUT_MS=20000
#ifndef ESP32OTA_CONNECT_TIMEOUT_MS
#define ESP32OTA_CONNECT_TIMEOUT_MS 10000
#endif

// Maximum time in milliseconds to wait between data chunks before aborting.
// Override at build time: -DESP32OTA_STALL_TIMEOUT_MS=10000
#ifndef ESP32OTA_STALL_TIMEOUT_MS
#define ESP32OTA_STALL_TIMEOUT_MS 5000
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Client.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <vector>
#include <functional>

using OTAProgressCallback      = std::function<void(const char* partition, size_t written, size_t total)>;
using OTAPartitionDoneCallback = std::function<void(const char* partition, bool success)>;
using OTACompleteCallback      = std::function<void(bool success)>;
using OTAAvailableCallback     = std::function<void(const char* version, const char* application_name, const char* release_url)>;
using OTAErrorCallback         = std::function<void(const char* partition, int error_code)>;

class esp32OTA {
public:
    // URL of manifest JSON to fetch for periodic/automatic OTA checks.
    void setManifestURL(const char* url);
    void setManifestURL(const String& url);

    // Add a custom HTTP header sent with every request (manifest and binary downloads).
    // Call multiple times to register multiple headers.
    void addHeader(const char* name, const char* value);

    // Set the network client. Must be set before any OTA operation.
    // For WiFi:   pass a WiFiClient* or WiFiClientSecure*
    // For W5500:  pass an SSLClient<EthernetClient>* or EthernetClient*
    // The library does not own the pointer; the caller manages the client lifetime.
    void setClient(Client* client);

    // Intent marker — TLS configuration is handled entirely by the Client* passed
    // via setClient(). Attach the certificate bundle to the client before calling setClient().
    void useBundledCerts();

    // Convenience getter/setter for a PEM-encoded root CA string.
    // The library does not apply it; the caller sets the cert on the client before setClient().
    void setCertificate(const char* pem);
    const char* getCertificate() const;

    // Block a partition label from any OTA write.
    // Library-level type protection (SPIFFS/FAT only for non-app partitions) is always enforced.
    // Example: ota.setBlockedPartitions({"config", "nvs"});
    void setBlockedPartitions(std::initializer_list<const char*> labels);

    void onProgress(OTAProgressCallback cb);
    void onPartitionComplete(OTAPartitionDoneCallback cb);
    void onComplete(OTACompleteCallback cb);
    void onAvailable(OTAAvailableCallback cb);
    void onError(OTAErrorCallback cb);

    // Fetch manifest from the configured URL. Compare manifest version against the running
    // firmware's esp_app_desc_t. Fires onAvailable if a newer version is found.
    // Returns true if an update is available. Does not flash.
    bool checkForUpdate();

    // Execute OTA using the manifest most recently fetched by checkForUpdate().
    // No-op if checkForUpdate() has not been called or returned false.
    // Calls esp_restart() on success.
    bool execOTA();

    // Convenience: fetch manifest from url, compare version, execute if newer.
    bool execOTA(const char* url);

    // Execute OTA from a pre-parsed JsonDocument. No version comparison — flashes immediately.
    // application_name filtering still applies if the field is present.
    bool execOTA(JsonDocument& doc);

    // Flash a single partition immediately from a URL. No version check. No restart.
    // Blocked partition list and library-level type protection are enforced.
    bool flashPartition(const char* partitionLabel, const char* url);

    // Confirm the running app firmware is valid. Call after startup self-tests pass.
    // Wraps esp_ota_mark_app_valid_cancel_rollback().
    void markAppValid();

    // Reject the running firmware and immediately reboot to the previous OTA slot.
    // Wraps esp_ota_mark_app_invalid_rollback_and_reboot().
    void markAppInvalid();

    const char* getAvailableVersion() const;
    const char* getReleaseURL() const;

private:
    struct Header { String name; String value; };

    Client*              _client = nullptr;
    String               _manifestURL;
    std::vector<Header>  _headers;
    std::vector<String>  _blockedPartitions;
    String               _certificate;
    String               _availableVersion;
    String               _releaseURL;
    String               _cachedManifest;

    OTAProgressCallback      _onProgress      = nullptr;
    OTAPartitionDoneCallback _onPartitionDone = nullptr;
    OTACompleteCallback      _onComplete      = nullptr;
    OTAAvailableCallback     _onAvailable     = nullptr;
    OTAErrorCallback         _onError         = nullptr;

    bool        _execManifest(JsonDocument& doc, bool forceUpdate);
    bool        _flashApp(const char* url, const char* sha256 = nullptr);
    bool        _flashDataPartition(const char* label, const char* url, const char* sha256 = nullptr);
    bool        _isBlocked(const char* label);
    bool        _downloadToPartition(const char* url, const esp_partition_t* target,
                                     bool isApp, esp_ota_handle_t* otaHandle,
                                     const char* sha256 = nullptr);
    JsonObject  _findManifestEntry(JsonDocument& doc, const char* projectName);
    bool        _httpConnect(const char* url, int& contentLength);
    String      _httpGetString(const char* url);
    JsonDocument* _allocateJsonDocument(size_t capacity);
    int         _semverCompare(const char* a, const char* b);
    bool        _verifySHA256(const uint8_t* digest, size_t len, const char* expectedHex);
};
