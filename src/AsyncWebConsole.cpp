#include "AsyncWebConsole.h"
#include <stdarg.h>
#include "freertos/portmacro.h"

#include <FS.h>
#if __has_include(<LittleFS.h>)
#include <LittleFS.h>
#define AWC_HAS_LITTLEFS 1
#endif

#if __has_include(<esp_rom_sys.h>)
#include <esp_rom_sys.h> // may provide esp_rom_install_uart_printf(void) to restore default
#define AWC_HAVE_ROM_PRINTF 1
#endif
#if __has_include(<rom/ets_sys.h>)
extern "C" void ets_install_putc1(void (*func)(char));
#define AWC_HAVE_ETSPUTC 1
#endif
#if __has_include(<SPIFFS.h>)
#include <SPIFFS.h>
#define AWC_HAS_SPIFFS 1
#endif

AsyncWebConsole* AsyncWebConsole::_sink = nullptr;
vprintf_like_t   AsyncWebConsole::_origVprintf = nullptr;
AsyncWebConsole::putc1_t AsyncWebConsole::_origPutc1 = nullptr;

#include <pgmspace.h>

const char AsyncWebConsole::_defaultIndexHtml[] PROGMEM =
#include "../web/console.html"
;

static const char* TAG = "AsyncWebConsole";

// helpers
static char * _vsformat(const char* fmt, size_t maxLen, va_list ap) {
  va_list ap2; va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap2);
  va_end(ap2);

  if (n <= 0) return nullptr;

  size_t required = static_cast<size_t>(n) + 2;
  size_t capacity = maxLen ? min(maxLen + 2, required) : required;

  char* p = (char*)malloc(capacity);
  if (!p) return nullptr;
  vsnprintf(p, capacity, fmt, ap);
  return p;
}

bool AsyncWebConsole::_inIsr() {
#if defined(ARDUINO_ARCH_ESP32)
  #ifdef portCHECK_IF_IN_ISR
    return portCHECK_IF_IN_ISR();
  #else
    return xPortInIsrContext();
  #endif
#else
  return false;
#endif
}

AsyncWebConsole::AsyncWebConsole(const char* wsPath, size_t backlogBytes)
: AsyncWebConsole(wsPath, backlogBytes, Config{})
{}

AsyncWebConsole::AsyncWebConsole(const char* wsPath, size_t backlogBytes, const Config& cfg)
: _ws(wsPath), _wsPath(wsPath)
{
  _cfg = cfg;
  if (backlogBytes) { _logbuf = (char*)malloc(backlogBytes); _bufCap = backlogBytes; }
  _ws.onEvent([this](AsyncWebSocket *srv,
                     AsyncWebSocketClient *cli,
                     AwsEventType type, void *arg,
                     uint8_t *data, size_t len){
    this->_onWsEvent(srv, cli, type, arg, data, len);
  });
  _q = xQueueCreate(_cfg.queueLen, sizeof(LogMsg));
  _mtx = xSemaphoreCreateMutex();
  _startDrainTask();
}

AsyncWebConsole::~AsyncWebConsole() {
  _stopDrainTask();
}

void AsyncWebConsole::attachTo(AsyncWebServer& server, const char* routePath) {
  _server = &server;
  _server->on(routePath, HTTP_GET, [this](AsyncWebServerRequest* r){
    r->send(200, String("text/html; charset=utf-8"), _indexHtml);
  });
  _server->addHandler(&_ws);
}

void AsyncWebConsole::onCommand(CmdHandler h) { _handler = std::move(h); }

void AsyncWebConsole::pushLineToBuffer(const char * s){
  if (!_logbuf || _bufCap == 0) return;
  size_t sl = strlen(s);
  if (sl >= _bufCap){
    // keep tail of the line only
    const char* src = s + (sl - _bufCap);
    memcpy(_logbuf, src, _bufCap);
    _head = 0; _used = _bufCap; return;
  }
  // ensure space exactly
  if (_used + sl > _bufCap){
    size_t need = (_used + sl) - _bufCap;
    if (need > _used) need = _used;
    _head = (_head + need) % _bufCap;
    _used -= need;
  }
  size_t tail = (_head + _used) % _bufCap;
  size_t first = min(sl, _bufCap - tail);
  memcpy(_logbuf + tail, s, first);
  if (first < sl){ memcpy(_logbuf, s + first, sl - first); }
  _used += sl;
}

void AsyncWebConsole::print(const char* s) { printf("%s", s); }
void AsyncWebConsole::print(const String& s) { printf("%s", s.c_str()); }

