'use strict';

const COMMAND_LOCK_MS = 3000;
const REQUEST_TIMEOUT_MS = 5000;
const STATE_REFRESH_DELAY_MS = 1200;

const elements = {
  network: document.querySelector('#networkBadge'),
  weatherTemperature: document.querySelector('#weatherTemperature'),
  weatherDetail: document.querySelector('#weatherDetail'),
  weatherAge: document.querySelector('#weatherAge'),
  statusCard: document.querySelector('#statusCard'),
  status: document.querySelector('#gateStatus'),
  detail: document.querySelector('#statusDetail'),
  refreshed: document.querySelector('#lastRefresh'),
  message: document.querySelector('#commandMessage'),
  pedestrian: document.querySelector('#pedestrianButton'),
  vehicle: document.querySelector('#vehicleButton'),
  refresh: document.querySelector('#refreshButton'),
  historyRefresh: document.querySelector('#historyRefresh'),
  historyState: document.querySelector('#historyState'),
  historyList: document.querySelector('#historyList'),
  dialog: document.querySelector('#confirmDialog'),
  dialogTitle: document.querySelector('#dialogTitle'),
  dialogText: document.querySelector('#dialogText'),
  dialogConfirm: document.querySelector('#dialogConfirm'),
  install: document.querySelector('#installButton'),
  installHint: document.querySelector('#installHint')
};

let commandLocked = false;
let portalReachable = false;
let pendingCommand = null;
let installPrompt = null;
let weatherAgeAtFetch = null;
let weatherReceivedAt = 0;
let hasWeatherValue = false;
let historyLoaded = false;

