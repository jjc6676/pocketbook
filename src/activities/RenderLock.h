#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  // Deferred construction: does NOT acquire. Use tryAcquire() to take the lock
  // cancellably — for background workers that must not block forever (an
  // unconditional RenderLock would deadlock against the activity swap that joins
  // them while holding the lock). Releases via RAII like any other RenderLock.
  struct Deferred {};
  explicit RenderLock(Deferred) {}
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  // Attempt to take the lock with a timeout; returns true (and stays locked) on
  // success. Call repeatedly in a cancel-check loop until it returns true.
  bool tryAcquire(unsigned timeoutMs);
  bool locked() const { return isLocked; }
  void unlock();
  static bool peek();
};
