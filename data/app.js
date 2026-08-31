'use strict';

const COMMAND_LOCK_MS = 3000;
const REQUEST_TIMEOUT_MS = 5000;
const STATE_REQUEST_TIMEOUT_MS = 1800;
const STATE_POLL_VISIBLE_MS = 2000;
const STATE_POLL_HIDDEN_MS = 15000;
const STATE_FAILURE_THRESHOLD = 2;
const DIAGNOSTIC_POLL_MS = 5000;

const elements = {
  network: document.querySelector('#networkBadge'),
  statusCard: document.querySelector('#statusCard'),
  status: document.querySelector('#gateStatus'),
  detail: document.querySelector('#statusDetail'),
  refreshed: document.querySelector('#lastRefreshText'),
  refreshDot: document.querySelector('#refreshDot'),
  message: document.querySelector('#commandMessage'),
  openActions: document.querySelector('#openActions'),
  pedestrian: document.querySelector('#pedestrianButton'),
  vehicle: document.querySelector('#vehicleButton'),
  close: document.querySelector('#closeButton'),
  refresh: document.querySelector('#refreshButton'),
  historyRefresh: document.querySelector('#historyRefresh'),
  historyState: document.querySelector('#historyState'),
  historyList: document.querySelector('#historyList'),
  install: document.querySelector('#installButton'),
  installHint: document.querySelector('#installHint')
};

const diagnosticElements = {
  refresh: document.querySelector('#diagnosticRefresh'),
  updated: document.querySelector('#diagnosticUpdated'),
  state: document.querySelector('#diagnosticState'),
  firmware: document.querySelector('#diagnosticFirmware'),
  git: document.querySelector('#diagnosticGit'),
  uptime: document.querySelector('#diagnosticUptime'),
  resetReason: document.querySelector('#diagnosticResetReason'),
  bootCount: document.querySelector('#diagnosticBootCount'),
  wifi: document.querySelector('#diagnosticWifi'),
  rssi: document.querySelector('#diagnosticRssi'),
  ip: document.querySelector('#diagnosticIp'),
  reconnectCount: document.querySelector('#diagnosticReconnectCount'),
  recoveryAp: document.querySelector('#diagnosticRecoveryAp'),
  freeHeap: document.querySelector('#diagnosticFreeHeap'),
  minFreeHeap: document.querySelector('#diagnosticMinFreeHeap'),
  portalClosed: document.querySelector('#diagnosticPortalClosed'),
  sensorRaw: document.querySelector('#diagnosticSensorRaw'),
  sensorStable: document.querySelector('#diagnosticSensorStable'),
  sensorBounceCount: document.querySelector('#diagnosticSensorBounceCount'),
  clock: document.querySelector('#diagnosticClock'),
  relayActive: document.querySelector('#diagnosticRelayActive')
};

let commandLocked = false;
let portalReachable = false;
let installPrompt = null;
let historyLoaded = false;
let stateRequestInProgress = false;
let statePollTimer = null;
let lastGateState = null;
let lastNetworkStatus = null;
let lastNetworkLabel = '';
let lastStateUpdateAt = 0;
let consecutiveStateFailures = 0;
let currentView = 'home';
let diagnosticRequestInProgress = false;
let diagnosticPollTimer = null;

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

async function syncLocalClock() {
  if (!navigator.onLine) return false;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    const epoch = Math.floor(Date.now() / 1000);
    const response = await fetch('/api/time', {
      method: 'POST',
      cache: 'no-store',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
      body: new URLSearchParams({ epoch: String(epoch) }),
      signal: controller.signal
    });
    return response.ok;
  } catch (_) {
    return false;
  } finally {
    clearTimeout(timer);
  }
}

function setNetwork(kind, label) {
  if (lastNetworkStatus === kind && lastNetworkLabel === label) return;
  lastNetworkStatus = kind;
  lastNetworkLabel = label;
  elements.network.className = `network network-${kind}`;
  elements.network.lastChild.textContent = label;
}

function showMessage(text, kind = '') {
  elements.message.className = kind ? `message message-${kind}` : 'message';
  elements.message.textContent = text;
}

