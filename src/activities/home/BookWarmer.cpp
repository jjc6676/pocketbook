#include "BookWarmer.h"

#include <Arduino.h>  // ESP.getFreeHeap
#include <Epub.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>  // makeUniqueNoThrow

#include <algorithm>
#include <cassert>
#include <memory>

#include "CrossPointSettings.h"
#include "activities/RenderLock.h"
#include "activities/reader/ReaderUtils.h"  // applyOrientation
#include "components/UITheme.h"
#include "stores/progress/ProgressStore.h"
#include "util/PathHash.h"

namespace {
constexpr char LOG_TAG[] = "WARM";

// The reader's Epub cache root (ReaderActivity::loadEpub passes "/.crosspoint");
// the warm MUST use the same root or the section cache it builds lands under a
// different key and the reader rebuilds anyway.
constexpr char CACHE_ROOT[] = "/.crosspoint";
}  // namespace

void BookWarmer::start() {
  stateLock_ = xSemaphoreCreateBinary();
  signal_ = xSemaphoreCreateBinary();
  exited_ = xSemaphoreCreateBinary();
  assert(stateLock_ != nullptr && signal_ != nullptr && exited_ != nullptr);
  // Binary semaphores are born "taken" (count 0); give the guard once so the
  // first acquirer can take it.
  xSemaphoreGive(stateLock_);
  cancel_ = false;
  shutdown_ = false;
  hasPending_ = false;
  xTaskCreate(
    &taskTrampoline,
    "BookWarm",
    8192,  // stack: SD I/O + EPUB parse + section layout (text measurement)
    this,
    1,  // priority: same tier as render / cover prefetch
    &task_
  );
  assert(task_ != nullptr);
}

void BookWarmer::stop() {
  if (task_ != nullptr) {
    xSemaphoreTake(stateLock_, portMAX_DELAY);
    shutdown_ = true;
    cancel_ = true;
    hasPending_ = false;
    xSemaphoreGive(stateLock_);
    xSemaphoreGive(signal_);
    xSemaphoreTake(exited_, portMAX_DELAY);
    vTaskDelete(task_);
    task_ = nullptr;
  }

  if (signal_ != nullptr) {
    vSemaphoreDelete(signal_);
    signal_ = nullptr;
  }
  if (exited_ != nullptr) {
    vSemaphoreDelete(exited_);
    exited_ = nullptr;
  }
  if (stateLock_ != nullptr) {
    vSemaphoreDelete(stateLock_);
    stateLock_ = nullptr;
  }
  hasPending_ = false;
  pendingWarm_ = false;
}

void BookWarmer::warm(const std::string& path) {
  if (stateLock_ == nullptr || path.empty()) return;
  if (path == lastWarmedPath_) return;  // already warming/warmed this exact book
  lastWarmedPath_ = path;

  xSemaphoreTake(stateLock_, portMAX_DELAY);
  pendingPath_ = path;
  hasPending_ = true;
  cancel_ = false;  // arm a fresh work unit (a prior cancel doesn't kill this one)
  xSemaphoreGive(stateLock_);
  xSemaphoreGive(signal_);
}

void BookWarmer::cancelInFlight() {
  if (stateLock_ == nullptr) return;
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  cancel_ = true;
  hasPending_ = false;
  xSemaphoreGive(stateLock_);
  xSemaphoreGive(signal_);
}

// ---- Debounce (caller's main loop) -----------------------------------------

void BookWarmer::noteSelectionChanged(unsigned long nowMs) {
  lastChangeMs_ = nowMs;
  pendingWarm_ = true;
}

bool BookWarmer::shouldWarmNow(unsigned long nowMs, bool rapidJumping) {
  if (!pendingWarm_ || rapidJumping) return false;
  if (nowMs - lastChangeMs_ < kWarmSettleMs) return false;
  pendingWarm_ = false;
  return true;
}

// ---- Worker ----------------------------------------------------------------

void BookWarmer::taskTrampoline(void* ctx) {
  auto* self = static_cast<BookWarmer*>(ctx);
  self->taskLoop();

  xSemaphoreGive(self->exited_);
  while (true) vTaskDelay(portMAX_DELAY);
}

void BookWarmer::taskLoop() {
  while (true) {
    xSemaphoreTake(signal_, portMAX_DELAY);

    std::string path;
    xSemaphoreTake(stateLock_, portMAX_DELAY);
    if (shutdown_) {
      xSemaphoreGive(stateLock_);
      return;
    }
    if (hasPending_) {
      path = std::move(pendingPath_);
      pendingPath_.clear();
      hasPending_ = false;
    }
    xSemaphoreGive(stateLock_);

    if (!path.empty() && !cancel_) {
      warmPath(path);
    }
  }
}

