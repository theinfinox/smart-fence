// FenceGuard AI Service Worker v2.1
const CACHE_NAME = 'fenceguard-v2.1';
const LOCAL_ASSETS = [
  './',
  './index.html',
  './style.css',
  './manifest.json',
  './icon.svg'
];

self.addEventListener('install', (event) => {
  // Pre-cache only local static files
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => {
      console.log('[SW v2.0] Pre-caching local core assets');
      return cache.addAll(LOCAL_ASSETS);
    }).then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', (event) => {
  // Aggressively clear old cache v1.0 so user gets latest code immediately
  event.waitUntil(
    caches.keys().then((cacheNames) => {
      return Promise.all(
        cacheNames.map((name) => {
          if (name !== CACHE_NAME) {
            console.log('[SW v2.0] Purging outdated cache:', name);
            return caches.delete(name);
          }
        })
      );
    }).then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', (event) => {
  // Only handle GET requests
  if (event.request.method !== 'GET') return;

  const url = new URL(event.request.url);

  // For external CDNs (TensorFlow, Tailwind, Fonts), use Network-First and NEVER fallback to index.html
  if (url.origin !== location.origin) {
    event.respondWith(
      fetch(event.request).catch((err) => {
        console.warn('[SW] External resource offline:', event.request.url);
        // Do NOT return index.html for scripts/fonts! Return a clean 503 response
        return new Response('Offline resource unavailable', { status: 503, statusText: 'Service Unavailable' });
      })
    );
    return;
  }

  // For local files (index.html, manifest.json, icon.svg), use Network-First with cache fallback
  event.respondWith(
    fetch(event.request)
      .then((networkResponse) => {
        if (networkResponse && networkResponse.status === 200) {
          const responseClone = networkResponse.clone();
          caches.open(CACHE_NAME).then((cache) => {
            cache.put(event.request, responseClone);
          });
        }
        return networkResponse;
      })
      .catch(() => {
        // If offline, serve from cache
        return caches.match(event.request).then((cached) => {
          return cached || caches.match('./index.html');
        });
      })
  );
});
