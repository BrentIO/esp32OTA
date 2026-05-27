#include "esp32OTA.h"
#include <mbedtls/md.h>
#include <esp_timer.h>

// ─── Lifecycle ────────────────────────────────────────────────────────────────

esp32OTA::~esp32OTA() {
    delete _bundleClient;
}

// ─── Configuration ────────────────────────────────────────────────────────────

void esp32OTA::setManifestURL(const char* url)   { _manifestURL = url; }
void esp32OTA::setManifestURL(const String& url) { _manifestURL = url; }

void esp32OTA::addHeader(const char* name, const char* value) {
    _headers.push_back({String(name), String(value)});
}

void esp32OTA::setClient(Client* client) { _client = client; }

void esp32OTA::useBundledCerts() {
    if (!_bundleClient) {
        _bundleClient = new esp32OTA_SecureClient();
        _bundleClient->useFrameworkCertBundle();
    }
    _client = _bundleClient;
}

void esp32OTA::setCertificate(const char* pem) { _certificate = pem; }
const char* esp32OTA::getCertificate() const   { return _certificate.c_str(); }

void esp32OTA::setBlockedPartitions(std::initializer_list<const char*> labels) {
    for (const char* l : labels) _blockedPartitions.emplace_back(l);
}

// ─── Callbacks ────────────────────────────────────────────────────────────────

void esp32OTA::onProgress(OTAProgressCallback cb)              { _onProgress     = cb; }
void esp32OTA::onPartitionComplete(OTAPartitionDoneCallback cb){ _onPartitionDone = cb; }
void esp32OTA::onComplete(OTACompleteCallback cb)              { _onComplete      = cb; }
void esp32OTA::onAvailable(OTAAvailableCallback cb)            { _onAvailable     = cb; }
void esp32OTA::onError(OTAErrorCallback cb)                    { _onError         = cb; }

// ─── Introspection ────────────────────────────────────────────────────────────

const char* esp32OTA::getAvailableVersion() const { return _availableVersion.c_str(); }
const char* esp32OTA::getReleaseURL()       const { return _releaseURL.c_str(); }

// ─── Rollback ─────────────────────────────────────────────────────────────────

void esp32OTA::markAppValid()   { esp_ota_mark_app_valid_cancel_rollback(); }
void esp32OTA::markAppInvalid() { esp_ota_mark_app_invalid_rollback_and_reboot(); }

// ─── Semver ───────────────────────────────────────────────────────────────────

int esp32OTA::_semverCompare(const char* a, const char* b) {
    int a0 = 0, a1 = 0, a2 = 0, b0 = 0, b1 = 0, b2 = 0;
    sscanf(a, "%d.%d.%d", &a0, &a1, &a2);
    sscanf(b, "%d.%d.%d", &b0, &b1, &b2);
    if (a0 != b0) return a0 > b0 ? 1 : -1;
    if (a1 != b1) return a1 > b1 ? 1 : -1;
    if (a2 != b2) return a2 > b2 ? 1 : -1;
    return 0;
}

// ─── SHA-256 ──────────────────────────────────────────────────────────────────

// digest: 32-byte pre-computed SHA-256 result; expectedHex: lowercase or uppercase hex string
bool esp32OTA::_verifySHA256(const uint8_t* digest, size_t /*len*/, const char* expectedHex) {
    char hex[65] = {};
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return strcasecmp(hex, expectedHex) == 0;
}

// ─── JSON allocation ──────────────────────────────────────────────────────────

JsonDocument* esp32OTA::_allocateJsonDocument(size_t /*capacity*/) {
    return new JsonDocument();
}

// ─── Blocked partition check ──────────────────────────────────────────────────

bool esp32OTA::_isBlocked(const char* label) {
    for (const auto& b : _blockedPartitions) {
        if (b == label) return true;
    }
    return false;
}

// ─── HTTP helpers ─────────────────────────────────────────────────────────────

