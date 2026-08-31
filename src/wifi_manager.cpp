#include <WiFi.h>
#include <Preferences.h>
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "wifi_manager.h"

namespace {
constexpr unsigned long INITIAL_RECONNECT_DELAY_MS = 5000;
constexpr unsigned long MAX_RECONNECT_DELAY_MS = 60000;
constexpr unsigned long RECOVERY_AP_AFTER_MS = 300000;
constexpr unsigned long SIGNIFICANT_OUTAGE_MS = 30000;
constexpr unsigned long RESTART_AFTER_SAVE_MS = 1000;
constexpr size_t MAX_SSID_LENGTH = 32;
constexpr size_t MAX_PASSWORD_LENGTH = 63;

Preferences preferences;
String configuredSsid;
String configuredPassword;
WiFiRecoveryState state = WiFiRecoveryState::DISCONNECTED;
QueueHandle_t eventQueue = nullptr;
unsigned long disconnectedSince = 0;
unsigned long nextReconnectAt = 0;
unsigned long reconnectDelay = INITIAL_RECONNECT_DELAY_MS;
uint32_t reconnectCount = 0;
uint32_t lastOutageDurationSeconds = 0;
bool lossPublished = false;
bool recoveryAPActive = false;
bool restartPending = false;
unsigned long restartRequestedAt = 0;
SemaphoreHandle_t configMutex = nullptr;
String pendingSsid;
String pendingPassword;
bool credentialsPending = false;

void publishEvent(WiFiRecoveryEvent event) {
  if (eventQueue != nullptr) xQueueSend(eventQueue, &event, 0);
}

bool validCredential(const String &value, size_t maxLength, bool allowEmpty) {
  if ((!allowEmpty && value.isEmpty()) || value.length() > maxLength) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 32 || c == 127) return false;
  }
  return true;
}

void startRecoveryAP() {
  if (recoveryAPActive) return;
  WiFi.mode(WIFI_AP_STA);
  if (WiFi.softAP("Portail_Config")) {
    recoveryAPActive = true;
    state = WiFiRecoveryState::RECOVERY_AP;
    publishEvent(WiFiRecoveryEvent::RECOVERY_AP_STARTED);
    Serial.printf("[WiFi] Recovery AP: %s\n", WiFi.softAPIP().toString().c_str());
  }
}

void beginStationConnection() {
  if (configuredSsid.isEmpty()) {
    startRecoveryAP();
    return;
  }
  WiFi.mode(recoveryAPActive ? WIFI_AP_STA : WIFI_STA);
  state = recoveryAPActive ? WiFiRecoveryState::RECOVERY_AP : WiFiRecoveryState::RECONNECTING;
  ++reconnectCount;
  WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
  nextReconnectAt = millis() + reconnectDelay;
  reconnectDelay = min(reconnectDelay * 2UL, MAX_RECONNECT_DELAY_MS);
}

void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    const bool wasDisconnected = disconnectedSince != 0;
    const unsigned long outageDuration = wasDisconnected ? millis() - disconnectedSince : 0;
    state = WiFiRecoveryState::CONNECTED;
    reconnectDelay = INITIAL_RECONNECT_DELAY_MS;
    if (wasDisconnected && lossPublished) {
      lastOutageDurationSeconds = outageDuration / 1000UL;
      publishEvent(WiFiRecoveryEvent::RECONNECTED);
    }
    disconnectedSince = 0;
    lossPublished = false;
    Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    if (disconnectedSince == 0) {
      disconnectedSince = millis();
      lossPublished = false;
    }
    state = recoveryAPActive ? WiFiRecoveryState::RECOVERY_AP : WiFiRecoveryState::DISCONNECTED;
    nextReconnectAt = millis() + reconnectDelay;
  }
}
}

void setupWiFi() {
  preferences.begin("wifi", true);
  configuredSsid = preferences.getString("ssid", "");
  configuredPassword = preferences.getString("password", "");
  preferences.end();

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  eventQueue = xQueueCreate(8, sizeof(WiFiRecoveryEvent));
  WiFi.onEvent(onWiFiEvent);
  configMutex = xSemaphoreCreateMutex();

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid", true) || !request->hasParam("password", true)) {
      request->send(400, "text/plain", "Parametres Wi-Fi manquants");
      return;
    }
    const String ssid = request->getParam("ssid", true)->value();
    const String password = request->getParam("password", true)->value();
    if (!validCredential(ssid, MAX_SSID_LENGTH, false) ||
        !validCredential(password, MAX_PASSWORD_LENGTH, true)) {
      request->send(400, "text/plain", "Parametres Wi-Fi invalides");
      return;
    }

    if (configMutex == nullptr || xSemaphoreTake(configMutex, 0) != pdTRUE) {
      request->send(503, "text/plain", "Configuration Wi-Fi occupee");
      return;
    }
    pendingSsid = ssid;
    pendingPassword = password;
    credentialsPending = true;
    xSemaphoreGive(configMutex);
    request->send(202, "text/plain", "Sauvegarde demandee. Redemarrage apres validation.");
  });

  disconnectedSince = millis();
  if (configuredSsid.isEmpty()) startRecoveryAP();
  else beginStationConnection();
}

void handleWiFi() {
  const unsigned long now = millis();
  if (configMutex != nullptr && xSemaphoreTake(configMutex, 0) == pdTRUE) {
    if (credentialsPending) {
      const String ssid = pendingSsid;
      const String password = pendingPassword;
      credentialsPending = false;
      xSemaphoreGive(configMutex);

      preferences.begin("wifi", false);
      const bool ssidSaved = preferences.putString("ssid", ssid) > 0;
      const bool passwordSaved = preferences.putString("password", password) > 0 || password.isEmpty();
      preferences.end();
      if (ssidSaved && passwordSaved) {
        restartPending = true;
        restartRequestedAt = now;
      } else {
        Serial.println("[WiFi] Echec sauvegarde des identifiants");
      }
    } else {
      xSemaphoreGive(configMutex);
    }
  }
  if (restartPending || WiFi.status() == WL_CONNECTED || configuredSsid.isEmpty()) return;
  if (!lossPublished && disconnectedSince != 0 && now - disconnectedSince >= SIGNIFICANT_OUTAGE_MS) {
    lossPublished = true;
    publishEvent(WiFiRecoveryEvent::LOST);
  }
  if (!recoveryAPActive && disconnectedSince != 0 && now - disconnectedSince >= RECOVERY_AP_AFTER_MS) {
    startRecoveryAP();
  }
  if (static_cast<long>(now - nextReconnectAt) >= 0) beginStationConnection();
}

WiFiRecoveryState getWiFiRecoveryState() { return state; }
WiFiRecoveryEvent consumeWiFiRecoveryEvent() {
  WiFiRecoveryEvent event = WiFiRecoveryEvent::NONE;
  if (eventQueue != nullptr) xQueueReceive(eventQueue, &event, 0);
  return event;
}
uint32_t getWiFiReconnectCount() { return reconnectCount; }
uint32_t getLastWiFiOutageDurationSeconds() { return lastOutageDurationSeconds; }
bool isWiFiRecoveryAPActive() { return recoveryAPActive; }
bool isWiFiRestartPending() {
  return restartPending && millis() - restartRequestedAt >= RESTART_AFTER_SAVE_MS;
}
void clearWiFiRestartPending() { restartPending = false; }
