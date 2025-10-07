#pragma once
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <functional>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

class AsyncWebConsole {
public:
  using CmdHandler = std::function<String(const String&)>;
  using CmdArgHandler = std::function<String(int, const String*)>;

  struct Config {
    size_t   queueLen    = 8;      // messages in queue
    uint32_t taskStack   = 4096;    // drain task stack (bytes)
    UBaseType_t taskPrio = 3;       // drain task priority
    Print*   mirrorOut   = nullptr; // mirror to this Print (e.g., &Serial); nullptr = no mirror
    bool     timestamps   = true;   // prefix lines with HH:MM:SS.mmm
    size_t   maxLineLen   = 512;    // Max length of line bufer including '\0'

    // File logging (optional)
    bool     fileLogEnable = false;
    const char* filePath   = "/console.log";
    size_t   maxFileSize   = 32 * 1024; // rotate when exceeded
    uint8_t  maxFiles      = 3;          // .1 .. .maxFiles
    // esp_log filtering for console/backlog: allow up to this level (inclusive)
    // Use ESP-IDF levels: ESP_LOG_NONE, _ERROR, _WARN, _INFO, _DEBUG, _VERBOSE
    esp_log_level_t syslogMaxLevel = ESP_LOG_VERBOSE;

    // WebSocket batching
    size_t   wsBatchMaxBytes   = 1024;   // batch buffer size before trimming
    uint32_t wsFlushIntervalMs = 100;    // flush buffered messages at least every N ms
  };

  // backlogBytes: maximum bytes stored in memory backlog (0 = disabled)
  AsyncWebConsole(const char* wsPath = "/ws", size_t backlogBytes = 16 * 1024);
  AsyncWebConsole(const char* wsPath, size_t backlogBytes, const Config& cfg);
  virtual ~AsyncWebConsole();
  void attachTo(AsyncWebServer& server, const char* routePath = "/");
  void onCommand(CmdHandler h);

  // Log API (thread-safe)
  // Enqueue a line for processing by the drain task
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void print(const char *s);
  void print(const String &s);
  
  void sendBacklog(AsyncWebSocketClient* client);
  void setIndexHtml(const char* htmlProgmem);

  // IDF log bridge (async, background task)
  // Set mirror output (e.g., setMirrorSerial(&Serial); nullptr disables)
  void setMirrorSerial(Print* out);
  // Enable/disable esp_log bridge
  void enableEspLogBridge();
  void disableEspLogBridge();
  // Convenience setter
  void setEspLogBridge(bool enable);
  // Optional: bridge low-level ets_printf/ROM UART putc to console
  void enableEtsPrintfBridge();
  void disableEtsPrintfBridge();
  void setEtsPrintfBridge(bool enable);

  // Optional configuration; apply before enableEspLogBridge(true)
  void setConfig(const Config& cfg);
  void setTimestamps(bool enable);
  void setMaxLineLen(size_t n);
  void setSyslogMaxLevel(esp_log_level_t level);
  esp_log_level_t getSyslogMaxLevel() const { return _cfg.syslogMaxLevel; }

  // Built-in command registry (optional)
  bool addCommand(const char* name, const char* args, const char* help, CmdArgHandler fn);
  String helpText() const;
  virtual String dispatch(const String& raw);

  // esp_log filters (set IDF log levels)
  void setGlobalLogLevel(esp_log_level_t level);
  void setTagLogLevel(const char* tag, esp_log_level_t level);

  // File logging controls
  void enableFileLog(const char* path = nullptr,
                     size_t maxSize = 0, uint8_t maxFiles = 0);
  void disableFileLog();
  // Convenience setter
  void setFileLog(bool enable, const char* path = nullptr,
                  size_t maxSize = 0, uint8_t maxFiles = 0);

private:
  // Backlog byte ring buffer (single allocation)
  void pushLineToBuffer(const char * s);
  char*   _logbuf = nullptr; // circular byte buffer
  size_t  _bufCap = 0;       // capacity in bytes
  size_t  _used   = 0;       // used bytes
  size_t  _head   = 0;       // index of oldest byte
  
  // Web
  AsyncWebServer*   _server = nullptr;
  AsyncWebSocket    _ws;
  const char*       _wsPath;
  CmdHandler        _handler = nullptr;

  static const char _defaultIndexHtml[];
  const char*       _indexHtml = _defaultIndexHtml;

  void _onWsEvent(AsyncWebSocket *server,
                  AsyncWebSocketClient *client,
                  AwsEventType type, void *arg,
                  uint8_t *data, size_t len);
                  
  // esp_log bridge
  struct LogMsg { char * data; };
  static int  _idfVprintfShim(const char* fmt, va_list ap);
  static bool _inIsr();
  static vprintf_like_t _origVprintf;
  static AsyncWebConsole* _sink;
  using putc1_t = void (*)(char);
  static putc1_t _origPutc1;
  static void _etsPutcHook(char c);
  
  void _startDrainTask();
  void _stopDrainTask();
  static void _drainTask(void* arg);
  
  QueueHandle_t     _q = nullptr;
  TaskHandle_t      _task = nullptr;

  // synchronization for buffer and broadcast
  SemaphoreHandle_t _mtx = nullptr;

  // config
  Config            _cfg;

  // commands registry
  struct CommandEntry { const char* name; const char* args; const char* help; CmdArgHandler fn; };
  static constexpr size_t _maxCmds = 32;
  static constexpr int _maxArgs = 12;
  CommandEntry _cmds[_maxCmds] = {};
  size_t       _cmdCount = 0;
  
  // file logging helpers
  void _appendToFile(const char* data, size_t len);
  void _rotateIfNeeded();
  size_t _currentFileSize();

  // websocket batching helpers
  void _queueWsBroadcast(const char* data, size_t len);
  void _flushWsBroadcast(bool force);
  void _trimWsBatch(size_t dropBytes);
  void _sendPendingWsDrop(uint32_t now);

  // helpers for rendering
  String _formatTimestamp();
  String _clip(const String& s);
  static int _tokenize(const String& in, String out[], int maxOut);
  static esp_log_level_t _detectEspLogLevel(const String& s); // maps E/W/I/D/V to ESP_LOG_* (unknown -> ESP_LOG_NONE)
  bool        _allowSyslog(const char * s) const;

  // queue helpers
  void _processLine(char * data);
  bool _enqueueRaw(char * data);

  String           _wsBatch;
  uint32_t         _lastWsFlushMs = 0;
  bool             _wsDropPending = false;
  String           _wsDropMessage;
};
