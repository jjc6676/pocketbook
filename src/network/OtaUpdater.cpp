#include "OtaUpdater.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include "FirmwareFlasher.h"
#include "HttpDownloader.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/jjc6676/pocketbook/releases/latest";

// Where the OTA firmware is staged on the SD card before flashing. The OTA path
// now downloads to this file and hands it to the same verified flasher the SD
// update UI uses (validateImageFile -> raw partition write -> ota_boot::switchTo),
// instead of esp_https_ota (which skips our SHA256 gate and calls the forbidden
// esp_ota_set_boot_partition that re-verifies our patched image).
constexpr char kOtaStagePath[] = "/.crosspoint/ota_stage.bin";

size_t totalBytesReceived = 0;

esp_err_t event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  totalBytesReceived += event->data_len;
  LOG_DBG("OTA", "HTTP chunk: %d bytes (total: %zu)", event->data_len, totalBytesReceived);
  auto* parser = static_cast<ReleaseJsonParser*>(event->user_data);
  parser->feed(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  esp_err_t esp_err;
  ReleaseJsonParser releaseParser;

  esp_http_client_config_t client_config = {
      .url = latestReleaseUrl,
      .event_handler = event_handler,
      // 4096 holds the API response headers; the 32KB body streams through the
      // parser in chunks so RX needn't be larger. TX only carries our GET.
      // Both free before installUpdate, so smaller leaves it less fragmentation.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .user_data = &releaseParser,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  totalBytesReceived = 0;
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(client_handle, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return HTTP_ERROR;
  }

  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);
  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // Stage the firmware to the SD card, then flash it through the SAME verified
  // writer the SD update UI uses. This unifies both update entry points on
  // firmware_flash::flashFromSdPath, which runs the full image-integrity check
  // (header / segment table / XOR checksum / SHA256 trailer) and writes otadata
  // via ota_boot::switchTo — the only path X3/X4 patched images survive. As a
  // side effect, RollbackGuard::markPending (inside flashFromSdPath) now also
  // covers OTA updates for free.

  // A too-full card fails cleanly: downloadToFile below errors out and we never
  // flash (the staged file is validated by flashFromSdPath). No pre-check needed.
  Storage.mkdir("/.crosspoint");

  // Download with the same whole-percent progress throttle the old path used:
  // the render task's framebuffer work contends with TLS on a tight internal
  // arena, and e-ink can't repaint faster than a percent tick anyway.
  totalSize = otaSize;
  processedSize = 0;
  int lastReportedPct = -1;
  HttpDownloader::ProgressCallback progressCb = [&](size_t downloaded, size_t total) {
    processedSize = downloaded;
    if (total > 0) totalSize = total;
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
  };

  const HttpDownloader::DownloadError dlErr = HttpDownloader::downloadToFile(otaUrl, kOtaStagePath, progressCb);
  if (dlErr != HttpDownloader::OK) {
    LOG_ERR("OTA", "download failed: %d", static_cast<int>(dlErr));
    Storage.remove(kOtaStagePath);
    return HTTP_ERROR;
  }

  // alreadyValidated=false: let flashFromSdPath run its full integrity pass on
  // the freshly downloaded image before it touches otadata.
  const firmware_flash::Result flashRes =
      firmware_flash::flashFromSdPath(kOtaStagePath, nullptr, nullptr, /*alreadyValidated=*/false);

  // The staged copy is large; delete it whether the flash succeeded or not. The
  // image already lives in the OTA partition on success, and a failed flash
  // leaves nothing worth keeping.
  Storage.remove(kOtaStagePath);

  if (flashRes != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "flash failed: %s", firmware_flash::resultName(flashRes));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
