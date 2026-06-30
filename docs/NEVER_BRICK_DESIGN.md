# Never-Brick: App-Space A/B Auto-Rollback — Design

> Guarantee: a firmware that flashes/OTAs successfully but then **boots-then-bricks**
> (hangs after `setup()`, never reaches a usable screen) **auto-reverts** to the last
> confirmed-good slot — without the stock bootloader's help.

## The constraint that shapes everything
This fork **cannot use stock ESP-IDF rollback.** `esp_ota_set_boot_partition` calls
`esp_image_verify`, which rejects these patched X3/X4 images (bogus efuse-blk-rev errors —
see `OtaBootSwitch.h` + the `--wrap=bootloader_common_check_efuse_blk_validity` hack in
`platformio.ini`). That's why both flash paths already hand-write `otadata` raw via
`ota_boot::switchTo()`. **Rollback must likewise be enforced in app space by rewriting
`otadata` — never via `esp_ota_set_boot_partition` / `esp_ota_mark_app_valid*`** (they
re-trigger image verification). The NVS pending flag *is* our "mark valid."

## Persistence: NVS (not RTC_NOINIT, not SD-JSON)
The counter must survive power loss (rules out `RTC_NOINIT_ATTR`) and be readable before
`Storage.begin()` and even with no/corrupt SD (rules out SD-JSON). Use the **NVS C API**
(`nvs_open`/`nvs_get_blob`/`nvs_set_blob`/`nvs_commit`; `nvs_flash_init()` already runs
before `setup()`). Namespace `"rollback"`, key `"rec"`:
```c
struct RollbackRecord {     // versioned blob
  uint32_t magic;           // 'RLBG' 0x524C4247 — set vs erased
  uint8_t  version;         // = 1
  uint8_t  pending;         // 1 = a freshly-flashed slot is on trial
  uint8_t  boot_attempts;   // ++ each boot while pending
  uint8_t  target_subtype;  // OTA_0/1 of the slot under trial
  uint32_t seq;             // otadata seq written for the trial slot
  uint32_t prev_subtype;    // the previous-good slot to revert TO
};
constexpr uint8_t kMaxBootAttempts = 3;
```

## P0.1 — Unify OTA + SD flash through ONE verified writer (land FIRST)
Today the **OTA path** (`OtaUpdater::installUpdate`, `OtaUpdater.cpp:146-222`) uses
`esp_https_ota_*` directly — it **skips** `validateImageFile`'s SHA256/segment gate AND calls
the forbidden `esp_ota_set_boot_partition`. The **SD path** (`FirmwareFlasher::flashFromSdPath`)
is the safe one. Rewrite the OTA path to: `HttpDownloader::downloadToFile(url, "/.crosspoint/
ota_stage.bin", ...)` (already exists, wolfSSL-backed) → `flashFromSdPath(stage, …, alreadyValidated=false)`
→ delete stage. Add an SD free-space pre-check (~5.7 MB). Drops `esp_https_ota` entirely
(also frees flash). This makes `markPending` (below) cover both entry points for free.

## P0.2 — App-space rollback
New module `src/network/RollbackGuard.{h,cpp}` (next to `OtaBootSwitch`, which it reuses).

| Step | Hook | Action |
|---|---|---|
| **markPending** | `FirmwareFlasher.cpp:304`, *before* `ota_boot::switchTo(dest)` | resolve running=prev via `esp_ota_get_running_partition()` (read-only, no verify); write NVS `{pending=1, target=dest, prev=running, seq, attempts=0}` **before** the otadata switch |
| **onBoot** | `main.cpp:345`, after `HalSystem::begin()`, before `gpio.begin()` | read NVS; if `!pending` → Proceed. If running-slot ≠ target (switch never took) → clear, Proceed. Else `attempts++`; if `> kMaxBootAttempts` → **revert**; else commit, Proceed |
| **revert** | inside onBoot | `ota_boot::switchTo(prev_good)` (reuse power-loss-aware writer), clear pending **before** `ESP.restart()` |
| **confirm** | top of `LibraryActivity::onEnter` **and** `ReaderActivity::onEnter` | if pending: set `pending=0, attempts=0`, commit. Reaching Home/Reader = full boot chain succeeded |

Ordering (markPending NVS *before* otadata): power-loss between them → next boot still on old
slot, onBoot sees target≠running → clears (no harm). After switch → trial begins as intended.
Do **NOT** hook confirm on Boot/Crash/Message activities (a half-dead boot must never confirm).

## P0.3 — Invariants (can't be removed/dead-stripped)
1. CI step (`ci.yml`): grep `partitions.csv` still has `app0/ota_0`, `app1/ota_1`, `otadata`.
2. Link anchor in `main.cpp` force-keeping `flashFromSdPath`, `RollbackGuard::onBoot`,
   `ota_boot::switchTo`; CI grep that the recovery UI + onBoot are still referenced.
3. Host gtest `test/rollback/`: seq math, attempts-reverts-at-N+1, confirm-clears-pending,
   power-loss-before-switch path, corrupt-image rejected by `validateImageFile`.

## On-device test protocol (REQUIRED before trusting it)
Safe to test on a **non-USB-locked** X4 (USB flashing = ground-truth recovery).
1. Flash good build → reach Home (commits slot A).
2. Build a **sabotaged** firmware (`while(true){}` at end of `setup()`, or `return;` after
   `setupDisplayAndFonts` so Home never loads). Flash via SD UI → lands slot B.
3. Over serial: boot 1→`attempts=1` hang, power-cycle, boot 2→`=2`, boot 3→`=3`, boot 4→
   `4>3` → "REVERT to ota_0" → `ESP.restart()` → **boots good slot A → Home**. NVS `pending=0`.
4. Repeat with a panic variant (`*(int*)0=1;`) and (after P0.1) the OTA path.

**Watch for:** confirm-too-early (only hook Home/Reader onEnter), counter not committed before
a hang (commit synchronously in onBoot), revert ping-pong (clear pending before restart; the
prev slot was confirmed-good when it ran). Guarantee = "revert to last *confirmed-good* slot,"
not "make a bad slot good."

## Sequence: P0.1 → P0.2 → P0.3. Reuses `ota_boot::switchTo` for both flash + revert; adds one
## NVS blob + four call sites; touches nothing in the EPUB/display/activity engines.
