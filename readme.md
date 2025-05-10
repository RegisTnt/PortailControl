# 🚪 Projet Portail ESP32

Commande de deux relais (piéton et voiture) via interface web sur ESP32.
Interface hébergée dans SPIFFS, pilotée par `AsyncWebServer`.

## 📁 Arborescence du projet


## ⚙️ Fonctionnalités prévues

- Connexion Wi-Fi automatique
- Commande relais piéton/voiture
- Interface Web locale (HTML SPIFFS)
- OTA à venir (mise à jour par WiFi)

## 🚀 En cours

- [X] Connexion WiFi
- [X] Contrôle relais
- [X] Page Web SPIFFS
- [ ] Ajout OTA
- [ ] Commande Bluetooth secours

## 🔐 Informations sensibles

> **Ne pas oublier** de cacher les identifiants Wi-Fi (à terme, utiliser `secrets.h` non versionné via `.gitignore`)
>
