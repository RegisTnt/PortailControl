# Contrat HTTP de PortailControl 1.0.3-recovery

Ce document décrit le firmware présent dans `src/main.cpp` et `src/wifi_manager.cpp` au 14 juillet 2026. Le serveur écoute en HTTP non chiffré sur le port 80. Les corps sont du texte brut sauf indication contraire. Aucune route n'applique actuellement d'authentification, même si certaines pages et l'ancien README laissent entendre le contraire.

## Modèle de commande physique

- `GPIO 16` pilote le relais piéton ; `GPIO 17` pilote le relais complet.
- Le niveau de repos est `LOW` et le niveau actif `HIGH`.
- L'impulsion vaut 500 ms par défaut, ou la valeur persistée dans Preferences sous `portail/delai`.
- Un seul relais peut être actif à la fois. Si une seconde demande arrive pendant une impulsion, elle est ignorée par `activerRelais`, mais la route répond tout de même HTTP 200. Le client ne peut donc pas déduire que le relais a réellement été actionné.
- `GPIO 34`, en entrée, confirme seulement la position fermée quand il est `HIGH`. La réponse `ouvert` signifie en réalité « non confirmé fermé » : ouvert, en mouvement ou capteur non actif.
- L'entrée complète est une impulsion de commande du contrôleur de portail. Elle ne garantit pas un sens ouvrir/fermer.

Niveaux de danger utilisés : **faible** (lecture), **modéré** (données/configuration), **élevé** (relais ou redémarrage).

## 1. Lecture sans effet physique

| Méthode | Chemin | Rôle et paramètres | Réponse, MIME et exemple | Codes observables | Répétition et temporisation |
|---|---|---|---|---|---|
| GET | `/etat` | Lit l'état stable débouncé de `GPIO 34`. Aucun paramètre. | `text/plain`, `ferme` ou `ouvert`. | 200. | Faible, répétable. Réseau direct, jamais de cache. |
| GET | `/api/diagnostic` | État recovery, Wi-Fi, mémoire, relais, capteur brut/stable et horloge. | `application/json`. | 200. | Faible, répétable. Réseau direct, jamais de cache. |
| GET | `/log.txt` | Lit le journal SPIFFS. | `text/plain`, horodatage `epoch:<unix>` ou `boot:<id>+<uptime>`. | 200, 404 si absent, 500 si ouverture impossible. | Faible, répétable sur action utilisateur. Réseau direct, jamais de cache. |
| GET | `/get-users` | Lit la table JSON des utilisateurs. | `application/json`, `{}` si le fichier n'existe pas. | 200. | Modéré : expose les jetons éventuels. Pas de polling. |
| GET | `/users.json` | Lit directement la table JSON. | `application/json`, ex. `{}`. | 200, ou 404 avec `{}`. | Modéré : expose les jetons éventuels. Pas de cache. |
| GET | `/download-users` | Télécharge `users.json`. | `application/json`, pièce jointe `users.json`. | 200 ou 404 `text/plain`. | Modéré. Action humaine uniquement. |
| GET | `/` | Sert `index.html` en fonctionnement normal ; sert `wifi_config.html` si le gestionnaire Wi-Fi a enregistré sa route AP en premier. | `text/html`. | 200 ; échec de fichier possible si SPIFFS est incomplet. | Faible. Seule la coquille PWA principale est mise en cache. |
| GET | `/settings` | Sert `settings.html`. | `text/html`. | 200 ; échec de fichier possible. | Faible, réseau direct. |
| GET | `/historique` | Sert `historique.html`, qui lit ensuite `/log.txt`. | `text/html`. | 200 ; échec de fichier possible. | Faible, réseau direct car contenu associé dynamique. |
| GET | `/admin` | Sert `admin.html`. | `text/html`. | 200 ; échec de fichier possible. | Modéré : page non protégée. Réseau direct. |
| GET | `/users` | Sert `users.html`. | `text/html`. | 200 ; échec de fichier possible. | Modéré : page non protégée. Réseau direct. |
| GET | `/wifi_config` | Sert `wifi_config.html`. | `text/html`. | 200 ; échec de fichier possible. | Modéré : page de saisie Wi-Fi non protégée. Réseau direct. |
| GET | `/upload`, `/upload.html` | Sert `upload.html`. | `text/html`. | 200 ; échec de fichier possible. | Modéré. La route de traitement annoncée par la page n'existe pas. |
| GET | `/enrol`, `/enrol.html` | Sert `enrol.html`. | `text/html`. | 200 ; échec de fichier possible. | Modéré. La route POST annoncée par la page n'existe pas. |
| GET | chemin de fichier SPIFFS | Le gestionnaire `onNotFound` sert tout chemin existant, notamment `/app.css`, `/app.js`, `/service-worker.js`, `/manifest.webmanifest` et `/icons/*.png`. | MIME selon extension : HTML, CSS, JavaScript, JSON, manifeste ou PNG ; sinon `text/plain`. | 200 si le fichier existe, sinon 404 `Page introuvable`. | Faible pour les assets statiques. |

