#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include "wifi_manager.h"

// ⚡ Broches relais
const int RELAY_PIETON = 16;
const int RELAY_VOITURE = 17;

// 🔁 Logique relais
#define RELAIS_REPOS LOW
#define RELAIS_ACTIVE HIGH

Preferences prefs;
int relaisDelay = 500;  // valeur par défaut

// 🌐 Serveur web
extern AsyncWebServer server;

void setup() {
  Serial.begin(115200);

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("Erreur SPIFFS !");
    return;
  }

  // Préférences
  prefs.begin("portail", false);
  relaisDelay = prefs.getInt("delai", 500);
  prefs.end();

  // WiFi auto + fallback AP
  setupWiFi();

  // mDNS
  if (MDNS.begin("portail")) {
    Serial.println("Accès local : http://portail.local");
  }

  // OTA
  ArduinoOTA.setHostname("portail-esp32");
  ArduinoOTA.setPassword("changemoi");
  ArduinoOTA.begin();

  // 🛠️ Setup relais
  pinMode(RELAY_PIETON, OUTPUT);
  pinMode(RELAY_VOITURE, OUTPUT);
  digitalWrite(RELAY_PIETON, RELAIS_REPOS);
  digitalWrite(RELAY_VOITURE, RELAIS_REPOS);

  // 🌐 Routes principales
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html", "text/html");
  });

  server.on("/pieton", HTTP_GET, [](AsyncWebServerRequest *request) {
    digitalWrite(RELAY_PIETON, RELAIS_ACTIVE);
    delay(relaisDelay);
    digitalWrite(RELAY_PIETON, RELAIS_REPOS);
    request->send(200, "text/plain", "Bouton Piéton activé !");
  });

  server.on("/voiture", HTTP_GET, [](AsyncWebServerRequest *request) {
    digitalWrite(RELAY_VOITURE, RELAIS_ACTIVE);
    delay(relaisDelay);
    digitalWrite(RELAY_VOITURE, RELAIS_REPOS);
    request->send(200, "text/plain", "Bouton Voiture activé !");
  });

  // 📄 Page de configuration
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/settings.html", "text/html");
  });

  // 💾 Mise à jour du temps d’activation
  server.on("/set-delay", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("delai", true)) {
      String valeur = request->getParam("delai", true)->value();
      relaisDelay = valeur.toInt();
      prefs.begin("portail", false);
      prefs.putInt("delai", relaisDelay);
      prefs.end();
      Serial.printf("Nouveau délai relais : %d ms\n", relaisDelay);
    }
    request->redirect("/settings");
  });

  // 🔎 Infos WiFi en JSON
  server.on("/wifi-info", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"ssid\":\"" + String(WiFi.SSID()) + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":\"" + String(WiFi.RSSI()) + "\"}";
    request->send(200, "application/json", json);
  });

  server.begin();
}

void loop() {
  ArduinoOTA.handle();
}
