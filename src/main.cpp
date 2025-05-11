#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "wifi_manager.h"

// === PARAMÈTRES ===
#define RELAY_PIETON 16
#define RELAY_VOITURE 17
#define RELAIS_REPOS LOW
#define RELAIS_ACTIVE HIGH
#define WIFI_MAISON "Bbox-69ABA8B3"

Preferences prefs;
String enrolPassword = "changemoi";
int relaisDelay = 500;
extern AsyncWebServer server;

// === FONCTIONS DE SÉCURITÉ ===
bool isTokenValid(AsyncWebServerRequest *request) {
  if (!request->hasHeader("Cookie")) return false;
  String cookie = request->getHeader("Cookie")->value();
  int start = cookie.indexOf("auth_token=");
  if (start == -1) return false;

  start += strlen("auth_token=");
  int end = cookie.indexOf(";", start);
  if (end == -1) end = cookie.length();
  String token = cookie.substring(start, end);

  File file = SPIFFS.open("/users.json", "r");
  if (!file) return false;

  StaticJsonDocument<1024> doc;
  deserializeJson(doc, file);
  file.close();

  for (JsonPair kv : doc.as<JsonObject>()) {
    if (kv.value().as<String>() == token) return true;
  }
  return false;
}

void addOrUpdateUser(const String &username, const String &token) {
  StaticJsonDocument<1024> doc;
  File file = SPIFFS.open("/users.json", "r");
  if (file) {
    deserializeJson(doc, file);
    file.close();
  }
  doc[username] = token;
  file = SPIFFS.open("/users.json", "w");
  serializeJsonPretty(doc, file);
  file.close();
  Serial.println("✅ users.json mis à jour");
}

void setup() {
  Serial.begin(115200);

  if (!SPIFFS.begin(true)) {
    Serial.println("Erreur SPIFFS !");
    return;
  }

  prefs.begin("portail", false);
  relaisDelay = prefs.getInt("delai", 500);
  enrolPassword = prefs.getString("enrol_pwd", "changemoi");
  prefs.end();

  setupWiFi();

  if (MDNS.begin("portail")) {
    Serial.println("http://portail.local");
  }

  ArduinoOTA.setHostname("portail-esp32");
  ArduinoOTA.setPassword("changemoi");
  ArduinoOTA.begin();

  pinMode(RELAY_PIETON, OUTPUT);
  pinMode(RELAY_VOITURE, OUTPUT);
  digitalWrite(RELAY_PIETON, RELAIS_REPOS);
  digitalWrite(RELAY_VOITURE, RELAIS_REPOS);

  // === ROUTES ===
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isTokenValid(request)) return request->redirect("/enrol");
    request->send(SPIFFS, "/index.html", "text/html");
  });

  server.on("/pieton", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isTokenValid(request)) return request->redirect("/enrol");
    digitalWrite(RELAY_PIETON, RELAIS_ACTIVE);
    delay(relaisDelay);
    digitalWrite(RELAY_PIETON, RELAIS_REPOS);
    request->send(200, "text/plain", "Piéton activé !");
  });

  server.on("/voiture", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isTokenValid(request)) return request->redirect("/enrol");
    digitalWrite(RELAY_VOITURE, RELAIS_ACTIVE);
    delay(relaisDelay);
    digitalWrite(RELAY_VOITURE, RELAIS_REPOS);
    request->send(200, "text/plain", "Voiture activée !");
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/settings.html", "text/html");
  });

  server.on("/set-delay", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("delai", true)) {
      relaisDelay = request->getParam("delai", true)->value().toInt();
      prefs.begin("portail", false);
      prefs.putInt("delai", relaisDelay);
      prefs.end();
    }
    request->redirect("/settings");
  });

  server.on("/wifi-info", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"ssid\":\"" + String(WiFi.SSID()) + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":\"" + String(WiFi.RSSI()) + "\"}";
    request->send(200, "application/json", json);
  });

  server.on("/enrol", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/enrol.html", "text/html");
  });

  server.on("/enrol", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("username", true) || !request->hasParam("password", true)) {
      request->send(400, "text/plain", "Champs manquants");
      return;
    }
    String username = request->getParam("username", true)->value();
    String password = request->getParam("password", true)->value();

    if (password != enrolPassword) {
      request->send(403, "text/plain", "Mot de passe incorrect");
      return;
    }

    String token = "";
    for (int i = 0; i < 16; i++) token += String(random(16), HEX);
    addOrUpdateUser(username, token);
    request->send(200, "text/plain", token);
  });

  // === Upload/Download Users ===
  server.on("/download-users", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!SPIFFS.exists("/users.json")) {
      request->send(404, "text/plain", "Fichier introuvable");
      return;
    }
    request->send(SPIFFS, "/users.json", "application/json", true);
  });

  server.on("/upload-users", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Fichier reçu");
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    static File uploadFile;
    if (index == 0) {
      uploadFile = SPIFFS.open("/users.json", "w");
    }
    if (uploadFile) {
      uploadFile.write(data, len);
    }
    if (final) {
      uploadFile.close();
      Serial.println("✅ Upload terminé !");
    }
  });

  // === ADMIN ===
  server.on("/admin", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/admin.html", "text/html");
  });

  server.on("/update-pwd", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("newpwd", true)) {
      request->send(400, "text/plain", "Champ manquant");
      return;
    }
    if (WiFi.SSID() != WIFI_MAISON) {
      request->send(403, "text/plain", "⚠️ Réseau non autorisé");
      return;
    }
    String newpwd = request->getParam("newpwd", true)->value();
    prefs.begin("portail", false);
    prefs.putString("enrol_pwd", newpwd);
    prefs.end();
    enrolPassword = newpwd;
    Serial.println("🔐 Nouveau mot de passe d'enrôlement enregistré !");
    request->send(200, "text/plain", "Mot de passe mis à jour !");
  });

  // === USERS ===
  server.on("/users", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/users.html", "text/html");
  });

  server.on("/get-users", HTTP_GET, [](AsyncWebServerRequest *request) {
    File file = SPIFFS.open("/users.json", "r");
    if (!file) {
      request->send(500, "application/json", "{}");
      return;
    }
    StaticJsonDocument<1024> doc;
    deserializeJson(doc, file);
    file.close();
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
  });

  server.on("/delete-user", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "Paramètre manquant");
      return;
    }
    String userToDelete = request->getParam("name")->value();
    File file = SPIFFS.open("/users.json", "r");
    if (!file) {
      request->send(500, "text/plain", "Fichier introuvable");
      return;
    }
    StaticJsonDocument<1024> doc;
    deserializeJson(doc, file);
    file.close();
    doc.remove(userToDelete);
    file = SPIFFS.open("/users.json", "w");
    serializeJsonPretty(doc, file);
    file.close();
    request->redirect("/users");
  });
server.on("/upload.html", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send(SPIFFS, "/upload.html", "text/html");
});

  server.begin();
}

void loop() {
  ArduinoOTA.handle();
}