void AsyncWebConsole::printf(const char* fmt, ...){
  va_list ap2; va_start(ap2, fmt);
  char *s = _vsformat(fmt, _cfg.maxLineLen, ap2);
  va_end(ap2);
  if (s) _enqueueRaw(s);
}

void AsyncWebConsole::sendBacklog(AsyncWebSocketClient* c){
  String blob;
  if (_mtx) xSemaphoreTake(_mtx, portMAX_DELAY);
  if (_logbuf && _used){
    blob.reserve(_used);
    for (size_t i=0;i<_used;i++) blob += _logbuf[(_head + i) % _bufCap];
  }
  if (_mtx) xSemaphoreGive(_mtx);
  if (blob.length()) c->text(blob);
}

// Queue helpers
bool AsyncWebConsole::_enqueueRaw(char* data){
  if (!data || !_q) return false;

  bool result = true;

  LogMsg msg{ data };
  if (_inIsr()) {
    BaseType_t hpw = pdFALSE;
    if (xQueueSendFromISR(_q, &msg, &hpw) != pdTRUE) { 
      free(data); 
      result = false; 
    }
    if (hpw == pdTRUE) portYIELD_FROM_ISR();
  } else {
    if (xQueueSend(_q, &msg, 0) != pdTRUE) {
      free(data);
      result = false;
    }
  }
  return result;
}


void AsyncWebConsole::_processLine(char* data){
  if (!data) return;
  size_t dataLength = strlen(data);

  String line;
  if (_cfg.timestamps) {
    line = _formatTimestamp();
    line.reserve(line.length() + dataLength + 1);
    line += data;
  } else {
    line = data;
  }

  const char* payload = nullptr;
  size_t payloadLen = 0;
  if (line.length()) {
    payload = line.c_str();
    payloadLen = line.length();
  } else {
    payload = data;
    payloadLen = dataLength;
  }

  if (_mtx) xSemaphoreTake(_mtx, portMAX_DELAY);
  pushLineToBuffer(payload);
  if (_mtx) xSemaphoreGive(_mtx);

  _queueWsBroadcast(payload, payloadLen);
  if (_cfg.mirrorOut) {
    _cfg.mirrorOut->print(payload);
  }
  if (_cfg.fileLogEnable) _appendToFile(payload, payloadLen);
}

void AsyncWebConsole::_trimWsBatch(size_t dropBytes){
  if (!_wsBatch.length() || dropBytes == 0) return;
  if (dropBytes >= _wsBatch.length()) {
    _wsBatch = "";
    return;
  }

  size_t removeUpTo = 0;
  size_t remaining = dropBytes;
  while (remaining && removeUpTo < _wsBatch.length()){
    int newlinePos = _wsBatch.indexOf('\n', removeUpTo);
    if (newlinePos < 0) {
      _wsBatch = "";
      return;
    }
    size_t segment = static_cast<size_t>(newlinePos + 1 - removeUpTo);
    removeUpTo = newlinePos + 1;
    if (segment >= remaining) break;
    remaining -= segment;
  }

  ESP_LOGW(TAG, "WS batch overflow, dropping %u bytes", static_cast<unsigned>(removeUpTo));

  _wsDropMessage.reserve(96);
  _wsDropMessage = F("[AsyncWebConsole] WS batch overflow, dropped ");
  _wsDropMessage += static_cast<unsigned>(removeUpTo);
  _wsDropMessage += F(" bytes\n");
  _wsDropPending = true;

  if (removeUpTo >= _wsBatch.length()) _wsBatch = "";
  else _wsBatch.remove(0, removeUpTo);
}

void AsyncWebConsole::_flushWsBroadcast(bool force){
  if (!_wsBatch.length()) return;
  if (_ws.count() == 0) {
    _wsBatch = "";
    return;
  }

  uint32_t now = millis();
  if (_lastWsFlushMs == 0) _lastWsFlushMs = now;

  bool shouldFlush = force;
  if (!shouldFlush) {
    if (_cfg.wsFlushIntervalMs == 0) {
      shouldFlush = true;
    } else if ((now - _lastWsFlushMs) >= _cfg.wsFlushIntervalMs) {
      shouldFlush = true;
    } else if (_wsBatch.length() >= _cfg.wsBatchMaxBytes) {
      shouldFlush = true;
    }
  }

  if (!shouldFlush) {
    _sendPendingWsDrop(now);
    return;
  }

  _sendPendingWsDrop(now);

  if (!_ws.availableForWriteAll()) return;

  _ws.textAll(_wsBatch);
  _wsBatch = "";
  _lastWsFlushMs = now;
}

