#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "AsyncWebConsole.h"

AsyncWebServer server(80);

// Customise console behaviour (optional)
const AsyncWebConsole::Config consoleCfg = []{
  AsyncWebConsole::Config cfg;
  cfg.wsBatchMaxBytes   = 1024;  // aggregate logs before flushing to the socket
  cfg.wsFlushIntervalMs = 100;   // flush at least every 100 ms
  return cfg;
}();

AsyncWebConsole console("/ws", 16 * 1024, consoleCfg);

static const char* WIFI_SSID = "ESP32-WebConsole";
static const char* WIFI_PASS = "pass";

void setup(){
  Serial.begin(115200);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);

  console.attachTo(server, "/console");

  console.addCommand("heap", "", "Print free heap",
    [](int, const String*){
      return String(F("heap: ")) + String(ESP.getFreeHeap()) + F("\n");
    });

  console.addCommand("uptime", "", "Print formatted uptime",
    [](int, const String*){
      uint32_t s = millis() / 1000;
      uint32_t m = s / 60;
      uint32_t h = m / 60;
      char buf[16];
      snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
               static_cast<unsigned>(h),
               static_cast<unsigned>(m % 60),
               static_cast<unsigned>(s % 60));
      return String(F("uptime: ")) + buf + F("\n");
    });

  console.addCommand("echo", "<text>", "Echo text",
    [](int argc, const String* argv){
      if (argc < 2) return String(F("Usage: echo <text>\n"));
      String out;
      for (int i = 1; i < argc; ++i){
        if (i > 1) out += ' ';
        out += argv[i];
      }
      out += '\n';
      return out;
    });
  // Mirror to Serial and enable async esp_log bridge
  console.setMirrorSerial(&Serial);
  console.enableEspLogBridge();
  // Optionally capture early ets_printf/ROM UART too
  console.enableEtsPrintfBridge();

  server.begin();

  console.print("== AsyncWebConsole example ==\n");
}

void loop(){
}
