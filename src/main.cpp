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

#define WIFI_CHECK_INTERVAL 10000
#define KEEPALIVE_INTERVAL 60000
#define DAILY_RESTART_HOUR 3
#define DAILY_RESTART_MIN 0
#define WATCHDOG_TIMEOUT 30
#define WEATHER_UPDATE_INTERVAL 900000UL
#define WEATHER_RETRY_INTERVAL 60000UL
#define WEATHER_HTTP_TIMEOUT_MS 4000
#define WEATHER_TASK_STACK_SIZE 8192

const char *WEATHER_URL =
  "http://api.open-meteo.com/v1/forecast"
  "?latitude=43.1242&longitude=5.9280"
  "&current=temperature_2m,apparent_temperature,weather_code"
  "&timezone=Europe%2FParis&forecast_days=1";

AsyncWebServer server(80);
Preferences prefs;

String enrolPassword = PORTAL_DEFAULT_ENROL_PASSWORD;
int relaisDelay = 500;
bool notifOuverture = false;
bool notifRappel = false;
int delaiRappelMinutes = 10;

unsigned long lastWifiCheck = 0;
unsigned long lastKeepAlive = 0;
unsigned long relaisStartTime = 0;

bool relaisActif = false;
int relaisEnCours = -1;

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

// -------------------------
// Logs
// -------------------------
void logEtatPortail(const String &etat) {
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);

  char timestamp[24];

  if (timeinfo) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
  } else {
    strcpy(timestamp, "1970-01-01 00:00:00");
  }

  File log = SPIFFS.open("/log.txt", FILE_APPEND);

  if (log) {
    log.printf("%s;%s\n", timestamp, etat.c_str());
    log.close();
    Serial.printf("📝 Log : %s;%s\n", timestamp, etat.c_str());
  } else {
    Serial.println("❌ Impossible d'écrire dans /log.txt");
  }
}

void initialiserLog() {
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
}

// -------------------------
// Notification IFTTT
// -------------------------
void notifierPortailOuvert() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ IFTTT impossible : WiFi non connecté");
    logEtatPortail("notification_ifttt_echec_wifi");
    return;
  }

  HTTPClient http;
  http.begin(PORTAL_IFTTT_URL);

  int httpCode = http.GET();

  if (httpCode > 0) {
    Serial.println("🔔 Notification IFTTT envoyée !");
    logEtatPortail("notification_ifttt_envoyee");
  } else {
    Serial.printf("❌ Erreur IFTTT : %d\n", httpCode);
    logEtatPortail("notification_ifttt_erreur");
  }

  http.end();
}

// -------------------------
// Meteo Open-Meteo (tache reseau independante)
// -------------------------
void terminerRequeteMeteo() {
  portENTER_CRITICAL(&weatherMux);
  weatherRequestInProgress = false;
  portEXIT_CRITICAL(&weatherMux);
}

void recupererMeteoTask(void *parameter) {
  (void)parameter;

  if (WiFi.status() != WL_CONNECTED) {
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
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, http.getStream());
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
        portEXIT_CRITICAL(&weatherMux);

        updated = true;
        Serial.printf("[Weather] Toulon %.1f C, ressenti %.1f C, code %d\n",
                      temperature, apparent, code);
      } else {
        Serial.println("[Weather] Reponse JSON invalide");
      }
    } else {
      Serial.printf("[Weather] Echec HTTP : %d\n", httpCode);
    }

    http.end();
  } else {
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
void reconnectWiFiIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Perdu, tentative de reconnexion...");
    logEtatPortail("wifi_perdu_reconnexion");
    WiFi.disconnect();
    WiFi.reconnect();
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
      delay(1000);
      ESP.restart();
    }
  } else {
    dejaRedemarreCetteMinute = false;
  }
}

// -------------------------
// Relais non bloquants
// -------------------------
void activerRelais(int pin) {
  if (relaisActif) {
    Serial.println("⚠️ Relais déjà actif, commande ignorée");
    logEtatPortail("commande_ignoree_relais_deja_actif");
    return;
  }

  relaisEnCours = pin;
  relaisStartTime = millis();
  relaisActif = true;

  digitalWrite(pin, RELAIS_ACTIVE);
}

void gererRelais() {
  if (relaisActif && millis() - relaisStartTime >= (unsigned long)relaisDelay) {
    digitalWrite(relaisEnCours, RELAIS_REPOS);
    relaisActif = false;
    relaisEnCours = -1;
  }
}

