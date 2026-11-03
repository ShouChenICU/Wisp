// Wi-Fi pieces shared by the network page and the setup wizard: signal
// bars, the scan list, the join form and the watcher that follows a join
// attempt to its result.
//
// SSIDs come from the air, so they are untrusted text: they are only ever
// rendered as text nodes (never as HTML), wrapped in <bdi> so their
// direction cannot reorder the text around them, with control and bidi
// override characters shown as U+FFFD.
//
// An SSID is up to 32 raw bytes and need not be UTF-8 (GBK routers are
// common). The device's JSON turns bytes that are not UTF-8 into U+FFFD,
// so such a name cannot be sent back as text; every SSID from the device
// also comes as `ssid_hex` (its bytes in lowercase hex, spec 6.1). Networks
// are told apart by that hex, and a name that shows U+FFFD goes back to the
// device as hex (ssidField(), networkPath()).

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError } from '../api.js';
import { t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { IconAlert, IconChevronRight, IconEye, IconEyeOff, IconLock } from '../icons.js';
import { Badge } from './badge.js';

/** Join attempts give up after this long (spec 7.3 wizard). */
export const JOIN_TIMEOUT_MS = 60000;
// The event stream is the fast path; this poll covers a socket that is
// down or full (the device allows two).
const JOIN_POLL_MS = 3000;
// How long a join waits for the event socket before sending anyway.
const JOIN_OPEN_WAIT_MS = 2000;
// This many failed polls in a row at the end of the window mean the page
// lost the device, most likely because it moved to the new network.
const JOIN_LOST_POLLS = 2;

/** The admin page's port and mDNS address on any network (spec 6.1). */
export const ADMIN_PORT = 8900;
export const MDNS_URL = `http://wisp.local:${ADMIN_PORT}`;

// The device's limits (WifiService::validate): SSID 1-32 bytes, password
// empty (open network) or 8-63 bytes, counted in UTF-8.
export const SSID_MAX_BYTES = 32;
export const PASSWORD_MIN_BYTES = 8;
export const PASSWORD_MAX_BYTES = 63;

const encoder = new TextEncoder();

/** UTF-8 length of `s`. */
export function byteLength(s) {
  return encoder.encode(s).length;
}

/** Lowercase hex of the UTF-8 bytes of `s`. */
export function hexOf(s) {
  let out = '';
  for (const b of encoder.encode(s)) out += b.toString(16).padStart(2, '0');
  return out;
}

/**
 * The byte-exact identity of a network ({ssid, ssid_hex?}): the device's
 * ssid_hex, or the hex of a name typed into the page.
 */
export function ssidKey(n) {
  if (!n) return '';
  return typeof n.ssid_hex === 'string' && n.ssid_hex ? n.ssid_hex : hexOf(String(n.ssid ?? ''));
}

// The device could not render this name as UTF-8: its text is lossy.
function lossy(n) {
  return String(n.ssid).includes('\uFFFD') && typeof n.ssid_hex === 'string' && n.ssid_hex !== '';
}

/** The SSID part of a POST /wifi/networks body for network `n`. */
export function ssidField(n) {
  return lossy(n) ? { ssid_hex: n.ssid_hex } : { ssid: n.ssid };
}

/** The path segment that names network `n` in DELETE /wifi/networks/{ssid}. */
export function networkPath(n) {
  return lossy(n) ? n.ssid_hex.replace(/../g, '%$&') : encodeURIComponent(n.ssid);
}

/**
 * `ssid` made safe to show: control characters and bidi embedding /
 * override / isolate controls become U+FFFD, so a name cannot hide
 * characters or flip into a misleading order.
 */
export function displaySsid(ssid) {
  // eslint-disable-next-line no-control-regex
  return String(ssid).replace(/[\u0000-\u001f\u007f\u202a-\u202e\u2066-\u2069]/g, '\uFFFD');
}

/** An SSID as an isolated text run. */
export function Ssid({ ssid, class: cls }) {
  return html`<bdi class=${cls ? 'ssid ' + cls : 'ssid'}>${displaySsid(ssid)}</bdi>`;
}

export function signalLevel(rssi) {
  if (!Number.isFinite(rssi) || rssi === 0) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

export function SignalBars({ rssi }) {
  const level = signalLevel(rssi);
  return html`<span class="bars" role="img" aria-label=${t('wifi.signal_bars', { level })}>
    ${[1, 2, 3, 4].map((i) => html`<i key=${i} class=${i <= level ? 'on' : ''}></i>`)}
  </span>`;
}

/** The localized cause of a failed join ("not_found", "auth_failed", ...). */
export function reasonText(reason) {
  return t('wifi.reason_' + (['not_found', 'auth_failed'].includes(reason) ? reason : 'other'));
}

/**
 * @typedef {Object} ScanEntry  one network from GET /wifi/scan
 * @property {string} ssid
 * @property {string} ssid_hex
 * @property {number} rssi
 * @property {boolean} secured
 * @property {boolean} saved
 */

/**
 * Cleans a scan result for display: entries without an SSID (hidden
 * networks, which can only be joined by name) are counted, not listed;
 * repeats of one SSID (several access points, compared byte for byte)
 * keep the strongest; the rest are sorted strongest first.
 *
 * @param {ScanEntry[]} networks
 * @returns {{list: ScanEntry[], hidden: number}}
 */
export function prepareScan(networks) {
  const best = new Map();
  let hidden = 0;
  for (const n of Array.isArray(networks) ? networks : []) {
    if (!n || typeof n.ssid !== 'string') continue;
    if (n.ssid === '') {
      hidden++;
      continue;
    }
    const rssi = Number(n.rssi);
    const entry = {
      ssid: n.ssid,
      ssid_hex: ssidKey(n),
      rssi: Number.isFinite(rssi) ? rssi : -100,
      secured: !!n.secured,
      saved: !!n.saved,
    };
    const seen = best.get(entry.ssid_hex);
    if (!seen) best.set(entry.ssid_hex, entry);
    else {
      seen.saved = seen.saved || entry.saved;
      seen.secured = seen.secured || entry.secured;
      if (entry.rssi > seen.rssi) seen.rssi = entry.rssi;
    }
  }
  const list = [...best.values()].sort((a, b) => b.rssi - a.rssi);
  return { list, hidden };
}

/**
 * The networks from a scan, each a button. `selected` (an ssidKey()) marks
 * the one being joined; `current` (an ssidKey()) the one the device is on.
 *
 * @param {{networks: ScanEntry[], hidden?: number, selected?: string,
 *          current?: string, onPick: (n: ScanEntry) => void,
 *          disabled?: boolean, children?: any}} props
 *   `children` renders right below the selected row (the join form).
 */
export function ScanList({ networks, hidden = 0, selected, current, onPick, disabled, children }) {
  return html`<ul class="scan-list" aria-label=${t('scan.list_label')}>
    ${networks.map((n) => {
      const isSel = selected === n.ssid_hex;
      const isCur = current === n.ssid_hex;
      return html`<li key=${'n:' + n.ssid_hex} class=${isSel ? 'scan-item is-selected' : 'scan-item'}>
        <button
          type="button"
          class="scan-row"
          onClick=${() => onPick(n)}
          disabled=${disabled}
          aria-expanded=${isSel ? 'true' : 'false'}
        >
          <${SignalBars} rssi=${n.rssi} />
          <span class="scan-main">
            <${Ssid} ssid=${n.ssid} class="scan-ssid" />
            <span class="scan-sub">
              ${n.secured ? t('scan.secured') : t('scan.open')}
            </span>
          </span>
          ${isCur
            ? html`<${Badge} tone="ok">${t('scan.current')}</${Badge}>`
            : n.saved
              ? html`<${Badge} tone="accent">${t('scan.saved')}</${Badge}>`
              : null}
          ${n.secured ? html`<span class="scan-lock" title=${t('scan.secured')}><${IconLock} size=${15} /></span>` : null}
          <span class="scan-chevron"><${IconChevronRight} size=${16} /></span>
        </button>
        ${isSel ? children : null}
      </li>`;
    })}
    ${hidden
      ? html`<li key="hidden" class="scan-hidden muted">${t('scan.hidden', { count: hidden })}</li>`
      : null}
  </ul>`;
}

/**
 * Asks for what a join needs and validates it before anything is sent.
 * With `network` it asks only for the password (none for an open
 * network); without one it also asks for the SSID (a hidden network).
 *
 * @param {{network?: ScanEntry, busy?: boolean, error?: string,
 *          submitText?: string, onCancel?: () => void, idPrefix?: string,
 *          onSubmit: (target: {ssid: string, ssid_hex?: string,
 *                              secured: boolean, manual?: boolean},
 *                     password: string) => void}} props
 *   `target` is `network` itself, or the typed hidden network.
 */
export function JoinForm({ network, busy, error, submitText, onSubmit, onCancel, idPrefix = 'join' }) {
  const manual = !network;
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');
  const [reveal, setReveal] = useState(false);
  const [problem, setProblem] = useState('');
  const first = useRef(null);
  const needsPassword = manual || network.secured;

  useEffect(() => {
    first.current?.focus();
  }, [network?.ssid_hex, manual]);

  function submit(e) {
    e.preventDefault();
    if (busy) return;
    const name = manual ? ssid : network.ssid;
    if (manual) {
      if (!name) return setProblem(t('join.err_ssid_empty'));
      if (byteLength(name) > SSID_MAX_BYTES) return setProblem(t('join.err_ssid_long', { max: SSID_MAX_BYTES }));
    }
    const pwBytes = byteLength(password);
    if (needsPassword && !(manual && pwBytes === 0)) {
      if (pwBytes < PASSWORD_MIN_BYTES || pwBytes > PASSWORD_MAX_BYTES) {
        return setProblem(t('join.err_password', { min: PASSWORD_MIN_BYTES, max: PASSWORD_MAX_BYTES }));
      }
    }
    setProblem('');
    const pw = needsPassword ? password : '';
    onSubmit(manual ? { ssid: name, ssid_hex: hexOf(name), secured: pw !== '', manual: true } : network, pw);
  }

  const shown = problem || error;
  const errId = idPrefix + '-err';
  return html`<form class="join-form" onSubmit=${submit} novalidate>
    ${manual
      ? html`<div class="field">
          <label class="field-label" for=${idPrefix + '-ssid'}>${t('join.ssid')}</label>
          <input
            id=${idPrefix + '-ssid'}
            ref=${first}
            class="input"
            type="text"
            autocomplete="off"
            autocapitalize="off"
            spellcheck=${false}
            value=${ssid}
            onInput=${(e) => {
              setSsid(e.currentTarget.value);
              setProblem('');
            }}
            disabled=${busy}
            aria-describedby=${shown ? errId : undefined}
          />
        </div>`
      : null}
    ${needsPassword
      ? html`<div class="field">
          <label class="field-label" for=${idPrefix + '-pw'}>
            ${manual ? t('join.password_optional') : t('join.password_for', { ssid: displaySsid(network.ssid) })}
          </label>
          <div class="input-group">
            <input
              id=${idPrefix + '-pw'}
              ref=${manual ? undefined : first}
              class="input"
              type=${reveal ? 'text' : 'password'}
              autocomplete="off"
              autocapitalize="off"
              spellcheck=${false}
              value=${password}
              onInput=${(e) => {
                setPassword(e.currentTarget.value);
                setProblem('');
              }}
              disabled=${busy}
              aria-describedby=${shown ? errId : undefined}
              aria-invalid=${shown ? 'true' : 'false'}
            />
            <button
              type="button"
              class="btn btn-ghost btn-icon input-addon"
              onClick=${() => setReveal(!reveal)}
              aria-label=${reveal ? t('join.hide_password') : t('join.show_password')}
              aria-pressed=${reveal ? 'true' : 'false'}
              title=${reveal ? t('join.hide_password') : t('join.show_password')}
            >
              ${reveal ? html`<${IconEyeOff} size=${18} />` : html`<${IconEye} size=${18} />`}
            </button>
          </div>
        </div>`
      : html`<p class="muted">${t('join.open_note')}</p>`}
    ${shown
      ? html`<div class="alert tone-err" role="alert" id=${errId}>
          <${IconAlert} size=${18} /><span>${shown}</span>
        </div>`
      : null}
    <div class="form-actions">
      ${onCancel
        ? html`<button type="button" class="btn btn-ghost" onClick=${onCancel} disabled=${busy}>${t('common.cancel')}</button>`
        : null}
      <button type="submit" class="btn btn-primary" disabled=${busy}>
        ${busy ? html`<span class="spinner" aria-hidden="true"></span>` : null}${submitText || t('join.connect')}
      </button>
    </div>
  </form>`;
}

/**
 * Sends a join (POST /wifi/networks) and follows it to an outcome.
 *
 * The device answers 202 and connects on its own task, announcing each
 * step as net.wifi.state. The request goes out once the event socket is
 * open (or after JOIN_OPEN_WAIT_MS without it), so this join's own events
 * are not published before anyone listens. Events for the same network
 * published before the request may still be in flight on the socket, so an
 * event only counts after this join's own "connecting" event has been
 * seen; GET /wifi (polled, and fetched whenever the socket reopens) is
 * always current and counts at once.
 *
 * Joining another network while the device is on one makes the device
 * leave the network this page reaches it through, so the outcome may never
 * arrive: a timeout whose last polls all failed is reported as `lost`.
 *
 * @param {{ssid: string, ssid_hex?: string}} target  a scan or saved entry,
 *   or a typed name
 * @param {string} password  "" for an open network
 * @param {(result: {ok: true, ip: string, ssid: string}
 *                 | {ok: false, reason: string}
 *                 | {ok: false, timeout: true, lost?: true}
 *                 | {ok: false, error: any}) => void} onResult
 *   called once; `error` is an ApiError from the request itself
 * @returns {() => void} stops following (no result is reported after)
 */
export function joinNetwork(target, password, onResult) {
  const key = ssidKey(target);
  // Declared before subscribe(): it may report "open" synchronously.
  let done = false;
  let sent = false;
  let posted = false;
  let armed = false;
  let failedPolls = 0;
  let pollTimer = 0;
  let openWait = 0;
  let timeout = 0;
  let unsubscribe = () => {};

  function stop() {
    clearTimeout(timeout);
    clearTimeout(openWait);
    clearInterval(pollTimer);
    unsubscribe();
  }

  function finish(result) {
    if (done) return;
    done = true;
    stop();
    onResult(result);
  }

  function judge(st, fromEvent) {
    if (!st || ssidKey(st) !== key) return;
    if (st.state === 'connecting') {
      if (fromEvent) armed = true;
      return;
    }
    if (fromEvent && !armed) return;
    if (st.state === 'connected') finish({ ok: true, ip: st.ip || '', ssid: target.ssid });
    else if (st.state === 'backoff') finish({ ok: false, reason: st.reason || 'other' });
  }

  async function poll() {
    if (done || !posted) return;
    let st;
    try {
      st = await api('GET', '/wifi');
    } catch {
      // A failed poll is not a failed join; the next one may get through.
      failedPolls++;
      return;
    }
    failedPolls = 0;
    judge(st, false);
  }

  function send() {
    if (sent || done) return;
    sent = true;
    clearTimeout(openWait);
    const body = { ...ssidField(target), ...(password ? { password } : {}) };
    api('POST', '/wifi/networks', body)
      .then(() => {
        if (done) return;
        posted = true;
        pollTimer = setInterval(poll, JOIN_POLL_MS);
        poll();
      })
      .catch((e) => finish({ ok: false, error: e }));
  }

  unsubscribe = subscribe(
    ['events'],
    (msg) => {
      if (msg.t === 'dropped') poll();
      else if (msg.topic === 'net.wifi.state') judge(msg.payload || {}, true);
    },
    {
      onState: (st) => {
        if (st !== 'open') return;
        send();
        poll();
      },
    },
  );
  if (!sent) openWait = setTimeout(send, JOIN_OPEN_WAIT_MS);
  timeout = setTimeout(
    () => finish(failedPolls >= JOIN_LOST_POLLS ? { ok: false, timeout: true, lost: true } : { ok: false, timeout: true }),
    JOIN_TIMEOUT_MS,
  );

  return () => {
    done = true;
    stop();
  };
}

/** A sentence for a failed join result from joinNetwork(). */
export function joinFailureText(result) {
  if (result.lost) return t('join.err_lost', { url: MDNS_URL });
  if (result.timeout) return t('join.err_timeout');
  if (result.error) {
    if (result.error.code === 'quota_exceeded') return t('join.err_quota');
    return describeError(result.error);
  }
  return t('join.failed', { reason: reasonText(result.reason) });
}
