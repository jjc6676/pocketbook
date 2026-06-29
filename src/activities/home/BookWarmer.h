#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>

class GfxRenderer;

// Background "warm-on-open": when the library selection settles on a book, this
// pre-builds that book's resume section cache (sections/<n>.bin) off the render
// path so the first tap-to-open is a cache HIT instead of a synchronous build.
//
// Threading model (the font-safety constraint): the font glyph caches
// (SdFontGlyphCache / FontDecompressor) are single-threaded, render-task only.
// The warm worker therefore holds the existing RenderLock around the section
// build (text measurement) — serializing it with the render task so no glyph
// cache is touched concurrently. This is uncontended because warming only fires
// after an idle settle, and any input cancels it (the render task is idle then).
//
// Lifecycle mirrors CoverPrefetcher: start() spins up one worker; warm() hands it
// a path and signals; cancelInFlight() is a non-blocking cancel (tap-to-open);
// stop() cancels + JOINS the worker so its heap is fully freed before the reader
// allocates. Debounce state (noteSelectionChanged / shouldWarmNow) is owned by the
// caller's main loop — no extra timer task.
class BookWarmer {
 public:
  explicit BookWarmer(GfxRenderer& renderer) : renderer_(renderer) {}

  // Spin up the worker. Idempotent-safe to pair with stop(); call from onEnter.
  void start();
  // Cancel + JOIN the worker, then free its sync primitives. Idempotent and safe
  // when never started. Must run before the reader allocates (i.e. in onExit,
  // which replaceActivity runs before the reader's onEnter).
  void stop();

  // Hand the worker a book path to warm. No-op if already warming/warmed that
  // exact path (cache key dedupe). Clears the cancel flag for the new unit.
  void warm(const std::string& path);
  // Non-blocking cancel of any in-flight warm (sets cancel_, wakes the worker).
  // Call before goToReader so the render task gets an uncontended RenderLock.
  void cancelInFlight();

  // ---- Debounce (caller's main loop; no timer task) ----
  // Record that the highlighted selection changed at `nowMs`; arms a pending warm.
  void noteSelectionChanged(unsigned long nowMs);
  // True exactly once when a pending warm has settled (>= kWarmSettleMs since the
  // last change) and `rapidJumping` is false; clears the pending flag.
  bool shouldWarmNow(unsigned long nowMs, bool rapidJumping);

 private:
  // Skip warming below this free-heap floor: warming a section transiently
  // allocates EPUB/parse buffers, and starving the foreground UI is worse than a
  // cold first-open. ~100 KB leaves comfortable headroom on the ~360 KB heap.
  static constexpr size_t kMinHeapToWarm = 100 * 1024;
  // Idle settle before warming, so flicking through the grid doesn't warm every
  // tile in passing — only the book the user actually lands on.
  static constexpr unsigned long kWarmSettleMs = 500;

  static void taskTrampoline(void* ctx);
  void taskLoop();
  // The actual build: load the Epub, resolve the resume spine, hold RenderLock
  // around Section load-or-create. Best-effort; any failure returns silently.
  void warmPath(const std::string& path);

  GfxRenderer& renderer_;

  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t stateLock_ = nullptr;  // guards pendingPath_/hasPending_
  SemaphoreHandle_t signal_ = nullptr;
  SemaphoreHandle_t exited_ = nullptr;
  volatile bool cancel_ = false;
  volatile bool shutdown_ = false;

  // The path queued for the worker (single slot; newest request wins). Guarded by
  // stateLock_; read by the worker into a local copy before the unlocked build.
  std::string pendingPath_;
  bool hasPending_ = false;
  // Last path handed to warm() — dedupe so a settle on an already-warmed book is a
  // no-op. Owned by the caller's thread (warm() only), no lock needed.
  std::string lastWarmedPath_;

  // Debounce state (caller's main loop only; no lock needed).
  unsigned long lastChangeMs_ = 0;
  bool pendingWarm_ = false;
};