// Opens a connection, sends GET, parses status and headers.
// Returns true (HTTP 200) with client positioned at body start and contentLength set (-1 if unknown).
bool esp32OTA::_httpConnect(const char* url, int& contentLength) {
    String urlStr(url);
    bool isHttps = urlStr.startsWith("https://");
    String rest  = urlStr.substring(isHttps ? 8 : 7);

    int slashPos    = rest.indexOf('/');
    String hostPort = (slashPos >= 0) ? rest.substring(0, slashPos) : rest;
    String path     = (slashPos >= 0) ? rest.substring(slashPos)    : String("/");

    String host;
    uint16_t port;
    int colonPos = hostPort.lastIndexOf(':');
    if (colonPos >= 0) {
        host = hostPort.substring(0, colonPos);
        port = (uint16_t)hostPort.substring(colonPos + 1).toInt();
    } else {
        host = hostPort;
        port = isHttps ? 443 : 80;
    }

    if (!_client->connect(host.c_str(), port)) return false;

    _client->setTimeout(15000);
    _client->print(String("GET ") + path + " HTTP/1.1\r\n");
    _client->print(String("Host: ") + host + "\r\n");
    for (const auto& h : _headers) {
        _client->print(h.name + ": " + h.value + "\r\n");
    }
    _client->print("Connection: close\r\n\r\n");

    // Wait for response
    int64_t deadline = esp_timer_get_time() + ((int64_t)ESP32OTA_CONNECT_TIMEOUT_MS * 1000LL);
    while (!_client->available() && esp_timer_get_time() < deadline) delay(10);
    if (!_client->available()) {
        _client->stop();
        return false;
    }

    // Status line
    String statusLine = _client->readStringUntil('\n');
    int sp = statusLine.indexOf(' ');
    int httpCode = statusLine.substring(sp + 1, sp + 4).toInt();
    if (httpCode != 200) {
        _client->stop();
        return false;
    }

    // Headers
    contentLength = -1;
    while (_client->connected() || _client->available()) {
        String line = _client->readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) break;
        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith("content-length:")) {
            String val = line.substring(line.indexOf(':') + 1);
            val.trim();
            contentLength = val.toInt();
        }
    }

    return true;
}

// Fetches url and returns full body as a String. Empty on error.
String esp32OTA::_httpGetString(const char* url) {
    int contentLength;
    if (!_httpConnect(url, contentLength)) return String();

    String body;
    if (contentLength > 0) body.reserve(contentLength);

    uint8_t buf[256];
    int64_t lastData = esp_timer_get_time();
    while (_client->connected() || _client->available()) {
        if (_client->available()) {
            lastData = esp_timer_get_time();
            int toRead = sizeof(buf);
            if (contentLength > 0) toRead = min(toRead, contentLength - (int)body.length());
            int n = _client->readBytes(buf, toRead);
            body.concat((const char*)buf, n);
            if (contentLength > 0 && (int)body.length() >= contentLength) break;
        } else {
            if (esp_timer_get_time() - lastData > ((int64_t)ESP32OTA_STALL_TIMEOUT_MS * 1000LL)) break;
            delay(1);
        }
    }
    _client->stop();
    return body;
}

// ─── Manifest entry lookup ────────────────────────────────────────────────────

// Returns the matching manifest object, or a null JsonObject on mismatch.
// For arrays, matches by application_name == projectName.
// For objects, returns the object (application_name check is permissive if field is absent).
JsonObject esp32OTA::_findManifestEntry(JsonDocument& doc, const char* projectName) {
    if (doc.is<JsonArray>()) {
        for (JsonObject obj : doc.as<JsonArray>()) {
            const char* name = obj["application_name"] | (const char*)nullptr;
            if (name && strcmp(name, projectName) == 0) return obj;
        }
        return JsonObject();
    }

    JsonObject obj = doc.as<JsonObject>();
    const char* name = obj["application_name"] | (const char*)nullptr;
    if (!name || strcmp(name, projectName) != 0) {
        if (_onError) _onError("", (int)ESP_ERR_INVALID_ARG);
        return JsonObject();
    }
    return obj;
}

// ─── Download + write ─────────────────────────────────────────────────────────