function setGateState(kind, title, detail) {
  if (lastGateState === kind) return;
  lastGateState = kind;
  elements.statusCard.className = `status-card status-${kind}`;
  elements.status.textContent = title;
  elements.detail.textContent = detail;
  const icon = kind === 'closed' ? '#i-lock' : kind === 'open' ? '#i-home' : '#i-alert';
  elements.statusCard.querySelector('use').setAttribute('href', icon);
  elements.openActions.hidden = kind !== 'closed';
  elements.close.hidden = kind !== 'open';
}

function updateRefreshIndicator(fresh) {
  elements.refreshDot.className = `refresh-dot ${fresh ? 'refresh-fresh' : 'refresh-stale'}`;
  elements.refreshDot.title = fresh ? 'État actualisé récemment' : 'État ancien ou indisponible';
}

async function refreshState() {
  if (stateRequestInProgress) return false;
  stateRequestInProgress = true;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), STATE_REQUEST_TIMEOUT_MS);
  try {
    const response = await fetch('/etat', { method: 'GET', cache: 'no-store', signal: controller.signal });
    if (!response.ok) throw new Error(`Lecture refusée par l’ESP32 (HTTP ${response.status}).`);
    const value = (await response.text()).trim().toLowerCase();
    if (value === 'ferme') {
      setGateState('closed', 'PORTAIL FERMÉ', 'Capteur confirmé');
    } else if (value === 'ouvert') {
      setGateState('open', 'PORTAIL NON FERMÉ', 'Ouvert, en mouvement ou capteur non actif');
    } else {
      throw new Error(`Réponse d’état inconnue : ${value || 'vide'}.`);
    }
    consecutiveStateFailures = 0;
    lastStateUpdateAt = Date.now();
    elements.refreshed.textContent = `Actualisé à ${new Date(lastStateUpdateAt).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' })}`;
    updateRefreshIndicator(true);
    setNetwork('ok', 'ESP32 connecté');
    portalReachable = true;
    return true;
  } catch (error) {
    consecutiveStateFailures += 1;
    updateRefreshIndicator(false);
    if (consecutiveStateFailures >= STATE_FAILURE_THRESHOLD) {
      setGateState('error', 'ÉTAT INCONNU', 'ESP32 inaccessible');
      elements.refreshed.textContent = lastStateUpdateAt
        ? `Dernière réponse à ${new Date(lastStateUpdateAt).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' })}`
        : 'Aucune actualisation récente';
      setNetwork('error', navigator.onLine ? 'ESP32 inaccessible' : 'Hors ligne');
      portalReachable = false;
    }
    return false;
  } finally {
    clearTimeout(timer);
    stateRequestInProgress = false;
    lockCommands(commandLocked);
  }
}

function currentStatePollInterval() {
  return document.visibilityState === 'visible' ? STATE_POLL_VISIBLE_MS : STATE_POLL_HIDDEN_MS;
}

function scheduleStatePoll(delay = currentStatePollInterval()) {
  clearTimeout(statePollTimer);
  statePollTimer = setTimeout(async () => {
    await refreshState();
    scheduleStatePoll();
  }, delay);
}

function restartStatePolling(immediate = false) {
  clearTimeout(statePollTimer);
  if (immediate) {
    refreshState().finally(() => scheduleStatePoll());
  } else {
    scheduleStatePoll();
  }
}

async function refreshAll() {
  elements.refresh.classList.add('is-loading');
  elements.refresh.disabled = true;
  try {
    await refreshState();
  } finally {
    elements.refresh.disabled = false;
    elements.refresh.classList.remove('is-loading');
  }
}

function lockCommands(locked) {
  commandLocked = locked;
  elements.pedestrian.disabled = locked || !portalReachable;
  elements.vehicle.disabled = locked || !portalReachable;
  elements.close.disabled = locked || !portalReachable;
}

