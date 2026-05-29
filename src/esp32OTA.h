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

#include <NetworkClientSecure.h>

// Subclass that accesses the protected sslclient context to activate the
// ESP-IDF built-in Mozilla CA bundle via esp_crt_bundle_attach, without
// needing raw bundle bytes (which are not available as ELF symbols in
// Arduino/PlatformIO builds). Works on any lwIP-backed transport: WiFi,
// native RMII Ethernet, or W5500 attached via the ESP-IDF SPI Ethernet driver.
//
// connect() overrides re-attach the bundle before each connection because
// stop_ssl_socket() zeros the sslclient_context struct (including bundle_attach_cb),
// causing every connection after the first stop() to fail with error -1.
class esp32OTA_SecureClient : public NetworkClientSecure {
public:
    void useFrameworkCertBundle() {
        _useBundle = true;
        attach_ssl_certificate_bundle(sslclient.get(), true);
        _use_ca_bundle = true;
    }

    int connect(const char* host, uint16_t port) override {
        if (_useBundle) attach_ssl_certificate_bundle(sslclient.get(), true);
        return NetworkClientSecure::connect(host, port);
    }

    int connect(IPAddress ip, uint16_t port) override {
        if (_useBundle) attach_ssl_certificate_bundle(sslclient.get(), true);
        return NetworkClientSecure::connect(ip, port);
    }

private:
    bool _useBundle = false;
};

using OTAProgressCallback      = std::function<void(const char* partition, size_t written, size_t total)>;
using OTAPartitionDoneCallback = std::function<void(const char* partition, bool success)>;
using OTACompleteCallback      = std::function<void(bool success)>;
using OTAAvailableCallback     = std::function<void(const char* version, const char* application_name, const char* release_url)>;
using OTAErrorCallback         = std::function<void(const char* partition, int error_code)>;

class esp32OTA {
public:
    ~esp32OTA();

    // URL of manifest JSON to fetch for periodic/automatic OTA checks.
    void setManifestURL(const char* url);
    void setManifestURL(const String& url);

    // Add a custom HTTP header sent with every request (manifest and binary downloads).
    // Call multiple times to register multiple headers.
    void addHeader(const char* name, const char* value);

    // Set the network client. Must be set before any OTA operation.
    // For WiFi:   pass a WiFiClient* or WiFiClientSecure*
    // For W5500:  pass an EthernetClient* or SSLClient<EthernetClient>*
    // The library does not own the pointer; the caller manages the client lifetime.
    void setClient(Client* client);

    // Activate the ESP-IDF built-in Mozilla CA bundle for HTTPS.
    // Internally allocates an esp32OTA_SecureClient and configures it via
    // esp_crt_bundle_attach — no raw bundle bytes or manual cert setup required.
    // setClient() is not required when this is called.
    //
    // Requires a lwIP-backed network interface: WiFi, native RMII Ethernet, or
    // W5500 attached via the ESP-IDF SPI Ethernet driver (ETH.begin()).
    // Does not work with W5500 via the Arduino Ethernet library + SSLClient (BearSSL).
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

    // Set the running firmware version used for manifest comparison.
    // Must be called before checkForUpdate() or execOTA().
    void setCurrentVersion(const char* version);

    // Set the application name used to find the matching manifest entry.
    // Must be called before checkForUpdate() or execOTA().
    void setApplicationName(const char* name);

    // Fetch manifest from the configured URL. Compare manifest version against the running
    // firmware's version set via currentVersion(). Fires onAvailable if a newer version is found.
    // Returns true if an update is available. Does not flash.
    bool checkForUpdate();

    // Execute OTA using the manifest most recently fetched by checkForUpdate().
    // No-op if checkForUpdate() has not been called or returned false.
    // Calls esp_restart() on success.
    bool execOTA();

    // Convenience: fetch manifest from url, compare version, execute if newer.
    bool execOTA(const char* url);

    // Execute OTA from a pre-parsed JsonDocument. No version comparison — flashes immediately.
    // If the document is a JSON array, application_name is used to select the matching entry.
    // If the document is a plain object, it is used directly without name matching.
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

    Client*                  _client       = nullptr;
    esp32OTA_SecureClient*   _bundleClient = nullptr;
    String               _manifestURL;
    std::vector<Header>  _headers;
    std::vector<String>  _blockedPartitions;
    String               _certificate;
    String               _availableVersion;
    String               _releaseURL;
    String               _cachedManifest;
    String               _currentVersion;
    String               _applicationName;

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
