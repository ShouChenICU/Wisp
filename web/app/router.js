// Hash-based routing ("#/overview"): the device serves one index.html, and
// a hash never reaches the server, so every page works from a bookmark. A
// page may keep its own state in further segments ("#/models/asr"): those
// paths resolve to the page registered for their first segment.

import { useEffect, useState } from '../vendor/htm-preact-3.1.1.mjs';

/**
 * @typedef {Object} RouteEntry
 * @property {string} path
 * @property {Function} component  a Preact component taking no props
 */

/** @type {Map<string, RouteEntry>} */
const routes = new Map();

export const DEFAULT_PATH = '/overview';

/**
 * Registers a page. Registering the same path again replaces it, which is
 * how a placeholder page gets swapped for the real one.
 *
 * @param {string} path  e.g. "/overview"
 * @param {Function} component
 */
export function route(path, component) {
  routes.set(path, { path, component });
}

/** @returns {RouteEntry|undefined} */
export function lookup(path) {
  return routes.get(path) || routes.get('/' + path.split('/')[1]);
}

/** The current route path; "/" and an empty hash both mean the default. */
export function currentPath() {
  const p = location.hash.replace(/^#/, '').split('?')[0];
  return p && p !== '/' ? p : DEFAULT_PATH;
}

export function navigate(path) {
  if (currentPath() === path && location.hash) return;
  location.hash = '#' + path;
}

/** Re-renders the caller whenever the hash changes. */
export function useRoute() {
  const [path, setPath] = useState(currentPath());
  useEffect(() => {
    const onChange = () => setPath(currentPath());
    window.addEventListener('hashchange', onChange);
    return () => window.removeEventListener('hashchange', onChange);
  }, []);
  return path;
}