## 2. Commande avec effet physique

| Méthode | Chemin | Rôle et paramètres | Réponse et MIME | Codes observables | Effet, danger, répétition et temporisation |
|---|---|---|---|---|---|
| GET | `/pieton` | Demande l'impulsion piétonne. Aucun paramètre. | `text/plain`, `Piéton activé !`. | Toujours 200 après traitement de la route. | **GPIO 16 / relais réel, danger élevé. Non idempotent : ne jamais répéter automatiquement.** Attendre au moins 3 s côté UI et exiger une nouvelle action humaine. Relire `/etat`, sans en déduire le mouvement immédiat. |
| GET | `/voiture` | Demande l'impulsion complète. Aucun paramètre. | `text/plain`, `Voiture activée !`. | Toujours 200 après traitement de la route. | **GPIO 17 / relais réel, danger élevé. Non idempotent : ne jamais répéter automatiquement.** Cette impulsion ne garantit pas la direction. Attendre au moins 3 s côté UI et exiger une nouvelle action humaine. |

Ces routes ne journalisent que les commandes acceptées par `activerRelais`. Si un relais est déjà actif, elles répondent HTTP 409. Une réponse 200 confirme l'impulsion, jamais le déplacement physique du portail.

## 3. Administration ou maintenance

| Méthode | Chemin | Paramètres | Réponse, MIME et exemple | Codes observables | Effet, danger et répétition |
|---|---|---|---|---|---|
| POST | `/api/time` | Corps formulaire `epoch`, strictement compris entre 2020-01-01 et 2100-01-01. | 204, ou erreur JSON. | 204, 400, 500. | Règle uniquement l'horloge RAM ; aucune écriture flash et aucun effet portail. |
| POST | `/set-delay` | Corps formulaire numérique `delai`, validé entre 100 et 5000 ms. | Redirection vers `/settings`. | 302 ou 400. | **Modifie la durée physique persistée du relais : danger élevé.** Action administrative non répétable automatiquement. |
| GET | `/clear-log` | Aucun paramètre. | `text/plain`, effacement mis en file. | 202 ou 503. | Destructif, modéré. Confirmation humaine obligatoire, jamais automatisé. |
| GET | `/delete-user?name=...` | Query `name` obligatoire. | Redirection `/users` ou erreur `text/plain`. | 302, 400, 500. | Destructif, modéré. Non idempotent et sans authentification. |
| POST | `/save` | Disponible quand le mode point d'accès est initialisé. Corps formulaire `ssid`, `password`. | `text/plain`, `Sauvegarde réussie. Redémarrage...`. | 200 ; paramètres absents susceptibles de provoquer une erreur interne. | **Enregistre les identifiants Wi-Fi puis redémarre l'ESP32 : danger élevé.** Action humaine locale uniquement, aucun retry. |

## Routes référencées mais absentes

