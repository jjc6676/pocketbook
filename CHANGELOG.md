# Changelog

All notable changes to PocketBook are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versions follow [SemVer](https://semver.org/).

PocketBook is a custom firmware for the Xteink X3/X4 e-reader, forked from
[Folio](https://github.com/folio-etc/folio) (itself a fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)).

## [Unreleased]

## [0.1.0] — 2026-06-29

First PocketBook release. Branched from Folio with a focus on **function and speed first**.

### Added
- **Warm-on-open** — when the library selection settles on a book, a background worker
  pre-builds its resume-section cache so the first tap-to-open is near-instant. Font-safe
  (serialized via a cancellable render-lock) and best-effort (heap-gated, cancelled on tap).
- **Versioned firmware packaging** (`scripts/package_firmware.py`) — stamps every build into
  `dist/PocketBook-v<version>-<branch>-<sha>.bin` with a SHA256 sidecar.
- **Release CI/CD** — tagging `vX.Y.Z` builds and publishes a GitHub Release with `firmware.bin`
  + checksums; `-rc` tags publish as pre-releases.

### Changed
- **Rebranded** Folio → **PocketBook** (name across all 26 UI languages, boot logo, default
  theme label, OTA update source → `jjc6676/pocketbook`).
- **Instant library scrolling** — the cover cache now keeps a 3-page `{prev, cur, next}` window
  resident instead of clearing on every page move, so scrolling back to a recent page renders
  from RAM with zero SD I/O.
- **−403 KB flash** — dropped the unreachable built-in Literata 5/6 pt sizes (reader body uses
  8–18 pt); flash 90.2% → 83.9%, freeing headroom for features. (Compact UI captions now 8 pt.)

### Fixed
- **Security (untrusted files):** enabled uzlib bounds checks (OOB Huffman read), capped a ZIP
  integer overflow (`uncompressedSize+1` wrap → bad alloc + OOB write), and validate PNG
  `(colorType, bitDepth)` per spec (illegal pairs → heap OOB read / divide-by-zero).
- **Security (web file manager):** block `..` path traversal in `/download` and `/delete` so a
  LAN client can't read stored Wi-Fi/KOReader credentials or delete protected config.
- **Build:** `gen_theme.py` now runs under PlatformIO (it silently no-op'd on clean checkouts,
  breaking the build on a missing generated header).

[Unreleased]: https://github.com/jjc6676/pocketbook/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jjc6676/pocketbook/releases/tag/v0.1.0
