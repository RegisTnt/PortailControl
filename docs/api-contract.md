# Contrat HTTP de PortailControl

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
| GET | `/etat` | Lit `GPIO 34`. Aucun paramètre. | `text/plain`, `ferme` ou `ouvert`. | 200. | Faible, répétable. 1 s minimum recommandé pour un client interactif. Réseau direct, jamais de cache. |
| GET | `/api/weather` | Lit la dernière météo valide de Toulon conservée en RAM. Aucun accès Internet n'est déclenché par cette route. | `application/json`. Disponible : `{"available":true,"last_error":0,"temperature_2m":27.3,"apparent_temperature":28.1,"weather_code":1,"observed_at":"2026-07-14T17:15","age_seconds":42}`. Indisponible : `{"available":false,"last_error":-1}`. | 200 avec une valeur valide, 503 avant le premier succès. `last_error` vaut 0 après un succès et sert au diagnostic réseau en cas d'échec. | Faible, répétable. Réseau direct, jamais de cache. Les données sont perdues au redémarrage puis récupérées après connexion Wi-Fi. |
| GET | `/log.txt` | Lit le journal SPIFFS. | `text/plain`, ex. `Horodatage;État\n2026-07-14 10:20:30;ferme`. En-tête `Content-Disposition: inline`. | 200, 404 si absent, 500 si ouverture impossible. | Faible, répétable sur action utilisateur. Réseau direct, jamais de cache. |
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

Ces routes journalisent la demande avant d'appeler `activerRelais`. Une réponse 200 signifie « requête traitée par le handler », pas « relais actionné » ni « portail déplacé ». Si un relais est déjà actif, la nouvelle impulsion est ignorée mais le code HTTP reste 200.

## 3. Administration ou maintenance

| Méthode | Chemin | Paramètres | Réponse, MIME et exemple | Codes observables | Effet, danger et répétition |
|---|---|---|---|---|---|
| POST | `/set-delay` | Corps formulaire `delai`. La valeur est convertie par `toInt()` sans validation serveur. | Redirection vers `/settings` (corps HTML de redirection). | 302. | **Modifie la durée physique persistée du relais : danger élevé.** Action administrative non répétable automatiquement. L'UI actuelle propose 100–5000 ms, mais le serveur n'impose pas ces bornes. |
| POST | `/set-notifications` | `notif_ouverture`, `notif_rappel` (présence = vrai), `delai_rappel` en minutes. | Redirection vers `/settings`. | 302. | Modéré. Persisté dans Preferences. Pas de retry automatique. |
| GET | `/clear-log` | Aucun paramètre. | `text/plain`, `Historique effacé !`. | 200, y compris si la recréation du fichier échoue. | Destructif, modéré. Confirmation humaine obligatoire, jamais automatisé. |
| GET | `/delete-user?name=...` | Query `name` obligatoire. | Redirection `/users` ou erreur `text/plain`. | 302, 400, 500. | Destructif, modéré. Non idempotent et sans authentification. |
| POST | `/save` | Disponible quand le mode point d'accès est initialisé. Corps formulaire `ssid`, `password`. | `text/plain`, `Sauvegarde réussie. Redémarrage...`. | 200 ; paramètres absents susceptibles de provoquer une erreur interne. | **Enregistre les identifiants Wi-Fi puis redémarre l'ESP32 : danger élevé.** Action humaine locale uniquement, aucun retry. |

## Routes référencées mais absentes

Les fichiers web contiennent encore des appels ou formulaires vers `POST /enrol`, `POST /update-pwd` et une restauration d'utilisateurs, mais aucun handler correspondant n'est déclaré dans le firmware actuel. Ils tombent donc sur le fallback et reçoivent 404. Aucune validation de cookie `auth_token` n'est exécutée. Ces éléments ne font pas partie du contrat disponible.

## Règles pour les clients

- Ne jamais rejouer automatiquement une commande physique, même après timeout ou perte de réponse.
- Ne jamais faire de retry automatique sur `/pieton` ou `/voiture` et ne jamais les mettre en file hors ligne.
- Utiliser un timeout borné ; 5 s est la valeur de la PWA.
- Vérifier la connexion Wi-Fi et l'accessibilité de l'adresse locale avant d'activer les commandes.
- Demander une confirmation utilisateur immédiatement avant chaque commande physique.
- Appliquer un anti-double-clic ; la PWA utilise 3 s, sans prétendre que ce délai couvre le cycle mécanique.
- Relire `/etat` après une commande, tout en sachant que ce capteur confirme uniquement la fermeture.
- Distinguer une absence de réponse (résultat inconnu) d'un refus HTTP explicite.
- Distinguer trois niveaux : demande reçue par le handler, relais possiblement actionné, état physique confirmé par le capteur.
- Ne jamais présenter `/voiture` comme une commande garantie « ouvrir » ou « fermer ».
- Garder l'API sur le réseau local. HTTP sur Internet, redirection de port et exposition des routes sans authentification sont interdits sans architecture de sécurité dédiée.

## Services non HTTP et reprise après erreur

- Wi-Fi : identifiants stockés dans Preferences ; tentative initiale de 10 s, puis point d'accès `Portail_Config`; auto-reconnexion ESP32 et contrôle toutes les 10 s.
- Météo : tâche FreeRTOS indépendante après connexion Wi-Fi, puis toutes les 15 minutes après un succès. Tant qu'aucune valeur valide n'existe, une tentative est refaite après 1 minute. Open-Meteo est interrogé pour Toulon (`43.1242`, `5.9280`) avec un timeout de 4 s. Un échec conserve la dernière valeur valide et n'interagit jamais avec les relais.
- mDNS : `portail.local`.
- OTA Arduino : hostname `portail-esp32`, mot de passe local dans `include/config.h`.
- Watchdog : 30 s, alimenté dans `loop()`.
- Redémarrage quotidien : 03:00 heure issue de NTP (`UTC+1` fixe dans le code).
- Journal série : 115200 bauds ; journal persistant SPIFFS en CSV séparé par `;`.
- Notifications : webhook IFTTT sur changement d'état si activé et rappel configurable si non fermé. Aucune intégration Home Assistant, Alexa ou MQTT n'a été trouvée.