void AsyncWebConsole::_queueWsBroadcast(const char* data, size_t len){
  if (!data || !len) return;
  if (_ws.count() == 0) return;

  uint32_t now = millis();
  if (_lastWsFlushMs == 0) _lastWsFlushMs = now;

  const char* src = data;
  size_t srcLen = len;

  bool canSendNow = _ws.availableForWriteAll();

  if (canSendNow) {
    _sendPendingWsDrop(now);
    canSendNow = _ws.availableForWriteAll();
  }

  if (canSendNow && !_wsBatch.length()) {
    _ws.textAll(src);
    _lastWsFlushMs = now;
    return;
  }

  if (canSendNow && _wsBatch.length()) {
    _flushWsBroadcast(true);
    if (_ws.availableForWriteAll()) {
      _sendPendingWsDrop(now);
      if (_ws.availableForWriteAll()) {
        _ws.textAll(src);
        _lastWsFlushMs = now;
        return;
      }
    }
  }

  if (_cfg.wsBatchMaxBytes == 0){
    if (!_wsBatch.length() && canSendNow) {
      _ws.textAll(src);
      _lastWsFlushMs = now;
    } else {
      _wsBatch = String(src);
    }
    return;
  }

  size_t pendingLen = _wsBatch.length();
  if (pendingLen + srcLen > _cfg.wsBatchMaxBytes){
    if (canSendNow) _flushWsBroadcast(true);
    pendingLen = _wsBatch.length();
    if (pendingLen + srcLen > _cfg.wsBatchMaxBytes){
      size_t overflow = (pendingLen + srcLen) - _cfg.wsBatchMaxBytes;
      _trimWsBatch(overflow);
      pendingLen = _wsBatch.length();
      if (srcLen > _cfg.wsBatchMaxBytes){
        src += (srcLen - _cfg.wsBatchMaxBytes);
        srcLen = _cfg.wsBatchMaxBytes;
      }
      if (pendingLen + srcLen > _cfg.wsBatchMaxBytes){
        _wsBatch = "";
      }
    }
  }

  if (!_wsBatch.concat(src, static_cast<unsigned int>(srcLen))){
    if (canSendNow) _flushWsBroadcast(true);
    if (!_wsBatch.concat(src, static_cast<unsigned int>(srcLen))){
      _wsBatch = String(src);
    }
  }

  if (canSendNow) {
    _flushWsBroadcast(true);
  } else {
    _flushWsBroadcast(false);
  }
}

void AsyncWebConsole::_sendPendingWsDrop(uint32_t now){
  if (!_wsDropPending || !_wsDropMessage.length()) return;
  if (!_ws.availableForWriteAll()) return;

  _ws.textAll(_wsDropMessage);
  _wsDropPending = false;
  _wsDropMessage = "";
  _lastWsFlushMs = now;
}

void AsyncWebConsole::setIndexHtml(const char* htmlProgmem){
  _indexHtml = htmlProgmem ? htmlProgmem : _defaultIndexHtml;
}

void AsyncWebConsole::setConfig(const Config& cfg){
  bool wasEnabled = (_task != nullptr);
  if (wasEnabled) _stopDrainTask();

  if (!wasEnabled) _flushWsBroadcast(true);

  _cfg = cfg;
  // mirrorSerial is now read from _cfg directly

  _wsBatch = "";
  _lastWsFlushMs = millis();

  if (_q) { vQueueDelete(_q); _q = nullptr; }
  _q = xQueueCreate(_cfg.queueLen, sizeof(LogMsg));

  if (wasEnabled) _startDrainTask();
}

void AsyncWebConsole::setTimestamps(bool enable){
  _cfg.timestamps = enable;
}

void AsyncWebConsole::setMaxLineLen(size_t n){
  _cfg.maxLineLen = n;
}

void AsyncWebConsole::setSyslogMaxLevel(esp_log_level_t level){
  _cfg.syslogMaxLevel = level;
}

void AsyncWebConsole::setGlobalLogLevel(esp_log_level_t level){
  esp_log_level_set("*", level);
}

void AsyncWebConsole::setTagLogLevel(const char* tag, esp_log_level_t level){
  if (tag && *tag) esp_log_level_set(tag, level);
}

void AsyncWebConsole::setMirrorSerial(Print* out){ _cfg.mirrorOut = out; }

