# PocketBook — Build Plan (e-reader firmware, Folio base)

> A feature-complete, rock-solid **reading** firmware for the **Xteink X3/X4 e-reader**
> (ESP32-C3, no PSRAM, ~360 KB heap), built on Folio, cherry-picking the best of the CrossPoint
> fork family — clean OSS repo, automated release CI/CD, and a **never-brick** guarantee.
> **Preserve Folio's flow/UX above all.**
>
> Anchors: PocketBook builds green (`firmware.bin` 5.7 MB, **Flash 90.2% — ~645 KB headroom**),
> dual-OTA partitions (`app0`/`app1`) exist, rebrand done (name/logo/OTA→`jjc6676/pocketbook`).
> Flash is the **binding constraint** — every feature is judged against ~645 KB.

---

## 1. Feature audit & consolidation

Audited 5 forks (CrossInk, papyrix, crosspet, crosspoint-cjk, Inx) → 43 features. Flash-cost legend
(vs the 645 KB headroom): **tiny** <5 KB · **small** 5–20 KB · **medium** 20–80 KB · **large** >80 KB.

### ✅ TOP PORT — reading-first, low-risk, fit Folio's UX (do these)
| Feature | Source | Folio? | Flash | Difficulty |
|---|---|---|---|---|
| **Force paragraph indents** (when publisher CSS omits) | CrossInk | No | tiny | easy |
| **Strikethrough + `<hr>` section breaks** | CrossInk | Partial | small | moderate |
| **Arabic script shaping** (contextual forms, Lam-Alef, RTL) — Folio shows disconnected glyphs today | papyrix | No | small code + font glyphs | moderate |
| **Thai rendering** (clusters, vowel reorder, marks) | papyrix | No | small code + font glyphs | moderate |
| **Dual primary/supplement glyph-fallback fonts** (Latin + SD CJK/Cyrillic) | crosspet | Partial | small | moderate |
| **Per-book render modes + Safe-Mode OOM fallback ladder** ("opens degraded" vs "won't open") | CrossInk | Partial | medium | hard |
| **Memory-hardening patches** (grayscale-only JPEG, heap pre-checks, abort propagation, IRAM hot paths) | papyrix | Partial | tiny | moderate |
| **Minimal per-book reading stats + "End of book" screen** (lightweight tracker only) | Inx/CrossInk | No | medium | moderate |
| **Charset-driven sparse baked CJK UI font** (build-time technique, behind a flag) | cjk | No | ~54 KB if shipped | moderate |
| **CJK UI translations** (ZH-Hans/ZH-Hant/JA) — only as a package with the CJK font | cjk | Partial | small | easy |

