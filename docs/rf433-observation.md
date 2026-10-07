# Reception RF433 du portail

RXB6 : DATA sur GPIO27, VCC sur 3V3, GND sur GND. Moniteur serie : 115200 bauds.
La reception et le traitement RF sont locaux et independants du Wi-Fi.

## Empreintes par defaut et NVS

Au demarrage, le firmware initialise les quatre boutons avec les empreintes
validees sur le prototype, puis ouvre le namespace Preferences `portail-rf`.
Une empreinte NVS valide remplace uniquement la valeur du bouton correspondant.
Une valeur absente ou invalide conserve la valeur par defaut. Les valeurs par
defaut ne sont pas ecrites automatiquement en NVS.

```text
A : P0010111011010010010101000000011111110110011101001011
B : P0010111011001010111010101010101111110010010101011110
C : P0010111011010010010101000000110111111110000110101101
D : P0010111011010010010101000000111011111001100000011000
```

Un ESP32 avec une NVS vide reconnait donc immediatement ces telecommandes.
L'apprentissage conserve la possibilite de remplacer chaque bouton separement.

## Commandes et relais

`RF_RELAY_CONTROL_ENABLED` est `true`. A appelle exclusivement
`activerRelais(RELAY_VOITURE)` et B `activerRelais(RELAY_PIETON)`.
C et D sont reconnus sans action sur les relais. Les protections existantes
et la journalisation restent utilisees, sans contournement en cas de refus.

- `1`, `2`, `3`, `4` : apprendre A/B/C/D en maintenant le bouton environ une
  seconde. Trois trames identiques confirment la valeur ; delai de 15 secondes.
  Les doublons sont refuses. La valeur active n'est remplacee qu'apres sauvegarde
  NVS reussie ; un echec conserve l'ancienne empreinte.
- `l` : afficher les quatre empreintes actives.
- `n` : revenir en reconnaissance normale.
- `d` : diagnostic des fronts GPIO27 ; decodage suspendu.
- `r` : attendre 3 secondes, capturer 500 ms de RAW, afficher les intervalles,
  puis revenir automatiquement au mode normal.

L'anti-repetition exige 600 ms sans reconnaissance du bouton pour le relacher.
Le decodeur, les timings RF et les ISR ne sont pas modifies par l'ajout des
empreintes par defaut.

## Diagnostic et validation

`/api/diagnostic` expose `rf433` : enabled, gpio, relay_control, last_button,
last_received_ms et receive_count. Le compteur compte les reconnaissances apres
anti-repetition. Le temps est millis() lors de la reconnaissance. Ces informations
sont remises a zero au demarrage ; la reponse HTTP n'est pas mise en cache.

Sur carte, verifier les quatre empreintes avec `l`, puis A/B/C/D et
l'anti-repetition. Apprendre un bouton, redemarrer et verifier que lui seul est
remplace par sa valeur NVS. Ces essais materiels restent a effectuer.

Compilation : `pio run -e esp32dev_ota`. Cette commande compile uniquement ;
elle ne lance aucun upload OTA.