void AsyncWebConsole::_onWsEvent(AsyncWebSocket *srv, AsyncWebSocketClient *cli,
                                 AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT) {
    // Send banner with newline so next output starts on a new line
    cli->setCloseClientOnQueueFull(false);
    cli->text("== AsyncWebConsole connected ==\n");
    sendBacklog(cli);
    String ht = helpText(); if (ht.length()) cli->text(ht);
    _flushWsBroadcast(true);
    return;
  }
  if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index==0 && info->len==len && info->opcode==WS_TEXT) {
      String cmd; cmd.reserve(len+1);
      for (size_t i=0;i<len;i++) cmd += (char)data[i];
      print("> " + cmd);
      String out = dispatch(cmd);
      if (out.length()) print(out);
    }
  }
}

int AsyncWebConsole::_idfVprintfShim(const char* fmt, va_list ap){
  // Bridge not ready: return length only
  if (!_sink || !_sink->_q) {
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    return n > 0 ? n : 0;
  }
 
  // Normal path
  char * s = _vsformat(fmt, _sink->_cfg.maxLineLen, ap);
  if (!s) return 0;
  int len = strlen(s);
  if (_sink->_allowSyslog(s)) {
    if (!_sink->_enqueueRaw(s)) return 0;
  } else {
    free(s);
  }
  return len;
}

void AsyncWebConsole::_startDrainTask(){
  if (_task) return;
  xTaskCreatePinnedToCore(_drainTask, "awc_drain",
                          _cfg.taskStack, this,
                          _cfg.taskPrio, &_task,
                          tskNO_AFFINITY);
}

void AsyncWebConsole::_stopDrainTask(){
  if (_task){
    _flushWsBroadcast(true);
    TaskHandle_t t = _task;
    _task = nullptr;
    vTaskDelete(t);
  }
}

void AsyncWebConsole::_drainTask(void* arg){
  auto* self = static_cast<AsyncWebConsole*>(arg);
  for(;;){
    LogMsg msg;
    TickType_t waitTicks = portMAX_DELAY;
    if (self->_cfg.wsFlushIntervalMs){
      waitTicks = pdMS_TO_TICKS(self->_cfg.wsFlushIntervalMs);
      if (waitTicks == 0) waitTicks = 1;
    }
    if (xQueueReceive(self->_q, &msg, waitTicks) == pdTRUE){
      if (msg.data){
        size_t len = strlen(msg.data);
        if (len && msg.data[len - 1] != '\n') {
          char* extended = static_cast<char*>(realloc(msg.data, len + 2));
          if (extended) {
            extended[len] = '\n';
            extended[len + 1] = '\0';
            msg.data = extended;
          }
        }
        self->_processLine(msg.data);
        free(msg.data);
      }
    } else {
      self->_flushWsBroadcast(false);
    }
  }
}

void AsyncWebConsole::enableEspLogBridge(){
  if (_sink && _sink != this){ /* another instance using bridge */ }
  _sink = this;
  if (!_origVprintf) _origVprintf = esp_log_set_vprintf(&_idfVprintfShim);
  else               esp_log_set_vprintf(&_idfVprintfShim);
}

void AsyncWebConsole::disableEspLogBridge(){
  if (_origVprintf) esp_log_set_vprintf(_origVprintf);
  _sink = nullptr;
  _stopDrainTask();
}

void AsyncWebConsole::setEspLogBridge(bool enable){
  if (enable) enableEspLogBridge();
  else        disableEspLogBridge();
}

// (removed installEarlyLogHook; enableEspLogBridge() should be called in setup())

// ets_printf bridge
void AsyncWebConsole::_etsPutcHook(char c){
  // Minimal critical section to guard index only; we don't lock Serial
  static char lbuf[256];
  static size_t li = 0;
  static bool readyToSend = false;

#if defined(ARDUINO_ARCH_ESP32)
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&mux);
#endif

if (li < sizeof(lbuf) - 1) lbuf[li++] = c; else readyToSend = true;
  if (c == '\n') readyToSend = true;
  if (readyToSend) {
    char * buf = (char *) malloc(li + 2);
    if (buf) {
      memcpy(buf, lbuf, li);
      buf[li] = '\0';
      if (li && buf[li - 1] != '\n') {
        buf[li] = '\n';
        buf[li + 1] = '\0';
      }
      if (_sink && _sink->_q) _sink->_enqueueRaw(buf); else free(buf);
    }
    li = 0;
    readyToSend = false;
  }

#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&mux);
#endif

}