function resetCommandStyles() {
  [elements.pedestrian, elements.vehicle, elements.close].forEach(button => {
    button.classList.remove('is-sending', 'is-success', 'is-error');
    const copy = button.querySelector('.command-copy');
    if (copy.dataset.originalHtml) copy.innerHTML = copy.dataset.originalHtml;
  });
}

function setCommandLabel(button, label) {
  const copy = button.querySelector('.command-copy');
  if (!copy.dataset.originalHtml) copy.dataset.originalHtml = copy.innerHTML;
  copy.textContent = label;
}

async function sendCommand(path, button) {
  if (commandLocked) return;
  if (!portalReachable) {
    showMessage('Actualisez l’état et vérifiez que l’ESP32 est accessible.', 'error');
    return;
  }
  resetCommandStyles();
  button.classList.add('is-sending');
  setCommandLabel(button, 'Envoi de l’impulsion…');
  lockCommands(true);
  showMessage('Envoi de l’impulsion…');
  try {
    const response = await directFetch(path);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    await response.text();
    button.classList.remove('is-sending');
    button.classList.add('is-success');
    setCommandLabel(button, 'Impulsion transmise');
    showMessage('Impulsion transmise', 'success');
  } catch (error) {
    button.classList.remove('is-sending');
    button.classList.add('is-error');
    setCommandLabel(button, 'Commande non transmise');
    showMessage('Commande non transmise', 'error');
  } finally {
    // Relecture immédiate du capteur ; la commande physique n'est jamais rejouée.
    await refreshState();
    setTimeout(() => {
      resetCommandStyles();
      lockCommands(false);
    }, COMMAND_LOCK_MS);
  }
}

function historyMeta(eventName) {
  const value = eventName.toLowerCase();
  const durationMatch = value.match(/(?:^|;)duration=(\d+)/);
  const duration = durationMatch ? Number(durationMatch[1]) : null;
  if (value.includes('commande_pieton')) return { icon: '#i-walk', tone: 'walk', title: 'Ouverture piéton', detail: 'Commande locale' };
  if (value.includes('commande_voiture')) return { icon: '#i-car', tone: 'car', title: 'Ouverture voiture', detail: 'Commande locale' };
  if (value.startsWith('portail_ferme')) return { icon: '#i-lock', tone: 'closed', title: 'Portail refermé', detail: duration === null ? 'Fermeture confirmée' : `Ouvert pendant ${duration} s` };
  if (value.startsWith('portail_ouvert')) return { icon: '#i-home', tone: 'car', title: 'Portail ouvert', detail: 'Ouverture confirmée' };
  if (value === 'ferme' || value.includes('initial_ferme')) return { icon: '#i-lock', tone: 'closed', title: 'Portail fermé', detail: 'Capteur confirmé' };
  if (value === 'ouvert' || value.includes('initial_ouvert')) return { icon: '#i-home', tone: 'car', title: 'Portail non fermé', detail: 'Capteur non actif' };
  if (value.includes('redemarrage') || value.includes('demarrage')) return { icon: '#i-refresh', tone: '', title: 'ESP32 redémarré', detail: 'Système initialisé' };
  if (value.startsWith('wifi_reconnected')) return { icon: '#i-wifi', tone: '', title: 'Wi-Fi rétabli', detail: duration === null ? 'Connexion locale restaurée' : `Interruption ${duration} s` };
  if (value.startsWith('wifi_lost')) return { icon: '#i-wifi', tone: 'error', title: 'Wi-Fi interrompu', detail: 'Coupure supérieure à 30 s' };
  if (value.includes('wifi')) return { icon: '#i-wifi', tone: '', title: 'Recovery Wi-Fi', detail: 'Intervention locale disponible' };
  if (value.startsWith('reset_anormal')) return { icon: '#i-alert', tone: 'error', title: 'Redémarrage anormal', detail: eventName.split(';').slice(1).join(' · ').replaceAll('_', ' ') };
  if (value.includes('erreur') || value.includes('echec') || value.includes('ignoree')) return { icon: '#i-alert', tone: 'error', title: 'Alerte système', detail: eventName.replaceAll('_', ' ') };
  return { icon: '#i-settings', tone: '', title: 'Événement système', detail: eventName.replaceAll('_', ' ') };
}

