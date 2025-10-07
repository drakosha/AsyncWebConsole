# AsyncWebConsole (ESP32 / Arduino)

Асинхронная веб‑консоль для ESP32 на базе `ESPAsyncWebServer`. Перехватывает вывод `esp_log` из любого контекста (ISR/задача) в очередь FreeRTOS; фоновая задача читает очередь, рассылает по WebSocket и пишет бэколог в кольцевой буфер. Никакого опроса из `loop()` не требуется.

## Возможности
- WebSocket‑консоль на настраиваемом пути (по умолчанию: `/ws`).
- Кольцевой бэколог в памяти (объём задаётся в конструкторе, байты).
- Полноценный мост `esp_log` через `esp_log_set_vprintf()` (асинхронно и безопасно).
- Безопасно из ISR: в обработчиках только постановка в очередь, отправка — в задаче.
- Опциональное зеркалирование вывода в `Serial`.
- Встроенный HTML‑клиент с кнопками Clear / Pause / Resume, опцией накопления сообщений на паузе и поддержкой ANSI SGR.
- Автоматический батчинг WebSocket‑сообщений с детекцией переполнений очереди.
- Регистрация консольных команд с авто‑`help` и разбором аргументов.
- Файловый лог с ротацией (LittleFS/SPIFFS), настраиваемый.
- Метки времени, обрезка длинных строк, фильтр по уровню `esp_log` для консоли.

## Требования
- Платформа: `espressif32`
- Фреймворк: `arduino`
- Библиотеки:
  - `ESPAsyncWebServer`
  - `AsyncTCP`

## Установка
- Локально: библиотека уже находится в `lib/AsyncWebConsole` и автоматически подхватывается PlatformIO.
- Внешне: установить из GitHub через `lib_deps`.

```ini
lib_deps =
  drakosha/AsyncWebConsole@^0.2.2
```

## Быстрый старт
```cpp
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "AsyncWebConsole.h"

AsyncWebServer server(80);
// 16 КБ бэколога в памяти, WS по "/ws"
AsyncWebConsole console("/ws", 16 * 1024);

void setup(){
  Serial.begin(115200);

  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-WebConsole", "pass");

  // Опционально: дублировать вывод в Serial
  console.setMirrorSerial(&Serial);

  // Включить мост esp_log -> WebSocket (асинхронно)
  console.enableEspLogBridge();

  // Опционально: перехват раннего ets_printf/ROM UART
  console.enableEtsPrintfBridge();

  // Пример команды
  console.addCommand("heap", "", "свободная память",
    [](int, const String*){ return String(F("heap: ")) + String(ESP.getFreeHeap()) + F("\n"); });

  // Выдавать HTML по /console и WS по /ws
  console.attachTo(server, "/console");
  server.begin();

  console.print("== AsyncWebConsole готов ==\n");
}

void loop(){ }
```

## API (основное)
- `AsyncWebConsole(const char* wsPath = "/ws", size_t backlogBytes = 16*1024)`
- `AsyncWebConsole(const char* wsPath, size_t backlogBytes, const Config& cfg)`
- `void attachTo(AsyncWebServer& server, const char* routePath = "/")` — раздаёт HTML и регистрирует WS‑хендлер.
- `void onCommand(CmdHandler h)` — обработчик одной строкой (`std::function<String(const String&)>`).
- `bool addCommand(const char* name, const char* args, const char* help, CmdArgHandler fn)` — реестр команд c аргументами.
- Логирование: `log(const String&)`, `print(const String&)`, `printf(const char* fmt, ...)`.
- `void sendBacklog(AsyncWebSocketClient* client)` — отправка бэколога новому клиенту (автоматически при подключении).
- Временные метки/обрезка: `setTimestamps(bool)`, `setMaxLineLen(size_t)`.

