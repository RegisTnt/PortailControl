#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "wifi_manager.h"
#include <time.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "recovery_config.h"

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

#define RELAY_PIETON 16
#define RELAY_VOITURE 17
#define PIN_CAPTEUR_FERME 34

#define RELAIS_REPOS LOW
#define RELAIS_ACTIVE HIGH

#define KEEPALIVE_INTERVAL 60000
#define DAILY_RESTART_HOUR 3
#define DAILY_RESTART_MIN 0
#define WATCHDOG_TIMEOUT 30
#define WEATHER_UPDATE_INTERVAL 900000UL
#define WEATHER_RETRY_INTERVAL 60000UL
#define WEATHER_HTTP_TIMEOUT_MS 4000
#define WEATHER_TASK_STACK_SIZE 8192
#define IFTTT_HTTP_TIMEOUT_MS 4000
#define IFTTT_TASK_STACK_SIZE 6144
#define LOG_MAX_SIZE_BYTES 65536UL

const char *WEATHER_URL =
  "http://api.open-meteo.com/v1/forecast"
  "?latitude=43.1242&longitude=5.9280"
  "&current=temperature_2m,apparent_temperature,weather_code"
  "&timezone=Europe%2FParis&forecast_days=1";

AsyncWebServer server(80);
Preferences prefs;

String enrolPassword = PORTAL_DEFAULT_ENROL_PASSWORD;
unsigned long relaisDelay = RELAY_DELAY_DEFAULT_MS;
bool notifOuverture = false;
bool notifRappel = false;
int delaiRappelMinutes = 10;

unsigned long lastKeepAlive = 0;
unsigned long relaisStartTime = 0;

bool relaisActif = false;
int relaisEnCours = -1;
bool relaySafetyReady = false;
portMUX_TYPE relayMux = portMUX_INITIALIZER_UNLOCKED;
bool spiffsAvailable = false;
SemaphoreHandle_t spiffsMutex = nullptr;
QueueHandle_t iftttQueue = nullptr;
QueueHandle_t logQueue = nullptr;
uint32_t bootCount = 0;
esp_reset_reason_t lastResetReason = ESP_RST_UNKNOWN;
bool mdnsActive = false;
unsigned long mdnsNextAttemptAt = 0;

struct WeatherSnapshot {
  bool valid;
  float temperature;
  float apparentTemperature;
  int weatherCode;
  unsigned long fetchedAtMs;
  char observedAt[25];
};

WeatherSnapshot weather = {false, 0.0f, 0.0f, -1, 0, ""};
portMUX_TYPE weatherMux = portMUX_INITIALIZER_UNLOCKED;
bool weatherRequestInProgress = false;
bool weatherHasAttempted = false;
unsigned long weatherLastAttemptAt = 0;
int weatherLastError = 0;

// -------------------------
// Logs
// -------------------------
struct LogMessage { char event[64]; };

void writeLogEntry(const char *etat) {
  if (!spiffsAvailable || spiffsMutex == nullptr) return;
  if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println("[SPIFFS] Log ignore: stockage occupe");
    return;
  }
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);

  char timestamp[24];

  if (timeinfo) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
  } else {
    strcpy(timestamp, "1970-01-01 00:00:00");
  }

  File existing = SPIFFS.open("/log.txt", FILE_READ);
  const size_t logSize = existing ? existing.size() : 0;
  if (existing) existing.close();
  if (logSize >= LOG_MAX_SIZE_BYTES) {
    SPIFFS.remove("/log.old.txt");
    if (!SPIFFS.rename("/log.txt", "/log.old.txt")) {
      Serial.println("[SPIFFS] Rotation du journal impossible");
    }
  }

  File log = SPIFFS.open("/log.txt", FILE_APPEND);

  if (log) {
    log.printf("%s;%s\n", timestamp, etat);
    log.close();
    Serial.printf("[Log] %s;%s\n", timestamp, etat);
  } else {
    Serial.println("❌ Impossible d'écrire dans /log.txt");
  }
  xSemaphoreGive(spiffsMutex);
}

