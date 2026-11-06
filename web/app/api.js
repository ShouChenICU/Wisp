// REST client for the device's /api/v1 (spec 6.1). The pairing token lives
// in an HttpOnly cookie, so scripts never see it: the browser attaches it to
// every same-origin request on its own.

import { navigate, currentPath } from './router.js';
import { has, t } from './i18n/index.js';

const PREFIX = '/api/v1';

// What a 401 does. The admin app goes to its pairing page; the setup
// wizard (setup.html, no hash routes) and a page that expects its token to
// vanish (a factory reset revokes every token) install their own.
const goToPairing = () => {
  if (currentPath() !== '/pair') navigate('/pair');
};
let onUnauthorized = goToPairing;

/**
 * Replaces what a 401 from any request does (by default: go to the
 * pairing page).
 *
 * @param {() => void} fn
 * @returns {() => void} restores the handler that was there before
 */
export function setUnauthorizedHandler(fn) {
  const before = onUnauthorized;
  onUnauthorized = fn;
  return () => {
    if (onUnauthorized === fn) onUnauthorized = before;
  };
}

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
 * unauthorized handler (see setUnauthorizedHandler) runs before the error
 * is thrown.
 *
 * @param {'GET'|'POST'|'PUT'|'DELETE'} method
 * @param {string} path   path below /api/v1, e.g. "/device"
 * @param {unknown} [body] JSON-encoded when given
 * @param {{timeoutMs?: number}} [opts]  `timeoutMs`: give up after this
 *   long (including reading the body) and throw as if unreachable. Without
 *   it a request waits as long as the browser does, which for a device that
 *   left the network can be minutes.
 * @returns {Promise<any>} the decoded JSON body, or null for an empty one
 * @throws {ApiError}
 */
export async function api(method, path, body, opts = {}) {
  const headers = { 'X-Wisp': '1', Accept: 'application/json' };
  const init = { method, headers, credentials: 'same-origin', cache: 'no-store' };
  if (body !== undefined) {
    headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body);
  }
  let timer = 0;
  if (opts.timeoutMs > 0) {
    const ctrl = new AbortController();
    init.signal = ctrl.signal;
    timer = setTimeout(() => ctrl.abort(), opts.timeoutMs);
  }

  let res;
  let text;
  try {
    res = await fetch(PREFIX + path, init);
    text = await res.text();
  } catch {
    // An abort while reading the body is as unreachable as one before the
    // headers; any other body read failure leaves an empty body.
    if (init.signal?.aborted || !res) throw { status: 0, code: 'network', message: 'device unreachable' };
    text = '';
  } finally {
    clearTimeout(timer);
  }

  const data = parseJson(text);
  if (res.ok) return data;
  throw toApiError(res.status, res.statusText, data, res.headers.get('Retry-After'));
}

function parseJson(text) {
  if (!text) return null;
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

// The ApiError for a failed response; a 401 runs the unauthorized handler.
function toApiError(status, statusText, data, retryAfter) {
  if (status === 401) onUnauthorized();
  const err = (data && data.error) || {};
  /** @type {ApiError} */
  const thrown = {
    status,
    code: typeof err.code === 'string' ? err.code : 'http_' + status,
    message: typeof err.message === 'string' ? err.message : statusText,
  };
  const retry = Number(err.retry_after_s ?? retryAfter);
  if (status === 429 && Number.isFinite(retry) && retry > 0) thrown.retryAfterS = Math.ceil(retry);
  return thrown;
}

/**
 * POSTs a file as the raw request body (application/octet-stream, with
 * its Content-Length), reporting upload progress -- fetch() cannot, hence
 * XMLHttpRequest. Same X-Wisp header, errors and 401 handling as api().
 *
 * @param {string} path  path below /api/v1
 * @param {Blob} file
 * @param {{onProgress?: (sent: number, total: number) => void}} [opts]
 * @returns {{done: Promise<any>, abort: () => void}}  `done` resolves to
 *   the decoded JSON body and rejects with an ApiError (status 0 when the
 *   device was unreachable or the upload was aborted)
 */
export function upload(path, file, opts = {}) {
  const xhr = new XMLHttpRequest();
  const done = new Promise((resolve, reject) => {
    xhr.open('POST', PREFIX + path);
    xhr.setRequestHeader('X-Wisp', '1');
    xhr.setRequestHeader('Accept', 'application/json');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.withCredentials = true;
    if (opts.onProgress) {
      xhr.upload.onprogress = (e) => opts.onProgress(e.loaded, e.lengthComputable ? e.total : file.size);
    }
    const unreachable = () => reject({ status: 0, code: 'network', message: 'device unreachable' });
    xhr.onerror = unreachable;
    xhr.onabort = unreachable;
    xhr.onload = () => {
      const data = parseJson(xhr.responseText);
      if (xhr.status >= 200 && xhr.status < 300) {
        resolve(data);
      } else {
        reject(toApiError(xhr.status, xhr.statusText, data, xhr.getResponseHeader('Retry-After')));
      }
    };
    xhr.send(file);
  });
  return { done, abort: () => xhr.abort() };
}

/**
 * The URL of a scene's export (GET /scenes/{id}/export), for a download
 * link: a GET needs no X-Wisp header and the cookie goes along on its own.
 *
 * @param {string} id
 * @param {boolean} withData  include the scene's data/ directory
 * @returns {string}
 */
export function exportUrl(id, withData) {
  return `${PREFIX}/scenes/${encodeURIComponent(id)}/export?data=${withData ? 1 : 0}`;
}

/**
 * The URL of one of a scene's files (GET /scenes/{id}/files/{name}), for a
 * download link. Followed rather than fetched: a file may be far larger
 * than a page should hold in memory.
 *
 * @param {string} id
 * @param {string} name
 * @returns {string}
 */
export function fileUrl(id, name) {
  return `${PREFIX}/scenes/${encodeURIComponent(id)}/files/${encodeURIComponent(name)}`;
}

// Several parts of the app want /info at start-up (boot, the shell, the
// overview); one request serves them all.
const INFO_MAX_AGE_MS = 2000;
let infoCache = null;

/**
 * GET /info, shared with any other caller within the last couple of
 * seconds (including one still in flight).
 *
 * @returns {Promise<{name: string, fw: string, paired: boolean, boot_id: string}>}
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
const SPECIFIC_5XX = new Set([
  'unsupported',
  'timeout',
  'unavailable',
  'storage_unavailable',
  'no_memory',
  'write_failed',
]);

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
