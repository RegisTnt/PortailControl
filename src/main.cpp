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

#define RELAY_PIETON 16
#define RELAY_VOITURE 17
#define PIN_CAPTEUR_FERME 34
#define RELAIS_REPOS LOW
#define RELAIS_ACTIVE HIGH
#define WIFI_MAISON "Bbox-69ABA8B3"

AsyncWebServer server(80);  // seule définition ici
Preferences prefs;

String enrolPassword = "changemoi";
int relaisDelay = 500;
bool notifOuverture = false;
bool notifRappel = false;
int delaiRappelMinutes = 10;

void logEtatPortail(const String &etat) {
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);
  char timestamp[20];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
  File log = SPIFFS.open("/log.txt", FILE_APPEND);
  if (log) {
    log.printf("%s;%s\n", timestamp, etat.c_str());
    log.close();
    Serial.printf("📝 Log : %s;%s\n", timestamp, etat.c_str());
  }
}

void notifierPortailOuvert() {
  HTTPClient http;
  http.begin("https://maker.ifttt.com/trigger/portail_ouvert/with/key/ISeQ-N0oT9SG2bYB4N27h");
  int httpCode = http.GET();
  if (httpCode > 0) Serial.println("🔔 Notification IFTTT envoyée !");
  else Serial.printf("❌ Erreur IFTTT : %d\n", httpCode);
  http.end();
}

void setup() {
  Serial.begin(115200);
  if (!SPIFFS.begin(true)) return;

  if (!SPIFFS.exists("/log.txt")) {
    File log = SPIFFS.open("/log.txt", FILE_WRITE);
    if (log) {
      log.println("Horodatage;État");
      log.close();
    }
  }

  prefs.begin("portail", false);
  relaisDelay = prefs.getInt("delai", 500);
  enrolPassword = prefs.getString("enrol_pwd", "changemoi");
  notifOuverture = prefs.getBool("notif_ouverture", false);
  notifRappel = prefs.getBool("notif_rappel", false);
  delaiRappelMinutes = prefs.getInt("rappel_minutes", 10);
  prefs.end();

  setupWiFi();  // appel défini dans wifi_manager.cpp
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");

  if (MDNS.begin("portail")) Serial.println("http://portail.local");

  ArduinoOTA.setHostname("portail-esp32");
  ArduinoOTA.setPassword("changemoi");
  ArduinoOTA.begin();

  pinMode(RELAY_PIETON, OUTPUT);
  pinMode(RELAY_VOITURE, OUTPUT);
  pinMode(PIN_CAPTEUR_FERME, INPUT);
  digitalWrite(RELAY_PIETON, RELAIS_REPOS);
  digitalWrite(RELAY_VOITURE, RELAIS_REPOS);

  // === Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html", "text/html");
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
    request->redirect("/settings");
  });

    server.on("/pieton", HTTP_GET, [](AsyncWebServerRequest *request) {
    digitalWrite(RELAY_PIETON, RELAIS_ACTIVE);
    delay(relaisDelay);
    digitalWrite(RELAY_PIETON, RELAIS_REPOS);
    request->send(200, "text/plain", "Piéton activé !");
  });

  server.on("/voiture", HTTP_GET, [](AsyncWebServerRequest *request) {
    digitalWrite(RELAY_VOITURE, RELAIS_ACTIVE);
    delay(relaisDelay);
    digitalWrite(RELAY_VOITURE, RELAIS_REPOS);
    request->send(200, "text/plain", "Voiture activée !");
  });

  server.on("/etat", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool ferme = digitalRead(PIN_CAPTEUR_FERME) == HIGH;
    request->send(200, "text/plain", ferme ? "ferme" : "ouvert");
  });


  server.begin();
}

void loop() {
  ArduinoOTA.handle();

  static bool etatPrec = false;
  bool etatActuel = digitalRead(PIN_CAPTEUR_FERME) == HIGH;

  static unsigned long tempsOuverture = 0;
  static bool alerteEnvoyee = false;

  if (etatActuel != etatPrec) {
    String etatStr = etatActuel ? "ferme" : "ouvert";
    logEtatPortail(etatStr);
    if (notifOuverture) notifierPortailOuvert();
    etatPrec = etatActuel;
    if (!etatActuel) {
      tempsOuverture = millis();
      alerteEnvoyee = false;
    }
  }

  if (!etatActuel && notifRappel && !alerteEnvoyee &&
      millis() - tempsOuverture > (unsigned long)(delaiRappelMinutes * 60000)) {
    notifierPortailOuvert();
    alerteEnvoyee = true;
  }

  delay(500);
}
