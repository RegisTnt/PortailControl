'use strict';

const COMMAND_LOCK_MS = 3000;
const REQUEST_TIMEOUT_MS = 5000;
const STATE_REFRESH_DELAY_MS = 1200;

const elements = {
  network: document.querySelector('#networkBadge'),
  status: document.querySelector('#gateStatus'),
  detail: document.querySelector('#statusDetail'),
  refreshed: document.querySelector('#lastRefresh'),
  message: document.querySelector('#commandMessage'),
  pedestrian: document.querySelector('#pedestrianButton'),
  vehicle: document.querySelector('#vehicleButton'),
  refresh: document.querySelector('#refreshButton'),
  install: document.querySelector('#installButton'),
  installHint: document.querySelector('#installHint')
};

let commandLocked = false;
let portalReachable = false;
let installPrompt = null;

async function directFetch(path) {
  if (!navigator.onLine) throw new Error('Le navigateur est hors ligne. Commande non envoyée.');
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    return await fetch(path, { method: 'GET', cache: 'no-store', signal: controller.signal });
  } catch (error) {
    if (error.name === 'AbortError') throw new Error('Délai de réponse dépassé. Ne renvoyez pas automatiquement la commande.');
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

function setNetwork(kind, label) {
  elements.network.className = `badge badge-${kind}`;
  elements.network.textContent = label;
}

function showMessage(text, kind = '') {
  elements.message.className = kind ? `message message-${kind}` : 'message';
  elements.message.textContent = text;
}

async function refreshState() {
  elements.refresh.disabled = true;
  try {
    const response = await directFetch('/etat');
    if (!response.ok) throw new Error(`Lecture refusée par l’ESP32 (HTTP ${response.status}).`);
    const value = (await response.text()).trim().toLowerCase();
    elements.status.className = 'gate-status';
    if (value === 'ferme') {
      elements.status.classList.add('status-closed');
      elements.status.textContent = 'Portail fermé';
      elements.detail.textContent = 'Le capteur de fin de course confirme la position fermée.';
    } else if (value === 'ouvert') {
      elements.status.classList.add('status-open');
      elements.status.textContent = 'Non confirmé fermé';
      elements.detail.textContent = 'Le capteur ne confirme pas la fermeture : portail ouvert ou en mouvement.';
    } else {
      throw new Error(`Réponse d’état inconnue : ${value || 'vide'}.`);
    }
    elements.refreshed.textContent = `Dernière actualisation : ${new Date().toLocaleTimeString()}`;
    setNetwork('ok', 'ESP32 joignable');
    portalReachable = true;
  } catch (error) {
    elements.status.className = 'gate-status status-unknown';
    elements.status.textContent = 'État indisponible';
    elements.detail.textContent = error.message || 'ESP32 inaccessible.';
    setNetwork(navigator.onLine ? 'error' : 'warn', navigator.onLine ? 'ESP32 inaccessible' : 'Hors ligne');
    portalReachable = false;
  } finally {
    elements.refresh.disabled = false;
    lockCommands(commandLocked);
  }
}

function lockCommands(locked) {
  commandLocked = locked;
  elements.pedestrian.disabled = locked || !portalReachable;
  elements.vehicle.disabled = locked || !portalReachable;
}

async function sendCommand(path, label) {
  if (commandLocked) return;
  if (!portalReachable) {
    showMessage('Actualisez d’abord l’état et vérifiez que l’ESP32 est joignable.', 'error');
    return;
  }
  if (!window.confirm(`Confirmer ${label} ?\n\nCette action transmet une impulsion à un relais réel.`)) return;

  lockCommands(true);
  showMessage('Transmission en cours…');
  try {
    const response = await directFetch(path);
    const body = await response.text();
    if (!response.ok) throw new Error(`Commande refusée (HTTP ${response.status}) : ${body}`);
    showMessage('Commande reçue par l’ESP32. Le mouvement et l’action effective du relais ne sont pas confirmés par cette réponse.', 'success');
    setTimeout(refreshState, STATE_REFRESH_DELAY_MS);
  } catch (error) {
    showMessage(`${error.message || 'Aucune réponse de l’ESP32.'} Aucun nouvel essai automatique ne sera effectué.`, 'error');
  } finally {
    setTimeout(() => lockCommands(false), COMMAND_LOCK_MS);
  }
}

elements.pedestrian.addEventListener('click', () => sendCommand('/pieton', 'l’impulsion piétonne'));
elements.vehicle.addEventListener('click', () => sendCommand('/voiture', 'l’impulsion complète'));
elements.refresh.addEventListener('click', refreshState);
window.addEventListener('online', refreshState);
window.addEventListener('offline', () => setNetwork('warn', 'Hors ligne'));

window.addEventListener('beforeinstallprompt', event => {
  event.preventDefault();
  installPrompt = event;
  elements.install.hidden = false;
});

elements.install.addEventListener('click', async () => {
  if (!installPrompt) return;
  installPrompt.prompt();
  await installPrompt.userChoice;
  installPrompt = null;
  elements.install.hidden = true;
});

window.addEventListener('appinstalled', () => {
  elements.install.hidden = true;
  elements.installHint.textContent = 'PortailControl est installé.';
});

if (window.matchMedia('(display-mode: standalone)').matches) {
  elements.installHint.textContent = 'Application installée en mode autonome.';
}

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => navigator.serviceWorker.register('/service-worker.js')
    .catch(() => showMessage('Le mode hors ligne de l’interface n’a pas pu être activé.', 'error')));
}

refreshState();