void AsyncWebConsole::enableEtsPrintfBridge(){
#if defined(AWC_HAVE_ETSPUTC)
  ets_install_putc1(&_etsPutcHook);
#endif
}

void AsyncWebConsole::disableEtsPrintfBridge(){
#if defined(AWC_HAVE_ROM_PRINTF)
  // Restore default ROM UART printf
  esp_rom_install_uart_printf();
#endif
}

void AsyncWebConsole::setEtsPrintfBridge(bool enable){
  if (enable) enableEtsPrintfBridge();
  else        disableEtsPrintfBridge();
}

void AsyncWebConsole::enableFileLog(const char* path, size_t maxSize, uint8_t maxFiles){
  _cfg.fileLogEnable = true;
  if (path) _cfg.filePath = path;
  if (maxSize) _cfg.maxFileSize = maxSize;
  if (maxFiles) _cfg.maxFiles = maxFiles;
}

void AsyncWebConsole::disableFileLog(){
  _cfg.fileLogEnable = false;
}

void AsyncWebConsole::setFileLog(bool enable, const char* path, size_t maxSize, uint8_t maxFiles){
  if (enable) enableFileLog(path, maxSize, maxFiles);
  else        disableFileLog();
}

size_t AsyncWebConsole::_currentFileSize(){
#if defined(AWC_HAS_LITTLEFS)
  File f = LittleFS.open(_cfg.filePath, FILE_READ);
  if (!f) return 0;
  size_t s = f.size(); f.close(); return s;
#elif defined(AWC_HAS_SPIFFS)
  File f = SPIFFS.open(_cfg.filePath, FILE_READ);
  if (!f) return 0;
  size_t s = f.size(); f.close(); return s;
#else
  return 0;
#endif
}

void AsyncWebConsole::_rotateIfNeeded(){
#if defined(AWC_HAS_LITTLEFS) || defined(AWC_HAS_SPIFFS)
  size_t s = _currentFileSize();
  if (s <= _cfg.maxFileSize) return;

  auto doExists = [&](const String& p)->bool{
#if defined(AWC_HAS_LITTLEFS)
    return LittleFS.exists(p);
#else
    return SPIFFS.exists(p);
#endif
  };
  auto doRemove = [&](const String& p){
#if defined(AWC_HAS_LITTLEFS)
    LittleFS.remove(p);
#else
    SPIFFS.remove(p);
#endif
  };
  auto doRename = [&](const String& from, const String& to){
#if defined(AWC_HAS_LITTLEFS)
    LittleFS.rename(from, to);
#else
    SPIFFS.rename(from, to);
#endif
  };

  for (int i = (int)_cfg.maxFiles - 1; i >= 1; --i){
    String from = String(_cfg.filePath) + "." + String(i);
    String to   = String(_cfg.filePath) + "." + String(i+1);
    if (doExists(from)) {
      if (doExists(to)) doRemove(to);
      doRename(from, to);
    }
  }
  String to1 = String(_cfg.filePath) + ".1";
  if (doExists(to1)) doRemove(to1);
  doRename(String(_cfg.filePath), to1);
#endif
}

void AsyncWebConsole::_appendToFile(const char* data, size_t len){
  if (!_cfg.fileLogEnable) return;
#if defined(AWC_HAS_LITTLEFS)
  _rotateIfNeeded();
  File f = LittleFS.open(_cfg.filePath, FILE_APPEND, true);
  if (f) { f.write((const uint8_t*)data, len); f.close(); }
#elif defined(AWC_HAS_SPIFFS)
  _rotateIfNeeded();
  File f = SPIFFS.open(_cfg.filePath, FILE_APPEND);
  if (!f) { f = SPIFFS.open(_cfg.filePath, FILE_WRITE); }
  if (f) { f.write((const uint8_t*)data, len); f.close(); }
#else
  (void)data; (void)len;
#endif
}

String AsyncWebConsole::_formatTimestamp(){
  uint32_t ms = millis();
  uint32_t sec = ms / 1000U;
  uint32_t h = (sec / 3600U) % 100U; // wrap at 99h
  uint32_t m = (sec / 60U) % 60U;
  uint32_t s = sec % 60U;
  uint32_t mm = ms % 1000U;
  char buf[20];
  snprintf(buf, sizeof(buf), "[%02u:%02u:%02u.%03u] ", (unsigned)h, (unsigned)m, (unsigned)s, (unsigned)mm);
  return String(buf);
}

