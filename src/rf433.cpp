#include <Arduino.h>
#include <Preferences.h>
#include "FrameDecoder.h"
#include "rf433.h"

namespace {
RF433::ButtonHandler buttonHandler = nullptr;

constexpr uint8_t RX_PIN = 27;
Preferences preferences;
bool storageReady = false;
constexpr char DEFAULT_FINGERPRINTS[4][MAX_BITS + 2] = {
  "P0010111011010010010101000000011111110110011101001011",
  "P0010111011001010111010101010101111110010010101011110",
  "P0010111011010010010101000000110111111110000110101101",
  "P0010111011010010010101000000111011111001100000011000"
};
char learned[4][MAX_BITS + 2] = {};
int learning = -1;
char candidate[MAX_BITS + 2] = {};
uint8_t confirmations = 0;
unsigned long candidateAt = 0;
unsigned long learningAt = 0;
int heldButton = -1;
unsigned long lastRecognized = 0;
constexpr unsigned long RELEASE_MS = 600;
constexpr size_t FRAME_CAPACITY = MAX_BITS * 2 + 1;
int32_t frameWork[FRAME_CAPACITY];
int32_t frameReady[FRAME_CAPACITY];
size_t frameSize = 0, readySize = 0;
bool frameSynced = false, frameInvalid = false, framePending = false;
uint32_t frameLastEdge = 0;
bool diagnostic = false;
volatile uint32_t edges = 0;
portMUX_TYPE edgeMux = portMUX_INITIALIZER_UNLOCKED;
unsigned long lastReport = 0;
constexpr size_t RAW_CAPACITY = 4096;
volatile int32_t rawDurations[RAW_CAPACITY];
volatile size_t rawCount = 0;
volatile bool rawOverflow = false;
volatile bool rawActive = false;
volatile bool rawFirstEdge = true;
volatile uint32_t rawLastEdge = 0;
bool rawWaiting = false;
bool rawRunning = false;
bool rawDumping = false;
size_t rawDumpCount = 0, rawDumpIndex = 0;
unsigned long rawStarted = 0;

void IRAM_ATTR recordEdge() {
  const uint32_t now = micros();
  const int newLevel = digitalRead(RX_PIN);
  portENTER_CRITICAL_ISR(&edgeMux);
  if (rawActive) {
    if (rawFirstEdge) {
      rawFirstEdge = false;  // Le premier intervalle est incomplet.
    } else if (rawCount < RAW_CAPACITY) {
      const int32_t duration = static_cast<int32_t>(now - rawLastEdge);
      // Front descendant : la duree precedente etait au niveau haut.
      rawDurations[rawCount++] = newLevel == LOW ? duration : -duration;
    } else {
      rawOverflow = true;
      rawActive = false;
    }
    rawLastEdge = now;
  }
  portEXIT_CRITICAL_ISR(&edgeMux);
}

void IRAM_ATTR countEdge() {
  portENTER_CRITICAL_ISR(&edgeMux);
  ++edges;
  portEXIT_CRITICAL_ISR(&edgeMux);
}

// L'ISR ne decode pas et n'imprime pas : elle remet une trame au loop.
void IRAM_ATTR receiveEdge() {
  const uint32_t now = micros();
  const uint32_t duration = now - frameLastEdge;
  frameLastEdge = now;
  const int newLevel = digitalRead(RX_PIN);
  portENTER_CRITICAL_ISR(&edgeMux);
  if (duration >= 15000 && duration <= 32000 && newLevel == LOW) {
    if (frameSynced && !frameInvalid && frameSize && !framePending) {
      for (size_t i = 0; i < frameSize; ++i) frameReady[i] = frameWork[i];
      readySize = frameSize;
      framePending = true;
    }
    frameSynced = true;
    frameInvalid = false;
    frameSize = 0;
  } else if (frameSynced && !frameInvalid) {
    if (duration > 5000 || frameSize >= FRAME_CAPACITY) frameInvalid = true;
    else frameWork[frameSize++] = newLevel == LOW ? duration : -static_cast<int32_t>(duration);
  }
  portEXIT_CRITICAL_ISR(&edgeMux);
}

void startNormal() {
  detachInterrupt(digitalPinToInterrupt(RX_PIN));
  portENTER_CRITICAL(&edgeMux);
  frameSize = readySize = 0;
  frameSynced = frameInvalid = framePending = false;
  frameLastEdge = micros();
  portEXIT_CRITICAL(&edgeMux);
  diagnostic = false;
  learning = -1;
  confirmations = 0;
  candidate[0] = 0;
  heldButton = -1;
  attachInterrupt(digitalPinToInterrupt(RX_PIN), receiveEdge, CHANGE);
}

void printLearned() {
  for (int i = 0; i < 4; ++i)
    Serial.printf("%c : %s\n", 'A' + i, learned[i][0] ? learned[i] : "non appris");
}

void processFrame() {
  int32_t durations[FRAME_CAPACITY];
  size_t count = 0;
  portENTER_CRITICAL(&edgeMux);
  if (framePending) {
    count = readySize;
    for (size_t i = 0; i < count; ++i) durations[i] = frameReady[i];
    framePending = false;
  }
  portEXIT_CRITICAL(&edgeMux);
  if (!count) return;
  char fingerprint[MAX_BITS + 2];
  if (!normalizeFrame(durations, count, fingerprint)) return;
  const unsigned long now = millis();
  if (strcmp(candidate, fingerprint) || now - candidateAt > 400) {
    strcpy(candidate, fingerprint);
    confirmations = 1;
  } else if (confirmations < 3) ++confirmations;
  candidateAt = now;

  if (learning >= 0) {
    if (confirmations < 3) return;
    for (int i = 0; i < 4; ++i) {
      if (i != learning && !strcmp(learned[i], fingerprint)) {
        Serial.printf("Refuse : empreinte deja affectee a %c. Relachez puis relancez l'apprentissage.\n", 'A' + i);
        learning = -1;
        return;
      }
    }
    const char key[] = {static_cast<char>('A' + learning), 0};
    // N'ecraser l'ancienne empreinte qu'apres confirmation et sauvegarde reussie.
    if (!storageReady || preferences.putString(key, fingerprint) == 0) {
      Serial.println("Erreur de sauvegarde : ancienne empreinte conservee.");
    } else {
      strcpy(learned[learning], fingerprint);
      Serial.printf("APPRENTISSAGE %c OK : %s\n", 'A' + learning, fingerprint);
      Serial.println("Relachez le bouton. Envoyez le chiffre du bouton suivant (1=A, 2=B, 3=C, 4=D).");
    }
    heldButton = learning;
    lastRecognized = now;
    learning = -1;
    return;
  }
  for (int i = 0; i < 4; ++i) {
    if (strcmp(learned[i], fingerprint)) continue;
    if (heldButton == i) { lastRecognized = now; return; }
    if (confirmations >= 2) {
      Serial.printf(">>> BOUTON %c\n", 'A' + i);
      Serial.printf("[RF433] BOUTON %c reconnu\n", 'A' + i);
      if (buttonHandler) buttonHandler(static_cast<char>('A' + i));
      heldButton = i;
      lastRecognized = now;
    }
    return;
  }
}

} // namespace