function parseHistoryTimestamp(value) {
  if (value.startsWith('epoch:')) {
    const epoch = Number(value.slice(6));
    const date = new Date(epoch * 1000);
    if (Number.isFinite(epoch) && !Number.isNaN(date.getTime())) {
      const dateKey = `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`;
      return { date: dateKey, time: date.toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' }), sortable: epoch };
    }
  }
  const session = value.match(/^boot:(\d+)\+(\d+)$/);
  if (session) {
    const total = Number(session[2]);
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    const seconds = total % 60;
    return { date: `Session ${session[1]} sans heure`, time: `+${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`, sortable: total };
  }
  if (value.startsWith('1970-01-01')) return { date: 'Ancien journal sans heure', time: 'Heure inconnue', sortable: 0 };
  return { date: value.slice(0, 10), time: value.slice(11, 16), sortable: Date.parse(value.replace(' ', 'T')) || 0 };
}

function isUsefulHistoryEvent(eventName) {
  const value = eventName.toLowerCase();
  return value.startsWith('portail_') || value.startsWith('commande_') ||
    value.startsWith('wifi_') || value.includes('demarrage') ||
    value.startsWith('reset_anormal') || value.includes('erreur') || value.includes('echec');
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
      const parsed = parseHistoryTimestamp(timestamp);
      return { timestamp, ...parsed, event: line.slice(separator + 1).trim() };
    }).filter(entry => entry && isUsefulHistoryEvent(entry.event)).reverse();
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

function formatUptime(totalSeconds) {
  const seconds = Math.max(0, Math.floor(Number(totalSeconds) || 0));
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return `${days} j · ${hours} h · ${minutes} min`;
}

function formatBytes(value) {
  const bytes = Number(value);
  return Number.isFinite(bytes) ? `${Math.round(bytes / 1024)} Ko` : '--';
}

function renderHealth(data) {
  const wifi = data?.wifi ?? {};
  const memory = data?.memory ?? {};
  const portal = data?.portal ?? {};

  diagnosticElements.firmware.textContent = data?.firmware || '--';
  diagnosticElements.git.textContent = data?.git || '--';
  diagnosticElements.uptime.textContent = formatUptime(data?.uptime_seconds);
  diagnosticElements.resetReason.textContent = data?.reset_reason || '--';
  diagnosticElements.bootCount.textContent = Number.isFinite(Number(data?.boot_count)) ? String(data.boot_count) : '--';
  diagnosticElements.wifi.textContent = wifi.connected === true ? 'Connecté' : wifi.connected === false ? 'Déconnecté' : '--';
  diagnosticElements.rssi.textContent = Number.isFinite(Number(wifi.rssi)) ? `${wifi.rssi} dBm` : '--';
  diagnosticElements.ip.textContent = wifi.ip || '--';
  diagnosticElements.reconnectCount.textContent = Number.isFinite(Number(wifi.reconnect_count)) ? String(wifi.reconnect_count) : '--';
  diagnosticElements.recoveryAp.textContent = wifi.recovery_ap === true ? 'Actif' : wifi.recovery_ap === false ? 'Inactif' : '--';
  diagnosticElements.freeHeap.textContent = formatBytes(memory.free_heap);
  diagnosticElements.minFreeHeap.textContent = formatBytes(memory.min_free_heap);
  diagnosticElements.portalClosed.textContent = portal.closed === true ? 'Fermé' : portal.closed === false ? 'Non fermé' : '--';
  diagnosticElements.sensorRaw.textContent = portal.sensor_raw === true ? 'Fermé' : portal.sensor_raw === false ? 'Non fermé' : '--';
  diagnosticElements.sensorStable.textContent = portal.sensor_stable === true ? 'Fermé' : portal.sensor_stable === false ? 'Non fermé' : '--';
  diagnosticElements.sensorBounceCount.textContent = Number.isFinite(Number(portal.sensor_bounce_count)) ? String(portal.sensor_bounce_count) : '--';
  diagnosticElements.clock.textContent = data?.clock?.known === true ? 'Synchronisée localement' : 'Uptime de session';
  diagnosticElements.relayActive.textContent = portal.relay_active === true ? 'Actif' : portal.relay_active === false ? 'Inactif' : '--';
  diagnosticElements.state.className = `diagnostic-state ${wifi.connected === true && portal.relay_active === false ? 'diagnostic-ok' : 'diagnostic-warning'}`;
  diagnosticElements.state.textContent = wifi.connected === false
    ? 'ESP32 accessible, Wi-Fi signalé déconnecté'
    : portal.relay_active === true
      ? 'Relais actuellement actif'
      : 'Diagnostic recovery disponible';
  diagnosticElements.updated.textContent = `Actualisé à ${new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })}`;
}