void logWorkerTask(void *parameter) {
  (void)parameter;
  LogMessage message;
  for (;;) {
    if (xQueueReceive(logQueue, &message, portMAX_DELAY) == pdTRUE) {
      if (strcmp(message.event, "__CLEAR__") == 0) {
        if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          SPIFFS.remove("/log.txt");
          SPIFFS.remove("/log.old.txt");
          File log = SPIFFS.open("/log.txt", FILE_WRITE);
          if (log) {
            log.println("Horodatage;État");
            log.close();
          }
          xSemaphoreGive(spiffsMutex);
        }
      } else {
        writeLogEntry(message.event);
      }
    }
  }
}

void logEtatPortail(const String &etat) {
  if (logQueue == nullptr) return;
  LogMessage message{};
  strlcpy(message.event, etat.c_str(), sizeof(message.event));
  if (xQueueSend(logQueue, &message, 0) != pdTRUE) {
    Serial.println("[Log] File pleine, evenement ignore");
  }
}

void initialiserLog() {
  if (!spiffsAvailable || spiffsMutex == nullptr) return;
  if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  if (!SPIFFS.exists("/log.txt")) {
    File log = SPIFFS.open("/log.txt", FILE_WRITE);

    if (log) {
      log.println("Horodatage;État");
      log.close();
      Serial.println("✅ /log.txt créé");
    } else {
      Serial.println("❌ Impossible de créer /log.txt");
    }
  }
  xSemaphoreGive(spiffsMutex);
}

// -------------------------
// Notification IFTTT
// -------------------------
void iftttWorkerTask(void *parameter) {
  (void)parameter;
  uint8_t notification;
  for (;;) {
    if (xQueueReceive(iftttQueue, &notification, portMAX_DELAY) != pdTRUE) continue;
    if (WiFi.status() != WL_CONNECTED) {
      logEtatPortail("notification_ifttt_echec_wifi");
      continue;
    }

    HTTPClient http;
    http.setConnectTimeout(IFTTT_HTTP_TIMEOUT_MS);
    http.setTimeout(IFTTT_HTTP_TIMEOUT_MS);
    http.setReuse(false);
    const bool started = http.begin(PORTAL_IFTTT_URL);
    const int httpCode = started ? http.GET() : -1;
    if (started) http.end();

    if (httpCode >= 200 && httpCode < 300) {
      logEtatPortail("notification_ifttt_envoyee");
    } else {
      Serial.printf("[IFTTT] Echec HTTP: %d\n", httpCode);
      logEtatPortail("notification_ifttt_erreur");
    }
  }
}

void notifierPortailOuvert() {
  const uint8_t notification = 1;
  if (iftttQueue == nullptr || xQueueSend(iftttQueue, &notification, 0) != pdTRUE) {
    logEtatPortail("notification_ifttt_file_pleine");
  }
}

// -------------------------
// Meteo Open-Meteo (tache reseau independante)
// -------------------------
void terminerRequeteMeteo() {
  portENTER_CRITICAL(&weatherMux);
  weatherRequestInProgress = false;
  portEXIT_CRITICAL(&weatherMux);
}

void definirErreurMeteo(int errorCode) {
  portENTER_CRITICAL(&weatherMux);
  weatherLastError = errorCode;
  portEXIT_CRITICAL(&weatherMux);
}

void recupererMeteoTask(void *parameter) {
  (void)parameter;

  if (WiFi.status() != WL_CONNECTED) {
    definirErreurMeteo(-100);
    terminerRequeteMeteo();
    vTaskDelete(nullptr);
    return;
  }

  WiFiClient client;
  client.setTimeout(WEATHER_HTTP_TIMEOUT_MS);

  HTTPClient http;
  http.setConnectTimeout(WEATHER_HTTP_TIMEOUT_MS);
  http.setTimeout(WEATHER_HTTP_TIMEOUT_MS);

  bool updated = false;

  if (http.begin(client, WEATHER_URL)) {
    const int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      const String payload = http.getString();
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      JsonObject current = doc["current"];

      if (!error &&
          current["temperature_2m"].is<float>() &&
          current["apparent_temperature"].is<float>() &&
          current["weather_code"].is<int>() &&
          current["time"].is<const char *>()) {
        const float temperature = current["temperature_2m"].as<float>();
        const float apparent = current["apparent_temperature"].as<float>();
        const int code = current["weather_code"].as<int>();
        const char *observedAt = current["time"].as<const char *>();

        portENTER_CRITICAL(&weatherMux);
        weather.valid = true;
        weather.temperature = temperature;
        weather.apparentTemperature = apparent;
        weather.weatherCode = code;
        weather.fetchedAtMs = millis();
        strlcpy(weather.observedAt, observedAt, sizeof(weather.observedAt));
        weatherLastError = 0;
        portEXIT_CRITICAL(&weatherMux);

        updated = true;
        Serial.printf("[Weather] Toulon %.1f C, ressenti %.1f C, code %d\n",
                      temperature, apparent, code);
      } else {
        definirErreurMeteo(error ? -102 : -103);
        Serial.println("[Weather] Reponse JSON invalide");
      }
    } else {
      definirErreurMeteo(httpCode);
      Serial.printf("[Weather] Echec HTTP : %d\n", httpCode);
    }

    http.end();
  } else {
    definirErreurMeteo(-101);
    Serial.println("[Weather] Initialisation HTTP impossible");
  }

  if (!updated) {
    Serial.println("[Weather] Derniere valeur valide conservee");
  }

  terminerRequeteMeteo();
  vTaskDelete(nullptr);
}