### Мосты логов и уровни
- `void setMirrorSerial(Print* out)` — зеркалировать вывод в `Serial` (или другой `Print`).
- `void enableEspLogBridge()` / `void disableEspLogBridge()` / `void setEspLogBridge(bool)` — мост `esp_log`.
- `void enableEtsPrintfBridge()` / `void disableEtsPrintfBridge()` / `void setEtsPrintfBridge(bool)` — мост `ets_printf`.
- Фильтры IDF: `setGlobalLogLevel(esp_log_level_t)` и `setTagLogLevel(const char* tag, esp_log_level_t)`.
- Фильтр для консоли: `setSyslogMaxLevel(esp_log_level_t)` — ограничивает, какие строки `esp_log` попадают в консоль (по префиксу E/W/I/D/V).

### Файловый лог
- Включение: `enableFileLog(const char* path = nullptr, size_t maxSize = 0, uint8_t maxFiles = 0)`
- Отключение: `disableFileLog()` или `setFileLog(bool enable, ...)`
- Требуется примонтированная ФС: вызовите `LittleFS.begin()` или `SPIFFS.begin()` до включения файлового лога.

### Конфигурация
```cpp
const AsyncWebConsole::Config cfg = []{
  AsyncWebConsole::Config c;
  c.queueLen       = 8;            // длина очереди сообщений (с учётом maxLineLen)
  c.taskStack      = 4096;         // стек фоновой задачи (байт)
  c.taskPrio       = 3;            // приоритет фоновой задачи
  c.mirrorOut      = &Serial;      // зеркалирование в Serial (nullptr = выкл.)
  c.timestamps     = true;         // префикс времени [HH:MM:SS.mmm]
  c.maxLineLen     = 512;          // 0 = без ограничений, иначе обрезка
  c.fileLogEnable  = false;        // файловый лог выключен по умолчанию
  c.filePath       = "/console.log";
  c.maxFileSize    = 32 * 1024;    // ротация при превышении
  c.maxFiles       = 3;            // .1 .. .N
  c.syslogMaxLevel = ESP_LOG_VERBOSE; // максимум для попадания в консоль
  c.wsBatchMaxBytes   = 1024;      // размер агрегируемого WS-пакета
  c.wsFlushIntervalMs = 100;       // flush каждые 100 мс
  return c;
}();

AsyncWebConsole console("/ws", 16*1024, cfg);
```

## Регистрация команд
Команды удобно навешивать через `addCommand(...)`:
```cpp
console.addCommand("echo", "<text>", "повторить текст",
  [](int argc, const String* argv){
    if (argc < 2) return String(F("Usage: echo <text>\n"));
    String s; for (int i=1;i<argc;i++){ if (i>1) s+=' '; s+=argv[i]; }
    s += '\n'; return s;
  });
```
Есть и упрощённый хук `onCommand(...)`, если не нужен парсер аргументов. Встроенная команда `help` формируется автоматически на основе реестра.

## HTML‑клиент
- Отдаётся по `routePath`, указанному в `attachTo(...)` (например, `/console`).
- Подключается к вашему WS пути (конструктор, по умолчанию `/ws`).
- Управление: Clear, Pause/Resume; можно копить сообщения на паузе и воспроизводить их при возобновлении.
- В статусе отображается количество пропущенных/буферизованных сообщений.
- Поддерживает историю команд, auto-reconnect, ANSI SGR и метки времени.

## Примечания
- Никаких синглтонов не требуется. Просто вызовите `attachTo(server, "/console")` до `server.begin()`.
- При интенсивном логировании подберите `queueLen` в `Config` (учитывая `maxLineLen`).
- Настройте `wsBatchMaxBytes` / `wsFlushIntervalMs`, если требуется изменить поведение батчинга WebSocket.
- `sendBacklog(...)` автоматически вызывается для нового клиента (при `WS_EVT_CONNECT`).
- Для файлового лога убедитесь, что ФС смонтирована (LittleFS или SPIFFS).

---

_Дисклеймер: часть кода и документации была сгенерирована с использованием инструментов ИИ и впоследствии проверена автором._
