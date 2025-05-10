#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <SPIFFS.h>

AsyncWebServer server(80);
Preferences preferences;

void startAPMode() {
  WiFi.softAP("Portail_Config");
  Serial.println("Mode AP actif : connectez-vous à 'Portail_Config'");
  IPAddress IP = WiFi.softAPIP();
  Serial.print("Adresse IP : ");
  Serial.println(IP);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/wifi_config.html", "text/html");
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
    String ssid = request->getParam("ssid", true)->value();
    String password = request->getParam("password", true)->value();

    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();

    request->send(200, "text/plain", "Sauvegarde réussie. Redémarrage...");
    delay(2000);
    ESP.restart();
  });

  server.begin();
}

void setupWiFi() {
  preferences.begin("wifi", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  preferences.end();

  if (ssid == "") {
    Serial.println("Aucun WiFi enregistré, lancement du mode AP...");
    startAPMode();
    return;
  }

  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.printf("Connexion à %s...", ssid.c_str());

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnecté !");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nÉchec de connexion, lancement du mode AP...");
    startAPMode();
  }
}
