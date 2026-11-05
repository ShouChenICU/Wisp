// The device's live stream (spec 6.2), shared by the whole tab.
//
// The device accepts at most two WebSocket connections in total, so a tab
// must never open more than one: every subscribe() call joins a single
// socket, and the set of kinds sent to the device is the union of what the
// current subscribers want. The device replaces its subscription set with
// each {"sub": [...]} message, so the full union is resent on every change.
//
// The socket exists only while it is needed: it opens when the first kind
// is subscribed and closes shortly after the last subscriber leaves (the
// short linger lets a page switch between two subscribing pages reuse it
// instead of briefly holding two of the device's slots). It is closed at
// once while the tab is hidden (reopened when it becomes visible again),
// so a forgotten background tab does not hold one of the two slots.
//
// The device opens every connection with {"t":"hello","boot_id"} (spec 6.2).
// It is kept for the life of the socket and handed to each "log"
// subscriber when it joins, so a subscriber that joins an open socket
// still learns the boot before its replay arrives.
//
// Backoff resets only once a connection proves healthy -- a first message
// from the device, or STABLE_MS open -- because a server can complete the
// handshake and close right away (ESP-IDF httpd does when it is full), and
// resetting on open would then retry every second forever.

import { api } from './api.js';

const PATH = '/api/v1/ws';
const BACKOFF_FIRST_MS = 1000;
const BACKOFF_MAX_MS = 30000;
const STABLE_MS = 5000;
const LINGER_MS = 1500;

/**
 * @typedef {'connecting'|'open'|'retrying'|'closed'|'unauthorized'} WsState
 *
 * @typedef {Object} Subscriber
 * @property {Set<string>} kinds        "log" and/or "events"
 * @property {(msg: any) => void} onMessage
 * @property {(state: WsState, info?: {retryMs?: number}) => void} [onState]
 */

/** @type {Set<Subscriber>} */
const subscribers = new Set();
/** @type {WebSocket|null} */
let socket = null;
let retryTimer = 0;
let lingerTimer = 0;
let stableTimer = 0;
let backoff = BACKOFF_FIRST_MS;
/** @type {WsState} */
let state = 'closed';
let sentKey = '';
/** The current socket's hello, once it arrived. */
let hello = null;

function wanted() {
  const kinds = new Set();
  for (const s of subscribers) for (const k of s.kinds) kinds.add(k);
  return [...kinds].sort();
}

function setState(next, info) {
  state = next;
  for (const s of [...subscribers]) s.onState?.(next, info);
}

function sendSubscription() {
  if (!socket || socket.readyState !== WebSocket.OPEN) return;
  const kinds = wanted();
  const key = kinds.join(',');
  if (key === sentKey) return;
  sentKey = key;
  socket.send(JSON.stringify({ sub: kinds }));
}

function clearRetry() {
  if (retryTimer) clearTimeout(retryTimer);
  retryTimer = 0;
}

function clearLinger() {
  if (lingerTimer) clearTimeout(lingerTimer);
  lingerTimer = 0;
}

function clearStable() {
  if (stableTimer) clearTimeout(stableTimer);
  stableTimer = 0;
}

function markHealthy() {
  clearStable();
  backoff = BACKOFF_FIRST_MS;
}

function closeSocket() {
  clearRetry();
  clearLinger();
  clearStable();
  if (!socket) return;
  const s = socket;
  socket = null;
  hello = null;
  s.onopen = s.onmessage = s.onclose = s.onerror = null;
  try {
    s.close(1000);
  } catch {
    // Already closing; nothing left to release.
  }
}

function open() {
  clearRetry();
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const s = new WebSocket(proto + '//' + location.host + PATH);
  socket = s;
  sentKey = '';
  hello = null;
  setState('connecting');

  s.onopen = () => {
    clearStable();
    stableTimer = setTimeout(markHealthy, STABLE_MS);
    sendSubscription();
    setState('open');
  };
  s.onmessage = (ev) => {
    if (stableTimer) markHealthy();
    let msg;
    try {
      msg = JSON.parse(ev.data);
    } catch {
      return;
    }
    if (!msg || typeof msg !== 'object') return;
    if (msg.t === 'ping') {
      // The device closes a client it has not heard from in a while (a
      // vanished peer would keep Wi-Fi awake); answering keeps this one.
      try {
        s.send(JSON.stringify({ t: 'pong' }));
      } catch {
        // Closing; the reconnect logic takes over.
      }
      return;
    }
    if (msg.t === 'hello') hello = msg;
    for (const sub of [...subscribers]) {
      if (deliverable(sub, msg)) sub.onMessage(msg);
    }
  };
  s.onclose = () => {
    if (socket !== s) return;
    socket = null;
    hello = null;
    clearStable();
    clearLinger();
    onUnexpectedClose();
  };
}

