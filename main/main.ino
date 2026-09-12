#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "page.h"

// ---------------- Configuration ----------------
const char* AP_SSID     = "ESP32-ChatRoom";
const char* AP_PASSWORD = "chat12345";     // min 8 chars; use "" for open network
const int   AP_CHANNEL  = 1;
const int   AP_MAX_CONN = 8;

const uint8_t  MAX_HISTORY         = 60;   // messages kept in RAM
const uint8_t  MAX_USERNAME_LEN    = 20;
const uint16_t MAX_MESSAGE_LEN     = 200;
const uint32_t MIN_MSG_INTERVAL_MS = 400;  // basic per-client rate limit

// ---------------- Types ----------------
struct ChatMessage {
  char user[MAX_USERNAME_LEN + 1];
  char text[MAX_MESSAGE_LEN + 1];
  uint32_t timeSec;
};

struct ClientInfo {
  uint32_t id = 0;
  bool used = false;
  bool joined = false;
  char user[MAX_USERNAME_LEN + 1] = {0};
  uint32_t lastMsgMs = 0;
};

// ---------------- Globals ----------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

ChatMessage history[MAX_HISTORY];
uint8_t historyHead = 0;   // next write index
uint8_t historyCount = 0;

ClientInfo clients[AP_MAX_CONN];

// ---------------- Helpers ----------------
String formatUptime(uint32_t sec) {
  uint32_t h = (sec / 3600) % 24;
  uint32_t m = (sec / 60) % 60;
  uint32_t s = sec % 60;
  char buf[10];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
  return String(buf);
}

// Strip characters that could break the '|' delimited protocol or are unwanted
void sanitizeInPlace(char* s) {
  char* w = s;
  for (char* r = s; *r; r++) {
    unsigned char c = (unsigned char)*r;
    if (c == '|' || c == '\r' || c == '\n') continue;
    if (c < 0x20) continue;
    *w++ = *r;
  }
  *w = '\0';
}

ClientInfo* findClient(uint32_t id) {
  for (auto &c : clients) if (c.used && c.id == id) return &c;
  return nullptr;
}

ClientInfo* allocClient(uint32_t id) {
  for (auto &c : clients) {
    if (!c.used) {
      c = ClientInfo();
      c.used = true;
      c.id = id;
      return &c;
    }
  }
  return nullptr;
}

void freeClient(uint32_t id) {
  for (auto &c : clients) {
    if (c.used && c.id == id) {
      c.used = false;
      c.joined = false;
      c.id = 0;
      c.user[0] = 0;
    }
  }
}

uint16_t onlineCount() {
  uint16_t n = 0;
  for (auto &c : clients) if (c.used) n++;
  return n;
}

void addHistory(const char* user, const char* text, uint32_t timeSec) {
  ChatMessage &m = history[historyHead];
  strncpy(m.user, user, MAX_USERNAME_LEN); m.user[MAX_USERNAME_LEN] = 0;
  strncpy(m.text, text, MAX_MESSAGE_LEN); m.text[MAX_MESSAGE_LEN] = 0;
  m.timeSec = timeSec;
  historyHead = (historyHead + 1) % MAX_HISTORY;
  if (historyCount < MAX_HISTORY) historyCount++;
}

String buildMsgFrame(const char* user, const char* text, uint32_t timeSec) {
  String f = "M:";
  f += user; f += '|'; f += text; f += '|'; f += formatUptime(timeSec);
  return f;
}

void broadcastUserCount() {
  ws.textAll(String("U:") + String(onlineCount()));
}

void sendHistoryTo(AsyncWebSocketClient* client) {
  uint8_t start = (historyCount < MAX_HISTORY) ? 0 : historyHead;
  for (uint8_t i = 0; i < historyCount; i++) {
    uint8_t idx = (start + i) % MAX_HISTORY;
    client->text(buildMsgFrame(history[idx].user, history[idx].text, history[idx].timeSec));
  }
}

// ---------------- WebSocket message handling ----------------
void handleWsMessage(AsyncWebSocketClient* client, const char* data, size_t len) {
  if (len == 0 || len > (MAX_USERNAME_LEN + MAX_MESSAGE_LEN + 10)) return;

  char buf[MAX_USERNAME_LEN + MAX_MESSAGE_LEN + 16];
  size_t copyLen = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
  memcpy(buf, data, copyLen);
  buf[copyLen] = '\0';

  ClientInfo* ci = findClient(client->id());
  if (!ci) return;

  // ---- JOIN:<username> ----
  if (strncmp(buf, "JOIN:", 5) == 0) {
    if (ci->joined) return;

    char name[MAX_USERNAME_LEN + 1];
    strncpy(name, buf + 5, MAX_USERNAME_LEN); name[MAX_USERNAME_LEN] = 0;
    sanitizeInPlace(name);

    while (*name == ' ') memmove(name, name + 1, strlen(name));
    size_t nl = strlen(name);
    while (nl > 0 && name[nl - 1] == ' ') { name[--nl] = 0; }

    if (nl == 0) {
      client->text("E:Invalid username");
      return;
    }

    strncpy(ci->user, name, MAX_USERNAME_LEN); ci->user[MAX_USERNAME_LEN] = 0;
    ci->joined = true;

    sendHistoryTo(client);
    client->text(String("J:") + ci->user);

    uint32_t t = millis() / 1000;
    String sys = String(ci->user) + " joined the chat";
    addHistory("*", sys.c_str(), t);
    ws.textAll(String("S:") + sys);
    broadcastUserCount();
    return;
  }

  // ---- MSG:<text> ----
  if (strncmp(buf, "MSG:", 4) == 0) {
    if (!ci->joined) { client->text("E:Join first"); return; }

    uint32_t now = millis();
    if (now - ci->lastMsgMs < MIN_MSG_INTERVAL_MS) {
      client->text("E:You're sending messages too fast");
      return;
    }
    ci->lastMsgMs = now;

    char text[MAX_MESSAGE_LEN + 1];
    strncpy(text, buf + 4, MAX_MESSAGE_LEN); text[MAX_MESSAGE_LEN] = 0;
    sanitizeInPlace(text);

    size_t tl = strlen(text);
    while (tl > 0 && text[tl - 1] == ' ') { text[--tl] = 0; }
    if (tl == 0) return;

    uint32_t t = millis() / 1000;
    addHistory(ci->user, text, t);
    ws.textAll(buildMsgFrame(ci->user, text, t));
    return;
  }
  // unknown command -> ignore
}

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    if (!allocClient(client->id())) {
      client->text("E:Server full");
      client->close();
      return;
    }
    broadcastUserCount();
  } else if (type == WS_EVT_DISCONNECT) {
    ClientInfo* ci = findClient(client->id());
    if (ci && ci->joined) {
      uint32_t t = millis() / 1000;
      String sys = String(ci->user) + " left the chat";
      addHistory("*", sys.c_str(), t);
      ws.textAll(String("S:") + sys);
    }
    freeClient(client->id());
    broadcastUserCount();
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      handleWsMessage(client, (const char*)data, len);
    }
  }
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);

  Serial.print("AP started. IP: ");
  Serial.println(WiFi.softAPIP());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse_P(200, "text/html", INDEX_HTML);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(204);
  });

  server.onNotFound([](AsyncWebServerRequest* request) {
    request->redirect("/");
  });

  server.begin();
}

void loop() {
  ws.cleanupClients();
}