bool esp32OTA::_downloadToPartition(const char* url, const esp_partition_t* target,
                                    bool isApp, esp_ota_handle_t* otaHandle,
                                    const char* sha256) {
    const char* label = isApp ? "app" : target->label;

    int contentLength;
    if (!_httpConnect(url, contentLength)) {
        if (_onError) _onError(label, -1);
        return false;
    }

    mbedtls_md_context_t sha_ctx;
    bool doSha = (sha256 != nullptr && sha256[0] != '\0');
    if (doSha) {
        mbedtls_md_init(&sha_ctx);
        mbedtls_md_setup(&sha_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
        mbedtls_md_starts(&sha_ctx);
    }

    uint8_t buf[1024];
    size_t written  = 0;
    size_t total    = (contentLength > 0) ? (size_t)contentLength : 0;
    int    remaining = contentLength;
    int64_t lastData = esp_timer_get_time();

    while (_client->connected() || _client->available()) {
        if (_client->available()) {
            lastData = esp_timer_get_time();
            int toRead = sizeof(buf);
            if (remaining > 0) toRead = min(toRead, remaining);

            int bytesRead = _client->readBytes(buf, toRead);
            if (bytesRead <= 0) continue;

            if (doSha) mbedtls_md_update(&sha_ctx, buf, bytesRead);

            esp_err_t err = ESP_OK;
            if (isApp) {
                err = esp_ota_write(*otaHandle, buf, bytesRead);
            } else {
                err = esp_partition_write(target, written, buf, bytesRead);
            }

            if (err != ESP_OK) {
                if (doSha) mbedtls_md_free(&sha_ctx);
                _client->stop();
                if (_onError) _onError(label, (int)err);
                return false;
            }

            written += bytesRead;
            if (remaining > 0) remaining -= bytesRead;
            if (_onProgress) _onProgress(label, written, total);

            if (remaining == 0 && contentLength > 0) break;
        } else {
            if (esp_timer_get_time() - lastData > ((int64_t)ESP32OTA_STALL_TIMEOUT_MS * 1000LL)) break;
            delay(1);
        }
    }

    _client->stop();

    if (written == 0) {
        if (doSha) mbedtls_md_free(&sha_ctx);
        if (_onError) _onError(label, (int)ESP_ERR_INVALID_SIZE);
        return false;
    }

    if (doSha) {
        uint8_t digest[32];
        mbedtls_md_finish(&sha_ctx, digest);
        mbedtls_md_free(&sha_ctx);
        if (!_verifySHA256(digest, 32, sha256)) {
            if (_onError) _onError(label, (int)ESP_ERR_INVALID_CRC);
            return false;
        }
    }

    return true;
}

// ─── Flash helpers ────────────────────────────────────────────────────────────

bool esp32OTA::_flashApp(const char* url, const char* sha256) {
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        if (_onError)        _onError("app", (int)ESP_ERR_NOT_FOUND);
        if (_onPartitionDone) _onPartitionDone("app", false);
        return false;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        if (_onError)        _onError("app", (int)err);
        if (_onPartitionDone) _onPartitionDone("app", false);
        return false;
    }

    bool ok = _downloadToPartition(url, update_partition, true, &ota_handle, sha256);

    if (ok) {
        err = esp_ota_end(ota_handle);
        if (err == ESP_OK) err = esp_ota_set_boot_partition(update_partition);
        ok  = (err == ESP_OK);
        if (!ok && _onError) _onError("app", (int)err);
    } else {
        esp_ota_abort(ota_handle);
    }

    if (_onPartitionDone) _onPartitionDone("app", ok);
    return ok;
}

bool esp32OTA::_flashDataPartition(const char* label, const char* url, const char* sha256) {
    const esp_partition_t* target = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, label);
    if (!target) {
        target = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, label);
    }
    if (!target) {
        if (_onError)        _onError(label, (int)ESP_ERR_NOT_FOUND);
        if (_onPartitionDone) _onPartitionDone(label, false);
        return false;
    }
    if (target->subtype != ESP_PARTITION_SUBTYPE_DATA_SPIFFS &&
        target->subtype != ESP_PARTITION_SUBTYPE_DATA_FAT) {
        if (_onError)        _onError(label, (int)ESP_ERR_INVALID_ARG);
        if (_onPartitionDone) _onPartitionDone(label, false);
        return false;
    }

    esp_err_t err = esp_partition_erase_range(target, 0, target->size);
    if (err != ESP_OK) {
        if (_onError)        _onError(label, (int)err);
        if (_onPartitionDone) _onPartitionDone(label, false);
        return false;
    }

    bool ok = _downloadToPartition(url, target, false, nullptr, sha256);
    if (_onPartitionDone) _onPartitionDone(label, ok);
    return ok;
}

// ─── Public OTA entry points ──────────────────────────────────────────────────

