#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

enum class WiFiRecoveryState { CONNECTED, DISCONNECTED, RECONNECTING, RECOVERY_AP };
enum class WiFiRecoveryEvent { NONE, LOST, RECONNECTED, RECOVERY_AP_STARTED };

extern AsyncWebServer server;
void setupWiFi();
void handleWiFi();
WiFiRecoveryState getWiFiRecoveryState();
WiFiRecoveryEvent consumeWiFiRecoveryEvent();
uint32_t getWiFiReconnectCount();
bool isWiFiRecoveryAPActive();
bool isWiFiRestartPending();
void clearWiFiRestartPending();

#endif
