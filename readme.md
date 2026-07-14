# PortailControl

Firmware Arduino pour une carte `esp32dev` qui pilote deux contacts secs de portail et sert une interface web locale depuis SPIFFS. Le projet commande un équipement réel : aucune route de relais ne doit être testée automatiquement.

## Matériel et fonctionnement

| Fonction | Broche | Logique actuelle |
|---|---:|---|
| Relais piéton | GPIO 16 | repos `LOW`, impulsion `HIGH` |
| Relais complet | GPIO 17 | repos `LOW`, impulsion `HIGH` |
| Capteur fermé | GPIO 34 | `HIGH` = fermé confirmé |

L'impulsion dure 500 ms par défaut ou la valeur déjà persistée dans Preferences. `/voiture` est une impulsion unique vers le contrôleur : elle ne garantit pas le sens du mouvement et ne doit pas être présentée comme une commande séparée OUVRIR ou FERMER.

Le firmware utilise ESPAsyncWebServer, AsyncTCP, ArduinoJson, SPIFFS, Preferences, mDNS, ArduinoOTA, HTTPClient, NTP et le watchdog ESP32. Le contrat exact des routes se trouve dans [`docs/api-contract.md`](docs/api-contract.md).

## PWA locale

L'interface principale est une Progressive Web App sans CDN ni dépendance Internet. Elle fournit :

- état du capteur et heure de dernière actualisation ;
- gros boutons tactiles pour l'impulsion piétonne et l'impulsion complète ;
- confirmation explicite, timeout de 5 s et anti-double-appui de 3 s ;
- aucun retry ni stockage différé d'une commande ;
- distinction entre réception HTTP et état physique confirmé ;
- manifeste, service worker et icônes 192/512 px.

Le service worker met uniquement en cache la coquille statique (`index.html`, CSS, JavaScript, manifeste et icônes). Les routes d'état, de commande, d'historique, de journal et d'administration restent réseau uniquement. Hors ligne, l'interface peut s'afficher, mais une commande échoue immédiatement et n'est jamais rejouée plus tard.

> L'installation PWA exige normalement un contexte sécurisé. Les navigateurs acceptent `localhost`, mais une adresse HTTP privée servie directement par l'ESP32 peut ne pas proposer l'installation selon le navigateur et sa version. Le manifeste et le service worker restent utilisables dès que le contexte est accepté.

## Installation comme application

1. Ouvrir l'adresse locale du portail, par exemple `http://portail.local/`.
2. Dans Chrome ou Edge, utiliser **Installer l'application** ou **Ajouter à l'écran d'accueil** lorsque l'option est proposée.
3. Lancer PortailControl depuis son icône.
4. Rester connecté au réseau Wi-Fi local du portail.

Sur Android, l'option se trouve dans le menu du navigateur. Sous Windows, elle apparaît dans la barre d'adresse ou dans **Applications**. Cette application n'est pas destinée à fonctionner depuis Internet sans TLS, authentification, segmentation réseau et architecture de sécurité dédiés.

## Configuration et secrets

Copier `include/config.example.h` vers `include/config.h`, puis renseigner localement le mot de passe d'enrôlement par défaut, le mot de passe OTA et l'URL IFTTT. `include/config.h` est ignoré par Git. Les identifiants Wi-Fi sont saisis sur le point d'accès `Portail_Config` et stockés dans Preferences.

Les secrets qui étaient auparavant présents dans l'historique Git doivent être considérés compromis et remplacés côté OTA/IFTTT. Ne jamais versionner un SSID, mot de passe, jeton, IP publique ou clé d'API.

## Compilation

Installer PlatformIO, puis depuis la racine :

```powershell
pio run
pio run --target buildfs
```

Le premier appel compile le firmware ; le second construit l'image SPIFFS avec la PWA.

## Téléversement

Pour un téléversement OTA, ajouter localement sous `[env:esp32dev]` les lignes de `platformio.local.example.ini` (sans les committer), puis lancer :

```powershell
pio run --target upload
```

Fichiers web SPIFFS :

```powershell
pio run --target uploadfs
```

Remplacer l'adresse d'exemple par l'adresse LAN de l'ESP32. Le mot de passe OTA vient de `include/config.h`. Le téléversement est une opération humaine ; il n'est pas exécuté par les tests automatiques.

## Sécurité et limites connues

- Le serveur utilise HTTP sur le port 80 sans authentification effective dans le code actuel.
- Des pages historiques mentionnent enrôlement et utilisateurs, mais plusieurs handlers correspondants sont absents ; voir le contrat d'API.
- `/pieton` et `/voiture` répondent 200 même si une impulsion est ignorée parce qu'un relais est déjà actif.
- `/set-delay` n'impose pas de bornes côté serveur et modifie une durée critique persistée.
- Le capteur ne distingue pas « ouvert » de « en mouvement » ou d'un défaut de lecture ; il confirme seulement « fermé ».

Ne pas exposer le port 80 sur Internet et ne pas tester les relais sans présence et validation humaines.