void gererMeteo() {
  if (WiFi.status() != WL_CONNECTED) return;

  const unsigned long now = millis();
  bool startRequest = false;

  portENTER_CRITICAL(&weatherMux);
  const unsigned long interval = weather.valid
    ? WEATHER_UPDATE_INTERVAL
    : WEATHER_RETRY_INTERVAL;

  if (!weatherRequestInProgress &&
      (!weatherHasAttempted || now - weatherLastAttemptAt >= interval)) {
    weatherRequestInProgress = true;
    weatherHasAttempted = true;
    weatherLastAttemptAt = now;
    startRequest = true;
  }
  portEXIT_CRITICAL(&weatherMux);

  if (!startRequest) return;

  const BaseType_t result = xTaskCreate(
    recupererMeteoTask,
    "open-meteo",
    WEATHER_TASK_STACK_SIZE,
    nullptr,
    1,
    nullptr
  );

  if (result != pdPASS) {
    Serial.println("[Weather] Creation de la tache impossible");
    terminerRequeteMeteo();
  }
}

// -------------------------
// WiFi
// -------------------------
void journaliserEvenementWiFi() {
  switch (consumeWiFiRecoveryEvent()) {
    case WiFiRecoveryEvent::LOST: logEtatPortail("WIFI_LOST"); break;
    case WiFiRecoveryEvent::RECONNECTED: logEtatPortail("WIFI_RECONNECTED"); break;
    case WiFiRecoveryEvent::RECOVERY_AP_STARTED: logEtatPortail("WIFI_RECOVERY_AP"); break;
    default: break;
  }
}

void keepAliveLog() {
  Serial.printf("[KeepAlive] WiFi status: %d, IP: %s\n",
                WiFi.status(),
                WiFi.localIP().toString().c_str());
}

// -------------------------
// Redémarrage quotidien
// -------------------------
void redemarrageQuotidien() {
  static bool dejaRedemarreCetteMinute = false;

  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);

  if (!timeinfo) return;

  if (timeinfo->tm_hour == DAILY_RESTART_HOUR && timeinfo->tm_min == DAILY_RESTART_MIN) {
    if (!dejaRedemarreCetteMinute) {
      dejaRedemarreCetteMinute = true;
      logEtatPortail("redemarrage_quotidien");
      Serial.println("[System] Redémarrage quotidien à 3h00...");
      portENTER_CRITICAL(&relayMux);
      digitalWrite(RELAY_PIETON, RELAIS_REPOS);
      digitalWrite(RELAY_VOITURE, RELAIS_REPOS);
      relaisActif = false;
      relaisEnCours = -1;
      portEXIT_CRITICAL(&relayMux);
      ESP.restart();
    }
  } else {
    dejaRedemarreCetteMinute = false;
  }
}

// -------------------------
// Relais non bloquants
// -------------------------
bool activerRelais(int pin) {
  if (!relaySafetyReady || (pin != RELAY_PIETON && pin != RELAY_VOITURE)) return false;
  bool accepted = false;
  portENTER_CRITICAL(&relayMux);
  if (relaisActif) {
    portEXIT_CRITICAL(&relayMux);
    return false;
  }

  relaisEnCours = pin;
  relaisStartTime = millis();
  relaisActif = true;
  digitalWrite(pin, RELAIS_ACTIVE);
  accepted = true;
  portEXIT_CRITICAL(&relayMux);
  return accepted;
}