Les fichiers web contiennent encore des appels ou formulaires vers `POST /enrol`, `POST /update-pwd` et une restauration d'utilisateurs, mais aucun handler correspondant n'est déclaré dans le firmware actuel. Ils tombent donc sur le fallback et reçoivent 404. Aucune validation de cookie `auth_token` n'est exécutée. Ces éléments ne font pas partie du contrat disponible.

## Règles pour les clients

- Ne jamais rejouer automatiquement une commande physique, même après timeout ou perte de réponse.
- Ne jamais faire de retry automatique sur `/pieton` ou `/voiture` et ne jamais les mettre en file hors ligne.
- Utiliser un timeout borné ; 5 s est la valeur de la PWA.
- Pour `/etat`, la PWA utilise un timeout de 1,8 s et un polling non chevauchant : 2 s au premier plan, 15 s en arrière-plan. Deux échecs consécutifs rendent les commandes indisponibles ; une réponse valide rétablit automatiquement l'état connecté.
- Vérifier la connexion Wi-Fi et l'accessibilité de l'adresse locale avant d'activer les commandes.
- Considérer chaque appui volontaire sur un bouton actif comme l'intention d'envoyer exactement une impulsion ; la PWA ne présente pas de modale intermédiaire.
- Appliquer un anti-double-clic ; la PWA utilise 3 s, sans prétendre que ce délai couvre le cycle mécanique.
- Relire `/etat` après une commande, tout en sachant que ce capteur confirme uniquement la fermeture.
- Distinguer une absence de réponse (résultat inconnu) d'un refus HTTP explicite.
- Distinguer trois niveaux : demande reçue par le handler, relais possiblement actionné, état physique confirmé par le capteur.
- Un libellé contextuel « Fermer le portail » peut être affiché lorsque le capteur ne confirme pas la fermeture, mais `/voiture` reste une impulsion non directionnelle et ne garantit jamais la fermeture.
- Garder l'API sur le réseau local. HTTP sur Internet, redirection de port et exposition des routes sans authentification sont interdits sans architecture de sécurité dédiée.

## Services non HTTP et reprise après erreur

- Wi-Fi : identifiants stockés dans Preferences ; tentative initiale de 10 s, puis point d'accès `Portail_Config`; auto-reconnexion ESP32 et contrôle toutes les 10 s.
- mDNS : `portail.local`.
- OTA Arduino : hostname `portail-esp32`, mot de passe local dans `include/config.h`.
- Watchdog : 30 s, alimenté dans `loop()`.
- Journal série : 115200 bauds ; journal persistant SPIFFS en CSV séparé par `;`.
# Détails de fiabilisation locale

## Stable gate state

`GET /etat` returns `ferme` or `ouvert` from the debounced state. GPIO34 is
configured as `INPUT` because this ESP32 input has no usable internal pull-up or
pull-down. A raw change must remain unchanged for 400 ms before it becomes the
stable state.

`GET /api/diagnostic` exposes `portal.sensor_raw`, `portal.sensor_stable` and
`portal.sensor_bounce_count`. The counter records rejected pending states; it is
diagnostic data and is never written to SPIFFS.

## Local clock

`POST /api/time` accepts a form field named `epoch`. Values are restricted to
2020-01-01 through 2100-01-01. The value adjusts RAM/system time only; it is not
persisted and portal operation never depends on it.

Until a browser supplies time, log timestamps use `boot:<boot_count>+<uptime>`.
After synchronization they use `epoch:<unix_seconds>`. This prevents deceptive
1970 dates and keeps pre-reboot session timestamps unambiguous.

## History policy

GPIO transitions are logged only after debounce as `PORTAIL_OUVERT` and
`PORTAIL_FERME;duration=<seconds>`. Wi-Fi loss is logged only after 30 seconds;
recovery includes the outage duration. `/log.txt` remains capped at 64 KiB and
rotates once to `/log.old.txt` under the SPIFFS mutex.