### ⛔ REDUNDANT — Folio already does it (do **not** port)
Bionic Reading (= Folio's **Focus Reading**, same 43% bold-prefix algo), Arena allocator, Calibre
connect, Wi-Fi hotspot, embedded EPUB images, batch thumbnails (Folio's lazy CoverPrefetcher),
CJK line-breaking, streaming CSS parser, Lyra theme, 5-tab shell. Pulling these = duplicate
subsystems + wasted flash (the CJK external font engine is the worst — a *second* font path).

### ⚠️ RISKY — heavy / scope-adjacent / new failure modes (defer behind explicit decision)
Clippings/highlights export (~1,650 LOC), full reading-stats subsystem (~2,800 LOC, new formats),
text annotations (~1,265 LOC, selection UI in the hot paging path), **BLE page-turner** (NimBLE
~50 KB RAM, shares radio with Wi-Fi — kills reading quality on no-PSRAM C3), SD recovery firmware
(belongs in safety §2, not a casual pull).

### 🚫 OUT-OF-SCOPE — anti-mission / dangerous (hard pass)
Virtual pet, games suite, weather, flashcards/SRS, Pomodoro/clock/badge, **ESP-NOW nearby-sync**
(unauthenticated wireless write surface), Guide Dots (gimmick in the justification hot path), full
remap matrix, named presets, **mini-app launcher**, and papyrix's **Image Viewer that converts
JPEG/PNG→BMP *in place*, deleting the original** (data loss — disqualifying). These are the exact
"reader, not a PDA" surface Folio's SCOPE.md rejects; even toggled off they still consume the 645 KB.

**Format breadth (papyrix):** FB2 (~2,200 LOC; big in Russian/Slavic libraries) = **port-if-demand**.
Markdown (MD4C) = out-of-scope (dev/notes format, thin reader value). **X3 panel support + turbo
LUTs** = **port-if PocketBook wants X3 reach** (one coordinated hardware effort).

### Flash-budget verdict
**Porting everything blows 645 KB decisively.** The constraint forces *surgical* porting — the TOP
PORT set fits; everything else must be gated, demand-driven, or declined.

### Recommended consolidation set (in order — additive, low-risk, never disturbs reading flow)
1. **Force paragraph indents** — 1 bool + 2-line branch. Lowest-risk/highest-value typography fix.
2. **Strikethrough + `<hr>`** — restores publisher content Folio drops.
3. **Arabic shaping** — the single clearest reading-quality gap (budget the glyph coverage).
4. **Dual glyph-fallback font model** — graft the *model* onto Folio's `.cpfont` loader.
5. **Per-book render modes + Safe-Mode ladder** — the resilience win.
6. **Memory-hardening patches** — cherry-pick specific techniques where Folio's are weaker.
7. **Thai rendering** — same shape as Arabic, smaller base.
8. **Minimal per-book stats + End-of-book screen** — tracker only; defer the heavy Stats tab.

**Gated/demand-driven:** CJK UI (sparse-font technique + ZH/JA YAML, ship as a package), X3 hardware
(+turbo LUTs), FB2. **None of these change Folio's nav/tab model or reading flow** — they slot into
existing render/font/settings paths.

---

## 2. Never-brick safety architecture (X4)

**Guarantee:** a USB-locked X4 must never become permanently unbootable — no matter what firmware is
flashed or when power is lost. Folio's substrate is strong; the **automatic failover is missing**.

### Already present (good substrate)
Dual-partition A/B (`ota_0`/`ota_1` + `otadata` + coredump) · full pre-flash **SHA256 + ESP-image
validation on the SD path** (streamed, TOCTOU-safe) · size-bounds check · **writes target the
inactive slot only** (running firmware never erased) · **`otadata` switch is the last step**, and the
otadata writer is **power-loss-aware** (two-slot, CRC32, seq) · panic capture + **reader crash-loop
guard** + **on-device recovery mode** (BTN_UP+power → SD update UI) · watchdog fed during flash.

### Gaps (why a brick is still possible today)
- **No rollback discipline** — image committed as `IMG_NEW`, never `PENDING_VERIFY`; stock bootloader
  lacks rollback and we can't replace it on a locked unit. **A boots-then-bricks image (hangs after
  `setup()`) is committed permanently with no auto-revert.** A/B hardware exists; failover doesn't.
- **Two divergent write paths** — OTA (`esp_https_ota`) skips the strong SHA256 pre-flight that only
  the SD path runs (a stale comment claims a unity that doesn't exist).
- **No recovery/factory partition** — table is full to 16 MB; recovery combo only works if the bad
  image boots far enough to read buttons.
- **No brownout/power gating before flash**; **no defense against flashing both slots bad**;
  **no signature/authenticity** (OTA over hardcoded URL with `skip_cert_common_name_check=true`).
- **No enforced "OTA + recovery must exist" invariant** — a sibling fork (Papyrix) already bricked
  locked units by removing OTA. Today the guarantee rests on prose, not a build check.

### Prioritized safeguards
**P0 — must ship (closes the unrecoverable paths):**
- **P0.1 Unify both update paths through one verified writer** — OTA downloads to a cache file
  (`HttpDownloader::downloadToFile` exists) → `flashFromSdPath`; every byte that can reach `otadata`
  goes through the same SHA256/segment/checksum + power-loss-aware switch. Kills the "weaker path" bug class.
- **P0.2 App-space A/B rollback (self-healing even with the stock bootloader)** — write new entry as
  **pending** + NVS `{slot, seq, boot_attempts}`; bump `boot_attempts` early in `setup()`; **confirm**
  on first successful frame / reaching Home; if attempts exceed ~3 without confirm, **rewrite otadata
  back to the other slot**. This is the automatic failover the bootloader can't give us.
- **P0.3 Enforce "OTA + recovery always exist" as a CI/build invariant** — `static_assert`/required
  symbol so neither is dead-stripped; CI checks the partition table has 2 app slots + otadata; host
  test that `validateImageFile` rejects corrupt / accepts good. Document as load-bearing in SCOPE/GOVERNANCE.

**P1 — strongly recommended:**
- **P1.1 Immutable recovery/factory partition** (~1.5–2 MB, shrink the app slots) running only the SD
  update UI; jump to it after N failed boots — survives an early-`setup()` panic. Flash-layout
  migration; coordinate with the web flasher; test hard on real X3/X4.
- **P1.2 Brownout/power gating** — refuse flashing on low battery / no USB (BatteryMonitor exists);
  confirm `CONFIG_ESP_BROWNOUT_DET`; **read back otadata CRC** before reporting success.

**P2 — defense in depth:**
- **P2.1 Authenticate images** — sign releases (Ed25519/ECDSA or Secure Boot v2 verified *in app
  code*); embed public key; `validateImageFile` refuses unsigned/foreign images; drop
  `skip_cert_common_name_check` / pin the host.
- **P2.2 Protect the last-known-good slot** (refuse overwriting it while running slot unconfirmed) +
  flash-task watchdog (hung SD read aborts → reboot into still-good slot).

> Together: P0.1 = one audited path to otadata · P0.2 = boots-then-bricks auto-reverts · P0.3 = escape
> hatches can't be deleted · P1.1 = a route back even when the app won't run · P1.2 = shrinks
> power-loss windows · P2 = only authentic firmware committed, last-good never destroyed. **That set
> closes every identified path to a permanent brick on a locked X4.**

---

## 3. Repository structure (clean, scalable, contributor-friendly)

Folio's layout is already solid; keep it and add OSS-hygiene files.

```text
pocketbook/
├─ .github/  workflows/ (ci.yml · release.yml · release-please.yml) · ISSUE_TEMPLATE/ · PR template
├─ src/      activities/ · network/ · stores/ · components/ · images/
├─ lib/      Epub/ · GfxRenderer/ · ZipFile/ · I18n/ · EpdFont/ · MiniBidi/ …
├─ open-x4-sdk/   HAL (FreeInk SDK) — submodule
├─ third_party/   vendored deps (Boost.PFR)
├─ scripts/  gen_i18n · gen_theme · build_html · convert_icon · patch_jpegdec …
├─ test/     host unit tests (CMake + gtest) — the safety net
├─ docs/     USER_GUIDE · webserver · file-formats · contributing/ · RELEASING.md
├─ data/     web UI assets
├─ partitions.csv  platformio.ini
├─ README · LICENSE · SCOPE · GOVERNANCE
└─ + ADD: CHANGELOG.md · CONTRIBUTING.md · SECURITY.md · CODE_OF_CONDUCT.md
```
**Add for PocketBook:** `CHANGELOG.md` (Keep-a-Changelog), `SECURITY.md` (real web/OTA attack
surface), `CONTRIBUTING.md` (bake in the **Windows build gotchas**: PowerShell-not-MSYS,
`PYTHONUTF8=1`, `intelhex`, pioarduino fork), `CODE_OF_CONDUCT.md`, issue/PR templates,
`docs/RELEASING.md`. Keep internal `CrossPoint*` identifiers — not user-visible, renaming is churn.

---

## 4. CI/CD — build + publish to GitHub Releases

**Today:** `release.yml` builds `-e gh_release` on tag push and uploads *artifacts* (not a Release);
`ci.yml` runs clang-format-21 + cppcheck + build + unit tests on PRs.

**Two-workflow pipeline:**
- **`ci.yml` (PRs/main) — the gate.** Keep the 4 gates; aggregate into one required `result` job.
  Nothing merges red. **Validate tag format** (`vX.Y.Z` regex) before any release build.
- **`release.yml` (tag `v*`) — build + publish.** checkout `--recursive` → install **pioarduino**
  PlatformIO (`...pioarduino/platformio-core...v6.1.19.zip`) → cache `~/.platformio` → `pio run -e
  gh_release` (one binary covers **X3+X4**; optionally `-e slim`) → `sha256sum`→`checksums.txt` →
  **publish** with `softprops/action-gh-release@v2` attaching `firmware.bin`, `bootloader.bin`,
  `partitions.bin`, `.elf`, `.map`, `checksums.txt` + generated notes. This Release is exactly what
  OTA (`jjc6676/pocketbook/releases/latest`) pulls.

**Tooling for the "commit + version" half:**
| Option | Role | Fit |
|---|---|---|
| `softprops/action-gh-release` | attach assets + create Release | **Core — yes** |
| **`release-please`** | from Conventional Commits → version-bump PR + CHANGELOG + tag | **Recommended** for the "commit" half |
| raw `gh release create` | scripted fallback | ok |

**Recommended:** `release-please` (version bump + changelog commit + tag) → `release.yml` +
`softprops/action-gh-release` (build + publish). Satisfies *"builds, commits, publishes"* end-to-end.

**Other CI practices to adopt:** reusable `workflow_call` build (dev & release binaries byte-identical
except naming) · embed version+short-SHA in artifact names (traceability) · matrix-build variants from
PlatformIO envs · CI gate must be green before a Release attaches · publish SHA256 checksums.

> ⚠️ **Don't commit `firmware.bin` into git** — binaries belong on the Releases page (and are what OTA
> pulls), not in history. "Commit" = the version bump. One place to steer away from a literal reading.

---

## 5. Extras / quality-of-life (make it the *best* reader, not just complete)

**Reading polish (small, high-leverage):** Force-indents + strikethrough/`<hr>` (§1) · Arabic/Thai
correctness (§1) · finish Folio's **dictionary lookup** groundwork (inline, offline) · coarse CSS
**heading-size fidelity** (`h1..h6`→multiplier off the single loaded font — closes the biggest
perceived-quality gap cheaply) · minimal reading stats + End-of-book screen.

**Trust & safety as a feature:** signed releases + on-device **"verify after update"** first-boot
screen · **board-ID check before flashing** (refuse X3 binary on X4 — complements papyrix's I2C probe)
· **auto-backup settings/library/bookmarks before any flash** (restorable file) · RC/nightly release
channels (cautious users stay on stable) · document the ROM-bootloader (BOOT/EN-strap → esptool)
recovery path prominently for non-locked units.

**Project quality:** the host **gtest harness** is the differentiator — grow it with every fix (crafted
malformed EPUB/PNG/ZIP fixtures for the §audit bugs) · clean docs + CONTRIBUTING with the real build
recipe · SECURITY.md.

---

## Suggested first milestone (P0 cut)
1. Land the confirmed **security fixes** from the foundation audit (uzlib flag, ZIP overflow, PNG
   bit-depth, web-server auth/path lockdown) — see [POCKETBOOK_FOUNDATION.md](POCKETBOOK_FOUNDATION.md).
2. **Safety P0.1–P0.3** (unified verified flash path + app-space A/B rollback + CI invariants).
3. **CI/CD**: `release.yml` → real GitHub Release w/ `firmware.bin` + checksums; `release-please`.
4. First two **TOP PORT** features (force-indents, Arabic shaping) as the proof-of-flow for porting.
5. Repo hygiene files + flash to your X4 and dogfood.
