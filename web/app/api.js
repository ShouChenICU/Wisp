// REST client for the device's /api/v1 (spec 6.1). The pairing token lives
// in an HttpOnly cookie, so scripts never see it: the browser attaches it to
// every same-origin request on its own.

import { navigate, currentPath } from './router.js';
import { has, t } from './i18n/index.js';

const PREFIX = '/api/v1';

/**
 * @typedef {Object} ApiError
 * @property {number} status  HTTP status, or 0 when the device was unreachable
 * @property {string} code    the server's error code ("network" when unreachable)
 * @property {string} message the server's (English) message
 * @property {number} [retryAfterS] seconds until a rate limit lifts (429)
 */

/**
 * Calls the device API. Every request carries `X-Wisp: 1`: the device
 * refuses mutating requests without it (spec 6.3, CSRF) and it is harmless
 * on reads. A 401 means this browser is not (or no longer) paired, so the
 * app goes to the pairing page before the error is thrown.
 *
 * @param {'GET'|'POST'|'PUT'|'DELETE'} method
 * @param {string} path   path below /api/v1, e.g. "/device"
 * @param {unknown} [body] JSON-encoded when given
 * @returns {Promise<any>} the decoded JSON body, or null for an empty one
 * @throws {ApiError}
 */
export async function api(method, path, body) {
  const headers = { 'X-Wisp': '1', Accept: 'application/json' };
  const init = { method, headers, credentials: 'same-origin', cache: 'no-store' };
  if (body !== undefined) {
    headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body);
  }

  let res;
  try {
    res = await fetch(PREFIX + path, init);
  } catch {
    throw { status: 0, code: 'network', message: 'device unreachable' };
  }

  let data = null;
  const text = await res.text().catch(() => '');
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      data = null;
    }
  }

  if (res.ok) return data;

  if (res.status === 401 && currentPath() !== '/pair') navigate('/pair');
  const err = (data && data.error) || {};
  /** @type {ApiError} */
  const thrown = {
    status: res.status,
    code: typeof err.code === 'string' ? err.code : 'http_' + res.status,
    message: typeof err.message === 'string' ? err.message : res.statusText,
  };
  const retry = Number(err.retry_after_s ?? res.headers.get('Retry-After'));
  if (res.status === 429 && Number.isFinite(retry) && retry > 0) thrown.retryAfterS = Math.ceil(retry);
  throw thrown;
}

// Several parts of the app want /info at start-up (boot, the shell, the
// overview); one request serves them all.
const INFO_MAX_AGE_MS = 2000;
let infoCache = null;

/**
 * GET /info, shared with any other caller within the last couple of
 * seconds (including one still in flight).
 *
 * @returns {Promise<{name: string, fw: string, paired: boolean}>}
 */
export function getInfo() {
  const now = Date.now();
  if (infoCache && now - infoCache.at < INFO_MAX_AGE_MS) return infoCache.promise;
  const promise = api('GET', '/info');
  const entry = { at: now, promise };
  infoCache = entry;
  promise.catch(() => {
    if (infoCache === entry) infoCache = null;
  });
  return promise;
}

// Codes for which a 5xx status still says something specific; any other
// server-side failure is just "internal" (the device hides its details).
const SPECIFIC_5XX = new Set(['unsupported', 'timeout', 'unavailable', 'storage_unavailable', 'no_memory']);

/**
 * A localized sentence for an ApiError, from its `code` (keys `err.<code>`
 * in the i18n dictionaries). Pages with their own wording for specific
 * codes check those first.
 *
 * @param {ApiError} e
 * @returns {string}
 */
export function describeError(e) {
  if (!e || typeof e !== 'object') return t('err.unknown');
  if (e.status === 0) return t('err.network');
  if (e.status >= 500 && !SPECIFIC_5XX.has(e.code)) return t('err.internal');
  const key = 'err.' + e.code;
  return has(key) ? t(key) : t('err.http', { status: e.status });
}

/**
 * The server's own (English) message, for showing as a secondary detail
 * under describeError()'s text; empty when there is nothing to add.
 *
 * @param {ApiError} e
 * @returns {string}
 */
export function errorDetail(e) {
  if (!e || typeof e !== 'object' || !e.status) return '';
  return [e.status, e.code, e.message].filter(Boolean).join(' · ');
}

/**
 * Revokes this browser's pairing and returns to the pairing page. On
 * failure the error is thrown and the page stays (a 401 has already sent
 * the app to pairing: the token was gone anyway).
 */
export async function logout() {
  await api('POST', '/logout');
  infoCache = null;
  navigate('/pair');
}
