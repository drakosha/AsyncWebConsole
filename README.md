# AsyncWebConsole (ESP32 / Arduino)

Async WebSocket console for ESP32 built on top of `ESPAsyncWebServer`. It safely captures `esp_log` output from any context (ISR/task) into a FreeRTOS queue; a background task drains the queue, broadcasts to WebSocket clients, and stores an in‑memory circular backlog. No polling in `loop()` required.

## Features
- WebSocket console at a configurable path (default: `/ws`).
- In-memory circular backlog (size configured via constructor, bytes).
- Fully async `esp_log` bridge via `esp_log_set_vprintf()`.
- ISR-safe: only enqueues from ISRs; sending happens in a task.
- Optional mirroring to any `Print` (e.g., `Serial`).
- Built-in HTML client with Clear / Pause / Resume controls, optional buffering, and ANSI SGR styling.
- Automatic WebSocket batching with drop detection to avoid `_queueMessage()` overflows.
- Command registry with auto `help` and argv parsing.
- Optional file logging with rotation (LittleFS/SPIFFS).
- Timestamps, line clipping, and console `esp_log` level filtering.

## Requirements
- Platform: `espressif32`
- Framework: `arduino`
- Libraries:
  - `ESPAsyncWebServer`
  - `AsyncTCP`

## Installation
- Local: the library lives in `lib/AsyncWebConsole` and is auto‑discovered by PlatformIO.
- External: install from the public repo via `lib_deps`.

```ini
lib_deps =
  drakosha/AsyncWebConsole@^0.2.1
```

## Quick Start
```cpp
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "AsyncWebConsole.h"

AsyncWebServer server(80);
// 16 KB in‑RAM backlog, WS at "/ws"
AsyncWebConsole console("/ws", 16 * 1024);

void setup(){
  Serial.begin(115200);

  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-WebConsole", "pass");

  // Optionally mirror to Serial
  console.setMirrorSerial(&Serial);

  // Enable async esp_log -> WebSocket bridge
  console.enableEspLogBridge();

  // Optionally bridge early ets_printf/ROM UART
  console.enableEtsPrintfBridge();

  // Example command
  console.addCommand("heap", "", "free heap",
    [](int, const String*){ return String(F("heap: ")) + String(ESP.getFreeHeap()) + F("\n"); });

  // Serve HTML at /console and WS at /ws
  console.attachTo(server, "/console");
  server.begin();

  console.print("== AsyncWebConsole ready ==\n");
}

void loop(){ }
```

## Core API
- `AsyncWebConsole(const char* wsPath = "/ws", size_t backlogBytes = 16*1024)`
- `AsyncWebConsole(const char* wsPath, size_t backlogBytes, const Config& cfg)`
- `void attachTo(AsyncWebServer& server, const char* routePath = "/")` — serves HTML and registers WS handler.
- `void onCommand(CmdHandler h)` — single‑string handler (`std::function<String(const String&)>`).
- `bool addCommand(const char* name, const char* args, const char* help, CmdArgHandler fn)` — argv‑based command registry.
- Logging: `log(const String&)`, `print(const String&)`, `printf(const char* fmt, ...)`.
- `void sendBacklog(AsyncWebSocketClient* client)` — sends backlog to a new client (called on connect).
- Timestamps/clipping: `setTimestamps(bool)`, `setMaxLineLen(size_t)`.

### Bridges and Levels
- `void setMirrorSerial(Print* out)` — mirror output to `Serial` (or any `Print`).
- `void enableEspLogBridge()` / `void disableEspLogBridge()` / `void setEspLogBridge(bool)` — `esp_log` bridge.
- `void enableEtsPrintfBridge()` / `void disableEtsPrintfBridge()` / `void setEtsPrintfBridge(bool)` — `ets_printf` bridge.
- IDF filters: `setGlobalLogLevel(esp_log_level_t)` and `setTagLogLevel(const char* tag, esp_log_level_t)`.
- Console filter: `setSyslogMaxLevel(esp_log_level_t)` — limits which `esp_log` lines reach the console (based on E/W/I/D/V prefix).

### File Logging
- Enable: `enableFileLog(const char* path = nullptr, size_t maxSize = 0, uint8_t maxFiles = 0)`
- Disable: `disableFileLog()` or `setFileLog(bool enable, ...)`
- Requires a mounted FS: call `LittleFS.begin()` or `SPIFFS.begin()` before enabling file logging.

### Configuration
```cpp
const AsyncWebConsole::Config cfg = []{
  AsyncWebConsole::Config c;
  c.queueLen       = 8;            // queue depth; keep modest if maxLineLen is large
  c.taskStack      = 4096;         // drain task stack size (bytes)
  c.taskPrio       = 3;            // drain task priority
  c.mirrorOut      = &Serial;      // mirror to Serial (nullptr to disable)
  c.timestamps     = true;         // [HH:MM:SS.mmm] prefix
  c.maxLineLen     = 512;          // 0 = unlimited; otherwise clip
  c.fileLogEnable  = false;        // file logging off by default
  c.filePath       = "/console.log";
  c.maxFileSize    = 32 * 1024;    // rotate when exceeded
  c.maxFiles       = 3;            // .1 .. .N
  c.syslogMaxLevel = ESP_LOG_VERBOSE; // console allows up to this level
  c.wsBatchMaxBytes   = 1024;      // aggregate WS payload to reduce queue pressure
  c.wsFlushIntervalMs = 100;       // flush aggregated logs at least every 100 ms
  return c;
}();

AsyncWebConsole console("/ws", 16*1024, cfg);
```

## Command Registration
Attach commands via `addCommand(...)`:
```cpp
console.addCommand("echo", "<text>", "repeat text",
  [](int argc, const String* argv){
    if (argc < 2) return String(F("Usage: echo <text>\n"));
    String s; for (int i=1;i<argc;i++){ if (i>1) s+=' '; s+=argv[i]; }
    s += '\n'; return s;
  });
```
You can also use the simpler `onCommand(...)` if you don’t need argv parsing. The built‑in `help` output is generated automatically from the registry.

## HTML Client
- Served at `routePath` passed to `attachTo(...)` (e.g., `/console`).
- Connects to the WS path you pass in the constructor (default `/ws`).
- Controls: Clear log, Pause/Resume (optionally buffer while paused, or drop messages).
- Shows skipped/buffered counts in the status line when output is paused.
- Supports command history, auto-reconnect, ANSI SGR parsing, and timestamps.

## Notes
- No singletons required. Just call `attachTo(server, "/console")` before `server.begin()`.
- For high-volume logs, increase `queueLen` in `Config` (but mind `maxLineLen` memory usage).
- Tune `wsBatchMaxBytes` / `wsFlushIntervalMs` if you need larger or faster WebSocket batches.
- `sendBacklog(...)` is sent automatically to a newly connected client.
- For file logging, ensure LittleFS or SPIFFS is mounted.

---

_Disclaimer: Portions of the codebase and documentation were drafted with the assistance of AI tooling and then reviewed by the maintainer._