void handleMdns() {
  if (WiFi.status() == WL_CONNECTED && !mdnsActive) {
    if (static_cast<long>(millis() - mdnsNextAttemptAt) < 0) return;
    mdnsActive = MDNS.begin("portail");
    mdnsNextAttemptAt = millis() + 10000UL;
    Serial.println(mdnsActive ? "[mDNS] portail.local actif" : "[mDNS] Demarrage impossible");
  } else if (WiFi.status() != WL_CONNECTED && mdnsActive) {
    MDNS.end();
    mdnsActive = false;
  }
}

void gererRelais() {
  portENTER_CRITICAL(&relayMux);
  if (relaisActif && millis() - relaisStartTime >= relaisDelay) {
    digitalWrite(relaisEnCours, RELAIS_REPOS);
    relaisActif = false;
    relaisEnCours = -1;
  }
  portEXIT_CRITICAL(&relayMux);
}

void relaySafetyTask(void *parameter) {
  (void)parameter;
  for (;;) {
    gererRelais();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

const char *resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_SW: return "Software";
    case ESP_RST_PANIC: return "Panic";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Other watchdog";
    case ESP_RST_BROWNOUT: return "Brownout";
    default: return "Unknown";
  }
}

void declarerRouteDiagnostic() {
  server.on("/api/diagnostic", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool relayActive;
    portENTER_CRITICAL(&relayMux);
    relayActive = relaisActif;
    portEXIT_CRITICAL(&relayMux);

    JsonDocument response;
    response["firmware"] = FIRMWARE_VERSION;
    response["git"] = FIRMWARE_GIT_HASH;
    response["uptime_seconds"] = millis() / 1000UL;
    response["reset_reason"] = resetReasonText(lastResetReason);
    response["boot_count"] = bootCount;
    JsonObject wifi = response["wifi"].to<JsonObject>();
    wifi["connected"] = WiFi.status() == WL_CONNECTED;
    wifi["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    wifi["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
    wifi["reconnect_count"] = getWiFiReconnectCount();
    wifi["recovery_ap"] = isWiFiRecoveryAPActive();
    JsonObject memory = response["memory"].to<JsonObject>();
    memory["free_heap"] = ESP.getFreeHeap();
    memory["min_free_heap"] = ESP.getMinFreeHeap();
    JsonObject portal = response["portal"].to<JsonObject>();
    portal["closed"] = digitalRead(PIN_CAPTEUR_FERME) == HIGH;
    portal["relay_active"] = relayActive;

    String payload;
    serializeJson(response, payload);
    request->send(200, "application/json", payload);
  });
}

// -------------------------
// Routes pages HTML
// -------------------------
void declarerRoutesPages() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS,
                  isWiFiRecoveryAPActive() && WiFi.status() != WL_CONNECTED
                    ? "/wifi_config.html"
                    : "/index.html",
                  "text/html");
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/settings.html", "text/html");
  });

  server.on("/historique", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/historique.html", "text/html");
  });

  server.on("/admin", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/admin.html", "text/html");
  });

  server.on("/users", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/users.html", "text/html");
  });

  server.on("/wifi_config", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/wifi_config.html", "text/html");
  });

  server.on("/upload", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/upload.html", "text/html");
  });

  server.on("/upload.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/upload.html", "text/html");
  });

  server.on("/enrol", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/enrol.html", "text/html");
  });

  server.on("/enrol.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/enrol.html", "text/html");
  });
}

// -------------------------
// Routes actions portail
// -------------------------
void declarerRoutesActions() {
  server.on("/pieton", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!activerRelais(RELAY_PIETON)) {
      request->send(409, "text/plain", "Relais deja actif");
      return;
    }
    request->send(200, "text/plain", "Pieton active");
    logEtatPortail("commande_pieton");
  });

  server.on("/voiture", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!activerRelais(RELAY_VOITURE)) {
      request->send(409, "text/plain", "Relais deja actif");
      return;
    }
    request->send(200, "text/plain", "Voiture activee");
    logEtatPortail("commande_voiture");
  });

  server.on("/etat", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool ferme = digitalRead(PIN_CAPTEUR_FERME) == HIGH;
    request->send(200, "text/plain", ferme ? "ferme" : "ouvert");
  });
}

