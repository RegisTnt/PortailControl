
🚪 PortailControl ESP32

Système de commande de portail basé sur ESP32, avec interface web locale, OTA, SPIFFS, sécurité par enrôlement, et accès distant sécurisé.


---

✅ Version actuelle

🟢 v1.3-token-securite

> 🔒 Nouveautés :

Enrôlement utilisateur avec token sécurisé

Authentification automatique par cookie

Réglage du délai relais

Interface /admin pour mise à jour du mot de passe maître (seulement sur réseau local)

Téléchargement / restauration des utilisateurs (users.json)

Redirection automatique vers /enrol si non authentifié





---

⚙️ Fonctionnalités

🔐 Enrôlement sécurisé avec token unique stocké dans SPIFFS

🖱️ Commande des relais via interface web moderne

🌐 Accès local via mDNS (http://portail.local)

🔁 OTA (mise à jour firmware sans fil)

📁 Interface web hébergée sur SPIFFS (HTML/CSS/JS)

⚙️ Interface de configuration (/settings)

🔧 Réglage du temps d’activation des relais

👥 Interface de gestion des utilisateurs (/users)

🛡️ Page admin protégée accessible uniquement sur le bon réseau Wi-Fi

💾 Sauvegarde / restauration des utilisateurs (users.json)



---

📁 Arborescence du projet

PortailControl/
├── data/
│   ├── index.html
│   ├── settings.html
│   ├── enrol.html
│   ├── users.html
│   ├── admin.html
│   └── users.json         <-- Tokens enrôlés
├── src/
│   ├── main.cpp           <-- Code principal
│   ├── wifi_manager.cpp/h <-- Gestion Wi-Fi + fallback AP
├── include/
│   └── secrets.h          <-- (Wi-Fi privés, ignoré par git)
├── lib/                   <-- Libs éventuelles
├── platformio.ini         <-- Config PlatformIO
├── .gitignore
└── README.md              <-- Ce fichier


---

🖼️ Interfaces Web

index.html : commande portail

settings.html : réglages relais et accès

users.html : liste et suppression des tokens

admin.html : changement du mot de passe maître

enrol.html : enrôlement via mot de passe

/download-users : export de la base users.json

/upload-users : restauration après mise à jour



---

🚀 Déploiement (PlatformIO)

git clone https://github.com/RegisTnt/PortailControl.git
cd PortailControl
pio run --target uploadfs  # Pour SPIFFS
pio run --target upload    # Pour firmware


---

🔌 Branchement au portail Extel

Testé avec succès sur un moteur Extel piloté par contact sec :

Action	Relais	Bornes à relier sur carte portail

Ouverture piéton	RELAY_PIETON	PHO ↔ entrée piéton
Ouverture totale	RELAY_VOITURE	PHO ↔ entrée voiture


> ⚠️ Vérifie que ton relais fournit bien un contact sec (pas de tension) avant branchement.




---

🧪 Diagnostic relais : éviter la chauffe

Les relais doivent rester au repos lors du démarrage pour éviter :

surconsommation

déclenchement accidentel

usure prématurée


Procédure :

1. Déconnecte l’alim du module


2. Identifie la borne commune (COM) du relais


3. Utilise un multimètre en mode continuité :

repère le groupe où COM est fermé au repos (avec NC)

relie NO et COM au portail pour qu’ils ne se ferment qu’à l’activation




Code modifié :

#define RELAIS_REPOS LOW
#define RELAIS_ACTIVE HIGH


---

🔐 Enrôlement et sécurité

Un mot de passe maître (changemoi par défaut) est demandé pour enrôler un utilisateur

Chaque utilisateur reçoit un token stocké dans un cookie auth_token

Ce cookie est ensuite comparé à ceux du fichier users.json

🔄 Authentification automatique après enrôlement

Seuls les utilisateurs enrôlés peuvent accéder aux routes sensibles (/, /voiture, /pieton, etc.)



---

🔄 Administration

Accessible uniquement si l’ESP32 est connecté au Wi-Fi déclaré dans le code (WIFI_MAISON).

Fonctionnalité :

Changement du mot de passe maître

Accessible via /admin

Protection : vérification du WiFi.SSID()



---

💾 Sauvegarde & Restauration utilisateurs

Permet de conserver les accès après mise à jour du firmware ou réinstallation :

/download-users → télécharge users.json

/upload.html + formulaire → téléverse users.json


⚠️ Ne pas éditer le fichier manuellement sans valider la structure JSON.


---

🌍 Accès à distance

Fonctionne via redirection de port :

Exemple : https://votre_ip_publique:8080

NAT configuré sur la box vers IP locale de l'ESP32

Recommandé : choisir un port non standard (> 1024)

À terme, prévoir une validation par géolocalisation ou token partagé