// -------------------------
// Routes pages HTML
// -------------------------
void declarerRoutesPages() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html", "text/html");
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
    logEtatPortail("commande_pieton");
    activerRelais(RELAY_PIETON);
    request->send(200, "text/plain", "Piéton activé !");
  });

  server.on("/voiture", HTTP_GET, [](AsyncWebServerRequest *request) {
    logEtatPortail("commande_voiture");
    activerRelais(RELAY_VOITURE);
    request->send(200, "text/plain", "Voiture activée !");
  });

  server.on("/etat", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool ferme = digitalRead(PIN_CAPTEUR_FERME) == HIGH;
    request->send(200, "text/plain", ferme ? "ferme" : "ouvert");
  });
}

void declarerRouteMeteo() {
  server.on("/api/weather", HTTP_GET, [](AsyncWebServerRequest *request) {
    WeatherSnapshot snapshot;

    portENTER_CRITICAL(&weatherMux);
    snapshot = weather;
    portEXIT_CRITICAL(&weatherMux);

    JsonDocument response;
    response["available"] = snapshot.valid;

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

// -------------------------
// Routes paramètres
// -------------------------
void declarerRoutesParametres() {
  server.on("/set-delay", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("delai", true)) {
      relaisDelay = request->getParam("delai", true)->value().toInt();

      prefs.begin("portail", false);
      prefs.putInt("delai", relaisDelay);
      prefs.end();

      logEtatPortail("parametre_delai_relais_modifie");
    }

    request->redirect("/settings");
  });

  server.on("/set-notifications", HTTP_POST, [](AsyncWebServerRequest *request) {
    notifOuverture = request->hasParam("notif_ouverture", true);
    notifRappel = request->hasParam("notif_rappel", true);

    if (request->hasParam("delai_rappel", true)) {
      delaiRappelMinutes = request->getParam("delai_rappel", true)->value().toInt();
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
    if (!SPIFFS.exists("/log.txt")) {
      request->send(404, "text/plain", "Aucun historique trouvé.");
      return;
    }

    File log = SPIFFS.open("/log.txt", "r");

    if (!log || log.isDirectory()) {
      request->send(500, "text/plain", "Erreur à l'ouverture du fichier.");
      return;
    }

    AsyncWebServerResponse *response = request->beginResponse(log, "text/plain", false);
    response->addHeader("Content-Disposition", "inline; filename=log.txt");
    request->send(response);
  });

  server.on("/clear-log", HTTP_GET, [](AsyncWebServerRequest *request) {
    SPIFFS.remove("/log.txt");

    File log = SPIFFS.open("/log.txt", FILE_WRITE);

    if (log) {
      log.println("Horodatage;État");
      log.close();
    }

    logEtatPortail("historique_efface");

    request->send(200, "text/plain", "Historique effacé !");
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

  Serial.println("\n=== Portail ESP32 - Démarrage ===");

  esp_task_wdt_init(WATCHDOG_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  if (!SPIFFS.begin(true)) {
    Serial.println("❌ Erreur SPIFFS");
    return;
  }

  initialiserLog();

  prefs.begin("portail", false);
  relaisDelay = prefs.getInt("delai", 500);
  enrolPassword = prefs.getString("enrol_pwd", PORTAL_DEFAULT_ENROL_PASSWORD);
  notifOuverture = prefs.getBool("notif_ouverture", false);
  notifRappel = prefs.getBool("notif_rappel", false);
  delaiRappelMinutes = prefs.getInt("rappel_minutes", 10);
  prefs.end();

  pinMode(RELAY_PIETON, OUTPUT);
  pinMode(RELAY_VOITURE, OUTPUT);
  pinMode(PIN_CAPTEUR_FERME, INPUT);

  digitalWrite(RELAY_PIETON, RELAIS_REPOS);
  digitalWrite(RELAY_VOITURE, RELAIS_REPOS);

  setupWiFi();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");

  if (MDNS.begin("portail")) {
    Serial.println("✅ mDNS actif : http://portail.local");
  } else {
    Serial.println("❌ Erreur mDNS");
  }

  ArduinoOTA.setHostname("portail-esp32");
  ArduinoOTA.setPassword(PORTAL_OTA_PASSWORD);
  ArduinoOTA.begin();

  declarerRoutesPages();
  declarerRoutesActions();
  declarerRouteMeteo();
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

  if (millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();
    reconnectWiFiIfNeeded();
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