void declarerRouteMeteo() {
  server.on("/api/weather", HTTP_GET, [](AsyncWebServerRequest *request) {
    WeatherSnapshot snapshot;
    int lastError;

    portENTER_CRITICAL(&weatherMux);
    snapshot = weather;
    lastError = weatherLastError;
    portEXIT_CRITICAL(&weatherMux);

    JsonDocument response;
    response["available"] = snapshot.valid;
    response["last_error"] = lastError;

    if (snapshot.valid) {
      response["temperature_2m"] = snapshot.temperature;
      response["apparent_temperature"] = snapshot.apparentTemperature;
      response["weather_code"] = snapshot.weatherCode;
      response["observed_at"] = snapshot.observedAt;
      response["age_seconds"] = (millis() - snapshot.fetchedAtMs) / 1000UL;
    }

    String payload;
    serializeJson(response, payload);
    request->send(snapshot.valid ? 200 : 503, "application/json", payload);
  });
}

bool parseBoundedUnsigned(const String &value, unsigned long minimum,
                          unsigned long maximum, unsigned long &result) {
  if (value.isEmpty()) return false;
  unsigned long parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') return false;
    const unsigned long digit = static_cast<unsigned long>(c - '0');
    if (parsed > (maximum - digit) / 10UL) return false;
    parsed = parsed * 10UL + digit;
  }
  if (parsed < minimum || parsed > maximum) return false;
  result = parsed;
  return true;
}

// -------------------------
// Routes paramètres
// -------------------------
void declarerRoutesParametres() {
  server.on("/set-delay", HTTP_POST, [](AsyncWebServerRequest *request) {
    unsigned long requestedDelay = 0;
    if (!request->hasParam("delai", true) ||
        !parseBoundedUnsigned(request->getParam("delai", true)->value(),
                              RELAY_DELAY_MIN_MS, RELAY_DELAY_MAX_MS, requestedDelay)) {
      request->send(400, "text/plain", "Duree relais invalide (100-5000 ms)");
      return;
    }
    relaisDelay = requestedDelay;
    prefs.begin("portail", false);
    prefs.putInt("delai", static_cast<int32_t>(relaisDelay));
    prefs.end();
    logEtatPortail("parametre_delai_relais_modifie");
    request->redirect("/settings");
  });

  server.on("/set-notifications", HTTP_POST, [](AsyncWebServerRequest *request) {
    notifOuverture = request->hasParam("notif_ouverture", true);
    notifRappel = request->hasParam("notif_rappel", true);

    if (request->hasParam("delai_rappel", true)) {
      unsigned long reminder = 0;
      if (!parseBoundedUnsigned(request->getParam("delai_rappel", true)->value(), 1, 1440, reminder)) {
        request->send(400, "text/plain", "Delai de rappel invalide (1-1440 min)");
        return;
      }
      delaiRappelMinutes = static_cast<int>(reminder);
    }

    prefs.begin("portail", false);
    prefs.putBool("notif_ouverture", notifOuverture);
    prefs.putBool("notif_rappel", notifRappel);
    prefs.putInt("rappel_minutes", delaiRappelMinutes);
    prefs.end();

    logEtatPortail("parametres_notifications_modifies");

    request->redirect("/settings");
  });
}

// -------------------------
// Routes logs
// -------------------------
void declarerRoutesLogs() {
  server.on("/log.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!spiffsAvailable || spiffsMutex == nullptr ||
        xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      request->send(503, "text/plain", "Historique indisponible.");
      return;
    }
    if (!SPIFFS.exists("/log.txt")) {
      xSemaphoreGive(spiffsMutex);
      request->send(404, "text/plain", "Aucun historique trouvé.");
      return;
    }

    File log = SPIFFS.open("/log.txt", "r");

    if (!log || log.isDirectory()) {
      if (log) log.close();
      xSemaphoreGive(spiffsMutex);
      request->send(500, "text/plain", "Erreur à l'ouverture du fichier.");
      return;
    }
    const String content = log.readString();
    log.close();
    xSemaphoreGive(spiffsMutex);
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", content);
    response->addHeader("Content-Disposition", "inline; filename=log.txt");
    request->send(response);
  });

  server.on("/clear-log", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (logQueue == nullptr) {
      request->send(503, "text/plain", "Historique indisponible");
      return;
    }
    LogMessage message{};
    strlcpy(message.event, "__CLEAR__", sizeof(message.event));
    if (xQueueSend(logQueue, &message, 0) != pdTRUE) {
      request->send(503, "text/plain", "Journal occupe");
      return;
    }
    request->send(202, "text/plain", "Effacement du journal programme");
  });
}