String AsyncWebConsole::_clip(const String& s){
  if (_cfg.maxLineLen == 0) return s;
  if (s.length() <= _cfg.maxLineLen) return s;
  return String(s.c_str(), _cfg.maxLineLen);
}

esp_log_level_t AsyncWebConsole::_detectEspLogLevel(const String& s){
  // Try to detect IDF level prefix: "E (..)", "W (..)", "I (..)", "D (..)", "V (..)"
  // Scan first few characters to skip potential leading whitespace/escape
  for (size_t i = 0; i < s.length() && i < 8; ++i){
    char c = s[i];
    if (c == 'E') return ESP_LOG_ERROR;
    if (c == 'W') return ESP_LOG_WARN;
    if (c == 'I') return ESP_LOG_INFO;
    if (c == 'D') return ESP_LOG_DEBUG;
    if (c == 'V') return ESP_LOG_VERBOSE;
    if ((c >= 'A' && c <= 'Z') || c == '[') break; // other prefixes: treat as unknown
    if (c == '\x1b') continue; // ANSI escape start, keep scanning
  }
  return ESP_LOG_NONE; // unknown -> allow by default
}

bool AsyncWebConsole::_allowSyslog(const char * s) const{
  esp_log_level_t lvl = _detectEspLogLevel(s);
  if (lvl == ESP_LOG_NONE) return true; // unknown/none -> pass
  return lvl <= _cfg.syslogMaxLevel;
}

bool AsyncWebConsole::addCommand(const char* name, const char* args, const char* help, CmdArgHandler fn){
  if (!name || !*name || !fn) return false;
  if (_cmdCount >= _maxCmds) return false;
  _cmds[_cmdCount++] = CommandEntry{name, args?args:"", help?help:"", std::move(fn)};
  return true;
}

String AsyncWebConsole::helpText() const {
  if (_cmdCount == 0) return String();
  // Compute column widths
  size_t wName = 0, wArgs = 0;
  for (size_t i=0;i<_cmdCount;i++){
    size_t ln = _cmds[i].name ? strlen(_cmds[i].name) : 0;
    size_t la = (_cmds[i].args && *_cmds[i].args) ? strlen(_cmds[i].args) : 0;
    if (ln > wName) wName = ln;
    if (la > wArgs) wArgs = la;
  }
  // Build aligned table
  String s(F("Commands:\n"));
  for (size_t i=0;i<_cmdCount;i++){
    const char* name = _cmds[i].name ? _cmds[i].name : "";
    const char* args = (_cmds[i].args && *_cmds[i].args) ? _cmds[i].args : "";
    const char* help = (_cmds[i].help && *_cmds[i].help) ? _cmds[i].help : "";

    s += F("  ");
    // name column
    s += name;
    size_t padN = (wName > strlen(name)) ? (wName - strlen(name)) : 0;
    for (size_t p=0;p<padN;p++) s += ' ';
    s += ' ';
    // args column
    s += args;
    size_t padA = (wArgs > strlen(args)) ? (wArgs - strlen(args)) : 0;
    for (size_t p=0;p<padA;p++) s += ' ';
    // separator and help
    if (*help){ s += F("  - "); s += help; }
    s += '\n';
  }
  return s;
}

String AsyncWebConsole::dispatch(const String& raw){
  Serial.printf("Dispatching: %s\n", raw.c_str());
  String c = raw; c.trim(); if (!c.length()) return String();
  String argv[_maxArgs];
  int argc = _tokenize(c, argv, _maxArgs);
  if (argc <= 0) return String();
  if (argv[0] == F("help")) return helpText();
  for (size_t i=0;i<_cmdCount;i++){
    if (argv[0].equalsIgnoreCase(_cmds[i].name)){
      auto& fn = _cmds[i].fn; if (fn) return fn(argc, argv);
      break;
    }
  }
  return String(F("Unknown command. Type 'help'\n"));
}

// no explicit dispatcher toggle; subclass and override dispatch() if needed

int AsyncWebConsole::_tokenize(const String& in, String out[], int maxOut){
  int n = 0; bool inQuote = false; String cur;
  for (size_t i=0;i<in.length();i++){
    char ch = in[i];
    if (ch == '"') { inQuote = !inQuote; continue; }
    if (!inQuote && (ch==' ' || ch=='\t')){
      if (cur.length()){
        if (n < maxOut) out[n++] = cur;
        cur = String();
      }
      continue;
    }
    cur += ch;
  }
  if (cur.length()){
    if (n < maxOut) out[n++] = cur;
  }
  return n;
}