bool esp32OTA::checkForUpdate() {
    if (!_client || _manifestURL.isEmpty()) {
        if (_onError) _onError("", -1);
        return false;
    }

    String payload = _httpGetString(_manifestURL.c_str());
    if (payload.isEmpty()) {
        if (_onError) _onError("", -1);
        return false;
    }

    JsonDocument* doc  = _allocateJsonDocument(0);
    DeserializationError jsonErr = deserializeJson(*doc, payload);
    if (jsonErr) {
        if (_onError) _onError("", (int)ESP_ERR_INVALID_ARG);
        delete doc;
        return false;
    }

    const esp_app_desc_t* desc = esp_ota_get_app_description();
    if (desc->version[0] == '\0') {
        // Version was not embedded by the build system — semver comparison cannot work.
        // Set PROJECT_VER in CMakeLists.txt or via --build-property with arduino-cli.
        if (_onError) _onError("", (int)ESP_ERR_INVALID_VERSION);
        delete doc;
        return false;
    }

    JsonObject entry = _findManifestEntry(*doc, desc->project_name);
    if (entry.isNull()) {
        if (_onError) _onError("", (int)ESP_ERR_NOT_FOUND);
        delete doc;
        return false;
    }

    const char* manifestVersion = entry["version"] | (const char*)nullptr;
    if (!manifestVersion) {
        if (_onError) _onError("", (int)ESP_ERR_INVALID_ARG);
        delete doc;
        return false;
    }

    if (_semverCompare(manifestVersion, desc->version) <= 0) {
        delete doc;
        return false;
    }

    _availableVersion = manifestVersion;
    _releaseURL       = entry["release_url"] | "";
    serializeJson(*doc, _cachedManifest);

    if (_onAvailable) {
        _onAvailable(
            _availableVersion.c_str(),
            entry["application_name"] | "",
            _releaseURL.c_str()
        );
    }

    delete doc;
    return true;
}

// Uses manifest cached by checkForUpdate(); skips version re-check.
bool esp32OTA::execOTA() {
    if (_cachedManifest.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, _cachedManifest) != DeserializationError::Ok) return false;
    return _execManifest(doc, true);
}

bool esp32OTA::execOTA(const char* url) {
    setManifestURL(url);
    if (!checkForUpdate()) return false;
    return execOTA();
}

bool esp32OTA::execOTA(JsonDocument& doc) {
    return _execManifest(doc, true);
}

bool esp32OTA::flashPartition(const char* partitionLabel, const char* url) {
    if (_isBlocked(partitionLabel)) {
        if (_onError) _onError(partitionLabel, (int)ESP_ERR_INVALID_ARG);
        return false;
    }
    return _flashDataPartition(partitionLabel, url, nullptr);
}

// ─── Manifest execution ───────────────────────────────────────────────────────

bool esp32OTA::_execManifest(JsonDocument& doc, bool forceUpdate) {
    const esp_app_desc_t* desc = esp_ota_get_app_description();
    JsonObject entry = _findManifestEntry(doc, desc->project_name);

    if (entry.isNull()) {
        if (_onComplete) _onComplete(false);
        return false;
    }

    if (!forceUpdate) {
        const char* ver = entry["version"] | (const char*)nullptr;
        if (!ver || _semverCompare(ver, desc->version) <= 0) {
            if (_onComplete) _onComplete(false);
            return false;
        }
    }

    JsonArray binaries = entry["binaries"];
    if (binaries.isNull()) {
        if (_onError)   _onError("", (int)ESP_ERR_INVALID_ARG);
        if (_onComplete) _onComplete(false);
        return false;
    }

    // Locate app entry
    const char* appUrl    = nullptr;
    const char* appSha256 = nullptr;
    for (JsonObject bin : binaries) {
        const char* part = bin["partition"] | (const char*)nullptr;
        if (part && strcmp(part, "app") == 0) {
            appUrl    = bin["url"]    | (const char*)nullptr;
            appSha256 = bin["sha256"] | (const char*)nullptr;
            break;
        }
    }

    // App always flashed first; abort entire sequence on failure
    if (appUrl) {
        if (_isBlocked("app")) {
            if (_onError)   _onError("app", (int)ESP_ERR_INVALID_ARG);
            if (_onComplete) _onComplete(false);
            return false;
        }
        if (!_flashApp(appUrl, appSha256)) {
            if (_onComplete) _onComplete(false);
            return false;
        }
    }

    // Remaining partitions in manifest order; continue on individual failures
    bool allSuccess = true;
    for (JsonObject bin : binaries) {
        const char* part   = bin["partition"] | (const char*)nullptr;
        if (!part || strcmp(part, "app") == 0) continue;

        const char* url    = bin["url"]    | (const char*)nullptr;
        const char* sha256 = bin["sha256"] | (const char*)nullptr;

        if (!url) {
            if (_onError) _onError(part, (int)ESP_ERR_INVALID_ARG);
            allSuccess = false;
            continue;
        }
        if (_isBlocked(part)) {
            if (_onError)        _onError(part, (int)ESP_ERR_INVALID_ARG);
            if (_onPartitionDone) _onPartitionDone(part, false);
            allSuccess = false;
            continue;
        }
        if (!_flashDataPartition(part, url, sha256)) allSuccess = false;
    }

    if (_onComplete) _onComplete(allSuccess);
    if (allSuccess)  esp_restart();
    return allSuccess;
}