void RF433::begin(ButtonHandler handler) {
  buttonHandler = handler;
  pinMode(RX_PIN, INPUT);
  // Les empreintes valides en NVS remplacent individuellement ces valeurs.
  for (int i = 0; i < 4; ++i) strcpy(learned[i], DEFAULT_FINGERPRINTS[i]);
  storageReady = preferences.begin("portail-rf", false);
  if (storageReady) {
    for (int i = 0; i < 4; ++i) {
      const char key[] = {static_cast<char>('A' + i), 0};
      const String value = preferences.getString(key, "");
      if (validFingerprint(value.c_str())) value.toCharArray(learned[i], sizeof learned[i]);
    }
  } else Serial.println("Erreur : stockage des empreintes indisponible.");
  startNormal();
  Serial.println("[RF433] RXB6 initialise sur GPIO27");
  Serial.println("[RF433] Mode local actif");
  Serial.println("Mode normal : reconnaissance des empreintes apprises.");
  Serial.println("Apprentissage : 1=A, 2=B, 3=C, 4=D ; envoyer le chiffre puis appuyer sur le bouton.");
  Serial.println("l : afficher les empreintes ; n : mode normal ; RESET : mode normal.");
  printLearned();
  Serial.println("Diagnostic : envoyer d dans le moniteur serie. RESET pour revenir au sniff.");
  Serial.println("Capture brute : envoyer r, puis maintenir A pendant 4 secondes.");
}