function renderHealthUnavailable() {
  diagnosticElements.state.className = 'diagnostic-state diagnostic-error';
  diagnosticElements.state.textContent = 'Diagnostic temporairement indisponible';
  diagnosticElements.updated.textContent = `Dernière tentative à ${new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })}`;
}

async function refreshHealth() {
  if (diagnosticRequestInProgress) return;
  diagnosticRequestInProgress = true;
  diagnosticElements.refresh.classList.add('is-loading');
  diagnosticElements.refresh.disabled = true;
  try {
    const response = await directFetch('/api/diagnostic');
    if (!response.ok) throw new Error(`Diagnostic indisponible (HTTP ${response.status}).`);
    renderHealth(await response.json());
  } catch (error) {
    renderHealthUnavailable();
  } finally {
    diagnosticElements.refresh.disabled = false;
    diagnosticElements.refresh.classList.remove('is-loading');
    diagnosticRequestInProgress = false;
  }
}

function restartDiagnosticPolling(immediate = false) {
  clearInterval(diagnosticPollTimer);
  if (currentView !== 'diagnostic' || document.visibilityState !== 'visible') return;
  if (immediate) refreshHealth();
  diagnosticPollTimer = setInterval(refreshHealth, DIAGNOSTIC_POLL_MS);
}

function showView(name, updateHash = true) {
  const target = ['home', 'history', 'diagnostic', 'settings'].includes(name) ? name : 'home';
  currentView = target;
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
  if (target === 'diagnostic') restartDiagnosticPolling(true); else clearInterval(diagnosticPollTimer);
  document.querySelector(`[data-view="${target}"] h2`)?.focus?.({ preventScroll: true });
}

elements.pedestrian.addEventListener('click', () => sendCommand('/pieton', elements.pedestrian));
elements.vehicle.addEventListener('click', () => sendCommand('/voiture', elements.vehicle));
// Libellé contextuel uniquement : cette action reste une impulsion complète /voiture.
// Le sens dépend de l'automatisme et HTTP 200 ne prouve jamais la fermeture physique.
elements.close.addEventListener('click', () => sendCommand('/voiture', elements.close));
elements.refresh.addEventListener('click', refreshAll);
elements.historyRefresh.addEventListener('click', loadHistory);
diagnosticElements.refresh.addEventListener('click', refreshHealth);
document.querySelectorAll('.nav-item').forEach(item => item.addEventListener('click', () => showView(item.dataset.target)));
window.addEventListener('hashchange', () => showView(location.hash.slice(1), false));
window.addEventListener('online', () => {
  syncLocalClock();
  restartStatePolling(true);
  if (currentView === 'diagnostic') restartDiagnosticPolling(true);
});
window.addEventListener('offline', () => {
  consecutiveStateFailures = STATE_FAILURE_THRESHOLD;
  setGateState('error', 'ÉTAT INCONNU', 'ESP32 inaccessible');
  setNetwork('error', 'Hors ligne');
  portalReachable = false;
  updateRefreshIndicator(false);
  lockCommands(false);
});
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') {
    restartStatePolling(true);
    if (currentView === 'diagnostic') restartDiagnosticPolling(true);
  } else {
    updateRefreshIndicator(false);
    restartStatePolling(false);
    clearInterval(diagnosticPollTimer);
  }
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
syncLocalClock();
restartStatePolling(true);
