#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ArduinoOTA.h>
#include "wifi_manager.h"  // ⚙️ Nouvelle gestion WiFi modulaire

// ⚡ Broches relais
const int RELAY_PIETON = 16;
const int RELAY_VOITURE = 17;

// 🔁 Logique des relais
#define RELAIS_REPOS LOW      // Le relais est au repos quand la broche est à LOW
#define RELAIS_ACTIVE HIGH    // Le relais est activé quand la broche est à HIGH

// 🌐 Serveur web (déjà déclaré dans wifi_manager.cpp)
extern AsyncWebServer server;

void setup() {
  Serial.begin(115200);

  // 🧠 SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("Erreur SPIFFS !");
    return;
  }

  // 📡 WiFi auto avec fallback AP + formulaire
  setupWiFi();

  // 🌍 mDNS
  if (MDNS.begin("portail")) {
    Serial.println("Accès local : http://portail.local");
  }

  // 🔐 OTA
  ArduinoOTA.setHostname("portail-esp32");
  ArduinoOTA.setPassword("changemoi");
  ArduinoOTA.onStart([]() {
    Serial.println("🔄 Début de la mise à jour OTA...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("✅ Mise à jour terminée !");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("❌ Erreur OTA [%u]\n", error);
  });
  ArduinoOTA.begin();

  // 🛠️ Setup relais
  pinMode(RELAY_PIETON, OUTPUT);
  pinMode(RELAY_VOITURE, OUTPUT);
  digitalWrite(RELAY_PIETON, RELAIS_REPOS);
  digitalWrite(RELAY_VOITURE, RELAIS_REPOS);

  // 🌐 Routes HTTP principales
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });

  server.on("/pieton", HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(RELAY_PIETON, RELAIS_ACTIVE);
    delay(500);
    digitalWrite(RELAY_PIETON, RELAIS_REPOS);
    request->send(200, "text/plain", "Bouton Piéton actionné !");
  });

  server.on("/voiture", HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(RELAY_VOITURE, RELAIS_ACTIVE);
    delay(500);
    digitalWrite(RELAY_VOITURE, RELAIS_REPOS);
    request->send(200, "text/plain", "Bouton Voiture actionné !");
  });

  // ✅ Démarrage du serveur
  server.begin();
}

void loop() {
  ArduinoOTA.handle();  // 🚀 OTA
}