void RF433::handle() {
  // Budget fixe : une entree serie continue ne doit pas monopoliser loop().
  for (unsigned int budget = 0; budget < 8 && Serial.available(); ++budget) {
    const char command = Serial.read();
    if (rawWaiting || rawRunning || rawDumping) continue;
    if (command == 'l' || command == 'L') { printLearned(); continue; }
    if (command == 'n' || command == 'N') { startNormal(); continue; }
    if (command >= '1' && command <= '4') {
      startNormal();
      learning = command - '1';
      learningAt = millis();
      Serial.printf("Apprentissage %c : appuyez maintenant et maintenez environ 1 seconde (delai 15 s).\n", 'A' + learning);
      continue;
    }
    if (command == 'r' || command == 'R') {
      learning = -1;
      detachInterrupt(digitalPinToInterrupt(RX_PIN));
      diagnostic = false;
      rawWaiting = true;
      rawStarted = millis();
      Serial.println("Maintenez A maintenant pendant 4 secondes. Capture dans 3 secondes...");
      continue;
    }
    if ((command == 'd' || command == 'D') && !diagnostic) {
      detachInterrupt(digitalPinToInterrupt(RX_PIN));
      learning = -1;
      portENTER_CRITICAL(&edgeMux);
      edges = 0;
      portEXIT_CRITICAL(&edgeMux);
      attachInterrupt(digitalPinToInterrupt(RX_PIN), countEdge, CHANGE);
      diagnostic = true;
      lastReport = millis();
      Serial.println("DIAGNOSTIC GPIO27 : comptage des fronts, decodage suspendu.");
      Serial.println("Comparer 5 secondes au repos puis plusieurs appuis sur A.");
    }
  }
  if (rawWaiting) {
    if (millis() - rawStarted >= 3000) {
      portENTER_CRITICAL(&edgeMux);
      rawCount = 0;
      rawOverflow = false;
      rawFirstEdge = true;
      rawActive = true;
      portEXIT_CRITICAL(&edgeMux);
      rawStarted = millis();
      attachInterrupt(digitalPinToInterrupt(RX_PIN), recordEdge, CHANGE);
      rawWaiting = false;
      rawRunning = true;
    }
    return;
  }
  if (rawRunning) {
    if (millis() - rawStarted >= 500) {
      portENTER_CRITICAL(&edgeMux);
      rawActive = false;
      const size_t count = rawCount;
      const bool overflow = rawOverflow;
      portEXIT_CRITICAL(&edgeMux);
      detachInterrupt(digitalPinToInterrupt(RX_PIN));
      Serial.printf("\nRAW_BEGIN GPIO27 | fenetre 500 ms | %u intervalles | saturation=%s\n",
                    static_cast<unsigned int>(count), overflow ? "oui" : "non");
      Serial.println("Microsecondes : positif=HAUT, negatif=BAS. Premier et dernier intervalles incomplets exclus.");
      rawDumpCount = count;
      rawDumpIndex = 0;
      rawDumping = true;
      rawRunning = false;
      startNormal();
    }
    return;
  }
  if (rawDumping) {
    // Au plus 8 intervalles par passage, et seulement si le tampon UART a de la place.
    for (unsigned int budget = 0; budget < 8 && rawDumpIndex < rawDumpCount; ++budget) {
      char text[20];
      const size_t i = rawDumpIndex;
      const int length = snprintf(text, sizeof(text), "%ld%s%s",
          static_cast<long>(rawDurations[i]), i + 1 < rawDumpCount ? "," : "",
          (i + 1) % 16 == 0 ? "\n" : "");
      if (Serial.availableForWrite() < length) break;
      Serial.write(reinterpret_cast<const uint8_t *>(text), length);
      ++rawDumpIndex;
    }
    if (rawDumpIndex == rawDumpCount) {
      Serial.println("\nRAW_END");
      Serial.println("Vous pouvez relacher A. r pour recommencer ; mode normal retabli.");
      rawDumping = false;
    }
    return;
  }
  if (diagnostic) {
    const unsigned long now = millis();
    if (now - lastReport >= 1000) {
      const unsigned long elapsed = now - lastReport;
      lastReport = now;
      portENTER_CRITICAL(&edgeMux);
      const uint32_t count = edges;
      edges = 0;
      portEXIT_CRITICAL(&edgeMux);
      Serial.printf("GPIO27 : %lu fronts / %lu ms | niveau instantane : %d\n",
                    static_cast<unsigned long>(count), elapsed, digitalRead(RX_PIN));
    }
    return;
  }
  const unsigned long now = millis();
  if (heldButton >= 0 && now - lastRecognized >= RELEASE_MS) heldButton = -1;
  if (learning >= 0 && now - learningAt >= 15000) {
    Serial.println("Apprentissage expire : aucune empreinte confirmee, ancienne valeur conservee.");
    learning = -1;
  }
  processFrame();
}
