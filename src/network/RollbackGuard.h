#pragma once

#include <cstdint>

// App-space A/B auto-rollback ("never-brick"). A freshly flashed/OTA'd slot is
// put "on trial": markPending() records the switch in NVS *before* otadata is
// rewritten, onBoot() counts boot attempts, and the slot is only confirmed good
// once the boot chain actually reaches a usable screen (Home or Reader). If it
// boots-then-bricks (hangs in setup(), never reaches a screen) for too many
// attempts, onBoot() reverts to the previous confirmed-good slot.
//
// WHY THIS LIVES IN APP SPACE (do not "simplify" to esp_ota_* rollback):
// these X3/X4 images are patched (see OtaBootSwitch.h + the
// --wrap=bootloader_common_check_efuse_blk_validity hack in platformio.ini).
// The running ESP-IDF's esp_image_verify rejects them with bogus efuse-blk-rev
// errors. So esp_ota_set_boot_partition / esp_ota_mark_app_valid_cancel_rollback
// / esp_ota_get_state_partition all FAIL on these images — they re-trigger that
// verification. Both the flash and the revert therefore go through
// ota_boot::switchTo(), which hand-writes otadata raw. The NVS `pending` flag
// below *is* our "mark valid": clearing it (confirmRunningSlotGood) is what
// declares the running slot trustworthy.
//
// Persistence is NVS (not RTC_NOINIT — must survive power loss; not SD-JSON —
// must be readable before Storage.begin() and with no/corrupt SD). nvs_flash
// is already initialised before setup() runs.
namespace rollback_guard {

// Versioned NVS blob (namespace "rollback", key "rec"). Validated on read.
struct __attribute__((packed)) RollbackRecord {
  uint32_t magic;          // kMagic when set vs erased
  uint8_t version;         // = kVersion
  uint8_t pending;         // 1 = a freshly-flashed slot is on trial
  uint8_t boot_attempts;   // ++ each boot while pending
  uint8_t target_subtype;  // OTA_0/1 subtype of the slot under trial
  uint32_t seq;            // otadata seq written for the trial slot
  uint32_t prev_subtype;   // the previous-good slot subtype to revert TO
};

constexpr uint32_t kMagic = 0x524C4247u;  // 'RLBG'
constexpr uint8_t kVersion = 1;
constexpr uint8_t kMaxBootAttempts = 3;

// Outcome of onBoot(). Reverted means onBoot has already rewritten otadata to
// the previous-good slot and cleared pending — the caller MUST ESP.restart()
// immediately so the device comes up on that slot.
enum class BootDecision {
  Proceed,
  Reverted,
};

// Called by FirmwareFlasher right BEFORE ota_boot::switchTo(dest), so the NVS
// record lands before otadata is rewritten (ordering is load-bearing — see the
// power-loss analysis below). `dest` is the slot being flashed; `prev` is the
// currently-running slot to revert to (resolve via
// esp_ota_get_running_partition()). `seq` must be ota_boot::computeNextSeq(dest)
// so the recorded trial seq matches what switchTo() will commit. Returns false
// only if the record could not be written (NVS error) — caller may still flash,
// but the trial won't be tracked.
//
// Power-loss ordering: if power is lost between markPending and the otadata
// switch, the next boot is still on the OLD slot; onBoot sees target != running
// and simply clears pending (no harm). After the switch lands, the trial begins
// as intended.
bool markPending(uint8_t dest_subtype, uint8_t prev_subtype, uint32_t seq);

// Called EARLY in setup() (after HalSystem::begin(), before gpio/SD/display/
// WiFi). Increments the boot-attempt counter and commits to NVS SYNCHRONOUSLY
// before returning, so a hang later in setup() can't lose the increment. On the
// (kMaxBootAttempts+1)th attempt it reverts: ota_boot::switchTo(prev_good),
// clears pending, and returns Reverted (caller must ESP.restart()). Otherwise
// returns Proceed.
BootDecision onBoot();

// Called at the TOP of LibraryActivity::onEnter and ReaderActivity::onEnter —
// reaching either means the full boot chain succeeded. Clears the trial
// (pending = 0, attempts = 0) and commits. Never hook this on Boot/Crash/
// Message activities: a half-dead boot must never confirm itself good.
void confirmRunningSlotGood();

// Force-clear the trial record (pending = 0). Used by the revert path and as a
// safety reset; normal "this slot is good" clearing goes through
// confirmRunningSlotGood().
void clearPending();

}  // namespace rollback_guard
