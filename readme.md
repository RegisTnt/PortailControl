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
>
> # 🚪 PortailControl ESP32
>
> Contrôleur de portail à base d'ESP32, avec interface web, mise à jour OTA, SPIFFS et gestion automatique du WiFi.
>
> ---
>
> ## ✅ Version stable
>
> 🟢 Version actuelle : **`v1.2-relais-repos`**
>
>> 🔁 Les relais sont désormais **au repos à LOW** pour éviter la chauffe à la mise sous tension.
>>
>
> ---
>
> ## 🔧 Fonctionnalités
>
> - Contrôle piéton et voiture via deux relais
> - Interface web épurée (hébergée en SPIFFS)
> - Configuration WiFi via fallback en mode AP
> - Mise à jour OTA (Over-The-Air) possible
> - Accès via `http://portail.local` grâce à mDNS
>
> ---
>
> ## 🖼️ Aperçu
>
> 📘 [Voir la notice PDF](doc/Notice%20portail.pdf)
>
> ---
>
> ## 🚀 Déploiement
>
> 1. Cloner le dépôt :
>
> ```bash
> git clone https://github.com/RegisTnt/PortailControl.git
> cd PortailControl
> ```
