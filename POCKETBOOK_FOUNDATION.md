# PocketBook Foundation Report — Building on Folio

*Custom e-reader firmware for the Xteink X3/X4 (ESP32-C3, no PSRAM) · Base: Folio (fork of CrossPoint Reader) · 2026-06-29*

> Produced by a 23-agent profile + bug-hunt + adversarial-verify + synthesis pass over the
> Folio tree. Branch: `pocketbook-main`. GitHub: `jjc6676`.

---

## 1. Why Folio is the base
Folio is the only candidate combining real engineering momentum (1,168 commits / 543 in 90 days /
177 contributors vs Inx's 25) with the hard reading-specific machinery already solved: a streamed
low-RAM EPUB engine with a DP minimum-raggedness line breaker, Liang hyphenation (9 langs), UAX#9
BiDi, CJK kinsoku, NFC normalization — all inside a ~380KB heap, no PSRAM. One binary drives both
X3 (UC8253) and X4 (SSD1677) panels. It ships a **host unit-test harness** (CMake + gtest, ~85
tests, no Arduino dep) and a 4-gate CI. It already fixed several bugs Inx still has (KOSync TLS
verify, JPEG dimension bounds, ZIP `nameLen` guards, SAX OPF parsing that kills the `getImages`
VLA). Remaining work is sharply scoped and non-architectural.

## 2. Architecture map (key anchors)
- **Build/Test** — PlatformIO (pioarduino `platform-espressif32` 55.03.37), C++20, `-fno-exceptions`.
  5 `pre:` codegen scripts; host tests are a separate CMake project under `test/`.
  `platformio.ini:1-129`, `test/CMakeLists.txt:14-45`, `.github/workflows/ci.yml`.
- **Arch/State** — single-threaded `setup()/loop()` + 1 render task; Android-style Activity model
  with deferred nav; RAII `RenderLock`. Two persistence stacks: legacy `JsonSettingsIO` (non-atomic)
  and newer Boost.PFR `JsonStore<T>` (atomic temp+rename). `src/activities/ActivityManager.cpp`,
  `src/stores/util/PfrJson.h`, `lib/Memory/Arena.cpp`.
- **EPUB engine** — 3-stage streamed pipeline (ZIP→OPF/NCX SAX→`book.bin`; lazy per-chapter
  pagination→`sections/<n>.bin`; O(1) page turns). Box-model-lite: CSS `font-size`/`line-height`/
  `color`/`font-family`/`@font-face` are dropped. `lib/Epub/Epub.cpp:387`,
  `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`, `lib/Epub/Epub/ParsedText.cpp:466`.
- **Display/Themes** — FreeInk facade → `HalDisplay` → `GfxRenderer` (2500-line blitter) → `EpdFont`
  (DEFLATE fonts + SD `.cpfont`, shared glyph LRU). Themes codegen from `theme_schema.yaml`; a
  `.cptheme` is a zip of `theme.json` + `.cpfont`, mapping 9 FontRoles. `scripts/gen_theme.py`.
- **Network/Sync** — 3 transports: on-device web/WS/WebDAV file API (**zero auth**); KOSync/OTA over
  verified TLS (`esp_crt_bundle`); `HttpDownloader` over wolfSSL with **`setInsecure()`**.
  `src/network/CrossPointWebServer.cpp`, `WebDAVHandler.cpp`, `OtaUpdater.cpp`.
- **i18n/Text** — 26 lang YAMLs → `gen_i18n.py` → blob + offset table. UTF-8/NFC; RTL via MiniBidi
  (Hebrew-only, no Arabic shaping). `lib/I18n/I18n.cpp`, `lib/MiniBidi/minibidi.c`.

## 3. Build & host-test loop (Windows)
```powershell
# one-time
git submodule update --init --recursive
python -m pip install platformio intelhex pyyaml   # system Python 3.13 (pio lives in AppData/Roaming/.../Scripts)

# build firmware  ->  .pio/build/default/firmware.bin
pio run                         # dev (LOG_LEVEL=2, serial, git version)
pio run -e gh_release           # release

# static analysis (matches CI)
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high

# host unit tests (needs CMake + Ninja + a C++20 compiler — NOT yet installed on this box)
cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/test
ctest --test-dir build/test --output-on-failure -j

# format before pushing (CI gate, clang-format >= 21)
./bin/clang-format-fix
```
**Gotcha:** PlatformIO runs `esptool.py` with the *system* Python; image packaging fails with
`No module named 'intelhex'` unless intelhex is in that Python. Host tests need a C++ toolchain we
have not installed yet (P0 item).

## 4. Confirmed security/bug audit of our base

### CONFIRMED critical/high (adversarially verified)
| # | Sev | File:Line | Bug | Fix | Lineage |
|---|-----|-----------|-----|-----|---------|
| 1/7 | HIGH | `lib/ZipFile/ZipFile.cpp:381,436` | `uncompressedSize+1` wraps in 32-bit → `malloc(0)` then OOB write from untrusted EPUB | 64-bit size math + heap-bounded cap before malloc; bound inflate output | Inherited |
| 2 | HIGH | `lib/uzlib/src/uzlib_conf.h:19`; `tinflate.c:297,388` | `UZLIB_CONF_PARANOID_CHECKS=0` → `trans[]` OOB read on malformed Huffman | `-DUZLIB_CONF_PARANOID_CHECKS=1` | Inherited |
| 3 | HIGH | `lib/PngToBmpConverter/PngToBmpConverter.cpp:425` | IHDR `bitDepth` not validated vs `(colorType,bitDepth)` → heap OOB on any cover PNG | reject illegal pairs right after IHDR | Folio-introduced |
| 4 | HIGH | `src/network/CrossPointWebServer.cpp:134-184` | Web/WS/WebDAV file API has **zero auth** — any LAN/AP client gets full SD r/w/delete | PIN/token gate on all file + mutating endpoints | Inherited |
| 5 | HIGH | `CrossPointWebServer.cpp:490-520` | `/download` + file-list check only the **last** path segment → `/.crosspoint/*` creds enumerable/downloadable | route via `FsHelpers::normalisePath` + per-segment protected check | Partially inherited |
| 6 | HIGH | `CrossPointWebServer.cpp:627-663,1559-1618,1019-1085` | upload / WS-upload / delete build paths by raw concat, no `../` check → overwrite/delete `.crosspoint/settings.json` | same normalize + per-segment check; reject `/`,`\`,leading `.` | Partially inherited |

*(One claimed `/download` traversal variant was **refuted** by verification — false positive removed.)*

### Medium/low (selected, mostly unverified)
PNG `bitDepth=0`/palette-16 → divide-by-zero panic; unbounded OPF `itemIndex` deque + string
accumulation (DoS); OTA `skip_cert_common_name_check=true` + no firmware signature; `HttpDownloader`
`setInsecure()`; TocNcx/TocNav `uint8_t` depth wrap.

### Lineage
- **Still shared with Inx:** uzlib paranoid-off, ZIP overflow, partial web traversal, web no-auth,
  OTA hostname/signature gaps, `HttpDownloader setInsecure`, TocNcx depth.
- **Already fixed by Folio (vs Inx):** ZIP `nameLen` overflow, OPF `getImages` VLA (now SAX),
  JpegToBmp unchecked malloc + dimension bounds, KOSync TLS, OTA TLS chain verify.
- **Newly introduced by Folio:** PNG `bitDepth` gaps + div-by-zero, unbounded OPF deque/strings.
- **Audit gap:** the stores/deserialization surface returned a malformed result — re-audit needed.

## 5. What to port from Inx
Inx's value is **feature ideas, not code** — Folio already won architecture + security. Folio
already has OPDS, KOSync, WebDAV, themes/fonts, i18n/RTL, tabbed nav. Realistic borrow candidates
(verify absence first): **reading statistics / session tracking** (build on atomic `JsonStore<T>`),
and any richer library/shelf affordances Folio lacks. **Do NOT port** Inx ZIP/inflate/PNG/network
code — it carries the same-or-worse bugs.

## 6. PocketBook roadmap
**P0 — green build, host tests, fix confirmed crit/high (gate):**
1. Install host toolchain (CMake+Ninja+C++20), pin `CMAKE_GENERATOR=Ninja`, get `pio run` + `ctest` green.
2. Fix ZIP integer overflow (#1/#7) + host gtest with crafted `uncompressedSize=0xFFFFFFFF`.
3. `-DUZLIB_CONF_PARANOID_CHECKS=1` (#2).
4. Validate PNG `(colorType,bitDepth)` (#3 + div-by-zero) + malformed-IHDR gtest.
5. Lock down web/WebDAV file API (#4/#5/#6): PIN/token auth + `normalisePath` per-segment checks.
6. CI parity (clang-format-21, cppcheck, build, unit-tests).

**P1 — function & stability:** atomic EPUB progress save; unify persistence on `JsonStore<T>`;
cap OPF/TOC DoS; enum-range validation in `pfrjson::read`; tighten outbound TLS.

**P2 — differentiation:** firmware signature/authenticity; reading stats; coarse CSS heading-size
fidelity (cheap `h1..h6`→multiplier); widen host-test coverage.

**P3 — styles, themes, branding:** PocketBook theme + branding via `theme_schema.yaml`; branded
`.cptheme`; surface theme/font failures; i18n/RTL polish (sync docs, decide on Arabic shaping).

> **Guardrail for every feature:** does it invalidate the EPUB section cache (force re-pagination)?
> does it assume `malloc` succeeds? Preserve the Arena + `makeUniqueNoThrow` + fail-soft-to-null
> no-PSRAM strategy, the X3 driver, PanelDriver/facade split, shared glyph LRU, streamed SAX EPUB
> pipeline, and deferred-nav Activity model **as-is** — they are the hard parts Folio got right.