function deliverable(sub, msg) {
  if (msg.t === 'log' || msg.t === 'hello') return sub.kinds.has('log');
  if (msg.t === 'event') return sub.kinds.has('events');
  // The device dropped queued messages; an events subscriber may have
  // missed a state change and should refetch.
  if (msg.t === 'dropped') return sub.kinds.has('log') || sub.kinds.has('events');
  return false;
}

// The device closed the socket or refused the upgrade. A browser cannot
// see why (a refused handshake is just code 1006), and the reasons differ:
// the token was revoked, both slots are taken by other tabs, or the device
// went away. One cheap authenticated request tells the first case apart --
// reconnecting with a revoked token would only fail forever.
async function onUnexpectedClose() {
  if (!subscribers.size) {
    setState('closed');
    return;
  }
  const delay = backoff;
  backoff = Math.min(backoff * 2, BACKOFF_MAX_MS);
  setState('retrying', { retryMs: delay });
  try {
    await api('GET', '/settings');
  } catch (e) {
    if (e && e.status === 401) {
      // api() has already sent the page to pairing.
      setState('unauthorized');
      return;
    }
  }
  if (!subscribers.size || socket || document.hidden) return;
  clearRetry();
  retryTimer = setTimeout(() => {
    retryTimer = 0;
    if (subscribers.size && !socket && !document.hidden) open();
  }, delay);
}

function closeNow() {
  const hadSocket = socket !== null || retryTimer !== 0;
  closeSocket();
  if (hadSocket || state !== 'closed') setState('closed');
}

function sync() {
  if (document.hidden) {
    closeNow();
    return;
  }
  if (!subscribers.size) {
    if (!socket) {
      closeNow();
      return;
    }
    // Stop the device streaming now; close only if nobody resubscribes
    // within the linger.
    sendSubscription();
    if (!lingerTimer) {
      lingerTimer = setTimeout(() => {
        lingerTimer = 0;
        if (!subscribers.size) closeNow();
      }, LINGER_MS);
    }
    return;
  }
  clearLinger();
  if (!socket && !retryTimer) {
    open();
    return;
  }
  sendSubscription();
}

document.addEventListener('visibilitychange', () => {
  if (!document.hidden) backoff = BACKOFF_FIRST_MS;
  sync();
});

/**
 * Subscribes to the device's live stream.
 *
 * `onMessage` gets the decoded messages for the requested kinds:
 *   - "log":    {t:"hello", boot_id} first (on joining, and again from
 *               each new connection), then {t:"log", seq, ts, level, tag,
 *               msg[, replay]} and {t:"dropped", count}; every
 *               subscription (each Start, each reconnect) replays the
 *               device's recent log ring first, so callers dedupe by
 *               (boot_id, seq).
 *   - "events": {t:"event", topic, payload}, and {t:"dropped", count}
 *               when the device had to drop queued messages.
 *
 * `opts.onState` follows the shared socket: "connecting", "open" (also
 * after every reconnect, so callers can refetch what they missed),
 * "retrying" (with {retryMs}), "closed", or "unauthorized" when the
 * pairing was revoked. Reconnects back off 1 s, 2 s, 4 s... up to 30 s.
 *
 * @param {string[]} kinds
 * @param {(msg: any) => void} onMessage
 * @param {{onState?: Subscriber['onState']}} [opts]
 * @returns {() => void} unsubscribe; safe to call more than once
 */
export function subscribe(kinds, onMessage, opts = {}) {
  /** @type {Subscriber} */
  const sub = { kinds: new Set(kinds), onMessage, onState: opts.onState };
  subscribers.add(sub);
  // A socket already up (or between retries) does not announce its state
  // again, so the newcomer is told directly.
  const joined = socket !== null || retryTimer !== 0;
  sync();
  if (joined) sub.onState?.(state);
  if (hello && socket && sub.kinds.has('log')) sub.onMessage(hello);
  return () => {
    if (!subscribers.delete(sub)) return;
    sync();
  };
}
