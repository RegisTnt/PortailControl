'use strict';

const CACHE_NAME = 'portailcontrol-static-v6';
const STATIC_ASSETS = [
  '/',
  '/index.html',
  '/app.css?v=6',
  '/app.js?v=6',
  '/manifest.webmanifest',
  '/icons/icon-192.png',
  '/icons/icon-512.png'
];

const NETWORK_ONLY_PATHS = new Set([
  '/etat', '/api/weather', '/pieton', '/voiture', '/historique', '/log.txt', '/clear-log',
  '/set-delay', '/set-notifications', '/save', '/delete-user', '/get-users',
  '/users.json', '/download-users', '/enrol', '/admin', '/users', '/settings',
  '/wifi_config', '/upload', '/upload.html', '/enrol.html'
]);

self.addEventListener('install', event => {
  event.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(STATIC_ASSETS)));
  self.skipWaiting();
});

self.addEventListener('activate', event => {
  event.waitUntil(caches.keys().then(keys => Promise.all(
    keys.filter(key => key !== CACHE_NAME).map(key => caches.delete(key))
  )));
  self.clients.claim();
});

self.addEventListener('fetch', event => {
  const request = event.request;
  const url = new URL(request.url);
  if (url.origin !== self.location.origin) return;

  // Toute mutation et toute donnée dynamique vont directement au réseau.
  // Aucun Background Sync ni aucune file de commandes n'est enregistré.
  if (request.method !== 'GET' || NETWORK_ONLY_PATHS.has(url.pathname)) {
    event.respondWith(fetch(request));
    return;
  }

  if (request.mode === 'navigate') {
    event.respondWith(fetch(request).catch(() => caches.match('/index.html')));
    return;
  }

  event.respondWith(caches.match(request).then(cached => cached || fetch(request)));
});