// -------------------------
// Routes utilisateurs
// -------------------------
void declarerRoutesUtilisateurs() {
  server.on("/get-users", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!SPIFFS.exists("/users.json")) {
      request->send(200, "application/json", "{}");
      return;
    }

    request->send(SPIFFS, "/users.json", "application/json");
  });

  server.on("/users.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!SPIFFS.exists("/users.json")) {
      request->send(404, "application/json", "{}");
      return;
    }

    request->send(SPIFFS, "/users.json", "application/json");
  });

  server.on("/delete-user", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "Paramètre name manquant");
      return;
    }

    String userToDelete = request->getParam("name")->value();

    if (!SPIFFS.exists("/users.json")) {
      request->redirect("/users");
      return;
    }

    File file = SPIFFS.open("/users.json", "r");

    if (!file) {
      request->send(500, "text/plain", "Impossible d'ouvrir users.json");
      return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
      request->send(500, "text/plain", "Erreur JSON users.json");
      return;
    }

    if (!doc[userToDelete].isNull()) {
      doc.remove(userToDelete);
      logEtatPortail("utilisateur_supprime");
    }

    file = SPIFFS.open("/users.json", "w");

    if (!file) {
      request->send(500, "text/plain", "Impossible d'écrire users.json");
      return;
    }

    serializeJsonPretty(doc, file);
    file.close();

    request->redirect("/users");
  });

  server.on("/download-users", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!SPIFFS.exists("/users.json")) {
      request->send(404, "text/plain", "users.json introuvable");
      return;
    }

    AsyncWebServerResponse *response =
      request->beginResponse(SPIFFS, "/users.json", "application/json", true);

    response->addHeader("Content-Disposition", "attachment; filename=users.json");
    request->send(response);
  });
}

// -------------------------
// Fallback fichiers SPIFFS
// -------------------------
void declarerRouteFallback() {
  server.onNotFound([](AsyncWebServerRequest *request) {
    String path = request->url();

    if (SPIFFS.exists(path)) {
      String contentType = "text/plain";

      if (path.endsWith(".html")) contentType = "text/html";
      else if (path.endsWith(".css")) contentType = "text/css";
      else if (path.endsWith(".js")) contentType = "application/javascript";
      else if (path.endsWith(".json")) contentType = "application/json";
      else if (path.endsWith(".webmanifest")) contentType = "application/manifest+json";
      else if (path.endsWith(".png")) contentType = "image/png";

      request->send(SPIFFS, path, contentType);
      return;
    }

    request->send(404, "text/plain", "Page introuvable");
  });
}

