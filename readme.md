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
- [X] Ajout OTA
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
>
> ## 🔌 Branchement au portail Extel
>
> Ce projet a été testé avec succès sur un automatisme **Extel** acceptant un **contact sec** pour piloter l'ouverture piétonne ou totale. Le module ESP32, via ses deux relais, simule l'appui sur un bouton physique en **fermant brièvement le circuit entre les bornes** du portail :
>
> | Action            | Bornes à relier via relais |
> | ----------------- | --------------------------- |
> | Ouverture piéton | `PHO`↔`⏴ piéton`     |
> | Ouverture totale  | `PHO`↔`⏴ voiture`     |
>
> Les relais sont configurés pour rester fermés pendant une durée réglable via l’interface `/settings`, reproduisant un  **contact momentané de 0,5 à 1 seconde** .
>
>> ⚠️ Assurez-vous que votre module relais fournit un **contact sec isolé** et ne transmet **aucune tension** vers le bornier du portail.
>>
>>
>
>
>
> ## 🧪 Diagnostic des relais et prévention de chauffe
>
> Avant tout branchement au portail, il est essentiel de s'assurer que les **relais sont bien au repos** à la mise sous tension. En effet, un relais inutilement activé en continu entraînera une  **chauffe du module** , une **usure prématurée** et une  **consommation inutile** . Pour vérifier cela, j'ai utilisé un multimètre en mode **test de continuité** hors alimentation.
>
> Chaque module relais dispose de **trois bornes** :
>
> * **NC** (Normally Closed – normalement fermé)
> * **COM** (commun, au centre)
> * **NO** (Normally Open – normalement ouvert)
>
> En testant les combinaisons **entre les trois bornes** sans alimenter la carte, j’ai identifié le  **groupe de bornes où le contact est fermé au repos** . J’ai ensuite relié les **deux bornes non connectées au repos** (`NO` et `COM`) à la platine du portail. Ainsi, lors de l’activation, le relais ferme brièvement le circuit, simulant un **contact sec momentané** comme un bouton poussoir.
>
> Enfin, le fichier `main.cpp` a été adapté pour inverser la logique des relais : leur **repos est désormais défini comme `LOW`** dans le code, et l’activation comme `HIGH`. Cela garantit un fonctionnement silencieux, sans déclenchement parasite à la mise sous tension, et sans sollicitation inutile des bobines relais.
>
>
> ## 🌍 Accès distant au portail via Internet
>
> Le système ESP32 de commande du portail a été configuré pour être accessible à distance depuis n’importe quelle connexion Internet (4G, Wi-Fi externe, etc.). Cette fonctionnalité repose sur la redirection de port (NAT/PAT) configurée sur la box Internet. L’interface web reste donc disponible hors du réseau local, ce qui permet d’ouvrir le portail à distance en toute simplicité.
>
> Pour des raisons de sécurité, un port externe distinct du port par défaut a été utilisé, et l’adresse publique n’est volontairement pas indiquée dans ce document. Il est recommandé d’ajouter à l’avenir une authentification légère et de restreindre les accès par géolocalisation ou token partagé.
