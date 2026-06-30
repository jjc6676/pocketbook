#include "RollbackGuard.h"

#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <nvs.h>

#include <cstring>

#include "OtaBootSwitch.h"

namespace rollback_guard {

namespace {

constexpr char kNvsNamespace[] = "rollback";
constexpr char kNvsKey[] = "rec";

// Read the persisted record. Returns true only if a blob exists, is the right
// size, and passes the magic + version check. On false `out` is left zeroed so
// callers can treat "no/invalid record" as "nothing pending".
bool readRecord(RollbackRecord& out) {
  std::memset(&out, 0, sizeof(out));

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    // ESP_ERR_NVS_NOT_FOUND just means the namespace was never written.
    if (err != ESP_ERR_NVS_NOT_FOUND) LOG_ERR("RBCK", "nvs_open(ro) failed: %s", esp_err_to_name(err));
    return false;
  }

  RollbackRecord rec;
  size_t len = sizeof(rec);
  err = nvs_get_blob(handle, kNvsKey, &rec, &len);
  nvs_close(handle);

  if (err != ESP_OK || len != sizeof(rec)) {
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
      LOG_ERR("RBCK", "nvs_get_blob failed: %s", esp_err_to_name(err));
    }
    return false;
  }
  if (rec.magic != kMagic || rec.version != kVersion) {
    LOG_ERR("RBCK", "record magic/version mismatch (magic=0x%08x ver=%u)", static_cast<unsigned>(rec.magic),
            rec.version);
    return false;
  }

  out = rec;
  return true;
}

// Persist the record and commit synchronously. Returns true on success.
bool writeRecord(const RollbackRecord& rec) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    LOG_ERR("RBCK", "nvs_open(rw) failed: %s", esp_err_to_name(err));
    return false;
  }

  err = nvs_set_blob(handle, kNvsKey, &rec, sizeof(rec));
  if (err != ESP_OK) {
    LOG_ERR("RBCK", "nvs_set_blob failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return false;
  }

  err = nvs_commit(handle);  // synchronous: the increment must survive a hang
  nvs_close(handle);
  if (err != ESP_OK) {
    LOG_ERR("RBCK", "nvs_commit failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

// Subtype of the currently-running app partition (OTA_0/1). Read-only — does
// NOT call esp_ota_get_state_partition (which would re-verify our patched image
// and fail).
uint8_t runningSubtype() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running ? static_cast<uint8_t>(running->subtype) : 0;
}

}  // namespace

bool markPending(uint8_t dest_subtype, uint8_t prev_subtype, uint32_t seq) {
  RollbackRecord rec;
  std::memset(&rec, 0, sizeof(rec));
  rec.magic = kMagic;
  rec.version = kVersion;
  rec.pending = 1;
  rec.boot_attempts = 0;
  rec.target_subtype = dest_subtype;
  rec.seq = seq;
  rec.prev_subtype = prev_subtype;

  const bool ok = writeRecord(rec);
  if (ok) {
    LOG_INF("RBCK", "markPending: target=0x%02X prev=0x%02X seq=%u", dest_subtype, prev_subtype,
            static_cast<unsigned>(seq));
  }
  return ok;
}

BootDecision onBoot() {
  RollbackRecord rec;
  if (!readRecord(rec) || rec.pending == 0) {
    return BootDecision::Proceed;
  }

  const uint8_t running = runningSubtype();
  if (running != rec.target_subtype) {
    // The otadata switch never took effect (e.g. power loss between markPending
    // and the switch, or a manual reflash). We're not running the trial slot,
    // so there's nothing to count — drop the stale record and proceed.
    LOG_INF("RBCK", "pending target=0x%02X but running=0x%02X; clearing stale trial", rec.target_subtype, running);
    clearPending();
    return BootDecision::Proceed;
  }

  rec.boot_attempts++;
  LOG_INF("RBCK", "boot attempt %u/%u on trial slot 0x%02X", rec.boot_attempts, kMaxBootAttempts, rec.target_subtype);

  if (rec.boot_attempts > kMaxBootAttempts) {
    // The trial slot has failed to reach a usable screen too many times. Revert
    // to the previous confirmed-good slot via the same power-loss-aware otadata
    // writer used for flashing — never esp_ota_set_boot_partition (it would
    // re-verify our patched image and fail).
    LOG_ERR("RBCK", "trial slot 0x%02X exceeded %u attempts; reverting to 0x%02X", rec.target_subtype,
            kMaxBootAttempts, static_cast<unsigned>(rec.prev_subtype));

    const esp_partition_t* prev = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, static_cast<esp_partition_subtype_t>(rec.prev_subtype), nullptr);
    if (prev && ota_boot::switchTo(prev)) {
      // Clear pending BEFORE the restart so we don't ping-pong: the prev slot
      // was confirmed-good when it last ran, so it gets a clean (non-trial)
      // boot. Guarantee is "revert to last confirmed-good slot," not "retry."
      clearPending();
      return BootDecision::Reverted;
    }

    // Revert could not be performed (prev partition missing or switch failed).
    // Clear the record so we don't spin forever trying to revert each boot, and
    // let this slot keep booting — it's the best we can do.
    LOG_ERR("RBCK", "revert failed (prev=%p); clearing record and proceeding", static_cast<const void*>(prev));
    clearPending();
    return BootDecision::Proceed;
  }

  // Commit the incremented counter SYNCHRONOUSLY before returning, so a hang
  // later in setup() can't lose this attempt and stall the trial forever.
  writeRecord(rec);
  return BootDecision::Proceed;
}

void confirmRunningSlotGood() {
  RollbackRecord rec;
  if (!readRecord(rec) || rec.pending == 0) {
    return;  // nothing on trial — cheap no-op on every Home/Reader entry
  }
  rec.pending = 0;
  rec.boot_attempts = 0;
  if (writeRecord(rec)) {
    LOG_INF("RBCK", "confirmed running slot good; trial cleared");
  }
}

void clearPending() {
  RollbackRecord rec;
  if (!readRecord(rec)) {
    return;  // no valid record to clear
  }
  if (rec.pending == 0) {
    return;
  }
  rec.pending = 0;
  rec.boot_attempts = 0;
  writeRecord(rec);
}

}  // namespace rollback_guard