// -------------------------
// SETUP
// -------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.printf("\n=== Portail ESP32 %s (%s) ===\n", FIRMWARE_VERSION, FIRMWARE_GIT_HASH);

  pinMode(RELAY_PIETON, OUTPUT);
  pinMode(RELAY_VOITURE, OUTPUT);
  pinMode(PIN_CAPTEUR_FERME, INPUT);
  digitalWrite(RELAY_PIETON, RELAIS_REPOS);
  digitalWrite(RELAY_VOITURE, RELAIS_REPOS);

  esp_task_wdt_init(WATCHDOG_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  spiffsMutex = xSemaphoreCreateMutex();
  spiffsAvailable = SPIFFS.begin(false);
  if (!spiffsAvailable) Serial.println("[SPIFFS] Montage impossible; fonctionnement sans historique");
  initialiserLog();
  if (spiffsAvailable) {
    logQueue = xQueueCreate(16, sizeof(LogMessage));
    if (logQueue != nullptr &&
        xTaskCreate(logWorkerTask, "spiffs-log", 3072, nullptr, 1, nullptr) != pdPASS) {
      vQueueDelete(logQueue);
      logQueue = nullptr;
      Serial.println("[SPIFFS] Journal desactive: tache non creee");
    }
  }

  prefs.begin("portail", false);
  const int storedRelayDelay = prefs.getInt("delai", RELAY_DELAY_DEFAULT_MS);
  relaisDelay = storedRelayDelay >= static_cast<int>(RELAY_DELAY_MIN_MS) &&
                        storedRelayDelay <= static_cast<int>(RELAY_DELAY_MAX_MS)
                  ? static_cast<unsigned long>(storedRelayDelay)
                  : RELAY_DELAY_DEFAULT_MS;
  enrolPassword = prefs.getString("enrol_pwd", PORTAL_DEFAULT_ENROL_PASSWORD);
  notifOuverture = prefs.getBool("notif_ouverture", false);
  notifRappel = prefs.getBool("notif_rappel", false);
  delaiRappelMinutes = prefs.getInt("rappel_minutes", 10);
  if (delaiRappelMinutes < 1 || delaiRappelMinutes > 1440) delaiRappelMinutes = 10;
  bootCount = prefs.getUInt("boot_count", 0) + 1;
  prefs.putUInt("boot_count", bootCount);
  prefs.end();
  lastResetReason = esp_reset_reason();

  relaySafetyReady = xTaskCreate(relaySafetyTask, "relay-safety", 2048, nullptr, 3, nullptr) == pdPASS;
  if (!relaySafetyReady) {
    Serial.println("[Relay] ERREUR: tache de securite non creee");
  }
  iftttQueue = xQueueCreate(4, sizeof(uint8_t));
  if (iftttQueue != nullptr &&
      xTaskCreate(iftttWorkerTask, "ifttt", IFTTT_TASK_STACK_SIZE, nullptr, 1, nullptr) != pdPASS) {
    vQueueDelete(iftttQueue);
    iftttQueue = nullptr;
    Serial.println("[IFTTT] Notifications desactivees: tache non creee");
  }

  setupWiFi();

  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");

  handleMdns();

  ArduinoOTA.setHostname("portail-esp32");
  ArduinoOTA.setPassword(PORTAL_OTA_PASSWORD);
  ArduinoOTA.begin();

  declarerRoutesPages();
  declarerRoutesActions();
  declarerRouteMeteo();
  declarerRouteDiagnostic();
  declarerRoutesParametres();
  declarerRoutesLogs();
  declarerRoutesUtilisateurs();
  declarerRouteFallback();

  server.begin();

  logEtatPortail("demarrage_esp32");

  bool etatInitial = digitalRead(PIN_CAPTEUR_FERME) == HIGH;
  logEtatPortail(etatInitial ? "etat_initial_ferme" : "etat_initial_ouvert");

  Serial.println("✅ Serveur web démarré !");
}

// -------------------------
// LOOP
// -------------------------
void loop() {
  esp_task_wdt_reset();
  ArduinoOTA.handle();

  gererRelais();
  gererMeteo();
  handleWiFi();
  journaliserEvenementWiFi();
  handleMdns();
  if (isWiFiRestartPending()) {
    clearWiFiRestartPending();
    digitalWrite(RELAY_PIETON, RELAIS_REPOS);
    digitalWrite(RELAY_VOITURE, RELAIS_REPOS);
    ESP.restart();
  }

  if (millis() - lastKeepAlive >= KEEPALIVE_INTERVAL) {
    lastKeepAlive = millis();
    keepAliveLog();
  }

  redemarrageQuotidien();

  static bool etatPrec = digitalRead(PIN_CAPTEUR_FERME) == HIGH;
  bool etatActuel = digitalRead(PIN_CAPTEUR_FERME) == HIGH;

  static unsigned long tempsOuverture = 0;
  static bool alerteEnvoyee = false;

  if (etatActuel != etatPrec) {
    String etatStr = etatActuel ? "ferme" : "ouvert";

    logEtatPortail(etatStr);

    if (notifOuverture) {
      notifierPortailOuvert();
    }

    etatPrec = etatActuel;

    if (!etatActuel) {
      tempsOuverture = millis();
      alerteEnvoyee = false;
    }
  }

  if (!etatActuel &&
      notifRappel &&
      !alerteEnvoyee &&
      millis() - tempsOuverture > (unsigned long)(delaiRappelMinutes * 60000)) {
    logEtatPortail("alerte_portail_ouvert_trop_longtemps");
    notifierPortailOuvert();
    alerteEnvoyee = true;
  }

  delay(10);
}
