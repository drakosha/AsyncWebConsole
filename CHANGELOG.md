# AsyncWebConsole Changelog

## 0.3.1
- **Feature:** Added `Config::idfPassthrough` (default `true`). When enabled, the IDF log bridge calls the original `vprintf` so UART output is preserved immediately (no drain-task delay). `mirrorOut` is automatically skipped for IDF-originated messages to avoid duplication.
- Internal: added `fromIdf` flag to `LogMsg` to track message origin through the pipeline.

## 0.3.0
- **Breaking:** Removed `onCommand(CmdHandler)` and `CmdHandler` typedef (was dead code, never wired into dispatch).
- **Fix:** Memory leak in destructor — now properly frees ring buffer, queue, mutex, and remaining queued messages.
- **Fix:** Race condition in `_stopDrainTask()` — replaced external `vTaskDelete` with cooperative shutdown (flag + sentinel + self-delete).
- **Fix:** Inverted flush logic in `setConfig()` — pending WS messages are now flushed before stopping the drain task; remaining queue messages are drained before queue deletion.
- **Fix:** `disableEspLogBridge()` no longer stops the drain task (drain task serves all logging, not just esp_log bridge).
- **Fix:** Removed debug `Serial.printf` left in `dispatch()`.
- **Fix:** `_detectEspLogLevel()` now requires ESP-IDF format `"X ("` instead of matching bare letters, eliminating false positives.
- **Fix:** Duplicate `keydown` event listener accumulating on WebSocket reconnect in web UI.
- **Fix:** Hardcoded `/ws` path in web UI — now dynamically injected from constructor's `wsPath` parameter.
- **Perf:** `sendBacklog()` rewritten from O(n^2) byte-by-byte String concatenation to O(n) using `malloc` + `memcpy`.
- **Perf:** HTML page served via PROGMEM streaming in chunks instead of copying entire page into RAM String.
- **UI:** Added DOM line limit (5000 lines) to prevent browser slowdown on long sessions.
- **UI:** Fixed `lang="ru"` to `lang="en"`.
- Removed dead code: `_clip()` method (declared but never called).
- **Fix:** `_etsPutcHook()` — moved `malloc` outside `portENTER_CRITICAL` section (was undefined behavior).
- Added backlog overflow notification to WebSocket clients when ring buffer wraps.
- Added warning log when `enableEspLogBridge()` reassigns bridge from another instance.
- Added unit tests (native host) and integration tests (embedded ESP32) with PlatformIO.

## 0.2.2
- Fix: update quick start and example usage

## 0.2.1
- Fixed installation issue by including the `web/` assets in exported package.
- Bumped documentation and examples to reference v0.2.1.

## 0.2.0
- Added WebSocket batching with drop detection and configurable flush interval.
- Enhanced HTML client: Clear/Pause/Resume controls, optional buffering while paused, skipped/buffered counters.
- Added command registration example and updated documentation (EN/RU).
- Improved default configuration and metadata for publishing.

## 0.1.0
- Initial release with esp_log bridge, WebSocket console, in-memory backlog, and command registry.