async function directFetch(path) {
  if (!navigator.onLine) throw new Error('Le navigateur est hors ligne. Aucune commande n’est envoyée.');
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    return await fetch(path, { method: 'GET', cache: 'no-store', signal: controller.signal });
  } catch (error) {
    if (error.name === 'AbortError') {
      throw new Error('Délai de réponse dépassé. Ne renvoyez pas automatiquement la commande.');
    }
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

function setNetwork(kind, label) {
  elements.network.className = `network network-${kind}`;
  elements.network.lastChild.textContent = label;
}

function showMessage(text, kind = '') {
  elements.message.className = kind ? `message message-${kind}` : 'message';
  elements.message.textContent = text;
}

function weatherLabel(code) {
  if (code === 0) return 'Ciel dégagé';
  if ([1, 2, 3].includes(code)) return code === 3 ? 'Couvert' : 'Peu nuageux';
  if ([45, 48].includes(code)) return 'Brouillard';
  if (code >= 51 && code <= 57) return 'Bruine';
  if (code >= 61 && code <= 67) return 'Pluie';
  if (code >= 71 && code <= 77) return 'Neige';
  if (code >= 80 && code <= 82) return 'Averses';
  if (code >= 85 && code <= 86) return 'Averses de neige';
  if (code >= 95 && code <= 99) return 'Orage';
  return `Code météo ${code}`;
}

function formatWeatherAge(seconds) {
  if (seconds < 60) return 'à l’instant';
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `il y a ${minutes} min`;
  const hours = Math.floor(minutes / 60);
  return `il y a ${hours} h ${minutes % 60} min`;
}

function updateWeatherAge() {
  if (weatherAgeAtFetch === null) return;
  const elapsed = Math.floor((Date.now() - weatherReceivedAt) / 1000);
  elements.weatherAge.textContent = formatWeatherAge(weatherAgeAtFetch + elapsed);
}

async function refreshWeather() {
  try {
    const response = await directFetch('/api/weather');
    const data = await response.json();
    const temperature = Number(data.temperature_2m);
    const apparent = Number(data.apparent_temperature);
    const code = Number(data.weather_code);
    const age = Number(data.age_seconds);
    if (!response.ok || !data.available || !Number.isFinite(temperature) ||
        !Number.isFinite(apparent) || !Number.isInteger(code) || !Number.isFinite(age)) {
      throw new Error('Aucune donnée météo valide.');
    }
    elements.weatherTemperature.textContent = `${temperature.toFixed(1)} °C`;
    elements.weatherDetail.textContent = `Ressenti ${apparent.toFixed(1)} °C · ${weatherLabel(code)}`;
    weatherAgeAtFetch = Math.max(0, Math.floor(age));
    weatherReceivedAt = Date.now();
    hasWeatherValue = true;
    updateWeatherAge();
  } catch (error) {
    if (hasWeatherValue) {
      elements.weatherAge.textContent = `Dernière valeur · ${elements.weatherAge.textContent}`;
      return;
    }
    elements.weatherTemperature.textContent = '-- °C';
    elements.weatherDetail.textContent = 'Météo indisponible';
    elements.weatherAge.textContent = 'Aucune donnée';
    weatherAgeAtFetch = null;
  }
}

function setGateState(kind, title, detail) {
  elements.statusCard.className = `status-card status-${kind}`;
  elements.status.textContent = title;
  elements.detail.textContent = detail;
  const icon = kind === 'closed' ? '#i-lock' : kind === 'open' ? '#i-home' : '#i-alert';
  elements.statusCard.querySelector('use').setAttribute('href', icon);
}

async function refreshState() {
  elements.refresh.disabled = true;
  try {
    const response = await directFetch('/etat');
    if (!response.ok) throw new Error(`Lecture refusée par l’ESP32 (HTTP ${response.status}).`);
    const value = (await response.text()).trim().toLowerCase();
    if (value === 'ferme') {
      setGateState('closed', 'PORTAIL FERMÉ', 'Capteur confirmé');
    } else if (value === 'ouvert') {
      setGateState('open', 'PORTAIL NON FERMÉ', 'Ouvert, en mouvement ou capteur non actif');
    } else {
      throw new Error(`Réponse d’état inconnue : ${value || 'vide'}.`);
    }
    elements.refreshed.textContent = `Actualisé à ${new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' })}`;
    setNetwork('ok', 'ESP32 connecté');
    portalReachable = true;
  } catch (error) {
    setGateState('error', 'ÉTAT INCONNU', error.message || 'Capteur indisponible');
    elements.refreshed.textContent = 'Aucune actualisation récente';
    setNetwork('error', navigator.onLine ? 'ESP32 inaccessible' : 'Hors ligne');
    portalReachable = false;
  } finally {
    elements.refresh.disabled = false;
    lockCommands(commandLocked);
  }
}

async function refreshAll() {
  elements.refresh.classList.add('is-loading');
  await Promise.allSettled([refreshState(), refreshWeather()]);
  elements.refresh.classList.remove('is-loading');
}

function lockCommands(locked) {
  commandLocked = locked;
  elements.pedestrian.disabled = locked || !portalReachable;
  elements.vehicle.disabled = locked || !portalReachable;
}

function resetCommandStyles() {
  [elements.pedestrian, elements.vehicle].forEach(button => button.classList.remove('is-sending', 'is-success', 'is-error'));
}

function requestCommand(path, label, button) {
  if (commandLocked || !portalReachable) {
    showMessage('Actualisez l’état et vérifiez que l’ESP32 est accessible.', 'error');
    return;
  }
  pendingCommand = { path, label, button };
  elements.dialogTitle.textContent = `Envoyer ${label} ?`;
  elements.dialogText.textContent = 'La réponse HTTP confirmera la réception par l’ESP32. La position sera ensuite vérifiée par le capteur.';
  elements.dialog.showModal();
}

async function sendPendingCommand() {
  if (!pendingCommand || commandLocked) return;
  const command = pendingCommand;
  pendingCommand = null;
  resetCommandStyles();
  command.button.classList.add('is-sending');
  lockCommands(true);
  showMessage('Transmission en cours…');
  try {
    const response = await directFetch(command.path);
    const body = await response.text();
    if (!response.ok) throw new Error(`Commande refusée (HTTP ${response.status}) : ${body}`);
    command.button.classList.remove('is-sending');
    command.button.classList.add('is-success');
    showMessage('Impulsion transmise. Le mouvement n’est pas confirmé.', 'success');
    setTimeout(refreshState, STATE_REFRESH_DELAY_MS);
  } catch (error) {
    command.button.classList.remove('is-sending');
    command.button.classList.add('is-error');
    showMessage(`${error.message || 'Aucune réponse de l’ESP32.'} Aucun nouvel essai automatique.`, 'error');
  } finally {
    setTimeout(() => {
      resetCommandStyles();
      lockCommands(false);
    }, COMMAND_LOCK_MS);
  }
}

function historyMeta(eventName) {
  const value = eventName.toLowerCase();
  if (value.includes('pieton')) return { icon: '#i-walk', tone: 'walk', title: 'Impulsion piétonne', detail: 'Commande enregistrée' };
  if (value.includes('voiture')) return { icon: '#i-car', tone: 'car', title: 'Impulsion complète', detail: 'Commande enregistrée' };
  if (value === 'ferme' || value.includes('initial_ferme')) return { icon: '#i-lock', tone: 'closed', title: 'Portail fermé', detail: 'Capteur confirmé' };
  if (value === 'ouvert' || value.includes('initial_ouvert')) return { icon: '#i-home', tone: 'car', title: 'Portail non fermé', detail: 'Capteur non actif' };
  if (value.includes('redemarrage') || value.includes('demarrage')) return { icon: '#i-refresh', tone: '', title: 'ESP32 redémarré', detail: 'Système initialisé' };
  if (value.includes('wifi')) return { icon: '#i-wifi', tone: '', title: 'Connexion Wi-Fi', detail: value.includes('perdu') ? 'Reconnexion demandée' : 'Événement réseau' };
  if (value.includes('erreur') || value.includes('echec') || value.includes('ignoree')) return { icon: '#i-alert', tone: 'error', title: 'Alerte système', detail: eventName.replaceAll('_', ' ') };
  return { icon: '#i-settings', tone: '', title: 'Événement système', detail: eventName.replaceAll('_', ' ') };
}

function historyDateLabel(dateText) {
  const date = new Date(`${dateText}T12:00:00`);
  if (Number.isNaN(date.getTime())) return dateText;
  return date.toLocaleDateString('fr-FR', { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' });
}

function createHistoryItem(entry) {
  const meta = historyMeta(entry.event);
  const item = document.createElement('article');
  item.className = 'history-item';
  const icon = document.createElement('span');
  icon.className = `history-icon ${meta.tone}`.trim();
  icon.innerHTML = `<svg aria-hidden="true"><use href="${meta.icon}"></use></svg>`;
  const copy = document.createElement('div');
  const title = document.createElement('strong');
  const detail = document.createElement('small');
  title.textContent = meta.title;
  detail.textContent = meta.detail;
  copy.append(title, detail);
  const time = document.createElement('time');
  time.className = 'history-time';
  time.dateTime = entry.timestamp;
  time.textContent = entry.time;
  item.append(icon, copy, time);
  return item;
}

async function loadHistory() {
  elements.historyRefresh.classList.add('is-loading');
  elements.historyState.hidden = false;
  elements.historyState.textContent = 'Chargement de l’historique…';
  elements.historyList.replaceChildren();
  try {
    const response = await directFetch('/log.txt');
    if (!response.ok) throw new Error(`Journal indisponible (HTTP ${response.status}).`);
    const lines = (await response.text()).split(/\r?\n/).map(line => line.trim()).filter(Boolean);
    const entries = lines.filter(line => !line.toLowerCase().startsWith('horodatage;')).map(line => {
      const separator = line.indexOf(';');
      if (separator < 0) return null;
      const timestamp = line.slice(0, separator).trim();
      return { timestamp, date: timestamp.slice(0, 10), time: timestamp.slice(11, 16), event: line.slice(separator + 1).trim() };
    }).filter(Boolean).reverse();
    if (!entries.length) {
      elements.historyState.textContent = 'Aucun événement enregistré.';
      historyLoaded = true;
      return;
    }
    elements.historyState.hidden = true;
    const groups = new Map();
    entries.forEach(entry => {
      if (!groups.has(entry.date)) groups.set(entry.date, []);
      groups.get(entry.date).push(entry);
    });
    groups.forEach((items, date) => {
      const section = document.createElement('section');
      section.className = 'history-group';
      const heading = document.createElement('h3');
      heading.textContent = historyDateLabel(date);
      const list = document.createElement('div');
      list.className = 'history-group-items';
      items.forEach(entry => list.append(createHistoryItem(entry)));
      section.append(heading, list);
      elements.historyList.append(section);
    });
    historyLoaded = true;
  } catch (error) {
    elements.historyState.textContent = error.message || 'Erreur de lecture du journal.';
  } finally {
    elements.historyRefresh.classList.remove('is-loading');
  }
}

function showView(name, updateHash = true) {
  const target = ['home', 'history', 'settings'].includes(name) ? name : 'home';
  document.querySelectorAll('.view').forEach(view => {
    const active = view.dataset.view === target;
    view.hidden = !active;
    view.classList.toggle('is-active', active);
  });
  document.querySelectorAll('.nav-item').forEach(item => {
    const active = item.dataset.target === target;
    item.classList.toggle('is-active', active);
    if (active) item.setAttribute('aria-current', 'page'); else item.removeAttribute('aria-current');
  });
  if (updateHash) history.replaceState(null, '', target === 'home' ? location.pathname : `#${target}`);
  if (target === 'history' && !historyLoaded) loadHistory();
  document.querySelector(`[data-view="${target}"] h2`)?.focus?.({ preventScroll: true });
}

elements.pedestrian.addEventListener('click', () => requestCommand('/pieton', 'une impulsion piétonne', elements.pedestrian));
elements.vehicle.addEventListener('click', () => requestCommand('/voiture', 'une impulsion complète', elements.vehicle));
elements.dialog.addEventListener('close', () => {
  if (elements.dialog.returnValue === 'confirm') sendPendingCommand(); else pendingCommand = null;
});
elements.refresh.addEventListener('click', refreshAll);
elements.historyRefresh.addEventListener('click', loadHistory);
document.querySelectorAll('.nav-item').forEach(item => item.addEventListener('click', () => showView(item.dataset.target)));
window.addEventListener('hashchange', () => showView(location.hash.slice(1), false));
window.addEventListener('online', refreshAll);
window.addEventListener('offline', () => {
  setNetwork('error', 'Hors ligne');
  portalReachable = false;
  lockCommands(false);
});

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
if (window.matchMedia('(display-mode: standalone)').matches) elements.installHint.textContent = 'Application installée en mode autonome.';

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => navigator.serviceWorker.register('/service-worker.js')
    .catch(() => showMessage('Le mode hors ligne de l’interface n’a pas pu être activé.', 'error')));
}

showView(location.hash.slice(1), false);
refreshAll();
setInterval(refreshState, 30000);
setInterval(refreshWeather, 60000);
setInterval(updateWeatherAge, 30000);