void BookWarmer::warmPath(const std::string& path) {
  // Heap gate FIRST: don't compete with the foreground UI for heap. A cold
  // first-open is a better outcome than starving the library.
  if (ESP.getFreeHeap() < kMinHeapToWarm) {
    LOG_DBG(LOG_TAG, "Skip warm, low heap (%u)", static_cast<unsigned>(ESP.getFreeHeap()));
    return;
  }

  // Nothrow-allocate the Epub (the large, fallible allocation), null-check, then
  // move into a shared_ptr — Section holds a shared_ptr<Epub>. Mirrors the reader,
  // which moves loadEpub's unique_ptr into EpubReaderActivity's shared_ptr member.
  auto epubOwned = makeUniqueNoThrow<Epub>(path, CACHE_ROOT);
  if (!epubOwned) {
    LOG_ERR(LOG_TAG, "OOM: Epub for %s", path.c_str());
    return;
  }
  if (cancel_) return;
  std::shared_ptr<Epub> epub(std::move(epubOwned));

  // Match the reader exactly (ReaderActivity::loadEpub): build if missing, skip
  // CSS only when embeddedStyle is off.
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) return;
  if (cancel_) return;  // epub frees on scope exit

  // Ensure the cache dir exists before building the section file, mirroring the
  // reader (EpubReaderActivity::onEnter calls this before any section work).
  // createSectionFile only mkdir's sections/, not its parent.
  epub->setupCacheDir();

  // Resolve the resume spine the same way EpubReaderActivity::onEnter does: prefer
  // the library-wide store; first-open parity routes spine 0 to the text reference.
  int spineIndex = 0;
  if (const BookProgress* prog = PROGRESS_STORE.find(hashPath(path))) {
    spineIndex = prog->spineIndex;
  }
  if (spineIndex == 0) {
    const int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) spineIndex = textSpineIndex;
  }
  const int spineCount = epub->getSpineItemsCount();
  if (spineIndex < 0 || spineIndex >= spineCount) return;  // end-of-book / bad state

  // Build the section while holding the render lock — this serializes the text
  // measurement (which touches the single-threaded font glyph caches) with the
  // render task. Uncontended after a settle: the render task is idle.
  //
  // CANCELLABLE acquire: the activity swap that calls our stop() holds RenderLock
  // while it JOINS this worker (loop: exitActivity -> onExit -> stop()), so an
  // unconditional take here would deadlock against that join. Poll with a short
  // timeout, bailing the instant a cancel/shutdown lands.
  RenderLock lock{RenderLock::Deferred{}};
  while (!cancel_ && !shutdown_) {
    if (lock.tryAcquire(20)) break;
  }
  if (!lock.locked()) return;  // cancelled before acquiring — epub frees here, no deadlock

  // Replicate the reader's viewport math at open (EpubReaderActivity.cpp:913-934).
  // The auto-page-turn branch is false at open, so use the plain bottom-margin path.
  const GfxRenderer::Orientation savedOrientation = renderer_.getOrientation();
  ReaderUtils::applyOrientation(renderer_, SETTINGS.orientation);

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer_.getOrientedViewableTRBL(
    &orientedMarginTop, &orientedMarginRight, &orientedMarginBottom, &orientedMarginLeft
  );
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);

  const uint16_t viewportWidth =
    renderer_.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight =
    renderer_.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  Section sec(epub, spineIndex, renderer_);
  // Cache HIT: nothing to do (the reader would skip the build too). On a MISS,
  // build it — same 10 params, same key — with an EMPTY popupFn (never draw from
  // the worker). createSectionFile runs to completion; the join in stop() waits.
  if (!sec.loadSectionFile(
        SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
        SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
        viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled
      )) {
    if (!cancel_) {
      LOG_DBG(LOG_TAG, "Warming spine %d of %s", spineIndex, path.c_str());
      sec.createSectionFile(
        SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
        SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
        viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, []() {},
        [this]() { return cancel_ || shutdown_; }  // abort the build the instant the user acts
      );
    }
  }

  // Restore the renderer orientation we borrowed, still holding the render lock so
  // the render task never observes the reader's orientation; then release it.
  renderer_.setOrientation(savedOrientation);
  // `lock` releases at scope end (RAII); sec + epub free there too (no lock needed).
}
